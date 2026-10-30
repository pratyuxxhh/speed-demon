# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | C rewrite: parallel two-pass CSR, -1 without search, Dijkstra per query in parallel | _not pushed yet_ | | | |
| 2 | Breadth-first node renumbering; touched-list growth | _not pushed yet_ | | | |
| 3 | Parallel renumbering rewrite, BFS prefetch | _not pushed yet_ | | | |
| 4 | Dial bucket queue (ring of 4096), binary-heap 64-bit fallback | _not pushed yet_ | | | |
| 5 | 64-bit fallback array mapped on demand and released | _not pushed yet_ | | | |
| 6 | Edges packed into 4 bytes (16-bit head delta, 16-bit weight) with an escape table | _not pushed yet_ | | | |

The board columns are filled from board runs only. Until a push is measured, the local numbers below are what we have.

## Local measurements (sample scale, seed 1)

Machine: WSL2 Ubuntu on Windows 11. The CPU is an Intel i5-13500H laptop chip: 4 P-cores with hyper-threading plus 8 E-cores, under a laptop power limit, with 6 GB RAM for the VM and Docker 29.1.3. Runs are pinned with `--cpuset-cpus`. `0-11` matches the board's thread count, but it mixes in E-cores, so the 1-CPU time is the steadiest measure of the code itself.

Data: `speed-demon-2026/data/d`: 174,051 nodes, 500,000 edges, 200 queries. `graph.txt` sha256 `16351d40…c9b7cd` and `queries.txt` sha256 `fb4065ff…88a5`, both as published. The expected `distances.txt` sha256 is `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0`: the spec's published sample hash, and what the unchanged reference produced here.

- **inside** is the program alone, timed by the organisers' `harness/timer/sd-timer.c` (CLOCK_MONOTONIC, exec to exit). It was built statically with the allowlisted gcc:15 image and run as the container's timer, as on the board.
- **peak** is the container cgroup's `memory.peak`. An empty container already peaks at about 5.3 MB here.

| Step | CPUs | check.py runs (s) | inside median (min–max), ms | peak median (KiB) | sha256 |
|---|---|---|---|---|---|
| 0 reference, session 1 | 0-11 | 11.89, 11.23, 11.19 | 11106 (10371–11732), 5 runs | 89552 | match |
| 0 reference, session 2 | 0-11 | | 10873 (10536–10903), 3 runs | 89512 | match |
| 1 | 0 | | 465 (397–540) | 8404 | match |
| 1 | 0-7 | | 103 (99–110) | 20176 | match |
| 1 | 0-11 | 0.47, 0.45, 0.50 | 109 (96–141) | 25292 | match |
| 0 reference, session 3 | 0-11 | | 11637 (11102–11819), 3 runs | 89604 | match |
| 2 | 0 | | 326 (306–445) | 11824 | match |
| 2 | 0-7 | | 99 (92–106) | 22084 | match |
| 2 | 0-11 | 0.48, 0.41, 0.36 | 112 (93–127) | 28372 | match |

Steps 1 and 2 have 7 runs per row.

### Local K estimate (sample, 12 CPUs, not the board's number)

`K = (T_ref/T)^0.5 × min(M_ref/M, 50)^0.2 × min(S_ref/S, 1)^0.15 × (D_ref/D)^0.15`, with S_ref = 43 (`harness/reference_axes.json`) and S counted as non-blank, non-comment lines. The board counts after clang-format.

| Step | T ratio | M ratio | S | D | K | points |
|---|---|---|---|---|---|---|
| 2 | 11637/112 = 103.9 → 10.19 | 89.6/28.4 = 3.16 → 1.26 | 274 → 0.757 | 1 → 1 | 9.7 | 49 |

The memory ratio on the sample understates the board's. The reference's peak grows with the graph, from 89.6 MB on the sample to 893 MB on the board's full set, and this program's graph is about 8 bytes per edge plus 4 per node.

## Step 1: C rewrite

- **Problem identified:**
  - The reference stores the graph as Python lists of `(head, weight)` tuples, and runs a fresh single-threaded `heapq` Dijkstra with a `dict` of distances for every query (spec section 7).
  - The two "target has no in-edges" queries at full each search the whole 1.74M-node main body, about 3 s each on the reference, just to answer -1.
- **Optimization:**
  1. **Graph build, in parallel, two passes over `graph.txt`.** Pass 1 counts each tail's out-degree with relaxed atomic adds and marks which nodes have an in-edge. Pass 2 stores each edge at an atomically advanced cursor in a CSR: `uint32` offsets and 8-byte `{head, weight}` edges. No intermediate edge list is kept.
  2. **Self-loops are dropped:** they never shorten a path. Parallel edges are kept, because Dijkstra relaxes both and the lighter wins; deduplicating would need a sort for about 0.1% of the edges.
  3. **Answered without a search:**
     - `s == t` is 0;
     - a target with no in-edges (self-loops excluded) is -1;
     - a source with no out-edges is -1.
  4. **Queries in parallel:** threads take queries one at a time from an atomic counter. Each runs Dijkstra from `s` with a binary heap and lazy deletion, and stops when `t` is popped.
  5. **32-bit distances with a 64-bit fallback.** Each thread keeps a 32-bit distance array, initialised once, and resets only the nodes a search touched. A search whose tentative distance would reach 2^32 − 1 stops and is redone with a lazily allocated 64-bit array. Only the queries that cross near-2^32 edges (no-in sources, islands' spurs) pay for it.
- **Phases (sample, timers in a throwaway copy):**

  | CPUs | pass 1 | pass 2 | queries |
  |---|---|---|---|
  | 1 | 20 ms | 25 ms | 451 ms |
  | 12 | 7–11 ms | 8–19 ms | 56–82 ms |

  The queries are 85–90% of the run.

## Step 2: breadth-first renumbering

- **Problem identified:** node ids are a random relabelling (spec section 2), so neighbouring nodes sit far apart in the offset, edge and distance arrays, and almost every edge a search follows is a cache miss.
- **Optimization:** after the CSR is built, the nodes are renumbered in breadth-first order: a BFS from each not-yet-numbered node in id order, which covers the strongly connected main body in one sweep. The CSR, the in-edge marks and the queries are then rewritten in the new numbering. Neighbours on the road lattice now get nearby ids. Each thread's touched list also starts at 64K entries and grows, instead of being n entries.
- **Result:**

  | Phase | before | after |
  |---|---|---|
  | queries, 1 CPU | 451 ms | 300–307 ms |
  | queries, 12 CPUs | 56–82 ms | 42–58 ms |
  | the renumbering itself (serial) | | 13–17 ms |

  End to end: 1 CPU 465 → 326 ms. At 12 CPUs it is a wash (109 → 112 ms medians), because this laptop's 12-thread runs are limited by E-cores and the power limit. At 200 queries a few long searches (Dijkstra rank up to 131,071) also decide when the last thread finishes.
- **Memory:** the peak rose from 25.3 to 28.4 MB, because the renumbering briefly holds two copies of the CSR.
- **Correctness (both steps):**
  - The sample sha256 matches on 1 and 12 CPUs.
  - The spec's worked example is byte-identical. It covers parallel edges listed heavier-first, weight-0 paths, a self-loop, a node with no edges, a no-in target, a no-out source, a sum above 2^32 − 1 and a duplicate query.
  - **Other seeds, against the reference:** tiny seeds 1–5 and sample seed 2 were generated with the organisers' generator, and our output matched the unchanged reference's on 1 and 12 CPUs.
  - All of the above was run with a `-fsanitize=address,undefined` build with `-Wall -Wextra`: no reports, no warnings.

## What full scale changes, and what has not been measured

The sample is a tenth of the graph, and full scale behaves differently: the graph is 47 MB, and each thread's 32-bit distance array is 7 MB, which is 84 MB at 12 threads. Neither fits in cache. Estimated full-scale peak: about 130 MB in the query phase, and about 110 MB during the renumbering. These are estimates; the full graph has not been run locally.
That estimate was too low: measured at full scale (below), step 2 peaked at 238 MB with 12 threads.

## Full scale (seed 1, generated locally)

Generated with the organisers' generator (`--seed 1 --scale full`) into `~/sd-data/d-full`, outside the repos. The inputs match the spec: `graph.txt` sha256 `fe6471f2712d10b23c2ec58a4cbc7db1ea3e742daf6b6db62829e74d6c4a26c7` and `queries.txt` `160f1ef48378dcb567a8d9ddd2676ee777b044a5ede3ad4c7b42e812aa4b3a6f`. The unchanged reference ran once at 0-11: **218,050 ms, 881,356 KiB peak**. Its output, `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e` (the spec's published full hash), is kept, and every full-scale run below was compared with it. All of them matched.

The board's calibration is 72.69 s, so this machine runs the reference about 3x slower than the scoring box. Comparisons below are A/B in the same session, 7 runs per configuration, medians (min–max); the machine's speed drifts between sessions.

### Step 0: what the data and the program say

From the generator's full output, with a one-off Python pass:

1. **Queries:** 2,000 lines with 1,979 distinct sources. Only 21 sources repeat, and they are the 20 duplicate lines. One search per distinct source would save about 1%, so it was not built.
2. **Edge order:** fully shuffled. 4,999,995 tail changes in 5,000,000 lines, so not grouped by tail. Numbering nodes by their first appearance in the file gives little locality: the median |tail − head| is 328,653 (510,104 with the input ids), and only 23.2% of edges are within 2^15 (3.7% with the input ids).
3. **Weights:**
   - the largest weight below 2^20 is 3,466;
   - 50 edges are at or above 2^20, all at least 4,293,948,136 (the spec's near-2^32 spurs and no-in sources);
   - 99.999% of weights fit in 16 bits; 2,533 are zero.
4. **Searches that answer -1:** only 4 search at all. Each settles 22–24 nodes (island sources). No -1 query searches the main body, so a union-find rejection would remove nothing, and it was not built.
5. **Baseline (step 2, phase timers in a throwaway copy, 12 CPUs):**

   | Phase | Time | Peak RSS after it |
   |---|---|---|
   | pass 1 | 114 ms | |
   | pass 2 | 130 ms | |
   | **renumbering** | **1,067 ms** | 106 MB |
   | queries | 906 ms | 242 MB |
   | output | 0.6 ms | |

   - Queries took 10.1 s of CPU in total and settled 23.2M nodes.
   - The five longest searches settled 129,001–130,418 nodes in 34–50 ms each.
   - Every thread touched 1,686–1,700 of the 1,700 pages of its 7 MB distance array.

   Step 2 at full scale:

   | CPUs | Time | Peak |
   |---|---|---|
   | 1 | 6777 ms (5908–7175) | 110 MB |
   | 0-7 | 1559 ms (1466–1600) | 180 MB |
   | 0-11 | 1379 ms (1315–1515) | 238 MB |

**Renumbering at all?** The plan expected renumbering to be optional. Measured, it pays:

| CPUs | renumbered | not renumbered |
|---|---|---|
| 1 | 6299 ms | 16840 ms |
| 0-11 | 1502 ms | 1937 ms |

So it stays, and the work went into making it cheaper.

### Step 3: parallel renumbering rewrite, BFS prefetch

- **Problem identified:** inside the 1,067 ms of renumbering, the serial BFS took 222–404 ms and the serial rewrite of the CSR in the new order took 221–393 ms. Both are random reads in the shuffled input numbering.
- **Optimization:**
  - The new CSR is built in parallel: each thread fills the degrees of its share of the new ids, a serial prefix sum gives the offsets, and each thread copies and remaps its share's edges and in-edge marks.
  - The BFS prefetches 16 queue entries ahead for offsets, 8 ahead for edges, and 4 ahead for the visited marks of the heads. Same order, so the output is unchanged.
- **Result** (A/B, full):

  | CPUs | step 2 | step 3 |
  |---|---|---|
  | 1 | 6209 ms (6077–6495), 109.8 MB | 6048 ms (6006–6304), 109.8 MB |
  | 0-7 | 1642 ms (1367–1871), 193.3 MB | 1315 ms (1195–1442), 192.9 MB |
  | 0-11 | 1566 ms (1405–1689), 224.7 MB | 1129 ms (964–1204), 237.6 MB |

### Step 4: Dial bucket queue

- **Problem identified:** queries were 906 ms of the run. Step 0 showed every main-body weight is at most 3,466, so a bucket queue with a ring of 4,096 buckets holds every pending distance, one distance per bucket.
- **Optimization:**
  - `dial()`: a ring of 4,096 growable buckets, lazy deletion (a popped node whose stored distance differs is stale), and weight-0 edges appending to the current bucket. It stops when t is popped.
  - A search that meets a weight of 4,096 or more, or a sum reaching 2^32 − 1, returns -2. It is redone by the binary-heap search with 64-bit distances. Only the special no-in, island and spur queries do; the worked example and every generated dataset contain such edges, so the fallback runs in the suite.
- **Result** (A/B, full):

  | CPUs | step 3 | step 4 |
  |---|---|---|
  | 1 | 6546 ms (6439–6914), 109.8 MB | 3470 ms (3445–3609), 109.9 MB |
  | 0-7 | 1324 ms (1211–1369), 193.0 MB | 810 ms (749–949), 201.7 MB |
  | 0-11 | 1131 ms (1060–1201), 237.8 MB | 756 ms (681–883), 250.7 MB |

### Step 5: the 64-bit fallback array, on demand

- **Problem identified:** a thread that ran one fallback search kept a 14 MB 64-bit distance array, fully written by `memset`, for the rest of the run. The 32-bit arrays were fully touched by every thread (step 0).
- **Tried first, not adopted: every distance array on demand.** Arrays mapped with `MAP_NORESERVE`, holding complemented distances (0 = unreached), returned with `madvise(MADV_DONTNEED)` after every search, and no touched list:

  | CPUs | step 4 | all on demand |
  |---|---|---|
  | 1 | 3350 ms | 4099 ms |
  | 0-7 | 766 ms / 214.9 MB | 1199 ms / 110.8 MB |
  | 0-11 | 737 ms / 251.0 MB | 1159 ms / 111.5 MB |

  Re-faulting and zeroing each search's pages cost more than the memory saved: K × √(737/1159) × (251/111.5)^0.2 ≈ 0.94.
- **Optimization kept:**
  - Only the fallback's 64-bit array is mapped on demand, complemented and returned after each use; it serves about six searches.
  - The 32-bit array keeps `malloc`, `memset` and the touched-list reset.
  - The narrow binary-heap search became redundant (a Dial failure goes straight to the 64-bit search) and was removed.
- **Result** (A/B, full):

  | CPUs | step 4 | step 5 |
  |---|---|---|
  | 1 | 3500 ms (3465–3631), 109.9 MB | 3539 ms (3203–3889), 109.9 MB |
  | 0-7 | 824 ms (774–1052), 201.5 MB | 780 ms (737–932), 133.8 MB |
  | 0-11 | 733 ms (702–878), 237.4 MB | 780 ms (736–803), 169.4 MB |

  The 12-CPU time difference is within this session's spread; (733/780)^0.5 × (237/169)^0.2 ≈ 1.04.

### Step 6: 4-byte edges

- **Problem identified:** 8-byte edges are 40 MB of the graph, and two CSR copies coexist during renumbering. Measured in BFS order: 4,996,796 of 4,999,492 edges (99.946%) have a head delta within ±32,767 and a weight below 65,535.
- **Optimization:**
  - The parallel rewrite writes each edge as `int16 delta << 16 | uint16 weight`, directly in the new numbering.
  - An edge that does not fit is written as the marker 0xFFFF and kept in an escape table `{edge index, head, weight}`. That table is sorted by construction (each thread's share is in order) and looked up by binary search; 2,696 entries at full.
  - The old 8-byte CSR is freed after the rewrite.
- **Forced path:** a throwaway copy that escapes every edge gave identical output on the worked example, the sample, tiny seeds 1–2 and sample seed 2.
- **Result** (A/B, full):

  | CPUs | step 5 | step 6 |
  |---|---|---|
  | 1 | 3763 ms (3393–4163), 109.9 MB | 3635 ms (3438–3806), 90.4 MB |
  | 0-7 | 814 ms (725–875), 133.8 MB | 794 ms (716–877), 114.1 MB |
  | 0-11 | 774 ms (723–839), 169.3 MB | 739 ms (697–808), 150.0 MB |

  UBSan flagged a `memcpy` from a NULL pointer of a thread with no escapes (length 0), which is fixed.

### Tried and not adopted

- **Prefetching in `dial()`** (offsets 16 ahead, packed edges 8 ahead, and the distance entries of the heads of the node 4 ahead in the bucket):

  | CPUs | step 6 | with prefetch |
  |---|---|---|
  | 1 | 3363 ms | 3331 ms |
  | 0-7 | 735 ms | 792 ms |
  | 0-11 | 738 ms | 724 ms |

  Peak memory was the same, and every difference is within the spread. Reverted.
- **Pass 1 skipping the weight's digits** (`memchr` to the line end):

  | CPUs | step 6 | skipping |
  |---|---|---|
  | 1 | 3612 ms | 3759 ms |
  | 0-7 | 840 ms | 839 ms |
  | 0-11 | 791 ms | 847 ms |

  The pass's cost is its random `off[u+1]` increments and `has_in[v]` stores on shuffled ids, not the digits. Reverted.
- **Not built:**
  - one search per distinct source (step 0: about 1% of searches);
  - union-find rejection (step 0: no -1 search covers the main body);
  - ALT and contraction hierarchies, as instructed.

  Two steps in a row failed, so the work stopped here, as planned.

### Final, A/B against step 2 (same session, full)

| CPUs | step 2 | step 6 (final) |
|---|---|---|
| 1 | 6843 ms (6728–6963), 109.8 MB | 3662 ms (3566–3857), 90.4 MB |
| 0-7 | 1739 ms (1615–1838), 193.3 MB | 787 ms (740–846), 114.3 MB |
| 0-11 | 1583 ms (1481–1634), 238.3 MB | 871 ms (789–908), 150.0 MB |

This session was slower than the earlier ones for both (step 6 measured 739 ms and step 2 1379 ms at 0-11 elsewhere).

### Local K estimates (full)

S is non-blank, non-comment lines; the clang-format tools image could not be built offline here.

| Step | Session | T ratio | M ratio | S | K | points |
|---|---|---|---|---|---|---|
| 2 | final A/B | 218050/1583 = 137.7 → 11.74 | 881.4/238.3 = 3.70 → 1.30 | 274 → 0.757 | 11.5 | 53 |
| 6 | final A/B | 218050/871 = 250.3 → 15.82 | 881.4/150.0 = 5.88 → 1.43 | 379 → 0.722 | 16.3 | 61 |
| 6 | step-6 A/B | 218050/739 = 295.1 → 17.18 | 881.4/150.0 = 5.88 → 1.43 | 379 → 0.722 | 17.7 | 62 |

**What going further would take,** measured at 12 CPUs after step 6. The phase times sum to about 690 ms, against the program's 739–871 ms measured whole:

| Phase | Time |
|---|---|
| queries | 296–305 ms |
| pass 1 | 62–110 ms |
| pass 2 | 125–130 ms |
| BFS | 117–137 ms (serial) |
| rewrite | 69–123 ms |

- **The build** is random-access-bound. A cache-friendly partition of the edges by tail before the CSR would remove most of the misses, at about 60 MB of transient memory (below the query-phase peak).
- **The serial BFS** has about 2,600 levels on this lattice, so a level-synchronous parallel BFS pays a barrier per level.
- **Memory** is dominated by 12 per-thread 7 MB distance arrays (84 MB) plus the 27 MB packed graph. The memory cap of 50x (17.9 MB) is out of reach with per-thread arrays.
