#ifndef IMMIX_MARKER_H
#define IMMIX_MARKER_H

#include "Heap.h"
#include "datastructures/Stack.h"

extern bool collectingOld;

void Marker_MarkRoots(Heap *heap, Stack *stack, bool collectingOld);
void Marker_Mark(Heap *heap, Stack *stack, bool collectingOld);

#endif // IMMIX_MARKER_H
