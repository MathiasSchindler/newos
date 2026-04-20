/*
 * make_parse.c - Makefile parser and shared data-management helpers for make.
 *
 * Covers: string/buffer utilities, variable management, rule lookup, pattern
 * matching, text expansion, path helpers, and the makefile reader.
 */

#include "make_impl.h"

static void trim_trailing_whitespace(char *text) {
    size_t len = rt_strlen(text);

    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len -= 1U;
    }
}

char *trim_leading_whitespace(char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

static int starts_with_space(const char *text) {
    return text[0] == ' ' || text[0] == '\t';
}

static int starts_with_keyword(const char *text, const char *keyword) {
    size_t i = 0;

    while (keyword[i] != '\0') {
        if (text[i] != keyword[i]) {
            return 0;
        }
        i += 1U;
    }

    return text[i] == ' ' || text[i] == '\t';
}

static int append_text(char *buffer, size_t *used, size_t buffer_size, const char *text) {
    size_t len = rt_strlen(text);

    if (*used + len + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer + *used, text, len);
    *used += len;
    buffer[*used] = '\0';
    return 0;
}

static int append_char(char *buffer, size_t *used, size_t buffer_size, char ch) {
    if (*used + 2 > buffer_size) {
        return -1;
    }
    buffer[*used] = ch;
    *used += 1U;
    buffer[*used] = '\0';
    return 0;
}

static int make_target_has_pattern(const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == '%') {
            return 1;
        }
        i += 1U;
    }

    return 0;
}

static int is_blank_or_comment(const char *text) {
    const char *cursor = text;

    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }

    return *cursor == '\0' || *cursor == '#';
}

static MakeVariable *find_variable(MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->var_count; ++i) {
        if (rt_strcmp(program->vars[i].name, name) == 0) {
            return &program->vars[i];
        }
    }

    return 0;
}

const char *get_variable_value(const MakeProgram *program, const char *name) {
    size_t i;
    const char *env_value;

    for (i = 0; i < program->var_count; ++i) {
        if (rt_strcmp(program->vars[i].name, name) == 0) {
            return program->vars[i].value;
        }
    }

    env_value = platform_getenv(name);
    if (env_value != 0) {
        return env_value;
    }
    return "";
}

static MakeVariableOrigin get_variable_origin(const MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->var_count; ++i) {
        if (rt_strcmp(program->vars[i].name, name) == 0) {
            return program->vars[i].origin;
        }
    }

    if (platform_getenv(name) != 0) {
        return MAKE_ORIGIN_ENVIRONMENT;
    }
    return 0;
}

static const char *get_variable_origin_text(const MakeProgram *program, const char *name) {
    MakeVariableOrigin origin = get_variable_origin(program, name);

    if (origin == MAKE_ORIGIN_COMMAND_LINE) {
        return "command line";
    }
    if (origin == MAKE_ORIGIN_ENVIRONMENT) {
        return "environment";
    }
    if (origin == MAKE_ORIGIN_FILE) {
        return "file";
    }
    return "undefined";
}

int set_variable_with_origin(MakeProgram *program, const char *name, const char *value, MakeVariableOrigin origin) {
    MakeVariable *variable = find_variable(program, name);

    if (variable == 0) {
        if (program->var_count >= MAKE_MAX_VARS) {
            return -1;
        }
        variable = &program->vars[program->var_count++];
        rt_memset(variable, 0, sizeof(*variable));
    } else if (variable->origin == MAKE_ORIGIN_COMMAND_LINE && origin != MAKE_ORIGIN_COMMAND_LINE) {
        return 0;
    }

    rt_copy_string(variable->name, sizeof(variable->name), name);
    rt_copy_string(variable->value, sizeof(variable->value), value);
    variable->origin = origin;
    return 0;
}

int set_variable(MakeProgram *program, const char *name, const char *value) {
    return set_variable_with_origin(program, name, value, MAKE_ORIGIN_FILE);
}

int mark_variable_exported(MakeProgram *program, const char *name, int exported) {
    MakeVariable *variable = find_variable(program, name);

    if (variable == 0) {
        const char *env_value;
        if (program->var_count >= MAKE_MAX_VARS) {
            return -1;
        }
        variable = &program->vars[program->var_count++];
        rt_memset(variable, 0, sizeof(*variable));
        rt_copy_string(variable->name, sizeof(variable->name), name);
        env_value = platform_getenv(name);
        if (env_value != 0) {
            rt_copy_string(variable->value, sizeof(variable->value), env_value);
            variable->origin = MAKE_ORIGIN_ENVIRONMENT;
        } else {
            variable->origin = MAKE_ORIGIN_FILE;
        }
    }

    variable->exported = exported != 0;
    return 0;
}

int sync_program_environment(const MakeProgram *program) {
    size_t i;

    for (i = 0; i < program->var_count; ++i) {
        const MakeVariable *variable = &program->vars[i];
        if (variable->name[0] == '\0') {
            continue;
        }
        if (variable->exported ||
            variable->origin == MAKE_ORIGIN_COMMAND_LINE ||
            rt_strcmp(variable->name, "MAKE") == 0 ||
            rt_strcmp(variable->name, "MAKEFLAGS") == 0) {
            if (platform_setenv(variable->name, variable->value, 1) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int append_variable(MakeProgram *program, const char *name, const char *value) {
    MakeVariable *variable = find_variable(program, name);
    char combined[MAKE_VALUE_CAPACITY];
    size_t used = 0;

    if (variable != 0 && variable->origin == MAKE_ORIGIN_COMMAND_LINE) {
        return 0;
    }

    if (variable == 0 || variable->value[0] == '\0') {
        return set_variable(program, name, value);
    }

    combined[0] = '\0';
    if (append_text(combined, &used, sizeof(combined), variable->value) != 0) {
        return -1;
    }
    if (value[0] != '\0') {
        if (append_char(combined, &used, sizeof(combined), ' ') != 0 ||
            append_text(combined, &used, sizeof(combined), value) != 0) {
            return -1;
        }
    }

    return set_variable(program, name, combined);
}

static int handle_variable_assignment(MakeProgram *program, char *content, int export_after) {
    char *equals = 0;
    char *colon = 0;
    size_t j = 0U;

    while (content[j] != '\0') {
        if (content[j] == ':' && colon == 0) {
            colon = content + j;
        }
        if (content[j] == '=' && equals == 0) {
            equals = content + j;
        }
        j += 1U;
    }

    if (equals == 0 ||
        !(colon == 0 || equals < colon ||
          (equals > content && (equals[-1] == ':' || equals[-1] == '+' || equals[-1] == '?')))) {
        return 0;
    }

    {
        char name[MAKE_NAME_CAPACITY];
        char value[MAKE_VALUE_CAPACITY];
        char expanded[MAKE_VALUE_CAPACITY];
        char assign_mode = '=';
        char *name_end = equals;
        size_t name_len;

        if (equals > content && (equals[-1] == ':' || equals[-1] == '+' || equals[-1] == '?')) {
            assign_mode = equals[-1];
            name_end = equals - 1;
        }

        *name_end = '\0';
        rt_copy_string(name, sizeof(name), trim_leading_whitespace(content));
        trim_trailing_whitespace(name);
        rt_copy_string(value, sizeof(value), trim_leading_whitespace(equals + 1));
        name_len = rt_strlen(name);

        if (name_len == 0U) {
            return -1;
        }

        if (assign_mode == ':') {
            if (expand_text(program, 0, value, expanded, sizeof(expanded)) != 0 ||
                set_variable(program, name, expanded) != 0) {
                return -1;
            }
        } else if (assign_mode == '?') {
            if ((get_variable_origin(program, name) == 0 || get_variable_value(program, name)[0] == '\0') &&
                set_variable(program, name, value) != 0) {
                return -1;
            }
        } else if (assign_mode == '+') {
            if (append_variable(program, name, value) != 0) {
                return -1;
            }
        } else if (set_variable(program, name, value) != 0) {
            return -1;
        }

        if (export_after && mark_variable_exported(program, name, 1) != 0) {
            return -1;
        }

        if (rt_strcmp(name, ".DEFAULT_GOAL") == 0) {
            const char *goal = get_variable_value(program, name);
            if (goal[0] != '\0') {
                rt_copy_string(program->first_target, sizeof(program->first_target), goal);
            }
        }
    }

    return 1;
}

MakeRule *find_rule(MakeProgram *program, const char *target) {
    size_t i;

    for (i = 0; i < program->rule_count; ++i) {
        if (rt_strcmp(program->rules[i].target, target) == 0) {
            return &program->rules[i];
        }
    }

    return 0;
}

int is_phony_target(const MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->phony_count; ++i) {
        if (rt_strcmp(program->phony[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

int is_target_active(const MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->active_count; ++i) {
        if (rt_strcmp(program->active_targets[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

int push_active_target(MakeProgram *program, const char *name) {
    if (program->active_count >= MAKE_MAX_RULES) {
        return -1;
    }

    rt_copy_string(program->active_targets[program->active_count], sizeof(program->active_targets[program->active_count]), name);
    program->active_count += 1U;
    return 0;
}

void pop_active_target(MakeProgram *program) {
    if (program->active_count > 0) {
        program->active_count -= 1U;
    }
}

static int match_target_pattern(const char *pattern, const char *target, char *stem_out, size_t stem_size) {
    size_t pattern_len;
    size_t target_len;
    size_t percent_index = 0;
    size_t suffix_len;
    size_t stem_len;
    size_t i;

    if (!make_target_has_pattern(pattern)) {
        if (rt_strcmp(pattern, target) != 0) {
            return 0;
        }
        if (stem_out != 0 && stem_size > 0U) {
            stem_out[0] = '\0';
        }
        return 1;
    }

    pattern_len = rt_strlen(pattern);
    target_len = rt_strlen(target);
    while (pattern[percent_index] != '\0' && pattern[percent_index] != '%') {
        percent_index += 1U;
    }

    suffix_len = pattern_len - percent_index - 1U;
    if (target_len < percent_index + suffix_len) {
        return 0;
    }

    for (i = 0; i < percent_index; ++i) {
        if (pattern[i] != target[i]) {
            return 0;
        }
    }
    for (i = 0; i < suffix_len; ++i) {
        if (pattern[pattern_len - suffix_len + i] != target[target_len - suffix_len + i]) {
            return 0;
        }
    }

    stem_len = target_len - percent_index - suffix_len;
    if (stem_out == 0 || stem_len + 1U > stem_size) {
        return 0;
    }
    memcpy(stem_out, target + percent_index, stem_len);
    stem_out[stem_len] = '\0';
    return 1;
}

static int substitute_pattern_text(const char *text, const char *stem, char *out, size_t out_size) {
    size_t used = 0;
    size_t i = 0;

    out[0] = '\0';
    while (text[i] != '\0') {
        if (text[i] == '%') {
            if (append_text(out, &used, out_size, stem) != 0) {
                return -1;
            }
        } else if (append_char(out, &used, out_size, text[i]) != 0) {
            return -1;
        }
        i += 1U;
    }

    return 0;
}

MakeRule *find_pattern_rule(MakeProgram *program, const char *target, char *stem_out, size_t stem_size) {
    size_t i;

    for (i = 0; i < program->rule_count; ++i) {
        if (program->rules[i].is_pattern && match_target_pattern(program->rules[i].target, target, stem_out, stem_size)) {
            return &program->rules[i];
        }
    }

    return 0;
}

int instantiate_pattern_rule(const MakeRule *pattern_rule, const char *target, const char *stem, MakeRule *out_rule) {
    size_t i;

    rt_memset(out_rule, 0, sizeof(*out_rule));
    rt_copy_string(out_rule->target, sizeof(out_rule->target), target);
    rt_copy_string(out_rule->stem, sizeof(out_rule->stem), stem);

    for (i = 0; i < pattern_rule->dep_count; ++i) {
        if (out_rule->dep_count >= MAKE_MAX_DEPS ||
            substitute_pattern_text(pattern_rule->deps[i], stem, out_rule->deps[out_rule->dep_count], sizeof(out_rule->deps[out_rule->dep_count])) != 0) {
            return -1;
        }
        out_rule->dep_count += 1U;
    }

    for (i = 0; i < pattern_rule->command_count; ++i) {
        rt_copy_string(out_rule->commands[out_rule->command_count], sizeof(out_rule->commands[out_rule->command_count]), pattern_rule->commands[i]);
        out_rule->command_count += 1U;
    }

    return 0;
}

static int normalize_whitespace_text(const char *text, char *out, size_t out_size) {
    size_t used = 0;
    int in_space = 1;
    size_t i = 0;

    out[0] = '\0';
    while (text[i] != '\0') {
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
            in_space = 1;
        } else {
            if (!in_space || used == 0U) {
                if (append_char(out, &used, out_size, text[i]) != 0) {
                    return -1;
                }
            } else {
                if (append_char(out, &used, out_size, ' ') != 0 ||
                    append_char(out, &used, out_size, text[i]) != 0) {
                    return -1;
                }
            }
            in_space = 0;
        }
        i += 1U;
    }

    if (used > 0U && out[used - 1U] == ' ') {
        out[used - 1U] = '\0';
    }
    return 0;
}

static size_t split_function_args(const char *text, char args[][MAKE_VALUE_CAPACITY], size_t max_args) {
    size_t count = 0;
    size_t used = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t i = 0;

    if (max_args == 0U) {
        return 0U;
    }

    args[0][0] = '\0';
    while (text[i] != '\0' && count < max_args) {
        char ch = text[i];

        if (ch == ',' && paren_depth == 0 && brace_depth == 0) {
            trim_trailing_whitespace(args[count]);
            count += 1U;
            if (count >= max_args) {
                return count;
            }
            used = 0U;
            args[count][0] = '\0';
            i += 1U;
            while (text[i] == ' ' || text[i] == '\t') {
                i += 1U;
            }
            continue;
        }

        if (ch == '(') {
            paren_depth += 1;
        } else if (ch == ')' && paren_depth > 0) {
            paren_depth -= 1;
        } else if (ch == '{') {
            brace_depth += 1;
        } else if (ch == '}' && brace_depth > 0) {
            brace_depth -= 1;
        }

        if (used + 1U < MAKE_VALUE_CAPACITY) {
            args[count][used++] = ch;
            args[count][used] = '\0';
        }
        i += 1U;
    }

    trim_trailing_whitespace(args[count]);
    return count + 1U;
}

static int word_matches_pattern(const char *pattern, const char *word) {
    size_t pattern_len = rt_strlen(pattern);
    size_t word_len = rt_strlen(word);
    size_t percent = 0U;
    size_t suffix_len;
    size_t i;

    while (pattern[percent] != '\0' && pattern[percent] != '%') {
        percent += 1U;
    }
    if (pattern[percent] == '\0') {
        return rt_strcmp(pattern, word) == 0;
    }

    suffix_len = pattern_len - percent - 1U;
    if (word_len < percent + suffix_len) {
        return 0;
    }

    for (i = 0; i < percent; ++i) {
        if (pattern[i] != word[i]) {
            return 0;
        }
    }
    for (i = 0; i < suffix_len; ++i) {
        if (pattern[pattern_len - suffix_len + i] != word[word_len - suffix_len + i]) {
            return 0;
        }
    }

    return 1;
}

static int run_shell_capture(const char *command, char *out, size_t out_size) {
    int pipe_fds[2];
    int pid = -1;
    int status = 0;
    char raw[MAKE_VALUE_CAPACITY];
    size_t used = 0U;
    char *argv_exec[4];

    out[0] = '\0';
    if (platform_create_pipe(pipe_fds) != 0) {
        return -1;
    }

    argv_exec[0] = "/bin/sh";
    argv_exec[1] = "-c";
    argv_exec[2] = (char *)command;
    argv_exec[3] = 0;

    if (platform_spawn_process(argv_exec, -1, pipe_fds[1], 0, 0, 0, &pid) != 0) {
        platform_close(pipe_fds[0]);
        platform_close(pipe_fds[1]);
        return -1;
    }

    platform_close(pipe_fds[1]);
    while (used + 1U < sizeof(raw)) {
        long bytes_read = platform_read(pipe_fds[0], raw + used, sizeof(raw) - used - 1U);
        if (bytes_read <= 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    raw[used] = '\0';
    platform_close(pipe_fds[0]);
    (void)platform_wait_process(pid, &status);

    return normalize_whitespace_text(raw, out, out_size);
}

static int expand_text_recursive(
    const MakeProgram *program,
    const MakeRule *rule,
    const char *text,
    char *out,
    size_t out_size,
    int depth
);

static int evaluate_make_function(
    const MakeProgram *program,
    const MakeRule *rule,
    const char *name,
    const char *args_text,
    char *out,
    size_t out_size,
    int depth
) {
    char args[3][MAKE_VALUE_CAPACITY];
    size_t arg_count = split_function_args(args_text, args, 3);

    out[0] = '\0';

    if (rt_strcmp(name, "strip") == 0) {
        char expanded[MAKE_VALUE_CAPACITY];
        if (arg_count == 0U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), expanded, sizeof(expanded), depth + 1) != 0) {
            return -1;
        }
        return normalize_whitespace_text(expanded, out, out_size);
    }

    if (rt_strcmp(name, "shell") == 0) {
        char expanded[MAKE_VALUE_CAPACITY];
        if (arg_count == 0U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), expanded, sizeof(expanded), depth + 1) != 0) {
            return -1;
        }
        return run_shell_capture(expanded, out, out_size);
    }

    if (rt_strcmp(name, "origin") == 0) {
        char expanded[MAKE_VALUE_CAPACITY];
        char *trimmed;
        if (arg_count == 0U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), expanded, sizeof(expanded), depth + 1) != 0) {
            return -1;
        }
        trimmed = trim_leading_whitespace(expanded);
        trim_trailing_whitespace(trimmed);
        rt_copy_string(out, out_size, get_variable_origin_text(program, trimmed));
        return 0;
    }

    if (rt_strcmp(name, "if") == 0) {
        char condition[MAKE_VALUE_CAPACITY];
        char normalized[MAKE_VALUE_CAPACITY];
        const char *selected = "";

        if (arg_count == 0U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), condition, sizeof(condition), depth + 1) != 0 ||
            normalize_whitespace_text(condition, normalized, sizeof(normalized)) != 0) {
            return -1;
        }

        if (normalized[0] != '\0') {
            if (arg_count >= 2U) {
                selected = trim_leading_whitespace(args[1]);
            }
        } else if (arg_count >= 3U) {
            selected = trim_leading_whitespace(args[2]);
        }

        return expand_text_recursive(program, rule, selected, out, out_size, depth + 1);
    }

    if (rt_strcmp(name, "filter") == 0) {
        char patterns[MAKE_VALUE_CAPACITY];
        char text_words[MAKE_VALUE_CAPACITY];
        char normalized[MAKE_VALUE_CAPACITY];
        char *word_cursor;
        size_t used = 0U;

        if (arg_count < 2U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), patterns, sizeof(patterns), depth + 1) != 0 ||
            expand_text_recursive(program, rule, trim_leading_whitespace(args[1]), text_words, sizeof(text_words), depth + 1) != 0 ||
            normalize_whitespace_text(text_words, normalized, sizeof(normalized)) != 0) {
            return -1;
        }

        out[0] = '\0';
        word_cursor = normalized;
        while (*word_cursor != '\0') {
            char word[MAKE_NAME_CAPACITY];
            char pattern_text[MAKE_VALUE_CAPACITY];
            char *pattern_cursor;
            size_t word_len = 0U;
            int matched = 0;

            while (*word_cursor == ' ') {
                word_cursor += 1;
            }
            while (*word_cursor != '\0' && *word_cursor != ' ' && word_len + 1U < sizeof(word)) {
                word[word_len++] = *word_cursor++;
            }
            word[word_len] = '\0';
            if (word[0] == '\0') {
                break;
            }

            rt_copy_string(pattern_text, sizeof(pattern_text), patterns);
            pattern_cursor = pattern_text;
            while (*pattern_cursor != '\0') {
                char pattern[MAKE_NAME_CAPACITY];
                size_t pattern_len = 0U;

                while (*pattern_cursor == ' ') {
                    pattern_cursor += 1;
                }
                while (*pattern_cursor != '\0' && *pattern_cursor != ' ' && pattern_len + 1U < sizeof(pattern)) {
                    pattern[pattern_len++] = *pattern_cursor++;
                }
                pattern[pattern_len] = '\0';
                if (pattern[0] != '\0' && word_matches_pattern(pattern, word)) {
                    matched = 1;
                    break;
                }
            }

            if (matched) {
                if (used > 0U && append_char(out, &used, out_size, ' ') != 0) {
                    return -1;
                }
                if (append_text(out, &used, out_size, word) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    if (rt_strcmp(name, "addprefix") == 0 || rt_strcmp(name, "addsuffix") == 0) {
        char affix[MAKE_VALUE_CAPACITY];
        char words_text[MAKE_VALUE_CAPACITY];
        char normalized[MAKE_VALUE_CAPACITY];
        char *cursor;
        size_t used = 0U;
        int prefix_mode = (rt_strcmp(name, "addprefix") == 0);

        if (arg_count < 2U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), affix, sizeof(affix), depth + 1) != 0 ||
            expand_text_recursive(program, rule, trim_leading_whitespace(args[1]), words_text, sizeof(words_text), depth + 1) != 0 ||
            normalize_whitespace_text(words_text, normalized, sizeof(normalized)) != 0) {
            return -1;
        }

        out[0] = '\0';
        cursor = normalized;
        while (*cursor != '\0') {
            char word[MAKE_NAME_CAPACITY];
            size_t word_len = 0U;

            while (*cursor == ' ') {
                cursor += 1;
            }
            while (*cursor != '\0' && *cursor != ' ' && word_len + 1U < sizeof(word)) {
                word[word_len++] = *cursor++;
            }
            word[word_len] = '\0';
            if (word[0] == '\0') {
                break;
            }

            if (used > 0U && append_char(out, &used, out_size, ' ') != 0) {
                return -1;
            }
            if (prefix_mode) {
                if (append_text(out, &used, out_size, affix) != 0 || append_text(out, &used, out_size, word) != 0) {
                    return -1;
                }
            } else {
                if (append_text(out, &used, out_size, word) != 0 || append_text(out, &used, out_size, affix) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    if (rt_strcmp(name, "dir") == 0) {
        char words_text[MAKE_VALUE_CAPACITY];
        char normalized[MAKE_VALUE_CAPACITY];
        char *cursor;
        size_t used = 0U;

        if (arg_count == 0U) {
            return 0;
        }
        if (expand_text_recursive(program, rule, trim_leading_whitespace(args[0]), words_text, sizeof(words_text), depth + 1) != 0 ||
            normalize_whitespace_text(words_text, normalized, sizeof(normalized)) != 0) {
            return -1;
        }

        out[0] = '\0';
        cursor = normalized;
        while (*cursor != '\0') {
            char word[MAKE_NAME_CAPACITY];
            size_t word_len = 0U;
            size_t slash = 0U;
            size_t i;

            while (*cursor == ' ') {
                cursor += 1;
            }
            while (*cursor != '\0' && *cursor != ' ' && word_len + 1U < sizeof(word)) {
                word[word_len++] = *cursor++;
            }
            word[word_len] = '\0';
            if (word[0] == '\0') {
                break;
            }

            for (i = 0; i < word_len; ++i) {
                if (word[i] == '/') {
                    slash = i + 1U;
                }
            }

            if (used > 0U && append_char(out, &used, out_size, ' ') != 0) {
                return -1;
            }
            if (slash == 0U) {
                if (append_text(out, &used, out_size, "./") != 0) {
                    return -1;
                }
            } else {
                char dir_text[MAKE_NAME_CAPACITY];
                memcpy(dir_text, word, slash);
                dir_text[slash] = '\0';
                if (append_text(out, &used, out_size, dir_text) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    return -1;
}

static int evaluate_make_expression(
    const MakeProgram *program,
    const MakeRule *rule,
    const char *expression,
    char *out,
    size_t out_size,
    int depth
) {
    char expr_copy[MAKE_VALUE_CAPACITY];
    char name[MAKE_NAME_CAPACITY];
    const char *cursor;
    size_t name_len = 0U;

    rt_copy_string(expr_copy, sizeof(expr_copy), expression);
    cursor = trim_leading_whitespace(expr_copy);
    trim_trailing_whitespace((char *)cursor);

    while (cursor[name_len] != '\0' && cursor[name_len] != ' ' && cursor[name_len] != '\t' && cursor[name_len] != ',') {
        if (name_len + 1U >= sizeof(name)) {
            return -1;
        }
        name[name_len] = cursor[name_len];
        name_len += 1U;
    }
    name[name_len] = '\0';

    if ((rt_strcmp(name, "strip") == 0 ||
         rt_strcmp(name, "shell") == 0 ||
         rt_strcmp(name, "filter") == 0 ||
         rt_strcmp(name, "addprefix") == 0 ||
         rt_strcmp(name, "addsuffix") == 0 ||
         rt_strcmp(name, "if") == 0 ||
         rt_strcmp(name, "origin") == 0 ||
         rt_strcmp(name, "dir") == 0) && cursor[name_len] != '\0') {
        return evaluate_make_function(program, rule, name, trim_leading_whitespace((char *)(cursor + name_len)), out, out_size, depth + 1);
    }

    {
        char expanded_name[MAKE_VALUE_CAPACITY];
        if (expand_text_recursive(program, rule, cursor, expanded_name, sizeof(expanded_name), depth + 1) != 0) {
            return -1;
        }
        return expand_text_recursive(program, rule, get_variable_value(program, expanded_name), out, out_size, depth + 1);
    }
}

static int expand_text_recursive(
    const MakeProgram *program,
    const MakeRule *rule,
    const char *text,
    char *out,
    size_t out_size,
    int depth
) {
    size_t used = 0;
    size_t pos = 0;

    if (depth > 32) {
        return -1;
    }

    out[0] = '\0';

    while (text[pos] != '\0') {
        if (text[pos] == '$') {
            char next = text[pos + 1];

            if (next == '$') {
                if (append_char(out, &used, out_size, '$') != 0) {
                    return -1;
                }
                pos += 2U;
            } else if (next == '@') {
                if (rule != 0 && append_text(out, &used, out_size, rule->target) != 0) {
                    return -1;
                }
                pos += 2U;
            } else if (next == '<') {
                if (rule != 0 && rule->dep_count > 0 && append_text(out, &used, out_size, rule->deps[0]) != 0) {
                    return -1;
                }
                pos += 2U;
            } else if (next == '^') {
                size_t i;
                if (rule != 0) {
                    for (i = 0; i < rule->dep_count; ++i) {
                        if (i > 0 && append_char(out, &used, out_size, ' ') != 0) {
                            return -1;
                        }
                        if (append_text(out, &used, out_size, rule->deps[i]) != 0) {
                            return -1;
                        }
                    }
                }
                pos += 2U;
            } else if (next == '*') {
                if (rule != 0 && append_text(out, &used, out_size, rule->stem) != 0) {
                    return -1;
                }
                pos += 2U;
            } else if (next == '(' || next == '{') {
                char expr[MAKE_VALUE_CAPACITY];
                char value[MAKE_VALUE_CAPACITY];
                char open_ch = next;
                char end_ch = (next == '(') ? ')' : '}';
                int nested = 1;
                size_t expr_len = 0U;

                pos += 2U;
                while (text[pos] != '\0') {
                    if (text[pos] == open_ch) {
                        nested += 1;
                    } else if (text[pos] == end_ch) {
                        nested -= 1;
                        if (nested == 0) {
                            break;
                        }
                    }

                    if (expr_len + 1U >= sizeof(expr)) {
                        return -1;
                    }
                    expr[expr_len++] = text[pos++];
                }
                if (text[pos] != end_ch) {
                    return -1;
                }
                expr[expr_len] = '\0';
                pos += 1U;

                if (evaluate_make_expression(program, rule, expr, value, sizeof(value), depth + 1) != 0 ||
                    append_text(out, &used, out_size, value) != 0) {
                    return -1;
                }
            } else {
                if (append_char(out, &used, out_size, text[pos]) != 0) {
                    return -1;
                }
                pos += 1U;
            }
        } else {
            if (append_char(out, &used, out_size, text[pos]) != 0) {
                return -1;
            }
            pos += 1U;
        }
    }

    return 0;
}

int expand_text(const MakeProgram *program, const MakeRule *rule, const char *text, char *out, size_t out_size) {
    return expand_text_recursive(program, rule, text, out, out_size, 0);
}

static int add_dependency_tokens(MakeProgram *program, MakeRule *rule, char *deps_text) {
    char expanded[MAKE_LINE_CAPACITY];
    char *cursor;

    if (expand_text(program, 0, deps_text, expanded, sizeof(expanded)) != 0) {
        return -1;
    }

    cursor = trim_leading_whitespace(expanded);
    while (*cursor != '\0') {
        char name[MAKE_NAME_CAPACITY];
        size_t len = 0;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor += 1;
        }
        if (*cursor == '\0') {
            break;
        }

        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && len + 1U < sizeof(name)) {
            name[len++] = *cursor++;
        }
        name[len] = '\0';

        if (rt_strcmp(name, "|") == 0) {
            continue;
        }
        if (rule->stem[0] != '\0' && make_target_has_pattern(name)) {
            char substituted[MAKE_NAME_CAPACITY];
            if (substitute_pattern_text(name, rule->stem, substituted, sizeof(substituted)) != 0) {
                return -1;
            }
            rt_copy_string(name, sizeof(name), substituted);
        }

        if (rule->dep_count >= MAKE_MAX_DEPS) {
            return -1;
        }
        rt_copy_string(rule->deps[rule->dep_count], sizeof(rule->deps[rule->dep_count]), name);
        rule->dep_count += 1U;
    }

    return 0;
}

static void copy_parent_directory(const char *path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(path);
    size_t i;

    if (buffer_size == 0) {
        return;
    }

    rt_copy_string(buffer, buffer_size, ".");
    if (path == 0 || path[0] == '\0') {
        return;
    }

    if (len + 1U > buffer_size) {
        len = buffer_size - 1U;
    }
    memcpy(buffer, path, len);
    buffer[len] = '\0';

    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1U) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int resolve_make_path(const char *base_path, const char *include_path, char *buffer, size_t buffer_size) {
    char parent[MAKE_LINE_CAPACITY];

    if (include_path[0] == '/') {
        rt_copy_string(buffer, buffer_size, include_path);
        return 0;
    }

    copy_parent_directory(base_path, parent, sizeof(parent));
    return tool_join_path(parent, include_path, buffer, buffer_size);
}

static int starts_with_directive(const char *text, const char *directive) {
    size_t i = 0U;

    while (directive[i] != '\0') {
        if (text[i] != directive[i]) {
            return 0;
        }
        i += 1U;
    }

    return text[i] == '\0' || text[i] == ' ' || text[i] == '\t' || text[i] == '(';
}

static int evaluate_condition_directive(const MakeProgram *program, const char *text, int *result_out) {
    char inner[MAKE_LINE_CAPACITY];
    char args[2][MAKE_VALUE_CAPACITY];
    char lhs[MAKE_VALUE_CAPACITY];
    char rhs[MAKE_VALUE_CAPACITY];
    const char *cursor = text;
    int negate = 0;
    int nested = 1;
    size_t used = 0U;
    size_t arg_count;
    char *lhs_trim;
    char *rhs_trim;

    if (starts_with_directive(cursor, "ifeq")) {
        cursor += 4;
    } else if (starts_with_directive(cursor, "ifneq")) {
        negate = 1;
        cursor += 5;
    } else {
        return -1;
    }

    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    if (*cursor != '(') {
        return -1;
    }
    cursor += 1;

    while (*cursor != '\0') {
        if (*cursor == '(') {
            nested += 1;
        } else if (*cursor == ')') {
            nested -= 1;
            if (nested == 0) {
                break;
            }
        }
        if (used + 1U >= sizeof(inner)) {
            return -1;
        }
        inner[used++] = *cursor++;
    }
    if (*cursor != ')') {
        return -1;
    }
    inner[used] = '\0';

    arg_count = split_function_args(inner, args, 2);
    if (arg_count < 2U) {
        return -1;
    }

    if (expand_text(program, 0, trim_leading_whitespace(args[0]), lhs, sizeof(lhs)) != 0 ||
        expand_text(program, 0, trim_leading_whitespace(args[1]), rhs, sizeof(rhs)) != 0) {
        return -1;
    }

    trim_trailing_whitespace(lhs);
    trim_trailing_whitespace(rhs);
    lhs_trim = trim_leading_whitespace(lhs);
    rhs_trim = trim_leading_whitespace(rhs);
    *result_out = (rt_strcmp(lhs_trim, rhs_trim) == 0);
    if (negate) {
        *result_out = !*result_out;
    }
    return 0;
}

static int parse_makefile_internal(MakeProgram *program, const char *path, int depth) {
    int fd;
    char chunk[512];
    char line[MAKE_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;
    MakeRule *current_rules[MAKE_MAX_RULES];
    size_t current_rule_count = 0U;
    int cond_parent[32];
    int cond_active[32];
    int cond_taken[32];
    size_t cond_depth = 0U;

    if (depth > 8) {
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                char *content;
                char *trimmed;
                int active = 1;

                if (line_len > 0U && line[line_len - 1U] == '\\') {
                    line[line_len - 1U] = ' ';
                    continue;
                }

                line[line_len] = '\0';
                trim_trailing_whitespace(line);
                content = line;
                trimmed = trim_leading_whitespace(content);

                if (starts_with_directive(trimmed, "ifeq") || starts_with_directive(trimmed, "ifneq")) {
                    int cond = 0;
                    int parent_active = (cond_depth == 0U) ? 1 : cond_active[cond_depth - 1U];

                    if (cond_depth >= sizeof(cond_parent) / sizeof(cond_parent[0])) {
                        platform_close(fd);
                        return -1;
                    }
                    if (parent_active && evaluate_condition_directive(program, trimmed, &cond) != 0) {
                        platform_close(fd);
                        return -1;
                    }
                    cond_parent[cond_depth] = parent_active;
                    cond_active[cond_depth] = parent_active && cond;
                    cond_taken[cond_depth] = cond ? 1 : 0;
                    cond_depth += 1U;
                    current_rule_count = 0U;
                    line_len = 0U;
                    continue;
                }

                if (starts_with_directive(trimmed, "else")) {
                    const char *rest = trim_leading_whitespace(trimmed + 4);
                    size_t top;

                    if (cond_depth == 0U) {
                        platform_close(fd);
                        return -1;
                    }
                    top = cond_depth - 1U;
                    if (*rest == '\0') {
                        cond_active[top] = cond_parent[top] && !cond_taken[top];
                        cond_taken[top] = 1;
                    } else {
                        int cond = 0;
                        if (evaluate_condition_directive(program, rest, &cond) != 0) {
                            platform_close(fd);
                            return -1;
                        }
                        cond_active[top] = cond_parent[top] && !cond_taken[top] && cond;
                        if (cond) {
                            cond_taken[top] = 1;
                        }
                    }
                    current_rule_count = 0U;
                    line_len = 0U;
                    continue;
                }

                if (starts_with_directive(trimmed, "endif")) {
                    if (cond_depth == 0U) {
                        platform_close(fd);
                        return -1;
                    }
                    cond_depth -= 1U;
                    current_rule_count = 0U;
                    line_len = 0U;
                    continue;
                }

                if (cond_depth > 0U) {
                    active = cond_active[cond_depth - 1U];
                }
                if (!active) {
                    current_rule_count = 0U;
                    line_len = 0U;
                    continue;
                }

                if (is_blank_or_comment(content)) {
                    current_rule_count = 0U;
                } else if (starts_with_space(content)) {
                    if (current_rule_count > 0U) {
                        char *command = trim_leading_whitespace(content);
                        size_t rule_index;

                        for (rule_index = 0U; rule_index < current_rule_count; ++rule_index) {
                            MakeRule *current_rule = current_rules[rule_index];
                            if (current_rule->command_count >= MAKE_MAX_COMMANDS) {
                                platform_close(fd);
                                return -1;
                            }
                            rt_copy_string(
                                current_rule->commands[current_rule->command_count],
                                sizeof(current_rule->commands[current_rule->command_count]),
                                command
                            );
                            current_rule->command_count += 1U;
                        }
                    }
                } else {
                    char *colon = 0;
                    char *second_colon = 0;
                    size_t j = 0;

                    current_rule_count = 0U;

                    while (content[j] != '\0') {
                        if (content[j] == ':' && colon == 0) {
                            colon = content + j;
                        } else if (content[j] == ':' && colon != 0 && second_colon == 0) {
                            second_colon = content + j;
                        }
                        j += 1U;
                    }

                    if (starts_with_keyword(content, "include") || starts_with_keyword(content, "-include")) {
                        char expanded[MAKE_LINE_CAPACITY];
                        char include_path[MAKE_LINE_CAPACITY];
                        char *cursor;
                        int optional = (content[0] == '-');
                        char *include_text = content + (optional ? 8 : 7);

                        if (expand_text(program, 0, trim_leading_whitespace(include_text), expanded, sizeof(expanded)) != 0) {
                            platform_close(fd);
                            return -1;
                        }

                        cursor = expanded;
                        while (*cursor != '\0') {
                            size_t include_len = 0;

                            while (*cursor == ' ' || *cursor == '\t') {
                                cursor += 1U;
                            }
                            if (*cursor == '\0') {
                                break;
                            }

                            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && include_len + 1U < sizeof(include_path)) {
                                include_path[include_len++] = *cursor++;
                            }
                            include_path[include_len] = '\0';

                            if (include_path[0] != '\0') {
                                char resolved[MAKE_LINE_CAPACITY];
                                if (resolve_make_path(path, include_path, resolved, sizeof(resolved)) != 0 ||
                                    parse_makefile_internal(program, resolved, depth + 1) != 0) {
                                    if (!optional) {
                                        platform_close(fd);
                                        return -1;
                                    }
                                }
                            }
                        }
                    } else if (starts_with_keyword(content, "export")) {
                        char export_text[MAKE_LINE_CAPACITY];
                        char *cursor;

                        rt_copy_string(export_text, sizeof(export_text), trim_leading_whitespace(content + 6));
                        trim_trailing_whitespace(export_text);

                        if (export_text[0] != '\0') {
                            int handled = handle_variable_assignment(program, export_text, 1);
                            if (handled < 0) {
                                platform_close(fd);
                                return -1;
                            }
                            if (handled == 0) {
                                cursor = export_text;
                                while (*cursor != '\0') {
                                    char name[MAKE_NAME_CAPACITY];
                                    size_t name_len = 0U;

                                    while (*cursor == ' ' || *cursor == '\t') {
                                        cursor += 1U;
                                    }
                                    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
                                           name_len + 1U < sizeof(name)) {
                                        name[name_len++] = *cursor++;
                                    }
                                    name[name_len] = '\0';

                                    if (name[0] != '\0' && mark_variable_exported(program, name, 1) != 0) {
                                        platform_close(fd);
                                        return -1;
                                    }
                                }
                            }
                        }
                    } else {
                        int handled_assignment = handle_variable_assignment(program, content, 0);
                        if (handled_assignment < 0) {
                            platform_close(fd);
                            return -1;
                        }
                        if (handled_assignment == 0 && colon != 0) {
                        char targets_text[MAKE_LINE_CAPACITY];
                        char deps_text[MAKE_LINE_CAPACITY];
                        char pattern_text[MAKE_LINE_CAPACITY];
                        char expanded_targets[MAKE_LINE_CAPACITY];
                        char expanded_pattern[MAKE_LINE_CAPACITY];
                        char *cursor;
                        int has_static_pattern = 0;

                        *colon = '\0';
                        rt_copy_string(targets_text, sizeof(targets_text), trim_leading_whitespace(content));
                        trim_trailing_whitespace(targets_text);
                        if (targets_text[0] == '\0') {
                            platform_close(fd);
                            return -1;
                        }

                        if (second_colon != 0) {
                            *second_colon = '\0';
                            rt_copy_string(pattern_text, sizeof(pattern_text), trim_leading_whitespace(colon + 1));
                            trim_trailing_whitespace(pattern_text);
                            rt_copy_string(deps_text, sizeof(deps_text), trim_leading_whitespace(second_colon + 1));
                            has_static_pattern = 1;
                        } else {
                            pattern_text[0] = '\0';
                            rt_copy_string(deps_text, sizeof(deps_text), trim_leading_whitespace(colon + 1));
                        }

                        if (expand_text(program, 0, targets_text, expanded_targets, sizeof(expanded_targets)) != 0) {
                            platform_close(fd);
                            return -1;
                        }
                        if (has_static_pattern && expand_text(program, 0, pattern_text, expanded_pattern, sizeof(expanded_pattern)) != 0) {
                            platform_close(fd);
                            return -1;
                        }

                        cursor = trim_leading_whitespace(expanded_targets);
                        while (*cursor != '\0') {
                            char target[MAKE_NAME_CAPACITY];
                            size_t len = 0U;
                            MakeRule *rule;

                            while (*cursor == ' ' || *cursor == '\t') {
                                cursor += 1;
                            }
                            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && len + 1U < sizeof(target)) {
                                target[len++] = *cursor++;
                            }
                            target[len] = '\0';
                            if (target[0] == '\0') {
                                break;
                            }

                            if (rt_strcmp(target, ".ONESHELL") == 0) {
                                program->oneshell = 1;
                                continue;
                            }

                            if (rt_strcmp(target, ".PHONY") == 0) {
                                char phony_text[MAKE_LINE_CAPACITY];
                                char *phony_cursor;

                                rt_copy_string(phony_text, sizeof(phony_text), deps_text);
                                phony_cursor = phony_text;
                                while (*phony_cursor != '\0') {
                                    char name[MAKE_NAME_CAPACITY];
                                    size_t name_len = 0U;

                                    while (*phony_cursor == ' ' || *phony_cursor == '\t') {
                                        phony_cursor += 1;
                                    }
                                    while (*phony_cursor != '\0' && *phony_cursor != ' ' && *phony_cursor != '\t' && name_len + 1U < sizeof(name)) {
                                        name[name_len++] = *phony_cursor++;
                                    }
                                    name[name_len] = '\0';

                                    if (name[0] != '\0' && rt_strcmp(name, "|") != 0 && program->phony_count < MAKE_MAX_PHONY) {
                                        rt_copy_string(program->phony[program->phony_count], sizeof(program->phony[program->phony_count]), name);
                                        program->phony_count += 1U;
                                    }
                                }
                                continue;
                            }

                            rule = find_rule(program, target);
                            if (rule == 0) {
                                if (program->rule_count >= MAKE_MAX_RULES) {
                                    platform_close(fd);
                                    return -1;
                                }
                                rule = &program->rules[program->rule_count++];
                                rt_memset(rule, 0, sizeof(*rule));
                                rt_copy_string(rule->target, sizeof(rule->target), target);
                            }

                            rule->dep_count = 0U;
                            rule->is_pattern = has_static_pattern ? 0 : make_target_has_pattern(target);
                            if (has_static_pattern) {
                                char stem[MAKE_NAME_CAPACITY];
                                if (!match_target_pattern(expanded_pattern, target, stem, sizeof(stem))) {
                                    platform_close(fd);
                                    return -1;
                                }
                                rt_copy_string(rule->stem, sizeof(rule->stem), stem);
                            } else {
                                rule->stem[0] = '\0';
                            }

                            if (program->first_target[0] == '\0' && target[0] != '.' && !rule->is_pattern) {
                                rt_copy_string(program->first_target, sizeof(program->first_target), target);
                            }

                            if (add_dependency_tokens(program, rule, deps_text) != 0) {
                                platform_close(fd);
                                return -1;
                            }

                            if (current_rule_count < MAKE_MAX_RULES) {
                                current_rules[current_rule_count++] = rule;
                            }
                        }
                        }
                    }
                }

                line_len = 0U;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    platform_close(fd);
    return bytes_read < 0 ? -1 : 0;
}

int parse_makefile(MakeProgram *program, const char *path) {
    return parse_makefile_internal(program, path, 0);
}
