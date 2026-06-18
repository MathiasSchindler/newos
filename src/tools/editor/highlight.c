#include "highlight.h"

#include "runtime.h"
#include "tool_util.h"

static int editor_highlight_path_has_suffix(const char *path, const char *suffix) {
    size_t path_length;
    size_t suffix_length;

    if (path == 0 || suffix == 0) {
        return 0;
    }
    path_length = rt_strlen(path);
    suffix_length = rt_strlen(suffix);
    if (path_length < suffix_length) {
        return 0;
    }
    return rt_strcmp(path + path_length - suffix_length, suffix) == 0;
}

int editor_highlight_enabled(const char *path) {
    return editor_highlight_path_has_suffix(path, ".c") ||
           editor_highlight_path_has_suffix(path, ".h") ||
           editor_highlight_path_has_suffix(path, ".S");
}

static int editor_highlight_is_keyword(const char *text, size_t length) {
    static const char *keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if", "int",
        "long", "register", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while"
    };
    size_t index;

    for (index = 0U; index < sizeof(keywords) / sizeof(keywords[0]); ++index) {
        if (rt_strlen(keywords[index]) == length && rt_strncmp(text, keywords[index], length) == 0) {
            return 1;
        }
    }
    return 0;
}

int editor_highlight_style_at(const char *path, const char *line, size_t line_length, size_t offset) {
    size_t index = 0U;

    if (!editor_highlight_enabled(path) || line == 0) {
        return TUI_STYLE_NORMAL;
    }
    while (index < line_length) {
        char ch = line[index];

        if (ch == '/' && index + 1U < line_length && line[index + 1U] == '/') {
            return offset >= index ? TUI_STYLE_COMMENT : TUI_STYLE_NORMAL;
        }
        if (ch == '"' || ch == '\'') {
            char quote = ch;
            size_t end = index + 1U;
            while (end < line_length) {
                if (line[end] == '\\' && end + 1U < line_length) {
                    end += 2U;
                } else if (line[end] == quote) {
                    end += 1U;
                    break;
                } else {
                    end += 1U;
                }
            }
            if (offset >= index && offset < end) {
                return TUI_STYLE_STRING;
            }
            index = end;
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            size_t end = index + 1U;
            while (end < line_length && ((line[end] >= '0' && line[end] <= '9') || line[end] == '.' || tool_ascii_is_identifier_char(line[end]))) {
                end += 1U;
            }
            if (offset >= index && offset < end) {
                return TUI_STYLE_NUMBER;
            }
            index = end;
            continue;
        }
        if (tool_ascii_is_identifier_start(ch)) {
            size_t end = index + 1U;
            while (end < line_length && tool_ascii_is_identifier_char(line[end])) {
                end += 1U;
            }
            if (offset >= index && offset < end && editor_highlight_is_keyword(line + index, end - index)) {
                return TUI_STYLE_KEYWORD;
            }
            index = end;
            continue;
        }
        index += 1U;
    }
    return TUI_STYLE_NORMAL;
}
