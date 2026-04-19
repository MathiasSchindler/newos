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

static int expand_command_globs(ShCommand *command) {
    char *expanded_argv[SH_MAX_ARGS + 1];
    char storage[SH_MAX_ARGS][SH_MAX_LINE];
    int new_argc = 0;
    int arg_index;

    for (arg_index = 0; arg_index < command->argc; ++arg_index) {
        const char *arg = command->argv[arg_index];

        if (command->no_expand[arg_index] || !sh_contains_wildcards(arg)) {
            expanded_argv[new_argc++] = command->argv[arg_index];
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
                        int slot;

                        if (new_argc >= SH_MAX_ARGS) {
                            return -1;
                        }
                        slot = new_argc;
                        if (slash != 0) {
                            if (tool_join_path(dir_path, entries[entry_index].name, storage[slot], sizeof(storage[slot])) != 0) {
                                return -1;
                            }
                        } else {
                            rt_copy_string(storage[slot], sizeof(storage[slot]), entries[entry_index].name);
                        }
                        expanded_argv[slot] = storage[slot];
                        new_argc += 1;
                        matched = 1;
                    }
                }
            }

            if (!matched) {
                expanded_argv[new_argc++] = command->argv[arg_index];
            }
        }
    }

    for (arg_index = 0; arg_index < new_argc; ++arg_index) {
        command->argv[arg_index] = expanded_argv[arg_index];
    }
    command->argv[new_argc] = 0;
    command->argc = new_argc;
    return 0;
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
    pipeline->count = 1;
    current = &pipeline->commands[0];
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
            if (pipeline->count >= SH_MAX_COMMANDS) {
                return -1;
            }
            current = &pipeline->commands[pipeline->count++];
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

        if (current->argc >= SH_MAX_ARGS) {
            return -1;
        }

        current->argv[current->argc] = word;
        current->no_expand[current->argc] = no_expand;
        current->argc += 1;
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
