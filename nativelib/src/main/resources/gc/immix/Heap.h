#ifndef IMMIX_HEAP_H
#define IMMIX_HEAP_H

#include "GCTypes.h"
#include "Allocator.h"
#include "LargeAllocator.h"
#include "datastructures/Stack.h"
#include "datastructures/Bytemap.h"
#include "Stats.h"
#include <stdio.h>

typedef struct {
    word_t *blockMetaStart;
    word_t *blockMetaEnd;
    word_t *heapStart;
    word_t *heapEnd;
    size_t heapSize;
    size_t maxHeapSize;
    uint32_t blockCount;
    uint32_t maxBlockCount;
    Bytemap *bytemap;
    Stats *stats;
} Heap;

static inline bool Heap_IsWordInHeap(Heap *heap, word_t *word) {
    return word >= heap->heapStart && word < heap->heapEnd;
}

void Heap_Init(Heap *heap, size_t minHeapSize, size_t maxHeapSize);
word_t *Heap_Alloc(Heap *heap, uint32_t objectSize);
word_t *Heap_AllocSmall(Heap *heap, uint32_t objectSize);
word_t *Heap_AllocLarge(Heap *heap, uint32_t objectSize);

void Heap_Collect(Heap *heap, Stack *stack);
void Heap_CollectOld(Heap *heap, Stack *stack);

void Heap_Grow(Heap *heap, uint32_t increment);
void Heap_Recycle(Heap *heap, bool collectingOld);

#endif // IMMIX_HEAP_H
