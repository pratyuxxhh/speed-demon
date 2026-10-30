// Speed Demon 2026, Track D (Traverse): exact shortest-path distances for each
// query in a directed graph with non-negative uint32 weights. See
// specs/d-traverse.md.
//
// The graph is read by all threads in two passes into a CSR (out-degrees, then
// edges at atomically advanced cursors); self-loops are dropped, since they never
// shorten a path. The nodes are renumbered in breadth-first order for locality.
// Queries run in parallel, each a Dijkstra search from s that stops when t is
// settled, with Dial's bucket queue and 32-bit distances. A query whose target has
// no in-edges, or whose source has no out-edges, is answered -1 without
// searching. A search that meets a weight of RING or more, or would reach
// 2^32 - 1, is redone with a binary heap and 64-bit distances.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFSZ (1 << 16)
#define INF32 UINT32_MAX

typedef struct {
  uint32_t v, w;
} Edge;
typedef struct {
  uint32_t e, v, w; // an edge the packed form cannot hold: its index, head, weight
} Esc;
#define ESC 0xFFFF // packed weight field that marks an escaped edge
typedef struct {
  uint64_t d;
  uint32_t u;
} Item;
#define RING 4096 // Dial's buckets; a power of two above every main-body weight
typedef struct {
  uint32_t *v, n, cap;
} Bucket;
typedef struct {
  long beg, end;
  char *buf;
  uint32_t *d32, *touched; // dial()'s distances, reset through the touched list
  uint64_t *d64;           // search()'s, complemented (0: unreached), mapped on demand
  Item *heap;
  Esc *esc;
  size_t hcap, tcap, nesc, esccap;
  Bucket ring[RING];
} Th;

static int fd, nth, pass, next_q;
static Th *th;
static uint32_t n, nq, *off, *cur, *qs, *qt;
static uint8_t *has_in, *has_in2;
static Edge *edges;
// After renumbering, edge e of node u is pk[e]: a 16-bit head delta (head - u) and
// a 16-bit weight, or ESC with the edge in esc[], sorted by e.
static uint32_t *pk;
static Esc *esc;
static size_t nesc;
static int64_t *ans;
static uint32_t *order, *perm, *off2; // renumbering: new -> old, old -> new, new offsets

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// Pass 1 counts each tail's out-edges; pass 2 stores the edges.
static void edge_lines(const char *p, const char *lim) {
  while (p < lim) {
    uint32_t u = 0, v = 0;
    uint64_t w = 0;
    for (; *p != ' '; p++)
      u = u * 10 + (*p - '0');
    for (p++; *p != ' '; p++)
      v = v * 10 + (*p - '0');
    for (p++; *p != '\n'; p++)
      w = w * 10 + (*p - '0');
    p++;
    if (u == v || u >= n || v >= n)
      continue; // a self-loop never shortens a path
    if (pass == 1) {
      __atomic_fetch_add(&off[u + 1], 1, __ATOMIC_RELAXED);
      __atomic_store_n(&has_in[v], 1, __ATOMIC_RELAXED);
    } else {
      edges[__atomic_fetch_add(&cur[u], 1, __ATOMIC_RELAXED)] = (Edge){v, (uint32_t)w};
    }
  }
}

static void *scan(void *arg) {
  Th *t = arg;
  size_t have = 0;
  for (long o = t->beg; o < t->end;) {
    long want = t->end - o < (long)(BUFSZ - have) ? t->end - o : (long)(BUFSZ - have);
    ssize_t k = pread(fd, t->buf + have, want, o);
    if (k <= 0)
      die("read failed");
    o += k;
    have += k;
    char *last = memrchr(t->buf, '\n', have);
    if (!last)
      continue;
    edge_lines(t->buf, last + 1);
    have -= last + 1 - t->buf;
    memmove(t->buf, last + 1, have);
  }
  return NULL;
}

static void push(Th *t, size_t *hn, uint64_t d, uint32_t u) {
  if (*hn == t->hcap) {
    t->hcap *= 2;
    t->heap = realloc(t->heap, t->hcap * sizeof(Item));
  }
  size_t i = (*hn)++;
  for (; i && t->heap[(i - 1) / 2].d > d; i = (i - 1) / 2)
    t->heap[i] = t->heap[(i - 1) / 2];
  t->heap[i] = (Item){d, u};
}

static Item pop(Th *t, size_t *hn) {
  Item top = t->heap[0], last = t->heap[--*hn];
  size_t i = 0;
  for (size_t c; (c = 2 * i + 1) < *hn; i = c) {
    if (c + 1 < *hn && t->heap[c + 1].d < t->heap[c].d)
      c++;
    if (t->heap[c].d >= last.d)
      break;
    t->heap[i] = t->heap[c];
  }
  if (*hn)
    t->heap[i] = last;
  return top;
}

// Edge e of node u, from its packed form or the escape table.
static void edge_at(uint32_t u, uint32_t e, uint32_t *v, uint32_t *w) {
  uint32_t x = pk[e];
  if ((x & 0xFFFF) != ESC) {
    *v = u + (int16_t)(x >> 16), *w = x & 0xFFFF;
    return;
  }
  size_t lo = 0, hi = nesc;
  while (esc[lo].e != e) { // binary search; the edge is there
    size_t mid = (lo + hi) / 2;
    *(esc[mid].e <= e ? &lo : &hi) = mid;
  }
  *v = esc[lo].v, *w = esc[lo].w;
}

// search()'s array holds the complement of each distance, so a page the kernel
// hands out zeroed reads as "unreached", and its pages are returned after each
// of the few searches that need it.
static void *dist_array(size_t bytes) {
  void *a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (a == MAP_FAILED)
    die("out of memory");
  return a;
}

// Dijkstra with a binary heap and 64-bit distances, from s until t is settled:
// the distance, or -1 when t is unreachable. Used when dial() cannot be.
static int64_t search(Th *t, uint32_t s, uint32_t tg) {
  size_t hn = 0;
  int64_t res = -1;
  if (!t->d64)
    t->d64 = dist_array(n * 8ull);
  t->d64[s] = ~0ull;
  push(t, &hn, 0, s);
  while (hn) {
    Item it = pop(t, &hn);
    if (it.d > ~t->d64[it.u])
      continue; // stale: u was settled at a shorter distance
    if (it.u == tg) {
      res = it.d;
      break;
    }
    for (uint32_t e = off[it.u]; e < off[it.u + 1]; e++) {
      uint32_t v, w;
      edge_at(it.u, e, &v, &w);
      uint64_t nd = it.d + w;
      if (nd < ~t->d64[v]) {
        t->d64[v] = ~nd;
        push(t, &hn, nd, v);
      }
    }
  }
  madvise(t->d64, n * 8ull, MADV_DONTNEED);
  return res;
}

static void bpush(Bucket *b, uint32_t v) {
  if (b->n == b->cap)
    b->v = realloc(b->v, (b->cap = b->cap ? b->cap * 2 : 64) * 4ull);
  b->v[b->n++] = v;
}

// Dijkstra with Dial's bucket queue: a ring of RING buckets holds every pending
// distance in [d, d + RING), one distance per bucket, as long as no edge weighs
// RING or more (main-body weights are at most a few thousand). Returns as
// search() does, or -2 when such a weight or a 32-bit overflow is met.
static int64_t dial(Th *t, uint32_t s, uint32_t tg) {
  size_t live = 1, nt = 0;
  int64_t res = -1;
  t->d32[s] = 0;
  t->touched[nt++] = s;
  bpush(&t->ring[0], s);
  for (uint32_t d = 0; live && res == -1; d++) {
    Bucket *b = &t->ring[d & (RING - 1)];
    for (uint32_t k = 0; k < b->n && res == -1; k++) { // weight-0 edges add to this bucket
      uint32_t u = b->v[k];
      live--;
      if (t->d32[u] != d)
        continue; // stale: u was settled at a shorter distance
      if (u == tg)
        res = d;
      for (uint32_t e = off[u]; e < off[u + 1] && res == -1; e++) {
        uint32_t v, w;
        edge_at(u, e, &v, &w);
        uint64_t nd = (uint64_t)d + w;
        if (w >= RING || nd >= INF32)
          res = -2;
        else if (nd < t->d32[v]) {
          if (t->d32[v] == INF32) {
            if (nt == t->tcap)
              t->touched = realloc(t->touched, (t->tcap *= 2) * 4);
            t->touched[nt++] = v;
          }
          t->d32[v] = nd;
          bpush(&t->ring[nd & (RING - 1)], v);
          live++;
        }
      }
    }
    b->n = 0;
  }
  for (int i = 0; i < RING; i++)
    t->ring[i].n = 0;
  for (size_t i = 0; i < nt; i++) // reset only what this search touched
    t->d32[t->touched[i]] = INF32;
  return res;
}

static void *answer(void *arg) {
  Th *t = arg;
  t->d32 = malloc(n * 4ull);
  memset(t->d32, 0xff, n * 4ull);
  t->tcap = 1 << 16;
  t->touched = malloc(t->tcap * 4);
  t->hcap = 1 << 16;
  t->heap = malloc(t->hcap * sizeof(Item));
  for (uint32_t q; (q = __atomic_fetch_add(&next_q, 1, __ATOMIC_RELAXED)) < nq;) {
    uint32_t s = qs[q], tg = qt[q];
    if (s == tg)
      ans[q] = 0;
    else if (!has_in[tg] || off[s] == off[s + 1])
      ans[q] = -1; // nothing enters t, or nothing leaves s
    else if ((ans[q] = dial(t, s, tg)) == -2)
      ans[q] = search(t, s, tg);
  }
  return NULL;
}

// Thread k's share of the new ids: [n * k / nth, n * (k + 1) / nth).
static void share(Th *t, uint32_t *lo, uint32_t *hi) {
  *lo = (uint64_t)n * (t - th) / nth, *hi = (uint64_t)n * (t - th + 1) / nth;
}

static void *degrees(void *arg) {
  uint32_t lo, hi;
  share(arg, &lo, &hi);
  for (uint32_t i = lo; i < hi; i++)
    off2[i + 1] = off[order[i] + 1] - off[order[i]];
  return NULL;
}

// The packed edges of this thread's share of the new ids. Its escapes come out
// sorted by edge index, and the shares are in order.
static void *remap(void *arg) {
  Th *t = arg;
  uint32_t lo, hi;
  share(t, &lo, &hi);
  for (uint32_t i = lo; i < hi; i++) {
    uint32_t u = order[i], k = off2[i];
    for (uint32_t e = off[u]; e < off[u + 1]; e++, k++) {
      uint32_t v = perm[edges[e].v], w = edges[e].w;
      int64_t dv = (int64_t)v - i;
      if (dv >= INT16_MIN && dv <= INT16_MAX && w < ESC) {
        pk[k] = (uint32_t)(uint16_t)dv << 16 | w;
        continue;
      }
      if (t->nesc == t->esccap)
        t->esc = realloc(t->esc, (t->esccap = t->esccap ? 2 * t->esccap : 256) * sizeof(Esc));
      t->esc[t->nesc++] = (Esc){k, v, w};
      pk[k] = ESC;
    }
    has_in2[i] = has_in[u];
  }
  return NULL;
}

static void run_threads(void *(*f)(void *)) {
  pthread_t id[64];
  for (int i = 0; i < nth; i++)
    if (pthread_create(&id[i], NULL, f, &th[i]))
      die("pthread_create failed");
  for (int i = 0; i < nth; i++)
    pthread_join(id[i], NULL);
}

int main(void) {
  // queries.txt: "s t" per line.
  FILE *qf = fopen("/data/queries.txt", "r");
  if (!qf)
    die("cannot open /data/queries.txt");
  size_t qcap = 1024;
  qs = malloc(qcap * 4), qt = malloc(qcap * 4);
  for (uint32_t s, t; fscanf(qf, "%u %u", &s, &t) == 2; nq++) {
    if (nq == qcap) {
      qcap *= 2;
      qs = realloc(qs, qcap * 4), qt = realloc(qt, qcap * 4);
    }
    qs[nq] = s, qt[nq] = t;
  }
  fclose(qf);

  struct stat st;
  fd = open("/data/graph.txt", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    die("cannot open /data/graph.txt");
  long size = st.st_size, body;
  {
    char c[64];
    ssize_t k = pread(fd, c, sizeof c - 1, 0);
    c[k > 0 ? k : 0] = 0;
    n = strtoul(c, NULL, 10);
    char *nl = strchr(c, '\n');
    if (!nl)
      die("bad graph.txt header");
    body = nl - c + 1;
  }
  for (uint32_t i = 0; i < nq; i++)
    if (qs[i] >= n || qt[i] >= n)
      die("query node out of range");
  off = calloc(n + 2, 4);
  has_in = calloc(n + 1, 1);

  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth < 1 ? 1 : nth > 64 ? 64 : nth;
  th = calloc(nth, sizeof(Th));
  th[0].beg = body;
  for (int i = 1; i < nth; i++) {
    long b = body + (size - body) * i / nth;
    char c[64];
    ssize_t k = pread(fd, c, sizeof c, b - 1);
    char *nl = k > 0 ? memchr(c, '\n', k) : NULL; // lines are at most 33 bytes
    b = nl ? b + (nl - c) : size;
    th[i].beg = b < th[i - 1].beg ? th[i - 1].beg : b;
    th[i - 1].end = th[i].beg;
  }
  th[nth - 1].end = size;
  for (int i = 0; i < nth; i++)
    th[i].buf = malloc(BUFSZ);
  pass = 1;
  run_threads(scan);
  for (uint32_t u = 0; u < n; u++)
    off[u + 1] += off[u];
  edges = malloc((off[n] + 1ull) * sizeof(Edge));
  cur = malloc((n + 1) * 4ull);
  memcpy(cur, off, (n + 1) * 4ull);
  pass = 2;
  run_threads(scan);
  free(cur);
  for (int i = 0; i < nth; i++)
    free(th[i].buf);

  // Renumber the nodes in breadth-first order, so that nodes near each other in
  // the network are near each other in memory (the input ids are shuffled).
  // The BFS prefetches ahead in its queue: the offsets, edges and marks of the
  // nodes it is about to expand are random reads in the input numbering.
  order = malloc(n * 4ull), perm = malloc(n * 4ull);
  uint32_t tail = 0;
  memset(perm, 0xff, n * 4ull);
  for (uint32_t r = 0; r < n; r++) {
    if (perm[r] != INF32)
      continue;
    perm[r] = tail, order[tail++] = r;
    for (uint32_t h = tail - 1; h < tail; h++) {
      if (h + 16 < tail)
        __builtin_prefetch(&off[order[h + 16]]);
      if (h + 8 < tail)
        __builtin_prefetch(&edges[off[order[h + 8]]]);
      for (uint32_t e = h + 4 < tail ? off[order[h + 4]] : 0; h + 4 < tail && e < off[order[h + 4] + 1]; e++)
        __builtin_prefetch(&perm[edges[e].v]);
      for (uint32_t e = off[order[h]]; e < off[order[h] + 1]; e++)
        if (perm[edges[e].v] == INF32)
          perm[edges[e].v] = tail, order[tail++] = edges[e].v;
    }
  }
  // The graph in the new numbering, built in parallel: degrees, offsets, edges.
  off2 = malloc((n + 2) * 4ull), pk = malloc((off[n] + 1ull) * 4);
  has_in2 = malloc(n + 1ull);
  off2[0] = 0;
  run_threads(degrees);
  for (uint32_t i = 0; i < n; i++)
    off2[i + 1] += off2[i];
  run_threads(remap);
  free(edges), free(off), free(has_in), free(order);
  off = off2, has_in = has_in2;
  for (int i = 0; i < nth; i++)
    nesc += th[i].nesc;
  esc = malloc((nesc + 1) * sizeof(Esc));
  for (int i = 0, k = 0; i < nth; k += th[i].nesc, i++)
    if (th[i].nesc)
      memcpy(esc + k, th[i].esc, th[i].nesc * sizeof(Esc));
  for (uint32_t i = 0; i < nq; i++)
    qs[i] = perm[qs[i]], qt[i] = perm[qt[i]];
  free(perm);

  ans = malloc((nq + 1) * sizeof *ans);
  run_threads(answer);
  FILE *out = fopen("/out/distances.txt", "w");
  if (!out)
    die("cannot create /out/distances.txt");
  for (uint32_t i = 0; i < nq; i++)
    fprintf(out, "%lld\n", (long long)ans[i]);
  if (fclose(out))
    die("write failed");
  return 0;
}
