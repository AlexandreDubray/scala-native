#include <stdlib.h>
#include "Allocator.h"
#include "Block.h"
#include <stdio.h>
#include <memory.h>
#include "State.c"

bool Allocator_newBlock(Allocator *allocator);
bool Allocator_newPretenuredBlock(Allocator *allocator);

void Allocator_Init(Allocator *allocator, BlockAllocator *blockAllocator,
                    Bytemap *bytemap, word_t *blockMetaStart,
                    word_t *heapStart) {
    allocator->blockMetaStart = blockMetaStart;
    allocator->blockAllocator = blockAllocator;
    allocator->bytemap = bytemap;
    allocator->heapStart = heapStart;

    // For remembering old object that might contains inter-generational
    // pointers
    Stack_Init(&allocator->rememberedObjects, INITIAL_STACK_SIZE);
    Stack_Init(&allocator->rememberedYoungObjects, INITIAL_STACK_SIZE);

    Allocator_InitCursors(allocator);
}

/**
 * The Allocator needs one free block for overflow allocation and a free or
 * recyclable block for normal allocation.
 *
 * @param allocator
 * @return `true` if there are enough block to initialise the cursors, `false`
 * otherwise.
 */
bool Allocator_CanInitCursors(Allocator *allocator) {
    return allocator->blockAllocator->freeBlockCount >= 3;
}

void Allocator_InitCursors(Allocator *allocator) {

    // Init cursor
    bool didInit = Allocator_newBlock(allocator);
    assert(didInit);

    if (PRETENURE_OBJECT) {
        didInit = Allocator_newPretenuredBlock(allocator);
        assert(didInit);
    }

    // Init large cursor
    BlockMeta *largeBlock =
        BlockAllocator_GetFreeBlock(allocator->blockAllocator);
    assert(largeBlock != NULL);
    allocator->largeBlock = largeBlock;
    word_t *largeBlockStart = BlockMeta_GetBlockStart(
        allocator->blockMetaStart, allocator->heapStart, largeBlock);
    allocator->largeBlockStart = largeBlockStart;
    allocator->largeCursor = largeBlockStart;
    allocator->largeLimit = Block_GetBlockEnd(largeBlockStart);
}

/**
 * Overflow allocation uses only free blocks, it is used when the bump limit of
 * the fast allocator is too small to fit
 * the block to alloc.
 */
word_t *Allocator_overflowAllocation(Allocator *allocator, size_t size) {
    word_t *start = allocator->largeCursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    if (end > allocator->largeLimit) {
        if (blockAllocator.youngBlockCount >= MAX_YOUNG_BLOCKS) {
#ifdef DEBUG_PRINT
            printf("Young generation full\n");
            fflush(stdout);
#endif
            return NULL;
        }
        BlockMeta *block =
            BlockAllocator_GetFreeBlock(allocator->blockAllocator);
        if (block == NULL) {
            return NULL;
        }
        allocator->blockAllocator->youngBlockCount++;
        allocator->largeBlock = block;
        word_t *blockStart = BlockMeta_GetBlockStart(
            allocator->blockMetaStart, allocator->heapStart, block);
        allocator->largeBlockStart = blockStart;
        allocator->largeCursor = blockStart;
        allocator->largeLimit = Block_GetBlockEnd(blockStart);
        return Allocator_overflowAllocation(allocator, size);
    }

    memset(start, 0, size);

    allocator->largeCursor = end;

    return start;
}

/**
 * Allocation fast path, uses the cursor and limit.
 */
INLINE word_t *Allocator_Alloc(Allocator *allocator, size_t size) {
    word_t *start = allocator->cursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    // Checks if the end of the block overlaps with the limit
    if (end > allocator->limit) {
        // If it overlaps but the block to allocate is a `medium` sized block,
        // use overflow allocation
        if (size > LINE_SIZE) {
            return Allocator_overflowAllocation(allocator, size);
        } else {
            // If maximal number of free block reached, need to collect the young generation
            if (!(allocator->blockAllocator->youngBlockCount < MAX_YOUNG_BLOCKS)) {
                return NULL;
            }
            if (Allocator_newBlock(allocator)) {
                return Allocator_Alloc(allocator, size);
            }

            return NULL;
        }
    }

    memset(start, 0, size);

    allocator->cursor = end;

    return start;
}

/**
 * Updates the the cursor and the limit of the Allocator to point to the first
 * free line of the new block.
 */
bool Allocator_newBlock(Allocator *allocator) {
    BlockMeta * block = BlockAllocator_GetFreeBlock(allocator->blockAllocator);
    if (block == NULL) {
        return false;
    }
    assert(BlockMeta_GetAge(block) == 0);
    word_t * blockStart = BlockMeta_GetBlockStart(allocator->blockMetaStart,
                                         allocator->heapStart, block);

    allocator->cursor = blockStart;
    allocator->limit = Block_GetBlockEnd(blockStart);
    BlockMeta_SetFirstFreeLine(block, LAST_HOLE);

    allocator->block = block;
    allocator->blockStart = blockStart;
    blockAllocator.youngBlockCount ++;
    return true;
}

INLINE word_t *Allocator_AllocPretenured(Allocator *allocator, size_t size) {
    word_t *start = allocator->pretenuredCursor;
    word_t *end = (word_t *)((uint8_t *)start + size);
    if (end > allocator->pretenuredLimit) {
        if (Allocator_newPretenuredBlock(allocator)) {
            return Allocator_AllocPretenured(allocator, size);
        }
        return NULL;
    }

    memset(start, 0, size);
    allocator->pretenuredCursor = end;
    return start;
}

bool Allocator_newPretenuredBlock(Allocator *allocator) {
    BlockMeta *block = BlockAllocator_GetFreeBlock(allocator->blockAllocator);
    if (block == NULL) {
        return false;
    }
    BlockMeta_SetOld(block);
    assert(BlockMeta_IsOld(block));
    word_t * blockStart = BlockMeta_GetBlockStart(allocator->blockMetaStart,
                                         allocator->heapStart, block);
    allocator->pretenuredCursor = blockStart;
    allocator->pretenuredLimit = Block_GetBlockEnd(blockStart);
    BlockMeta_SetFirstFreeLine(block, LAST_HOLE);
    allocator->pretenuredBlock = block;
    allocator->pretenuredBlockStart = blockStart;
    blockAllocator.oldBlockCount++;
    return true;
}
