#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Type Type;

typedef enum {
    TYPE_INT,
    TYPE_BOOL,
    TYPE_DOUBLE,
    TYPE_BIGINT,
    TYPE_STRING,
    TYPE_BYTES,
    TYPE_ARRAY,
    TYPE_NIL,
    TYPE_VOID,
    TYPE_FUNCTION,
    TYPE_FUTURE,
    TYPE_ANY,
    TYPE_RECORD,
    TYPE_INTERFACE,
    TYPE_TUPLE,
    TYPE_MAP,
    TYPE_SET,
    TYPE_TYPE_PARAM
} TypeKind;

typedef struct RecordField {
    char* name;
    Type* type;
    int index;
} RecordField;

typedef struct RecordDef {
    char* name;
    RecordField* fields;
    int field_count;
    int field_capacity;
    bool is_native_opaque;
    int ref_count;
} RecordDef;

typedef struct InterfaceMethod {
    char* name;
    Type* type; // function signature without receiver parameter
} InterfaceMethod;

typedef struct InterfaceDef {
    char* name;
    InterfaceMethod* methods;
    int method_count;
    int method_capacity;
    int ref_count;
} InterfaceDef;

typedef struct TupleType {
    Type** element_types;
    int element_count;
} TupleType;

typedef struct MapType {
    Type* key_type;    // string or int
    Type* value_type;  // any type
} MapType;

typedef struct SetType {
    Type* element_type;  // string or int
} SetType;

struct Type {
    TypeKind kind;
    bool nullable;
    union {
        struct Type* element_type;     // For arrays
        struct Type* return_type;      // For functions
        RecordDef* record_def;         // For records
        InterfaceDef* interface_def;   // For interfaces
        TupleType* tuple_def;          // For tuples
        MapType* map_def;              // For maps
        SetType* set_def;              // For sets
        char* type_param_name;         // For type parameters
    };
    struct Type** param_types;         // Function parameter types or named-type arguments (e.g., Box[int])
    int param_count;
    char** type_param_names;           // For generic function types
    Type** type_param_constraints;     // Optional generic constraints per type parameter
    int type_param_count;
};

typedef struct {
    Type* type;
    char* name;
    bool is_mutable;
    bool is_type_alias;
    bool is_public;
    char* decl_file;
    void* function_obj;
} Symbol;

// Basic type constructors
Type* type_int(void);
Type* type_bool(void);
Type* type_double(void);
Type* type_bigint(void);
Type* type_string(void);
Type* type_bytes(void);
Type* type_array(Type* element_type);
Type* type_nil(void);
Type* type_void(void);
Type* type_function(Type* return_type, Type** param_types, int param_count);
Type* type_future(Type* value_type);
Type* type_any(void);
Type* type_type_param(const char* name);
void type_function_set_type_params(Type* function_type,
                                   char** type_param_names,
                                   Type** type_param_constraints,
                                   int type_param_count);

// Record type constructors
Type* type_record(const char* name);
Type* type_interface(const char* name);
RecordDef* record_def_create(const char* name);
void record_def_retain(RecordDef* def);
void record_def_release(RecordDef* def);
void record_def_free(RecordDef* def);
void record_def_add_field(RecordDef* def, const char* name, Type* type);
int record_def_get_field_index(RecordDef* def, const char* name);
RecordField* record_def_get_field(RecordDef* def, int index);
Type* record_def_get_field_type(RecordDef* def, const char* name);

// Interface type constructors
InterfaceDef* interface_def_create(const char* name);
void interface_def_retain(InterfaceDef* def);
void interface_def_release(InterfaceDef* def);
void interface_def_free(InterfaceDef* def);
void interface_def_add_method(InterfaceDef* def, const char* name, Type* type);
int interface_def_get_method_index(InterfaceDef* def, const char* name);
InterfaceMethod* interface_def_get_method(InterfaceDef* def, int index);
Type* interface_def_get_method_type(InterfaceDef* def, const char* name);

// Tuple type constructors
Type* type_tuple(Type** element_types, int element_count);
TupleType* tuple_type_create(Type** element_types, int element_count);
void tuple_type_free(TupleType* tuple);
int tuple_type_get_arity(Type* type);
Type* tuple_type_get_element(Type* type, int index);

// Map type constructors
Type* type_map(Type* key_type, Type* value_type);
MapType* map_type_create(Type* key_type, Type* value_type);
void map_type_free(MapType* map);

// Set type constructors
Type* type_set(Type* element_type);
SetType* set_type_create(Type* element_type);
void set_type_free(SetType* set);

// Type operations
Type* type_clone(Type* type);
Type* type_nullable(Type* type);
bool type_equals(Type* a, Type* b);
bool type_assignable(Type* to, Type* from);
void type_to_string(Type* type, char* buffer, size_t buffer_size);
void type_free(Type* type);

Symbol* symbol_create(Type* type, const char* name, bool is_mutable);
void symbol_free(Symbol* sym);

#endif
