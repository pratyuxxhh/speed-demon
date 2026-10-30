# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3`. No graph library.

1. **Graph.** All threads read `graph.txt` in two passes:
   - the first counts out-degrees and marks nodes that have an in-edge;
   - the second fills a compressed sparse row layout of 8-byte `{head, weight}` edges.

   Self-loops are dropped: they never shorten a path.
2. **Locality.** Node ids in the input are shuffled, so the nodes are renumbered in breadth-first order and the graph is rewritten. Neighbours on the road network then sit next to each other in memory.
3. **Queries without a search.**
   - `s == t` answers 0.
   - A target nothing points into answers -1.
   - A source with no out-edges answers -1.
4. **Queries with a search.** Every other query is a Dijkstra search from `s` that stops when `t` is settled, using a binary heap. Queries run in parallel, one thread per CPU the container may use, taking queries from a shared counter. Each thread keeps a 32-bit distance array and resets only what a search touched. A search whose distance would reach 2^32 − 1 is redone with 64-bit distances, so answers above 4,294,967,295 stay exact.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Measure on the full graph: time, peak memory, and the thread count that pays for its 7 MB distance array.
- Bidirectional Dijkstra, with a reverse graph, to settle fewer nodes per query.
- Smaller per-thread distance storage at full scale (a hash map sized to the search), and renumbering without a second copy of the graph.
- An 8-byte heap entry (32-bit key and node) for the common 32-bit case.
