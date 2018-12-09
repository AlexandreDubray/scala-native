#ifndef IMMIX_OBJECTMETA_H
#define IMMIX_OBJECTMETA_H

#define REMEMBERED_MASK 0x8

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    om_free = 0x0,
    om_placeholder = 0x1,
    om_allocated = 0x2,
    om_marked = 0x4,
} Flag;

typedef ubyte_t ObjectMeta;

static inline bool ObjectMeta_IsFree(ObjectMeta *metadata) {
    return (*metadata & ~REMEMBERED_MASK) == om_free;
}

static inline bool ObjectMeta_IsPlaceholder(ObjectMeta *metadata) {
    return (*metadata & ~REMEMBERED_MASK) == om_placeholder;
}

static inline bool ObjectMeta_IsAllocated(ObjectMeta *metadata) {
    return (*metadata & ~REMEMBERED_MASK) == om_allocated;
}

static inline bool ObjectMeta_IsMarked(ObjectMeta *metadata) {
    return (*metadata & ~REMEMBERED_MASK) == om_marked;
}

static inline bool ObjectMeta_IsRemembered(ObjectMeta *metadata) {
    return (*metadata & REMEMBERED_MASK) != 0;
}

static inline bool ObjectMeta_IsAlive(ObjectMeta *metadata, bool isOld) {
    if (isOld) {
        return ObjectMeta_IsMarked(metadata);
    } else {
        return ObjectMeta_IsAllocated(metadata);
    }
}

static inline bool ObjectMeta_IsAliveSweep(ObjectMeta *metadata, bool collectingOld) {
    if (collectingOld) {
        return ObjectMeta_IsAllocated(metadata);
    } else {
        return ObjectMeta_IsMarked(metadata);
    }
}

static inline void ObjectMeta_SetFree(ObjectMeta *metadata) {
    *metadata = (*metadata & REMEMBERED_MASK) | om_free;
}

static inline void ObjectMeta_SetPlaceholder(ObjectMeta *metadata) {
    *metadata = (*metadata & REMEMBERED_MASK) | om_placeholder;
}

static inline void ObjectMeta_SetAllocated(ObjectMeta *metadata) {
    *metadata = (*metadata & REMEMBERED_MASK) | om_allocated;
}

static inline void ObjectMeta_SetMarked(ObjectMeta *metadata) {
    *metadata = (*metadata & REMEMBERED_MASK) | om_marked;
}

static inline void ObjectMeta_SetRemembered(ObjectMeta *metadata) {
    *metadata |= REMEMBERED_MASK;
}

static inline void ObjectMeta_SetUnremembered(ObjectMeta *metadata) {
    *metadata &= ~REMEMBERED_MASK;
}

static inline void ObjectMeta_ClearLineAt(ObjectMeta *cursor) {
    memset(cursor, 0, WORDS_IN_LINE / ALLOCATION_ALIGNMENT_WORDS);
}

static inline void ObjectMeta_ClearBlockAt(ObjectMeta *cursor) {
    memset(cursor, 0, WORDS_IN_BLOCK / ALLOCATION_ALIGNMENT_WORDS);
}

#define SWEEP_MASK 0x0404040404040404UL
#define SWEEP_MASK_REMEMBERED 0x0808080808080808UL
static inline void ObjectMeta_SweepLineAt(ObjectMeta *start) {
    //    implements this, just with hardcoded constants:
    //
    //    size_t startIndex = Bytemap_index(bytemap, start);
    //    size_t endIndex = startIndex + WORDS_IN_LINE /
    //    ALLOCATION_ALIGNMENT_WORDS; ObjectMeta *data = bytemap->data; for
    //    (size_t i = startIndex; i < endIndex; i++) {
    //        if (data[i] == om_marked) {
    //            data[i] = om_allocated;
    //        } else if (data[i] == om_allocated) {
    //            data[i] = om_free;
    //        }
    //    }
    assert(WORDS_IN_LINE / ALLOCATION_ALIGNMENT_WORDS / 8 == 2);
    uint64_t *first = (uint64_t *)start;
    first[0] = (first[0] & SWEEP_MASK) >> 1;
    first[1] = (first[1] & SWEEP_MASK) >> 1;
}

static inline void ObjectMeta_SweepNewOldLineAt(ObjectMeta *start) {
    // implements this
    // if (ObjectMeta_IsMarked(cursor)) {
    //     // Do nothing
    // } else {
    //     ObjectMeta_SetFree(cursor);
    // }
    assert(WORDS_IN_LINE / ALLOCATION_ALIGNMENT_WORDS / 8 == 2);
    uint64_t *first = (uint64_t *)start;
    first[0] = (first[0] & SWEEP_MASK_REMEMBERED) | (first[0] & SWEEP_MASK);
    first[1] = (first[1] & SWEEP_MASK_REMEMBERED) | (first[1] & SWEEP_MASK);
}

#define SWEEP_MASK_OLD 0x0202020202020202UL
static inline void ObjectMeta_SweepOldLineAt(ObjectMeta *start) {
    // implements this
    // if (ObjectMeta_IsAllocated(cursor)) {
    //     ObjectMeta_SetMarked(cursor);
    // } else {
    //     ObjectMeta_SetFree(curor);
    // }
    //
    // Note that here, object can be in the remembered set. So we have
    // to keep this information with SWEEP_MASK_REMEMBERED
    uint64_t *first = (uint64_t *)start;
    first[0] = (first[0] & SWEEP_MASK_REMEMBERED) | ((first[0] & SWEEP_MASK_OLD) << 1);
    first[1] = (first[1] & SWEEP_MASK_REMEMBERED) | ((first[1] & SWEEP_MASK_OLD) << 1);
}

static inline void ObjectMeta_Sweep(ObjectMeta *cursor) {
    //    implements this, just with hardcoded constants:
    //
    //    if (ObjectMeta_IsMarked(cursor)) {
    //        ObjectMeta_SetAllocated(cursor);
    //    } else {
    //        ObjectMeta_SetFree(cursor)
    //    }
    *cursor = (*cursor & 0x04) >> 1;
}

static inline void ObjectMeta_SweepOld(ObjectMeta *cursor) {
    // if (ObjectMeta_IsAllocated(cursor)) {
    //     ObjectMeta_SetMarked(cursor);
    // } else {
    //    ObjectMeta_SetFree(cursor);
    // }
    // 
    // Since this is a old object, we also have to make sure it is still
    // remembered if it was
    *cursor = (*cursor & REMEMBERED_MASK) | ((*cursor & 0x2) << 1);
}

static inline void ObjectMeta_SweepNewOld(ObjectMeta *cursor) {
    // if (ObjectMeta_IsMarked(cursor)) {
    //     // do nothing
    // } else {
    //     ObjectMeta_SetFree(cursor);
    // }
    *cursor = (*cursor & REMEMBERED_MASK) | (*cursor & 0x4);
}
#endif // IMMIX_OBJECTMETA_H
