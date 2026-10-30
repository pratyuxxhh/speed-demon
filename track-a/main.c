// Speed Demon 2026, Track A (Ingest): per (UTC hour, endpoint) request count,
// nearest-rank p50/p95/p99 latency and 5xx count/ppm. See specs/a-ingest.md.
//
// One pass over the text: requests.log is mapped read-only and cut into 1 MB
// chunks that threads take from a counter. Each line's endpoint is interned in
// one shared lock-free table, and its latency (u32) and tag (endpoint id | 5xx
// bit, u16) are appended to the thread's bucket for that hour. Then threads take
// whole hours: gather the hour's records from every thread, group them by
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
#include <sys/stat.h>
#include <unistd.h>

#define HOURS 72      // 2026-10-30T18:00Z to 2026-11-02T17:59Z, fixed by the spec
#define CAP 4096      // max distinct endpoints (12-bit ids); the spec promises 1,842
#define TBL (CAP * 2) // shared table slots
#define CHUNK (1 << 20)
#define ERRBIT 0x8000

typedef struct {
  uint32_t *lat, n, cap;
  uint16_t *tag;
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

static uint64_t hash_bytes(const char *s, uint32_t n) { // reads up to 7 bytes past s + n
  uint64_t h = n * 0x9E3779B97F4A7C15ull, w;
  for (uint32_t i = 0; i < n; i += 8) {
    memcpy(&w, s + i, 8);
    if (n - i < 8)
      w &= (1ull << (8 * (n - i))) - 1;
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
  }
  return h | 1ull << 63; // never 0 (empty) or 1 (being written)
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
    const char *e = next_space(s);
    uint16_t tag = ep_id(s, e - s) | (e[1] == '5' ? ERRBIT : 0);
    uint32_t lat = 0;
    for (p = e + 5; *p != '\n'; p++)
      lat = lat * 10 + (*p - '0');
    p++;
    Bucket *b = &t->b[hr];
    if (b->n == b->cap) {
      b->cap = b->cap ? b->cap * 2 : 4096;
      b->lat = realloc(b->lat, b->cap * 4ull);
      b->tag = realloc(b->tag, b->cap * 2ull);
    }
    b->lat[b->n] = lat;
    b->tag[b->n++] = tag;
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
      for (uint32_t k = 0; k < b->n; k++) {
        t->cnt[b->tag[k] & (CAP - 1)]++;
        t->err[b->tag[k] & (CAP - 1)] += b->tag[k] >> 15;
      }
      n += b->n;
    }
    if (n > t->lcap)
      t->lats = realloc(t->lats, (t->lcap = n) * 4ull);
    for (uint32_t id = 0, at = 0; id < E; id++)
      t->pos[id] = at, at += t->cnt[id];
    for (int i = 0; i < nth; i++) {
      Bucket *b = &th[i].b[hr];
      for (uint32_t k = 0; k < b->n; k++)
        t->lats[t->pos[b->tag[k] & (CAP - 1)]++] = b->lat[k];
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
