# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2). No CSV library: the parser is the spec's section 2.1, written out.

1. **Regions, in parallel.**
   - Every thread parses a slice of `regions.csv` into a flat map from region id to a group index. Group 0 is `_UNMATCHED`, which is also what a missing id or an id above the table maps to.
   - A country's group is claimed once, through a slot per 3-letter code.
2. **Chunks.** `records.csv` is memory-mapped and cut into 1 MB chunks that threads take from a counter, so a slow or late thread just takes fewer.
   - Spec section 2.2, which may be relied on, guarantees that a note's LF is followed by a lowercase word and that names hold no LF.
   - So an LF followed by a digit or a BOM is always a record boundary, and a chunk's first record can be found from any offset.
3. **Parsing, 64 bytes at a time.**
   - AVX2 builds masks of quotes, commas and LFs.
   - A prefix XOR of the quote bits marks which bytes are inside quotes; a `""` escape flips it twice, so it needs no special case.
   - The commas and LFs outside quotes are the field ends. A quoted field's unescaped length is its raw length minus 1 minus half its quote count, so notes are measured without being read again.
   - A BOM is skipped only at a record start, CRLF and LF both end a record, and amounts go straight to integer cents.
4. **Aggregates.**
   - Records are queued in batches of 32. For each one, the name's table slot and the region map entry are prefetched, then the batch is applied, so the cache misses overlap.
   - Each thread keeps its own count, sums, min and max per group.
5. **Distinct names, exactly.**
   - A shared lock-free table of 1-byte tags and 64-byte records. A record holds the name's first 24 bytes and its length (compared with one 32-byte AVX2 compare), plus the set of groups the name occurs in.
   - Names are compared as exact bytes, never by hash alone. `names_distinct` is counted from the group sets at the end, in parallel.
6. **Output.** Keys are sorted bytewise (`_UNMATCHED` last) and money is printed from integer cents.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Under the 7.3 MB memory cap: about 6 MB of the peak is above the container baseline. The name records (8 MB of slots for about 100,000 names) are the largest part; 32-byte records or a denser table would help.
- SWAR number parsing for ids, region ids and amounts.
- Profile-guided optimisation.
