/*
 * awk.c - entry point for the awk tool.
 *
 * Argument parsing and top-level orchestration only.
 * Implementation is split across awk/awk_parse.c and awk/awk_exec.c.
 */

#include "awk/awk_impl.h"

#define AWK_PROGRAM_CAPACITY (32U * 1024U)

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-F SEP] [-v VAR=VALUE] [-f PROGRAM_FILE]... ['program'] [file ...]");
}

static int is_identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static int is_identifier_char(char ch) {
    return is_identifier_start(ch) || (ch >= '0' && ch <= '9');
}

static int append_program_text(char *buffer, size_t *length, size_t capacity, const char *text) {
    size_t i = 0;

    if (buffer == 0 || length == 0 || text == 0) {
        return -1;
    }

    if (*length > 0U && buffer[*length - 1U] != '\n') {
        if (*length + 2U > capacity) {
            return -1;
        }
        buffer[(*length)++] = '\n';
    }

    while (text[i] != '\0') {
        if (*length + 2U > capacity) {
            return -1;
        }
        buffer[(*length)++] = text[i++];
    }

    buffer[*length] = '\0';
    return 0;
}

static int append_program_file(char *buffer, size_t *length, size_t capacity, const char *path) {
    int fd;
    int should_close;
    char chunk[1024];
    long bytes_read;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    if (*length > 0U && buffer[*length - 1U] != '\n') {
        if (*length + 2U > capacity) {
            tool_close_input(fd, should_close);
            return -1;
        }
        buffer[(*length)++] = '\n';
        buffer[*length] = '\0';
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        size_t offset = 0U;

        while (offset < (size_t)bytes_read) {
            if (*length + 2U > capacity) {
                tool_close_input(fd, should_close);
                return -1;
            }
            buffer[(*length)++] = chunk[offset++];
        }
        buffer[*length] = '\0';
    }

    tool_close_input(fd, should_close);
    return bytes_read < 0 ? -1 : 0;
}

static int parse_v_assignment(const char *text, AwkState *state) {
    char name[AWK_MAX_TEXT];
    char value[AWK_MAX_TEXT];
    size_t split = 0U;
    size_t out = 0U;

    if (text == 0) {
        return -1;
    }

    while (text[split] != '\0' && text[split] != '=') {
        if ((split == 0U && !is_identifier_start(text[split])) ||
            (split > 0U && !is_identifier_char(text[split])) ||
            split + 1U >= sizeof(name)) {
            return -1;
        }
        name[split] = text[split];
        split += 1U;
    }

    if (split == 0U || text[split] != '=') {
        return -1;
    }
    name[split] = '\0';

    if (tool_parse_escaped_string(text + split + 1U, value, sizeof(value), &out) != 0) {
        return -1;
    }

    (void)out;
    return awk_assign_variable(state, name, value);
}

int main(int argc, char **argv) {
    AwkProgram program;
    AwkState state;
    AwkRecord edge_record;
    unsigned long long line_number = 0;
    unsigned long long last_nf = 0;
    char program_text[AWK_PROGRAM_CAPACITY];
    size_t program_length = 0U;
    int argi = 1;
    int have_script_file = 0;
    int i;
    int exit_code = 0;

    init_state(&state);
    program_text[0] = '\0';

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *value = 0;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (rt_strcmp(argv[argi], "-F") == 0 || (argv[argi][1] == 'F' && argv[argi][2] != '\0')) {
            char fs_value[AWK_MAX_TEXT];
            value = (rt_strcmp(argv[argi], "-F") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || tool_parse_escaped_string(value, fs_value, sizeof(fs_value), 0) != 0 ||
                awk_assign_variable(&state, "FS", fs_value) != 0) {
                tool_write_error("awk", "invalid field separator", 0);
                print_usage(argv[0]);
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-F") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-v") == 0 || (argv[argi][1] == 'v' && argv[argi][2] != '\0')) {
            value = (rt_strcmp(argv[argi], "-v") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || parse_v_assignment(value, &state) != 0) {
                tool_write_error("awk", "invalid -v assignment: ", value != 0 ? value : "");
                print_usage(argv[0]);
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-v") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-f") == 0 || (argv[argi][1] == 'f' && argv[argi][2] != '\0')) {
            value = (rt_strcmp(argv[argi], "-f") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || append_program_file(program_text, &program_length, sizeof(program_text), value) != 0) {
                tool_write_error("awk", "cannot read program file ", value != 0 ? value : "");
                return 1;
            }
            have_script_file = 1;
            argi += (rt_strcmp(argv[argi], "-f") == 0) ? 2 : 1;
            continue;
        }
        break;
    }

    if (!have_script_file) {
        if (argi >= argc || append_program_text(program_text, &program_length, sizeof(program_text), argv[argi]) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (program_text[0] == '\0' || parse_program(program_text, &program) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    init_record(&edge_record, "", 0, &state);
    if (execute_clauses(&program, AWK_CLAUSE_BEGIN, &edge_record, &state) != 0) {
        return 1;
    }

    if (argi == argc) {
        awk_set_filename(&state, "-");
        if (awk_stream(0, &program, &state, &line_number, &last_nf) != 0) {
            return 1;
        }
    } else {
        for (i = argi; i < argc; ++i) {
            int fd;
            int should_close;

            awk_set_filename(&state, argv[i]);
            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "awk: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }

            if (awk_stream(fd, &program, &state, &line_number, &last_nf) != 0) {
                rt_write_cstr(2, "awk: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    init_record(&edge_record, "", line_number, &state);
    edge_record.nf = last_nf;
    if (execute_clauses(&program, AWK_CLAUSE_END, &edge_record, &state) != 0) {
        return 1;
    }

    return exit_code;
}
