#include <stdio.h>
#include <setjmp.h>
#include "Marker.h"
#include "Object.h"
#include "Log.h"
#include "State.h"
#include "datastructures/Stack.h"
#include "headers/ObjectHeader.h"
#include "Block.h"
#include "StackoverflowHandler.h"

extern int __object_array_id;
extern word_t *__modules;
extern int __modules_size;
extern word_t **__stack_bottom;

#define LAST_FIELD_OFFSET -1

void Marker_Mark(Heap *heap, Stack *stack);
void StackOverflowHandler_largeHeapOverflowHeapScan(Heap *heap, Stack *stack);
bool StackOverflowHandler_smallHeapOverflowHeapScan(Heap *heap, Stack *stack);

bool collectingOld = false;

void Marker_markObject(Heap *heap, Stack *stack, Object *object) {
    assert(Object_Size(&object->header) != 0);
    if (!collectingOld) {
        assert(!Object_IsMarked(&object->header));
        Object_Mark(object);
    } else {
        // If we are collecting the old Generation, they are all marked
        // and we unmark them if they are reachable
        assert(Object_IsMarked(&object->header));
        Object_Unmark(object);
    }
    if (!overflow) {
        overflow = Stack_Push(stack, object);
    }
}

void Marker_markConservative(Heap *heap, Stack *stack, word_t *address) {
    assert(Heap_IsWordInHeap(heap, address));
    Object *object = NULL;
    if (Heap_IsWordInSmallHeap(heap, address)) {
        object = Object_GetObject(address);
        assert(
            object == NULL ||
            Line_ContainsObject(&Block_GetBlockHeader((word_t *)object)
                                     ->lineHeaders[Block_GetLineIndexFromWord(
                                         Block_GetBlockHeader((word_t *)object),
                                         (word_t *)object)]));
#ifdef DEBUG_PRINT
        if (object == NULL) {
            printf("Not found: %p\n", address);
        }
#endif
    } else {
        object = Object_GetLargeObject(heap->largeAllocator, address);
    }

    if (!collectingOld) {
        if (object != NULL && !Object_IsMarked(&object->header)) {
            Marker_markObject(heap, stack, object);
        }
    } else {
        if (object != NULL && Object_IsMarked(&object->header)) {
            Marker_markObject(heap, stack, object);
        }
    }
}

void Marker_Mark(Heap *heap, Stack *stack) {
    while (!Stack_IsEmpty(stack)) {
        Object *object = Stack_Pop(stack);

        if (object->rtti->rt.id == __object_array_id) {
            // remove header and rtti from size
            size_t size =
                Object_Size(&object->header) - OBJECT_HEADER_SIZE - WORD_SIZE;
            size_t nbWords = size / WORD_SIZE;
            for (int i = 0; i < nbWords; i++) {

                word_t *field = object->fields[i];
                Object *fieldObject = Object_FromMutatorAddress(field);

                if (!collectingOld) {
                    if (heap_isObjectInHeap(heap, fieldObject) &&
                        !Object_IsMarked(&fieldObject->header)) {
                        Marker_markObject(heap, stack, fieldObject);
                    }
                } else {
                    if (heap_isObjectInHeap(heap, fieldObject) &&
                        Object_IsMarked(&fieldObject->header)) {
                        Marker_markObject(heap, stack, fieldObject);
                    }
                }
            }
        } else {
            int64_t *ptr_map = object->rtti->refMapStruct;
            int i = 0;
            while (ptr_map[i] != LAST_FIELD_OFFSET) {
                word_t *field = object->fields[ptr_map[i]];
                Object *fieldObject = Object_FromMutatorAddress(field);

                if (!collectingOld) {
                    if (heap_isObjectInHeap(heap, fieldObject) &&
                        !Object_IsMarked(&fieldObject->header)) {
                        Marker_markObject(heap, stack, fieldObject);
                    }
                } else {
                    if (heap_isObjectInHeap(heap, fieldObject) &&
                        Object_IsMarked(&fieldObject->header)) {
                        Marker_markObject(heap, stack, fieldObject);
                    }
                }

                ++i;
            }
        }
    }
    StackOverflowHandler_CheckForOverflow();
}

void Marker_markProgramStack(Heap *heap, Stack *stack) {
    // Dumps registers into 'regs' which is on stack
    jmp_buf regs;
    setjmp(regs);
    word_t *dummy;

    word_t **current = &dummy;
    word_t **stackBottom = __stack_bottom;

    while (current <= stackBottom) {

        word_t *stackObject = (*current) - WORDS_IN_OBJECT_HEADER;
        if (Heap_IsWordInHeap(heap, stackObject)) {
            Marker_markConservative(heap, stack, stackObject);
        }
        current += 1;
    }
}

void Marker_markModules(Heap *heap, Stack *stack) {
    word_t **modules = &__modules;
    int nb_modules = __modules_size;

    for (int i = 0; i < nb_modules; i++) {
        Object *object = Object_FromMutatorAddress(modules[i]);
        if (heap_isObjectInHeap(heap, object) &&
            !Object_IsMarked(&object->header)) {
            Marker_markObject(heap, stack, object);
        }
    }
}

void Marker_markOld(Heap *heap, Stack *stack) {
    Stack *roots = heap->allocator->oldObjectToRoot;
    Bitmap *bitmap = heap->allocator->oldObjectDirty;
    while (!Stack_IsEmpty(roots)) {
        Object *object = (Object *)Stack_Pop(roots);
        if (Bitmap_GetBit(bitmap, (void *)object)) {
            Bitmap_ClearBit(bitmap, (void *)object);

            assert(Object_Size(&object->header) != 0);

            if (!overflow) {
                overflow = Stack_Push(stack, object);
            }
        }
    }
    StackOverflowHandler_CheckForOverflow();
}

void Marker_MarkRoots(Heap *heap, Stack *stack) {

    Marker_markProgramStack(heap, stack);

    Marker_markModules(heap, stack);

    // We need to trace inter-generational pointer only when we
    // collect the young generation.
    if (!collectingOld) {
        Marker_markOld(heap, stack);
    }

    Marker_Mark(heap, stack);
}
