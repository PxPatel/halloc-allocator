#include <stdlib.h>
#include <stdio.h>
#include "allocator.h"
#include <unistd.h>

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

static size_t block_padding(size_t words)
{
    return (words % ALIGNMENT ? ALIGNMENT - (words % ALIGNMENT) : 0);
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

    PUT(prev_block_free ? prev_block_hdr : HDRP(bp), new_header);
    PUT(next_block_free ? FTRP(next_block) : FTRP(bp), new_header);

    return prev_block_free ? prev_block : bp;
}

static void *mem_extend_heap(size_t words)
{
    void *bp;
    size_t size;

    size = WSIZE * (words + block_padding(words));
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
    size_t r_size = n + DSIZE + block_padding(n + DSIZE);

    void *blockp;
    if ((blockp = placement(r_size)) == NULL)
    {
        unsigned int multipler = (r_size / CHUNKSIZE) + (r_size % CHUNKSIZE ? 1 : 0);
        if ((blockp = mem_extend_heap(CHUNKSIZE * multipler / WSIZE)) == NULL)
        {
            return NULL;
        }
    }

    void *bp = split(blockp, r_size);
    return bp;
}

void hfree(void *p)
{
    size_t size = GET_SIZE(HDRP(p));
    PUT(HDRP(p), PACK(size, 0));
    PUT(FTRP(p), PACK(size, 0));
    coalesce(p);
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