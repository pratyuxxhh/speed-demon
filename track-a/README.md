# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2).

- **Split.** The file is cut into one line-aligned range per CPU the container may use. Each thread reads its range with `pread` into a 64 KB buffer.
- **Parse.** Each line is parsed by hand:
  - The hour comes from fixed byte offsets in the 27-byte timestamp. The spec fixes the 72-hour span, so there is no date parsing.
  - The method length comes from its first two letters; the endpoint end is found with an AVX2 space search, hashed 8 bytes at a time and interned in a per-thread open-addressing table.
  - A status starting with `5` counts as an error, and the latency is read as a u32.
- **Pass 1** counts lines per (hour, local endpoint id).
- **Merge.** The per-thread endpoint tables are merged and the endpoints are sorted bytewise. Group id `hour * E + rank` is then already in output order.
- **Cursors.** The per-thread counts become per-thread write cursors into one array, with one contiguous slice per group.
- **Pass 2** parses again. It scatters each latency into its group's slice and counts 5xx with an atomic add.
- **Percentiles.** Quickselect finds the nearest-rank p99, then p95 inside the p99 prefix, then p50 inside the p95 prefix. Groups are spread over threads by hour.
- **Output.** Lines are formatted with hand-written integer printing into a 64 KB buffer.

Memory is about 4 bytes per input line plus small fixed tables.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Measure on the full dataset and decide on hour bands with real numbers.
- SIMD (SSE2/AVX2) scanning for the endpoint and line ends.
- A compact 16-bit latency encoding for the common small values, to shrink the latency array.
- Profile-guided optimisation.
