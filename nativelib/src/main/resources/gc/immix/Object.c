#include <stddef.h>
#include <stdio.h>
#include "Object.h"
#include "headers/BlockHeader.h"
#include "Line.h"
#include "Log.h"
#include "utils/MathUtils.h"
#include "Marker.h"

extern int __object_array_id;
#define LAST_FIELD_OFFSET -1

Object *Object_NextLargeObject(Object *object) {
    size_t size = Object_ChunkSize(object);
    assert(size != 0);
    return (Object *)((ubyte_t *)object + size);
}

Object *Object_NextObject(Object *object) {
    size_t size = Object_Size(&object->header);
    assert(size < LARGE_BLOCK_SIZE);
    if (size == 0) {
        return NULL;
    }
    Object *next = (Object *)((ubyte_t *)object + size);
    assert(Block_GetBlockHeader((word_t *)next) ==
               Block_GetBlockHeader((word_t *)object) ||
           (ubyte_t *)Block_GetBlockHeader((word_t *)next) ==
               (ubyte_t *)Block_GetBlockHeader((word_t *)object) +
                   BLOCK_TOTAL_SIZE);
    return next;
}

static inline bool isWordAligned(word_t *word) {
    return ((word_t)word & WORD_INVERSE_MASK) == (word_t)word;
}

Object *Object_getInLine(BlockHeader *blockHeader, int lineIndex,
                         word_t *word, bool youngObject) {
    assert(Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex)));

    Object *current =
        Line_GetFirstObject(Block_GetLineHeader(blockHeader, lineIndex));
    Object *next = Object_NextObject(current);

    word_t *lineEnd =
        Block_GetLineAddress(blockHeader, lineIndex) + WORDS_IN_LINE;

    while (next != NULL && (word_t *)next < lineEnd && (word_t *)next <= word) {
        current = next;
        next = Object_NextObject(next);
    }

    if ((youngObject && Object_IsAllocated(&current->header)) || (!youngObject && Object_IsMarked(&current->header))) {
        if (word >= (word_t *)current && word < (word_t *)next) {
#ifdef DEBUG_PRINT
            if ((word_t *)current != word) {
                printf("inner pointer: %p object: %p\n", word, current);
                fflush(stdout);
            }
#endif
            return current;
        }
#ifdef DEBUG_PRINT
        printf("ignoring %p\n", word);
        fflush(stdout);
#endif
    }
    return NULL;
}

Object *Object_findObject(word_t *word, bool youngObject) {
    BlockHeader *blockHeader = Block_GetBlockHeader(word);

    // Check if the word points on the block header
    if (word < Block_GetFirstWord(blockHeader)) {
#ifdef DEBUG_PRINT
        printf("Points on block header %p\n", word);
        fflush(stdout);
#endif
        return NULL;
    }

    if (!isWordAligned(word)) {
#ifdef DEBUG_PRINT
        printf("Word not aligned: %p aligning to %p\n", word,
               (word_t *)((word_t)word & WORD_INVERSE_MASK));
        fflush(stdout);
#endif
        word = (word_t *)((word_t)word & WORD_INVERSE_MASK);
    }

    int lineIndex = Block_GetLineIndexFromWord(blockHeader, word);
    while (lineIndex > 0 &&
           !Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex))) {
        lineIndex--;
    }

    if (Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex))) {
        return Object_getInLine(blockHeader, lineIndex, word, youngObject);
    } else {
#ifdef DEBUG_PRINT
        printf("Word points to empty line %p\n", word);
        fflush(stdout);
#endif
        return NULL;
    }
}

Object *Object_GetYoungObject(word_t *word) {
    return Object_findObject(word, true);
}

Object *Object_GetOldObject(word_t *word) {
    return Object_findObject(word, false);
}

Object *Object_GetObject(word_t *word) {
    Object *object = Object_GetYoungObject(word);
    if (object == NULL) {
        object = Object_GetOldObject(word);
    }
    return object;
}

Object *Object_getLargeInnerPointer(LargeAllocator *allocator, word_t *word) {
    word_t *current = (word_t *)((word_t)word & LARGE_BLOCK_MASK);

    while (!Bitmap_GetBit(allocator->bitmap, (ubyte_t *)current)) {
        current -= LARGE_BLOCK_SIZE / WORD_SIZE;
    }

    Object *object = (Object *)current;
    if (word < (word_t *)object + Object_ChunkSize(object) / WORD_SIZE &&
        object->rtti != NULL) {
#ifdef DEBUG_PRINT
        printf("large inner pointer: %p, object: %p\n", word, object);
        fflush(stdout);
#endif
        return object;
    } else {

        return NULL;
    }
}

Object *Object_findLargeObject(LargeAllocator *allocator, word_t *word, bool youngObject) {
    if (((word_t)word & LARGE_BLOCK_MASK) != (word_t)word) {
        word = (word_t *)((word_t)word & LARGE_BLOCK_MASK);
    }
    if (Bitmap_GetBit(allocator->bitmap, (ubyte_t *)word) &&
        ((youngObject && Object_IsAllocated(&((Object *)word)->header)) || (!youngObject && Object_IsMarked(&((Object *)word)->header)))) {
        return (Object *)word;
    } else {
        Object *object = Object_getLargeInnerPointer(allocator, word);
        assert(object == NULL ||
               (word >= (word_t *)object &&
                word < (word_t *)Object_NextLargeObject(object)));
        return object;
    }
}

Object *Object_GetLargeYoungObject(LargeAllocator *allocator, word_t *word) {
    return Object_findLargeObject(allocator, word, true);
}

Object *Object_GetLargeOldObject(LargeAllocator *allocator, word_t *word) {
    return Object_findLargeObject(allocator, word, false);
}

Object *Object_GetLargeObject(LargeAllocator *allocator, word_t *word) {
    Object *object = Object_GetLargeYoungObject(allocator,word);
    if (object == NULL) {
        object = Object_GetLargeOldObject(allocator, word);
    }
    return object;
}

void Object_Mark(Object *object, bool collectingOld) {
    // Mark the object itself
    if (!collectingOld) {
        Object_MarkObjectHeader(&object->header);
    } else {
        Object_SetAllocated(&object->header);
    }

    if (!Object_IsLargeObject(&object->header)) {
        // Mark the block
        BlockHeader *blockHeader = Block_GetBlockHeader((word_t *)object);
        Block_Mark(blockHeader);

        // Mark all Lines
        int startIndex =
            Block_GetLineIndexFromWord(blockHeader, (word_t *)object);
        word_t *lastWord = (word_t *)Object_NextObject(object) - 1;
        int endIndex = Block_GetLineIndexFromWord(blockHeader, lastWord);
        assert(startIndex >= 0 && startIndex < LINE_COUNT);
        assert(endIndex >= 0 && endIndex < LINE_COUNT);
        assert(startIndex <= endIndex);
        for (int i = startIndex; i <= endIndex; i++) {
            LineHeader *lineHeader = Block_GetLineHeader(blockHeader, i);
            Line_Mark(lineHeader);
        }
    }
}

bool Object_HasPointerToOldObject(Heap *heap, Object *object) {
    BlockHeader *currentBlockHeader = Block_GetBlockHeader((word_t *)object);
    if (object->rtti->rt.id == __object_array_id) {
        // remove header and rtti from size
        size_t size =
            Object_Size(&object->header) - OBJECT_HEADER_SIZE - WORD_SIZE;
        size_t nbWords = size / WORD_SIZE;
        for (int i = 0; i < nbWords; i++) {

            word_t *field = object->fields[i];
            Object *fieldObject = Object_FromMutatorAddress(field);
            if (heap_isObjectInHeap(heap, fieldObject)) {
                if (Object_IsLargeObject(&fieldObject->header)) {
                    if (Object_IsMarked(&fieldObject->header)) {
                        return true;
                    }
                } else {
                    BlockHeader *blockHeader = Block_GetBlockHeader((word_t *)fieldObject);
                    if (Block_IsOld(blockHeader) || ( currentBlockHeader < blockHeader && Block_IsMarked(blockHeader) && (Block_GetAge(blockHeader) == MAX_AGE_YOUNG_OBJECT - 1))) {
                        return true;
                    }
                }
            }
        }
    } else {
        int64_t *ptr_map = object->rtti->refMapStruct;
        int i = 0;
        while (ptr_map[i] != LAST_FIELD_OFFSET) {
            word_t *field = object->fields[ptr_map[i]];
            Object *fieldObject = Object_FromMutatorAddress(field);
            if (heap_isObjectInHeap(heap, fieldObject)) {
                if (Object_IsLargeObject(&fieldObject->header)) {
                    if (Object_IsMarked(&fieldObject->header)) {
                        return true;
                    }
                } else {
                    BlockHeader *blockHeader = Block_GetBlockHeader((word_t *)fieldObject);
                    if (Block_IsOld(blockHeader) || (currentBlockHeader < blockHeader && Block_IsMarked(blockHeader) && Block_GetAge(blockHeader) == MAX_AGE_YOUNG_OBJECT - 1)) {
                        return true;
                    }
                }
            }
            ++i;
        }
    }
    return false;

}

bool Object_HasPointerToYoungObject(Heap *heap, Object *object) {
    BlockHeader *currentBlockHeader = Block_GetBlockHeader((word_t *)object);
    if (object->rtti->rt.id == __object_array_id) {
        // remove header and rtti from size
        size_t size =
            Object_Size(&object->header) - OBJECT_HEADER_SIZE - WORD_SIZE;
        size_t nbWords = size / WORD_SIZE;
        for (int i = 0; i < nbWords; i++) {

            word_t *field = object->fields[i];
            Object *fieldObject = Object_FromMutatorAddress(field);
            if (!heap_isObjectInHeap(heap, fieldObject)) {
                continue;
            }
            // At the moment, large object are still promoted in masse after
            // first collection. So when collecting old gen, large object can only be old
            if (!Object_IsLargeObject(&fieldObject->header)) {
                BlockHeader *blockHeader = Block_GetBlockHeader((word_t *)fieldObject);
                if ((word_t *)object > (word_t *)blockHeader && !Block_IsOld(blockHeader)) {
                    return true;
                } else if (currentBlockHeader < blockHeader && Block_IsMarked(blockHeader) && (Block_GetAge(blockHeader) < MAX_AGE_YOUNG_OBJECT - 1)) {
                    return true;
                }
            }
        }
    } else {
        int64_t *ptr_map = object->rtti->refMapStruct;
        int i = 0;
        while (ptr_map[i] != LAST_FIELD_OFFSET) {
            word_t *field = object->fields[ptr_map[i]];
            Object *fieldObject = Object_FromMutatorAddress(field);
            if (!heap_isObjectInHeap(heap, fieldObject)) {
                ++i;
                continue;
            }
            if (!Object_IsLargeObject(&fieldObject->header)) {
                BlockHeader *blockHeader = Block_GetBlockHeader((word_t *)fieldObject);
                if ((word_t *)object > (word_t *)blockHeader && !Block_IsOld(blockHeader)) {
                    return true;
                } else if (currentBlockHeader < blockHeader && Block_IsMarked(blockHeader) && (Block_GetAge(blockHeader) < MAX_AGE_YOUNG_OBJECT - 1)) {
                    return true;
                }
            }
            ++i;
        }
    }
    return false;

}

size_t Object_ChunkSize(Object *object) {
    return MathUtils_RoundToNextMultiple(Object_Size(&object->header),
                                         MIN_BLOCK_SIZE);
}
