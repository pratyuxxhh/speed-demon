# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3 -mpclmul` (AVX2 plus carry-less multiply). No CSV library: the parser follows the spec's section 2.1, and relies on section 2.2, which the spec says may be relied on.

1. **Regions.** `regions.csv` (2 MB) is parsed on the main thread into a flat map from region id to a group index; it takes about 2 ms. Group 0 is `_UNMATCHED`. An id that `regions.csv` lacks maps to it, and so does any id of 10,000,000 or more (spec 2.2 says none occur), which reads a spare last byte that is always 0.
2. **Chunks.** `records.csv` is memory-mapped and cut into 1 MB chunks that threads take from a counter, so a slow or late thread just takes fewer. A thread maps each chunk's pages with one `MADV_POPULATE_READ` call instead of a page fault per 4 KB.
   - Spec 2.2 guarantees that a note's LF is followed by a lowercase word and that names hold no LF.
   - So an LF followed by a digit or a BOM is always a record boundary, and a chunk's first record can be found from any offset.
3. **Parsing, 64 bytes at a time.**
   - AVX2 builds masks of quotes, commas and LFs.
   - The bytes inside quotes are the prefix XOR of the quote bits, computed with one carry-less multiply; a `""` escape flips it twice, so it needs no special case.
   - The commas and LFs outside quotes end the fields, and every fifth one ends a record.
   - A quoted field's unescaped length is its raw length minus 1 minus half its quote count, so names and notes are measured without being read again.
   - Ids, region ids and amounts are read eight digits at once (SWAR). A BOM is skipped only at a record start; CRLF and LF both end a record.
4. **Aggregates.**
   - Records are queued in batches of 32. For each one, the name's record line and the region map entry are prefetched, then the batch is applied, so the cache misses overlap.
   - Each thread keeps its own count, sums, min and max per group.
5. **Distinct names, exactly.**
   - A name is identified by its bytes as written between its quotes, never unescaped. CSV escaping (`"` as `""`) is one to one and non-empty names are always quoted (spec 2.2), so two names are equal exactly when their written forms are.
   - A shared lock-free table of 1-byte tags and 64-byte records. A record holds the name's first 24 bytes and its length (compared with one 32-byte AVX2 compare), an arena offset for longer names, and the set of groups the name occurs in. The record line is read only when the tag matches.
   - Names are compared as exact bytes, never by hash alone. `names_distinct` is counted from the group sets at the end, in parallel.
6. **Memory.** Transparent huge pages are turned off for the process (`PR_SET_THP_DISABLE`): with THP set to `always`, the kernel backs the name table, the region map and the thread stacks with 2 MB pages that are charged in full.
7. **Output.** Keys are sorted bytewise (`_UNMATCHED` last) and money is printed from integer cents.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- A denser name store (a 4-byte slot table plus per-name arrays), about 2.5 MB less, if the board confirms that memory is now the program's own and not huge pages.
- One thread per physical core: a board A/B, since this laptop's mix of P- and E-cores cannot predict it.
- Profile-guided optimisation.
