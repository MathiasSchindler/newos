#include "semantic.h"

#include "runtime.h"

static int names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

static int types_are_compatible(const CompilerType *lhs, const CompilerType *rhs) {
    if (lhs->base == COMPILER_BASE_UNKNOWN || rhs->base == COMPILER_BASE_UNKNOWN) {
        return 1;
    }

    return lhs->base == rhs->base &&
           lhs->pointer_depth == rhs->pointer_depth &&
           lhs->is_function == rhs->is_function &&
           lhs->is_array == rhs->is_array &&
           lhs->is_unsigned == rhs->is_unsigned;
}

static int is_macro_like_name(const char *name) {
    size_t i = 0;
    int saw_alpha = 0;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[i] != '\0') {
        char ch = name[i];

        if (ch >= 'a' && ch <= 'z') {
            return 0;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '_') {
            saw_alpha = 1;
        } else if (ch < '0' || ch > '9') {
            return 0;
        }
        i += 1;
    }

    return saw_alpha;
}

static void set_error(CompilerSemantic *semantic, const char *message) {
    rt_copy_string(semantic->error_message, sizeof(semantic->error_message), message != 0 ? message : "semantic error");
}

static int find_symbol(const CompilerSemantic *semantic, const char *name) {
    size_t i = semantic->symbol_count;

    while (i > 0) {
        i -= 1U;
        if (names_equal(semantic->symbols[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int find_symbol_in_current_scope(const CompilerSemantic *semantic, const char *name) {
    size_t start = semantic->scope_markers[semantic->scope_count - 1U];
    size_t i = semantic->symbol_count;

    while (i > start) {
        i -= 1U;
        if (names_equal(semantic->symbols[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

void compiler_type_init(CompilerType *type) {
    rt_memset(type, 0, sizeof(*type));
    type->base = COMPILER_BASE_INT;
}

void compiler_semantic_init(CompilerSemantic *semantic) {
    rt_memset(semantic, 0, sizeof(*semantic));
    semantic->scope_count = 1U;
    semantic->scope_markers[0] = 0U;
}

int compiler_semantic_enter_scope(CompilerSemantic *semantic) {
    if (semantic->scope_count >= COMPILER_MAX_SCOPES) {
        set_error(semantic, "scope nesting exceeded semantic limits");
        return -1;
    }

    semantic->scope_markers[semantic->scope_count] = semantic->symbol_count;
    semantic->scope_count += 1U;
    return 0;
}

void compiler_semantic_exit_scope(CompilerSemantic *semantic) {
    if (semantic->scope_count <= 1U) {
        return;
    }

    semantic->scope_count -= 1U;
    semantic->symbol_count = semantic->scope_markers[semantic->scope_count];
}

int compiler_semantic_declare(
    CompilerSemantic *semantic,
    const char *name,
    CompilerSymbolKind kind,
    const CompilerType *type,
    int is_definition
) {
    int existing;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    existing = find_symbol_in_current_scope(semantic, name);
    if (existing >= 0) {
        CompilerSymbol *symbol = &semantic->symbols[existing];

        if (symbol->kind == COMPILER_SYMBOL_FUNCTION && kind == COMPILER_SYMBOL_FUNCTION) {
            if (symbol->defined && is_definition) {
                set_error(semantic, "redefinition of function");
                return -1;
            }
            if (is_definition) {
                symbol->defined = 1;
            }
            symbol->type = *type;
            return 0;
        }

        if (symbol->kind == COMPILER_SYMBOL_OBJECT && kind == COMPILER_SYMBOL_OBJECT) {
            if (!types_are_compatible(&symbol->type, type)) {
                set_error(semantic, "conflicting declaration in the same scope");
                return -1;
            }
            if (semantic->scope_count > 1U && (symbol->defined || is_definition)) {
                set_error(semantic, "duplicate declaration in the same scope");
                return -1;
            }
            if (symbol->defined && is_definition) {
                set_error(semantic, "duplicate declaration in the same scope");
                return -1;
            }
            if (is_definition || symbol->type.base == COMPILER_BASE_UNKNOWN) {
                symbol->type = *type;
            }
            if (is_definition) {
                symbol->defined = 1;
            }
            return 0;
        }

        set_error(semantic, "duplicate declaration in the same scope");
        return -1;
    }

    if (semantic->symbol_count >= COMPILER_MAX_SYMBOLS) {
        set_error(semantic, "symbol table exhausted");
        return -1;
    }

    rt_copy_string(semantic->symbols[semantic->symbol_count].name, sizeof(semantic->symbols[semantic->symbol_count].name), name);
    semantic->symbols[semantic->symbol_count].kind = kind;
    semantic->symbols[semantic->symbol_count].type = *type;
    semantic->symbols[semantic->symbol_count].scope_level = semantic->scope_count - 1U;
    semantic->symbols[semantic->symbol_count].defined = is_definition;
    semantic->symbol_count += 1U;
    return 0;
}

int compiler_semantic_use_identifier(CompilerSemantic *semantic, const char *name, int as_function_call) {
    int index;
    CompilerType implicit_type;

    if (name == 0 || name[0] == '\0' || is_macro_like_name(name)) {
        return 0;
    }

    index = find_symbol(semantic, name);
    if (index < 0) {
        if (as_function_call) {
            compiler_type_init(&implicit_type);
            implicit_type.is_function = 1;
            return compiler_semantic_declare(semantic, name, COMPILER_SYMBOL_FUNCTION, &implicit_type, 0);
        }

        set_error(semantic, "use of undeclared identifier");
        return -1;
    }

    if (semantic->symbols[index].kind == COMPILER_SYMBOL_TYPEDEF) {
        set_error(semantic, "type name used where an expression was expected");
        return -1;
    }

    if (as_function_call && semantic->symbols[index].kind != COMPILER_SYMBOL_FUNCTION) {
        set_error(semantic, "called object is not a function");
        return -1;
    }

    return 0;
}

void compiler_semantic_begin_function(CompilerSemantic *semantic, const CompilerType *type) {
    semantic->current_function_type = *type;
    semantic->in_function = 1;
}

void compiler_semantic_end_function(CompilerSemantic *semantic) {
    semantic->in_function = 0;
    compiler_type_init(&semantic->current_function_type);
}

int compiler_semantic_check_return(CompilerSemantic *semantic, int has_value) {
    if (!semantic->in_function) {
        return 0;
    }

    if (semantic->current_function_type.base == COMPILER_BASE_VOID &&
        semantic->current_function_type.pointer_depth == 0 &&
        has_value) {
        set_error(semantic, "void function should not return a value");
        return -1;
    }

    return 0;
}

const char *compiler_semantic_error_message(const CompilerSemantic *semantic) {
    return semantic->error_message;
}
