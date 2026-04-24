#ifndef NEWOS_COMPILER_BACKEND_INTERNAL_H
#define NEWOS_COMPILER_BACKEND_INTERNAL_H

#include "backend.h"
#include "runtime.h"

#define COMPILER_BACKEND_MAX_FUNCTIONS 1024
#define COMPILER_BACKEND_MAX_GLOBALS 1024
#define COMPILER_BACKEND_MAX_LOCALS 1024
#define COMPILER_BACKEND_MAX_STRINGS 1024
#define COMPILER_BACKEND_MAX_CONSTANTS 512
#define COMPILER_BACKEND_MAX_AGGREGATES 256
#define COMPILER_BACKEND_MAX_AGGREGATE_MEMBERS 4096
#define COMPILER_BACKEND_MAX_SWITCH_DEPTH 64
#define COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY 2048
#define COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY 2048
#define COMPILER_BACKEND_LOCAL_INDEX_CAPACITY 2048
#define COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY 1024
#define COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY 512
#define COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY 8192
#define BACKEND_ARRAY_STACK_BYTES 4096
#define BACKEND_STRUCT_STACK_BYTES 16384
#define BACKEND_MAX_OBJECT_STACK_BYTES (4 * 1024 * 1024)

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    char return_type[128];
    int global;
    int stack_bytes;
    int returns_object;
} BackendFunctionName;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    char type_text[128];
    char init_text[COMPILER_IR_LINE_CAPACITY];
    long long init_value;
    int initialized;
    int is_array;
    int pointer_depth;
    int char_based;
    int prefers_word_index;
    int global;
    int has_storage;
} BackendGlobal;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    char type_text[128];
    char symbol_name[COMPILER_IR_NAME_CAPACITY];
    int offset;
    int stack_bytes;
    int is_array;
    int pointer_depth;
    int char_based;
    int prefers_word_index;
    int static_storage;
} BackendLocal;

typedef struct {
    char label[32];
    char text[COMPILER_IR_LINE_CAPACITY];
} BackendStringLiteral;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long value;
} BackendConstant;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    int is_union;
    int size_bytes;
    int align_bytes;
} BackendAggregate;

typedef struct {
    char aggregate_name[COMPILER_IR_NAME_CAPACITY];
    char name[COMPILER_IR_NAME_CAPACITY];
    char type_text[128];
    int offset_bytes;
} BackendAggregateMember;

typedef struct {
    char end_label[32];
    char default_label[32];
    unsigned int switch_id;
    unsigned int case_count;
    unsigned int next_case_index;
    int has_default;
} BackendSwitchContext;

typedef struct {
    CompilerBackend *backend;
    const CompilerIr *ir;
    int fd;
    BackendFunctionName functions[COMPILER_BACKEND_MAX_FUNCTIONS];
    unsigned int function_index[COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY];
    size_t function_count;
    BackendGlobal globals[COMPILER_BACKEND_MAX_GLOBALS];
    unsigned int global_index[COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY];
    size_t global_count;
    BackendStringLiteral strings[COMPILER_BACKEND_MAX_STRINGS];
    size_t string_count;
    BackendConstant constants[COMPILER_BACKEND_MAX_CONSTANTS];
    unsigned int constant_index[COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY];
    size_t constant_count;
    BackendAggregate aggregates[COMPILER_BACKEND_MAX_AGGREGATES];
    unsigned int aggregate_index[COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY];
    size_t aggregate_count;
    BackendAggregateMember aggregate_members[COMPILER_BACKEND_MAX_AGGREGATE_MEMBERS];
    unsigned int aggregate_member_index[COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY];
    size_t aggregate_member_count;
    BackendLocal locals[COMPILER_BACKEND_MAX_LOCALS];
    unsigned int local_index[COMPILER_BACKEND_LOCAL_INDEX_CAPACITY];
    size_t local_count;
    BackendSwitchContext switch_stack[COMPILER_BACKEND_MAX_SWITCH_DEPTH];
    size_t switch_depth;
    char current_function[COMPILER_IR_NAME_CAPACITY];
    int in_function;
    int param_count;
    int saw_return_in_function;
    int stack_size;
    int reserved_stack_size;
    unsigned int label_counter;
} BackendState;

typedef enum {
    EXPR_TOKEN_EOF = 0,
    EXPR_TOKEN_IDENTIFIER,
    EXPR_TOKEN_NUMBER,
    EXPR_TOKEN_CHAR,
    EXPR_TOKEN_STRING,
    EXPR_TOKEN_PUNCT
} ExprTokenKind;

typedef struct {
    ExprTokenKind kind;
    char text[COMPILER_IR_LINE_CAPACITY];
    long long number_value;
} ExprToken;

typedef struct {
    const char *cursor;
    ExprToken current;
    BackendState *state;
} ExprParser;

#define set_error backend_set_error
#define set_error_with_line backend_set_error_with_line

void backend_set_error(CompilerBackend *backend, const char *message);
void backend_set_error_with_line(CompilerBackend *backend, const char *message, const char *line);
int emit_text(BackendState *state, const char *text);
int emit_line(BackendState *state, const char *text);
int emit_instruction(BackendState *state, const char *text);
int names_equal(const char *lhs, const char *rhs);
int text_contains(const char *text, const char *needle);
int starts_with(const char *text, const char *prefix);
int name_looks_like_macro_constant(const char *name);
const char *skip_spaces(const char *text);
int backend_is_aarch64(const BackendState *state);
int backend_is_darwin(const BackendState *state);
const char *backend_private_label_prefix(const BackendState *state);
int backend_stack_slot_size(const BackendState *state);
int backend_register_arg_limit(const BackendState *state);
void format_symbol_name(const BackendState *state, const char *name, char *buffer, size_t buffer_size);
const char *copy_next_word(const char *cursor, char *buffer, size_t buffer_size);
void copy_last_word(const char *text, char *buffer, size_t buffer_size);
int parse_signed_value(const char *text, long long *value_out);
int add_function_name(BackendState *state, const char *name, int global, const char *return_type);
int should_prefer_word_index(const char *name, const char *type_text);
int is_function_name(const BackendState *state, const char *name);
int function_returns_object(const BackendState *state, const char *name);
const char *function_return_type(const BackendState *state, const char *name);
int find_global(const BackendState *state, const char *name);
int find_constant(const BackendState *state, const char *name);
int add_constant(BackendState *state, const char *name, long long value);
int add_aggregate_layout(BackendState *state, const char *name, int is_union, int size_bytes, int align_bytes);
int add_aggregate_member(BackendState *state, const char *aggregate_name, const char *name, const char *type_text, int offset_bytes);
int lookup_aggregate_size(const BackendState *state, const char *type_text);
int lookup_aggregate_member(const BackendState *state,
                            const char *base_type,
                            const char *member_name,
                            int *offset_out,
                            const char **type_text_out);
int add_global(BackendState *state, const char *name, const char *type_text, int is_array, int pointer_depth, int char_based, int prefers_word_index, int global, int has_storage);
int find_local(const BackendState *state, const char *name);
void reset_local_index(BackendState *state);
int allocate_local(BackendState *state, const char *name, const char *type_text, int stack_bytes, int is_array, int pointer_depth, int char_based, int prefers_word_index);
int allocate_static_local(BackendState *state, const char *name, const char *symbol_name, const char *type_text, int storage_bytes, int is_array, int pointer_depth, int char_based, int prefers_word_index);
void build_static_local_symbol_name(const BackendState *state, const char *function_name, const char *name, char *buffer, size_t buffer_size);
const char *lookup_name_type_text(const BackendState *state, const char *name);
int write_label_name(const BackendState *state, char *buffer, size_t buffer_size, const char *label);
int emit_pop_to_register(BackendState *state, const char *reg);
int emit_local_address(BackendState *state, int offset, const char *reg);
int emit_load_from_address_into_register(BackendState *state, const char *address_reg, const char *dst_reg, int byte_value);
int emit_load_from_address_register(BackendState *state, const char *reg, int byte_value);
int emit_move_value_register(BackendState *state, const char *dst_reg);
int emit_store_to_address_register(BackendState *state, const char *reg, int byte_value);
int emit_pop_address_and_store(BackendState *state, int byte_value);
int backend_type_access_size(const char *type_text, int word_index);
char backend_decode_escaped_char(const char **cursor_inout);
int backend_member_prefers_word_index(const char *name, const char *type_text);
int backend_member_result_decays_to_address(const char *type_text);
int backend_member_byte_offset(const BackendState *state, const char *base_type, const char *member_name);
void backend_copy_member_result_type(const BackendState *state,
                                     const char *base_type,
                                     const char *member_name,
                                     char *buffer,
                                     size_t buffer_size);
int backend_type_is_pointer_like(const char *base_type);
void backend_copy_indexed_type_text(const char *base_type, char *buffer, size_t buffer_size);
void backend_copy_dereferenced_type_text(const char *base_type, char *buffer, size_t buffer_size);
long long backend_type_storage_bytes(const BackendState *state, const char *type_text);
int backend_array_index_scale(const BackendState *state, const char *base_type, int word_index);
int find_string_literal(const BackendState *state, const char *text);
int add_string_literal(BackendState *state, const char *text);
int emit_address_of_name(BackendState *state, const char *name);
int emit_load_string_literal(BackendState *state, const char *text);
int emit_load_name_into_register(BackendState *state, const char *name, const char *dst_reg);
int emit_load_name(BackendState *state, const char *name);
int emit_store_name(BackendState *state, const char *name);
int emit_copy_object_to_name(BackendState *state, const char *name);
int emit_copy_name_to_pointer_name(BackendState *state, const char *src_name, const char *dst_pointer_name);
int emit_copy_object_to_pushed_address(BackendState *state, int bytes);
int lookup_array_storage(const BackendState *state, const char *name, int *word_index_out);
int emit_load_immediate_register(BackendState *state, const char *reg, long long value);
int emit_load_immediate(BackendState *state, long long value);
int emit_push_value(BackendState *state);
int emit_cmp_zero(BackendState *state);
int emit_set_condition(BackendState *state, const char *condition);
int emit_jump_to_label(BackendState *state, const char *mnemonic, const char *label);

int emit_expression(BackendState *state, const char *expr);
int emit_array_initializer_store(BackendState *state, const char *name, const char *expr);
int emit_object_initializer_store(BackendState *state, const char *name, const char *expr);
int emit_object_copy_store(BackendState *state, const char *name, const char *expr);

#endif
