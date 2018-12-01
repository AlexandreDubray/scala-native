#ifndef IMMIX_BLOCK_H
#define IMMIX_BLOCK_H

#include "metadata/BlockMeta.h"
#include "Heap.h"

void Block_Recycle(Allocator *allocator, BlockMeta *block, word_t *blockStart,
                   bool collectingOld);
#endif // IMMIX_BLOCK_H
