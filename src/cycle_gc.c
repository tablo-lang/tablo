#include "cycle_gc.h"
#include "vm.h"
#include "safe_alloc.h"

#include <stddef.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#define CYCLE_GC_FLAG_REACHABLE 0x01

int cycle_gc_allocations_since_collect(const CycleGc* gc) {
    if (!gc) return 0;
#ifdef _MSC_VER
    long value = _InterlockedCompareExchange((volatile long*)&gc->allocations_since_collect, 0, 0);
    return (int)value;
#elif defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&gc->allocations_since_collect, __ATOMIC_RELAXED);
#else
    return gc->allocations_since_collect;
#endif
}

void cycle_gc_reset_allocations_since_collect(CycleGc* gc) {
    if (!gc) return;
#ifdef _MSC_VER
    (void)_InterlockedExchange((volatile long*)&gc->allocations_since_collect, 0);
#elif defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&gc->allocations_since_collect, 0, __ATOMIC_RELAXED);
#else
    gc->allocations_since_collect = 0;
#endif
}

void cycle_gc_increment_allocations_since_collect(CycleGc* gc) {
    if (!gc) return;
#ifdef _MSC_VER
    (void)_InterlockedIncrement((volatile long*)&gc->allocations_since_collect);
#elif defined(__GNUC__) || defined(__clang__)
    (void)__atomic_add_fetch(&gc->allocations_since_collect, 1, __ATOMIC_RELAXED);
#else
    gc->allocations_since_collect++;
#endif
}

void cycle_gc_node_init(CycleGcNode* node, CycleGcObjType type, void* obj) {
    node->prev = NULL;
    node->next = NULL;
    node->owner = NULL;
    node->obj = obj;
    node->gc_refs = 0;
    node->gen = 0;
    node->type = (uint8_t)type;
    node->flags = 0;
}

void cycle_gc_init(CycleGc* gc) {
    if (!gc) return;
    gc->head = NULL;
    gc->tail = NULL;
    gc->tracked_count = 0;
    cycle_gc_reset_allocations_since_collect(gc);
    gc->threshold = 256;
    gc->is_collecting = false;
    gc->last_reclaimed = 0;
    gc->total_collections = 0;
}

void cycle_gc_deinit(CycleGc* gc) {
    if (!gc) return;
    gc->head = NULL;
    gc->tail = NULL;
    gc->tracked_count = 0;
    cycle_gc_reset_allocations_since_collect(gc);
    gc->threshold = 0;
    gc->is_collecting = false;
    gc->last_reclaimed = 0;
    gc->total_collections = 0;
}

void cycle_gc_track(CycleGc* gc, CycleGcNode* node) {
    if (!gc || !node) return;
    if (node->owner) return;

    node->owner = gc;
    node->prev = gc->tail;
    node->next = NULL;
    if (gc->tail) {
        gc->tail->next = node;
    } else {
        gc->head = node;
    }
    gc->tail = node;
    gc->tracked_count++;
    cycle_gc_increment_allocations_since_collect(gc);
}

void cycle_gc_untrack(CycleGcNode* node) {
    if (!node || !node->owner) return;
    CycleGc* gc = node->owner;

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        gc->head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        gc->tail = node->prev;
    }

    node->prev = NULL;
    node->next = NULL;
    node->owner = NULL;

    if (gc->tracked_count > 0) {
        gc->tracked_count--;
    }
}

static int32_t cycle_gc_obj_ref_count(const CycleGcNode* node) {
    if (!node || !node->obj) return 0;
    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY: return ((const ObjArray*)node->obj)->ref_count;
        case CYCLE_GC_OBJ_RECORD: return ((const ObjRecord*)node->obj)->ref_count;
        case CYCLE_GC_OBJ_TUPLE: return ((const ObjTuple*)node->obj)->ref_count;
        case CYCLE_GC_OBJ_MAP: return ((const ObjMap*)node->obj)->ref_count;
        default: return 0;
    }
}

static void cycle_gc_obj_hold(const CycleGcNode* node) {
    if (!node || !node->obj) return;
    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY:
            obj_array_retain((ObjArray*)node->obj);
            break;
        case CYCLE_GC_OBJ_RECORD:
            obj_record_retain((ObjRecord*)node->obj);
            break;
        case CYCLE_GC_OBJ_TUPLE:
            obj_tuple_retain((ObjTuple*)node->obj);
            break;
        case CYCLE_GC_OBJ_MAP:
            obj_map_retain((ObjMap*)node->obj);
            break;
        default:
            break;
    }
}

static void cycle_gc_obj_drop_hold(const CycleGcNode* node) {
    if (!node || !node->obj) return;
    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY:
            obj_array_release((ObjArray*)node->obj);
            break;
        case CYCLE_GC_OBJ_RECORD:
            obj_record_release((ObjRecord*)node->obj);
            break;
        case CYCLE_GC_OBJ_TUPLE:
            obj_tuple_release((ObjTuple*)node->obj);
            break;
        case CYCLE_GC_OBJ_MAP:
            obj_map_release((ObjMap*)node->obj);
            break;
        default:
            break;
    }
}

static CycleGcNode* cycle_gc_node_from_value(Value* val, CycleGc* owner) {
    if (!val || !owner) return NULL;
    switch (value_get_type(val)) {
        case VAL_ARRAY: {
            ObjArray* arr = value_get_array_obj(val);
            if (!arr) return NULL;
            if (arr->gc_node.owner != owner) return NULL;
            return &arr->gc_node;
        }
        case VAL_RECORD: {
            ObjRecord* record = value_get_record_obj(val);
            if (!record) return NULL;
            if (record->gc_node.owner != owner) return NULL;
            return &record->gc_node;
        }
        case VAL_TUPLE: {
            ObjTuple* tuple = value_get_tuple_obj(val);
            if (!tuple) return NULL;
            if (tuple->gc_node.owner != owner) return NULL;
            return &tuple->gc_node;
        }
        case VAL_MAP: {
            ObjMap* map = value_get_map_obj(val);
            if (!map) return NULL;
            if (map->gc_node.owner != owner) return NULL;
            return &map->gc_node;
        }
        default:
            return NULL;
    }
}

static void cycle_gc_subtract_children_refs(CycleGcNode* node, CycleGc* owner) {
    if (!node || !owner || !node->obj) return;

    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)node->obj;
            if (arr->kind == ARRAY_KIND_BOXED && arr->data.elements) {
                for (int i = 0; i < arr->count; i++) {
                    CycleGcNode* child = cycle_gc_node_from_value(&arr->data.elements[i], owner);
                    if (child) child->gc_refs--;
                }
            }
            break;
        }
        case CYCLE_GC_OBJ_RECORD: {
            ObjRecord* record = (ObjRecord*)node->obj;
            for (int i = 0; i < record->field_count; i++) {
                CycleGcNode* child = cycle_gc_node_from_value(&record->fields[i], owner);
                if (child) child->gc_refs--;
            }
            break;
        }
        case CYCLE_GC_OBJ_TUPLE: {
            ObjTuple* tuple = (ObjTuple*)node->obj;
            for (int i = 0; i < tuple->element_count; i++) {
                CycleGcNode* child = cycle_gc_node_from_value(&tuple->elements[i], owner);
                if (child) child->gc_refs--;
            }
            break;
        }
        case CYCLE_GC_OBJ_MAP: {
            ObjMap* map = (ObjMap*)node->obj;
            if (!map->slots) break;
            for (int i = 0; i < map->capacity; i++) {
                MapSlot* slot = &map->slots[i];
                if (slot->hash < 2) continue;
                CycleGcNode* child = cycle_gc_node_from_value(&slot->value, owner);
                if (child) child->gc_refs--;
            }
            break;
        }
        default:
            break;
    }
}

static void cycle_gc_mark_reachable(CycleGcNode* start, CycleGcNode** stack, int* stack_top, int stack_cap, CycleGc* owner) {
    if (!start || !owner) return;
    if (start->flags & CYCLE_GC_FLAG_REACHABLE) return;
    if (*stack_top >= stack_cap) return;

    start->flags |= CYCLE_GC_FLAG_REACHABLE;
    stack[(*stack_top)++] = start;
}

static void cycle_gc_mark_children_reachable(CycleGcNode* node, CycleGcNode** stack, int* stack_top, int stack_cap, CycleGc* owner) {
    if (!node || !owner || !node->obj) return;

    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)node->obj;
            if (arr->kind == ARRAY_KIND_BOXED && arr->data.elements) {
                for (int i = 0; i < arr->count; i++) {
                    CycleGcNode* child = cycle_gc_node_from_value(&arr->data.elements[i], owner);
                    if (child) cycle_gc_mark_reachable(child, stack, stack_top, stack_cap, owner);
                }
            }
            break;
        }
        case CYCLE_GC_OBJ_RECORD: {
            ObjRecord* record = (ObjRecord*)node->obj;
            for (int i = 0; i < record->field_count; i++) {
                CycleGcNode* child = cycle_gc_node_from_value(&record->fields[i], owner);
                if (child) cycle_gc_mark_reachable(child, stack, stack_top, stack_cap, owner);
            }
            break;
        }
        case CYCLE_GC_OBJ_TUPLE: {
            ObjTuple* tuple = (ObjTuple*)node->obj;
            for (int i = 0; i < tuple->element_count; i++) {
                CycleGcNode* child = cycle_gc_node_from_value(&tuple->elements[i], owner);
                if (child) cycle_gc_mark_reachable(child, stack, stack_top, stack_cap, owner);
            }
            break;
        }
        case CYCLE_GC_OBJ_MAP: {
            ObjMap* map = (ObjMap*)node->obj;
            if (!map->slots) break;
            for (int i = 0; i < map->capacity; i++) {
                MapSlot* slot = &map->slots[i];
                if (slot->hash < 2) continue;
                CycleGcNode* child = cycle_gc_node_from_value(&slot->value, owner);
                if (child) cycle_gc_mark_reachable(child, stack, stack_top, stack_cap, owner);
            }
            break;
        }
        default:
            break;
    }
}

static void cycle_gc_clear_children(CycleGcNode* node) {
    if (!node || !node->obj) return;

    switch (node->type) {
        case CYCLE_GC_OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)node->obj;
            if (arr->kind == ARRAY_KIND_BOXED && arr->data.elements) {
                for (int i = 0; i < arr->count; i++) {
                    value_free(&arr->data.elements[i]);
                }
            }
            arr->count = 0;
            break;
        }
        case CYCLE_GC_OBJ_RECORD: {
            ObjRecord* record = (ObjRecord*)node->obj;
            for (int i = 0; i < record->field_count; i++) {
                value_free(&record->fields[i]);
            }
            break;
        }
        case CYCLE_GC_OBJ_TUPLE: {
            ObjTuple* tuple = (ObjTuple*)node->obj;
            for (int i = 0; i < tuple->element_count; i++) {
                value_free(&tuple->elements[i]);
            }
            tuple->element_count = 0;
            break;
        }
        case CYCLE_GC_OBJ_MAP: {
            ObjMap* map = (ObjMap*)node->obj;
            if (!map->slots) break;
            for (int i = 0; i < map->capacity; i++) {
                MapSlot* slot = &map->slots[i];
                if (slot->hash < 2) continue;
                value_free(&slot->key);
                value_free(&slot->value);
                Value nil;
                value_init_nil(&nil);
                slot->key = nil;
                slot->value = nil;
                slot->hash = 0;
            }
            map->count = 0;
            map->used = 0;
            break;
        }
        default:
            break;
    }
}

int vm_gc_tracked_count(VM* vm) {
    if (!vm) return 0;
    return vm->cycle_gc.tracked_count;
}

static int cycle_gc_snapshot_nodes(CycleGc* gc, CycleGcNode** out_nodes, int cap) {
    int idx = 0;
    for (CycleGcNode* n = gc->head; n && idx < cap; n = n->next) {
        out_nodes[idx++] = n;
    }
    return idx;
}

int vm_gc_collect(VM* vm) {
    if (!vm) return 0;
    CycleGc* gc = &vm->cycle_gc;
    if (gc->is_collecting) return 0;
    if (gc->tracked_count <= 0) return 0;

    gc->is_collecting = true;
    gc->total_collections++;
    gc->last_reclaimed = 0;

    int tracked_cap = gc->tracked_count;
    CycleGcNode** nodes = (CycleGcNode**)safe_malloc((size_t)tracked_cap * sizeof(CycleGcNode*));
    int tracked_count = cycle_gc_snapshot_nodes(gc, nodes, tracked_cap);
    if (tracked_count <= 0) {
        free(nodes);
        cycle_gc_reset_allocations_since_collect(gc);
        gc->is_collecting = false;
        return 0;
    }

    // Initialize gc_refs and clear flags.
    for (int i = 0; i < tracked_count; i++) {
        CycleGcNode* node = nodes[i];
        if (!node || node->owner != gc) continue;
        node->gc_refs = cycle_gc_obj_ref_count(node);
        node->flags = 0;
    }

    // Subtract internal references.
    for (int i = 0; i < tracked_count; i++) {
        CycleGcNode* node = nodes[i];
        if (!node || node->owner != gc) continue;
        cycle_gc_subtract_children_refs(node, gc);
    }

    // Mark reachable objects starting from nodes with gc_refs > 0.
    CycleGcNode** stack = (CycleGcNode**)safe_malloc((size_t)tracked_count * sizeof(CycleGcNode*));
    int stack_top = 0;
    for (int i = 0; i < tracked_count; i++) {
        CycleGcNode* node = nodes[i];
        if (!node || node->owner != gc) continue;
        if (node->gc_refs > 0) {
            cycle_gc_mark_reachable(node, stack, &stack_top, tracked_count, gc);
        }
    }

    while (stack_top > 0) {
        CycleGcNode* node = stack[--stack_top];
        cycle_gc_mark_children_reachable(node, stack, &stack_top, tracked_count, gc);
    }

    // Collect unreachable nodes.
    CycleGcNode** unreachable = (CycleGcNode**)safe_malloc((size_t)tracked_count * sizeof(CycleGcNode*));
    int unreachable_count = 0;
    for (int i = 0; i < tracked_count; i++) {
        CycleGcNode* node = nodes[i];
        if (!node || node->owner != gc) continue;
        if (!(node->flags & CYCLE_GC_FLAG_REACHABLE)) {
            unreachable[unreachable_count++] = node;
        }
    }

    // Hold all unreachable nodes to avoid re-entrant frees while breaking cycles.
    for (int i = 0; i < unreachable_count; i++) {
        cycle_gc_obj_hold(unreachable[i]);
    }

    for (int i = 0; i < unreachable_count; i++) {
        cycle_gc_clear_children(unreachable[i]);
    }

    for (int i = 0; i < unreachable_count; i++) {
        cycle_gc_obj_drop_hold(unreachable[i]);
    }

    gc->last_reclaimed = unreachable_count;
    cycle_gc_reset_allocations_since_collect(gc);

    free(nodes);
    free(stack);
    free(unreachable);

    gc->is_collecting = false;
    return gc->last_reclaimed;
}
