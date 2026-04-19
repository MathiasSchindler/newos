/*
 * make.c - entry point for the make tool.
 *
 * Argument parsing and top-level orchestration only.
 * Implementation is split across make/make_parse.c and make/make_exec.c.
 */

#include "make/make_impl.h"

int main(int argc, char **argv) {
    MakeProgram program;
    ToolOptState s;
    const char *makefile_path = 0;
    const char *targets[32];
    char selected_makefile[MAKE_LINE_CAPACITY];
    size_t target_count = 0;
    int r;
    int i;

    rt_memset(&program, 0, sizeof(program));

    tool_opt_init(&s, argc, argv, "make", "[-n] [-f makefile] [VAR=value] [target ...]");
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(s.flag, "-n") == 0) {
            program.dry_run = 1;
        } else if (rt_strcmp(s.flag, "-f") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            makefile_path = s.value;
        } else if (rt_strcmp(s.flag, "-C") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (platform_change_directory(s.value) != 0) {
                tool_write_error("make", "cannot change directory to ", s.value);
                return 1;
            }
        } else {
            tool_write_error("make", "unknown option: ", s.flag);
            tool_write_usage("make", "[-n] [-f makefile] [VAR=value] [target ...]");
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        tool_write_usage(tool_base_name(argv[0]), "[-n] [-f makefile] [VAR=value] [target ...]");
        return 0;
    }

    /* Process remaining arguments: VAR=value assignments and target names */
    for (i = s.argi; i < argc; ++i) {
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

    if (makefile_path == 0) {
        long long mtime = 0;

        if (path_exists_and_mtime("Makefile", &mtime)) {
            rt_copy_string(selected_makefile, sizeof(selected_makefile), "Makefile");
        } else if (path_exists_and_mtime("makefile", &mtime)) {
            rt_copy_string(selected_makefile, sizeof(selected_makefile), "makefile");
        } else if (path_exists_and_mtime("GNUmakefile", &mtime)) {
            rt_copy_string(selected_makefile, sizeof(selected_makefile), "GNUmakefile");
        } else {
            rt_copy_string(selected_makefile, sizeof(selected_makefile), "Makefile");
        }
        makefile_path = selected_makefile;
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
