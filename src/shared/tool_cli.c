/*
 * tool_cli.c - lightweight iterative option parser for newos tools.
 *
 * See tool_util.h (ToolOptState) for the API and usage pattern.
 */

#include "runtime.h"
#include "tool_util.h"

void tool_opt_init(ToolOptState *s, int argc, char **argv,
                   const char *prog, const char *usage_suffix) {
    s->argc = argc;
    s->argv = argv;
    s->prog = prog;
    s->usage_suffix = usage_suffix;
    s->argi = 1;
    s->flag = 0;
    s->value = 0;
}

/*
 * Advance to the next option.  Returns one of:
 *   TOOL_OPT_END   - no more options; s->argi = first positional index
 *   TOOL_OPT_FLAG  - s->flag = current option string; argi advanced past it
 *   TOOL_OPT_HELP  - -h / --help was seen
 *   TOOL_OPT_ERROR - reserved for future use (currently never returned here)
 *
 * Stops at the first argument that does not begin with '-', at a bare '-'
 * (stdin placeholder), or at the "--" end-of-options marker.
 */
int tool_opt_next(ToolOptState *s) {
    const char *arg;

    while (1) {
        if (s->argi >= s->argc) {
            return TOOL_OPT_END;
        }

        arg = s->argv[s->argi];

        /* bare "--" ends option scanning */
        if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
            s->argi += 1;
            return TOOL_OPT_END;
        }

        /* not a flag: single '-' is a stdin placeholder, not an option */
        if (arg[0] != '-' || arg[1] == '\0') {
            return TOOL_OPT_END;
        }

        s->flag = arg;
        s->argi += 1;

        if (rt_strcmp(arg, "--json") == 0) {
            tool_json_set_enabled(1);
            continue;
        }

        /* unified help detection */
        if ((arg[1] == 'h' && arg[2] == '\0') || rt_strcmp(arg, "--help") == 0) {
            return TOOL_OPT_HELP;
        }

        return TOOL_OPT_FLAG;
    }
}

/*
 * Consume the next argv entry as the value for the current option.
 * Writes an error and returns -1 if no argument is available.
 */
int tool_opt_require_value(ToolOptState *s) {
    if (s->argi >= s->argc) {
        tool_write_error(s->prog, "option requires an argument: ", s->flag);
        tool_write_usage(s->prog, s->usage_suffix);
        return -1;
    }
    s->value = s->argv[s->argi];
    s->argi += 1;
    return 0;
}

int tool_should_print_file_header(int verbose, int quiet, int path_count) {
    if (verbose) {
        return 1;
    }
    if (quiet) {
        return 0;
    }
    return path_count > 1;
}

void tool_write_usage(const char *program_name, const char *usage_suffix) {
    if (tool_json_is_enabled()) {
        (void)tool_json_write_usage(program_name, usage_suffix);
        return;
    }
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_CYAN, "Usage:");
    rt_write_char(2, ' ');
    rt_write_cstr(2, program_name);
    if (usage_suffix != 0 && usage_suffix[0] != '\0') {
        rt_write_char(2, ' ');
        rt_write_cstr(2, usage_suffix);
    }
    rt_write_char(2, '\n');
}

void tool_write_error(const char *tool_name, const char *message, const char *detail) {
    if (tool_json_is_enabled()) {
        (void)tool_json_write_diagnostic(tool_name, "error", message, detail);
        return;
    }
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_RED, tool_name);
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_RED, ": ");
    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

int tool_prompt_yes_no(const char *message, const char *path) {
    char ch = '\0';
    char answer = '\0';
    long bytes_read;

    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (path != 0) {
        rt_write_cstr(2, path);
    }
    rt_write_cstr(2, "? ");

    for (;;) {
        bytes_read = platform_read(0, &ch, 1U);
        if (bytes_read <= 0) {
            return 0;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        if (answer == '\0' && ch != ' ' && ch != '\t') {
            answer = ch;
        }
    }

    return answer == 'y' || answer == 'Y';
}
