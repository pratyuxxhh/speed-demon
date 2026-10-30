# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | C rewrite: index only query terms, threaded scan, sorted posting merges, phrase check by re-tokenizing | _not pushed yet_ | | | |
| 2 | Hybrid list/bitmap index, block SIMD tokenizer, bitmap BOOL, shared scan for all-frequent phrases, -O3 -march=x86-64-v3 | _fill in from the board_ | | | |
| 3 | Phrases matched during the scan by an Aho-Corasick automaton over term ids; block streams; batched prefetched lookups | _not pushed yet_ | | | |

The board columns are filled from board runs only. Until a push is measured, the local numbers below are what we have.

## Local measurements (sample scale, seed 1)

Machine: WSL2 Ubuntu on Windows 11, 16 hardware threads, 6 GB RAM, Docker 29.1.3. Every timed run is pinned with `--cpuset-cpus=0-11`, like the board's 12 threads.

Data: `speed-demon-2026/data/b`: 25,000 documents; `docs.txt` sha256 `e157fabb…6eaa4`, `queries.txt` sha256 `930ba871…f16aa`, both as published in the spec. The expected `results.txt` sha256 is `eeeec1e9319da98a6f3c664c78f93e7078fd603c25c2bb3027174e81e3638a8f`: the spec's published sample hash, and what the unchanged reference produced here.

- **inside** is the program alone, timed by the organisers' `harness/timer/sd-timer.c` (CLOCK_MONOTONIC, exec to exit). It was built statically with the allowlisted gcc:15 image and run as the container's timer, as on the board.
- **peak** is the container cgroup's `memory.peak`. An empty container already peaks at about 5.3 MB here.
- **check.py** time includes container start-up.

| Step | check.py runs (s) | inside runs (ms) | median | peak (KiB) | median | image | sha256 |
|---|---|---|---|---|---|---|---|
| 0 reference, session 1 | 6.41, 5.34, 5.15 | 5056, 4850, 4824, 5086, 5105 | 5056 | 303820–328920 | 304088 | 118 MB | match |
| 0 reference, session 2 (same run as step 1) | | 2827, 2864, 3770, 5080, 5543 | 3770 | 303960–310300 | 304164 | 118 MB | match |
| 1 C rewrite | 0.45, 0.44, 0.47, 0.40, 0.42 | 65, 78, 79, 79, 81, 90, 93, 96, 99 | 81 | 19540–20424 | 20156 | 80 MB | match |
| 0 reference, session 3 (same run as step 2) | | 4880, 4910, 5184, 5207, 5225 | 5184 | 303948–304196 | 304000 | 118 MB | match |
| 2 hybrid index + SIMD tokenizer | 0.44, 0.34, 0.38 | 49, 52, 52, 52, 58, 59, 62, 65, 77 | 58 | 15012–16028 | 15456 | 80 MB | match |

The reference spread widely in session 2: the machine was busier. Step 1 is compared with the session-2 reference, which was measured in the same script run.

### Local K estimate (sample, 12 CPUs, not the board's number)

`K = (T_ref/T)^0.5 × min(M_ref/M, 50)^0.2 × min(S_ref/S, 1)^0.15 × (D_ref/D)^0.15`, with S_ref = 91 (`harness/reference_axes.json`) and S counted as non-blank, non-comment lines. The board counts after clang-format, so S may differ slightly.

| Step | T ratio | M ratio | S | D | K | points |
|---|---|---|---|---|---|---|
| 1 | 3770/81 = 46.5 → 6.82 | 304.2/20.2 = 15.1 → 1.72 | 452 → 0.786 | 1 → 1 | 9.2 | 48 |
| 2 | 5184/58 = 89.4 → 9.45 | 304.0/15.5 = 19.7 → 1.81 | 628 → 0.748 | 1 → 1 | 12.8 | 55 |

Against the session-1 reference (5056 ms), the T ratio would be 62 and K would be 10.7.

## Step 1: C rewrite

- **Problem identified:** the reference builds a Python `set` for every one of the 609,833 distinct corpus terms and keeps every document's text in memory (2.2 GB at full). It answers PHRASE queries by re-tokenizing every candidate with a regex, and it fully sorts each result set. At sample scale, the queries use only 11,430 distinct terms; their (term, doc) pairs are 1.51M of the corpus's 2.97M. That was measured with a one-off Python script over the sample.
- **Optimization:**
  1. Queries are read first, and their terms are interned in an open-addressing FNV-1a table. That table is the whole vocabulary that gets indexed.
  2. `docs.txt` is split into one line-aligned range per CPU the container may use, read with `pread` into a 256 KB buffer per thread. A 256-byte lookup table does the ASCII lowercasing and the token/separator test. Each token is hashed as it is scanned and looked up; tokens longer than 32 bytes cannot be query terms and are skipped.
  3. A per-thread stamp array keeps one entry per (doc, term). Each thread appends `DOC|id` markers and term ids to a local stream and counts its document frequencies. It also records every document's text offset and length.
  4. Per-term slices in one u32 posting array, each thread's cursor after the earlier threads'. Threads own contiguous id ranges, so every list comes out sorted without a sort.
  5. Queries run in parallel in blocks of 16:
     - BOOL applies AND (galloping, iterating the smaller side), NOT (galloping) and OR (linear merge) strictly left to right, into two scratch buffers per thread.
     - PHRASE intersects the terms' lists starting from the rarest. It then confirms each candidate by `pread`ing its text, tokenizing it, and matching term ids through a k-slot ring.
  6. Results (count, sum, first 10 ids) are formatted with hand-written integer printing into a 64 KB buffer.
- **Speedup (local, sample):** 46.5x by the program's own time (3770 ms → 81 ms) and about 12x by check.py wall time, which container start-up dominates at this size.
- **Memory:** 20.2 MB peak versus 304 MB. About 5 MB of that is the container baseline.
- **Correctness:**
  - The sample sha256 matches.
  - The spec's worked example (section 4) is byte-identical. It covers the left-to-right `estate OR surety AND bail`, `zurich` matching nothing, `7` versus `007`, `"bond bail"` across a comma, `"estate bond"` across `. `, `"forfeited writ"` not spanning two documents, and the empty document.
  - A `-fsanitize=address,undefined` build with `-Wall -Wextra` runs clean on the example and on the sample, apart from the output buffer deliberately not being freed at exit.
- **Bug found on the way:** the first build crashed with exit 139. `intern` looked up the term before the hash table was first allocated. The fix grows the table before the lookup; ASan pinpointed it.

## Step 2: hybrid list/bitmap index, block SIMD tokenizer

- **Problem identified.** Phase timers in a throwaway copy of step 1 (stderr, CLOCK_MONOTONIC; sample, 12 CPUs) gave:

  | Phase | Time |
  |---|---|
  | queries | 2–3 ms |
  | scan | 47–51 ms |
  | build | 3–5 ms |
  | answer | 11–14 ms |
  | write | about 1 ms |

  The scan was about 70% of the run. Single-threaded it took 306 ms, about 85 ns per token. Aligning the per-thread structs to cache lines changed nothing, which ruled out false sharing.

  Variants of the step-2 scan isolated the cost (single-threaded, before the block tokenizer):

  | Variant | Scan time |
  |---|---|
  | tokenize only | 122–150 ms |
  | plus term lookup | 203–211 ms |
  | full indexing | 223–235 ms |

  So the tokenizer was the largest part, and the term lookup next.

  At full scale, step 1 would also have kept every (term, doc) pair twice for a while: in the per-thread streams and in the posting array. It also gave every thread two N-sized scratch lists.
- **Optimization:**
  1. **Hybrid index.** A term starts as a list; its matches go to the per-thread stream. When a thread has counted it in more than 64 documents and more than 1/32 of the documents it has seen, the term is promoted to a shared N-bit bitmap. From then on, every thread sets bits directly, and its later matches never touch a stream. Matches streamed before the promotion are folded into the bitmap by the fill step.
  2. **Bitmap writes without atomics, except at the edges.** Each thread knows its first document id and the next thread's before scanning. Only the two 64-bit words holding its first and last document can be shared, so only those use an atomic OR.
  3. **Block tokenizer.** AVX2 classifies 64 bytes into a mask; token starts and ends are the mask's rising and falling edges, enumerated with `ctz`. A token's four zero-padded 8-byte words are built without branches, and lowercasing is a single `| 0x20` per word: every token byte is a letter or a digit, and that OR lowercases A–Z and leaves a–z and 0–9 alone. One routine turns a document into an array of term ids for indexing, phrase checking and the shared scan alike.
  4. **Hash slots carry the term's first 8 bytes and its length,** so a lookup of a term of up to 8 bytes (most of them) is one 16-byte slot read. The hash mixes 8-byte words, not single bytes. The stamp and count per (thread, term) share one 8-byte entry.
  5. **BOOL over lists and bitmaps,** strictly left to right:
     - list op list: gallop or merge; an OR whose inputs exceed the list cap becomes a bitmap;
     - list AND/NOT bitmap: bit tests;
     - bitmap AND list: bit tests into a list;
     - otherwise word-wise on a per-thread bitmap.

     Scratch is two lists of `max(longest list, N/32)` entries plus one N-bit bitmap per thread, instead of two N-entry lists. Bitmap results are summarized with `ctz` over set bits.
  6. **PHRASE** takes the rarest term's documents, keeps those that hold every other term (bit test or gallop), and confirms each by re-tokenizing it. A phrase whose rarest term is still in more than N/64 documents would need about N such checks. All of those phrases are instead matched in one extra parallel scan of the corpus, with phrases chained by first term. The sample has none (`nheavy 0`); the spec's 92 anchor-less `repeat` phrases at full are what it is for.
  7. `-O3 -march=x86-64-v3`: AVX2, which the scoring box's P-cores have. It is a fixed target, not `-march=native`, so the board's build machine can build it.
- **Result (sample, 12 CPUs, same session as its reference):** 58 ms median vs 81 ms for step 1; peak 15.5 MB vs 20.2 MB.
  - Phase timers after the change: scan 62–72 ms in one session, versus 34–79 ms for step 1's code in the same kind of session. This machine's timing noise is large at this size.
  - Single-threaded, the whole scan fell from about 300 ms to 223–240 ms.
- **Correctness:** the sample sha256 matches, and the spec's worked example is byte-identical. The `-fsanitize=address,undefined` build with `-Wall -Wextra` is clean on both, apart from the output buffer not being freed at exit.
- **The shared scan, tested by force.** The sample never triggers it, so a throwaway copy with the threshold set to 0 sent every one of the 1,900 sample phrases through it. That copy gave the same sample sha256 (`eeeec1e9…`) on 1 and on 12 CPUs, and the worked example byte for byte.
- **Not verified here:** memory at full scale. The stream now carries only list terms, which is the change that matters at full scale and cannot be seen on the sample.
- **Source size:** 628 code lines, up from 452. The (91/S)^0.15 factor fell from 0.786 to 0.748; the time and memory gains outweigh it.

## Full-scale measurements (seed 1, generated locally)

The full dataset was generated with the organisers' generator (`--seed 1 --scale full`), which is the board's dataset. `docs.txt` sha256 `6cd46167…f18e66c8` and `queries.txt` `77ef7ffc…8d39f9` match the spec. Every row below matched the published expected `results.txt` sha256 `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927`. This is the same machine and timer as above. The reference ran once at full: **118,718 ms, 2,154 MB peak** (the board's calibration is 56.35 s, so this machine is about 2.1x slower than the scoring box).

| Step | CPUs | runs | inside median (min–max), ms | peak median (KiB) |
|---|---|---|---|---|
| 2 | 1 | 1 | 7049 | 125892 |
| 2 | 0-11 | 7 | 958 (902–1001) | 164324 |
| 3 | 1 | 1 | 4382 | 112204 |
| 3 | 0-11 | 7 | 759 (707–812) | 136956 |

Same session, sample scale, 7 runs each:

| Step | CPUs | ms | peak (KiB) |
|---|---|---|---|
| 2 | 1 | 286 | 8400 |
| 3 | 1 | 271 | 8604 |
| 2 | 0-11 | 80 | 15668 |
| 3 | 0-11 | 70 | 15872 |

The reference: 5873 ms and 303984 KiB at 0-11.

### Local K estimates

| Data | Step | T ratio | M ratio | S | K | points |
|---|---|---|---|---|---|---|
| full | 2 | 118718/958 = 123.9 → 11.13 | 2154/164.3 = 13.1 → 1.67 | 628 → 0.748 | 13.9 | 57 |
| full | 3 | 118718/759 = 156.4 → 12.51 | 2154/137.0 = 15.7 → 1.74 | 633 → 0.748 | 16.2 | 61 |
| sample | 2 | 5873/80 = 73.4 → 8.57 | 304.0/15.7 = 19.4 → 1.81 | 628 → 0.748 | 11.6 | 53 |
| sample | 3 | 5873/70 = 83.9 → 9.16 | 304.0/15.9 = 19.2 → 1.80 | 633 → 0.748 | 12.4 | 55 |

## Step 3: phrases in the scan, measured at full scale

- **Problem identified on full data (step 2, 12 CPUs).** Phase timers with peak RSS:

  | Phase | Time | Peak RSS after it |
  |---|---|---|
  | queries | 45–60 ms | 12 MB |
  | scan | 383–650 ms | 113 MB |
  | fill | 50–59 ms | 157 MB |
  | shared phrase scan | 0.7 ms | 157 MB |
  | answer | 459–511 ms | 157 MB |
  | write | 7–14 ms | 157 MB |

  - The shared scan never runs at full either: no phrase's rarest term is above N/64.
  - The answer phase was about half the run. Its CPU time split was 794 ms for BOOL and **5270 ms for PHRASE**. Re-reading and re-tokenizing candidate documents with `pread` is what grows at full scale.
  - The fill added 44 MB: per-thread streams and the posting array alive together.
- **Optimization:**
  1. **Aho-Corasick over term ids.**
     - Every PHRASE query's term sequence goes into one trie; identical phrases share an end node. Failure links and output links are computed in order of depth.
     - During the scan, each query-term token steps the automaton, and any other token or a document start resets it.
     - A completed match is recorded exactly like a term, under an id after the terms. It goes through the same stamp, stream, bitmap promotion and posting slices.
     - A PHRASE query then just reads that id's set.

     This removed `phrase_in`, the candidate intersection, the shared scan and the per-document offset and length arrays.
  2. **Streams as chains of 1 MB `mmap`ed blocks**, unmapped as the fill copies them; no `realloc` doubling. This did **not** remove the fill's extra memory. Each thread's entries are spread over every term's slice, so the whole posting array is written early in the fill while most blocks are still mapped. Peak after fill stays about streams + postings. Removing that would need compressed postings.
  3. **Batched lookups.** Each line is tokenized first; every token's words and hash are computed, its slot is prefetched, and only then are the lookups done, so their cache misses overlap. At full the term table has 75,747 terms in 4 MB of slots, which does not fit in L2.
  4. **Automaton fast paths:**
     - A term in no phrase resets to the root without a lookup.
     - At the root, only a phrase's first term moves, read from a direct per-term array.
     - A node with one child keeps that child inline; only nodes with several children use the edge hash.
- **What the automaton costs.** Counters, 1 CPU, full: 36.3M tokens, 23.3M automaton steps, 14.6M of them from a non-root state, 13.6M failure-link moves, and only 19,356 phrase matches. Phrases are windows of real text, so they start with common words: most tokens step the automaton and most steps fail.

  | Single-thread scan variant (full) | Time |
  |---|---|
  | tokenize only | 774 ms |
  | + lookup | 1531 ms |
  | + indexing | 2538 ms |
  | + automaton, before the fast paths | 4090 ms |

  With the fast paths, the automaton still adds 1.4–1.5 s of single-thread scan time: 2356–2690 ms without it, 3733–4226 ms with it.
- **Result (full, same session, medians):**

  | CPUs | step 2 | step 3 |
  |---|---|---|
  | 1 | 7049 ms | 4382 ms |
  | 12 | 958 ms | 759 ms |
  | peak | 164 MB | 137 MB |

  Answer phase at 12 CPUs went from 459–511 ms to 52–110 ms. Sample, 12 CPUs: 80 → 70 ms.
- **Correctness:** ASan/UBSan build, `-Wall -Wextra`:
  - the worked example, byte for byte;
  - the sample sha256 on 1 and 12 CPUs;
  - tiny seeds 1–3 and sample seed 2 against the reference on 1 and 12 CPUs;
  - the full dataset's published sha256, with the optimised build.

  All match.
- **Not done:**
  - **A 1-byte tag filter in front of the term table:** most tokens are query terms, so it would reject few.
  - **`mmap` and chunked work for the scan:** the scan is CPU-bound, not copy-bound.
  - **Memory under 44 MB:** it would need compressed posting lists. The 11.4M list pairs (45 MB as u32) alone exceed it.
