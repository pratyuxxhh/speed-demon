// Speed Demon 2026, Track A (Ingest): per (UTC hour, endpoint) request count,
// nearest-rank p50/p95/p99 latency and 5xx count/ppm. See specs/a-ingest.md.
//
// Pass 1: threads scan disjoint line ranges, intern endpoints in a local table
// and count lines per (hour, endpoint). The endpoints are merged and ranked
// bytewise, so the group id hour * E + rank is already in output order.
// Pass 2: the same scan scatters each latency into its group's slice of one
// array and counts 5xx. Then, per hour and in parallel, a selection finds the
// three ranks, and the lines are written in order. (Writing from several threads
// at precomputed offsets was measured slower: the kernel serializes writes to one
// file.)
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOURS 72        // 2026-10-30T18:00Z to 2026-11-02T17:59Z, fixed by the spec
#define CAP 4096        // max distinct endpoints; the spec promises 1,842
#define TBL (CAP * 2)   // open-addressing slots
#define BUFSZ (1 << 16) // read and write buffer per thread

typedef struct {
  const char *s;
  uint32_t len, hash;
} Ep;
typedef struct {
  int16_t slot[TBL];
  Ep ep[CAP];
  uint32_t n;
} Tab;
typedef struct {
  long beg, end;
  Tab tab;
  char *arena, *buf;
  size_t used;
  uint32_t *cnt, *rank; // cnt: [hour * CAP + local id]
  uint8_t seen[HOURS];
} Th;

static int fd, out, nth, pass, next_hour;
static Th *th;
static Ep *gep;
static uint32_t E, *ord, *lats, *gend, *gcnt, *gerr;

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

static uint32_t hash_bytes(const char *s, uint32_t n) { // reads up to 7 bytes past s + n
  uint64_t h = n * 0x9E3779B97F4A7C15ull, w;
  for (uint32_t i = 0; i < n; i += 8) {
    memcpy(&w, s + i, 8);
    if (n - i < 8)
      w &= (1ull << (8 * (n - i))) - 1;
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
  }
  return (uint32_t)(h >> 32);
}

// Returns the id of s in t, adding it (copied into arena if given) when new.
static uint32_t find(Tab *t, const char *s, uint32_t len, uint32_t h, char *arena,
                     size_t *used) {
  for (uint32_t i = h & (TBL - 1);; i = (i + 1) & (TBL - 1)) {
    int id = t->slot[i];
    if (id < 0) {
      if (t->n == CAP)
        die("too many distinct endpoints");
      if (arena) {
        memcpy(arena + *used, s, len);
        s = arena + *used;
        *used += len;
      }
      t->ep[t->n] = (Ep){s, len, h};
      t->slot[i] = t->n;
      return t->n++;
    }
    Ep *e = &t->ep[id];
    if (e->hash == h && e->len == len && memcmp(e->s, s, len) == 0)
      return id;
  }
}

// The first space at or after q, 32 bytes at a time (buffers have 64 bytes of slack).
static const char *next_space(const char *q) {
  for (;; q += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)q);
    uint32_t m = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(' ')));
    if (m)
      return q + __builtin_ctz(m);
  }
}

static void parse(Th *t, const char *p, const char *lim) {
  while (p < lim) {
    int day = (p[8] - '0') * 10 + p[9] - '0';
    day = p[6] == '0' ? day - 30 : day + 1; // Oct 30 -> 0 ... Nov 2 -> 3
    int hr = day * 24 + (p[11] - '0') * 10 + p[12] - '0' - 18;
    if ((unsigned)hr >= HOURS)
      die("timestamp outside the 72-hour span");
    // The method starts at byte 28; its first two letters give its length.
    char m0 = p[28], m1 = p[29];
    const char *s = p + 29 +
                    (m0 == 'G'   ? 3
                     : m0 == 'H' ? 4
                     : m0 == 'D' ? 6
                     : m0 == 'O' ? 7
                     : m1 == 'U' ? 3
                     : m1 == 'O' ? 4
                                 : 5); // PUT, POST, PATCH
    const char *e = next_space(s);
    int is5xx = e[1] == '5';
    uint32_t lat = 0;
    for (p = e + 5; *p != '\n'; p++)
      lat = lat * 10 + (*p - '0');
    p++;
    uint32_t id = find(&t->tab, s, e - s, hash_bytes(s, e - s), t->arena, &t->used);
    size_t k = (size_t)hr * CAP + id;
    if (pass == 1) {
      t->cnt[k]++;
      t->seen[hr] = 1;
    } else {
      lats[t->cnt[k]++] = lat; // cnt now holds this thread's write cursor
      if (is5xx)
        __atomic_fetch_add(&gerr[(size_t)hr * E + t->rank[id]], 1, __ATOMIC_RELAXED);
    }
  }
}

static void *scan(void *arg) {
  Th *t = arg;
  size_t have = 0;
  for (long off = t->beg; off < t->end;) {
    long want = t->end - off;
    if (want > (long)(BUFSZ - have))
      want = BUFSZ - have;
    ssize_t n = pread(fd, t->buf + have, want, off);
    if (n <= 0)
      die("read failed");
    off += n;
    have += n;
    char *last = memrchr(t->buf, '\n', have);
    if (!last)
      continue; // only possible mid-line at a buffer edge; lines are < 200 bytes
    parse(t, t->buf, last + 1);
    have -= last + 1 - t->buf;
    memmove(t->buf, last + 1, have);
  }
  return NULL;
}

// The k-th smallest of a[0..n), leaving a[0..k) <= a[k] <= a[k+1..n).
static uint32_t select_k(uint32_t *a, long n, long k) {
  long lo = 0, hi = n - 1;
  while (lo < hi) {
    uint32_t x = a[lo], y = a[(lo + hi) / 2], z = a[hi];
    uint32_t piv = x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
    long i = lo, j = hi;
    while (i <= j) {
      while (a[i] < piv)
        i++;
      while (a[j] > piv)
        j--;
      if (i <= j) {
        uint32_t tmp = a[i];
        a[i++] = a[j];
        a[j--] = tmp;
      }
    }
    if (k <= j)
      hi = j;
    else if (k >= i)
      lo = i;
    else
      break;
  }
  return a[k];
}

static char *num(char *o, uint64_t v) {
  int d = 1;
  for (uint64_t x = v; x >= 10; x /= 10)
    d++;
  *o = ' ';
  for (char *q = o + d; q > o; v /= 10)
    *q-- = '0' + v % 10;
  return o + d + 1;
}

// Per hour: leave p50, p95, p99 in the first min(n, 3) slots of each group's slice.
static void *percentiles(void *arg) {
  (void)arg;
  for (int hr; (hr = __atomic_fetch_add(&next_hour, 1, __ATOMIC_RELAXED)) < HOURS;)
    for (size_t g = (size_t)hr * E; g < (size_t)(hr + 1) * E; g++) {
      long n = gcnt[g];
      if (!n)
        continue;
      long k99 = (99 * n + 99) / 100 - 1, k95 = (95 * n + 99) / 100 - 1;
      long k50 = (50 * n + 99) / 100 - 1;
      uint32_t *a = lats + gend[g] - n;
      uint32_t v99 = select_k(a, n, k99), v95 = select_k(a, k99 + 1, k95);
      a[0] = select_k(a, k95 + 1, k50);
      a[1 < n ? 1 : 0] = v95; // n == 1: all three are the one value
      a[2 < n ? 2 : n - 1] = v99;
    }
  return NULL;
}

static void write_lines(char *buf) {
  char *o = buf;
  for (int hr = 0; hr < HOURS; hr++) {
    int day = (hr + 18) / 24;
    char hs[32];
    snprintf(hs, sizeof hs, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
             day < 2 ? 30 + day : day - 1, (hr + 18) % 24);
    for (uint32_t r = 0; r < E; r++) {
      size_t gi = (size_t)hr * E + r;
      uint32_t n = gcnt[gi], *a = lats + gend[gi] - n;
      if (!n)
        continue;
      Ep *e = &gep[ord[r]];
      memcpy(o, hs, 21);
      memcpy(o + 21, e->s, e->len);
      o = num(o + 21 + e->len, n);
      o = num(o, a[0]);
      o = num(o, a[n > 1]);
      o = num(o, a[n > 2 ? 2 : n - 1]);
      o = num(o, gerr[gi]);
      o = num(o, (uint64_t)gerr[gi] * 1000000 / n);
      *o++ = '\n';
      if (o - buf > BUFSZ - 512) {
        if (write(out, buf, o - buf) != o - buf)
          die("write failed");
        o = buf;
      }
    }
  }
  if (write(out, buf, o - buf) != o - buf)
    die("write failed");
}

static void run_threads(void *(*f)(void *)) {
  pthread_t id[64];
  for (int i = 0; i < nth; i++)
    if (pthread_create(&id[i], NULL, f, &th[i]))
      die("pthread_create failed");
  for (int i = 0; i < nth; i++)
    pthread_join(id[i], NULL);
}

static int cmp_ep(const void *x, const void *y) {
  const Ep *a = &gep[*(const uint32_t *)x], *b = &gep[*(const uint32_t *)y];
  int c = memcmp(a->s, b->s, a->len < b->len ? a->len : b->len);
  return c ? c : (a->len > b->len) - (a->len < b->len);
}

int main(void) {
  struct stat st;
  fd = open("/data/requests.log", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    die("cannot open /data/requests.log");
  long size = st.st_size;
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth < 1 ? 1 : nth > 64 ? 64 : nth;
  th = calloc(nth, sizeof(Th));

  // Split the file into nth ranges, each starting at a line start.
  for (int i = 1; i < nth; i++) {
    long b = size * i / nth;
    if (b > 0) {
      char c[256];
      ssize_t n = pread(fd, c, sizeof c, b - 1);
      char *nl = n > 0 ? memchr(c, '\n', n) : NULL;
      b = nl ? b + (nl - c) : size;
    }
    th[i].beg = b < th[i - 1].beg ? th[i - 1].beg : b;
    th[i - 1].end = th[i].beg;
  }
  th[nth - 1].end = size;
  for (int i = 0; i < nth; i++) {
    Th *t = &th[i];
    memset(t->tab.slot, 0xff, sizeof t->tab.slot);
    t->cnt = calloc((size_t)HOURS * CAP, 4);
    t->arena = malloc((size_t)CAP * 125);
    t->buf = malloc(BUFSZ + 64); // slack for next_space's 32-byte reads
  }
  pass = 1;
  run_threads(scan);

  // Merge the endpoint tables and rank the endpoints bytewise.
  Tab *g = calloc(1, sizeof(Tab));
  memset(g->slot, 0xff, sizeof g->slot);
  for (int i = 0; i < nth; i++) {
    Tab *t = &th[i].tab;
    th[i].rank = malloc((t->n + 1) * 4);
    for (uint32_t l = 0; l < t->n; l++)
      th[i].rank[l] = find(g, t->ep[l].s, t->ep[l].len, t->ep[l].hash, NULL, NULL);
  }
  E = g->n;
  gep = g->ep;
  ord = malloc((E + 1) * 4);
  uint32_t *rk = malloc((E + 1) * 4);
  for (uint32_t i = 0; i < E; i++)
    ord[i] = i;
  qsort(ord, E, 4, cmp_ep);
  for (uint32_t i = 0; i < E; i++)
    rk[ord[i]] = i;

  size_t groups = (size_t)HOURS * E;
  gcnt = calloc(groups + 1, 4);
  gerr = calloc(groups + 1, 4);
  gend = calloc(groups + 1, 4);
  for (int i = 0; i < nth; i++) {
    Th *t = &th[i];
    for (uint32_t l = 0; l < t->tab.n; l++)
      t->rank[l] = rk[t->rank[l]];
    for (int hr = 0; hr < HOURS; hr++)
      for (uint32_t l = 0; t->seen[hr] && l < t->tab.n; l++)
        gcnt[(size_t)hr * E + t->rank[l]] += t->cnt[(size_t)hr * CAP + l];
  }
  // Each thread's counts become its write cursors inside the groups' slices.
  uint32_t total = 0;
  for (size_t gi = 0; gi < groups; gi++) {
    gend[gi] = total;
    total += gcnt[gi];
  }
  for (int i = 0; i < nth; i++) {
    Th *t = &th[i];
    for (int hr = 0; hr < HOURS; hr++)
      for (uint32_t l = 0; t->seen[hr] && l < t->tab.n; l++) {
        size_t k = (size_t)hr * CAP + l, gi = (size_t)hr * E + t->rank[l];
        uint32_t c = t->cnt[k];
        t->cnt[k] = gend[gi];
        gend[gi] += c;
      }
  }
  lats = malloc((size_t)total * 4 + 4);
  pass = 2;
  run_threads(scan);
  run_threads(percentiles);

  out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    die("cannot create /out/summary.txt");
  write_lines(th[0].buf);
  if (close(out))
    die("write failed");
  return 0;
}
