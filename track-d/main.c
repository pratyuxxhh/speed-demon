// Speed Demon 2026, Track D (Traverse): exact shortest-path distances for each
// query in a directed graph with non-negative uint32 weights. See
// specs/d-traverse.md.
//
// graph.txt is mapped and read by all threads three times, 32 lines at a time
// with the node entries they touch prefetched: out-degrees, then the heads in a
// CSR, then (after renumbering) the edges packed into 4 bytes at their final
// place. Self-loops are dropped: they never shorten a path. The nodes are
// renumbered by BFS regions that all threads grow at once, so nodes near each
// other in the network are near in memory (the input ids are shuffled).
// Queries run in parallel, each a Dijkstra search from s that stops when t is
// settled, with Dial's bucket queue and 64-bit distances. A search keeps no
// distance per node: a node is pushed whenever it is reached and settled, one
// bit, at its first pop. A query whose target has no in-edges, or whose source
// has no out-edges, is answered -1 without searching.
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define INF32 UINT32_MAX
#define ESC 0xFFFF // the packed weight that marks an escaped edge
#define RING 4096  // Dial's buckets; a power of two above every main-body weight

typedef struct {
  uint32_t e, v, w; // an edge the packed form cannot hold: its index, head, weight
} Esc;
typedef struct {
  uint64_t d; // a node pending beyond the ring, at distance d
  uint32_t u;
} Far;
typedef struct {
  uint32_t *v, n, cap;
} Bucket;
typedef struct {
  const char *beg, *end; // this thread's lines of graph.txt
  uint64_t *done;        // the nodes a search has settled, one bit each
  uint32_t lo, hi;       // the words of done that a search wrote
  uint32_t *list, nl, base, nfar, farcap; // list: the nodes this thread's BFS claimed
  Far *far;
  Esc *esc;
  size_t nesc, esccap;
  Bucket ring[RING];
} Th;

static int nth, pass, next_q;
static Th *th;
static uint32_t n, nq, *off, *cur, *qs, *qt, *adj, *perm;
static uint8_t *has_in;
// After renumbering, edge e of node u is pk[e]: a 16-bit head delta (head - u)
// and a 16-bit weight, or ESC with the edge in esc[], sorted by e.
static uint32_t *pk;
static Esc *esc;
static size_t nesc;
static int64_t *ans;

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// Pass 3: edge u -> v of weight w, in the new numbering, at index k; its head
// gains an in-edge.
static void put(Th *t, uint32_t k, uint32_t u, uint32_t v, uint32_t w) {
  int64_t dv = (int64_t)v - u;
  __atomic_store_n(&has_in[v], 1, __ATOMIC_RELAXED);
  if (dv >= INT16_MIN && dv <= INT16_MAX && w < ESC) {
    pk[k] = (uint32_t)(uint16_t)dv << 16 | w;
    return;
  }
  if (t->nesc == t->esccap)
    t->esc = realloc(t->esc, (t->esccap = t->esccap ? 2 * t->esccap : 256) * sizeof(Esc));
  t->esc[t->nesc++] = (Esc){k, v, w};
  pk[k] = ESC;
}

// The value of the 1 to 8 digits at s: shifted to the top of a word, then
// pairs, quads and octets combined (reads 8 bytes from s).
static uint64_t num(const char *s, long k) {
  uint64_t x;
  memcpy(&x, s, 8);
  x = x << (64 - 8 * k) & 0x0F0F0F0F0F0F0F0Full;
  x = (x * 10 + (x >> 8)) & 0x00FF00FF00FF00FFull;
  x = (x * 100 + (x >> 16)) & 0x0000FFFF0000FFFFull;
  return (x * 10000 + (x >> 32)) & 0xFFFFFFFF;
}

// This thread's lines: pass 1 counts each tail's out-edges; pass 2 stores their
// heads; pass 3, once the nodes are renumbered, stores the edges packed. Lines
// are parsed 32 at a time and the node entries they will touch prefetched
// first, so the cache misses of the shuffled ids overlap; passes 2 and 3 then
// take all 32 cursors and prefetch the edge slots before writing them (and
// pass 3 maps the ids through the renumbering, prefetched too).
static void *scan(void *arg) {
  Th *t = arg;
  const char *p = t->beg;
  uint32_t a[4][32], m = 0;
  if (pass == 1) // the pages in one call, not a fault each
    madvise((void *)((uintptr_t)p & ~4095ul), t->end - p + 4096, 22); // POPULATE_READ
  while (p < t->end) {
    // The line's spaces and LF from one 32-byte compare, then each number eight
    // digits at once; a line too long for that is read digit by digit.
    __m256i x = _mm256_loadu_si256((const __m256i *)p);
    uint32_t sp = _mm256_movemask_epi8(_mm256_or_si256(
        _mm256_cmpeq_epi8(x, _mm256_set1_epi8(' ')), _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\n'))));
    uint32_t i1 = _tzcnt_u32(sp), i2 = _tzcnt_u32(_blsr_u32(sp)), i3 = _tzcnt_u32(_blsr_u32(_blsr_u32(sp)));
    uint32_t u = 0, v = 0;
    uint64_t w = 0;
    if (i3 < 32 && i1 <= 8 && i2 - i1 <= 9) {
      u = num(p, i1), v = num(p + i1 + 1, i2 - i1 - 1);
      w = i3 - i2 > 9 ? num(p + i2 + 1, i3 - i2 - 9) * 100000000 + num(p + i3 - 8, 8)
                      : num(p + i2 + 1, i3 - i2 - 1);
      p += i3 + 1;
    } else {
      for (; *p != ' '; p++)
        u = u * 10 + (*p - '0');
      for (p++; *p != ' '; p++)
        v = v * 10 + (*p - '0');
      for (p++; *p != '\n'; p++)
        w = w * 10 + (*p - '0');
      p++;
    }
    if (u != v && u < n && v < n) { // a self-loop never shortens a path
      a[0][m] = u, a[1][m] = v, a[2][m++] = w;
      __builtin_prefetch(pass == 1 ? &off[u + 1] : pass == 2 ? &cur[u] : &perm[u], 1);
      if (pass == 3)
        __builtin_prefetch(&perm[v]);
    }
    if (m < 32 && p < t->end)
      continue;
    for (uint32_t i = 0; pass == 3 && i < m; i++) {
      a[0][i] = perm[a[0][i]], a[1][i] = perm[a[1][i]];
      __builtin_prefetch(&cur[a[0][i]], 1), __builtin_prefetch(&has_in[a[1][i]], 1);
    }
    for (uint32_t i = 0; pass > 1 && i < m; i++) {
      a[3][i] = __atomic_fetch_add(&cur[a[0][i]], 1, __ATOMIC_RELAXED);
      __builtin_prefetch(pass == 2 ? &adj[a[3][i]] : &pk[a[3][i]], 1);
    }
    for (uint32_t i = 0; i < m; i++)
      if (pass == 1)
        __atomic_fetch_add(&off[a[0][i] + 1], 1, __ATOMIC_RELAXED);
      else if (pass == 2)
        adj[a[3][i]] = a[1][i];
      else
        put(t, a[3][i], a[0][i], a[1][i], a[2][i]);
    m = 0;
  }
  return NULL;
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

static int is_done(const Th *t, uint32_t u) { return t->done[u >> 6] >> (u & 63) & 1; }

// Settle u: false if it already was. lo and hi bound the words written, which
// the breadth-first numbering keeps to a narrow span, so clearing them is cheap.
static int settle(Th *t, uint32_t u) {
  if (is_done(t, u))
    return 0;
  t->done[u >> 6] |= 1ull << (u & 63);
  t->lo = u >> 6 < t->lo ? u >> 6 : t->lo, t->hi = u >> 6 > t->hi ? u >> 6 : t->hi;
  return 1;
}

static void bpush(Bucket *b, uint32_t v) {
  if (b->n == b->cap)
    b->v = realloc(b->v, (b->cap = b->cap ? b->cap * 2 : 8) * 4ull);
  b->v[b->n++] = v;
}

// Dijkstra from s until t is settled: the distance, or -1 when t is
// unreachable. Dial's ring of RING buckets holds every pending distance in
// [d, d + RING), one distance per bucket. An edge of RING or more (only the
// rare ones near 2^32; main-body weights are a few thousand at most) sends its
// head to the far list instead, which joins the ring once d comes within RING
// of it; d jumps to the nearest far node when the ring is empty.
static int64_t dial(Th *t, uint32_t s, uint32_t tg) {
  size_t inring = 1;
  int64_t res = -1;
  bpush(&t->ring[0], s);
  for (uint64_t d = 0; (inring || t->nfar) && res == -1; d++) {
    for (uint32_t i = 0; !inring && i < t->nfar; i++)
      d = i && d < t->far[i].d ? d : t->far[i].d;
    for (uint32_t i = 0; i < t->nfar;)
      if (t->far[i].d < d + RING)
        bpush(&t->ring[t->far[i].d & (RING - 1)], t->far[i].u), inring++,
            t->far[i] = t->far[--t->nfar];
      else
        i++;
    Bucket *b = &t->ring[d & (RING - 1)];
    for (uint32_t k = 0; k < b->n && res == -1; k++) { // weight-0 edges add to this bucket
      uint32_t u = b->v[k];
      if (k + 4 < b->n)
        __builtin_prefetch(&off[b->v[k + 4]]);
      if (k + 2 < b->n)
        __builtin_prefetch(&pk[off[b->v[k + 2]]]);
      inring--;
      if (!settle(t, u))
        continue; // already settled, at a shorter distance
      if (u == tg)
        res = d;
      for (uint32_t e = off[u], v, w; e < off[u + 1] && res == -1; e++) {
        edge_at(u, e, &v, &w);
        if (is_done(t, v))
          continue;
        if (w < RING)
          bpush(&t->ring[(d + w) & (RING - 1)], v), inring++;
        else if (t->nfar < t->farcap || (t->far = realloc(t->far, (t->farcap += 16) * sizeof(Far))))
          t->far[t->nfar++] = (Far){d + w, v};
      }
    }
    b->n = 0;
  }
  for (int i = 0; i < RING; i++)
    t->ring[i].n = 0;
  memset(t->done + t->lo, 0, (t->hi - t->lo + 1) * 8ull), t->lo = n, t->hi = 0, t->nfar = 0;
  return res;
}

static void *answer(void *arg) {
  Th *t = arg;
  t->done = calloc(n / 64 + 1, 8), t->lo = n, t->hi = 0; // a search settles s at least
  for (uint32_t q; (q = __atomic_fetch_add(&next_q, 1, __ATOMIC_RELAXED)) < nq;) {
    uint32_t s = qs[q], tg = qt[q];
    if (s == tg)
      ans[q] = 0;
    else if (!has_in[tg] || off[s] == off[s + 1])
      ans[q] = -1; // nothing enters t, or nothing leaves s
    else
      ans[q] = dial(t, s, tg);
  }
  return NULL;
}

// Claim v for the renumbering: false if another BFS already has it.
static int claim(uint32_t v) {
  uint32_t x = INF32;
  return perm[v] == INF32 &&
         __atomic_compare_exchange_n(&perm[v], &x, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// One thread's regions of the renumbering: a BFS from each node of its share of
// the old ids that no thread has claimed yet, listing what it claims in BFS
// order. The queue prefetches ahead: the offsets, edges and marks of the nodes
// it is about to expand are random reads in the input numbering.
static void *bfs(void *arg) {
  Th *t = arg;
  uint32_t *q = t->list = malloc(n * 4ull), tl = 0; // only the pages written count
  for (uint32_t r = (uint64_t)n * (t - th) / nth; r < (uint64_t)n * (t - th + 1) / nth; r++) {
    if (!claim(r))
      continue;
    q[tl++] = r;
    for (uint32_t h = tl - 1; h < tl; h++) {
      if (h + 16 < tl)
        __builtin_prefetch(&off[q[h + 16]]);
      if (h + 8 < tl)
        __builtin_prefetch(&adj[off[q[h + 8]]]);
      for (uint32_t e = h + 4 < tl ? off[q[h + 4]] : 0; h + 4 < tl && e < off[q[h + 4] + 1]; e++)
        __builtin_prefetch(&perm[adj[e]]);
      for (uint32_t e = off[q[h]]; e < off[q[h] + 1]; e++)
        if (claim(adj[e]))
          q[tl++] = adj[e];
    }
  }
  t->nl = tl;
  return NULL;
}

// Each thread's nodes take consecutive new ids from base; their degrees go to
// cur, which becomes the new offsets.
static void *number(void *arg) {
  Th *t = arg;
  for (uint32_t j = 0; j < t->nl; j++)
    perm[t->list[j]] = t->base + j, cur[t->base + j + 1] = off[t->list[j] + 1] - off[t->list[j]];
  free(t->list);
  return NULL;
}

static int by_index(const void *a, const void *b) {
  return (((const Esc *)a)->e > ((const Esc *)b)->e) - (((const Esc *)a)->e < ((const Esc *)b)->e);
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
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // 2 MB pages would be charged whole
  mallopt(M_MMAP_THRESHOLD, 1 << 17);    // big blocks go back when freed
  // queries.txt: "s t" per line.
  FILE *qf = fopen("/data/queries.txt", "r");
  if (!qf)
    die("cannot open /data/queries.txt");
  size_t qcap = 1024;
  qs = malloc(qcap * 4), qt = malloc(qcap * 4);
  for (uint32_t s, t; fscanf(qf, "%u %u", &s, &t) == 2; nq++) {
    if (nq == qcap)
      qcap *= 2, qs = realloc(qs, qcap * 4), qt = realloc(qt, qcap * 4);
    qs[nq] = s, qt[nq] = t;
  }
  fclose(qf);

  // graph.txt: "n m" then "u v w" lines, each thread a line-aligned share.
  struct stat st;
  int fd = open("/data/graph.txt", O_RDONLY);
  char *z = fd < 0 || fstat(fd, &st) ? MAP_FAILED // over zeros a page longer, so
          : mmap(NULL, st.st_size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // the
  const char *g = z == MAP_FAILED ? z // 32-byte loads past the last line stay mapped
                  : mmap(z, st.st_size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
  if (g == MAP_FAILED || !(n = strtoul(g, NULL, 10)))
    die("cannot read /data/graph.txt");
  const char *end = g + st.st_size, *body = (const char *)memchr(g, '\n', st.st_size) + 1;
  for (uint32_t i = 0; i < nq; i++)
    if (qs[i] >= n || qt[i] >= n)
      die("query node out of range");
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth < 1 ? 1 : nth > 64 ? 64 : nth;
  th = calloc(nth, sizeof(Th));
  for (int i = 0; i < nth; i++) {
    const char *b = body + (end - body) * i / nth;
    th[i].beg = i ? (const char *)memchr(b - 1, '\n', end - b + 1) + 1 : body;
    th[i].beg = i && th[i].beg < th[i - 1].beg ? th[i - 1].beg : th[i].beg;
    th[i - !!i].end = th[i].beg, th[i].end = end;
  }

  // The CSR of heads: degrees, offsets, then the heads with off[u] as u's
  // cursor; after it each off[u] has reached off[u + 1], so they move up one.
  off = calloc(n + 2, 4);
  pass = 1;
  run_threads(scan);
  for (uint32_t u = 0; u < n; u++)
    off[u + 1] += off[u];
  adj = malloc((off[n] + 1ull) * 4), cur = off, pass = 2;
  run_threads(scan);
  memmove(off + 1, off, n * 4ull), off[0] = 0;
  // Renumber in BFS regions, each a block of new ids; the new offsets come from
  // the old degrees. Then a third read of graph.txt writes the packed edges
  // straight into place, so only one copy of the edges ever exists; again the
  // offsets are the cursors.
  perm = malloc(n * 4ull), memset(perm, 0xff, n * 4ull);
  run_threads(bfs);
  free(adj);
  for (int i = 0, b = 0; i < nth; b += th[i].nl, i++)
    th[i].base = b;
  cur = calloc(n + 2, 4);
  run_threads(number);
  for (uint32_t i = 0; i < n; i++)
    cur[i + 1] += cur[i];
  free(off), off = cur;
  pk = malloc((off[n] + 1ull) * 4), has_in = calloc(n + 1, 1), pass = 3;
  run_threads(scan);
  memmove(off + 1, off, n * 4ull), off[0] = 0;
  for (int i = 0; i < nth; i++)
    nesc += th[i].nesc;
  esc = malloc((nesc + 1) * sizeof(Esc));
  for (int i = 0, j = 0; i < nth; j += th[i].nesc, i++)
    if (th[i].nesc)
      memcpy(esc + j, th[i].esc, th[i].nesc * sizeof(Esc));
  qsort(esc, nesc, sizeof(Esc), by_index); // the threads wrote them in no order
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
