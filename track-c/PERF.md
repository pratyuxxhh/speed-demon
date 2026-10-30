# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C rewrite: quote-parity split, hand-written CSV parser, array join, shared name table + per-group bitsets | _not pushed yet_ | | | |
| 2 | Memory pass: 64 KB buffers, 1-byte region map, packed name slots, one shared name arena | _not pushed yet_ | | | |
| 3 | No quote pass (spec 2.2 boundary rule); 64-byte name records holding their groups' bits (commit `a3eb9fc`) | _fill in from the board_ | | | |
| 4 | AVX2 bitmask CSV parser, mmap + 1 MB chunks from a counter, tag + record name table, batched prefetch, parallel regions.csv | _not pushed yet_ | | | |

The board columns are filled from board runs only. Until a push is measured, the local numbers below are what we have.

## Local measurements (sample scale, seed 1)

Machine: WSL2 Ubuntu on Windows 11, 16 hardware threads, 6 GB RAM, Docker 29.1.3. Every timed run is pinned with `--cpuset-cpus=0-11`, like the board's 12 threads.

Data: `speed-demon-2026/data/c`: 332,000 records. `records.csv` sha256 `07bc6368…addfd86` and `regions.csv` sha256 `1bd3904f…e5ece1`, both as published in the spec. The expected `aggregates.txt` sha256 is `9035fed36823232dd41ae2d1b9d6d21aa7c73f49d8e95647e953a89d5ebe9a66`: the spec's published sample hash, and what the unchanged reference produced here.

- **inside** is the program alone, timed by the organisers' `harness/timer/sd-timer.c` (CLOCK_MONOTONIC, exec to exit). It was built statically with the allowlisted gcc:15 image and run as the container's timer, as on the board.
- **peak** is the container cgroup's `memory.peak`. An empty container already peaks at about 5.3 MB here.
- **check.py** time includes container start-up.

| Step | check.py runs (s) | inside runs (ms) | median | peak (KiB) | median | image | sha256 |
|---|---|---|---|---|---|---|---|
| 0 reference, session 1 | 3.61, 2.81, 3.03 | 2597, 2508, 2678, 2594, 2649 | 2594 | 70096–70244 | 70160 | 118 MB | match |
| 0 reference, session 2 (same run as step 1) | | 2786, 2803, 2858, 2995, 3235 | 2858 | 70024–76312 | 70188 | 118 MB | match |
| 1 C rewrite | 0.53, 0.58, 0.43, 0.38, 0.33 | 48, 52, 53, 54, 54, 54, 54, 58, 63 | 54 | 17200–17844 | 17524 | 80 MB | match |
| 0 reference, session 3 (same run as step 2) | | 2758, 2854, 2870, 2969, 3284 | 2870 | 69980–70164 | 70136 | 118 MB | match |
| 2 memory pass | 0.39, 0.34, 0.34 | 43, 47, 48, 50, 53, 53, 53, 54, 69 | 53 | 11668–12324 | 12072 | 80 MB | match |

### Local K estimate (sample, 12 CPUs, not the board's number)

`K = (T_ref/T)^0.5 × min(M_ref/M, 50)^0.2 × min(S_ref/S, 1)^0.15 × (D_ref/D)^0.15`, with S_ref = 108 (`harness/reference_axes.json`) and S counted as non-blank, non-comment lines. The board counts after clang-format, so S may differ slightly.

| Step | T ratio | M ratio | S | D | K | points |
|---|---|---|---|---|---|---|
| 1 | 2858/54 = 52.9 → 7.27 | 70.2/17.5 = 4.01 → 1.32 | 367 → 0.832 | 1 → 1 | 8.0 | 45 |
| 2 | 2870/53 = 54.2 → 7.36 | 70.1/12.1 = 5.81 → 1.42 | 366 → 0.833 | 1 → 1 | 8.7 | 47 |
| 3 | 2625/45 = 58.3 → 7.64 | 70.2/13.1 = 5.34 → 1.40 | 353 → 0.837 | 1 → 1 | 8.9 | 48 |

The memory ratio on the sample understates the board's. The reference's peak grows with the data: 70 MB on sample, 367 MB on the board's full set. This program's peak is mostly fixed tables: the name table, the per-group bitsets, the region map and the read buffers. That is an expectation, not a measurement; full data has not been run locally.

## Step 1: C rewrite

- **Problem identified:** the reference decodes every byte into Python `str` objects through the `csv` module, re-encodes names and notes to count their bytes, and builds a list and five strings per record, all on one core (spec section 7).
- **Optimization:**
  1. **Parallel split that is exact despite LFs inside quotes.**
     - Pass 1 counts the `"` bytes in each thread's byte range with AVX2. Every `"` flips the quote state and a `""` escape flips it twice, so the running parity gives each range's starting state exactly.
     - From each range start, the first LF outside quotes is a record boundary, and the ranges are moved to those boundaries.
     - Pass 2 parses whole records per thread, reading with `pread` into a 256 KB buffer and carrying a partial record over the buffer edge.
  2. **A hand-written parser for spec section 2.1:**
     - a BOM (`EF BB BF`) is skipped only at a record start;
     - quoted fields end at a `"` that is not part of `""`, found with `memchr`;
     - unquoted fields run to `,`, CR or LF;
     - the terminator is CRLF or LF, and may be omitted at the end of the file;
     - amounts are parsed straight to integer cents.

     A record that would run past the buffer returns "incomplete" before any aggregate changes, and is parsed again once the next block is read.
  3. **Notes are never copied.** Their length after unescaping is (raw length − the number of `""` pairs).
  4. **Join:** a flat array maps region id to a group index (0 = `_UNMATCHED`; ids at or above 10,000,000 are unmatched too).
  5. **Distinct names, exactly:**
     - One shared open-addressing name table. An inserter claims an empty slot by swapping 0 for 1, writes the entry, then publishes the 64-bit hash, so readers wait only on a slot that is being filled.
     - A name containing `""` is unescaped into a scratch buffer first.
     - Each group has a bitset of name ids, so `names_distinct` is a popcount, and equality is byte for byte, never by hash.
     - If the table passes 3/4 full, the pass is redone with a table 4 times larger. That never happens with the spec's pool of 100,000 names.
  6. Per-thread aggregates per group, merged at the end; money printed from integer cents.
- **Speedup (local, sample):** 52.9x by the program's own time (2858 ms → 54 ms) and about 6–8x by check.py wall time, which container start-up dominates at this size.
- **Memory:** 17.5 MB peak vs 70.2 MB.
- **Correctness** (the track's main risk):
  - The sample sha256 matches.
  - The spec's worked example (section 4) is byte-identical.
  - **Other seeds, against the reference:** tiny seeds 1–5 and sample seeds 2 and 3 were generated with the organisers' generator, and each output was compared with the unchanged reference's. All 7 match.
  - **Thread count:** every dataset above gives the same hash on 1 CPU and on 12 CPUs. The split is exercised on every run, and a boundary mistake would change the hash with the thread count.
  - **Final record without a terminator** (allowed by section 2.1, never generated): the example with its last CRLF removed matches the reference.
  - Everything above was run with a `-fsanitize=address,undefined` build with `-Wall -Wextra`: no reports, no warnings.
- **Bug found on the way:** the group-key array was `char[4]`, and `"_UNMATCHED"` overflowed into the next country codes. The numbers were right but the keys were garbled. ASan cannot see an overflow inside one static array, but the worked-example diff showed it at once. The fix sized the keys to 12 bytes.

## Step 2: memory pass

- **Problem identified:** step 1 peaked at 17.5 MB, about 12 MB above the 5.3 MB empty-container baseline. Nearly all of it was fixed tables, not data:

  | Item | Size |
  |---|---|
  | 256 KB read buffer per thread | about 3 MB at 12 threads |
  | region map, 2 bytes per id, touched up to id ~1M | 2 MB |
  | name table, a separate 8-byte hash and 4-byte id per slot (2^18 slots) | 3 MB |
  | a 16-byte pointer+length per distinct name, plus a 64 KB name arena per thread (which also made each thread create its own glibc malloc arena) | |
  | 1,024 per-thread aggregate slots, all initialised | |
- **Optimization:**
  1. Read buffer 64 KB per thread. Each `pread` still moves 64 KB, so the syscall count stays trivial.
  2. Region map of one byte per id. Groups are capped at 256; the spec has 240 countries, and the program stops with an error above 255.
  3. One packed 8-byte word per name-table slot, `hash bits above 24 | name id`. 0 means empty and 1 means being written; the hash's top bit is always set, so a filled slot is never 0 or 1. A matching hash still leads to a full byte comparison.
  4. All distinct names in one shared arena, reserved at the size of `records.csv` (only the pages written count) and allocated by an atomic bump. Each name is an 8-byte offset+length entry, and no thread calls `malloc` while parsing.
  5. Only the groups that exist are initialised per thread, at most 256 instead of 1,024.
- **Result (sample, 12 CPUs, same session as its reference):** peak 12.1 MB vs 17.5 MB, at the same speed (53 ms vs 54 ms).
- **Correctness:**
  - The whole step-1 suite was re-run with the ASan/UBSan build: the worked example, the sample on 1 and 12 CPUs, tiny seeds 1–5 and sample seeds 2–3 against the reference on 1 and 12 CPUs, and the unterminated last record. All match.
  - The table-regrowth path never runs on real data. A throwaway copy that starts the name table at 1,024 slots, forcing two regrowths, gave the sample sha256 on 1 and on 12 CPUs.
- **What is left:** about 6.8 MB above the container baseline:
  - the per-group name bitsets: 241 groups × about 100,000 name ids, about 3 MB;
  - the name table: 2 MB;
  - the name arena and entries: about 2 MB;
  - the region map: 1 MB;
  - read buffers and stacks.

  The bitsets and the table are sized by the pool of about 100,000 names, which full scale shares, so they should not grow there. That is an expectation, not a measurement.

## Step 3: where the time goes, and the name-table layout

**Measuring first.** Phase timers in a throwaway copy of step 2 (sample):

| Phase | 1 CPU | 12 CPUs |
|---|---|---|
| regions.csv load | 5–9 ms | 5–7 ms |
| quote-count pass | 6–14 ms | 3–5 ms |
| parse and aggregate | 172–193 ms | 40–52 ms |
| output | 2–5 ms | 2–5 ms |

Variants of the parse isolated its parts:

| Variant | 1 CPU | 12 CPUs |
|---|---|---|
| CSV parsing only | 49 ms | 19–20 ms |
| + name lookup | 67–136 ms | 33–39 ms |
| + per-group bitsets (full step 2) | 89–193 ms | 40–49 ms |

Distinct-name counting cost more than the CSV parsing itself. Each record made about four scattered reads: the hash slot, the name entry, the name bytes in the arena, and an atomic OR into a 3 MB per-group bitset.

**Optimization:**

1. **No quote-count pass.** Spec 2.2, which the spec says may be relied on, guarantees that a note's LF is followed by a lowercase word and that names hold no LF. So an LF followed by a digit or a BOM is always a record boundary, and each thread starts at the first one at or after its share of the file. Checked on the sample: across all 371k LFs, the byte after an LF is a digit or a lowercase letter, nothing else. This saves one read of the file and the AVX2 quote counter.
2. **A 64-byte record per distinct name** (one cache line), holding:
   - its length;
   - its first 24 bytes, zero padded;
   - an arena offset for any bytes past 24;
   - its own 256-bit set of groups.

   A lookup is then the 8-byte hash slot plus that one line, which does both the exact comparison and the group bit. On the sample, 94% of names are at most 16 bytes and all are at most 24 (measured with a one-off script), so the arena is almost never touched. `names_distinct` per group is counted after the parse, in parallel, by popcounting the records' group sets, with thread k taking every nth name.

**Tried and not adopted: per-owner buckets instead of atomic group bits.** Threads logged `(group, name id)` pairs into buckets by `id % threads`, and each thread then set the bits of the names it owned, with no atomics. Same session, 7 runs each:

| Variant | 1 CPU | 8 P-core threads | Peak (KiB) |
|---|---|---|---|
| atomic bits | 135 ms | 55 ms | 10004–12188 |
| buckets | 113 ms | 47 ms | 11372–14080 |

The buckets were about 15% faster but log 4 bytes per record: about 13 MB at full. At full the time gain is worth √1.16 ≈ ×1.08 in K, and the memory cost about (12/25)^0.2 ≈ ×0.86, so atomic bits stay.

**This machine's scaling limit.** Per-thread timestamps showed some threads starting 12–27 ms late, and each of 12 threads running at 50–80 MB/s against about 250 MB/s for one thread alone. The CPU is an i5-13500H laptop chip: 4 P-cores with hyper-threading plus 8 E-cores, under a laptop power limit. A 12-thread run mixes in E-cores and lower clocks. The scoring box is 6 P-cores with hyper-threading and turbo off, so the 1-CPU time is the steadiest local measure of the code's own speed.

**Result (A/B against step 2 in the same session, 7 runs each, medians):**

| CPUs | step 2 | step 3 |
|---|---|---|
| 1 | 142 ms | 94 ms |
| 8 (P-cores) | 52 ms | 47 ms |
| 12 | 53 ms | 45 ms |

Peak memory at 12 CPUs went from 12.0 to 13.1 MB: 64 bytes per name instead of the previous entries and bitsets. Code fell from 366 to 353 lines.

**Correctness:** the full suite passed on the final code, with the ASan/UBSan build for every item:
- the worked example;
- the sample on 1 and 12 CPUs;
- tiny seeds 1–5 and sample seeds 2–3 against the reference, on 1 and 12 CPUs;
- the unterminated last record;
- the forced name-table regrowth (starting at 1,024 slots) on 1 and 12 CPUs.

All match.

## Full-scale measurements (seed 1, generated locally)

The full dataset was generated with the organisers' generator (`--seed 1 --scale full`), which is the board's dataset. `records.csv` sha256 `36166f4a…` and `regions.csv` `414c57c4…` match the spec, and every row below matched the published expected `aggregates.txt` sha256 `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536`. The reference ran once at full: **31,188 ms, 365 MB peak** (the board's calibration is 14.44 s, so this machine is about 2.2x slower than the scoring box).

| Session | Step | CPUs | runs | inside median (min–max), ms | peak median (KiB) |
|---|---|---|---|---|---|
| A | 3 | 1 | 1 | 1820 | 13048 |
| A | 3 | 0-11 | 7 | 209 (178–312) | 13408 |
| B (final A/B) | 3 | 1 | 3 | 1179 (952–1513) | 10480 |
| B | 4 | 1 | 3 | 646 (552–731) | 10708 |
| B | 3 | 0-11 | 9 | 166 (145–190) | 13476 |
| B | 4 | 0-11 | 9 | 121 (110–130) | 13404 |

Sample, session B:

| Step | CPUs | ms | peak (KiB) |
|---|---|---|---|
| 3 | 1 | 138 | 10036 |
| 4 | 1 | 85 | 10748 |
| 3 | 0-11 | 51 | 13000 |
| 4 | 0-11 | 37 | 12884 |

The reference: 2939 ms and 70164 KiB at 0-11.

### Local K estimates

| Data | Step | T ratio | M ratio | S | K | points |
|---|---|---|---|---|---|---|
| full (A) | 3 | 31188/209 = 149 → 12.22 | 365.1/13.4 = 27.2 → 1.94 | 353 → 0.837 | 19.8 | 65 |
| full (B) | 3 | 31188/166 = 188 → 13.71 | 365.1/13.5 = 27.1 → 1.94 | 353 → 0.837 | 22.2 | 67 |
| full (B) | 4 | 31188/121 = 258 → 16.05 | 365.1/13.4 = 27.2 → 1.94 | 372 → 0.831 | 25.8 | 71 |
| sample (B) | 3 | 2939/51 = 57.6 → 7.59 | 70.2/13.0 = 5.40 → 1.40 | 353 → 0.837 | 8.9 | 47 |
| sample (B) | 4 | 2939/37 = 79.4 → 8.91 | 70.2/12.9 = 5.45 → 1.40 | 372 → 0.831 | 10.4 | 51 |

## Step 4: SIMD parser, chunks, batched lookups

- **Problem identified (step 3).** At 1 CPU on sample:

  | Variant | Time |
  |---|---|
  | CSV parsing only | 49 ms |
  | full step 3 | 89–193 ms |

  The scalar parser found field ends with `memchr` and byte loops. Each record then made dependent cache misses: the hash slot, then the 64-byte name record, then its group bit. Twelve fixed ranges let late-starting threads hold up the end.
- **Optimization:**
  1. **AVX2 bitmask parsing.** For each 64-byte block:
     - masks of `"`, `,` and LF;
     - inside-quotes = prefix XOR of the quote bits, carried from block to block;
     - field ends = commas and LFs outside quotes, walked with `ctz`.

     A `""` escape flips the state twice, so it needs no special case. A chunk starts at a record boundary, so the state starts outside. A quoted field of raw length L with Q quote bytes, counted by popcount, has L − 1 − Q/2 value bytes, so notes are never scanned again. A CR before an LF is dropped from the amount, and a BOM is checked only at a record start.
  2. **`mmap` and 1 MB chunks from an atomic counter.** By the spec 2.2 rule (an LF followed by a digit or a BOM starts a record), a chunk's first record is found from any offset. The file is mapped over a zeroed region one page longer, so the 64-byte loads past the end see zeros. This removes the read buffers and the "incomplete record, read more" logic.
  3. **Name table: 1-byte tags plus 64-byte records in slot order.** A record holds the name's first 24 bytes and its 32-bit length (compared in one 32-byte AVX2 compare), an arena offset for longer names, and the 256-bit group set. That is 2^17 slots (8 MB of records, 128 KB of tags). It is regrown and the pass redone if more than 7/8 full; a forced regrowth was tested in step 3.
  4. **Batched prefetch.** Each parsed record is queued with its name hash; its tag, record line and region-map byte are prefetched, and every 32 records the batch is applied. A name with `""` escapes is unescaped and applied at once. The group bit is read with a plain load before `lock or`.
  5. **`regions.csv` in parallel.** Every thread parses a line-aligned slice into the shared map. A country's group is claimed by swapping 0 for BUSY in its 3-letter code's slot, so there are no holes in the numbering.
- **Tried and not adopted:**
  - **Huge pages for the name records** (`MADV_HUGEPAGE`; THP is in `madvise` mode here). Full scale, same session: 12 CPUs 126 ms against 142 ms without, but peak 19.2 MB against 13.4 MB. In K that is about √(142/126) × (13.4/19.2)^0.2 ≈ 0.99, a small loss, so it is off.
  - **Not done: a hash as the name's identity.** The spec compares names as exact byte strings, and a 64-bit collision, however unlikely, would change `names_distinct`. Names stay compared byte for byte.
- **Correctness:**
  - The whole step-3 suite passed again with the ASan/UBSan build: the worked example, the sample on 1 and 12 CPUs, tiny seeds 1–5 and sample seeds 2–3 against the reference on 1 and 12 CPUs, and the unterminated last record. The full dataset's published sha256 also matches, with the optimised build.
  - **Differential fuzzing against the unchanged reference.** A generator of hostile files following spec 2.1/2.2 produced 60 files: 551 mid-file BOMs, 5,704 CRLF endings, about 17,000 `""` escapes, 5,040 LFs inside notes, and 15 files without a final terminator. Each ran with `CHUNK` set to 64 bytes, 4 KB and 1 MB, on 1 and 12 CPUs: 360 runs, 0 mismatches. The final code ran another 20 files the same way: 0 mismatches.
