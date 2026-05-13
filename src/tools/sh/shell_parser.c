#include "shell_shared.h"
#include "runtime.h"
#include "tool_util.h"

static int shell_text_is_space(const char *text, size_t *advance_out) {
    size_t index = 0U;
    unsigned int codepoint = 0U;
    size_t length;

    if (text == 0 || text[0] == '\0') {
        if (advance_out != 0) {
            *advance_out = 0U;
        }
        return 0;
    }

    length = rt_strlen(text);
    if (rt_utf8_decode(text, length, &index, &codepoint) != 0 || index == 0U) {
        index = 1U;
        codepoint = (unsigned char)text[0];
    }

    if (advance_out != 0) {
        *advance_out = index;
    }
    return rt_unicode_is_space(codepoint);
}

static int parse_word(char **cursor, char **word_out, int *no_expand_out) {
    char *src = *cursor;
    char *dst = *cursor;
    char terminator;
    int saw_any = 0;
    int no_expand = 0;
    size_t separator_advance = 0U;

    while (*src != '\0' && !shell_text_is_space(src, &separator_advance) && *src != '|' && *src != '<' && *src != '>') {
        if (*src == '\\' && src[1] != '\0') {
            src += 1;
            *dst++ = *src++;
            saw_any = 1;
            no_expand = 1;
        } else if (*src == '\'') {
            src += 1;
            no_expand = 1;
            saw_any = 1;
            while (*src != '\0' && *src != '\'') {
                *dst++ = *src++;
            }
            if (*src != '\'') {
                return -1;
            }
            src += 1;
        } else if (*src == '"') {
            src += 1;
            no_expand = 1;
            saw_any = 1;
            while (*src != '\0' && *src != '"') {
                if (*src == '\\' && src[1] != '\0') {
                    src += 1;
                }
                *dst++ = *src++;
            }
            if (*src != '"') {
                return -1;
            }
            src += 1;
        } else {
            *dst++ = *src++;
            saw_any = 1;
        }
    }

    if (!saw_any) {
        return -1;
    }

    terminator = *src;
    *dst = '\0';
    *word_out = *cursor;
    *no_expand_out = no_expand;

    if (terminator != '\0' && terminator != '|' && terminator != '<' && terminator != '>') {
        src += separator_advance > 0U ? separator_advance : 1U;
    }

    *cursor = src;
    return 0;
}

static void sh_free_command(ShCommand *command) {
    size_t i;

    if (command == 0) {
        return;
    }
    for (i = 0; i < command->owned_word_count; ++i) {
        rt_free(command->owned_words[i]);
    }
    rt_free(command->owned_words);
    rt_free(command->argv);
    rt_free(command->no_expand);
    rt_memset(command, 0, sizeof(*command));
}

void sh_free_pipeline(ShPipeline *pipeline) {
    size_t i;

    if (pipeline == 0) {
        return;
    }
    for (i = 0; i < pipeline->count; ++i) {
        sh_free_command(&pipeline->commands[i]);
    }
    rt_free(pipeline->commands);
    rt_memset(pipeline, 0, sizeof(*pipeline));
}

static char *sh_duplicate_text(const char *text) {
    size_t length = rt_strlen(text);
    char *copy = (char *)rt_malloc(length + 1U);

    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static int sh_command_ensure_arg_capacity(ShCommand *command, size_t needed) {
    size_t next_capacity;
    char **next_argv;
    int *next_no_expand;

    if (needed <= command->argv_capacity) {
        return 0;
    }

    next_capacity = command->argv_capacity == 0U ? 8U : command->argv_capacity;
    while (next_capacity < needed) {
        next_capacity *= 2U;
    }

    next_argv = (char **)rt_realloc(command->argv, (next_capacity + 1U) * sizeof(*next_argv));
    if (next_argv == 0) {
        return -1;
    }
    command->argv = next_argv;

    next_no_expand = (int *)rt_realloc(command->no_expand, next_capacity * sizeof(*next_no_expand));
    if (next_no_expand == 0) {
        return -1;
    }
    command->no_expand = next_no_expand;
    command->argv_capacity = next_capacity;
    return 0;
}

static int sh_command_append_arg(ShCommand *command, char *word, int no_expand) {
    if (sh_command_ensure_arg_capacity(command, (size_t)command->argc + 1U) != 0) {
        return -1;
    }
    command->argv[command->argc] = word;
    command->no_expand[command->argc] = no_expand;
    command->argc += 1;
    command->argv[command->argc] = 0;
    return 0;
}

static int sh_command_take_owned_word(ShCommand *command, char *word) {
    char **next_words;
    size_t next_capacity;

    if (command->owned_word_count >= command->owned_word_capacity) {
        next_capacity = command->owned_word_capacity == 0U ? 8U : command->owned_word_capacity * 2U;
        next_words = (char **)rt_realloc(command->owned_words, next_capacity * sizeof(*next_words));
        if (next_words == 0) {
            return -1;
        }
        command->owned_words = next_words;
        command->owned_word_capacity = next_capacity;
    }
    command->owned_words[command->owned_word_count++] = word;
    return 0;
}

static int sh_pipeline_add_command(ShPipeline *pipeline, ShCommand **command_out) {
    ShCommand *next_commands;
    size_t next_capacity;

    if (pipeline->count >= pipeline->capacity) {
        next_capacity = pipeline->capacity == 0U ? 4U : pipeline->capacity * 2U;
        next_commands = (ShCommand *)rt_realloc(pipeline->commands, next_capacity * sizeof(*next_commands));
        if (next_commands == 0) {
            return -1;
        }
        rt_memset(next_commands + pipeline->capacity, 0, (next_capacity - pipeline->capacity) * sizeof(*next_commands));
        pipeline->commands = next_commands;
        pipeline->capacity = next_capacity;
    }
    *command_out = &pipeline->commands[pipeline->count++];
    rt_memset(*command_out, 0, sizeof(**command_out));
    return 0;
}

static int expand_command_globs(ShCommand *command) {
    char **expanded_argv = 0;
    int *expanded_no_expand = 0;
    size_t expanded_capacity = 0U;
    int new_argc = 0;
    int arg_index;

#define APPEND_EXPANDED_ARG(arg_text, no_expand_value) do { \
        if ((size_t)new_argc >= expanded_capacity) { \
            size_t next_capacity__ = expanded_capacity == 0U ? 8U : expanded_capacity * 2U; \
            char **next_argv__ = (char **)rt_malloc((next_capacity__ + 1U) * sizeof(*next_argv__)); \
            int *next_no_expand__ = (int *)rt_malloc(next_capacity__ * sizeof(*next_no_expand__)); \
            if (next_argv__ == 0 || next_no_expand__ == 0) { \
                rt_free(next_argv__); \
                rt_free(next_no_expand__); \
                rt_free(expanded_argv); \
                rt_free(expanded_no_expand); \
                return -1; \
            } \
            if (new_argc > 0) { \
                memcpy(next_argv__, expanded_argv, (size_t)(new_argc + 1) * sizeof(*next_argv__)); \
                memcpy(next_no_expand__, expanded_no_expand, (size_t)new_argc * sizeof(*next_no_expand__)); \
            } \
            rt_free(expanded_argv); \
            rt_free(expanded_no_expand); \
            expanded_argv = next_argv__; \
            expanded_no_expand = next_no_expand__; \
            expanded_capacity = next_capacity__; \
        } \
        expanded_argv[new_argc] = (arg_text); \
        expanded_no_expand[new_argc] = (no_expand_value); \
        new_argc += 1; \
        expanded_argv[new_argc] = 0; \
    } while (0)

    for (arg_index = 0; arg_index < command->argc; ++arg_index) {
        const char *arg = command->argv[arg_index];

        if (command->no_expand[arg_index] || !sh_contains_wildcards(arg)) {
            APPEND_EXPANDED_ARG(command->argv[arg_index], command->no_expand[arg_index]);
            continue;
        }

        {
            PlatformDirEntry entries[SH_ENTRY_CAPACITY];
            size_t count = 0;
            int is_directory = 0;
            char dir_path[SH_MAX_LINE];
            char pattern[SH_MAX_LINE];
            const char *slash = 0;
            size_t i = 0;
            int matched = 0;

            while (arg[i] != '\0') {
                if (arg[i] == '/') {
                    slash = arg + i;
                }
                i += 1;
            }

            if (slash != 0) {
                size_t dir_len = (size_t)(slash - arg);
                if (dir_len + 1 > sizeof(dir_path)) {
                    return -1;
                }
                memcpy(dir_path, arg, dir_len);
                dir_path[dir_len] = '\0';
                rt_copy_string(pattern, sizeof(pattern), slash + 1);
            } else {
                rt_copy_string(dir_path, sizeof(dir_path), ".");
                rt_copy_string(pattern, sizeof(pattern), arg);
            }

            if (platform_collect_entries(dir_path, pattern[0] == '.', entries, SH_ENTRY_CAPACITY, &count, &is_directory) == 0 && is_directory) {
                size_t entry_index;

                for (entry_index = 0; entry_index < count; ++entry_index) {
                    if (entries[entry_index].name[0] == '\0') {
                        continue;
                    }
                    if (tool_wildcard_match(pattern, entries[entry_index].name)) {
                        char matched_path[SH_MAX_LINE];
                        char *owned_path;

                        if (slash != 0) {
                            if (tool_join_path(dir_path, entries[entry_index].name, matched_path, sizeof(matched_path)) != 0) {
                                return -1;
                            }
                        } else {
                            rt_copy_string(matched_path, sizeof(matched_path), entries[entry_index].name);
                        }
                        owned_path = sh_duplicate_text(matched_path);
                        if (owned_path == 0 || sh_command_take_owned_word(command, owned_path) != 0) {
                            rt_free(owned_path);
                            rt_free(expanded_argv);
                            rt_free(expanded_no_expand);
                            return -1;
                        }
                        APPEND_EXPANDED_ARG(owned_path, 1);
                        matched = 1;
                    }
                }
            }

            if (!matched) {
                APPEND_EXPANDED_ARG(command->argv[arg_index], command->no_expand[arg_index]);
            }
        }
    }

    if (sh_command_ensure_arg_capacity(command, (size_t)new_argc) != 0) {
        rt_free(expanded_argv);
        rt_free(expanded_no_expand);
        return -1;
    }
    for (arg_index = 0; arg_index < new_argc; ++arg_index) {
        command->argv[arg_index] = expanded_argv[arg_index];
        command->no_expand[arg_index] = expanded_no_expand[arg_index];
    }
    rt_free(expanded_argv);
    rt_free(expanded_no_expand);
    command->argv[new_argc] = 0;
    command->argc = new_argc;
    return 0;
#undef APPEND_EXPANDED_ARG
}

void sh_cleanup_pipeline_temp_inputs(const ShPipeline *pipeline) {
    size_t i;

    for (i = 0; i < pipeline->count; ++i) {
        const char *path = pipeline->commands[i].input_path;
        const char *prefix = SH_HEREDOC_PREFIX;
        size_t j = 0;

        if (path == 0 || path[0] != '/') {
            continue;
        }

        while (prefix[j] != '\0' && path[j] == prefix[j]) {
            j += 1;
        }
        if (prefix[j] == '\0') {
            (void)platform_remove_file(path);
        }
    }
}

int sh_parse_pipeline(char *line, ShPipeline *pipeline, int *empty_out) {
    char *cursor = line;
    ShCommand *current;
    size_t i;

    rt_memset(pipeline, 0, sizeof(*pipeline));
    if (sh_pipeline_add_command(pipeline, &current) != 0) {
        return -1;
    }
    *empty_out = 0;

    for (;;) {
        char *word;
        int no_expand = 0;

        sh_skip_spaces(&cursor);
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '|') {
            if (current->argc == 0) {
                return -1;
            }
            current->argv[current->argc] = 0;
            cursor += 1;
            if (sh_pipeline_add_command(pipeline, &current) != 0) {
                return -1;
            }
            continue;
        }

        if (*cursor == '<') {
            cursor += 1;
            sh_skip_spaces(&cursor);
            if (parse_word(&cursor, &word, &no_expand) != 0) {
                return -1;
            }
            current->input_path = word;
            continue;
        }

        if (*cursor == '>') {
            int append = 0;
            cursor += 1;
            if (*cursor == '>') {
                append = 1;
                cursor += 1;
            }
            sh_skip_spaces(&cursor);
            if (parse_word(&cursor, &word, &no_expand) != 0) {
                return -1;
            }
            current->output_path = word;
            current->output_append = append;
            continue;
        }

        if (parse_word(&cursor, &word, &no_expand) != 0) {
            return -1;
        }

        if (sh_command_append_arg(current, word, no_expand) != 0) {
            return -1;
        }
    }

    if (pipeline->count == 1 && current->argc == 0 && current->input_path == 0 && current->output_path == 0) {
        *empty_out = 1;
        return 0;
    }

    for (i = 0; i < pipeline->count; ++i) {
        if (pipeline->commands[i].argc == 0) {
            return -1;
        }
        if (expand_command_globs(&pipeline->commands[i]) != 0) {
            return -1;
        }
        pipeline->commands[i].argv[pipeline->commands[i].argc] = 0;
    }

    return 0;
}
