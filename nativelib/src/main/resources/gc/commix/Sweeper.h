#ifndef IMMIX_SWEEPER_H
#define IMMIX_SWEEPER_H

#include "Heap.h"
#include "Stats.h"
#include "datastructures/BlockRange.h"
#include "SweepResult.h"

void Sweeper_Sweep(Heap *heap, Stats *stats, atomic_uint_fast32_t *cursorDone,
                   uint32_t maxCount, bool collectingOld);
uint_fast32_t Sweeper_minSweepCursor(Heap *heap);
void Sweeper_LazyCoalesce(Heap *heap, Stats *stats);
void Sweeper_LazySweepUntilDone(Heap *heap);

static inline bool Sweeper_IsCoalescingDone(Heap *heap) {
    return heap->sweep.coalesceDone >= heap->sweep.limit;
}

static inline bool Sweeper_IsSweepDone(Heap *heap) {
    return heap->sweep.postSweepDone;
}

#ifdef DEBUG_ASSERT
void Sweeper_ClearIsSwept(Heap *heap);
void Sweeper_AssertIsConsistent(Heap *heap);
#endif

#endif // IMMIX_SWEEPER_H
