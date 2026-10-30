# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C rewrite: quote-parity split, hand-written CSV parser, array join, shared name table + per-group bitsets | _not pushed yet_ | | | |
| 2 | Memory pass: 64 KB buffers, 1-byte region map, packed name slots, one shared name arena | _not pushed yet_ | | | |

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
