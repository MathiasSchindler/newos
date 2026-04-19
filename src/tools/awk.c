/*
 * awk.c - entry point for the awk tool.
 *
 * Argument parsing and top-level orchestration only.
 * Implementation is split across awk_parse.c and awk_exec.c.
 */

#include "awk_impl.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " 'BEGIN { FS=\":\"; OFS=\"|\" } /pattern/ { printf \"%s\\n\", $1 } END { print NR }' [file ...]");
}

int main(int argc, char **argv) {
    AwkProgram program;
    AwkState state;
    AwkRecord edge_record;
    unsigned long long line_number = 0;
    unsigned long long last_nf = 0;
    int i;
    int exit_code = 0;

    if (argc < 2 || parse_program(argv[1], &program) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    init_state(&state);
    init_record(&edge_record, "", 0, &state);
    if (execute_clauses(&program, AWK_CLAUSE_BEGIN, &edge_record, &state) != 0) {
        return 1;
    }

    if (argc == 2) {
        if (awk_stream(0, &program, &state, &line_number, &last_nf) != 0) {
            return 1;
        }
    } else {
        for (i = 2; i < argc; ++i) {
            int fd;
            int should_close;

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
