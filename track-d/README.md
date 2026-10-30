# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3`. No graph library.

1. **Graph.** All threads read `graph.txt` in two passes:
   - the first counts out-degrees and marks nodes that have an in-edge;
   - the second fills a compressed sparse row layout of 8-byte `{head, weight}` edges.

   Self-loops are dropped: they never shorten a path.
2. **Locality.** Node ids in the input are shuffled, so the nodes are renumbered in breadth-first order; the BFS prefetches ahead in its queue. Neighbours on the road network then sit next to each other in memory: without it, queries at full scale take about 2.7x longer on one CPU.
   - **Packed rewrite.** All threads rewrite the graph in the new numbering, each edge packed into 4 bytes: a 16-bit head delta and a 16-bit weight.
   - **Escapes.** The 0.05% of edges that do not fit go to a small escape table, sorted by edge index.
3. **Queries without a search.**
   - `s == t` answers 0.
   - A target nothing points into answers -1.
   - A source with no out-edges answers -1.
4. **Queries with a search.** Every other query is a Dijkstra search from `s` that stops when `t` is settled.
   - **Parallel.** Queries run in parallel, one thread per CPU the container may use, taking queries from a shared counter.
   - **Dial's bucket queue.** Every main-body weight is at most a few thousand, so a ring of 4,096 buckets holds every pending distance with O(1) push and pop.
   - **32-bit distances.** Each thread keeps a 32-bit distance array and resets only what a search touched.
   - **64-bit fallback.** A search that meets a weight of 4,096 or more, or would reach 2^32 − 1, is redone with a binary heap and 64-bit distances, in an array mapped only while it is needed. Answers above 4,294,967,295 stay exact.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- A cache-friendly build: partition the edges by tail before the CSR. Both passes are bound by random accesses on the shuffled ids.
- A parallel BFS for the renumbering; about 2,600 levels make a level-synchronous one costly.
- Smaller per-thread distance storage: 12 × 7 MB arrays are most of the 150 MB peak.
- Fewer source lines (379 against the reference's 43).
