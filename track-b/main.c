// Speed Demon 2026, Track B (Index). See specs/b-index.md.
//
// Only query terms are looked up. docs.txt is mapped and cut into pieces that
// threads take in order; each thread scans its lines in batches of 64 and
// sets, in pr[slot], one bit per document for every term and phrase it holds.
// A phrase is found at its anchor, the adjacent pair of its words that is
// rarest, and checked against the line's term ids. After a batch, the queries
// that can match (one of their trigger slots shows up) run left to right on
// those words, and their matches are counted at once.
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
#include <sys/stat.h>
#include <unistd.h>

#define NONE UINT32_MAX
#define PIECES 8            // pieces of the file per thread
#define SM ((1u << 30) - 1) // a query entry is slot | op << 30: 0 AND, 1 OR, 2 NOT

typedef struct {
  uint64_t w0;     // the term's first 8 bytes
  uint32_t lf, id; // lf: length | flags << 8 | slot << 11; flags: 1 BOOL
} Slot;            // term, 2 first and 4 second word of a phrase's anchor
typedef struct {
  uint32_t n, o, slot, next, t[8]; // o: where its anchor's second word is;
} Ph;                              // next: another phrase with that anchor
typedef struct {
  uint32_t q;             // input number
  uint8_t n, lo, tm, phr; // lo: the index of the last OR, or 0; tm: which
  uint32_t e[8];          // entries are triggers
} Q;
typedef struct __attribute__((packed)) {
  uint32_t c; // matches and their sum, per thread
  uint64_t s;
} Acc;
typedef struct {
  uint32_t c, m, ids[10];
  uint64_t s;
} Res;
typedef struct __attribute__((aligned(128))) { // no line shared with another
  uint64_t *pr, *tb, *log; // tb: the slots set in pr; log: q << 32 | doc
  uint32_t *ids, icap, base; // the line's term ids; the batch's first doc
  uint8_t *fl;               // and the ids' flags
  int eval;                  // 0 while sampling: tb then holds term ids
  size_t nlog;
  Acc *acc;
} Th;

static Slot *tab;
static uint64_t (*tw)[4], *pkey;
static uint32_t tmask, nterms, nph, pmask, nq, nw, nb, nslot, *ptop, *toff, *trig;
static uint32_t *freq, *tsl; // tsl: a BOOL term's slot; phrases follow them
static uint8_t *tlen, *tfl;
static Ph *ph;
static Q *qs;
static const char *map;
static long *pc; // the file's pieces: pc[j] to pc[j + 1], each at a line start
static uint32_t npc, nextpc;
static int nth;
static Th *th;

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// n bytes of zeros: with now, mapped at once (one call, not a fault a page);
// otherwise only the pages written are ever charged.
static void *zeros(size_t n, int now) {
  void *p = mmap(NULL, n + 1, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | (now ? MAP_POPULATE : MAP_NORESERVE),
                 -1, 0);
  if (p == MAP_FAILED)
    die("out of memory");
  return p;
}

// The file, mapped over zeros a page longer, so reads just past its end work.
static const char *mapfile(const char *path, long *size) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    die("cannot open input");
  long pg = sysconf(_SC_PAGESIZE), n = *size = st.st_size;
  char *b = mmap(NULL, (n + pg - 1) / pg * pg + pg, PROT_READ,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (b == MAP_FAILED ||
      (n && mmap(b, n, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED))
    die("cannot map input");
  madvise(b, n, MADV_WILLNEED);
  return b;
}

// The lowercased 8 bytes at p of a token with k bytes left (a token byte is a
// letter or a digit, so | 0x20 lowercases it).
static uint64_t word(const uint8_t *p, int k) {
  uint64_t v;
  memcpy(&v, p, 8);
  v |= 0x2020202020202020ull;
  return k >= 8 ? v : k > 0 ? v & ((1ull << 8 * k) - 1) : 0;
}

// The slot of the term w (zero padded) in tb, or the empty slot for it.
static Slot *find(Slot *tb, uint32_t mk, const uint64_t *w, uint32_t len) {
  for (uint32_t i = _mm_crc32_u64(len, w[0]) & mk;; i = (i + 1) & mk) {
    Slot *x = &tb[i];
    if (!x->lf || (x->w0 == w[0] && (x->lf & 255) == len &&
                   (len <= 8 || !memcmp(tw[x->id] + 1, w + 1, 24))))
      return x;
  }
}

static uint32_t intern(const char *s, uint32_t len) {
  uint64_t w[4] = {0};
  if (len > 32)
    die("query term longer than 32 bytes");
  memcpy(w, s, len);
  Slot *x = find(tab, tmask, w, len);
  if (!x->lf) {
    *x = (Slot){w[0], len, nterms};
    memcpy(tw[nterms], w, 32), tlen[nterms++] = len;
  }
  return x->id;
}

// The phrase table's cell for the anchor key: empty, or its first phrase.
static uint32_t *pcell(uint64_t key) {
  uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & pmask;
  while (ptop[i] != NONE && pkey[i] != key)
    i = (i + 1) & pmask;
  return &ptop[i];
}

// A 32 KB filter of the anchors, one bit per hash, that stays in L1.
static uint64_t pf[1 << 12];
static uint32_t pfh(uint64_t key) { return key * 0x9E3779B97F4A7C15ull >> 46; }
static int inpf(uint64_t key) { return pf[pfh(key) >> 6] >> (pfh(key) & 63) & 1; }

// The id (after the term ids) of the phrase t[0..n), n >= 2; identical phrases
// share one. It is filed under its anchor: the adjacent pair whose rarer word
// is rarest.
static uint32_t phrase(const uint32_t *t, uint32_t n) {
  uint32_t o = 1, best = NONE;
  for (uint32_t j = 1; j < n; j++) {
    uint32_t f = freq[t[j - 1]] < freq[t[j]] ? freq[t[j - 1]] : freq[t[j]];
    if (f < best)
      best = f, o = j;
  }
  uint64_t key = (uint64_t)t[o - 1] << 32 | t[o];
  uint32_t *c = pcell(key);
  pkey[c - ptop] = key, pf[pfh(key) >> 6] |= 1ull << (pfh(key) & 63);
  for (uint32_t j = *c; j != NONE; j = ph[j].next)
    if (ph[j].n == n && !memcmp(ph[j].t, t, 4 * n))
      return nterms + j;
  ph[nph] = (Ph){n, o, nb + nph, *c, {0}};
  memcpy(ph[nph].t, t, 4 * n);
  tfl[t[o - 1]] |= 2, tfl[t[o]] |= 4;
  *c = nph;
  return nterms + nph++;
}

// Bit i is set when p[i] is a token byte: A-Z, a-z or 0-9.
static uint32_t mask(const uint8_t *p) {
  __m256i v = _mm256_loadu_si256((const __m256i *)p);
  __m256i a = _mm256_sub_epi8(_mm256_or_si256(v, _mm256_set1_epi8(0x20)),
                              _mm256_set1_epi8('a'));
  __m256i d = _mm256_sub_epi8(v, _mm256_set1_epi8('0'));
  a = _mm256_cmpeq_epi8(_mm256_min_epu8(a, _mm256_set1_epi8(25)), a);
  d = _mm256_cmpeq_epi8(_mm256_min_epu8(d, _mm256_set1_epi8(9)), d);
  return _mm256_movemask_epi8(_mm256_or_si256(a, d));
}

// The term the token p[0..len) is, or NULL.
static const Slot *term(const uint8_t *p, uint32_t len) {
  if (len > 32)
    return NULL;
  uint64_t w[4] = {word(p, len)};
  for (int i = 1; len > 8 && i < 4; i++)
    w[i] = word(p + 8 * i, (int)len - 8 * i);
  const Slot *x = find(tab, tmask, w, len);
  return x->lf ? x : NULL;
}

// Slot s is in the batch's document k (when sampling, term s in some document).
static void mark(Th *t, uint32_t s, uint32_t k) {
  if (t->eval)
    t->pr[s] |= 1ull << k;
  t->tb[s >> 6] |= 1ull << (s & 63);
}

// Mark, as the batch's document b, the terms and then the phrases of the text
// [s, e). The tokens are found 64 bytes at a time (one starts where the mask
// rises and ends where it falls) and their table slots prefetched, so the
// lookups' cache misses overlap. Reads up to 64 bytes past e, which the
// spare page allows.
static void line(Th *t, const uint8_t *s, const uint8_t *e, uint32_t b) {
  uint32_t k = 0;
  uint64_t in = 0;
  const uint8_t *st = s;
  for (const uint8_t *p = s; p < e || in; p += 64) {
    uint64_t m = p < e ? mask(p) | (uint64_t)mask(p + 32) << 32 : 0;
    if (e - p < 64)
      m &= p < e ? (1ull << (e - p)) - 1 : 0;
    uint64_t ev = m ^ (m << 1 | in);
    in = m >> 63;
    for (; ev; ev &= ev - 1) {
      const uint8_t *q = p + __builtin_ctzll(ev);
      if (m >> (q - p) & 1) {
        st = q;
        continue;
      }
      if (k == t->icap) {
        t->icap = 2 * t->icap + 64;
        t->ids = realloc(t->ids, 4 * t->icap), t->fl = realloc(t->fl, t->icap);
      }
      uint32_t n = q - st; // offset and length (33: too long) for now
      t->ids[k] = st - s, t->fl[k++] = n > 32 ? 33 : n;
      __builtin_prefetch(&tab[_mm_crc32_u64(n, word(st, n)) & tmask]);
    }
  }
  for (uint32_t i = 0; i < k; i++) {
    const Slot *x = term(s + t->ids[i], t->fl[i]);
    t->ids[i] = x ? x->id : NONE, t->fl[i] = x ? x->lf >> 8 & 7 : 0;
    if (t->fl[i] & 1)
      mark(t, t->eval ? x->lf >> 11 : x->id, b);
  }
  for (uint32_t i = 1; i < k; i++) { // the phrases anchored at (i - 1, i)
    uint64_t key = (uint64_t)t->ids[i - 1] << 32 | t->ids[i];
    if (!(t->fl[i] & 4 && t->fl[i - 1] & 2 && inpf(key)))
      continue;
    for (uint32_t j = *pcell(key); j != NONE; j = ph[j].next)
      if (i >= ph[j].o && i - ph[j].o + ph[j].n <= k &&
          !memcmp(t->ids + i - ph[j].o, ph[j].t, 4 * ph[j].n))
        mark(t, ph[j].slot, b);
  }
}

// Query i over the batch, then its matches counted, summed and, while fewer
// than ten so far, logged. The ids of a batch run on by one from base.
static void run(Th *t, uint32_t i) {
  static const uint64_t dg[6] = {
      0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
      0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  const Q *q = &qs[i];
  uint64_t m = t->pr[q->e[0]];
  for (uint32_t k = 1; k < q->n && (m || k <= q->lo); k++) {
    uint64_t s = t->pr[q->e[k] & SM];
    uint32_t op = q->e[k] >> 30;
    m = op == 1 ? m | s : m & (op == 2 ? ~s : s);
  }
  if (!m)
    return;
  Acc *a = &t->acc[i];
  uint32_t c = a->c, pc = __builtin_popcountll(m);
  a->c = c + pc, a->s += (uint64_t)pc * t->base;
  for (int j = 0; j < 6; j++) // the positions' sum, binary digit by digit
    a->s += (uint64_t)__builtin_popcountll(m & dg[j]) << j;
  for (; m && c < 10; m &= m - 1, c++)
    t->log[t->nlog++] = (uint64_t)q->q << 32 | (t->base + __builtin_ctzll(m));
}

// The batch's slots in order, then pr and tb clean again; when evaluating,
// the queries each slot triggers run once, from the first of their triggers
// the batch holds; when sampling, each term counts once in freq.
static void each(Th *t) {
  for (int pass = 0; pass < 2; pass++)
    for (uint32_t w = 0; w < nw; w++)
      for (uint64_t x = t->tb[w]; x; x &= x - 1) {
        uint32_t s = w * 64 + __builtin_ctzll(x);
        if (!t->eval)
          pass ? 0 : __atomic_fetch_add(&freq[s], 1, __ATOMIC_RELAXED);
        else if (pass)
          t->pr[s] = 0;
        else
          for (uint32_t j = toff[s]; j < toff[s + 1]; j++) {
            const Q *q = &qs[trig[j]];
            uint32_t m = q->tm, y;
            while ((y = q->e[__builtin_ctz(m)] & SM) != s && !t->pr[y])
              m &= m - 1;
            if (y == s)
              run(t, trig[j]);
          }
      }
  memset(t->tb, 0, nw * 8ull);
}

// The lines from p (a line start) to lim in batches, for at most nb of them.
static void batches(Th *t, const char *p, const char *lim, long nb, int eval) {
  uint32_t k = 0;
  t->eval = eval;
  while (p < lim && nb) {
    uint32_t d = 0;
    for (; *p != '\t'; p++)
      d = d * 10 + (*p - '0');
    const char *s = p + 1, *e = memchr(s, '\n', lim - s);
    e = e ? e : lim;
    t->base = k ? t->base : d;
    line(t, (const uint8_t *)s, (const uint8_t *)e, k);
    if (++k == 64)
      each(t), k = 0, nb--;
    p = e + 1;
  }
  if (k)
    each(t);
}

static const char *after_nl(long b, long lim) {
  const char *nl = b > 0 && map[b - 1] != '\n' ? memchr(map + b, '\n', lim - b)
                                               : map + b - 1;
  return nl ? nl + 1 : map + lim;
}

// This thread's arrays (mapped here, so in parallel), then one batch from the
// start of every other piece.
static void *sample(void *arg) {
  Th *t = arg;
  t->pr = zeros(nslot * 8ull, 1), t->tb = zeros(nw * 8ull, 1);
  t->log = zeros(nq * 80ull, 0); // at most ten per query
  t->acc = zeros((nq + 1) * sizeof(Acc), 1);
  for (uint32_t j = 2 * (t - th); j < npc; j += 2 * nth)
    batches(t, map + pc[j], map + pc[j + 1], 1, 0);
  return NULL;
}

// The pieces in order, each to the first thread free: a thread's pieces still
// ascend, so its first ten matches of a query are its ten smallest.
static void *scan(void *arg) {
  Th *t = arg;
  for (uint32_t j; (j = __atomic_fetch_add(&nextpc, 1, __ATOMIC_RELAXED)) < npc;)
    batches(t, map + pc[j], map + pc[j + 1], -1, 1);
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

// Slots such that every document query q matches holds one, the cheapest by
// freq: x AND s lies within x and within s, x OR s within their union, and x
// NOT s within x. Each slot is a trigger once.
static void triggers(Q *q) {
  uint32_t cost = freq[q->e[0]], j;
  q->tm = 1;
  for (uint32_t k = 1; k < q->n; k++) {
    uint32_t s = q->e[k] & SM, op = q->e[k] >> 30;
    for (j = 0; j < k && !(q->tm >> j & 1 && (q->e[j] & SM) == s); j++)
      ;
    if (op == 0 && freq[s] < cost)
      q->tm = 1 << k, cost = freq[s];
    else if (op == 1 && j == k)
      q->tm |= 1 << k, cost += freq[s];
  }
}

static uint32_t first(const Q *q) { return q->e[__builtin_ctz(q->tm)] & SM; }

static int by_freq(const void *a, const void *b) {
  uint32_t x = freq[*(const uint32_t *)a], y = freq[*(const uint32_t *)b];
  return (x < y) - (x > y);
}

static char *num(char *o, uint64_t v) {
  char tmp[20];
  int i = 0;
  do
    tmp[i++] = '0' + v % 10;
  while (v /= 10);
  while (i)
    *o++ = tmp[--i];
  return o;
}

int main(void) {
  mallopt(M_MMAP_THRESHOLD, 1 << 17); // big blocks go back when freed
  // Queries: BOOL t1 op t2 ... or PHRASE "t1 t2 ...".
  long qn, size, words = 1, nphr = 1;
  const char *qb = mapfile("/data/queries.txt", &qn), *p = qb;
  for (long i = 0; i < qn; i++)
    nq += qb[i] == '\n', words += qb[i] == ' ' || qb[i] == '\n';
  for (tmask = 1; tmask <= words; tmask <<= 1)
    ;
  tab = zeros(tmask-- * sizeof(Slot), 1), qs = zeros((nq + 1) * sizeof(Q), 1);
  tw = zeros(words * 32, 0), tlen = zeros(words, 0); // words >= terms
  for (uint32_t i = 0; i < nq; i++) {
    Q *q = &qs[i];
    const char *e = memchr(p, '\n', qb + qn - p), *stop;
    q->q = i, q->phr = *p == 'P', nphr += q->phr;
    p += q->phr ? 8 : 5; // past `PHRASE "` or `BOOL `
    stop = q->phr ? e - 1 : e;
    for (uint32_t k = 0, op = 0; p < stop; k++, p++) {
      const char *w = p;
      while (p < stop && *p != ' ')
        p++;
      if (!q->phr && k % 2) {
        op = *w == 'O' ? 1 : *w == 'N' ? 2 : 0;
        continue;
      }
      if (q->n == 8)
        die("too many terms in a query");
      q->lo = op == 1 ? q->n : q->lo;
      q->e[q->n] = intern(w, p - w) | (q->n ? op << 30 : 0);
      q->n++;
    }
    p = e + 1;
  }

  // The term table again, as small as keeps its load under 2/3; until the
  // sample is done, every term is marked.
  tfl = calloc(nterms + 1, 1), tsl = calloc(nterms + 1, 4);
  for (uint32_t i = 0; i < nq; i++)
    for (uint32_t k = 0; k < qs[i].n && !(qs[i].phr && qs[i].n > 1); k++) {
      uint32_t id = qs[i].e[k] & SM;
      if (!(tfl[id] & 1))
        tfl[id] |= 1, tsl[id] = nb++;
    }
  munmap(tab, (tmask + 1) * sizeof(Slot));
  for (tmask = 1; tmask < nterms + nterms / 2 + 1; tmask <<= 1)
    ;
  tab = zeros(tmask-- * sizeof(Slot), 1);
  for (uint32_t i = 0; i < nterms; i++)
    *find(tab, tmask, tw[i], tlen[i]) = (Slot){tw[i][0], tlen[i] | 1 << 8, i};
  nslot = nb + nphr; // tb also holds term ids while sampling
  nw = (nterms + nphr + 63) / 64;

  map = mapfile("/data/docs.txt", &size);
  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth < 1 ? 1 : nth > 64 ? 64 : nth;
  npc = PIECES * nth, pc = calloc(npc + 1, 8);
  for (uint32_t j = 1; j <= npc; j++) {
    pc[j] = j < npc ? after_nl(size * j / npc, size) - map : size;
    pc[j] = pc[j] < pc[j - 1] ? pc[j - 1] : pc[j];
  }
  th = zeros(nth * sizeof(Th), 1), freq = calloc(nterms + nphr, 4);
  run_threads(sample);

  // Phrases of two or more words, filed under their anchors; then each term's
  // real flags.
  for (pmask = 1; pmask < 2 * nphr; pmask <<= 1)
    ;
  ptop = malloc(pmask * 4ull), pkey = malloc(pmask * 8ull);
  memset(ptop, 0xff, pmask-- * 4ull);
  ph = malloc(nphr * sizeof(Ph));
  for (uint32_t i = 0; i < nq; i++)
    if (qs[i].phr && qs[i].n > 1)
      qs[i].e[0] = phrase(qs[i].e, qs[i].n), qs[i].n = 1;
  uint32_t *by = malloc(nb * 4ull + 4); // BOOL terms, most frequent first, so
  for (uint32_t id = 0; id < nterms; id++) // the hot slots share cache lines
    if (tfl[id] & 1)
      by[tsl[id]] = id;
  qsort(by, nb, 4, by_freq);
  for (uint32_t r = 0; r < nb; r++)
    tsl[by[r]] = r;
  for (uint32_t i = 0; i <= tmask; i++)
    if (tab[i].lf)
      tab[i].lf = (tab[i].lf & 255) | tfl[tab[i].id] << 8 | tsl[tab[i].id] << 11;

  // Each query's triggers, its entries turned into slots, and the queries in
  // order of first trigger, so a slot's queries sit together; then each
  // query under each of its triggers.
  for (uint32_t i = 0; i < nq; i++) {
    Q *q = &qs[i];
    triggers(q);
    for (uint32_t k = 0; k < q->n; k++) // phrase ids follow the term ids
      q->e[k] = (q->e[k] & SM) >= nterms ? nb + q->e[k] - nterms
                                         : tsl[q->e[k] & SM] | (q->e[k] & ~SM);
  }
  uint32_t *at = calloc(nslot + 1, 4);
  Q *sorted = zeros((nq + 1) * sizeof(Q), 1);
  for (uint32_t i = 0; i < nq; i++)
    at[first(&qs[i]) + 1]++;
  for (uint32_t s = 1; s < nslot; s++)
    at[s] += at[s - 1];
  for (uint32_t i = 0; i < nq; i++)
    sorted[at[first(&qs[i])]++] = qs[i];
  munmap(qs, (nq + 1) * sizeof(Q)), free(at), qs = sorted;
  toff = calloc(nslot + 2, 4);
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < nq; i++)
      for (uint32_t m = qs[i].tm; m; m &= m - 1) {
        uint32_t s = qs[i].e[__builtin_ctz(m)] & SM;
        if (pass)
          trig[toff[s + 1]++] = i;
        else
          toff[s + 2]++;
      }
    for (uint32_t s = 2; !pass && s < nslot + 2; s++)
      toff[s] += toff[s - 1];
    trig = pass ? trig : malloc((toff[nslot + 1] + 1) * 4ull);
  }
  free(freq);
  run_threads(scan);

  // Counts and sums add up; the ten smallest ids are among the threads' logs.
  for (int i = 0; i < nth; i++)
    munmap(th[i].pr, nslot * 8ull), free(th[i].ids), free(th[i].fl);
  Res *res = calloc(nq + 1, sizeof(Res));
  for (int i = 0; i < nth; i++) {
    for (uint32_t j = 0; j < nq; j++)
      res[qs[j].q].c += th[i].acc[j].c, res[qs[j].q].s += th[i].acc[j].s;
    for (size_t j = 0; j < th[i].nlog; j++) {
      Res *r = &res[th[i].log[j] >> 32];
      uint32_t d = th[i].log[j], k = r->m < 10 ? r->m++ : 10;
      if (k == 10 && d > r->ids[9])
        continue;
      for (k -= k == 10; k && r->ids[k - 1] > d; k--)
        r->ids[k] = r->ids[k - 1];
      r->ids[k] = d;
    }
  }
  int out = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    die("cannot create /out/results.txt");
  char *ob = malloc(1 << 16), *o = ob;
  for (uint32_t i = 0; i < nq; i++) {
    o = num(o, res[i].c), *o++ = ' ', o = num(o, res[i].s);
    for (uint32_t k = 0; k < res[i].m; k++)
      *o++ = ' ', o = num(o, res[i].ids[k]);
    *o++ = '\n';
    if (o - ob > (1 << 16) - 256 || i + 1 == nq) {
      if (write(out, ob, o - ob) != o - ob)
        die("write failed");
      o = ob;
    }
  }
  if (close(out))
    die("write failed");
  _exit(0);
}
