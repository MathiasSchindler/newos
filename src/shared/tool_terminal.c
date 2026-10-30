#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if defined(NEWOS_TOOL_DEFAULT_COLOR_NEVER) && NEWOS_TOOL_DEFAULT_COLOR_NEVER
static int tool_global_color_mode = TOOL_COLOR_NEVER;
#else
static int tool_global_color_mode = TOOL_COLOR_AUTO;
#endif

static int env_value_is_nonzero(const char *value) {
    return value != 0 && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

static int tool_write_style_code(int fd, int style) {
    volatile int selected = style;
    if (selected == TOOL_STYLE_BOLD) return rt_write_cstr(fd, "1") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_RED) return rt_write_cstr(fd, "31") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_GREEN) return rt_write_cstr(fd, "32") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_YELLOW) return rt_write_cstr(fd, "33") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BLUE) return rt_write_cstr(fd, "34") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_MAGENTA) return rt_write_cstr(fd, "35") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_CYAN) return rt_write_cstr(fd, "36") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_RED) return rt_write_cstr(fd, "1;31") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_GREEN) return rt_write_cstr(fd, "1;32") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_YELLOW) return rt_write_cstr(fd, "1;33") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_BLUE) return rt_write_cstr(fd, "1;34") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_MAGENTA) return rt_write_cstr(fd, "1;35") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_CYAN) return rt_write_cstr(fd, "1;36") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_WHITE) return rt_write_cstr(fd, "1;97") == 0 ? 1 : -1;
    return 0;
}

int tool_parse_color_mode(const char *text, int *mode_out) {
    if (text == 0 || mode_out == 0) {
        return -1;
    }

    if (rt_strcmp(text, "always") == 0 || rt_strcmp(text, "yes") == 0 || rt_strcmp(text, "force") == 0) {
        *mode_out = TOOL_COLOR_ALWAYS;
        return 0;
    }
    if (rt_strcmp(text, "auto") == 0) {
        *mode_out = TOOL_COLOR_AUTO;
        return 0;
    }
    if (rt_strcmp(text, "never") == 0 || rt_strcmp(text, "none") == 0 || rt_strcmp(text, "no") == 0) {
        *mode_out = TOOL_COLOR_NEVER;
        return 0;
    }

    return -1;
}

void tool_set_global_color_mode(int mode) {
    tool_global_color_mode = mode;
}

int tool_get_global_color_mode(void) {
    return tool_global_color_mode;
}

int tool_should_use_color_fd(int fd, int mode) {
    const char *term;
    const char *no_color;
    const char *clicolor;
    const char *clicolor_force;

    if (mode == TOOL_COLOR_ALWAYS) {
        return 1;
    }
    if (mode == TOOL_COLOR_NEVER) {
        return 0;
    }

    clicolor_force = platform_getenv("CLICOLOR_FORCE");
    if (env_value_is_nonzero(clicolor_force)) {
        return 1;
    }

    no_color = platform_getenv("NO_COLOR");
    if (no_color != 0) {
        return 0;
    }

    clicolor = platform_getenv("CLICOLOR");
    if (clicolor != 0 && clicolor[0] == '0' && clicolor[1] == '\0') {
        return 0;
    }

    term = platform_getenv("TERM");
    if (term != 0 && rt_strcmp(term, "dumb") == 0) {
        return 0;
    }

    return platform_isatty(fd) != 0;
}

void tool_style_begin(int fd, int mode, int style) {
    int wrote_code;

    if (!tool_should_use_color_fd(fd, mode) || style == TOOL_STYLE_PLAIN) {
        return;
    }

    if (rt_write_cstr(fd, "\033[") != 0) {
        return;
    }
    wrote_code = tool_write_style_code(fd, style);
    if (wrote_code == 1) {
        rt_write_char(fd, 'm');
    }
}

void tool_style_end(int fd, int mode) {
    if (tool_should_use_color_fd(fd, mode)) {
        rt_write_cstr(fd, "\033[0m");
    }
}

void tool_write_styled(int fd, int mode, int style, const char *text) {
    int use_style;

    if (text == 0) {
        return;
    }
    use_style = tool_should_use_color_fd(fd, mode) && style != TOOL_STYLE_PLAIN;
    if (use_style) {
        tool_style_begin(fd, mode, style);
    }
    rt_write_cstr(fd, text);
    if (use_style) {
        tool_style_end(fd, mode);
    }
}

void tool_restore_terminal_mode_if_enabled(int fd, int *enabled_io, const PlatformTerminalState *state) {
    if (enabled_io != 0 && *enabled_io && state != 0) {
        (void)platform_terminal_restore_mode(fd, state);
        *enabled_io = 0;
    }
}

unsigned int tool_pager_page_lines(unsigned int default_lines) {
    const char *text = platform_getenv("LINES");
    unsigned long long value = 0;
    unsigned int rows = 0U;

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value > 1ULL && value < 1000ULL) {
        return (unsigned int)(value - 1ULL);
    }

    if (platform_get_terminal_size(1, &rows, 0) == 0 && rows > 1U && rows < 1000U) {
        return rows - 1U;
    }

    return default_lines;
}
