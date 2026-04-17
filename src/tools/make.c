#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAKE_MAX_RULES 128
#define MAKE_MAX_DEPS 32
#define MAKE_MAX_COMMANDS 32
#define MAKE_MAX_VARS 64
#define MAKE_MAX_PHONY 64
#define MAKE_NAME_CAPACITY 128
#define MAKE_VALUE_CAPACITY 512
#define MAKE_COMMAND_CAPACITY 512
#define MAKE_LINE_CAPACITY 1024

typedef struct {
    char name[MAKE_NAME_CAPACITY];
    char value[MAKE_VALUE_CAPACITY];
} MakeVariable;

typedef struct {
    char target[MAKE_NAME_CAPACITY];
    char deps[MAKE_MAX_DEPS][MAKE_NAME_CAPACITY];
    size_t dep_count;
    char commands[MAKE_MAX_COMMANDS][MAKE_COMMAND_CAPACITY];
    size_t command_count;
    int building;
    int built;
} MakeRule;

typedef struct {
    MakeVariable vars[MAKE_MAX_VARS];
    size_t var_count;
    MakeRule rules[MAKE_MAX_RULES];
    size_t rule_count;
    char phony[MAKE_MAX_PHONY][MAKE_NAME_CAPACITY];
    size_t phony_count;
    char first_target[MAKE_NAME_CAPACITY];
    int dry_run;
} MakeProgram;

static void trim_trailing_whitespace(char *text) {
    size_t len = rt_strlen(text);

    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len -= 1U;
    }
}

static char *trim_leading_whitespace(char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

static int starts_with_space(const char *text) {
    return text[0] == ' ' || text[0] == '\t';
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

static int set_variable(MakeProgram *program, const char *name, const char *value) {
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

static MakeRule *find_rule(MakeProgram *program, const char *target) {
    size_t i;

    for (i = 0; i < program->rule_count; ++i) {
        if (rt_strcmp(program->rules[i].target, target) == 0) {
            return &program->rules[i];
        }
    }

    return 0;
}

static int is_phony_target(const MakeProgram *program, const char *name) {
    size_t i;

    for (i = 0; i < program->phony_count; ++i) {
        if (rt_strcmp(program->phony[i], name) == 0) {
            return 1;
        }
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

static int expand_text(const MakeProgram *program, const MakeRule *rule, const char *text, char *out, size_t out_size) {
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

static int parse_makefile(MakeProgram *program, const char *path) {
    int fd;
    char chunk[512];
    char line[MAKE_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;
    MakeRule *current_rule = 0;

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

                    if (equals != 0 && (colon == 0 || equals < colon)) {
                        char name[MAKE_NAME_CAPACITY];
                        char value[MAKE_VALUE_CAPACITY];
                        size_t name_len;

                        *equals = '\0';
                        rt_copy_string(name, sizeof(name), trim_leading_whitespace(content));
                        trim_trailing_whitespace(name);
                        rt_copy_string(value, sizeof(value), trim_leading_whitespace(equals + 1));
                        name_len = rt_strlen(name);
                        if (name_len > 0 && set_variable(program, name, value) != 0) {
                            platform_close(fd);
                            return -1;
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
                        }

                        if (program->first_target[0] == '\0' && target[0] != '.') {
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

static int path_exists_and_mtime(const char *path, long long *mtime_out) {
    PlatformDirEntry entry;

    if (platform_get_path_info(path, &entry) != 0) {
        return 0;
    }

    *mtime_out = entry.mtime;
    return 1;
}

static int run_rule_commands(MakeProgram *program, MakeRule *rule) {
    size_t i;

    for (i = 0; i < rule->command_count; ++i) {
        char expanded[MAKE_LINE_CAPACITY];
        char *command_ptr = expanded;
        int silent = 0;
        int ignore_error = 0;
        char *argv_exec[4];
        int pid;
        int exit_status;

        if (expand_text(program, rule, rule->commands[i], expanded, sizeof(expanded)) != 0) {
            return -1;
        }

        while (*command_ptr == '@' || *command_ptr == '-') {
            if (*command_ptr == '@') {
                silent = 1;
            } else if (*command_ptr == '-') {
                ignore_error = 1;
            }
            command_ptr += 1;
        }
        command_ptr = trim_leading_whitespace(command_ptr);

        if (!silent) {
            rt_write_line(1, command_ptr);
        }

        if (program->dry_run) {
            continue;
        }

        argv_exec[0] = "sh";
        argv_exec[1] = "-c";
        argv_exec[2] = command_ptr;
        argv_exec[3] = 0;

        if (platform_spawn_process(argv_exec, 0, 1, 0, 0, 0, &pid) != 0 || platform_wait_process(pid, &exit_status) != 0) {
            return -1;
        }

        if (exit_status != 0 && !ignore_error) {
            return exit_status;
        }
    }

    return 0;
}

static int build_target(MakeProgram *program, const char *target) {
    MakeRule *rule = find_rule(program, target);
    size_t i;
    int need_run = 0;
    long long target_mtime = 0;
    int target_exists = path_exists_and_mtime(target, &target_mtime);

    if (rule == 0) {
        if (target_exists) {
            return 0;
        }
        tool_write_error("make", "no rule to make target ", target);
        return 1;
    }

    if (rule->building) {
        tool_write_error("make", "dependency cycle on ", target);
        return 1;
    }
    if (rule->built) {
        return 0;
    }

    rule->building = 1;

    for (i = 0; i < rule->dep_count; ++i) {
        long long dep_mtime = 0;
        if (build_target(program, rule->deps[i]) != 0) {
            rule->building = 0;
            return 1;
        }
        if (!target_exists || (path_exists_and_mtime(rule->deps[i], &dep_mtime) && dep_mtime > target_mtime)) {
            need_run = 1;
        }
    }

    if (is_phony_target(program, target)) {
        need_run = 1;
    }

    if (!target_exists && rule->command_count > 0) {
        need_run = 1;
    }

    if (rule->command_count == 0) {
        if (!target_exists && rule->dep_count == 0 && !is_phony_target(program, target)) {
            tool_write_error("make", "nothing to do for ", target);
            rule->building = 0;
            return 1;
        }
    } else if (need_run) {
        int run_status = run_rule_commands(program, rule);
        if (run_status != 0) {
            rule->building = 0;
            return run_status < 0 ? 1 : run_status;
        }
    }

    rule->building = 0;
    rule->built = 1;
    return 0;
}

int main(int argc, char **argv) {
    MakeProgram program;
    const char *makefile_path = "Makefile";
    const char *targets[32];
    size_t target_count = 0;
    int i;

    rt_memset(&program, 0, sizeof(program));

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            tool_write_usage(tool_base_name(argv[0]), "[-n] [-f makefile] [VAR=value] [target ...]");
            return 0;
        } else if (rt_strcmp(argv[i], "-n") == 0) {
            program.dry_run = 1;
        } else if (rt_strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            makefile_path = argv[++i];
        } else if (rt_strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            if (platform_change_directory(argv[++i]) != 0) {
                tool_write_error("make", "cannot change directory to ", argv[i]);
                return 1;
            }
        } else {
            size_t j = 0;
            int has_equals = 0;
            while (argv[i][j] != '\0') {
                if (argv[i][j] == '=') {
                    char name[MAKE_NAME_CAPACITY];
                    char value[MAKE_VALUE_CAPACITY];
                    size_t name_len = j;
                    if (name_len >= sizeof(name)) {
                        name_len = sizeof(name) - 1U;
                    }
                    memcpy(name, argv[i], name_len);
                    name[name_len] = '\0';
                    rt_copy_string(value, sizeof(value), argv[i] + j + 1U);
                    set_variable(&program, name, value);
                    has_equals = 1;
                    break;
                }
                j += 1U;
            }

            if (!has_equals) {
                if (target_count >= sizeof(targets) / sizeof(targets[0])) {
                    tool_write_error("make", "too many targets", 0);
                    return 1;
                }
                targets[target_count++] = argv[i];
            }
        }
    }

    if (parse_makefile(&program, makefile_path) != 0) {
        tool_write_error("make", "cannot read makefile ", makefile_path);
        return 1;
    }

    if (target_count == 0) {
        if (program.first_target[0] == '\0') {
            tool_write_error("make", "no targets found in ", makefile_path);
            return 1;
        }
        targets[target_count++] = program.first_target;
    }

    for (i = 0; i < (int)target_count; ++i) {
        if (build_target(&program, targets[i]) != 0) {
            return 1;
        }
    }

    return 0;
}
