// Speed Demon 2026, Track A (Ingest): per (UTC hour, endpoint) request count,
// nearest-rank p50/p95/p99 latency and 5xx count/ppm. See specs/a-ingest.md.
//
// One pass over the text: requests.log is mapped read-only and cut into 1 MB
// chunks that threads take from a counter. Each line's endpoint is interned in
// one shared lock-free table, and a record (a u16 tag of endpoint id, latency
// length and 5xx bit, then the latency in 1 to 4 bytes) goes to the thread's
// bucket for that hour. Then threads take whole hours: gather the hour's
// records from every thread, group them by
// endpoint, select p50/p95/p99 and format the hour's lines. Whoever holds the
// writer lock writes every finished hour in order, so the file sees one ordered
// stream and no thread waits for another's hour.
#define _GNU_SOURCE
#include <fcntl.h>
#include <immintrin.h>
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

#define HOURS 72      // 2026-10-30T18:00Z to 2026-11-02T17:59Z, fixed by the spec
#define CAP 4096      // max distinct endpoints (12-bit ids); the spec promises 1,842
#define TBL (CAP * 2) // shared table slots
#define CHUNK (1 << 20)
#define ERRBIT 0x8000

typedef struct {
  uint8_t *buf; // records: a u16 tag (id | (latency bytes - 1) << 12 | 5xx << 15), then the latency
  size_t n, cap; // in bytes
} Bucket;
typedef struct {
  uint32_t off, len; // the endpoint is arena[off..off+len)
} Ep;
typedef struct {
  Bucket b[HOURS];
  uint32_t cnt[CAP], err[CAP], pos[CAP], *lats, lcap;
  char *out;
} Th;

static const char *data;
static long size, nchunks, next_chunk;
static int out, nth, next_hour, turn;
static Th *th;
static uint64_t slots[TBL]; // 0 empty, 1 being written, else hash | id
static Ep eps[CAP];
static char arena[CAP * 128];
static uint32_t E, arena_used, ord[CAP];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; // the writer's
static char *obuf[HOURS + 1];                           // a finished hour's lines
static size_t olen[HOURS];
// Written hour buffers are recycled: a fresh mmap'd block per hour costs page
// faults, and each free a cross-CPU TLB flush.
static char *spare[HOURS];
static int nspare;
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER; // the spares'

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// The hash of endpoint s[0..n), with no loop to mispredict on its length: a CRC
// of its first 16 bytes (zero padded), its last 8 when it is longer, and n
// (reads 16 bytes from s). Never 0 (empty) or 1 (being written).
static uint64_t hash_bytes(const char *s, uint32_t n) {
  uint64_t a, b, c;
  memcpy(&a, s, 8), memcpy(&b, s + 8, 8), memcpy(&c, s + (n > 16 ? n - 8 : 0), 8);
  a &= n >= 8 ? ~0ull : (1ull << 8 * n) - 1;
  b &= n >= 16 ? ~0ull : n <= 8 ? 0 : (1ull << 8 * (n - 8)) - 1;
  c &= -(uint64_t)(n > 16);
  return _mm_crc32_u64(_mm_crc32_u64(_mm_crc32_u64(n, a), b), c) * 0x9E3779B97F4A7C15ull | 1ull << 63;
}

// The value of the 1 to 8 digits at s: shifted to the top of a word, then
// pairs, quads and octets combined (reads 8 bytes from s).
static uint64_t digits(const char *s, uint32_t k) {
  uint64_t x;
  memcpy(&x, s, 8);
  x = x << (64 - 8 * k) & 0x0F0F0F0F0F0F0F0Full;
  x = (x * 10 + (x >> 8)) & 0x00FF00FF00FF00FFull;
  x = (x * 100 + (x >> 16)) & 0x0000FFFF0000FFFFull;
  return (x * 10000 + (x >> 32)) & 0xFFFFFFFF;
}

// The id of endpoint s[0..n), inserted when new: an inserter claims an empty
// slot by swapping 0 for 1, stores the bytes, then publishes hash | id.
static uint32_t ep_id(const char *s, uint32_t n) {
  uint64_t h = hash_bytes(s, n), key = h & ~(uint64_t)(CAP - 1);
  for (uint32_t i = (h >> 12) & (TBL - 1);; i = (i + 1) & (TBL - 1)) {
    uint64_t v = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
    if (v == 0 && __atomic_compare_exchange_n(&slots[i], &v, 1, 0, __ATOMIC_ACQUIRE,
                                              __ATOMIC_ACQUIRE)) {
      uint32_t id = __atomic_fetch_add(&E, 1, __ATOMIC_RELAXED);
      if (id >= CAP)
        die("too many distinct endpoints");
      eps[id] = (Ep){__atomic_fetch_add(&arena_used, n, __ATOMIC_RELAXED), n};
      memcpy(arena + eps[id].off, s, n);
      __atomic_store_n(&slots[i], key | id, __ATOMIC_RELEASE);
      return id;
    }
    while (v == 1) // another thread is filling this slot
      v = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
    if ((v & ~(uint64_t)(CAP - 1)) == key) {
      Ep *e = &eps[v & (CAP - 1)];
      if (e->len == n && memcmp(arena + e->off, s, n) == 0)
        return v & (CAP - 1);
    }
  }
}

// The first space at or after q, 32 bytes at a time (the mapping has slack).
static const char *next_space(const char *q) {
  for (;; q += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)q);
    uint32_t m = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(' ')));
    if (m)
      return q + __builtin_ctz(m);
  }
}

// Parse the lines that start in [lo, hi) into this thread's hour buckets.
static void parse(Th *t, const char *p, const char *hi) {
  while (p < hi) {
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
    const char *e = next_space(s), *l = e + 5; // the latency: up to 10 digits, then LF
    uint32_t k = __builtin_ctz(_mm_movemask_epi8(
        _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)l), _mm_set1_epi8('\n'))));
    uint32_t lat = k > 8 ? digits(l, k - 8) * 100000000 + digits(l + k - 8, 8) : digits(l, k);
    p = l + k + 1;
    uint32_t len = lat < 1u << 8 ? 0 : lat < 1u << 16 ? 1 : lat < 1u << 24 ? 2 : 3; // bytes - 1
    uint16_t tag = ep_id(s, e - s) | len << 12 | (e[1] == '5' ? ERRBIT : 0);
    Bucket *b = &t->b[hr];
    if (b->cap - b->n < 6) // room for the tag and a full 4-byte write
      b->buf = realloc(b->buf, b->cap = b->cap ? b->cap * 2 : 16384);
    memcpy(b->buf + b->n, &tag, 2);
    memcpy(b->buf + b->n + 2, &lat, 4);
    b->n += 3 + len;
  }
}

// Chunk c holds the lines that start in [c * CHUNK, (c + 1) * CHUNK).
static void *scan(void *arg) {
  Th *t = arg;
  for (long c; (c = __atomic_fetch_add(&next_chunk, 1, __ATOMIC_RELAXED)) < nchunks;) {
    long lo = c * CHUNK, hi = lo + CHUNK < size ? lo + CHUNK : size;
    if (lo > 0) {
      const char *nl = memchr(data + lo - 1, '\n', size - lo + 1);
      lo = nl ? nl - data + 1 : size;
    }
    if (lo < hi)
      parse(t, data + lo, data + hi);
  }
  return NULL;
}

static void swap(uint32_t *a, long i, long j) {
  uint32_t x = a[i];
  a[i] = a[j], a[j] = x;
}

// The k-th smallest of a[0..n), leaving a[0..k) <= a[k] <= a[k+1..n). Each
// partition is branch-free (every element is swapped, and the boundary moves
// by the comparison), since latencies are random and branches would miss half
// the time. When nothing is below the pivot, its equals are gathered next to
// it, so runs of one value cannot make the search quadratic.
static uint32_t select_k(uint32_t *a, long n, long k) {
  for (long lo = 0, hi = n - 1, i, j; lo < hi;) {
    long mid = lo + (hi - lo) / 2; // the median of three goes to a[hi]
    if (a[mid] < a[lo])
      swap(a, mid, lo);
    if (a[hi] < a[lo])
      swap(a, hi, lo);
    if (a[mid] < a[hi])
      swap(a, mid, hi);
    uint32_t piv = a[hi], x;
    for (i = lo, j = lo; j < hi; j++)
      x = a[j], a[j] = a[i], a[i] = x, i += x < piv;
    a[hi] = a[i], a[i] = piv;
    if (k == i)
      return piv;
    if (k < i)
      hi = i - 1;
    else if (i > lo)
      lo = i + 1;
    else { // a[lo] is the least: [lo, i) becomes the run of values equal to it
      for (i = j = lo + 1; j <= hi; j++)
        x = a[j], a[j] = a[i], a[i] = x, i += x == piv;
      if (k < i)
        return piv;
      lo = i;
    }
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

// Write every finished hour from the next unwritten one on, if no one else is.
// After unlocking, check again: an hour handed over meanwhile saw the lock taken.
static void flush_ready(void) {
  while (__atomic_load_n(&obuf[__atomic_load_n(&turn, __ATOMIC_ACQUIRE)], __ATOMIC_ACQUIRE) &&
         !pthread_mutex_trylock(&mu)) {
    for (char *b; turn < HOURS && (b = __atomic_load_n(&obuf[turn], __ATOMIC_ACQUIRE));) {
      if (write(out, b, olen[turn]) != (ssize_t)olen[turn])
        die("write failed");
      pthread_mutex_lock(&smu);
      spare[nspare++] = b;
      pthread_mutex_unlock(&smu);
      __atomic_store_n(&turn, turn + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
  }
}

// Threads take whole hours: group the hour's records by endpoint, select the
// three ranks, format the lines, and hand them to the writer.
static void *hours(void *arg) {
  Th *t = arg;
  for (int hr; (hr = __atomic_fetch_add(&next_hour, 1, __ATOMIC_RELAXED)) < HOURS;) {
    uint32_t n = 0;
    memset(t->cnt, 0, E * 4);
    memset(t->err, 0, E * 4);
    for (int i = 0; i < nth; i++) {
      Bucket *b = &th[i].b[hr];
      for (size_t k = 0; k < b->n; n++) {
        uint16_t tag;
        memcpy(&tag, b->buf + k, 2);
        t->cnt[tag & (CAP - 1)]++;
        t->err[tag & (CAP - 1)] += tag >> 15;
        k += 3 + (tag >> 12 & 3);
      }
    }
    if (n > t->lcap)
      t->lats = realloc(t->lats, (t->lcap = n) * 4ull);
    for (uint32_t id = 0, at = 0; id < E; id++)
      t->pos[id] = at, at += t->cnt[id];
    for (int i = 0; i < nth; i++) {
      Bucket *b = &th[i].b[hr];
      for (size_t k = 0; k < b->n;) {
        uint16_t tag;
        uint32_t lat;
        memcpy(&tag, b->buf + k, 2);
        memcpy(&lat, b->buf + k + 2, 4); // 4 bytes were always written; keep the record's
        t->lats[t->pos[tag & (CAP - 1)]++] = lat & 0xFFFFFFFFu >> (8 * (3 - (tag >> 12 & 3)));
        k += 3 + (tag >> 12 & 3);
      }
      free(b->buf); // the hour's records are no longer needed
    }
    if (!t->out) { // a line is at most 21 + 125 + 57 + 1 = 204 bytes
      pthread_mutex_lock(&smu);
      t->out = nspare ? spare[--nspare] : malloc((size_t)E * 256 + 64);
      pthread_mutex_unlock(&smu);
    }
    int day = (hr + 18) / 24;
    char *o = t->out;
    snprintf(o, 32, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11, day < 2 ? 30 + day : day - 1,
             (hr + 18) % 24);
    char hs[21];
    memcpy(hs, o, 21);
    for (uint32_t r = 0; r < E; r++) {
      uint32_t id = ord[r];
      long c = t->cnt[id];
      if (!c)
        continue;
      uint32_t *a = t->lats + t->pos[id] - c;
      long k99 = (99 * c + 99) / 100 - 1, k95 = (95 * c + 99) / 100 - 1, k50 = (50 * c + 99) / 100 - 1;
      uint32_t v99 = select_k(a, c, k99), v95 = select_k(a, k99 + 1, k95), v50 = select_k(a, k95 + 1, k50);
      memcpy(o, hs, 21);
      memcpy(o + 21, arena + eps[id].off, eps[id].len);
      o = num(o + 21 + eps[id].len, c);
      o = num(o, v50);
      o = num(o, v95);
      o = num(o, v99);
      o = num(o, t->err[id]);
      o = num(o, (uint64_t)t->err[id] * 1000000 / c);
      *o++ = '\n';
    }
    // Hand the hour over: whoever holds the writer lock writes every finished hour
    // in order, so no thread ever waits for another's hour.
    olen[hr] = o - t->out;
    __atomic_store_n(&obuf[hr], t->out, __ATOMIC_RELEASE);
    t->out = NULL;
    flush_ready();
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

static int cmp_ep(const void *x, const void *y) {
  const Ep *a = &eps[*(const uint32_t *)x], *b = &eps[*(const uint32_t *)y];
  int c = memcmp(arena + a->off, arena + b->off, a->len < b->len ? a->len : b->len);
  return c ? c : (a->len > b->len) - (a->len < b->len);
}

int main(void) {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // 2 MB pages would be charged whole
  struct stat st;
  int fd = open("/data/requests.log", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    die("cannot open /data/requests.log");
  size = st.st_size;
  // Map the file over a zeroed region one page longer, so that reads a little
  // past the last line (next_space, hash_bytes) stay inside mapped memory.
  char *m = mmap(NULL, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED || (size && mmap(m, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED))
    die("mmap failed");
  data = m;
  nchunks = (size + CHUNK - 1) / CHUNK;
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth > nchunks ? nchunks : nth > 64 ? 64 : nth; // no more threads than chunks
  nth = nth < 1 ? 1 : nth;
  th = calloc(nth, sizeof(Th));
  run_threads(scan);

  for (uint32_t i = 0; i < E; i++) // rank the endpoints bytewise
    ord[i] = i;
  qsort(ord, E, 4, cmp_ep);
  out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    die("cannot create /out/summary.txt");
  run_threads(hours);
  flush_ready(); // anything a racing handover left behind
  if (close(out))
    die("write failed");
  return 0;
}
