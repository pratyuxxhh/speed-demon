// Speed Demon 2026, Track B (Index): BOOL (strict left-to-right AND/OR/NOT) and
// PHRASE queries over a one-document-per-line corpus. See specs/b-index.md.
//
// The queries are read first and their terms interned; only those terms are
// looked up. The phrases' term sequences go into one Aho-Corasick automaton,
// and a phrase match counts like one more term. No index is built: docs.txt is
// mapped, each thread scans a line-aligned range in batches of 64 documents,
// and records in present[slot] one bit per document of the batch for every
// term or phrase it holds. After each batch the queries are evaluated over
// those words left to right (& | &~), and their matches counted at once; a
// query is skipped when none of its trigger slots (chosen from a sample of
// the corpus) shows up in the batch, since it then cannot match.
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

#define CH (1 << 12) // a line is tokenized in chunks of at most this many bytes (cut at a separator)
#define OUTSZ (1 << 16)
#define MAXT 8 // terms per query
#define NONE UINT32_MAX
#define OPS 30 // a program entry is slot | op << OPS; op 0 AND (or first), 1 OR, 2 NOT
#define SAMPLE 48 // batches sampled to estimate how often each slot shows up

typedef struct {
  uint64_t w[4]; // the term's bytes, zero padded; terms are at most 32 bytes
  uint32_t len;
} Term;
typedef struct {
  uint64_t w0; // first 8 bytes, so most compares never touch the Term
  uint32_t len, id1; // id1 = term id + 1, 0 when empty
} Slot;
typedef struct {
  uint8_t phrase, n;
  char op[MAXT];
  uint32_t t[MAXT]; // terms; a phrase's id (after the terms) is t[0]
} Query;
typedef struct {
  uint32_t count, m, ids[10];
  uint64_t sum;
} Result;
typedef struct __attribute__((packed)) {
  uint32_t cnt; // a query's matches so far, and their sum
  uint64_t sum;
} Qs;
typedef struct {
  long beg, end;
  uint64_t *present, *tw, *log; // tw: 4 words per token of a chunk
  Qs *qr;                       // per query, in program order
  uint32_t *touched, *tokids, nt, base, doc[64]; // base: doc[0] when the ids run on by one, else NONE
  size_t nlog, logcap; // log: (query << 32 | doc) for each query's first 10 matches here
} Th;

static Term *terms;
static Slot *slots;
static uint32_t nterms, tcap, smask, nids, nslots, *slot, *prog, *qoff;
static uint32_t *toff, *trig; // the queries slot s triggers: trig[toff[s]..toff[s + 1])
static uint32_t *freq;        // per slot: in how many sampled batches it showed up
static Query *qs;
static Result *res;
static uint32_t nq;
// The phrase automaton: node 0 is the root; edges (node, term) live in a hash.
// outp is a node's phrase: its id while building, then its slot.
static uint32_t nnode, *fail, *outp, *olink, *par, *lab, *gval, gmask;
static uint64_t *gkey;
static uint8_t *inph; // per term: 1 in some phrase, 2 first in some phrase
// Fast paths: the root's children by term, and each node's child when it has one.
static uint32_t *root_next, *kid, *kid_lab;
#define MULTI (NONE - 1) // kid: several children, look them up in the hash
static int nth;
static const char *map;
static Th *th;

static void die(const char *m) {
  fprintf(stderr, "%s\n", m);
  exit(1);
}

// CRC32C over the term's 8-byte words, seeded with its length.
static uint32_t hash_words(const uint64_t *w, uint32_t len) {
  uint64_t h = _mm_crc32_u64(len, w[0]);
  for (uint32_t i = 1; i < (len + 7) / 8; i++)
    h = _mm_crc32_u64(h, w[i]);
  return (uint32_t)h;
}

// The id of the zero-padded token w, or NONE when it is not a query term.
static uint32_t lookup(const uint64_t *w, uint32_t len) {
  if (len > 32)
    return NONE;
  for (uint32_t i = hash_words(w, len) & smask;; i = (i + 1) & smask) {
    const Slot *s = &slots[i];
    if (!s->id1)
      return NONE;
    if (s->w0 == w[0] && s->len == len) {
      const uint64_t *tw = terms[s->id1 - 1].w;
      if (len <= 8 || (tw[1] == w[1] && tw[2] == w[2] && tw[3] == w[3]))
        return s->id1 - 1;
    }
  }
}

static void place(uint32_t id) {
  Term *t = &terms[id];
  uint32_t i = hash_words(t->w, t->len) & smask;
  while (slots[i].id1)
    i = (i + 1) & smask;
  slots[i] = (Slot){t->w[0], t->len, id + 1};
}

// The final table, once every term is in: the smallest power of two that keeps
// the load at most 2/3, and the terms array cut to its size.
static void rehash(void) {
  uint32_t sz = 1;
  while (sz < nterms + nterms / 2 + 1)
    sz <<= 1;
  free(slots);
  smask = sz - 1;
  slots = calloc(sz, sizeof(Slot));
  terms = realloc(terms, (nterms + 1) * sizeof(Term));
  for (uint32_t i = 0; i < nterms; i++)
    place(i);
}

static uint32_t intern(const char *s, uint32_t len) {
  if (len > 32)
    die("query term longer than 32 bytes");
  if (nterms == tcap) { // grow first, so the table exists before the first lookup
    tcap = tcap ? tcap * 2 : 1024;
    terms = realloc(terms, tcap * sizeof(Term));
    free(slots);
    smask = tcap * 2 - 1; // load at most 1/2
    slots = calloc(smask + 1, sizeof(Slot));
    for (uint32_t i = 0; i < nterms; i++)
      place(i);
  }
  Term t = {{0}, len};
  memcpy(t.w, s, len);
  uint32_t id = lookup(t.w, len);
  if (id != NONE)
    return id;
  terms[nterms] = t;
  place(nterms);
  return nterms++;
}

// The automaton's edge from node s on term t, or NONE; with add, created if missing.
static uint32_t edge(uint32_t s, uint32_t t, int add) {
  uint64_t key = (uint64_t)s << 32 | t;
  for (uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & gmask;; i = (i + 1) & gmask) {
    if (gval[i] == NONE) {
      if (!add)
        return NONE;
      gkey[i] = key, gval[i] = nnode;
      par[nnode] = s, lab[nnode] = t, outp[nnode] = NONE;
      return nnode++;
    }
    if (gkey[i] == key)
      return gval[i];
  }
}

// The automaton's edge from s on term t, or NONE, by the fast paths when possible.
static uint32_t step(uint32_t s, uint32_t t) {
  if (!s)
    return root_next[t];
  if (kid[s] == MULTI)
    return edge(s, t, 0);
  return kid_lab[s] == t ? kid[s] : NONE;
}

static int is_tok(uint8_t c) { return (uint8_t)((c | 0x20) - 'a') < 26 || (uint8_t)(c - '0') < 10; }

// Bit i is set when p[i] is a token byte: A-Z, a-z or 0-9 (c | 0x20 folds A-Z
// onto a-z and leaves a-z and 0-9 alone).
static uint32_t token_mask(const uint8_t *p) {
  __m256i v = _mm256_loadu_si256((const __m256i *)p);
  __m256i a = _mm256_sub_epi8(_mm256_or_si256(v, _mm256_set1_epi8(0x20)), _mm256_set1_epi8('a'));
  __m256i d = _mm256_sub_epi8(v, _mm256_set1_epi8('0'));
  __m256i al = _mm256_cmpeq_epi8(_mm256_min_epu8(a, _mm256_set1_epi8(25)), a);
  __m256i dg = _mm256_cmpeq_epi8(_mm256_min_epu8(d, _mm256_set1_epi8(9)), d);
  return _mm256_movemask_epi8(_mm256_or_si256(al, dg));
}

// The lowercased, zero-padded words of the token p[0..n) (every byte of a token
// is a letter or a digit, so | 0x20 lowercases it).
static void words_of(const uint8_t *p, uint32_t n, uint64_t *w) {
  memcpy(w, p, 32);
  for (int i = 0; i < 4; i++) {
    int k = (int)n - 8 * i;
    w[i] = k >= 8 ? w[i] | 0x2020202020202020ull
           : k > 0 ? (w[i] | 0x2020202020202020ull) & ((1ull << (8 * k)) - 1)
                   : 0;
  }
}

// The term ids (NONE for other tokens) of the text [s, e), 64 bytes at a time:
// a token starts where the mask rises and ends where it falls. The tokens are
// collected first and their table slots prefetched, so the lookups' cache misses
// overlap. Reads up to 64 bytes past e, so the mapping has a page of slack.
// tw needs 4 words per token.
static uint32_t tokenize(const uint8_t *s, const uint8_t *e, uint32_t *ids, uint64_t *tw) {
  uint32_t n = 0;
  uint64_t in = 0; // 1 when a token runs into the next block
  const uint8_t *start = s;
  for (const uint8_t *p = s; p < e || in; p += 64) {
    uint64_t m = p < e ? token_mask(p) | (uint64_t)token_mask(p + 32) << 32 : 0;
    if (e - p < 64)
      m &= p < e ? (1ull << (e - p)) - 1 : 0;
    uint64_t prev = m << 1 | in;
    in = m >> 63;
    for (uint64_t ev = m ^ prev; ev; ev &= ev - 1) {
      const uint8_t *q = p + __builtin_ctzll(ev);
      if (m >> (q - p) & 1) {
        start = q;
      } else {
        uint32_t len = (q < e ? q : e) - start;
        words_of(start, len, tw + 4 * n);
        ids[n] = len;
        __builtin_prefetch(&slots[hash_words(tw + 4 * n, len) & smask]);
        n++;
      }
    }
  }
  for (uint32_t i = 0; i < n; i++)
    ids[i] = lookup(tw + 4 * i, ids[i]);
  return n;
}

// Set bit b of present[sl], remembering the slot so the batch can clear it.
static void mark(Th *t, uint32_t sl, uint64_t b) {
  t->touched[t->nt] = sl;
  t->nt += !t->present[sl];
  t->present[sl] |= b;
}

// Mark, with bit b, each term and each phrase the automaton completes in [s, e).
static void index_line(Th *t, const uint8_t *s, const uint8_t *e, uint64_t b) {
  uint32_t st = 0;
  while (s < e) {
    const uint8_t *c = e; // this chunk is [s, c), and c is a separator or e
    if (e - s > CH) {
      for (c = s + CH; c > s && is_tok(*c); c--)
        ;
      if (c == s) // (s, s + CH] is one token: run on to its end
        for (c = s + CH; c < e && is_tok(*c); c++)
          ;
    }
    for (uint32_t i = 0, n = tokenize(s, c, t->tokids, t->tw); i < n; i++) {
      uint32_t id = t->tokids[i], g = NONE;
      if (id == NONE) {
        st = 0; // a token in no phrase breaks every phrase
        continue;
      }
      if (slot[id] != NONE)
        mark(t, slot[id], b);
      if (!(inph[id] & (st ? 1 : 2))) {
        st = 0; // from the root, only a first term moves
        continue;
      }
      while ((g = step(st, id)) == NONE && st)
        st = fail[st];
      st = g == NONE ? 0 : g;
      for (uint32_t o = outp[st] != NONE ? st : olink[st]; o != NONE; o = olink[o])
        mark(t, outp[o], b);
    }
    s = c;
  }
}

static void log_match(Th *t, uint32_t q, uint32_t d) {
  if (t->nlog == t->logcap) {
    t->logcap = t->logcap ? t->logcap * 2 : 4096;
    if (!(t->log = realloc(t->log, t->logcap * 8)))
      die("out of memory");
  }
  t->log[t->nlog++] = (uint64_t)q << 32 | d;
}

// The sum of the positions of m's set bits: bit b of a position is set in the
// positions picked out by the b-th mask.
static uint64_t bit_positions(uint64_t m) {
  static const uint64_t mk[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                 0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  uint64_t s = 0;
  for (int b = 0; b < 6; b++)
    s += (uint64_t)__builtin_popcountll(m & mk[b]) << b;
  return s;
}

// Query q (in program order) over the batch: its entries left to right on the
// present words, then its matching documents counted, summed and, among its
// first ten in this thread, logged under the query's input number.
static void run_query(Th *t, uint32_t q) {
  const uint32_t *p = prog + qoff[q];
  uint32_t n = p[-1] & 0xff, lo = p[-1] >> 8 & 0xff; // lo: the index of the last OR, or 0
  uint64_t m = t->present[p[0]];
  for (uint32_t i = 1; i < n && (m || i <= lo); i++) { // empty stays empty without an OR
    uint32_t e = p[i], op = e >> OPS;
    uint64_t s = t->present[e & ((1u << OPS) - 1)];
    m = op == 1 ? m | s : m & (op == 2 ? ~s : s);
  }
  if (!m)
    return;
  Qs *r = &t->qr[q];
  uint32_t c = r->cnt, pc = __builtin_popcountll(m);
  r->cnt = c + pc;
  if (t->base != NONE) { // the batch's ids are base, base + 1, ...
    r->sum += (uint64_t)pc * t->base + bit_positions(m);
    for (; m && c < 10; m &= m - 1, c++)
      log_match(t, p[-2], t->base + __builtin_ctzll(m));
    return;
  }
  for (; m; m &= m - 1) {
    uint32_t d = t->doc[__builtin_ctzll(m)];
    r->sum += d;
    if (c++ < 10)
      log_match(t, p[-2], d);
  }
}

// Slots of query q such that every document it matches holds one of them, the
// cheapest by freq: left to right, x AND s lies within x and within s, x OR s
// within the union, and x NOT s within x. Returns their number.
static uint32_t triggers(uint32_t q, uint32_t *ts) {
  const uint32_t *e = prog + qoff[q];
  uint32_t n = 1, cost = freq[e[0]];
  ts[0] = e[0];
  for (uint32_t k = 1, nk = e[-1] & 0xff; k < nk; k++) {
    uint32_t s = e[k] & ((1u << OPS) - 1), op = e[k] >> OPS, j = 0;
    if (op == 0 && freq[s] < cost)
      ts[0] = s, n = 1, cost = freq[s];
    else if (op == 1) {
      while (j < n && ts[j] != s)
        j++;
      if (j == n) // each slot once
        ts[n++] = s, cost += freq[s];
    }
  }
  return n;
}

// Every query over the batch, then a clean present for the next. Only a query
// one of whose triggers some document of the batch holds can match. Each runs
// once, from the first of its triggers that the batch holds.
static void evaluate(Th *t, uint32_t k) {
  t->base = t->doc[0];
  for (uint32_t i = 1; i < k; i++)
    if (t->doc[i] != t->doc[0] + i)
      t->base = NONE;
  for (uint32_t i = 0; i < t->nt; i++) {
    uint32_t s = t->touched[i];
    for (uint32_t j = toff[s], je = toff[s + 1]; j < je; j++) {
      const uint32_t *p = prog + qoff[trig[j]], *tr = p + (p[-1] & 0xff);
      if (p[-1] >> 16 > 1) {
        while (*tr != s && !t->present[*tr])
          tr++;
        if (*tr != s)
          continue;
      }
      run_query(t, trig[j]);
    }
  }
  for (uint32_t i = 0; i < t->nt; i++)
    t->present[t->touched[i]] = 0;
  t->nt = 0;
}

// A sampled batch: each slot it holds counts once in freq.
static void tally(Th *t, uint32_t k) {
  (void)k;
  for (uint32_t i = 0; i < t->nt; i++) {
    __atomic_fetch_add(&freq[t->touched[i]], 1, __ATOMIC_RELAXED);
    t->present[t->touched[i]] = 0;
  }
  t->nt = 0;
}

// The lines from p (a line start) to lim, in batches of 64, each handed to
// done; stops after nb batches.
static void batches(Th *t, const char *p, const char *lim, long nb, void (*done)(Th *, uint32_t)) {
  uint32_t k = 0;
  while (p < lim && nb) {
    uint32_t doc = 0;
    for (; *p != '\t'; p++)
      doc = doc * 10 + (*p - '0');
    const char *s = ++p, *e = memchr(p, '\n', lim - p);
    if (!e)
      e = lim; // a last line without a newline
    index_line(t, (const uint8_t *)s, (const uint8_t *)e, 1ull << k);
    t->doc[k++] = doc;
    if (k == 64) {
      done(t, k);
      k = 0, nb--;
    }
    p = e + 1;
  }
  if (k)
    done(t, k);
}

// A few batches spread over this thread's range, to estimate how often each
// slot shows up in a batch.
static void *sample(void *arg) {
  Th *t = arg;
  for (int j = 0, ns = (SAMPLE + nth - 1) / nth; j < ns; j++) {
    long b = t->beg + (t->end - t->beg) * j / ns;
    if (b > t->beg && map[b - 1] != '\n') {
      const char *nl = memchr(map + b, '\n', t->end - b);
      b = nl ? nl + 1 - map : t->end;
    }
    batches(t, map + b, map + t->end, 1, tally);
  }
  return NULL;
}

static void *scan(void *arg) {
  Th *t = arg;
  batches(t, map + t->beg, map + t->end, -1, evaluate);
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

static char *slurp(const char *path, long *size) {
  int f = open(path, O_RDONLY);
  struct stat st;
  if (f < 0 || fstat(f, &st))
    die("cannot open input");
  char *b = malloc(st.st_size + 1);
  for (long got = 0; got < st.st_size;) {
    ssize_t n = pread(f, b + got, st.st_size - got, got);
    if (n <= 0)
      die("read failed");
    got += n;
  }
  close(f);
  *size = st.st_size;
  return b;
}

int main(void) {
  // Queries: BOOL t1 op t2 ... or PHRASE "t1 t2 ...".
  long qsize;
  char *qb = slurp("/data/queries.txt", &qsize);
  for (long i = 0; i < qsize; i++)
    nq += qb[i] == '\n';
  qs = calloc(nq + 1, sizeof(Query));
  char *p = qb;
  uint32_t edges = 1;
  for (uint32_t i = 0; i < nq; i++) {
    Query *q = &qs[i];
    char *e = memchr(p, '\n', qb + qsize - p);
    q->phrase = *p == 'P';
    p += q->phrase ? 8 : 5; // past `PHRASE "` or `BOOL `
    char *stop = q->phrase ? e - 1 : e;
    for (int k = 0; p < stop; k++) {
      char *w = p;
      while (p < stop && *p != ' ')
        p++;
      if (q->phrase || k % 2 == 0) {
        if (q->n == MAXT)
          die("too many terms in a query");
        q->t[q->n++] = intern(w, p - w);
      } else {
        q->op[q->n] = *w; // the operator applied with the next term
      }
      p++;
    }
    edges += q->phrase ? q->n : 0;
    p = e + 1;
  }
  free(qb);
  rehash();

  // The phrase automaton: a trie of the phrases' term sequences (identical
  // phrases share an end node), then failure links in order of depth.
  uint32_t gsz = 1;
  while (gsz < edges + edges / 2)
    gsz <<= 1;
  gmask = gsz - 1;
  gkey = malloc(gsz * 8ull), gval = malloc(gsz * 4ull);
  memset(gval, 0xff, gsz * 4ull);
  par = malloc(edges * 4ull), lab = malloc(edges * 4ull), outp = malloc(edges * 4ull);
  fail = calloc(edges, 4), olink = malloc(edges * 4ull);
  nnode = 1, outp[0] = olink[0] = NONE;
  inph = calloc(nterms + 1, 1);
  for (uint32_t i = 0; i < nq; i++) {
    Query *q = &qs[i];
    uint32_t s = 0;
    for (uint32_t k = 0; q->phrase && k < q->n; k++) {
      s = edge(s, q->t[k], 1);
      inph[q->t[k]] |= k ? 1 : 3;
    }
    if (q->phrase) {
      if (outp[s] == NONE)
        outp[s] = nids++;
      q->t[0] = nterms + outp[s]; // from here on, the phrase's id
    }
  }
  uint32_t *bydepth = malloc(nnode * 4ull), *dep = calloc(nnode, 4), nb = 0;
  for (uint32_t d = 1; d <= MAXT; d++) // nodes are created after their parents
    for (uint32_t v = 1; v < nnode; v++)
      if ((dep[v] = dep[par[v]] + 1) == d)
        bydepth[nb++] = v;
  for (uint32_t k = 0; k < nb; k++) {
    uint32_t v = bydepth[k], f = fail[par[v]], g = NONE;
    if (par[v])
      while ((g = edge(f, lab[v], 0)) == NONE && f)
        f = fail[f];
    fail[v] = par[v] && g != NONE ? g : 0;
    olink[v] = outp[fail[v]] != NONE ? fail[v] : olink[fail[v]];
  }
  free(bydepth), free(dep);
  root_next = malloc((nterms + 1) * 4ull), kid = malloc(nnode * 4ull), kid_lab = calloc(nnode, 4);
  memset(root_next, 0xff, (nterms + 1) * 4ull);
  memset(kid, 0xff, nnode * 4ull);
  for (uint32_t v = 1; v < nnode; v++) {
    uint32_t u = par[v];
    if (!u)
      root_next[lab[v]] = v;
    else if (kid[u] == NONE)
      kid[u] = v, kid_lab[u] = lab[v];
    else
      kid[u] = MULTI;
  }
  nids += nterms; // terms, then phrases

  // A present slot for each id a query reads (a term only in phrases needs
  // none), and the queries as one program: per query its input number, then
  // n | lo << 8, then its n entries slot | op << OPS; qoff[q] is the first entry.
  slot = malloc(nids * 4ull);
  memset(slot, 0xff, nids * 4ull);
  uint32_t plen = 0;
  for (uint32_t i = 0; i < nq; i++)
    plen += 2 + (qs[i].phrase ? 1 : qs[i].n);
  prog = malloc(plen * 4ull), qoff = malloc(nq * 4ull);
  uint32_t *w = prog;
  for (uint32_t i = 0; i < nq; i++) {
    Query *q = &qs[i];
    uint32_t n = q->phrase ? 1 : q->n, lo = 0;
    *w++ = i, *w++ = 0;
    qoff[i] = w - prog;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t id = q->t[k], op = k ? (q->op[k] == 'O' ? 1 : q->op[k] == 'N' ? 2 : 0) : 0;
      if (slot[id] == NONE)
        slot[id] = nslots++;
      lo = op == 1 ? k : lo;
      *w++ = slot[id] | op << OPS;
    }
    w[-1 - (int)n] = n | lo << 8;
  }
  for (uint32_t v = 0; v < nnode; v++)
    outp[v] = outp[v] == NONE ? NONE : slot[nterms + outp[v]];
  free(qs);

  int fd = open("/data/docs.txt", O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    die("cannot open /data/docs.txt");
  long size = st.st_size, pg = sysconf(_SC_PAGESIZE), len = (size + pg - 1) / pg * pg + pg;
  // The file over an anonymous zero mapping one page longer, so tokenize's
  // reads past the last byte stay mapped.
  char *base = mmap(NULL, len, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED || (size && mmap(base, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED))
    die("cannot map /data/docs.txt");
  if (size)
    madvise(base, size, MADV_WILLNEED);
  map = base;

  cpu_set_t cpus; // the CPUs this container may use, not the host's count
  nth = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  nth = nth < 1 ? 1 : nth > 64 ? 64 : nth;
  th = calloc(nth, sizeof(Th));
  for (int i = 1; i < nth; i++) { // each range starts after a newline
    long b = size * i / nth;
    if (b > 0 && map[b - 1] != '\n') {
      const char *nl = memchr(map + b, '\n', size - b);
      b = nl ? nl + 1 - map : size;
    }
    th[i].beg = b < th[i - 1].beg ? th[i - 1].beg : b;
    th[i - 1].end = th[i].beg;
  }
  th[nth - 1].end = size;
  for (int i = 0; i < nth; i++) {
    th[i].present = calloc(nslots + 1, 8);
    th[i].touched = malloc((nslots + 1) * 4ull);
    th[i].tokids = malloc((CH / 2 + 2) * 4ull); // a chunk has at most CH / 2 + 1 tokens
    th[i].tw = malloc((CH / 2 + 2) * 32ull);
    th[i].qr = calloc(nq + 1, sizeof(Qs));
  }
  freq = calloc(nslots + 1, 4);
  run_threads(sample);

  // The program reordered by each query's first trigger, so the queries one
  // slot triggers sit together, in the program and in each thread's qr. Each
  // query's header gains nt << 16, and its nt triggers follow its entries.
  uint32_t *at = calloc(nslots + 1, 4), *qoff2 = malloc(nq * 4ull), *ord = malloc(nq * 4ull);
  for (uint32_t i = 0; i < nq; i++) {
    uint32_t ts[MAXT];
    plen += triggers(i, ts);
    qoff2[i] = ts[0];
    at[ts[0] + 1]++;
  }
  for (uint32_t s = 1; s < nslots; s++)
    at[s] += at[s - 1];
  for (uint32_t i = 0; i < nq; i++)
    ord[at[qoff2[i]]++] = i;
  uint32_t *prog2 = malloc(plen * 4ull);
  w = prog2;
  for (uint32_t j = 0; j < nq; j++) {
    const uint32_t *e = prog + qoff[ord[j]] - 2;
    uint32_t n = 2 + (e[1] & 0xff), nt = triggers(ord[j], w + n);
    memcpy(w, e, n * 4ull);
    w[1] |= nt << 16;
    qoff2[j] = w + 2 - prog2;
    w += n + nt;
  }
  free(prog), free(qoff), free(at), free(ord);
  prog = prog2, qoff = qoff2;

  // Each query under the slots of its cheapest trigger set, by counting then placing.
  toff = calloc(nslots + 2, 4);
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < nq; i++) {
      uint32_t ts[MAXT], n = triggers(i, ts);
      for (uint32_t k = 0; k < n; k++)
        if (pass)
          trig[toff[ts[k] + 1]++] = i;
        else
          toff[ts[k] + 2]++;
    }
    if (!pass) {
      for (uint32_t s = 2; s < nslots + 2; s++)
        toff[s] += toff[s - 1];
      trig = malloc((toff[nslots + 1] + 1) * 4ull);
    }
  }
  free(freq);
  run_threads(scan);
  for (int i = 0; i < nth; i++)
    free(th[i].present), free(th[i].touched), free(th[i].tokids), free(th[i].tw);
  res = calloc(nq + 1, sizeof(Result));

  // Counts and sums add up; the first ten ids come from the threads in order,
  // since each holds a run of ascending ids and logged its first ten in order.
  for (int i = 0; i < nth; i++) {
    for (uint32_t q = 0; q < nq; q++) {
      Result *r = &res[prog[qoff[q] - 2]];
      r->count += th[i].qr[q].cnt, r->sum += th[i].qr[q].sum;
    }
    for (size_t j = 0; j < th[i].nlog; j++) {
      Result *r = &res[th[i].log[j] >> 32];
      if (r->m < 10)
        r->ids[r->m++] = (uint32_t)th[i].log[j];
    }
  }

  int out = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    die("cannot create /out/results.txt");
  char *ob = malloc(OUTSZ), *o = ob;
  for (uint32_t i = 0; i < nq; i++) {
    Result *r = &res[i];
    o = num(o, r->count);
    *o++ = ' ';
    o = num(o, r->sum);
    for (uint32_t k = 0; k < r->m; k++) {
      *o++ = ' ';
      o = num(o, r->ids[k]);
    }
    *o++ = '\n';
    if (o - ob > OUTSZ - 256) {
      if (write(out, ob, o - ob) != o - ob)
        die("write failed");
      o = ob;
    }
  }
  if (write(out, ob, o - ob) != o - ob || close(out))
    die("write failed");
  return 0;
}
