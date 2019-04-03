#include <stdio.h>
#include <setjmp.h>
#include "Marker.h"
#include "Object.h"
#include "Log.h"
#include "State.h"
#include "headers/ObjectHeader.h"
#include "datastructures/GreyPacket.h"
#include "GCThread.h"
#include <sched.h>

extern word_t *__modules;
extern int __modules_size;
extern word_t **__stack_bottom;

#define LAST_FIELD_OFFSET -1

// Marking is done using grey packets. A grey packet is a fixes size list that
// contains pointers to objects for marking.
//
// Each marker has a grey packet with references to check ("in" packet).
// When it finds a new unmarked object the marker puts a pointer to
// it in the "out" packet. When the "in" packet is empty it gets
// another from the full packet list and returns the empty one to the empty
// packet list. Similarly, when the "out" packet get full, marker gets another
// empty packet and pushes the full one on the full packet list.
//
// Marking is done when all the packets are empty and in the empty packet list.

static inline GreyPacket *Marker_takeEmptyPacket(Heap *heap, Stats *stats) {
    Stats_RecordTimeSync(stats, start_ns);
    GreyPacket *packet =
        GreyList_Pop(&heap->mark.empty, heap->greyPacketsStart);
    Stats_RecordTimeSync(stats, end_ns);
    Stats_RecordEventSync(stats, event_sync, start_ns, end_ns);
    if (packet != NULL) {
        // Another thread setting size = 0 might not arrived, just write it now.
        // Avoiding a memfence.
        packet->size = 0;
        packet->type = grey_packet_reflist;
    }
    assert(packet != NULL);
    return packet;
}

static inline GreyPacket *Marker_takeFullPacket(Heap *heap, Stats *stats) {
    Stats_RecordTimeSync(stats, start_ns);
    GreyPacket *packet = GreyList_Pop(&heap->mark.full, heap->greyPacketsStart);
    if (packet != NULL) {
        atomic_thread_fence(memory_order_release);
    }
    Stats_RecordTimeSync(stats, end_ns);
    Stats_RecordEventSync(stats, event_sync, stats->mark_waiting_start_ns,
                          end_ns);
    if (packet == NULL) {
        Stats_MarkerNoFullPacket(stats, start_ns, end_ns);
    } else {
        Stats_MarkerGotFullPacket(stats, end_ns);
    }
    assert(packet == NULL || packet->type == grey_packet_refrange ||
           packet->size > 0);
    return packet;
}

static inline void Marker_giveEmptyPacket(Heap *heap, Stats *stats,
                                          GreyPacket *packet) {
    assert(packet->size == 0);
    // no memfence needed see Marker_takeEmptyPacket
    Stats_RecordTimeSync(stats, start_ns);
    GreyList_Push(&heap->mark.empty, heap->greyPacketsStart, packet);
    Stats_RecordTimeSync(stats, end_ns);
    Stats_RecordEventSync(stats, event_sync, start_ns, end_ns);
}

static inline void Marker_giveFullPacket(Heap *heap, Stats *stats,
                                         GreyPacket *packet) {
    assert(packet->type == grey_packet_refrange || packet->size > 0);
    // make all the contents visible to other threads
    atomic_thread_fence(memory_order_acquire);
    assert(GreyList_Size(&heap->mark.full) <= heap->mark.total);
    Stats_RecordTimeSync(stats, start_ns);
    GreyList_Push(&heap->mark.full, heap->greyPacketsStart, packet);
    Stats_RecordTimeSync(stats, end_ns);
    Stats_RecordEventSync(stats, event_sync, start_ns, end_ns);
}

static inline void Marker_rememberOldObject(Heap *heap, Stats *stats, Object *object) {
    if (!GreyPacket_Push(heap->mark.oldRoots, object)) {
        atomic_thread_fence(memory_order_acquire);
        GreyList_Push(&heap->mark.rememberedOld, heap->greyPacketsStart, heap->mark.oldRoots);
        heap->mark.oldRoots = Marker_takeEmptyPacket(heap, stats);
        GreyPacket_Push(heap->mark.oldRoots, object);
    }
}

static inline void Marker_rememberYoungObject(Heap *heap, Stats *stats, Object *object) {
    if (!GreyPacket_Push(heap->mark.youngRoots, object)) {
        atomic_thread_fence(memory_order_acquire);
        GreyList_Push(&heap->mark.rememberedYoung, heap->greyPacketsStart, heap->mark.youngRoots);
        heap->mark.youngRoots = Marker_takeEmptyPacket(heap, stats);
        GreyPacket_Push(heap->mark.youngRoots, object);
    }
}

void Marker_markObject(Heap *heap, Stats *stats, GreyPacket **outHolder,
                       Bytemap *bytemap, Object *object,
                       ObjectMeta *objectMeta, bool collectingOld) {
    assert(ObjectMeta_IsAllocated(objectMeta) ||
           ObjectMeta_IsMarked(objectMeta));

    assert(Object_Size(object) != 0);
    Object_Mark(heap, object, objectMeta, collectingOld);

    GreyPacket *out = *outHolder;
    if (!GreyPacket_Push(out, object)) {
        Marker_giveFullPacket(heap, stats, out);
        *outHolder = out = Marker_takeEmptyPacket(heap, stats);
        GreyPacket_Push(out, object);
    }
}

void Marker_markConservative(Heap *heap, Stats *stats, GreyPacket **outHolder,
                             word_t *address,bool collectingOld) {
    assert(Heap_IsWordInHeap(heap, address));
    Object *object = Object_GetUnmarkedObject(heap, address, collectingOld);
    Bytemap *bytemap = heap->bytemap;
    if (object != NULL) {
        ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
        if (ObjectMeta_IsAlive(objectMeta, collectingOld)) {
            Marker_markObject(heap, stats, outHolder, bytemap, object,
                              objectMeta, collectingOld);
        }
    }
}

int Marker_markRange(Heap *heap, Stats *stats, Object *object, GreyPacket **outHolder,
                     Bytemap *bytemap, word_t **fields, size_t length, bool collectingOld) {
    // if the object has pointer to old object and is young after collection we
    // need to store it in allocator->rememberedYoungObject
    //
    // if the object has pointer to young object and is old after collection we
    // need to store it in allocator->rememberedYoungObject
    ObjectMeta *objectMeta = Bytemap_Get(heap->bytemap, (word_t *)object);
    ObjectMeta_SetUnremembered(objectMeta);

    BlockMeta *blockMeta = Block_GetBlockMeta(heap->blockMetaStart, heap->heapStart, (word_t *)object);
    if (BlockMeta_ContainsLargeObjects(blockMeta)) {
        blockMeta = BlockMeta_GetSuperblockStart(heap->blockMetaStart, blockMeta);
    }

    bool hasPointerToOld = false;
    bool hasPointerToYoung = false;
    bool willBeOld = BlockMeta_IsOld(blockMeta) || (BlockMeta_GetAge(blockMeta) == MAX_AGE_YOUNG_BLOCK - 1);

    int objectsTraced = 0;
    word_t **limit = fields + length;
    for (word_t **current = fields; current < limit; current++) {
        word_t *field = *current;
        if (Heap_IsWordInHeap(heap, field)) {
            ObjectMeta *fieldMeta = Bytemap_Get(bytemap, field);

            if (ObjectMeta_IsAlive(fieldMeta, collectingOld)) {
                BlockMeta *blockFieldMeta = Block_GetBlockMeta(heap->blockMetaStart, heap->heapStart, field);
                if (BlockMeta_ContainsLargeObjects(blockFieldMeta)) {
                    blockFieldMeta = BlockMeta_GetSuperblockStart(heap->blockMetaStart, blockFieldMeta);
                }

                // if BlockMeta_GetAge(blockFieldMeta) == MAX_AGE_YOUNG_BLOCK || BlockMeta_GetAge(blockFieldMeta) == MAX_AGE_YOUNG_BLOCK-1
                if (BlockMeta_GetAge(blockFieldMeta) >= MAX_AGE_YOUNG_BLOCK - 1) {
                    hasPointerToOld = true;
                } else {
                    hasPointerToYoung = true;
                }
                Marker_markObject(heap, stats, outHolder, bytemap,
                                  (Object *)field, fieldMeta, collectingOld);
            }
            objectsTraced += 1;
        }
    }

    if (willBeOld && hasPointerToYoung && !ObjectMeta_IsRemembered(objectMeta)) {
        ObjectMeta_SetRemembered(objectMeta);
        Marker_rememberOldObject(heap, stats, object);
    } else if (!willBeOld && hasPointerToOld && !ObjectMeta_IsRemembered(objectMeta)) {
        ObjectMeta_SetRemembered(objectMeta);
        Marker_rememberYoungObject(heap, stats, object);
    }
    return objectsTraced;
}

int Marker_markRegularObject(Heap *heap, Stats *stats, Object *object,
                             GreyPacket **outHolder, Bytemap *bytemap, bool collectingOld) {
    // if the object has pointer to old object and is young after collection we
    // need to store it in heap->rememberedYoungObject
    //
    // if the object has pointer to young object and is old after collection we
    // need to store it in heap->rememberedOldObject
    ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
    ObjectMeta_SetUnremembered(objectMeta);

    BlockMeta *blockMeta = Block_GetBlockMeta(heap->blockMetaStart, heap->heapStart, (word_t *)object);
    if (BlockMeta_ContainsLargeObjects(blockMeta)) {
        blockMeta = BlockMeta_GetSuperblockStart(heap->blockMetaStart, blockMeta);
    }

    bool hasPointerToOld = false;
    bool hasPointerToYoung = false;

    bool willBeOld = BlockMeta_IsOld(blockMeta) || (BlockMeta_GetAge(blockMeta) == MAX_AGE_YOUNG_BLOCK - 1);

    int objectsTraced = 0;
    int64_t *ptr_map = object->rtti->refMapStruct;
    for (int64_t *current = ptr_map; *current != LAST_FIELD_OFFSET; current++) {
        word_t *field = object->fields[*current];
        if (Heap_IsWordInHeap(heap, field)) {
            ObjectMeta *fieldMeta = Bytemap_Get(bytemap, field);

            if (ObjectMeta_IsAlive(fieldMeta, collectingOld)) {
                BlockMeta *blockFieldMeta = Block_GetBlockMeta(heap->blockMetaStart, heap->heapStart, field);

                if (BlockMeta_ContainsLargeObjects(blockFieldMeta)) {
                    blockFieldMeta = BlockMeta_GetSuperblockStart(heap->blockMetaStart, blockFieldMeta);
                }

                // if BlockMeta_GetAge(blockFieldMeta) == MAX_AGE_YOUNG_BLOCK || BlockMeta_GetAge(blockFieldMeta) == MAX_AGE_YOUNG_BLOCK-1
                if (BlockMeta_GetAge(blockFieldMeta) >= MAX_AGE_YOUNG_BLOCK - 1) {
                    hasPointerToOld = true;
                } else {
                    hasPointerToYoung = true;
                }

                Marker_markObject(heap, stats, outHolder, bytemap,
                                  (Object *)field, fieldMeta, collectingOld);
            }
            objectsTraced += 1;
        }
    }

    if (willBeOld && hasPointerToYoung) {
        assert(!ObjectMeta_IsRemembered(objectMeta));
        ObjectMeta_SetRemembered(objectMeta);
        Marker_rememberOldObject(heap, stats, object);
    } else if (!willBeOld && hasPointerToOld) {
        assert(!ObjectMeta_IsRemembered(objectMeta));
        ObjectMeta_SetRemembered(objectMeta);
        Marker_rememberYoungObject(heap, stats, object);
    }
    return objectsTraced;
}

int Marker_splitObjectArray(Heap *heap, Stats *stats, Object *object, GreyPacket **outHolder,
                            Bytemap *bytemap, word_t **fields, size_t length, bool collectingOld) {
    word_t **limit = fields + length;
    word_t **lastBatch =
        fields + (length / ARRAY_SPLIT_BATCH) * ARRAY_SPLIT_BATCH;

    assert(lastBatch <= limit);
    for (word_t **batchFields = fields; batchFields < limit;
         batchFields += ARRAY_SPLIT_BATCH) {
        GreyPacket *slice = Marker_takeEmptyPacket(heap, stats);
        assert(slice != NULL);
        slice->type = grey_packet_refrange;
        slice->items[0] = (Stack_Type)batchFields;
        slice->items[1] = (Stack_Type)object;
        // no point writing the size, because it is constant
        Marker_giveFullPacket(heap, stats, slice);
    }

    size_t lastBatchSize = limit - lastBatch;
    int objectsTraced = 0;
    if (lastBatchSize > 0) {
        objectsTraced = Marker_markRange(heap, stats, object, outHolder, bytemap,
                                         lastBatch, lastBatchSize, collectingOld);
    }
    return objectsTraced;
}

int Marker_markObjectArray(Heap *heap, Stats *stats, Object *object,
                            GreyPacket **outHolder, Bytemap *bytemap, bool collectingOld) {
    ArrayHeader *arrayHeader = (ArrayHeader *)object;
    size_t length = arrayHeader->length;
    word_t **fields = (word_t **)(arrayHeader + 1);
    int objectsTraced;
    if (length <= ARRAY_SPLIT_THRESHOLD) {
        objectsTraced =
            Marker_markRange(heap, stats, object, outHolder, bytemap, fields, length, collectingOld);
    } else {
        // object array is two large, split it into pieces for multiple threads
        // to handle
        objectsTraced = Marker_splitObjectArray(heap, stats, object, outHolder, bytemap,
                                                fields, length, collectingOld);
    }
    return objectsTraced;
}

static inline void Marker_splitIncomingPacket(Heap *heap, Stats *stats, GreyPacket *in) {
    int toMove = in->size / 2;
    if (toMove > 0) {
        GreyPacket *slice = Marker_takeEmptyPacket(heap, stats);
        assert(slice != NULL);
        GreyPacket_MoveItems(in, slice, toMove);
        Marker_giveFullPacket(heap, stats, slice);
    }
}

void Marker_markPacket(Heap *heap, Stats *stats, GreyPacket *in,
                       GreyPacket **outHolder, bool collectingOld) {
    Bytemap *bytemap = heap->bytemap;
    int objectsTraced = 0;
    if (*outHolder == NULL) {
        GreyPacket *fresh = Marker_takeEmptyPacket(heap, stats);
        assert(fresh != NULL);
        *outHolder = fresh;
    }
    while (!GreyPacket_IsEmpty(in)) {
        Object *object = GreyPacket_Pop(in);
        ObjectMeta *objectMeta = Bytemap_Get(heap->bytemap, (word_t *)object);
        // We can have garbage object due to the write barrier. See write_barrier_sweep.
        if (!ObjectMeta_IsFree(objectMeta)) {
            if (Object_IsArray(object)) {
                if (object->rtti->rt.id == __object_array_id) {
                    objectsTraced += Marker_markObjectArray(heap, stats, object,
                                                            outHolder, bytemap, collectingOld);
                }
                // non-object arrays do not contain pointers
            } else {
                objectsTraced += Marker_markRegularObject(heap, stats, object,
                                                          outHolder, bytemap, collectingOld);
            }
            if (objectsTraced > MARK_MAX_WORK_PER_PACKET) {
                // the packet has a lot of work split the remainder in two
                Marker_splitIncomingPacket(heap, stats, in);
                objectsTraced = 0;
            }
        }
    }
}

void Marker_markRangePacket(Heap *heap, Stats *stats, GreyPacket *in,
                            GreyPacket **outHolder, bool collectingOld) {
    Bytemap *bytemap = heap->bytemap;
    if (*outHolder == NULL) {
        GreyPacket *fresh = Marker_takeEmptyPacket(heap, stats);
        assert(fresh != NULL);
        *outHolder = fresh;
    }
    word_t **fields = (word_t **)in->items[0];
    Object *object = (Object *)in->items[1];
    Marker_markRange(heap, stats, object, outHolder, bytemap, fields,
                     ARRAY_SPLIT_BATCH, collectingOld);
    in->type = grey_packet_reflist;
    in->size = 0;
}

static inline void Marker_markBatch(Heap *heap, Stats *stats, GreyPacket *in,
                                    GreyPacket **outHolder,bool collectingOld) {
    Stats_RecordTimeBatch(stats, start_ns);
    switch (in->type) {
    case grey_packet_reflist:
        Marker_markPacket(heap, stats, in, outHolder, collectingOld);
        break;
    case grey_packet_refrange:
        Marker_markRangePacket(heap, stats, in, outHolder, collectingOld);
        break;
    }
    Stats_RecordTimeBatch(stats, end_ns);
    Stats_RecordEventBatches(stats, event_mark_batch, start_ns, end_ns);
}

void Marker_Mark(Heap *heap, Stats *stats, bool collectingOld) {
    GreyPacket *in = Marker_takeFullPacket(heap, stats);
    GreyPacket *out = NULL;
    while (in != NULL) {
        Marker_markBatch(heap, stats, in, &out, collectingOld);

        assert(out != NULL);
        assert(GreyPacket_IsEmpty(in));
        GreyPacket *next = Marker_takeFullPacket(heap, stats);
        if (next != NULL) {
            Marker_giveEmptyPacket(heap, stats, in);
        } else {
            if (!GreyPacket_IsEmpty(out)) {
                // use the out packet as source
                next = out;
                out = in;
            } else {
                // next == NULL, exits
                Marker_giveEmptyPacket(heap, stats, in);
                Marker_giveEmptyPacket(heap, stats, out);
            }
        }
        in = next;
    }
}

void Marker_MarkAndScale(Heap *heap, Stats *stats, bool collectingOld) {
    GreyPacket *in = Marker_takeFullPacket(heap, stats);
    GreyPacket *out = NULL;
    while (in != NULL) {
        Marker_markBatch(heap, stats, in, &out, collectingOld);

        assert(out != NULL);
        assert(GreyPacket_IsEmpty(in));
        GreyPacket *next = Marker_takeFullPacket(heap, stats);
        if (next != NULL) {
            Marker_giveEmptyPacket(heap, stats, in);
            uint32_t remainingFullPackets = next->next.sep.size;
            // Make sure than enough worker threads are running
            // given the number of packets available.
            // They will automatically stop if they run out of full packets.
            // If too many threads are started only a fraction of them would
            // get a packet and do useful work. Others would add unnecessary
            // overhead by checking the list of full packets.
            GCThread_ScaleMarkerThreads(heap, remainingFullPackets);
        } else {
            if (!GreyPacket_IsEmpty(out)) {
                // use the out packet as source
                next = out;
                out = in;
            } else {
                // next == NULL, exits
                Marker_giveEmptyPacket(heap, stats, in);
                Marker_giveEmptyPacket(heap, stats, out);
            }
        }
        in = next;
    }
}

void Marker_MarkUtilDone(Heap *heap, Stats *stats, bool collectingOld) {
    while (!Marker_IsMarkDone(heap)) {
        Marker_Mark(heap, stats, collectingOld);
        if (!Marker_IsMarkDone(heap)) {
            sched_yield();
        }
    }
#ifdef DEBUG_ASSERT
    assert(GreyList_Size(&heap->mark.full) == 0);
#endif
}

void Marker_markProgramStack(Heap *heap, Stats *stats, GreyPacket **outHolder, bool collectingOld) {
    // Dumps registers into 'regs' which is on stack
    jmp_buf regs;
    setjmp(regs);
    word_t *dummy;

    word_t **current = &dummy;
    word_t **stackBottom = __stack_bottom;

    while (current <= stackBottom) {

        word_t *stackObject = *current;
        if (Heap_IsWordInHeap(heap, stackObject)) {
            Marker_markConservative(heap, stats, outHolder, stackObject, collectingOld);
        }
        current += 1;
    }
}

void Marker_markModules(Heap *heap, Stats *stats, GreyPacket **outHolder, bool collectingOld) {
    word_t **modules = &__modules;
    int nb_modules = __modules_size;
    Bytemap *bytemap = heap->bytemap;
    word_t **limit = modules + nb_modules;
    for (word_t **current = modules; current < limit; current++) {
        Object *object = (Object *)*current;
        if (Heap_IsWordInHeap(heap, (word_t *)object)) {
            // is within heap
            ObjectMeta *objectMeta = Bytemap_Get(bytemap, (word_t *)object);
            if (ObjectMeta_IsAlive(objectMeta, collectingOld)) {
                Marker_markObject(heap, stats, outHolder, bytemap, object,
                                  objectMeta, collectingOld);
            }
        }
    }
}

void Marker_MarkRoots(Heap *heap, Stats *stats, bool collectingOld) {
    GreyPacket *out = Marker_takeEmptyPacket(heap, stats);
    Marker_markProgramStack(heap, stats, &out, collectingOld);
    Marker_markModules(heap, stats, &out, collectingOld);
    Marker_giveFullPacket(heap, stats, out);
}


bool Marker_IsMarkDone(Heap *heap) {
    // We save grey packets for the two remembered sets, and 1 for each pointer in heap->mark.{old, young}Roots
    return GreyList_Size(&heap->mark.empty) == heap->mark.total - (GreyList_Size(&heap->mark.rememberedOld) + GreyList_Size(&heap->mark.rememberedYoung) + 2);
}
