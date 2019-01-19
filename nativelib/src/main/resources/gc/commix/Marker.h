#ifndef IMMIX_MARKER_H
#define IMMIX_MARKER_H

#include "Heap.h"
#include "Stats.h"

void Marker_MarkRoots(Heap *heap);
void Marker_Mark(Heap *heap, Stats *stats);
bool Marker_IsMarkDone(Heap *heap);

#endif // IMMIX_MARKER_H