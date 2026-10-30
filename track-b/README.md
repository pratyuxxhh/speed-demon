# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

A single C file, `main.c`, built into a static binary with `gcc -O3 -march=x86-64-v3` (AVX2).

1. **Index only what is asked.** `queries.txt` is read first and every query term is interned in an open-addressing hash table. Only those terms are indexed. On the sample they are 11,430 of the corpus's 378,185 distinct tokens. Each hash slot keeps the term's first 8 bytes and its length, so most lookups read one slot.
2. **One parallel scan.** `docs.txt` is cut into one line-aligned range per CPU the container may use, and each thread reads its range with `pread`.
   - **Tokenizing.** AVX2 classifies 64 bytes at a time. Tokens are the runs of set bits, found with bit tricks, and lowercasing is a `| 0x20` per 8-byte word. That works because a token byte is always a letter or a digit, exactly the spec's ASCII-only rule.
   - **Recording.** Each (document, term) pair is recorded once.
3. **Hybrid index.**
   - **Rare terms** stay sorted id lists. Their matches go to a per-thread stream and are scattered into one array afterwards. Each thread owns a contiguous run of ids, so the lists come out sorted with no sort step.
   - **Frequent terms** are promoted during the scan to a shared bitmap with one bit per document, set directly. Only the words at the edges of a thread's range need atomics.
4. **BOOL.** Strictly left to right, as the spec requires, over any mix of lists and bitmaps:
   - list with list: gallop or merge;
   - list with bitmap: bit tests;
   - bitmap with bitmap: word-wise AND, OR and AND-NOT.
5. **PHRASE.**
   - Take the rarest term's documents and keep those that contain every term.
   - Confirm each by reading its text back from the file and matching the exact token sequence. No positions are stored.
   - Phrases whose terms are all frequent, such as `"the the"`, would have nearly every document as a candidate. All of them are instead matched together in one extra scan of the corpus.
6. **Output.** The count, the sum and the ten smallest ids per query, in input order.

## Dependencies

None. The C standard library and pthreads only.

## With another 24 hours

- Measure on the full dataset, and tune the promotion and shared-scan thresholds with real numbers.
- Delta-encode the rare-term lists, and shrink the per-thread stamp/count tables (8 bytes per term per thread).
- Profile why the scan scales only about 4x from 1 to 12 threads on this machine.
- Profile-guided optimisation; trim the code (628 lines against the reference's 91).
