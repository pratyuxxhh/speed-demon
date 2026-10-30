# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2). No CSV library: the parser is the spec's section 2.1, written out.

1. **Regions.** `regions.csv` becomes a flat array from region id to a group index. Group 0 is `_UNMATCHED`, which is also what a missing id or an id above the table maps to.
2. **An exact parallel split.** Notes contain LFs inside quotes, so a thread starting mid-file cannot tell a record end from a line inside a note on its own. But every `"` byte flips the quote state (a `""` escape flips it twice).
   - Pass 1 counts the quotes in each thread's slice with AVX2.
   - The running parity says whether each slice starts inside a quoted field.
   - From there, the first LF outside quotes is a true record boundary.
3. **Parsing.** Each thread parses whole records in its slice:
   - a BOM is skipped only at a record start;
   - quoted fields end at a `"` that is not part of a `""` pair;
   - CRLF and LF both end a record;
   - amounts go straight to integer cents.
4. **Aggregates.**
   - Notes are only measured: their unescaped length is the raw length minus the `""` pairs.
   - Names are interned in one shared lock-free table, and each group has a bitset of name ids. `names_distinct` is a popcount, and names are compared as exact bytes.
   - Each thread keeps its own count, sums, min and max per group, merged at the end.
5. **Output.** Keys are sorted bytewise (`_UNMATCHED` last) and money is printed from integer cents.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Measure on the full dataset.
- Trim memory further: the per-group name bitsets (about 3 MB) and the name table (2 MB) are what is left.
- Fold the quote count into the parse pass to read the file once.
- Profile-guided optimisation.
