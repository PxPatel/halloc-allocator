#include <stdlib.h>
#include <stdio.h>
#include "allocator.h"
#include <unistd.h>
#include <errno.h>

// Constants and macros
#define WSIZE 4
#define DSIZE 8
#define CHUNKSIZE (1 << 12)
#define ALIGNMENT 8

#define MAX(x, y) ((x) > (y) ? (x) : (y))

#define PACK(size, allocated) ((size) | (allocated))

#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = val)

#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

#define NEXT_BLOCK(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLOCK(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

// Global variable
static void *heap_listp;
static void *mem_cursorp;
static void *epilogue_blockp;

static bool add_checked(size_t a, size_t b, size_t *out)
{
    if (__builtin_add_overflow(a, b, out))
    {
        errno = ENOMEM;
        return false;
    }
    return true;
}
static bool mult_checked(size_t a, size_t b, size_t *out)
{
    if (__builtin_mul_overflow(a, b, out))
    {
        errno = ENOMEM;
        return false;
    }
    return true;
}

static size_t block_padding(size_t bytes)
{
    return (bytes % ALIGNMENT ? ALIGNMENT - (bytes % ALIGNMENT) : 0);
}

static void *coalesce(void *bp)
{
    void *prev_block = PREV_BLOCK(bp);
    void *prev_block_hdr = HDRP(prev_block);
    bool prev_block_free = (prev_block != heap_listp) && !GET_ALLOC(prev_block_hdr);

    void *next_block = NEXT_BLOCK(bp);
    void *next_block_hdr = HDRP(next_block);
    bool next_block_free = !(GET_SIZE(next_block_hdr) == 0 && GET_ALLOC(next_block_hdr)) && !GET_ALLOC(next_block_hdr);

    size_t new_total = GET_SIZE(HDRP(bp)) +
                       (prev_block_free ? GET_SIZE(prev_block_hdr) : 0) +
                       (next_block_free ? GET_SIZE(next_block_hdr) : 0);
    unsigned int new_header = PACK(new_total, 0);

    void *curr_hdr = HDRP(bp);
    void *curr_ftr = FTRP(bp);

    PUT(prev_block_free ? prev_block_hdr : curr_hdr, new_header);
    PUT(next_block_free ? FTRP(next_block) : curr_ftr, new_header);

    if (mem_cursorp >= (prev_block_free ? prev_block : bp) && mem_cursorp <= (next_block_free ? FTRP(next_block) : next_block))
    {
        mem_cursorp = next_block_free ? bp : mem_cursorp;
        mem_cursorp = prev_block_free ? prev_block : mem_cursorp;
    }
    return prev_block_free ? prev_block : bp;
}

static void *mem_extend_heap(size_t words)
{
    void *bp;
    size_t size;

    size_t bytes = WSIZE * words;
    size = bytes + block_padding(bytes);
    if ((long)(bp = sbrk(size)) == -1)
    {
        return NULL;
    }

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(NEXT_BLOCK(bp)), PACK(0, 1));

    epilogue_blockp += size;

    return coalesce(bp);
}

static void *placement(size_t r_size)
{
    void *startp = mem_cursorp;

    do
    {
        void *block_hdrp = HDRP(mem_cursorp);
        if (GET_SIZE(block_hdrp) == 0 && GET_ALLOC(block_hdrp))
        {
            mem_cursorp = heap_listp;
            continue;
        }

        mem_cursorp = NEXT_BLOCK(mem_cursorp);

        if (!GET_ALLOC(block_hdrp) && GET_SIZE(block_hdrp) >= r_size)
        {
            return block_hdrp + WSIZE;
        }
    } while (mem_cursorp != startp);

    return NULL;
}

static void *split(void *bp, size_t r_size)
{
    void *block_hdr = HDRP(bp);

    // Size of block before splitting
    size_t b_size = GET_SIZE(block_hdr);

    // Update the newly allocated block
    PUT(block_hdr, PACK(r_size, 1));
    PUT(FTRP(bp), PACK(r_size, 1));

    // Relabel the excess block if excess
    size_t excess_size = b_size - r_size;
    if (excess_size > 0)
    {
        PUT(HDRP(NEXT_BLOCK(bp)), PACK(excess_size, 0));
        PUT(FTRP(NEXT_BLOCK(bp)), PACK(excess_size, 0));
    }

    return bp;
}

// Functions
int mem_init(void)
{

    if ((heap_listp = (sbrk(4 * WSIZE))) == (void *)-1)
    {
        return -1;
    }

    PUT(heap_listp, 0);
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));
    heap_listp += (2 * WSIZE);
    epilogue_blockp = heap_listp + WSIZE;

    // Call mem_extend_heap to scope in CHUNKSIZE
    if ((mem_cursorp = mem_extend_heap(CHUNKSIZE / WSIZE)) == NULL)
    {
        return -1;
    }
    return 0;
}

void *halloc(size_t n)
{
    if (n == 0)
    {
        return NULL;
    }

    if (heap_listp == (void *)0 && epilogue_blockp == (void *)0)
    {
        mem_init();
    }

    // Block size = payload + header + footer + padding
    size_t unaligned_size;
    if (!add_checked(n, DSIZE, &unaligned_size))
        return NULL;

    size_t r_size;
    if (!add_checked(unaligned_size, block_padding(unaligned_size), &r_size))
        return NULL;

    void *blockp;
    if ((blockp = placement(r_size)) == NULL)
    {
        size_t multipler;
        if (!add_checked((r_size / CHUNKSIZE), (r_size % CHUNKSIZE ? 1 : 0), &multipler))
            return NULL;

        size_t extension_size;
        if (!mult_checked(CHUNKSIZE, multipler, &extension_size))
            return NULL;

        if ((blockp = mem_extend_heap(extension_size / WSIZE)) == NULL)
        {
            return NULL;
        }
    }

    void *bp = split(blockp, r_size);
    mem_cursorp = NEXT_BLOCK(bp);
    return bp;
}

void hfree(void *p)
{
    if (p == NULL)
    {
        return;
    }

    if (p < heap_listp || p >= epilogue_blockp)
    {
        return;
    }

    if ((size_t)p % 8 != 0)
    {
        return;
    }

    void *hdr = HDRP(p);
    if (!GET_ALLOC(hdr))
    {
        return;
    }

    if (*(int *)hdr != *(int *)(FTRP(p)))
    {
        return;
    }

    size_t p_size = GET_SIZE(hdr);
    if (p_size <= 0 || ((char *)FTRP(p) + WSIZE > (char *)epilogue_blockp))
    {
        return;
    }

    size_t size = p_size;
    PUT(HDRP(p), PACK(size, 0));
    PUT(FTRP(p), PACK(size, 0));
    coalesce(p);
}

void *hcalloc(size_t n)
{
    void *bp = halloc(n);
    if (bp == NULL)
    {
        return NULL;
    }

    size_t bp_size = GET_SIZE(HDRP(bp));
    int num_bytes = (bp_size - DSIZE) / WSIZE;
    for (int i = 0; i < num_bytes; i++)
    {
        ((int *)bp)[i] = 0;
    }
    return bp;
}

void *hrealloc(void *p, size_t n)
{
    if (p == NULL)
    {
        return halloc(n);
    }

    if (n == 0)
    {
        hfree(p);
        return NULL;
    }

    void *bp_hdr = HDRP(p);
    void *next_block_hdr = HDRP(NEXT_BLOCK(p));
    bool is_next_block_epilogue = GET_SIZE(next_block_hdr) == 0 && GET_ALLOC(next_block_hdr);

    size_t curr_size = GET_SIZE(bp_hdr);
    size_t unaligned_size;
    if (!add_checked(n, DSIZE, &unaligned_size))
        return NULL;

    size_t r_size;
    if (!add_checked(unaligned_size, block_padding(unaligned_size), &r_size))
        return NULL;
    signed long additional_needed = r_size - curr_size;

    if (additional_needed == 0)
    {
        return p;
    }

    // Space within current (if r_size is smaller) or forward block
    if (r_size < curr_size || (!is_next_block_epilogue && !GET_ALLOC(next_block_hdr) && (signed long)(GET_SIZE(next_block_hdr)) >= additional_needed))
    {

        PUT(bp_hdr, PACK(curr_size + (additional_needed > 0 ? GET_SIZE(next_block_hdr) : 0), 1));
        void *res_blockp = split(p, r_size);

        if (!(r_size < curr_size) && mem_cursorp == ((char *)next_block_hdr + WSIZE))
        {
            mem_cursorp = res_blockp;
        }

        if (additional_needed < 0)
        {
            coalesce(NEXT_BLOCK(res_blockp));
        }
        return res_blockp;
    }
    else
    {
        // Find a free block or extend heap space
        void *new_blockp;
        if ((new_blockp = placement(r_size)) == NULL)
        {
            size_t multipler;
            if (!add_checked((r_size / CHUNKSIZE), (r_size % CHUNKSIZE ? 1 : 0), &multipler))
                return NULL;

            size_t extension_size;
            if (!mult_checked(CHUNKSIZE, multipler, &extension_size))
                return NULL;

            if ((new_blockp = mem_extend_heap(extension_size / WSIZE)) == NULL)
            {
                return NULL;
            }
        }

        // Split new chunk
        void *res_blockp = split(new_blockp, r_size);

        // Copy data into new block
        int num_bytes = (curr_size - DSIZE) / WSIZE;
        for (int i = 0; i < num_bytes; i++)
        {
            ((int *)res_blockp)[i] = ((int *)p)[i];
        }

        // Free old block
        hfree(p);

        return res_blockp;
    }
}

void hshow()
{
    void *cursor = heap_listp;
    int counter = 0;
    printf("=======FREE LIST START=======\n");
    while (HDRP(cursor) != epilogue_blockp)
    {
        void *hdr = HDRP(cursor);
        printf("Block %d, Size %d, Allocated %d\n", counter, GET_SIZE(hdr), GET_ALLOC(hdr));
        cursor = NEXT_BLOCK(cursor);
        counter++;
    }
    printf("=======FREE LIST END=======\n\n");
}