# Design Log — Project 2

## Growth factor and amortized cost

`Conversation` doubles its capacity when full: `capacity_ == 0 ? 1 : 2 * capacity_`. I chose 2 over 1.5 because it means fewer reallocations and simpler math. The cost, up to 50% unused slots after a grow, is negligible here.

**Proof.** Take *n* appends from empty. A grow happens only when `size_ == capacity_`, at sizes 0, 1, 2, 4, …, 2^k, where 2^k < n. Growing from capacity *c* moves *c* messages, and `new Message[2c]` default-constructs 2*c* slots. Summed over all grows:

- moves: 1 + 2 + … + 2^k < 2n
- constructions: 1 + 2 + … + 2^(k+1) < 4n

Each append also does one write. Destroying the old arrays adds fewer than 2n slot destructions, keeping the total below 9n slot operations: **O(1) amortized per append**. A late System message shifts existing elements once; rejecting duplicates limits this extra work to O(n). These counts treat message operations as units; copying string contents also costs time proportional to their length. Growing by a fixed +1 instead costs O(n²).

*Test:* `GrowthDoublesAndPreservesContents` makes 5000 appends. After each one it asserts that capacity is the next power of two, and it asserts exactly 14 reallocations in total (1 → 8192). After every reallocation it re-checks every element.

## Rule of Five evidence

- **Destructor:** `delete[] data_`. This is a no-op on `nullptr` (empty or moved-from).
- **Copy constructor:** allocates its own `other.size_` slots and copies every `Message`. If a string copy throws, a `try/catch` frees the partial buffer before rethrowing.
- **Copy assignment:** copy-and-swap (`Conversation tmp(other); swap(tmp);`). All the work that can throw happens before `*this` changes, which gives the strong exception guarantee. Self-assignment is safe, and `tmp` frees the old buffer.
- **Move constructor and move assignment:** both are `noexcept`. They steal the three fields and set the source to `nullptr/0/0`. Move assignment frees its own buffer first and guards against `this == &other`.
- **`grow()`:** calls `new[]` before changing anything, then only uses `Message` move-assignment, which a `static_assert` checks is `noexcept`. If allocation fails, the object is unchanged.

*Tests:*

- `RuleOfFiveCopyIsDeep` asserts `copy.begin() != original.begin()`, that every string buffer is distinct, that the copy is independent after the original reallocates, and that self-assignment works.
- `RuleOfFiveMoveSteals` asserts that the stolen pointer is identical, that the source is `nullptr/0/0`, and that the moved-from object can be reused.

Everything runs under `-fsanitize=address,undefined`, so a shallow copy would abort with a double-free. ASan's leak checker is unsupported on Apple Silicon, so I also ran macOS `leaks --atExit` on a non-sanitized test build: **0 leaks**.

## Sentinel scanner: bounded pending_ proof

Let *m* = |*S*| = 20, where *S* is the sentinel. I refined the spec's approach slightly. Rather than always holding back *m*−1 characters, the scanner holds back the **longest suffix of the window that is a proper prefix of *S***. Clean text is therefore printed immediately.

**Claim:** after every `feed`, |`pending_`| ≤ *m* − 1.

**Proof by induction.** Initially `pending_` is empty. For `feed(chunk)`, let *w* = `pending_ + chunk`.

- If `w.find(S)` succeeds, `pending_` is cleared, so its length is 0.
- Otherwise, `pending_` becomes the last *k* characters of *w*, where *k* = `held_back_length(w)`. That function only tries *k* ≤ min(|*w*|, *m* − 1), so |`pending_`| ≤ *m* − 1.

This holds for any chunk size or prior state. ∎

**No sentinel is missed.** Suppose an occurrence of *S* starts at stream position *p* and completes during feed *t*. After any earlier feed, the text from *p* onward is a prefix of *S* of length *j* < *m*. That text is also a suffix of the window, so the longest such suffix is at least *j* long and *p* is never emitted. At feed *t* the whole occurrence is therefore inside *w*, and `find` catches it.

**Cost.** The window holds at most *m* − 1 + |chunk| characters, so temporary space is O(*m* + |chunk|); retained state is O(*m*). Concatenating the whole reply and re-searching it would cost O(N²).

*Test:* `ScannerBoundedMemoryUnderAdversarialStream` feeds 4 MB of patterns like `<|end_conversation|` and `<|end_<|end_`, one byte at a time. After every byte it asserts `pending_size() ≤ 19` and that emitted + pending = fed. It also asserts that 19 is actually reached.

## What I would change differently

`pending_` is always exactly `S.substr(0, k)`, so storing just the integer *k* would be enough. Combined with a KMP failure table, each byte would update *k* in amortized O(1) time with no string building. That would cut the stress test's 16 s runtime at -O0 under ASan. I would also allocate `Conversation` storage with `operator new` plus placement-new. `new Message[2c]` default-constructs slots that are immediately overwritten (the 4n term above).
