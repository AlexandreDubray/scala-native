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
                   word_t *blockStart, LineMeta *lineMetas, bool collectingOld) {

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
        LineMeta *lineMeta = lineMetas;
        word_t *lineStart = blockStart;
        ObjectMeta *bytemapCursor = Bytemap_Get(bytemap, lineStart);

        FreeLineMeta *lastRecyclable = NULL;
        while (lineIndex < LINE_COUNT) {
            // If the line is marked, we need to unmark all objects in the line
            if (Line_IsMarked(lineMeta)) {
                // Unmark line
                Line_Unmark(lineMeta);
                if (collectingOld) {
                    ObjectMeta_SweepOldLineAt(bytemapCursor);
                } else if (BlockMeta_IsOld(blockMeta)) {
                    ObjectMeta_SweepNewOldLineAt(bytemapCursor);
                } else {
                    ObjectMeta_SweepLineAt(bytemapCursor);
                }

                // next line
            }
            lineIndex++;
            lineMeta++;
            lineStart += WORDS_IN_LINE;
            bytemapCursor = Bytemap_NextLine(bytemapCursor);
        }
    }
}
