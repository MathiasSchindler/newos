/*
 * make.c - entry point for the make tool.
 *
 * Argument parsing and top-level orchestration only.
 * Implementation is split across make/make_parse.c and make/make_exec.c.
 */

#include "make/make_impl.h"

static int append_makeflag(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    if (*used_io > 0U) {
        if (*used_io + 1U >= buffer_size) {
            return -1;
        }
        buffer[*used_io] = ' ';
        *used_io += 1U;
    }
    if (rt_strlen(text) + *used_io + 1U > buffer_size) {
        return -1;
    }
    rt_copy_string(buffer + *used_io, buffer_size - *used_io, text);
    *used_io += rt_strlen(text);
    return 0;
}

static void refresh_makeflags(MakeProgram *program) {
    char flags[MAKE_VALUE_CAPACITY];
    size_t used = 0U;

    flags[0] = '\0';
    if (program->dry_run) {
        (void)append_makeflag(flags, sizeof(flags), &used, "-n");
    }
    if (program->silent) {
        (void)append_makeflag(flags, sizeof(flags), &used, "-s");
    }
    if (program->always_make) {
        (void)append_makeflag(flags, sizeof(flags), &used, "-B");
    }
    if (program->jobs_flag_present) {
        if (program->requested_jobs > 0ULL) {
            char jobs_text[32];
            char flag_text[40];
            rt_unsigned_to_string(program->requested_jobs, jobs_text, sizeof(jobs_text));
            flag_text[0] = '-';
            flag_text[1] = 'j';
            flag_text[2] = '\0';
            rt_copy_string(flag_text + 2, sizeof(flag_text) - 2U, jobs_text);
            (void)append_makeflag(flags, sizeof(flags), &used, flag_text);
        } else {
            (void)append_makeflag(flags, sizeof(flags), &used, "-j");
        }
    }

    (void)set_variable_with_origin(program, "MAKEFLAGS", flags, MAKE_ORIGIN_FILE);
}

int main(int argc, char **argv) {
    static MakeProgram program;
    ToolOptState s;
    const char *makefile_path = 0;
    const char *targets[32];
    char selected_makefile[MAKE_LINE_CAPACITY];
    size_t target_count = 0;
    int r;
    int i;

    rt_memset(&program, 0, sizeof(program));
    set_variable_with_origin(&program, "MAKE", argv[0] != 0 ? argv[0] : "make", MAKE_ORIGIN_FILE);

    tool_opt_init(&s, argc, argv, "make", "[-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]] [--color[=WHEN]] [VAR=value] [target ...]");
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(s.flag, "-n") == 0) {
            program.dry_run = 1;
        } else if (rt_strcmp(s.flag, "-s") == 0 || rt_strcmp(s.flag, "--silent") == 0) {
            program.silent = 1;
        } else if (rt_strcmp(s.flag, "-B") == 0 || rt_strcmp(s.flag, "--always-make") == 0) {
            program.always_make = 1;
        } else if (rt_strcmp(s.flag, "-f") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            makefile_path = s.value;
        } else if (rt_strcmp(s.flag, "-C") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (platform_change_directory(s.value) != 0) {
                tool_write_error("make", "cannot change directory to ", s.value);
                return 1;
            }
        } else if (rt_strcmp(s.flag, "--color") == 0) {
            tool_set_global_color_mode(TOOL_COLOR_AUTO);
        } else if (rt_strncmp(s.flag, "--color=", 8U) == 0) {
            int color_mode = TOOL_COLOR_AUTO;
            if (tool_parse_color_mode(s.flag + 8, &color_mode) != 0) {
                tool_write_error("make", "invalid color mode ", s.flag + 8);
                tool_write_usage("make", "[-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]] [--color[=WHEN]] [VAR=value] [target ...]");
                return 1;
            }
            tool_set_global_color_mode(color_mode);
        } else if (rt_strcmp(s.flag, "--no-print-directory") == 0 ||
                   (s.flag[0] == '-' && s.flag[1] == '-' && s.flag[2] == 'j' && s.flag[3] == 'o' &&
                    s.flag[4] == 'b' && s.flag[5] == 's' && s.flag[6] == 'e' && s.flag[7] == 'r' &&
                    s.flag[8] == 'v' && s.flag[9] == 'e' && s.flag[10] == 'r')) {
            /* GNU make compatibility flags accepted but ignored. */
        } else if (s.flag[0] == '-' && s.flag[1] == 'j') {
            program.jobs_flag_present = 1;
            if (rt_strcmp(s.flag, "-j") == 0 && s.argi < argc) {
                const char *jobs_arg = argv[s.argi];
                size_t jobs_index = 0U;
                int all_digits = jobs_arg[0] != '\0';

                while (jobs_arg[jobs_index] != '\0') {
                    if (jobs_arg[jobs_index] < '0' || jobs_arg[jobs_index] > '9') {
                        all_digits = 0;
                        break;
                    }
                    jobs_index += 1U;
                }
                if (all_digits) {
                    (void)tool_parse_uint_arg(jobs_arg, &program.requested_jobs, "make", "jobs");
                    s.argi += 1;
                }
            } else if (s.flag[2] != '\0') {
                if (tool_parse_uint_arg(s.flag + 2, &program.requested_jobs, "make", "jobs") != 0) {
                    return 1;
                }
            }
            /* Parallel execution is not implemented yet; accept the flag. */
        } else {
            tool_write_error("make", "unknown option: ", s.flag);
            tool_write_usage("make", "[-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]] [--color[=WHEN]] [VAR=value] [target ...]");
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        tool_write_usage(tool_base_name(argv[0]), "[-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]] [--color[=WHEN]] [VAR=value] [target ...]");
        return 0;
    }

    /* Process remaining arguments: compatibility flags, VAR=value assignments, and target names. */
    for (i = s.argi; i < argc; ++i) {
        size_t j = 0;
        int has_equals = 0;

            if (argv[i][0] == '-') {
                if (rt_strcmp(argv[i], "-n") == 0) {
                    program.dry_run = 1;
                    continue;
                }
                if (rt_strcmp(argv[i], "-s") == 0 || rt_strcmp(argv[i], "--silent") == 0) {
                    program.silent = 1;
                    continue;
                }
                if (rt_strcmp(argv[i], "-B") == 0 || rt_strcmp(argv[i], "--always-make") == 0) {
                    program.always_make = 1;
                    continue;
                }
                if (rt_strcmp(argv[i], "--color") == 0) {
                    tool_set_global_color_mode(TOOL_COLOR_AUTO);
                    continue;
            }
            if (rt_strncmp(argv[i], "--color=", 8U) == 0) {
                int color_mode = TOOL_COLOR_AUTO;
                if (tool_parse_color_mode(argv[i] + 8, &color_mode) != 0) {
                    tool_write_error("make", "invalid color mode ", argv[i] + 8);
                    return 1;
                }
                tool_set_global_color_mode(color_mode);
                continue;
            }
            if (rt_strcmp(argv[i], "-f") == 0) {
                if (i + 1 >= argc) {
                    tool_write_error("make", "missing value for ", "-f");
                    return 1;
                }
                makefile_path = argv[i + 1];
                i += 1;
                continue;
            }
            if (rt_strcmp(argv[i], "-C") == 0) {
                if (i + 1 >= argc) {
                    tool_write_error("make", "missing value for ", "-C");
                    return 1;
                }
                if (platform_change_directory(argv[i + 1]) != 0) {
                    tool_write_error("make", "cannot change directory to ", argv[i + 1]);
                    return 1;
                }
                i += 1;
                continue;
            }
            if (rt_strcmp(argv[i], "--no-print-directory") == 0 ||
                (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == 'j' && argv[i][3] == 'o' &&
                 argv[i][4] == 'b' && argv[i][5] == 's' && argv[i][6] == 'e' && argv[i][7] == 'r' &&
                 argv[i][8] == 'v' && argv[i][9] == 'e' && argv[i][10] == 'r')) {
                continue;
            }
            if (argv[i][0] == '-' && argv[i][1] == 'j') {
                program.jobs_flag_present = 1;
                if (argv[i][2] == '\0' && i + 1 < argc) {
                    const char *jobs_arg = argv[i + 1];
                    size_t jobs_index = 0U;
                    int all_digits = jobs_arg[0] != '\0';

                    while (jobs_arg[jobs_index] != '\0') {
                        if (jobs_arg[jobs_index] < '0' || jobs_arg[jobs_index] > '9') {
                            all_digits = 0;
                            break;
                        }
                        jobs_index += 1U;
                    }
                    if (all_digits) {
                        if (tool_parse_uint_arg(jobs_arg, &program.requested_jobs, "make", "jobs") != 0) {
                            return 1;
                        }
                        i += 1;
                    }
                } else if (argv[i][2] != '\0') {
                    if (tool_parse_uint_arg(argv[i] + 2, &program.requested_jobs, "make", "jobs") != 0) {
                        return 1;
                    }
                }
                continue;
            }

            tool_write_error("make", "unknown option: ", argv[i]);
            tool_write_usage("make", "[-n] [-s] [-B] [-f makefile] [-C dir] [-j [jobs]] [--color[=WHEN]] [VAR=value] [target ...]");
            return 1;
        }

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
                set_variable_with_origin(&program, name, value, MAKE_ORIGIN_COMMAND_LINE);
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

    refresh_makeflags(&program);

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
