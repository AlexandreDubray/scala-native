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

void Marker_Mark(Heap *heap, Stack *stack, bool collectingOld);
void StackOverflowHandler_largeHeapOverflowHeapScan(Heap *heap, Stack *stack);
bool StackOverflowHandler_smallHeapOverflowHeapScan(Heap *heap, Stack *stack);

void Marker_markObject(Heap *heap, Stack *stack, Object *object, bool collectingOld) {
    assert(Object_Size(&object->header) != 0);
    assert((!collectingOld && !Object_IsMarked(&object->header)) ||
           (collectingOld && !Object_IsAllocated(&object->header)));
    Object_Mark(object, collectingOld);
    if (!overflow) {
        overflow = Stack_Push(stack, object);
    }
}

void Marker_mark(Heap *heap, Stack *stack, Object *object, bool collectingOld) {
    if (!collectingOld && !Object_IsMarked(&object->header)) {
        Marker_markObject(heap, stack, object, collectingOld);
    } else if (collectingOld && !Object_IsAllocated(&object->header)) {
        Marker_markObject(heap, stack, object, collectingOld);
    }
}

void Marker_markConservative(Heap *heap, Stack *stack, word_t *address, bool collectingOld) {
    assert(Heap_IsWordInHeap(heap, address));
    Object *object = NULL;
    if (Heap_IsWordInSmallHeap(heap, address)) {
        if (!collectingOld) {
            object = Object_GetYoungObject(address);
        } else {
            object = Object_GetOldObject(address);
        }
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
        if (!collectingOld) {
            object = Object_GetLargeYoungObject(heap->largeAllocator, address);
        } else {
            object = Object_GetLargeOldObject(heap->largeAllocator, address);
        }
    }
    
    if (object != NULL) {
        Marker_mark(heap, stack, object, collectingOld);
    }
}

void Marker_Mark(Heap *heap, Stack *stack, bool collectingOld) {
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

                if (heap_isObjectInHeap(heap, fieldObject)) {
                    Marker_mark(heap, stack, fieldObject, collectingOld);
                }
            }
        } else {
            int64_t *ptr_map = object->rtti->refMapStruct;
            int i = 0;
            while (ptr_map[i] != LAST_FIELD_OFFSET) {
                word_t *field = object->fields[ptr_map[i]];
                Object *fieldObject = Object_FromMutatorAddress(field);
                if (heap_isObjectInHeap(heap, fieldObject)) {
                    Marker_mark(heap, stack, fieldObject, collectingOld);
                }

                ++i;
            }
        }
    }
    StackOverflowHandler_CheckForOverflow();
}

void Marker_markProgramStack(Heap *heap, Stack *stack, bool collectingOld) {
    // Dumps registers into 'regs' which is on stack
    jmp_buf regs;
    setjmp(regs);
    word_t *dummy;

    word_t **current = &dummy;
    word_t **stackBottom = __stack_bottom;

    while (current <= stackBottom) {

        word_t *stackObject = (*current) - WORDS_IN_OBJECT_HEADER;
        if (Heap_IsWordInHeap(heap, stackObject)) {
            Marker_markConservative(heap, stack, stackObject, collectingOld);
        }
        current += 1;
    }
}

void Marker_markModules(Heap *heap, Stack *stack, bool collectingOld) {
    word_t **modules = &__modules;
    int nb_modules = __modules_size;

    for (int i = 0; i < nb_modules; i++) {
        Object *object = Object_FromMutatorAddress(modules[i]);
        if (heap_isObjectInHeap(heap, object)) {
            Marker_mark(heap, stack, object, collectingOld);
        }
    }
}

void Marker_markRemembered(Heap *heap, Stack *stack) {
    Stack *roots = heap->allocator->rememberedObjects;
    while (!Stack_IsEmpty(roots)) {
        Object *object = (Object *)Stack_Pop(roots);

        assert(Object_Size(&object->header) != 0);
        assert(Object_IsRemembered(&object->header));

        Object_SetUnremembered(&object->header);

        overflow = Stack_Push(stack, object);
    }
}

void Marker_MarkRoots(Heap *heap, Stack *stack, bool collectingOld) {

    // We need to trace inter-generational pointer only when we
    // collect the young generation.
    if (!collectingOld) {
        Marker_markRemembered(heap, stack);
    }

    Marker_markProgramStack(heap, stack, collectingOld);

    Marker_markModules(heap, stack, collectingOld);

    Marker_Mark(heap, stack, collectingOld);
}
