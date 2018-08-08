#include <stdio.h>
#include <setjmp.h>
#include "Marker.h"
#include "Object.h"
#include "Log.h"
#include "State.h"
#include "datastructures/Stack.h"
#include "headers/ObjectHeader.h"
#include "Block.h"

extern word_t *__modules;
extern int __modules_size;
extern word_t **__stack_bottom;

#define LAST_FIELD_OFFSET -1

void Marker_markObject(Heap *heap, Stack *stack, Bytemap *bytemap,
                       Object *object, ObjectMeta *objectMeta, bool collectingOld) {
    assert(Object_Size(object) != 0);
    Object_Mark(heap, object, objectMeta, collectingOld);
    Stack_Push(stack, object);
}

void Marker_markConservative(Heap *heap, Stack *stack, word_t *address, bool collectingOld) {
    assert(Heap_IsWordInHeap(heap, address));
    Object *object = Object_GetUnmarkedObject(heap, address, collectingOld);
    Bytemap *bytemap = heap->bytemap;
    if (object != NULL) {
        ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
        if (ObjectMeta_IsAlive(objectMeta, collectingOld)) {
            Marker_markObject(heap, stack, bytemap,object, objectMeta, collectingOld);
        }
    }
}

void Marker_Mark(Heap *heap, Stack *stack, bool collectingOld) {
    Bytemap *bytemap = heap->bytemap;
    while (!Stack_IsEmpty(stack)) {
        Object *object = Stack_Pop(stack);
        ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
        word_t *bStart = Block_GetBlockStartForWord((word_t *)object);
        BlockMeta *bMeta = Block_GetBlockMeta(bStart, heap->heapStart, (word_t *)object);

        if (Object_IsArray(object)) {
            if (object->rtti->rt.id == __object_array_id) {

                ArrayHeader *arrayHeader = (ArrayHeader *)object;
                size_t length = arrayHeader->length;
                word_t **fields = (word_t **)(arrayHeader + 1);
                for (int i = 0; i < length; i++) {
                    word_t *field = fields[i];
                    if (Heap_IsWordInHeap(heap, field)) {
                        ObjectMeta *fieldMeta = Bytemap_Get(bytemap, field);
                        if (ObjectMeta_IsAlive(fieldMeta, collectingOld)) {
                            Marker_markObject(heap, stack, bytemap, (Object *) field, fieldMeta, collectingOld);
                        }
                    }
                }
            }
            // non-object arrays do not contain pointers
        } else {

            int64_t *ptr_map = object->rtti->refMapStruct;
            int i = 0;
            while (ptr_map[i] != LAST_FIELD_OFFSET) {
                word_t *field = object->fields[ptr_map[i]];
                if (Heap_IsWordInHeap(heap, field)) {
                    ObjectMeta *fieldMeta = Bytemap_Get(bytemap, field);
                    if (ObjectMeta_IsAlive(fieldMeta, collectingOld)) {
                        Marker_markObject(heap, stack, bytemap, (Object *)field, fieldMeta, collectingOld);
                    }
                }
                ++i;
            }
        }
    }
}

void Marker_markProgramStack(Heap *heap, Stack *stack, bool collectingOld) {
    // Dumps registers into 'regs' which is on stack
    jmp_buf regs;
    setjmp(regs);
    word_t *dummy;

    word_t **current = &dummy;
    word_t **stackBottom = __stack_bottom;

    while (current <= stackBottom) {

        word_t *stackObject = *current;
        if (Heap_IsWordInHeap(heap, stackObject)) {
            Marker_markConservative(heap, stack, stackObject, collectingOld);
        }
        current += 1;
    }
}

void Marker_markModules(Heap *heap, Stack *stack, bool collectingOld) {
    word_t **modules = &__modules;
    int nb_modules = __modules_size;
    Bytemap *bytemap = heap->bytemap;
    for (int i = 0; i < nb_modules; i++) {
        Object *object = (Object *)modules[i];
        if (Heap_IsWordInHeap(heap, (word_t *)object)) {
            // is within heap
            ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
            if (ObjectMeta_IsAlive(objectMeta, collectingOld)) {
                Marker_markObject(heap, stack, bytemap, object, objectMeta, collectingOld);
            }
        }
    }
}

void Marker_markRemembered(Heap *heap, Stack *stack) {
    Stack *roots = allocator.rememberedObjects;
    while (!Stack_IsEmpty(roots)) {
        Object *object = (Object *)Stack_Pop(roots);
        Bytemap *bytemap = heap->bytemap;
        if (bytemap != NULL) {
            ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
            assert(ObjectMeta_IsMarked(objectMeta) && ObjectMeta_IsRemembered(objectMeta));
            ObjectMeta_SetUnremembered(objectMeta);
            Stack_Push(stack, object);
        }
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
