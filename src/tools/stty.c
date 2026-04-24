#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void stty_usage(const char *program_name) {
    tool_write_usage(program_name, "[-a|size|raw|sane|echo|-echo|icanon|-icanon|isig|-isig|ixon|-ixon|opost|-opost|rows N|cols N|columns N]");
}

static int write_flag(const char *name, int enabled, int *first_io) {
    if (!*first_io && rt_write_char(1, ' ') != 0) {
        return -1;
    }
    *first_io = 0;
    if (!enabled && rt_write_char(1, '-') != 0) {
        return -1;
    }
    return rt_write_cstr(1, name);
}

static int print_mode(const PlatformTerminalMode *mode) {
    int first = 1;

    if (rt_write_cstr(1, "speed 38400 baud; rows ") != 0 ||
        rt_write_uint(1, mode->rows) != 0 ||
        rt_write_cstr(1, "; columns ") != 0 ||
        rt_write_uint(1, mode->columns) != 0 ||
        rt_write_char(1, '\n') != 0) {
        return -1;
    }
    if (write_flag("echo", mode->echo, &first) != 0 ||
        write_flag("icanon", mode->icanon, &first) != 0 ||
        write_flag("isig", mode->isig, &first) != 0 ||
        write_flag("ixon", mode->ixon, &first) != 0 ||
        write_flag("opost", mode->opost, &first) != 0 ||
        rt_write_char(1, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int parse_dimension(const char *text, unsigned int *value_out) {
    unsigned long long value = 0ULL;

    if (text == 0 || rt_parse_uint(text, &value) != 0 || value > 65535ULL) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static void set_flag(int enabled, int *field, unsigned int flag, unsigned int *mask) {
    *field = enabled;
    *mask |= flag;
}

int main(int argc, char **argv) {
    const char *program_name = tool_base_name(argv[0]);
    PlatformTerminalMode mode;
    unsigned int change_mask = 0U;
    int argi;

    if (argc == 2 &&
        (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0)) {
        stty_usage(program_name);
        return 0;
    }

    if (platform_terminal_get_mode(0, &mode) != 0) {
        tool_write_error(program_name, "standard input is not a terminal", 0);
        return 1;
    }

    if (argc == 1 || (argc == 2 && rt_strcmp(argv[1], "-a") == 0)) {
        return print_mode(&mode) == 0 ? 0 : 1;
    }
    if (argc == 2 && rt_strcmp(argv[1], "size") == 0) {
        return rt_write_uint(1, mode.rows) == 0 &&
               rt_write_char(1, ' ') == 0 &&
               rt_write_uint(1, mode.columns) == 0 &&
               rt_write_char(1, '\n') == 0 ? 0 : 1;
    }

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "raw") == 0) {
            set_flag(0, &mode.echo, PLATFORM_TERMINAL_ECHO, &change_mask);
            set_flag(0, &mode.icanon, PLATFORM_TERMINAL_ICANON, &change_mask);
            set_flag(0, &mode.isig, PLATFORM_TERMINAL_ISIG, &change_mask);
            set_flag(0, &mode.ixon, PLATFORM_TERMINAL_IXON, &change_mask);
            set_flag(0, &mode.opost, PLATFORM_TERMINAL_OPOST, &change_mask);
        } else if (rt_strcmp(argv[argi], "sane") == 0 || rt_strcmp(argv[argi], "cooked") == 0) {
            set_flag(1, &mode.echo, PLATFORM_TERMINAL_ECHO, &change_mask);
            set_flag(1, &mode.icanon, PLATFORM_TERMINAL_ICANON, &change_mask);
            set_flag(1, &mode.isig, PLATFORM_TERMINAL_ISIG, &change_mask);
            set_flag(1, &mode.ixon, PLATFORM_TERMINAL_IXON, &change_mask);
            set_flag(1, &mode.opost, PLATFORM_TERMINAL_OPOST, &change_mask);
        } else if (rt_strcmp(argv[argi], "echo") == 0) {
            set_flag(1, &mode.echo, PLATFORM_TERMINAL_ECHO, &change_mask);
        } else if (rt_strcmp(argv[argi], "-echo") == 0) {
            set_flag(0, &mode.echo, PLATFORM_TERMINAL_ECHO, &change_mask);
        } else if (rt_strcmp(argv[argi], "icanon") == 0) {
            set_flag(1, &mode.icanon, PLATFORM_TERMINAL_ICANON, &change_mask);
        } else if (rt_strcmp(argv[argi], "-icanon") == 0) {
            set_flag(0, &mode.icanon, PLATFORM_TERMINAL_ICANON, &change_mask);
        } else if (rt_strcmp(argv[argi], "isig") == 0) {
            set_flag(1, &mode.isig, PLATFORM_TERMINAL_ISIG, &change_mask);
        } else if (rt_strcmp(argv[argi], "-isig") == 0) {
            set_flag(0, &mode.isig, PLATFORM_TERMINAL_ISIG, &change_mask);
        } else if (rt_strcmp(argv[argi], "ixon") == 0) {
            set_flag(1, &mode.ixon, PLATFORM_TERMINAL_IXON, &change_mask);
        } else if (rt_strcmp(argv[argi], "-ixon") == 0) {
            set_flag(0, &mode.ixon, PLATFORM_TERMINAL_IXON, &change_mask);
        } else if (rt_strcmp(argv[argi], "opost") == 0) {
            set_flag(1, &mode.opost, PLATFORM_TERMINAL_OPOST, &change_mask);
        } else if (rt_strcmp(argv[argi], "-opost") == 0) {
            set_flag(0, &mode.opost, PLATFORM_TERMINAL_OPOST, &change_mask);
        } else if (rt_strcmp(argv[argi], "rows") == 0) {
            argi += 1;
            if (argi >= argc || parse_dimension(argv[argi], &mode.rows) != 0) {
                tool_write_error(program_name, "invalid rows value", 0);
                return 1;
            }
            change_mask |= PLATFORM_TERMINAL_ROWS;
        } else if (rt_strcmp(argv[argi], "cols") == 0 || rt_strcmp(argv[argi], "columns") == 0) {
            argi += 1;
            if (argi >= argc || parse_dimension(argv[argi], &mode.columns) != 0) {
                tool_write_error(program_name, "invalid columns value", 0);
                return 1;
            }
            change_mask |= PLATFORM_TERMINAL_COLUMNS;
        } else {
            tool_write_error(program_name, "unknown mode: ", argv[argi]);
            stty_usage(program_name);
            return 1;
        }
    }

    if (platform_terminal_set_mode(0, &mode, change_mask) != 0) {
        tool_write_error(program_name, "failed to update terminal mode", 0);
        return 1;
    }

    return 0;
}
