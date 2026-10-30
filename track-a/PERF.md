# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C rewrite: 2-pass threaded scan, flat group array, quickselect | _not pushed yet_ | | | |
| 2 | Memory cuts: small buffers, in-place percentiles, no per-thread error arrays, affinity-based thread count | _not pushed yet_ | | | |
| 3 | -O3 -march=x86-64-v3, AVX2 endpoint scan, word hash, method length from 2 letters, bands removed (commit `d8af656`) | _fill in from the board_ | | | |
| 4 | One parse pass into per-hour buckets, shared lock-free endpoint table, per-hour parallel finish with an ordered writer, mmap, 1 MB chunks from a counter | _not pushed yet_ | | | |

The board columns are filled from board runs only. Until a push is measured, the local numbers below are what we have.

## Local measurements (sample scale, seed 1)

Machine: WSL2 Ubuntu on Windows 11, 16 hardware threads, 6 GB RAM, Docker 29.1.3. Data: `speed-demon-2026/data/a` (310,000 lines, 23.9 MB, `requests.log` sha256 `7b22c678…86847`). Expected `summary.txt` sha256 `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` (the spec's published sample hash, and what the unchanged reference produced here).

- **check.py time** is wall time including container start-up.
- **inside** is the program alone: `date +%s%N` around the entrypoint inside the container.
- **peak** is the container cgroup's `memory.peak`. An empty container (`sh` + `date`) already peaks at about 5.3 MB on this machine.

| Step | check.py runs (s) | median | inside runs (ms) | median | peak (MB) | image | sha256 |
|---|---|---|---|---|---|---|---|
| 0 reference | 4.03, 5.16, 4.10 | 4.10 | 3780, 3650, 3842 | 3780 | 67.7–67.9 warm (99.8 on the first, cold run) | 118 MB | match |
| 1 C rewrite | 0.53, 0.41, 0.42, 0.40, 0.47 | 0.42 | 50, 32, 42, 39, 49 | 42 | 21.5–24.3 (median 22.2) | 80 MB | match |
| 2 memory cuts, unpinned (16 CPUs) | | | 31–41 (9 runs) | 37 | 13.4–14.9 (median 14.1) | 80 MB | match |
| 2 memory cuts, `--cpuset-cpus=0-11` | 0.40, 0.35, 0.31, 0.31, 0.23 | 0.31 | 24–44 (9 runs) | 29 | 12.8–14.3 (median 13.4) | 80 MB | match |
| 0 reference, `--cpuset-cpus=0-11` | | | 3622, 3667, 3671, 3675, 12255 | 3671 | 67.6–76.5 (median 67.8) | 118 MB | match |

The 12-CPU rows are the closest local match to the board (6 P-cores, 12 threads). The reference's 12255 ms run was an outlier on this machine, and the median ignores it.

**Timing correction.** The "inside" numbers above came from `date +%s%N`, which reads the wall clock. Under WSL2 that clock jumps: later runs printed negative times, and the reference's 12255 ms outlier was most likely one of those jumps. All later local timings use the organisers' `harness/timer/sd-timer.c` (CLOCK_MONOTONIC, exec to exit), built statically with the allowlisted gcc:15 image and run as the container's entry, as on the board. Re-measured that way, step 2 at `--cpuset-cpus=0-11`, 5 runs each:

| | inside runs (ms) | median | peak (KiB) |
|---|---|---|---|
| reference | 5110, 4945, 4919, 5283, 4990 | 4990 | 67732–67968 |
| step 2 | 43, 55, 47, 37, 34 | 43 | 13220–13572 |

That is 116x by the monotonic clock. The machine was busier during this second session, and both programs slowed together.

### Local K estimate (sample, 12 CPUs, not the board's number)

`K = (T_ref/T)^0.5 × min(M_ref/M, 50)^0.2 × min(S_ref/S, 1)^0.15 × (D_ref/D)^0.15`, with S counted as non-blank, non-comment lines (the board counts after clang-format, so it may differ slightly):

| Step | T ratio | M ratio | S | D | K | points (50 log10 K) |
|---|---|---|---|---|---|---|
| 1 | 3780/42 = 90.0 → 9.49 | 67.8/22.2 = 3.06 → 1.25 | 296 → 0.779 | 1 → 1 | 9.2 | 48 |
| 2 | 3671/29 = 126.6 → 11.25 | 67.8/13.4 = 5.08 → 1.38 | 324 → 0.769 | 1 → 1 | 12.0 | 54 |
| 2, sd-timer | 4990/43 = 116 → 10.77 | 67.8/13.4 = 5.06 → 1.38 | 324 → 0.769 | 1 → 1 | 11.5 | 53 |
| 2, session 3 (A/B) | 4210/35 = 120 → 10.97 | 67.9/13.5 = 5.02 → 1.38 | 324 → 0.769 | 1 → 1 | 11.7 | 53 |
| 3, session 3 (A/B) | 4210/32 = 132 → 11.47 | 67.9/13.3 = 5.11 → 1.39 | 318 → 0.771 | 1 → 1 | 12.3 | 54 |
| 3, session 4 (A/B) | 3831/32 = 120 → 10.94 | 67.9/13.4 = 5.06 → 1.38 | 318 → 0.771 | 1 → 1 | 11.7 | 53 |
| 4, session 4 (A/B, 11 runs) | 3831/29 = 132 → 11.49 | 67.9/16.2 = 4.20 → 1.33 | 283 → 0.784 | 1 → 1 | 12.0 | 54 |
| 4, session 4 (2 × 15 runs) | 3831/25 = 153 → 12.38 | 67.9/16.2 = 4.20 → 1.33 | 283 → 0.784 | 1 → 1 | 12.9 | 56 |

Step 1 was measured unpinned (16 CPUs) and step 2 pinned to 12 CPUs. Unpinned, step 2 is 37 ms and 14.1 MB, which is still better than step 1 on both.

## Step 4: one pass, no merge, per-hour parallel finish

- **Problem identified (step 3):**
  - Pass 2 re-read and re-parsed every line, including the endpoint hash and lookup, only to learn where each latency belonged.
  - The merge of per-thread endpoint tables and the output were serial: at 12 CPUs on sample, about 12–15 ms of a 32 ms run.
  - Twelve equal byte ranges assume equally fast cores, which a hybrid P/E-core CPU or a busy host breaks.
- **Optimization:**
  1. **One pass over the text.** Each line appends its latency (u32) and a tag (endpoint id in 12 bits, plus a 5xx bit, u16) to the parsing thread's bucket for its hour. That is 6 bytes per line: about 18.6 MB at full scale, against step 3's 12.4 MB latency array plus per-thread count tables. Latencies are full u32 per the spec (0 to 4294967295), so they cannot be packed into fewer bits.
  2. **One shared endpoint table**, lock-free like track C's: claim a slot by compare-and-swap, store the bytes, publish `hash | id`. After the first lines every lookup is a read. There are no per-thread tables to merge; the 1,842 endpoints are ranked once at the end with `qsort`.
  3. **Per-hour finish, in parallel.** A thread takes an hour from a counter, counts that hour's records by endpoint across every thread's bucket, scatters the latencies into a per-thread array grouped by endpoint, quickselects p99, p95, p50, and formats the hour's lines into a buffer.
  4. **Ordered output without waiting.** A finished hour's buffer is handed over. Whoever holds the writer lock (taken with `trylock`) writes every finished hour from the next unwritten one on, then checks again after unlocking. `main` flushes once more after the threads join, so a handover that raced the unlock is still written in order. Written buffers are recycled through a small spare list.
  5. **`mmap` instead of `pread`**, over a zeroed region one page longer, so the AVX2 space search and the word hash can read a few bytes past the last line safely. The harness pre-warms the dataset into the page cache with `vmtouch` before timing, so the mapped pages are not charged to the container. The local peaks below agree: step 4 at 1 CPU is 8.8 MB against step 3's 9.8 MB.
  6. **Chunked load balancing:** 1 MB line-aligned chunks taken from an atomic counter, and never more threads than chunks.
- **Not done:**
  - **Hour-by-hour streaming with a reorder buffer.** The spec says "no bound on how far a line can be out of order is promised: a solution must not rely on one", and shard 0 uploads each hour 5 to 10 minutes after it ends. The per-hour buckets above give the same per-hour parallelism without relying on order.
  - **A 14-bit latency field:** latencies span the full u32 range.
- **Tried on the way:**

  | Variant | Hour phase at 12 CPUs | Kept? |
  |---|---|---|
  | ticket order, waiting threads spinning on `sched_yield` | 13–21 ms (4 CPUs: 7.5 ms) | no: the spinners took CPU from the workers on this 4-P-core machine |
  | ticket order, waiting on a condition variable | 22–65 ms | no: a chain of 72 sleep/wake handoffs, each costing up to about 1 ms in this VM |
  | writer-combining handover (the design above) | 7–12 ms | yes |
  | raising glibc's mmap threshold, so 140 KB hour buffers stay in the heap | | no |
  | recycling hour buffers through a spare list instead | | yes |

  Same session, 15 runs × 2 rounds, glibc threshold vs recycling:

  | | 8 CPUs | 12 CPUs | peak |
  |---|---|---|---|
  | glibc threshold | 22 ms | 21 ms | 0.5–0.8 MB higher |
  | recycling | 22–23 ms | 24–26 ms | |

  Recycling is kept because the glibc setting changes every allocation, including the growing per-hour buckets, whose memory at full scale could suffer more than the sample shows.
- **Result (A/B against step 3, same session, 11 runs each, medians):**

  | CPUs | step 3 | step 4 |
  |---|---|---|
  | 1 | 62 ms, 9.8 MB | 41 ms, 8.8 MB |
  | 8 | 32 ms, 12.1 MB | 23 ms, 14.8 MB |
  | 12 | 32 ms, 13.4 MB | 29 ms, 16.2 MB (24–26 ms in the 15-run rounds) |

  Code fell from 318 to 283 lines. Peak memory rose at 8 and 12 CPUs. On sample the buckets are only 1.9 MB, so most of the rise is per-thread buckets and malloc arenas, plus the 5 MB output file, whose page cache is charged to the writing container in every variant.
- **Correctness:** everything below ran with the ASan/UBSan build with `-Wall -Wextra`, and the chunk splitting is exercised at every boundary:
  - the worked example, byte for byte;
  - an empty input, giving an empty output;
  - the sample sha256 on 1 and 12 CPUs;
  - tiny seeds 1–3 and sample seeds 2–3 against the reference on 1 and 12 CPUs.

  All match.

## Step 3: compiler flags, faster parsing, no dead code

- **Problem identified.** Phase timers in a throwaway copy of step 2 (sample):

  | Phase | 1 CPU | 12 CPUs |
  |---|---|---|
  | pass 1 | 39–63 ms | 12.5 ms |
  | merge | 1.4–2 ms | 2–5 ms (serial) |
  | pass 2 | 23–29 ms | 8.5–9.8 ms |
  | percentiles | 7–12 ms | 2–3 ms |
  | output | 8–14 ms | 9–10 ms (serial) |

  Step 2 was built with `-O2` and no CPU target, while tracks B–D use `-O3 -march=x86-64-v3`. Parsing went byte by byte: the method was skipped with a loop, the endpoint was scanned and hashed with FNV-1a at one multiply per byte, and the latency digits were read in a loop.
- **Optimization:**
  1. `-O3 -march=x86-64-v3` (AVX2; a fixed target, not `-march=native`).
  2. The method's length comes from its first two letters (G/H/D/O; P+U, P+O, P+A). The spec lists exactly seven methods.
  3. The endpoint's end is found by an AVX2 compare for the space, 32 bytes at a time. Read buffers have 64 bytes of slack.
  4. The endpoint hash mixes 8-byte words instead of single bytes.
  5. The unused hour-bands code (`-DBANDS`, default 1) is removed: 6 fewer counted lines.
- **Tried and not adopted: parallel output.** Line lengths were summed per hour during the percentile step, and each thread formatted whole hours and `pwrite`d them at their offsets. Five runs each at 12 CPUs, output phase only:

  | Variant | Output phase |
  |---|---|
  | 12 writers | 13–23 ms |
  | 12 writers, file pre-sized with `ftruncate` | 10–25 ms |
  | one writer | 9–12 ms |

  The kernel serializes writes to one file, so the threads mostly waited, and the code went back to one sequential writer.
- **Result:** in phase timers, pass 1 on 1 CPU fell from 39–63 ms to about 20 ms. End to end, A/B against step 2 in the same session (9 runs each, medians):

  | CPUs | step 2 | step 3 |
  |---|---|---|
  | 1 | 66 ms | 60 ms |
  | 8 | 40 ms | 32 ms |
  | 12 | 35 ms | 32 ms |

  Peak memory is unchanged, about 13.3 MB. The reference took 4210 ms in this session.
- **Correctness:** everything below ran with a `-fsanitize=address,undefined` build with `-Wall -Wextra`:
  - the worked example, byte for byte;
  - an empty input, giving an empty output;
  - the sample sha256 on 1 and 12 CPUs;
  - tiny seeds 1–3 and sample seeds 2–3, generated with the organisers' generator, against the unchanged reference on 1 and 12 CPUs.

  All match.

## Step 2: memory reduction

- **Problem identified:** an empty container already peaks at about 5.3 MB, so step 1's 22 MB was about 17 MB of our own. The sample's latency array is only 1.2 MB of that. The rest was fixed per-thread and global costs:
  - per thread: a 256 KB read buffer, a 64 KB 32-bit slot table, and per-thread count *and* error arrays;
  - global: a 1.6 MB percentile array, a 530 KB cursor copy and a 1 MB output buffer.
- **Optimization:**
  1. Read and output buffers cut from 256 KB and 1 MB to 64 KB each.
  2. 16-bit hash slots, with CAP 4096 (the spec promises 1,842 endpoints) in an 8192-slot table, so 16 KB per thread.
  3. p50/p95/p99 written in place into the first slots of each group's latency slice, so there is no percentile array.
  4. Cursors built by advancing `gend` itself, so there is no cursor copy.
  5. 5xx counted in pass 2 with a relaxed atomic add into the global array, so there is no per-thread error array (half the per-thread pages).
  6. The thread count comes from `sched_getaffinity` (the CPUs the container may use), not `_SC_NPROCESSORS_ONLN` (the host's CPUs). On the board, a cpuset on a larger machine would otherwise start too many threads and pay their memory.
- **Thread scaling (sample, median of 7 runs):**

  | CPUs | Time (ms) | Peak (KiB) |
  |---|---|---|
  | 1 | 70 | 9676 |
  | 2 | 39 | 10060 |
  | 4 | 29 | 10932 |
  | 6 | 49 | 11536 |
  | 12 | 36 | 13480 |

  So the fixed cost is about 4.4 MB above the container baseline: the binary, the three 530 KB group arrays and the 1.2 MB latency array. Each thread adds about 0.33 MB. Times at this data size are noisy; 12 threads stay, because the scan is 10x larger on the board.
- **Result:** peak memory down from 22.2 to 14.1 MB (16 CPUs) and 13.4 MB (12 CPUs). Time did not get worse (42 → 37 ms unpinned).
- **Correctness:** the sample sha256 matches, the spec's worked example is byte-identical, and an empty input gives an empty output.

### Tried and not adopted: hour bands

Pass 2 can run once per band of consecutive hours, holding only that band's latencies and writing its lines before the next band. Lines outside the band are skipped with `memchr` (`-DBANDS=N`, default 1). Measured on sample, 7 runs each, before change 5 and the affinity fix:

| BANDS | median ms | median peak KiB |
|---|---|---|
| 1 | 38 | 14740 |
| 2 | 44 | 14364 |
| 4 | 52 | 15308 |
| 8 | 69 | 15532 |

On sample the latency array is only 1.2 MB, so bands save nothing and cost about 4 ms per extra scan. On full the array is 12.4 MB, and 4 bands would save about 9 MB, which is about 1.09x on the M factor. That only pays if the 3 extra scans add under about 20% of the run time. This has not been measured at full scale, so the option stays in and defaults to 1 until full data is available to test it.

## Step 1: C rewrite

- **Problem identified:** the spec puts about 80% of the reference's time in per-line `datetime.strptime`. The rest goes to text decoding, `split`, a dict keyed by (datetime, str), and full sorts of every group.
- **Optimization:**
  - Static C binary with no dependencies.
  - 12 threads (one per online CPU), each reading its own line-aligned range with `pread` into a 256 KB buffer.
  - The hour index comes from fixed byte offsets (the span is fixed at 72 hours), so there is no date parsing.
  - Endpoints are interned in a per-thread FNV-1a open-addressing table.
  - Pass 1 counts lines and 5xx per (hour, local endpoint). The tables are merged and endpoints are ranked bytewise, so group id `hour * E + rank` is already in output order and no group sort is needed.
  - Counts are turned into per-thread write cursors. Pass 2 scatters every latency (u32) into its group's slice of one array.
  - Quickselect (median-of-3, Hoare partition) finds p99, then p95 within the p99 prefix, then p50 within the p95 prefix, with hours spread across threads.
  - Hand-rolled integer formatting, buffered `write`.
- **Speedup (local, sample):** 90x inside the container (3780 ms to 42 ms) and 9.8x by check.py wall time, which is dominated by container start-up at this size.
- **Correctness:** sha256 matches on sample. The spec's worked example (section 4) gives byte-identical output, and an empty `requests.log` gives an empty `summary.txt`.
- **Memory:** 22 MB peak on sample, of which about 5 MB is the container baseline. The rest is mostly per-thread fixed costs: the 256 KB read buffer, a 64 KB slot table, endpoint entries, touched count/error pages and stacks. That cost scales with the thread count, not the data. The latency array is only 1.2 MB on sample (12.4 MB on full).
