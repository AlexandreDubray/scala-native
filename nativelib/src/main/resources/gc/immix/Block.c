#include <stdio.h>
#include <memory.h>
#include "Block.h"
#include "Object.h"
#include "metadata/ObjectMeta.h"
#include "Log.h"
#include "Allocator.h"
#include "Marker.h"
#include "State.h"

INLINE void Block_recycleUnmarkedBlock(Allocator *allocator,
                                       BlockMeta *blockMeta,
                                       word_t *blockStart) {
    memset(blockMeta, 0, sizeof(BlockMeta));
    // does not unmark in LineMetas because those are ignored by the allocator
    BlockAllocator_AddFreeBlocks(allocator->blockAllocator, blockMeta, 1);
    ObjectMeta_ClearBlockAt(Bytemap_Get(allocator->bytemap, blockStart));
}

/**
 * recycles a block and adds it to the allocator
 */
void Block_Recycle(Allocator *allocator, BlockMeta *blockMeta,
                   word_t *blockStart, bool collectingOld) {

    // If the block is not marked, it means that it's completely free
    if (!BlockMeta_IsMarked(blockMeta)) {
        Block_recycleUnmarkedBlock(allocator, blockMeta, blockStart);
    } else {
        // If the block is marked, we need to recycle line by line
        assert(BlockMeta_IsMarked(blockMeta));
        BlockMeta_Unmark(blockMeta);
        if (!collectingOld) {
            assert(!BlockMeta_IsOld(blockMeta));
            BlockMeta_IncrementAge(blockMeta);
            if (BlockMeta_IsOld(blockMeta)) {
                blockAllocator.oldBlockCount ++;
            } else {
                blockAllocator.youngBlockCount ++;
            }
        }  else {
            blockAllocator.oldBlockCount ++;
        }

        Bytemap *bytemap = allocator->bytemap;

        // start at line zero, keep separate pointers into all affected data
        // structures
        int lineIndex = 0;
        word_t *lineStart = blockStart;
        ObjectMeta *bytemapCursor = Bytemap_Get(bytemap, lineStart);
        ObjectMeta *lastCursor = bytemapCursor + (WORDS_IN_LINE/ALLOCATION_ALIGNMENT_WORDS)*LINE_COUNT;

        if (collectingOld) {
            while (bytemapCursor < lastCursor) {
                ObjectMeta_SweepOldLineAt(bytemapCursor);
                bytemapCursor = Bytemap_NextLine(bytemapCursor);
            }
        } else if (BlockMeta_IsOld(blockMeta)) {
            while (bytemapCursor < lastCursor) {
                ObjectMeta_SweepNewOldLineAt(bytemapCursor);
                bytemapCursor = Bytemap_NextLine(bytemapCursor);
            }
        } else {
            while (bytemapCursor < lastCursor) {
                ObjectMeta_SweepNewOldLineAt(bytemapCursor);
                bytemapCursor = Bytemap_NextLine(bytemapCursor);
            }
        }
    }
}
