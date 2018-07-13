#include <stddef.h>
#include <stdio.h>
#include "Object.h"
#include "Block.h"
#include "Log.h"
#include "utils/MathUtils.h"
#include "metadata/BlockMeta.h"

#define LAST_FIELD_OFFSET -1

#define DEBUG_PRINT

word_t *Object_LastWord(Object *object) {
    size_t size = Object_Size(object);
    assert(size < LARGE_BLOCK_SIZE);
    word_t *last =
        (word_t *)((ubyte_t *)object + size) - ALLOCATION_ALIGNMENT_WORDS;
    return last;
}

Object *Object_getInnerPointer(Heap *heap, BlockMeta *blockMeta, word_t *word,
                               ObjectMeta *wordMeta, bool collectingOld) {
    int stride;
    word_t *blockStart;
    if (BlockMeta_ContainsLargeObjects(blockMeta)) {
        stride = MIN_BLOCK_SIZE / ALLOCATION_ALIGNMENT;
        BlockMeta *superblockStart =
            BlockMeta_GetSuperblockStart(heap->blockMetaStart, blockMeta);
        blockStart = BlockMeta_GetBlockStart(heap->blockMetaStart,
                                             heap->heapStart, superblockStart);
    } else {
        stride = 1;
        blockStart = Block_GetBlockStartForWord(word);
    }

    word_t *current = word;
    ObjectMeta *currentMeta = wordMeta;
    while (current >= blockStart && ObjectMeta_IsFree(currentMeta)) {
        current -= ALLOCATION_ALIGNMENT_WORDS * stride;
        currentMeta -= stride;
    }
    Object *object = (Object *)current;
    if (ObjectMeta_IsAlive(currentMeta, collectingOld) &&
        word < current + Object_Size(object) / WORD_SIZE) {
        return object;
    } else {
        return NULL;
    }
}

Object *Object_GetUnmarkedObject(Heap *heap, word_t *word, bool collectingOld) {
    BlockMeta *blockMeta =
        Block_GetBlockMeta(heap->blockMetaStart, heap->heapStart, word);

    if (BlockMeta_ContainsLargeObjects(blockMeta)) {
        word = (word_t *)((word_t)word & LARGE_BLOCK_MASK);
    } else {
        word = (word_t *)((word_t)word & ALLOCATION_ALIGNMENT_INVERSE_MASK);
    }

    ObjectMeta *wordMeta = Bytemap_Get(heap->bytemap, word);
    if (ObjectMeta_IsPlaceholder(wordMeta) || ObjectMeta_IsMarked(wordMeta)) {
        return NULL;
    } else if (ObjectMeta_IsAlive(wordMeta, collectingOld)) {
        return (Object *)word;
    } else {
        return Object_getInnerPointer(heap, blockMeta, word, wordMeta);
    }
}


void Object_Mark(Heap *heap, Object *object, ObjectMeta *objectMeta, bool collectingOld) {
    // Mark the object itself
    if(!collectingOld) {
        ObjectMeta_SetMarked(objectMeta);
    } else {
        ObjectMeta_SetAllocated(objectMeta);
    }

    BlockMeta *blockMeta = Block_GetBlockMeta(
        heap->blockMetaStart, heap->heapStart, (word_t *)object);
    if (!BlockMeta_ContainsLargeObjects(blockMeta)) {
        // Mark the block
        word_t *blockStart = Block_GetBlockStartForWord((word_t *)object);
        BlockMeta_Mark(blockMeta);

        // Mark all Lines
        word_t *lastWord = Object_LastWord(object);

        assert(blockMeta == Block_GetBlockMeta(heap->blockMetaStart,
                                               heap->heapStart, lastWord));
        LineMeta *firstLineMeta = Heap_LineMetaForWord(heap, (word_t *)object);
        LineMeta *lastLineMeta = Heap_LineMetaForWord(heap, lastWord);
        assert(firstLineMeta <= lastLineMeta);
        for (LineMeta *lineMeta = firstLineMeta; lineMeta <= lastLineMeta;
             lineMeta++) {
            Line_Mark(lineMeta);
        }
    }
}
