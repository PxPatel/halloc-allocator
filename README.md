# halloc

A dynamic memory allocator in C: a from-scratch `malloc`/`free` clone, built to understand how heap memory is managed under the hood.

Programs ask for memory at runtime, and an allocator decides where it comes from, keeps track of it, and recycles it when it's freed. This project implements that cycle on top of memory requested directly from the OS with `sbrk`.

## Status

**v1 is complete and benchmarked.** It uses an implicit free list with next-fit placement. v2 (explicit/segregated free lists) is planned.

## API

| Function | Behavior |
|---|---|
| `void *halloc(size_t n)` | Returns a pointer to at least `n` usable bytes, or `NULL` on failure. `halloc(0)` returns `NULL`. |
| `void hfree(void *p)` | Releases a block. `hfree(NULL)` is a no-op. Invalid and double frees are detected and ignored. |
| `void *hrealloc(void *p, size_t n)` | Resizes a block, in place when a free neighbor allows it, otherwise by allocating, copying, and freeing. |
| `void *hcalloc(size_t n)` | Allocates `n` bytes of zeroed memory. |
| `void hshow()` | Prints every block in the heap (index, size, allocated or free). |

## How it works

The heap is a sequence of blocks. Each block has a 4-byte **header** and a 4-byte **footer** (copies of its size and an allocated bit), so the allocator can step forward to the next block or backward to the previous one in constant time.

- **Allocate:** search for a free block that fits using **next fit**: the search resumes from where the last one stopped, not from the start of the heap. If a block is much bigger than needed, it's **split** and the remainder stays free. If nothing fits, the heap grows with `sbrk` in 4 KB chunks.
- **Free:** mark the block free, then **coalesce** it with free neighbors so fragmentation doesn't build up.
- **Realloc:** shrink in place, grow into a free neighbor if there is one, otherwise move.
- **Safety checks:** `hfree` rejects pointers outside the heap, misaligned pointers, blocks already marked free, and blocks whose header and footer disagree. Overflow in size calculations returns `NULL` with `errno = ENOMEM`.

Design based on the allocator walkthrough in *Computer Systems: A Programmer's Perspective* (CS:APP), Chapter 9.9, with my own implementation and design choices.

## Build and use

Requires a Linux environment (or WSL), `gcc`, and `make`.

```bash
gcc -Wall -Wextra -c allocator.c
```

```c
#include "allocator.h"

int main(void) {
    int *nums = halloc(10 * sizeof(int));
    for (int i = 0; i < 10; i++) {
        nums[i] = i;
    }
    hfree(nums);
    return 0;
}
```

```bash
gcc -Wall -Wextra your_program.c allocator.c -o your_program
```

## Benchmarks (v1)

`bench.c` runs the same seeded sequence of allocations against `halloc` and glibc `malloc`, so both see identical requests. Each scenario does one warmup pass and 7 timed passes, and reports the median. Each allocator runs in its own process, because both grow the heap with `sbrk` and can't share one.

```bash
gcc -O2 -Wall -Wextra -o bench bench.c allocator.c && ./bench
./bench churn     # run only scenarios whose name contains "churn"
```

Median time in milliseconds (lower is better):

| Scenario | What it does | halloc | malloc | halloc / malloc |
|---|---|---:|---:|---:|
| `free_fifo` | Allocate 100k small blocks, free oldest-first | 0.97 | 1.17 | 0.83x |
| `free_lifo` | Same, free newest-first | 0.90 | 1.22 | 0.74x |
| `churn` | Allocate and free one block, 1M times | 7.72 | 7.26 | 1.06x |
| `random_mixed` | Random sizes, random alloc/free order | 37.81 | 8.16 | **4.63x** |
| `frag_reuse` | Free every other block, then refill the holes | 2.25 | 5.11 | 0.44x |
| `frag_nofit` | Free every other block, then request blocks too big for any hole | 2.00 | 6.78 | 0.30x |
| `coalesce` | Free blocks in shuffled order, then request one large block | 1.94 | 3.65 | 0.53x |
| `realloc_growth` | Grow one buffer repeatedly with small blocks in the way | 0.17 | 0.23 | 0.76x |
| `mixed_sizes` | Mostly small, some medium, a few large blocks | 3.78 | 12.92 | 0.29x |

**Reading the results**
- `halloc` is faster than glibc on most scenarios, but that's not a fair fight. glibc does work `halloc` doesn't: thread safety, 16-byte alignment, per-thread caches, and heap integrity checks.
- `random_mixed` is the clear weakness at 4.6x slower. It's the most realistic workload, and it's where a linear search over a fragmented heap hurts. My hypothesis is that walking past allocated blocks is the bottleneck; v2 should confirm that by counting blocks visited per search.
- Numbers come from one machine, single-threaded, compiled with `-O2`. They show relative behavior, not absolute performance.
- Memory efficiency (peak heap size vs. bytes requested) is not measured yet.

## Known limitations (v1)

- **Not thread-safe.**
- **8-byte alignment** (glibc uses 16 on x86-64).
- **Assumes it's the only user of `sbrk`.** If anything else moves the program break, `halloc` can't grow its heap safely. This is why the benchmark runs each allocator in a separate process, and why `halloc` can't yet replace `malloc` inside an existing program.
- **Blocks are capped just under 4 GB** because headers are 32 bits.
- **Invalid frees are silently ignored** rather than reported.
- **`hcalloc` takes one size argument** instead of the standard `(count, size)`.
- No `mmap` path for large allocations, and memory is never returned to the OS.

## Roadmap: v2

- [ ] **Segregated free lists:** keep separate lists of free blocks by size class, so a search only visits free blocks of roughly the right size instead of walking the whole heap. This targets the `random_mixed` gap.
- [ ] **Heap checker (`check_heap`):** verify invariants (header matches footer, no two adjacent free blocks, blocks chain to the epilogue) and call it after every operation in the benchmarks.
- [ ] **Search statistics:** count blocks visited per allocation, to measure the cost of each placement policy instead of guessing.
- [ ] **`mmap`-based heap:** reserve one region at startup, so growth no longer depends on the program break.
- [ ] **Re-run the benchmarks** and add a v1 vs. v2 comparison.
- [ ] Stretch: 16-byte alignment, a standard `hcalloc(count, size)`, thread safety.

## References

- *Computer Systems: A Programmer's Perspective*, Chapter 9.9 (Dynamic Memory Allocation)
- *Operating Systems: Three Easy Pieces*, chapters on the Memory API and Free-Space Management
- Linux man pages: `brk(2)`, `mmap(2)`, `malloc(3)`