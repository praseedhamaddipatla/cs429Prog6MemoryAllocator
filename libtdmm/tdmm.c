#include "tdmm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

sec *frH = NULL;    // free head
sec *allocH = NULL; // allocated head

void *mStart = NULL; // memory start
size_t mSize = 0;    // memory size

alloc_strat_e currPol;

size_t totMap = 0;
size_t totAlloc = 0;
size_t totOh = 0;
double utilSum = 0;
size_t utilCount = 0;

// get ptr to tag at end of block
size_t *tag(sec *s) {
    return (size_t *)((char *)(s + 1) + s->size - sizeof(size_t));
}

// call when size or free changes
void setTag(sec *s) { *tag(s) = s->size; }

// init globals
void t_init(alloc_strat_e pol) {

    // unmap old heap
    if (mStart && mStart != MAP_FAILED) {
        munmap(mStart, mSize);
    }

    // reset all globals
    mStart = NULL;
    mSize = 0;
    frH = NULL;
    allocH = NULL;

    currPol = pol;

    totMap = 0;
    totAlloc = 0;
    totOh = 0;
    utilSum = 0;
    utilCount = 0;

    // map new heap
    size_t pgSize = getpagesize();
    size_t requested = 64 * 1024 * 1024;

    mSize = ((requested + pgSize - 1) / pgSize) * pgSize;

    mStart = mmap(NULL, mSize, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (mStart == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    // init free
    frH = (sec *)mStart;

    frH->size = mSize - sizeof(sec);
    frH->free = 1;
    frH->n = NULL;
    frH->p = NULL;

    setTag(frH);

    allocH = NULL;

    totMap = mSize;
}

// helper for removing block
void detach(sec **head, sec *s) {
    if (!s || !head)
        return;

    if (s->p)
        s->p->n = s->n;
    else
        *head = s->n;

    if (s->n)
        s->n->p = s->p;

    s->n = NULL;
    s->p = NULL;
}

// helper for inserting block sorted by addr
void insert(sec **head, sec *s) {
    if (!s || !head)
        return;

    // first node with addr > s
    sec *curr = *head;
    sec *prev = NULL;
    while (curr && curr < s) {
        prev = curr;
        curr = curr->n;
    }

    // insert between prev and curr
    s->n = curr;
    s->p = prev;
    if (curr)
        curr->p = s;
    if (prev)
        prev->n = s;
    else
        *head = s;
}

size_t align(size_t size) {
    if (size % 4 == 0) {
        return size;
    }
    return ((size + 3) / 4) * 4;
}

sec *findFirst(size_t size) {
    sec *search = frH;
    while (search != NULL) {
        if (search->size >= size) {
            return search;
        }
        search = search->n;
    }
    fprintf(stderr, "free block not found");
    return NULL;
}

sec *findBest(size_t size) {
    sec *search = frH;
    sec *best = NULL;
    size_t diff = SIZE_MAX;

    while (search != NULL) {
        if (search->size >= size) {

            size_t curr_diff = search->size - size;

            if (best == NULL || curr_diff < diff) {
                best = search;
                diff = curr_diff;
            }
        }
        search = search->n;
    }

    if (best == NULL) {
        fprintf(stderr, "free block not found");
    }

    return best;
}

sec *findWorst(size_t size) {
    sec *search = frH;
    sec *worst = NULL;
    size_t diff = 0;

    while (search != NULL) {

        if (search->size >= size) {

            size_t curr_diff = search->size - size;

            if (worst == NULL || curr_diff > diff) {
                worst = search;
                diff = curr_diff;
            }
        }

        search = search->n;
    }

    if (worst == NULL) {
        fprintf(stderr, "free block not found");
    }

    return worst;
}

void split(sec *s, size_t aligned)
{
    size_t totalBlockSize =
        sizeof(sec) + s->size + sizeof(size_t);

    size_t usedBlockSize =
        sizeof(sec) + aligned + sizeof(size_t);

    size_t remaining = totalBlockSize - usedBlockSize;

    if (remaining <= sizeof(sec) + sizeof(size_t) + 4)
        return;

    sec *new = (sec *)((char *)s + usedBlockSize);

    new->size = remaining - sizeof(sec) - sizeof(size_t);
    new->free = 1;
    new->n = NULL;
    new->p = NULL;

    s->size = aligned;

    setTag(s);
    setTag(new);

    insert(&frH, new);
}

void printStats() {
    printf("Mapped: %zu\n", totMap);
    printf("Allocated: %zu\n", totAlloc);
    printf("Utilization: %.2f%%\n", 100.0 * totAlloc / totMap);
    printf("Overhead: %zu\n", totOh);
    if (utilCount > 0)
        printf("Avg Utilization: %.4f%%\n", 100.0 * utilSum / utilCount);
}

void *t_malloc(size_t size) {
    if (size == 0)
        return NULL;
    size_t aligned = align(size);
    sec *found = NULL;

    // choose policy
    if (currPol == FIRST_FIT) {
        found = findFirst(aligned);
    } else if (currPol == BEST_FIT) {
        found = findBest(aligned);
    } else if (currPol == WORST_FIT) {
        found = findWorst(aligned);
    } else {
        fprintf(stderr, "policy undefined");
        exit(1);
    }
    if (found == NULL) {
        fprintf(stderr, "t_malloc failed");
        return NULL;
    }

    detach(&frH, found);
    split(found, aligned);

    found->free = 0;
    setTag(found);
    insert(&allocH, found);

    totAlloc += found->size;
    totOh += sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // return ptr
    return (void *)(found + 1);
}

// consolidate adj free blocks
void merge(sec *s)
{
    char *heapStart = (char *)mStart;
    char *heapEnd   = heapStart + mSize;

    // merge next
    char *nextAddr = (char *)s + sizeof(sec) + s->size + sizeof(size_t);

    if (nextAddr + sizeof(sec) <= heapEnd)
    {
        sec *next = (sec *)nextAddr;

        if (next->free)
        {
            detach(&frH, next);

            s->size += sizeof(sec) + sizeof(size_t) + next->size;

            setTag(s);
        }
    }

    // merge prev
    char *footerAddr = (char *)s - sizeof(size_t);

    if (footerAddr >= heapStart)
    {
        size_t prevSize = *(size_t *)footerAddr;

        char *prevAddr =
            (char *)s - sizeof(size_t) - sizeof(sec) - prevSize;

        if (prevAddr >= heapStart)
        {
            sec *prev = (sec *)prevAddr;

            if (prev->free && prev->size == prevSize)
            {
                detach(&frH, s);

                prev->size += sizeof(sec) + sizeof(size_t) + s->size;

                setTag(prev);

                s = prev;
            }
        }
    }
}

void t_free(void *ptr)
{
    if (!ptr)
        return;

    sec *block = (sec *)ptr - 1;

    // validate exists in alloc list
    sec *check = allocH;
    while (check && check != block)
        check = check->n;

    if (!check)
    {
        fprintf(stderr, "t_free: invalid or already-freed pointer\n");
        return;
    }

    // remove from alloc list
    detach(&allocH, block);

    block->free = 1;
    setTag(block);

    totAlloc -= block->size;
    totOh -= sizeof(sec);

    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // merge before inserting into free list
    merge(block);

    // find true merged block
    sec *merged = block;

    char *footerAddr = (char *)block - sizeof(size_t);
    if (footerAddr >= (char *)mStart)
    {
        size_t prevSize = *(size_t *)footerAddr;
        char *prevAddr =
            (char *)block - sizeof(size_t) - sizeof(sec) - prevSize;

        if (prevAddr >= (char *)mStart)
        {
            sec *prev = (sec *)prevAddr;
            if (prev->free)
                merged = prev;
        }
    }

    insert(&frH, merged);

    printStats();
}