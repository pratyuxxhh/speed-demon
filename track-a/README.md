# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2).

- **Read once.** `requests.log` is memory-mapped and cut into 1 MB line-aligned chunks, which threads take from a counter, so faster cores simply take more.
- **Parse.** Each line is parsed by hand:
  - The hour comes from fixed byte offsets in the 27-byte timestamp. The spec fixes the 72-hour span, so there is no date parsing.
  - The method's length comes from its first two letters.
  - The endpoint's end is found with an AVX2 space search. The endpoint is hashed 8 bytes at a time and interned in one shared lock-free table, so there are no per-thread tables to merge.
  - A status starting with `5` sets an error bit, and the latency is read as a u32.
- **Bucket by hour.** Each line becomes a record in the thread's bucket for its hour: a 2-byte tag (endpoint id, the latency's byte length, the 5xx bit) and the latency in 1 to 4 bytes, about 4.1 bytes per line on the full data. The text is never read a second time.
- **Finish each hour in parallel.** A thread takes an hour and does the rest:
  - it counts that hour's records by endpoint across all threads;
  - it gathers the latencies, grouped by endpoint;
  - quickselect finds the nearest-rank p99, then p95 inside the p99 prefix, then p50 inside the p95 prefix;
  - it formats the hour's lines in byte order of endpoint.
- **Ordered output.** Finished hours are handed to whichever thread holds the writer lock, which writes every finished hour in order. The file gets one sequential stream, and no thread waits for another's hour.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Confirm on the board that the variable-length records' memory saving (17–18% of peak locally) outweighs their about 5% time cost there too.
- A lower-memory percentile scheme: counts per latency range first, then keep only the latencies near p50/p95/p99. That is what M at the cap (4.6 MB) would need.
- Profile-guided optimisation.
