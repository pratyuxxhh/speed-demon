// Speed Demon 2026, Track C (Join): parse a hostile records.csv, join each
// record to regions.csv on region_id, and aggregate per country. See
// specs/c-join.md; this relies on section 2.2, which the spec allows.
//
// records.csv is mapped and cut into 1 MB chunks that threads take from a
// counter. An LF followed by a digit or a BOM starts a record (notes continue
// a line with a lowercase word, and names hold no LF), so a chunk's first
// record is found from any offset. A chunk is read 64 bytes at a time: a
// prefix XOR of the quote bits marks the bytes inside quotes (a "" escape
// flips twice), and the commas and LFs outside them end the fields; every
// fifth one ends a record. Numbers are read eight digits at once. Records
// are queued in batches whose name-table and region-map lines are
// prefetched. A distinct name has a 64-byte record: its first 24 bytes, its
// length, an arena offset for the rest, and the set of groups it occurs in.
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

#ifndef CHUNK
#define CHUNK (1 << 20)
#endif
#define MAPN 10000000 // region ids at or above this are unmatched
#define MAXG 256      // groups: _UNMATCHED (0) and the countries
#define INL 24        // name bytes kept in the record
#define BATCH 32

typedef struct {
  uint64_t count, id_sum, name_bytes, note_bytes;
  int64_t sum, min, max;
} Agg;
typedef struct {
  uint8_t name[INL]; // zero padded
  uint32_t len, off; // off: where arena holds the bytes past INL
  uint64_t groups[4];
} Rec; // one cache line
typedef struct {
  const char *name; // as written, between its quotes
  uint32_t h, len, raw, xlen, rid; // len: after unescaping; raw: as written
  uint64_t id;
  int64_t cents;
} Pend;
typedef struct __attribute__((aligned(64))) {
  Agg agg[MAXG];
  uint64_t distinct[MAXG];
  Pend pend[BATCH];
  int np;
} Th;

static const char *data;
static long size, body, next_chunk, nchunks;
static int nth, ngroups = 1, overflow;
static Th *th;
static uint8_t *region, by_code[26 * 26 * 26]; // id or code -> group
static char codes[MAXG][12] = {"_UNMATCHED"};
static uint8_t *tags; // 0 empty, 1 being written, else 0x80 | 7 hash bits
static Rec *recs;
static uint32_t cap, nnames;
static char *arena; // name bytes past INL; at most the size of records.csv
static uint64_t arena_used;

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// The file, mapped over zeros a page longer, so 64-byte loads past its end
// stay mapped.
static const char *map_file(const char *path, long *len) {
  struct stat st;
  int f = open(path, O_RDONLY);
  if (f < 0 || fstat(f, &st))
    die("cannot open input");
  *len = st.st_size;
  char *m = mmap(NULL, *len + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED ||
      (*len && mmap(m, *len, PROT_READ, MAP_PRIVATE | MAP_FIXED, f, 0) == MAP_FAILED))
    die("mmap failed");
  return m;
}

// The value of the 1 to 8 digits at s: shifted to the top of a word, then
// pairs, quads and octets combined (reads 8 bytes from s).
static uint64_t num(const char *s, long n) {
  uint64_t v;
  memcpy(&v, s, 8);
  v = v << (64 - 8 * n) & 0x0F0F0F0F0F0F0F0Full;
  v = (v * 10 + (v >> 8)) & 0x00FF00FF00FF00FFull;
  v = (v * 100 + (v >> 16)) & 0x0000FFFF0000FFFFull;
  return (v * 10000 + (v >> 32)) & 0xFFFFFFFF;
}

// A name as its record starts: its first 24 bytes, zero padded, then its
// length (reads 32 bytes from s).
static __m256i key(const char *s, uint32_t n) {
  __m256i k = _mm256_and_si256(
      _mm256_loadu_si256((const __m256i *)s),
      _mm256_cmpgt_epi8(_mm256_set1_epi8(n < INL ? n : INL),
                        _mm256_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                         13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                         23, 24, 25, 26, 27, 28, 29, 30, 31)));
  return _mm256_insert_epi32(k, n, 6);
}

// The record of a queued record's name, inserted when new: an inserter claims
// an empty tag by swapping 0 for 1, fills the record, then publishes the tag.
static Rec *name_rec(const Pend *p) {
  const char *s = p->name;
  uint32_t n = p->raw, tag = 0x80 | p->h >> 25;
  __m256i k = key(s, n); // again: cheaper than queueing 32 bytes more
  for (uint32_t i = p->h & (cap - 1);; i = (i + 1) & (cap - 1)) {
    uint8_t v = __atomic_load_n(&tags[i], __ATOMIC_ACQUIRE);
    if (!v && __atomic_load_n(&nnames, __ATOMIC_RELAXED) >= cap / 8 * 7) {
      overflow = 1; // too many names: the pass is redone with a larger table
      return recs;
    }
    if (!v && __atomic_compare_exchange_n(&tags[i], &v, 1, 0, __ATOMIC_ACQUIRE,
                                          __ATOMIC_ACQUIRE)) {
      Rec *x = &recs[i];
      _mm256_storeu_si256((__m256i *)x, k), memset(x->groups, 0, 32);
      if (n > INL) {
        x->off = __atomic_fetch_add(&arena_used, n - INL, __ATOMIC_RELAXED);
        memcpy(arena + x->off, s + INL, n - INL);
      }
      __atomic_fetch_add(&nnames, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&tags[i], tag, __ATOMIC_RELEASE);
      return &recs[i];
    }
    while (v == 1) // another thread is filling this slot
      v = __atomic_load_n(&tags[i], __ATOMIC_ACQUIRE);
    if (v != tag) // the record line is read only for a matching tag
      continue;
    __m256i r = _mm256_loadu_si256((const __m256i *)&recs[i]);
    if ((_mm256_movemask_epi8(_mm256_cmpeq_epi8(r, k)) & 0x0FFFFFFF) == 0x0FFFFFFF &&
        (n <= INL || !memcmp(arena + recs[i].off, s + INL, n - INL)))
      return &recs[i]; // the first 24 bytes and the length at once
  }
}

// The queued records applied: aggregates, and each name's group bit.
static void flush(Th *t) {
  for (Pend *p = t->pend; p < t->pend + t->np; p++) {
    uint32_t g = region[p->rid];
    Agg *a = &t->agg[g];
    a->count++, a->id_sum += p->id, a->sum += p->cents;
    a->min = p->cents < a->min ? p->cents : a->min;
    a->max = p->cents > a->max ? p->cents : a->max;
    a->name_bytes += p->len, a->note_bytes += p->xlen;
    uint64_t *w = &name_rec(p)->groups[g / 64];
    if (!(__atomic_load_n(w, __ATOMIC_RELAXED) >> g % 64 & 1)) // lock is costly
      __atomic_fetch_or(w, 1ull << g % 64, __ATOMIC_RELAXED);
  }
  t->np = 0;
}

// One record from its fields [f[i], fe[i]) and the quote bytes in each. A
// quoted field of raw length L with Q quote bytes holds L - 1 - Q/2 bytes;
// by 2.2 a name or note that is not quoted is empty. A name is known by its
// bytes as written: escaping (" as "") is one to one, so two names are equal
// exactly when their written forms are, and nothing is unescaped.
static void record(Th *t, const char **f, const char **fe, const uint64_t *fq) {
  const char *s = f[0] + 3 * ((uint8_t)*f[0] == 0xEF); // a BOM is not data
  const char *a = f[4], *ae = fe[4] - (fe[4][-1] == '\r');
  uint32_t nl = fe[1] - f[1], xl = fe[2] - f[2];
  int neg = *a == '-';
  int64_t c = num(a + neg, ae - 3 - a - neg) * 100 + (ae[-2] - '0') * 10 +
              (ae[-1] - '0');
  Pend *p = &t->pend[t->np];
  *p = (Pend){f[1] + !!nl, 0, nl ? nl - 1 - fq[1] / 2 : 0, nl ? nl - 2 : 0,
              xl ? xl - 1 - fq[2] / 2 : 0, fe[3] - f[3] > 7 ? MAPN : num(f[3], fe[3] - f[3]),
              num(s, fe[0] - s), neg ? -c : c};
  __m256i k = key(p->name, p->raw); // its CRC: no loop on the name's length
  uint64_t h = _mm_crc32_u64(0, _mm256_extract_epi64(k, 0));
  h = _mm_crc32_u64(h, _mm256_extract_epi64(k, 1));
  h = _mm_crc32_u64(h, _mm256_extract_epi64(k, 2));
  p->h = _mm_crc32_u64(h, _mm256_extract_epi64(k, 3));
  __builtin_prefetch(&recs[p->h & (cap - 1)], 1);
  __builtin_prefetch(&region[p->rid]);
  if (++t->np == BATCH)
    flush(t);
}

static uint64_t eq64(__m256i lo, __m256i hi, char c) {
  __m256i v = _mm256_set1_epi8(c);
  return (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hi, v)) << 32 |
         (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(lo, v));
}

// The records that start in [b, e); b is a record start, so outside quotes.
static void parse_range(Th *t, const char *b, const char *e) {
  const char *f[5] = {b}, *fe[5];
  uint64_t fq[5], carry = 0, qc = 0, qs = 0; // quotes so far; at field start
  int k = 0;
  for (const char *p = b;; p += 64) {
    __m256i lo = _mm256_loadu_si256((const __m256i *)p),
            hi = _mm256_loadu_si256((const __m256i *)(p + 32));
    uint64_t q = eq64(lo, hi, '"'), in = _mm_cvtsi128_si64(_mm_clmulepi64_si128(
        _mm_cvtsi64_si128(q), _mm_set1_epi8(-1), 0));
    in ^= carry, carry = (uint64_t)((int64_t)in >> 63);
    uint64_t sep = (eq64(lo, hi, ',') | eq64(lo, hi, '\n')) & ~in;
    for (; sep; sep &= sep - 1) {
      int i = __builtin_ctzll(sep);
      uint64_t cum = qc + __builtin_popcountll(q & ((2ull << i) - 1));
      fe[k] = p + i, fq[k] = cum - qs, qs = cum;
      if (k < 4) {
        f[++k] = p + i + 1;
        continue;
      }
      record(t, f, fe, fq);
      if (p + i + 1 >= e)
        return;
      k = 0, f[0] = p + i + 1;
    }
    qc += __builtin_popcountll(q);
  }
}

// The first record start at or after data + o: just past an LF that a digit
// or a BOM follows.
static const char *record_start(long o) {
  if (o <= body)
    return data + body;
  for (const char *p = data + o - 1, *end = data + size; p < end; p++) {
    if (!(p = memchr(p, '\n', end - p)))
      break;
    if (p + 1 < end && ((p[1] >= '0' && p[1] <= '9') || (uint8_t)p[1] == 0xEF))
      return p + 1;
  }
  return data + size;
}

static void *parse(void *arg) {
  Th *t = arg;
  for (int g = 0; g < ngroups; g++)
    t->agg[g] = (Agg){0, 0, 0, 0, 0, INT64_MAX, INT64_MIN};
  for (long c; (c = __atomic_fetch_add(&next_chunk, 1, __ATOMIC_RELAXED)) < nchunks;) {
    const char *b = record_start(body + c * CHUNK),
               *e = record_start(body + (c + 1) * CHUNK);
    if (b < e) // the chunk's pages in one call, not a fault each
      madvise((void *)((uintptr_t)b & ~4095ul), e - b + 4096, 22), // POPULATE_READ
          parse_range(t, b, e);
  }
  flush(t);
  return NULL;
}

// Thread k counts the groups of every nth name slot, from slot k.
static void *count_names(void *arg) {
  Th *t = arg;
  for (uint32_t i = t - th; i < cap; i += nth)
    for (int w = 0; tags[i] & 0x80 && w < 4; w++)
      for (uint64_t x = recs[i].groups[w]; x; x &= x - 1)
        t->distinct[w * 64 + __builtin_ctzll(x)]++;
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

static void money(FILE *o, int64_t c) {
  uint64_t a = c < 0 ? -(uint64_t)c : (uint64_t)c;
  fprintf(o, " %s%lu.%02lu", c < 0 ? "-" : "", a / 100, a % 100);
}

static int by_code_name(const void *x, const void *y) {
  return strcmp(codes[*(const int *)x], codes[*(const int *)y]);
}

int main(void) {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // 2 MB pages would be charged whole
  // regions.csv: "region_id,XYZ" lines after a header.
  long rsize;
  const char *r = map_file("/data/regions.csv", &rsize), *re = r + rsize;
  region = calloc(MAPN + 1, 1); // only the pages written are charged
  for (const char *p = (const char *)memchr(r, '\n', rsize) + 1; p < re; p += 5) {
    uint32_t rid = 0;
    for (; *p != ','; p++)
      rid = rid * 10 + (*p - '0');
    int c = ((p[1] - 'A') * 26 + p[2] - 'A') * 26 + p[3] - 'A';
    if (!by_code[c]) {
      if (ngroups == MAXG)
        die("too many countries");
      memcpy(codes[ngroups], p + 1, 3), by_code[c] = ngroups++;
    }
    if (rid < MAPN) // region[MAPN] stays 0: _UNMATCHED
      region[rid] = by_code[c];
  }

  data = map_file("/data/records.csv", &size);
  if (size > UINT32_MAX)
    die("records.csv over 4 GB"); // arena offsets are 32-bit
  body = (const char *)memchr(data, '\n', size) - data + 1; // past BOM, header
  nchunks = (size - body + CHUNK - 1) / CHUNK;
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth > nchunks ? nchunks : nth > 64 ? 64 : nth;
  nth = nth < 1 ? 1 : nth;
  th = aligned_alloc(64, nth * sizeof(Th)), memset(th, 0, nth * sizeof(Th));
  arena = malloc(size + 64); // untouched pages cost nothing
  for (cap = 1 << 17;; cap *= 2) { // redone with a larger table if it fills
    tags = calloc(cap, 1);
    recs = aligned_alloc(64, cap * sizeof(Rec));
    nnames = overflow = arena_used = next_chunk = 0;
    run_threads(parse);
    if (!overflow)
      break;
    free(tags), free(recs);
  }
  run_threads(count_names);

  // The threads' aggregates merged, and the groups in byte order of key.
  int order[MAXG], n = 0;
  for (int g = 0; g < ngroups; g++) {
    Agg *t0 = &th[0].agg[g];
    for (int i = 1; i < nth; i++) {
      Agg *a = &th[i].agg[g];
      t0->count += a->count, t0->id_sum += a->id_sum, t0->sum += a->sum;
      t0->name_bytes += a->name_bytes, t0->note_bytes += a->note_bytes;
      t0->min = a->min < t0->min ? a->min : t0->min;
      t0->max = a->max > t0->max ? a->max : t0->max;
      th[0].distinct[g] += th[i].distinct[g];
    }
    if (t0->count)
      order[n++] = g;
  }
  qsort(order, n, sizeof *order, by_code_name);
  FILE *out = fopen("/out/aggregates.txt", "w");
  if (!out)
    die("cannot create /out/aggregates.txt");
  for (int k = 0; k < n; k++) {
    Agg *a = &th[0].agg[order[k]];
    fprintf(out, "%s %lu %lu", codes[order[k]], a->count, a->id_sum);
    money(out, a->sum), money(out, a->min), money(out, a->max);
    fprintf(out, " %lu %lu %lu\n", th[0].distinct[order[k]], a->name_bytes,
            a->note_bytes);
  }
  if (fclose(out))
    die("write failed");
  return 0;
}
