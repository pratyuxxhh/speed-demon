// Speed Demon 2026, Track C (Join): parse a hostile records.csv, join each record
// to regions.csv on region_id, and aggregate per country. See specs/c-join.md.
//
// records.csv is mapped and cut into 1 MB chunks that threads take from a
// counter. Spec section 2.2 guarantees that a note's LF is followed by a
// lowercase word and that names hold no LF, so an LF followed by a digit or a
// BOM is a record boundary, found from any offset. A chunk is parsed 64 bytes at
// a time with AVX2 masks: a prefix XOR of the quote bits says which bytes are
// inside quotes (a "" escape flips twice), and the commas and LFs outside
// quotes are the field ends. Records are parsed in batches whose name-table and
// region-map lines are prefetched, then applied. Distinct names use a table of
// 1-byte tags and 64-byte records (the name's first 24 bytes, its length, the
// set of groups it occurs in), compared byte for byte.
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

#ifndef CHUNK
#define CHUNK (1 << 20)
#endif
#define MAPN 10000000 // region ids at or above this are unmatched
#define MAXG 256      // groups: _UNMATCHED (0) plus the countries (240 in the spec)
#define INL 24        // name bytes kept in the record; longer names keep the rest in the arena
#define BATCH 32
#define BUSY 0xFFFF

typedef struct {
  uint64_t count, id_sum, name_bytes, note_bytes;
  int64_t sum, min, max;
} Agg;
typedef struct {
  uint8_t name[INL]; // zero padded
  uint32_t len, off; // off: where arena holds the bytes past INL
  uint64_t groups[4];
} Rec; // 64 bytes, one cache line
typedef struct {
  const char *name;
  uint64_t h, id;
  uint32_t len, xlen, rid;
  int64_t cents;
} Pend;
typedef struct {
  long beg, end; // this thread's slice of regions.csv
  Agg agg[MAXG];
  uint64_t distinct[MAXG];
  Pend pend[BATCH];
  int np;
  char *tmp;
} Th;

static const char *data, *rdata;
static long size, rsize, body, next_chunk, nchunks;
static int nth, ngroups = 1, overflow;
static Th *th;
static uint8_t *region; // region id -> group, 0 = _UNMATCHED
static uint16_t by_code[26 * 26 * 26]; // country code -> group; 0 unseen, BUSY being assigned
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

// Map a file over a zeroed region one page longer, so that 64-byte loads a little
// past its end stay inside mapped memory and see only zeros.
static const char *map_file(const char *path, long *len) {
  struct stat st;
  int f = open(path, O_RDONLY);
  if (f < 0 || fstat(f, &st))
    die("cannot open input");
  *len = st.st_size;
  char *m = mmap(NULL, *len + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED || (*len && mmap(m, *len, PROT_READ, MAP_PRIVATE | MAP_FIXED, f, 0) == MAP_FAILED))
    die("mmap failed");
  close(f);
  return m;
}

static uint64_t hash_bytes(const char *s, uint32_t n) { // reads up to 7 bytes past s + n
  uint64_t h = n * 0x9E3779B97F4A7C15ull, w;
  for (uint32_t i = 0; i < n; i += 8) {
    memcpy(&w, s + i, 8);
    if (n - i < 8)
      w &= (1ull << (8 * (n - i))) - 1;
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
  }
  return h;
}

// The record of name s[0..n), inserted when new: an inserter claims an empty tag
// by swapping 0 for 1, fills the record, then publishes the tag.
static Rec *name_rec(const char *s, uint32_t n, uint64_t h) {
  uint8_t tag = 0x80 | h >> 57;
  Rec key = {{0}, n, 0, {0}};
  memcpy(key.name, s, n < INL ? n : INL);
  __m256i k = _mm256_loadu_si256((const __m256i *)&key);
  for (uint32_t i = h & (cap - 1);; i = (i + 1) & (cap - 1)) {
    uint8_t v = __atomic_load_n(&tags[i], __ATOMIC_ACQUIRE);
    if (v == 0) {
      if (__atomic_load_n(&nnames, __ATOMIC_RELAXED) >= cap / 8 * 7) {
        overflow = 1; // too many names for this table: the pass is redone, larger
        return recs;
      }
      if (__atomic_compare_exchange_n(&tags[i], &v, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        if (n > INL) {
          key.off = __atomic_fetch_add(&arena_used, n - INL, __ATOMIC_RELAXED);
          memcpy(arena + key.off, s + INL, n - INL);
        }
        recs[i] = key;
        __atomic_fetch_add(&nnames, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&tags[i], tag, __ATOMIC_RELEASE);
        return &recs[i];
      }
    }
    while (v == 1) // another thread is filling this slot
      v = __atomic_load_n(&tags[i], __ATOMIC_ACQUIRE);
    if (v == tag) { // compare the name's first 24 bytes and its length at once
      __m256i r = _mm256_loadu_si256((const __m256i *)&recs[i]);
      uint32_t eq = _mm256_movemask_epi8(_mm256_cmpeq_epi8(r, k));
      if ((eq & 0x0FFFFFFF) == 0x0FFFFFFF &&
          (n <= INL || memcmp(arena + recs[i].off, s + INL, n - INL) == 0))
        return &recs[i];
    }
  }
}

static void apply(Th *t, const Pend *p) {
  uint32_t g = p->rid < MAPN ? region[p->rid] : 0;
  Agg *a = &t->agg[g];
  a->count++;
  a->id_sum += p->id;
  a->sum += p->cents;
  a->min = p->cents < a->min ? p->cents : a->min;
  a->max = p->cents > a->max ? p->cents : a->max;
  a->name_bytes += p->len;
  a->note_bytes += p->xlen;
  uint64_t *w = &name_rec(p->name, p->len, p->h)->groups[g / 64], bit = 1ull << (g % 64);
  if (!(__atomic_load_n(w, __ATOMIC_RELAXED) & bit)) // a plain read first: lock or is a full barrier
    __atomic_fetch_or(w, bit, __ATOMIC_RELAXED);
}

static void flush(Th *t) {
  for (int i = 0; i < t->np; i++)
    apply(t, &t->pend[i]);
  t->np = 0;
}

static uint64_t eq64(__m256i lo, __m256i hi, char c) {
  __m256i v = _mm256_set1_epi8(c);
  return (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hi, v)) << 32 |
         (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(lo, v));
}

static uint64_t prefix_xor(uint64_t x) {
  x ^= x << 1, x ^= x << 2, x ^= x << 4, x ^= x << 8, x ^= x << 16;
  return x ^ x << 32;
}

static uint64_t digits(const char *s, const char *e) {
  uint64_t v = 0;
  for (; s < e; s++)
    v = v * 10 + (*s - '0');
  return v;
}

// One record from its five fields [f[i], fe[i]) and the quote counts of the
// name and note; queue it, prefetching what applying it will touch.
static void record(Th *t, const char **f, const char **fe, const uint64_t *fq) {
  const char *s = f[0];
  if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
    s += 3; // a byte order mark at a record start is not data
  Pend *p = &t->pend[t->np];
  p->id = digits(s, fe[0]);
  p->rid = (uint32_t)(fe[3] - f[3] > 8 ? MAPN : digits(f[3], fe[3]));
  // A quoted field of raw length L holding Q quote bytes has L - 1 - Q/2 value bytes.
  uint32_t nl = fe[1] - f[1], xl = fe[2] - f[2];
  int nq = nl && *f[1] == '"';
  p->len = nq ? nl - 1 - fq[1] / 2 : nl;
  p->xlen = xl && *f[2] == '"' ? xl - 1 - fq[2] / 2 : xl;
  const char *a = f[4], *ae = fe[4] - (fe[4] > f[4] && fe[4][-1] == '\r');
  int neg = *a == '-';
  const char *dot = ae - 3; // "-?[0-9]+.[0-9]{2}"
  int64_t c = (int64_t)digits(a + neg, dot) * 100 + (dot[1] - '0') * 10 + (dot[2] - '0');
  p->cents = neg ? -c : c;
  p->name = nq ? f[1] + 1 : f[1];
  if (nq && fq[1] > 2) { // escaped quotes: unescape into tmp and apply now
    if (p->len > CHUNK)
      die("name longer than the scratch buffer");
    flush(t);
    char *o = t->tmp;
    for (const char *q = p->name; o < t->tmp + p->len; q++) {
      *o++ = *q;
      q += *q == '"';
    }
    p->name = t->tmp;
    p->h = hash_bytes(p->name, p->len);
    apply(t, p);
    return;
  }
  p->h = hash_bytes(p->name, p->len);
  __builtin_prefetch(&tags[p->h & (cap - 1)]);
  __builtin_prefetch(&recs[p->h & (cap - 1)], 1);
  __builtin_prefetch(&region[p->rid < MAPN ? p->rid : 0]);
  if (++t->np == BATCH)
    flush(t);
}

// Parse the records that start in [b, e). b is a record start, so the quote
// state there is "outside".
static void parse_range(Th *t, const char *b, const char *e) {
  const char *f[5] = {b}, *fe[5], *end = data + size;
  uint64_t fq[5], carry = 0, qc = 0, qs = 0; // quotes before the block; at the field's start
  int k = 0;
  for (const char *p = b;; p += 64) {
    if (p >= end) { // end of file: an unterminated last record
      if (k == 4) {
        fe[4] = end;
        record(t, f, fe, fq);
      } else if (k || f[0] < end) {
        die("malformed records.csv");
      }
      return;
    }
    __m256i lo = _mm256_loadu_si256((const __m256i *)p), hi = _mm256_loadu_si256((const __m256i *)(p + 32));
    uint64_t q = eq64(lo, hi, '"'), inq = prefix_xor(q) ^ carry;
    carry = (uint64_t)((int64_t)inq >> 63);
    for (uint64_t sep = (eq64(lo, hi, ',') | eq64(lo, hi, '\n')) & ~inq; sep; sep &= sep - 1) {
      int i = __builtin_ctzll(sep);
      uint64_t cum = qc + __builtin_popcountll(q & ((2ull << i) - 1));
      fe[k] = p + i, fq[k] = cum - qs, qs = cum;
      if (p[i] == '\n') {
        if (k != 4)
          die("malformed records.csv");
        record(t, f, fe, fq);
        k = 0;
        if (p + i + 1 >= e)
          return;
      } else if (++k > 4) {
        die("malformed records.csv");
      }
      f[k] = p + i + 1;
    }
    qc += __builtin_popcountll(q);
  }
}

// The first record start at or after data + o: just past an LF that a digit or a
// BOM follows (see the header comment).
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
    const char *b = record_start(body + c * CHUNK), *e = record_start(body + (c + 1) * CHUNK);
    if (b < e)
      parse_range(t, b, e);
  }
  flush(t);
  return NULL;
}

// This thread's slice of regions.csv: "region_id,XYZ" lines. A country's group
// is claimed once by swapping 0 for BUSY in its code's slot.
static void *regions(void *arg) {
  Th *t = arg;
  for (const char *p = rdata + t->beg, *end = rdata + t->end; p < end; p += 5) { // ",XYZ\n"
    uint64_t rid = 0;
    for (; *p != ','; p++)
      rid = rid * 10 + (*p - '0');
    int code = ((p[1] - 'A') * 26 + (p[2] - 'A')) * 26 + (p[3] - 'A');
    uint16_t g = __atomic_load_n(&by_code[code], __ATOMIC_ACQUIRE), z = 0;
    if (!g && __atomic_compare_exchange_n(&by_code[code], &z, BUSY, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
      g = __atomic_fetch_add(&ngroups, 1, __ATOMIC_RELAXED);
      if (g >= MAXG)
        die("too many countries");
      memcpy(codes[g], p + 1, 3);
      __atomic_store_n(&by_code[code], g, __ATOMIC_RELEASE);
    }
    while (!g || g == BUSY)
      g = __atomic_load_n(&by_code[code], __ATOMIC_ACQUIRE);
    if (rid < MAPN)
      region[rid] = g;
  }
  return NULL;
}

// Thread k counts the groups of every nth name slot, starting at slot k.
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

static char *money(char *o, int64_t c) {
  uint64_t a = c < 0 ? -(uint64_t)c : (uint64_t)c;
  return o + sprintf(o, "%s%llu.%02llu", c < 0 ? "-" : "", (unsigned long long)(a / 100),
                     (unsigned long long)(a % 100));
}

static int cmp_group(const void *x, const void *y) {
  return strcmp(codes[*(const int *)x], codes[*(const int *)y]);
}

int main(void) {
  rdata = map_file("/data/regions.csv", &rsize);
  data = map_file("/data/records.csv", &size);
  if (size > UINT32_MAX)
    die("records.csv over 4 GB"); // arena offsets are 32-bit
  const char *nl = memchr(data, '\n', size); // the BOM and header line
  body = nl ? nl - data + 1 : size;
  nchunks = (size - body + CHUNK - 1) / CHUNK;
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth > nchunks ? nchunks : nth > 64 ? 64 : nth; // no more threads than chunks
  nth = nth < 1 ? 1 : nth;
  th = calloc(nth, sizeof(Th));

  // regions.csv in parallel: line-aligned slices after the header.
  region = calloc(MAPN, 1); // only the pages up to the largest id get touched
  nl = memchr(rdata, '\n', rsize);
  long rbody = nl ? nl - rdata + 1 : rsize;
  for (int i = 0; i < nth; i++) {
    long b = rbody + (rsize - rbody) * i / nth;
    const char *q = i ? memchr(rdata + b - 1, '\n', rsize - b + 1) : NULL;
    th[i].beg = i ? (q ? q - rdata + 1 : rsize) : rbody;
    th[i].tmp = malloc(CHUNK + 64);
    if (i)
      th[i - 1].end = th[i].beg;
  }
  th[nth - 1].end = rsize;
  run_threads(regions);

  arena = malloc(size + 64); // untouched pages cost nothing
  for (cap = 1 << 17;; cap *= 2) { // redone with a larger name table if it fills
    tags = calloc(cap, 1);
    recs = aligned_alloc(64, cap * sizeof(Rec)); // (huge pages: 11% faster, +5.8 MB; not worth it)
    nnames = overflow = arena_used = next_chunk = 0;
    run_threads(parse);
    if (!overflow)
      break;
    free(tags), free(recs);
  }
  run_threads(count_names);

  // Merge the threads' aggregates and write the groups in byte order of key.
  int order[MAXG], n = 0;
  Agg tot[MAXG];
  uint64_t distinct[MAXG] = {0};
  for (int g = 0; g < ngroups; g++) {
    tot[g] = th[0].agg[g];
    for (int i = 0; i < nth; i++)
      distinct[g] += th[i].distinct[g];
    for (int i = 1; i < nth; i++) {
      Agg *a = &th[i].agg[g];
      tot[g].count += a->count;
      tot[g].id_sum += a->id_sum;
      tot[g].sum += a->sum;
      tot[g].name_bytes += a->name_bytes;
      tot[g].note_bytes += a->note_bytes;
      tot[g].min = a->min < tot[g].min ? a->min : tot[g].min;
      tot[g].max = a->max > tot[g].max ? a->max : tot[g].max;
    }
    if (tot[g].count)
      order[n++] = g;
  }
  qsort(order, n, sizeof *order, cmp_group);
  FILE *out = fopen("/out/aggregates.txt", "w");
  if (!out)
    die("cannot create /out/aggregates.txt");
  for (int k = 0; k < n; k++) {
    Agg *a = &tot[order[k]];
    char line[256], *o = line;
    o += sprintf(o, "%s %llu %llu ", codes[order[k]], (unsigned long long)a->count,
                 (unsigned long long)a->id_sum);
    o = money(o, a->sum);
    *o++ = ' ';
    o = money(o, a->min);
    *o++ = ' ';
    o = money(o, a->max);
    sprintf(o, " %llu %llu %llu\n", (unsigned long long)distinct[order[k]],
            (unsigned long long)a->name_bytes, (unsigned long long)a->note_bytes);
    fputs(line, out);
  }
  if (fclose(out))
    die("write failed");
  return 0;
}
