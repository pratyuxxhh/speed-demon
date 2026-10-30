# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2).

1. **Index only what is asked.** `queries.txt` is read first and every query term is interned in an open-addressing hash table. Only those terms are indexed. On the sample they are 11,430 of the corpus's 378,185 distinct tokens. Each hash slot keeps the term's first 8 bytes and its length, so most lookups read one slot.
2. **One parallel scan.** `docs.txt` is cut into one line-aligned range per CPU the container may use, and each thread reads its range with `pread`.
   - **Tokenizing.** AVX2 classifies 64 bytes at a time. Tokens are the runs of set bits, found with bit tricks, and lowercasing is a `| 0x20` per 8-byte word. That works because a token byte is always a letter or a digit, exactly the spec's ASCII-only rule.
   - **Lookups.** A line's tokens are collected first and their hash slots prefetched, so the lookups' cache misses overlap. At full scale the term table does not fit in cache.
   - **Recording.** Each (document, term) pair is recorded once.
3. **Hybrid index.**
   - **Rare terms** stay sorted id lists. Their matches go to a per-thread stream and are scattered into one array afterwards. Each thread owns a contiguous run of ids, so the lists come out sorted with no sort step.
   - **Frequent terms** are promoted during the scan to a shared bitmap with one bit per document, set directly. Only the words at the edges of a thread's range need atomics.
4. **BOOL.** Strictly left to right, as the spec requires, over any mix of lists and bitmaps:
   - list with list: gallop or merge;
   - list with bitmap: bit tests;
   - bitmap with bitmap: word-wise AND, OR and AND-NOT.
5. **PHRASE, matched during the scan.**
   - Before the scan, every phrase's sequence of term ids goes into one Aho-Corasick automaton, and identical phrases share an end node.
   - During the scan, each query-term token steps it, and any other token or a new document resets it.
   - A completed phrase is indexed exactly like one more term, as a list or a bitmap.
   - A PHRASE query then just reads its finished set. Nothing is re-read from the file and no positions are stored.
6. **Output.** The count, the sum and the ten smallest ids per query, in input order.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Compressed posting lists (delta + varint), the only way under the 44 MB memory cap: the 11.4M rare-term pairs alone are 45 MB as u32.
- A cheaper phrase automaton: at full, 23M steps produce only 19k matches, because phrases start with common words.
- Shrink the per-thread stamp/count tables (8 bytes per id per thread).
- Profile-guided optimisation; trim the code (633 lines against the reference's 91).
