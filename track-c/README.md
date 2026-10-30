# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2). No CSV library: the parser is the spec's section 2.1, written out.

1. **Regions.** `regions.csv` becomes a flat array from region id to a group index. Group 0 is `_UNMATCHED`, which is also what a missing id or an id above the table maps to.
2. **An exact parallel split.** Notes contain LFs inside quotes, but spec section 2.2, which may be relied on, guarantees that a note's LF is followed by a lowercase word and that names hold no LF. So an LF followed by a digit or a BOM is always a record boundary, and each thread starts at the first one in its share of the file.
3. **Parsing.** Each thread parses whole records in its slice:
   - a BOM is skipped only at a record start;
   - quoted fields end at a `"` that is not part of a `""` pair;
   - CRLF and LF both end a record;
   - amounts go straight to integer cents.
4. **Aggregates.**
   - Notes are only measured: their unescaped length is the raw length minus the `""` pairs.
   - Names are interned in one shared lock-free table. Each distinct name owns a 64-byte record, one cache line, holding its first 24 bytes and the set of groups it occurs in. A lookup does the exact byte comparison and sets the group bit on that one line, and `names_distinct` is counted from those sets at the end.
   - Each thread keeps its own count, sums, min and max per group, merged at the end.
5. **Output.** Keys are sorted bytewise (`_UNMATCHED` last) and money is printed from integer cents.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Measure on the full dataset.
- Faster field scanning: an AVX2 structural mask for quotes, commas and LFs per 64-byte block, instead of `memchr` per field.
- Per-owner (group, name) buckets instead of atomic group bits: measured 15% faster here but about 13 MB more at full. Worth re-checking on the scoring box.
- Profile-guided optimisation.
