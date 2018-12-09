#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "LargeAllocator.h"
#include "utils/MathUtils.h"
#include "Object.h"
#include "Log.h"
#include "headers/ObjectHeader.h"
#include "Marker.h"
#include "State.h"

#include "metadata/BlockMeta.h"

inline static int LargeAllocator_sizeToLinkedListIndex(size_t size) {
    assert(size >= MIN_BLOCK_SIZE);
    assert(size % MIN_BLOCK_SIZE == 0);
    int index = size / MIN_BLOCK_SIZE - 1;
    assert(index < FREE_LIST_COUNT);
    return index;
}

Chunk *LargeAllocator_chunkAddOffset(Chunk *chunk, size_t words) {
    return (Chunk *)((ubyte_t *)chunk + words);
}

void LargeAllocator_freeListAddBlockLast(FreeList *freeList, Chunk *chunk) {
    if (freeList->first == NULL) {
        freeList->first = chunk;
    }
    freeList->last = chunk;
    chunk->next = NULL;
}

Chunk *LargeAllocator_freeListRemoveFirstBlock(FreeList *freeList) {
    if (freeList->first == NULL) {
        return NULL;
    }
    Chunk *chunk = freeList->first;
    if (freeList->first == freeList->last) {
        freeList->last = NULL;
    }

    freeList->first = chunk->next;
    return chunk;
}

void LargeAllocator_freeListInit(FreeList *freeList) {
    freeList->first = NULL;
    freeList->last = NULL;
}

void LargeAllocator_Init(LargeAllocator *allocator,
                         BlockAllocator *blockAllocator, Bytemap *bytemap,
                         word_t *blockMetaStart, word_t *heapStart) {
    allocator->heapStart = heapStart;
    allocator->blockMetaStart = blockMetaStart;
    allocator->bytemap = bytemap;
    allocator->blockAllocator = blockAllocator;

    for (int i = 0; i < FREE_LIST_COUNT; i++) {
        LargeAllocator_freeListInit(&allocator->freeLists[i]);
    }
}

void LargeAllocator_AddChunk(LargeAllocator *allocator, Chunk *chunk,
                             size_t total_block_size) {
    assert(total_block_size >= MIN_BLOCK_SIZE);
    assert(total_block_size < BLOCK_TOTAL_SIZE);
    assert(total_block_size % MIN_BLOCK_SIZE == 0);

    int listIndex = LargeAllocator_sizeToLinkedListIndex(total_block_size);
    chunk->nothing = NULL;
    chunk->size = total_block_size;
    ObjectMeta *chunkMeta = Bytemap_Get(allocator->bytemap, (word_t *)chunk);
    ObjectMeta_SetPlaceholder(chunkMeta);

    LargeAllocator_freeListAddBlockLast(&allocator->freeLists[listIndex],
                                        chunk);
}

static inline Chunk *LargeAllocator_getChunkForSize(LargeAllocator *allocator,
                                                    size_t requiredChunkSize) {
    for (int listIndex =
             LargeAllocator_sizeToLinkedListIndex(requiredChunkSize);
         listIndex < FREE_LIST_COUNT; listIndex++) {
        Chunk *chunk = allocator->freeLists[listIndex].first;
        if (chunk != NULL) {
            LargeAllocator_freeListRemoveFirstBlock(
                &allocator->freeLists[listIndex]);
            return chunk;
        }
    }
    return NULL;
}

Object *LargeAllocator_GetBlock(LargeAllocator *allocator,
                                size_t requestedBlockSize) {
    size_t actualBlockSize =
        MathUtils_RoundToNextMultiple(requestedBlockSize, MIN_BLOCK_SIZE);

    Chunk *chunk = NULL;
    if (actualBlockSize < BLOCK_TOTAL_SIZE) {
        // only need to look in free lists for chunks smaller than a block
        chunk = LargeAllocator_getChunkForSize(allocator, actualBlockSize);
    }

    if (chunk == NULL) {
        uint32_t superblockSize = (uint32_t)MathUtils_DivAndRoundUp(
            actualBlockSize, BLOCK_TOTAL_SIZE);
        BlockMeta *superblock = BlockAllocator_GetFreeSuperblock(
            allocator->blockAllocator, superblockSize);
        if (superblock != NULL) {
            assert(BlockMeta_GetAge(superblock) == 0);
            if (PRETENURE_OBJECT) {
                BlockMeta *lastBlock = superblock + superblockSize - 1;
                BlockMeta_SetOld(superblock);
                // We need to maintain that all block containing old
                // object are old
                if (lastBlock != superblock) {
                    BlockMeta_SetOld(lastBlock);
                }
            }
            chunk = (Chunk *)BlockMeta_GetBlockStart(
                allocator->blockMetaStart, allocator->heapStart, superblock);
            chunk->nothing = NULL;
            chunk->size = superblockSize * BLOCK_TOTAL_SIZE;
        }
    }

    if (chunk == NULL) {
        return NULL;
    }

    size_t chunkSize = chunk->size;
    assert(chunkSize >= MIN_BLOCK_SIZE);

    if (chunkSize - MIN_BLOCK_SIZE >= actualBlockSize) {
        Chunk *remainingChunk =
            LargeAllocator_chunkAddOffset(chunk, actualBlockSize);

        size_t remainingChunkSize = chunkSize - actualBlockSize;
        LargeAllocator_AddChunk(allocator, remainingChunk, remainingChunkSize);
    }

    ObjectMeta *objectMeta = Bytemap_Get(allocator->bytemap, (word_t *)chunk);
    assert(!ObjectMeta_IsRemembered(objectMeta));

    if (PRETENURE_OBJECT) {
        ObjectMeta_SetMarked(objectMeta);
    } else {
        ObjectMeta_SetAllocated(objectMeta);
    }
    Object *object = (Object *)chunk;
    memset(object, 0, actualBlockSize);
    return object;
}

void LargeAllocator_Clear(LargeAllocator *allocator) {
    for (int i = 0; i < FREE_LIST_COUNT; i++) {
        allocator->freeLists[i].first = NULL;
        allocator->freeLists[i].last = NULL;
    }
}

void LargeAllocator_Sweep(LargeAllocator *allocator, BlockMeta *blockMeta,
                          word_t *blockStart, bool collectingOld) {
    // Objects that are larger than a block
    // are always allocated at the begining the smallest possible superblock.
    // Any gaps at the end can be filled with large objects, that are smaller
    // than a block. This means that objects can ONLY start at the begining at
    // the first block or anywhere at the last block, except the begining.
    // Therefore we only need to look at a few locations.
    uint32_t superblockSize = BlockMeta_SuperblockSize(blockMeta);
    word_t *blockEnd = blockStart + WORDS_IN_BLOCK * superblockSize;

    ObjectMeta *firstObject = Bytemap_Get(allocator->bytemap, blockStart);
    //assert(!ObjectMeta_IsFree(firstObject));
    BlockMeta *lastBlock = blockMeta + superblockSize - 1;

    if (!collectingOld) {
        assert(!BlockMeta_IsOld(blockMeta));
        BlockMeta_IncrementAge(blockMeta);
        if (superblockSize > 1) {
            BlockMeta_IncrementAge(lastBlock);
        }
        assert(BlockMeta_GetAge(blockMeta) == BlockMeta_GetAge(lastBlock));
    }

    bool firstObjectAlive = ObjectMeta_IsAliveSweep(firstObject, collectingOld);
    if (superblockSize > 1 && !firstObjectAlive) {
        // release free superblock starting from the first object
        BlockAllocator_AddFreeBlocks(allocator->blockAllocator, blockMeta,
                                     superblockSize - 1);

        BlockMeta_SetFlag(lastBlock, block_superblock_start);
        BlockMeta_SetSuperblockSize(lastBlock, 1);
    }

    word_t *lastBlockStart = blockEnd - WORDS_IN_BLOCK;

    if (collectingOld) {
        ObjectMeta_SweepOld(firstObject);
    } else if (BlockMeta_IsOld(blockMeta)) {
        ObjectMeta_SweepNewOld(firstObject);
    } else {
        assert(!BlockMeta_IsOld(blockMeta) && !collectingOld);
        ObjectMeta_Sweep(firstObject);
    }

    word_t *current = lastBlockStart + (MIN_BLOCK_SIZE / WORD_SIZE);
    ObjectMeta *currentMeta = Bytemap_Get(allocator->bytemap, current);
    bool containsLiveObjects = firstObjectAlive;

    if (collectingOld) {
        while(current < blockEnd) {
            if (!containsLiveObjects) {
                // If the object is allocated, it is alive
                containsLiveObjects = *currentMeta & 0x2;
            }
            ObjectMeta_SweepOld(currentMeta);
            current += MIN_BLOCK_SIZE / WORD_SIZE;
            currentMeta += MIN_BLOCK_SIZE / ALLOCATION_ALIGNMENT;
        }
    } else if (BlockMeta_IsOld(lastBlock)) {
        // New old block during young collection. Live object inside it
        // are marked and dead are allocated
        while (current < blockEnd) {
            if (!containsLiveObjects) {
                // If the object is marked, it is alive
                containsLiveObjects = *currentMeta & 0x4;
            }
            ObjectMeta_SweepNewOld(currentMeta);
            current += MIN_BLOCK_SIZE / WORD_SIZE;
            currentMeta += MIN_BLOCK_SIZE / ALLOCATION_ALIGNMENT;
        }
    } else {
        // Young block
        while (current < blockEnd) {
            if (!containsLiveObjects) {
                containsLiveObjects = *currentMeta & 0x4;
            }
            ObjectMeta_Sweep(currentMeta);
            current += MIN_BLOCK_SIZE / WORD_SIZE;
            currentMeta += MIN_BLOCK_SIZE / ALLOCATION_ALIGNMENT;
        }
    }

    if (!containsLiveObjects) {
        // free chunk covers the entire last block, released it to the block
        // allocator. Note that if the first object is alive, the last
        // block MUST contain it and then be alive.
        BlockAllocator_AddFreeBlocks(allocator->blockAllocator, lastBlock, 1);
    }
}
