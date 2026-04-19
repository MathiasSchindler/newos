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

static const char *get_variable_value(const MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->var_count; ++i) {
        if (rt_strcmp(program->vars[i].name, name) == 0) {
            return program->vars[i].value;
        }
    }

    return "";
}

int set_variable(MakeProgram *program, const char *name, const char *value) {
    MakeVariable *variable = find_variable(program, name);

    if (variable == 0) {
        if (program->var_count >= MAKE_MAX_VARS) {
            return -1;
        }
        variable = &program->vars[program->var_count++];
    }

    rt_copy_string(variable->name, sizeof(variable->name), name);
    rt_copy_string(variable->value, sizeof(variable->value), value);
    return 0;
}

static int append_variable(MakeProgram *program, const char *name, const char *value) {
    MakeVariable *variable = find_variable(program, name);
    char combined[MAKE_VALUE_CAPACITY];
    size_t used = 0;

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

    if (depth > 8) {
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
                char end_ch = (next == '(') ? ')' : '}';
                char name[MAKE_NAME_CAPACITY];
                char value[MAKE_VALUE_CAPACITY];
                size_t name_len = 0;
                size_t value_len = 0;

                pos += 2U;
                while (text[pos] != '\0' && text[pos] != end_ch && name_len + 1U < sizeof(name)) {
                    name[name_len++] = text[pos++];
                }
                if (text[pos] != end_ch) {
                    return -1;
                }
                name[name_len] = '\0';
                pos += 1U;

                value[0] = '\0';
                if (expand_text_recursive(program, rule, get_variable_value(program, name), value, sizeof(value), depth + 1) != 0) {
                    return -1;
                }
                value_len = rt_strlen(value);
                if (used + value_len + 1 > out_size) {
                    return -1;
                }
                memcpy(out + used, value, value_len);
                used += value_len;
                out[used] = '\0';
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

static int parse_makefile_internal(MakeProgram *program, const char *path, int depth) {
    int fd;
    char chunk[512];
    char line[MAKE_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;
    MakeRule *current_rule = 0;

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

            if (ch == '\n') {
                char *content;

                line[line_len] = '\0';
                trim_trailing_whitespace(line);
                content = line;

                if (is_blank_or_comment(content)) {
                    current_rule = 0;
                } else if (starts_with_space(content)) {
                    if (current_rule != 0) {
                        char *command = trim_leading_whitespace(content);
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
                } else {
                    char *equals = 0;
                    char *colon = 0;
                    size_t j = 0;

                    current_rule = 0;

                    while (content[j] != '\0') {
                        if (content[j] == ':' && colon == 0) {
                            colon = content + j;
                        }
                        if (content[j] == '=' && equals == 0) {
                            equals = content + j;
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
                    } else if (equals != 0 &&
                               (colon == 0 || equals < colon ||
                                (equals > content && (equals[-1] == ':' || equals[-1] == '+' || equals[-1] == '?')))) {
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

                        if (name_len > 0) {
                            if (assign_mode == ':') {
                                if (expand_text(program, 0, value, expanded, sizeof(expanded)) != 0 ||
                                    set_variable(program, name, expanded) != 0) {
                                    platform_close(fd);
                                    return -1;
                                }
                            } else if (assign_mode == '?') {
                                MakeVariable *existing = find_variable(program, name);
                                if ((existing == 0 || existing->value[0] == '\0') &&
                                    set_variable(program, name, value) != 0) {
                                    platform_close(fd);
                                    return -1;
                                }
                            } else if (assign_mode == '+') {
                                if (append_variable(program, name, value) != 0) {
                                    platform_close(fd);
                                    return -1;
                                }
                            } else if (set_variable(program, name, value) != 0) {
                                platform_close(fd);
                                return -1;
                            }
                        }
                    } else if (colon != 0) {
                        char target[MAKE_NAME_CAPACITY];
                        MakeRule *rule;

                        *colon = '\0';
                        rt_copy_string(target, sizeof(target), trim_leading_whitespace(content));
                        trim_trailing_whitespace(target);
                        if (target[0] == '\0') {
                            platform_close(fd);
                            return -1;
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
                            rule->is_pattern = make_target_has_pattern(target);
                        }

                        if (program->first_target[0] == '\0' && target[0] != '.' && !make_target_has_pattern(target)) {
                            rt_copy_string(program->first_target, sizeof(program->first_target), target);
                        }

                        if (rt_strcmp(target, ".PHONY") == 0) {
                            char phony_text[MAKE_LINE_CAPACITY];
                            char *cursor;

                            rt_copy_string(phony_text, sizeof(phony_text), trim_leading_whitespace(colon + 1));
                            cursor = phony_text;
                            while (*cursor != '\0') {
                                char name[MAKE_NAME_CAPACITY];
                                size_t len = 0;

                                while (*cursor == ' ' || *cursor == '\t') {
                                    cursor += 1;
                                }
                                while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && len + 1U < sizeof(name)) {
                                    name[len++] = *cursor++;
                                }
                                name[len] = '\0';

                                if (name[0] != '\0' && program->phony_count < MAKE_MAX_PHONY) {
                                    rt_copy_string(program->phony[program->phony_count], sizeof(program->phony[program->phony_count]), name);
                                    program->phony_count += 1U;
                                }
                            }
                        } else {
                            rule->dep_count = 0;
                            if (add_dependency_tokens(program, rule, trim_leading_whitespace(colon + 1)) != 0) {
                                platform_close(fd);
                                return -1;
                            }
                            current_rule = rule;
                        }
                    }
                }

                line_len = 0;
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
