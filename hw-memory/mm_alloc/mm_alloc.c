/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

// #define DEBUG

/* Basic constants and macros */
#define WSIZE   8
#define DSIZE   16
#define MAX_N   21
#define CHUNKSIZE (1<<12)

#define MAX(x, y) ((x) > (y)? (x) : (y))
#define MIN(x, y) ((x) < (y)? (x) : (y))

#define PACK(size, alloc) ((size) | (alloc))

#define GET(p) (*(u_int64_t *)(p))
#define PUT(p, val) (*(u_int64_t*)(p) = (u_int64_t)(val))

#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define GET_BP(p) ((char *)(p) + WSIZE)

#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

static void *heap_listp;
static void *ptr;

/* List Operation */
static int find_id(int sz);
#define IS_HEAD_PTR(p) ((ptr) <= (p) && ((u_int64_t *)p) < ((u_int64_t *)ptr + MAX_N))
#define LIST_ID(bp) MIN((int)(find_id(GET_SIZE(HDRP(bp)) - 31)), MAX_N - 1)
#define HEAD_PTR_ADDR(id) ((u_int64_t *)ptr + id)
#define HEAD_PTR(id) (void *)(*HEAD_PTR_ADDR(id))
#define PUT_HEAD_PTR(id, val) (PUT(HEAD_PTR_ADDR(id), val))

#define PRED(bp) ((void *)(*(u_int64_t *)bp))
#define SUCC(bp) ((void *)(*((u_int64_t *)bp + 1)))
#define PUT_PRED(bp, val) (PUT(bp, val))
#define PUT_SUCC(bp, val) (PUT((u_int64_t *)bp + 1, val))

static void insert_to_list(void *bp) {
    #ifdef DEBUG
    printf("insert bp = %p\n", bp);
    #endif
    u_int64_t id = LIST_ID(bp);
    void *head_ptr_addr = HEAD_PTR_ADDR(id);
    void *head_ptr = HEAD_PTR(id);
    if (head_ptr == NULL) {
        PUT_HEAD_PTR(id, bp);
        PUT_PRED(bp, head_ptr_addr);
        PUT_SUCC(bp, 0);
    } else {
        void *next_block_bp = head_ptr;
        PUT_HEAD_PTR(id, bp);
        PUT_PRED(next_block_bp, bp);

        PUT_PRED(bp, head_ptr_addr);
        PUT_SUCC(bp, next_block_bp);
    }
}

static void delete_from_list(void *bp) {
    #ifdef DEBUG
    printf("delete bp = %p\n", bp);
    #endif
    if (IS_HEAD_PTR(PRED(bp))) {
        u_int64_t id = LIST_ID(bp);
        PUT_HEAD_PTR(id, SUCC(bp));
        if (SUCC(bp) != NULL) {
            PUT_PRED(SUCC(bp), HEAD_PTR_ADDR(id));
        }
    } else {
        PUT_SUCC(PRED(bp), SUCC(bp));
        if (SUCC(bp) != NULL) {
            PUT_PRED(SUCC(bp), PRED(bp));
        }
    }
}

static void *coalesce(void *bp) {
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));
    if (prev_alloc && next_alloc){
        return bp;
    }

    else if (prev_alloc && !next_alloc) {
        delete_from_list(bp);
        delete_from_list(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size,0));
        PUT(FTRP(bp), PACK(size,0));
        insert_to_list(bp);
    }

    else if (!prev_alloc && next_alloc) {
        delete_from_list(bp);
        delete_from_list(PREV_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_to_list(bp);
    }

    else {
        delete_from_list(bp);
        delete_from_list(NEXT_BLKP(bp));
        delete_from_list(PREV_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
            GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_to_list(bp);
    }

    return bp;
}

static void *extend_heap(size_t words) {
    #ifdef DEBUG
    printf("extend_heap %zu\n", words);
    #endif
    char *bp;
    size_t size;

    size = (words % 2) ? (words+1) * WSIZE : words * WSIZE;
    if ((long)(bp = sbrk(size)) == -1)
        return NULL;

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    insert_to_list(bp);
    return coalesce(bp);
}

static void *find_fit(size_t asize) {
    for (int id = find_id(asize); id < MAX_N; id++) {
        void *bp = HEAD_PTR(id);
        while (bp != NULL) {
            if (asize <= GET_SIZE(HDRP(bp)))
                return bp;
            bp = SUCC(bp);
        }
    }
    return NULL;
}

/* place when remaining part size is greater than 2 word, divide it. */
static void place(void *bp, size_t asize)
{
    delete_from_list(bp);
	size_t size = GET_SIZE(HDRP(bp));
	if (size - asize >= 4*WSIZE) {
		PUT(HDRP(bp), PACK(asize, 1));
		PUT(FTRP(bp), PACK(asize, 1));
		bp = NEXT_BLKP(bp);
		PUT(HDRP(bp), PACK(size-asize, 0));
		PUT(FTRP(bp), PACK(size-asize, 0));
        insert_to_list(bp);
	} else {
		PUT(HDRP(bp), PACK(size, 1));
		PUT(FTRP(bp), PACK(size, 1));
	}
}

/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void) {
    if ((heap_listp = sbrk((MAX_N+3)*WSIZE)) == (void *)-1)
    	return -1;
    for (int i = 0; i < MAX_N; i++)
        PUT(heap_listp + (i*WSIZE), 0);
    PUT(heap_listp + (MAX_N)*WSIZE, PACK(DSIZE, 1));
    PUT(heap_listp + (1+MAX_N)*WSIZE, PACK(DSIZE, 1));
    PUT(heap_listp + (2+MAX_N)*WSIZE, PACK(0, 1));
    ptr = heap_listp;
    heap_listp += (1+MAX_N)*WSIZE;
    #ifdef DEBUG
    printf("\nptr = %p, heap_listp = %p\n", ptr, heap_listp);
    #endif

    if (extend_heap(CHUNKSIZE/WSIZE) == NULL)
        return -1;
    return 0;
}

/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *mm_malloc(size_t size) {
    #ifdef DEBUG
    printf("\n[malloc]    size: %zu\n", size);
    #endif

    if (heap_listp == NULL)
      if (mm_init() == -1) return NULL;

    size_t asize;       /* Adjusted block size */
    size_t extendsize;  /* Amount to extend heap if no fit */
    char *bp;

    /* Ignore spurious requests */
    if (size == 0)
        return NULL;

    /* Adjust block size to include overhead and alignment reqs. */
    if (size <= DSIZE)
        asize = 2*DSIZE;
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);

    /* Search the free list for a fit */
    if ((bp = find_fit(asize)) != NULL) {
        place(bp, asize);
        #ifdef DEBUG
        printf("asize = %zu,\nfit_bp = %p\n", asize, bp);
        #endif
        return bp;
    }
    /* Search the free list for a fit */

    /* No fit found. Get more memory and place the block */
    extendsize = MAX(asize, CHUNKSIZE) ;
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL)
        return NULL;
    place(bp, asize);

    #ifdef DEBUG
    printf("asize = %zu,\nbp = %p\n", asize, bp);
    #endif
    return bp;
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *bp)
{
    #ifdef DEBUG
    printf("\n[free]    bp: %p\n", bp);
    #endif
    size_t size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    insert_to_list(bp);
    coalesce(bp);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size) {
    void *oldptr = ptr;
    void *newptr;
    size_t copySize;

    newptr = mm_malloc(size);
    if (newptr == NULL)
      return NULL;
    copySize = *(size_t *)((char *)oldptr - WSIZE);
    if (size < copySize)
      copySize = size;
    memcpy(newptr, oldptr, copySize);
    mm_free(oldptr);
    return newptr;
}

static int find_id(int sz) {
    int now = 1;
    for (int i = 0; i < MAX_N; i++, now <<= 1) {
        if (sz <= now) return i;
    }
    return MAX_N - 1;
}
