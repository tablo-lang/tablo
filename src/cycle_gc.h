#ifndef CYCLE_GC_H
#define CYCLE_GC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CYCLE_GC_OBJ_ARRAY = 1,
    CYCLE_GC_OBJ_RECORD = 2,
    CYCLE_GC_OBJ_TUPLE = 3,
    CYCLE_GC_OBJ_MAP = 4
} CycleGcObjType;

typedef struct CycleGc CycleGc;

typedef struct CycleGcNode {
    struct CycleGcNode* prev;
    struct CycleGcNode* next;
    CycleGc* owner;
    void* obj;
    int32_t gc_refs;
    uint16_t gen;
    uint8_t type;
    uint8_t flags;
} CycleGcNode;

typedef struct CycleGc {
    CycleGcNode* head;
    CycleGcNode* tail;
    int tracked_count;
    volatile int allocations_since_collect;
    int threshold;
    bool is_collecting;
    int last_reclaimed;
    int total_collections;
} CycleGc;

void cycle_gc_init(CycleGc* gc);
void cycle_gc_deinit(CycleGc* gc);

void cycle_gc_node_init(CycleGcNode* node, CycleGcObjType type, void* obj);

void cycle_gc_track(CycleGc* gc, CycleGcNode* node);
void cycle_gc_untrack(CycleGcNode* node);

int cycle_gc_allocations_since_collect(const CycleGc* gc);
void cycle_gc_reset_allocations_since_collect(CycleGc* gc);
void cycle_gc_increment_allocations_since_collect(CycleGc* gc);

#endif
