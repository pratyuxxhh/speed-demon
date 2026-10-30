# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | C rewrite: parallel two-pass CSR, -1 without search, Dijkstra per query in parallel | _not pushed yet_ | | | |
| 2 | Breadth-first node renumbering; touched-list growth | _not pushed yet_ | | | |

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