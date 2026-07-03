#include "editor/highlight.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "tui.h"

#include <stddef.h>

#define EDITOR_TAB_WIDTH 4U
#define EDITOR_STATUS_CAPACITY 160U
#define EDITOR_PATH_CAPACITY 512U
#define EDITOR_SEARCH_CAPACITY 128U

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} EditorLine;

typedef struct {
    EditorLine *lines;
    size_t line_count;
    size_t cursor_line;
    size_t cursor_byte;
    int dirty;
    int trailing_newline;
    int valid;
} EditorSnapshot;

typedef struct {
    EditorLine *lines;
    size_t line_count;
    size_t line_capacity;
    size_t cursor_line;
    size_t cursor_byte;
    size_t row_offset;
    size_t column_offset;
    int dirty;
    int trailing_newline;
    int show_line_numbers;
    char *path;
    int path_owned;
    char *clipboard;
    size_t clipboard_length;
    EditorSnapshot undo;
    char search[EDITOR_SEARCH_CAPACITY];
    char status[EDITOR_STATUS_CAPACITY];
    TuiTerminal terminal;
} EditorState;

static int editor_line_insert(EditorLine *line, size_t offset, const char *text, size_t length);
static size_t editor_next_char(const EditorLine *line, size_t offset);
static int editor_line_contains_at(const EditorLine *line, size_t offset, const char *needle, size_t needle_length);
static unsigned int editor_gutter_width(const EditorState *editor);

static void editor_set_status(EditorState *editor, const char *message) {
    rt_copy_string(editor->status, sizeof(editor->status), message);
}

static int editor_set_path(EditorState *editor, const char *path) {
    char *copy;
    size_t length;

    if (path == 0 || path[0] == '\0') {
        return -1;
    }
    length = rt_strlen(path);
    copy = (char *)rt_malloc(length + 1U);
    if (copy == 0) {
        return -1;
    }
    memcpy(copy, path, length + 1U);
    if (editor->path_owned) {
        rt_free(editor->path);
    }
    editor->path = copy;
    editor->path_owned = 1;
    return 0;
}

static void editor_free_lines(EditorLine *lines, size_t line_count) {
    size_t index;

    for (index = 0U; index < line_count; ++index) {
        rt_free(lines[index].data);
    }
    rt_free(lines);
}

static int editor_clone_lines(const EditorLine *source, size_t line_count, EditorLine **lines_out) {
    EditorLine *lines;
    size_t index;

    if (line_count == 0U) {
        *lines_out = 0;
        return 0;
    }
    lines = (EditorLine *)rt_malloc_array(line_count, sizeof(EditorLine));
    if (lines == 0) {
        return -1;
    }
    memset(lines, 0, line_count * sizeof(EditorLine));
    for (index = 0U; index < line_count; ++index) {
        if (source[index].length > 0U && editor_line_insert(&lines[index], 0U, source[index].data, source[index].length) != 0) {
            editor_free_lines(lines, line_count);
            return -1;
        }
    }
    *lines_out = lines;
    return 0;
}

static void editor_clear_snapshot(EditorSnapshot *snapshot) {
    if (snapshot->valid) {
        editor_free_lines(snapshot->lines, snapshot->line_count);
    }
    memset(snapshot, 0, sizeof(*snapshot));
}

static int editor_capture_undo(EditorState *editor) {
    EditorLine *lines = 0;

    if (editor_clone_lines(editor->lines, editor->line_count, &lines) != 0) {
        editor_set_status(editor, "Could not record undo");
        return -1;
    }
    editor_clear_snapshot(&editor->undo);
    editor->undo.lines = lines;
    editor->undo.line_count = editor->line_count;
    editor->undo.cursor_line = editor->cursor_line;
    editor->undo.cursor_byte = editor->cursor_byte;
    editor->undo.dirty = editor->dirty;
    editor->undo.trailing_newline = editor->trailing_newline;
    editor->undo.valid = 1;
    return 0;
}

static int editor_line_reserve(EditorLine *line, size_t capacity) {
    char *next;
    size_t next_capacity;

    if (capacity <= line->capacity) {
        return 0;
    }
    next_capacity = line->capacity == 0U ? 32U : line->capacity;
    while (next_capacity < capacity) {
        if (next_capacity > ((size_t)-1) / 2U) {
            return -1;
        }
        next_capacity *= 2U;
    }
    next = (char *)rt_realloc(line->data, next_capacity);
    if (next == 0) {
        return -1;
    }
    line->data = next;
    line->capacity = next_capacity;
    return 0;
}

static int editor_line_insert(EditorLine *line, size_t offset, const char *text, size_t length) {
    if (offset > line->length || text == 0) {
        return -1;
    }
    if (editor_line_reserve(line, line->length + length + 1U) != 0) {
        return -1;
    }
    memmove(line->data + offset + length, line->data + offset, line->length - offset);
    memcpy(line->data + offset, text, length);
    line->length += length;
    line->data[line->length] = '\0';
    return 0;
}

static void editor_line_delete_range(EditorLine *line, size_t offset, size_t length) {
    if (offset >= line->length) {
        return;
    }
    if (length > line->length - offset) {
        length = line->length - offset;
    }
    memmove(line->data + offset, line->data + offset + length, line->length - offset - length);
    line->length -= length;
    line->data[line->length] = '\0';
}

static int editor_reserve_lines(EditorState *editor, size_t capacity) {
    EditorLine *next;
    size_t next_capacity;

    if (capacity <= editor->line_capacity) {
        return 0;
    }
    next_capacity = editor->line_capacity == 0U ? 16U : editor->line_capacity;
    while (next_capacity < capacity) {
        if (next_capacity > ((size_t)-1) / 2U) {
            return -1;
        }
        next_capacity *= 2U;
    }
    next = (EditorLine *)rt_realloc_array(editor->lines, next_capacity, sizeof(EditorLine));
    if (next == 0) {
        return -1;
    }
    memset(next + editor->line_capacity, 0, (next_capacity - editor->line_capacity) * sizeof(EditorLine));
    editor->lines = next;
    editor->line_capacity = next_capacity;
    return 0;
}

static int editor_insert_empty_line(EditorState *editor, size_t index) {
    if (index > editor->line_count) {
        return -1;
    }
    if (editor_reserve_lines(editor, editor->line_count + 1U) != 0) {
        return -1;
    }
    memmove(editor->lines + index + 1U, editor->lines + index, (editor->line_count - index) * sizeof(EditorLine));
    memset(editor->lines + index, 0, sizeof(EditorLine));
    editor->line_count += 1U;
    return 0;
}

static void editor_free(EditorState *editor) {
    editor_free_lines(editor->lines, editor->line_count);
    if (editor->path_owned) {
        rt_free(editor->path);
    }
    rt_free(editor->clipboard);
    editor_clear_snapshot(&editor->undo);
}

static int editor_append_file_line(EditorState *editor, const char *text, size_t length) {
    EditorLine *line;

    if (editor_insert_empty_line(editor, editor->line_count) != 0) {
        return -1;
    }
    line = &editor->lines[editor->line_count - 1U];
    if (editor_line_insert(line, 0U, text, length) != 0) {
        return -1;
    }
    return 0;
}

static int editor_load_file(EditorState *editor, const char *path) {
    int fd;
    char buffer[4096];
    char *line_buffer = 0;
    size_t line_length = 0U;
    size_t line_capacity = 0U;
    int last_read_newline = 0;
    long count;

    if (path == 0) {
        return editor_insert_empty_line(editor, 0U);
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        editor_set_status(editor, "New file");
        return editor_insert_empty_line(editor, 0U);
    }

    while ((count = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        size_t index;
        for (index = 0U; index < (size_t)count; ++index) {
            char ch = buffer[index];
            if (ch == '\n') {
                last_read_newline = 1;
                if (editor_append_file_line(editor, line_buffer != 0 ? line_buffer : "", line_length) != 0) {
                    rt_free(line_buffer);
                    (void)platform_close(fd);
                    return -1;
                }
                line_length = 0U;
            } else if (ch != '\r') {
                last_read_newline = 0;
                if (line_length + 1U >= line_capacity) {
                    size_t next_capacity = line_capacity == 0U ? 128U : line_capacity * 2U;
                    char *next = (char *)rt_realloc(line_buffer, next_capacity);
                    if (next == 0) {
                        rt_free(line_buffer);
                        (void)platform_close(fd);
                        return -1;
                    }
                    line_buffer = next;
                    line_capacity = next_capacity;
                }
                line_buffer[line_length++] = ch;
            }
        }
    }
    (void)platform_close(fd);

    if (count < 0) {
        rt_free(line_buffer);
        return -1;
    }
    if (line_length > 0U || editor->line_count == 0U) {
        if (editor_append_file_line(editor, line_buffer != 0 ? line_buffer : "", line_length) != 0) {
            rt_free(line_buffer);
            return -1;
        }
    }
    rt_free(line_buffer);
    editor->trailing_newline = last_read_newline;
    editor_set_status(editor, "Opened file");
    return 0;
}

static int editor_save(EditorState *editor) {
    int fd;
    size_t index;

    if (editor->path == 0) {
        editor_set_status(editor, "No file name");
        return -1;
    }
    fd = platform_open_write(editor->path, 0644U);
    if (fd < 0) {
        editor_set_status(editor, "Could not save file");
        return -1;
    }
    for (index = 0U; index < editor->line_count; ++index) {
        EditorLine *line = &editor->lines[index];
        if (rt_write_all(fd, line->data != 0 ? line->data : "", line->length) != 0 ||
            (index + 1U < editor->line_count && rt_write_char(fd, '\n') != 0) ||
            (index + 1U == editor->line_count && editor->trailing_newline && rt_write_char(fd, '\n') != 0)) {
            (void)platform_close(fd);
            editor_set_status(editor, "Could not write file");
            return -1;
        }
    }
    (void)platform_close(fd);
    editor->dirty = 0;
    editor_set_status(editor, "Saved file");
    return 0;
}

static int editor_prompt_filename(EditorState *editor) {
    char buffer[EDITOR_PATH_CAPACITY];
    size_t length = 0U;

    buffer[0] = '\0';
    for (;;) {
        TuiKeyEvent event;
        const char *prompt = "File name: ";
        size_t prompt_length = rt_strlen(prompt);
        size_t visible_start = 0U;
        size_t available = editor->terminal.columns > prompt_length ? (size_t)editor->terminal.columns - prompt_length : 0U;

        if (available > 0U && length > available) {
            visible_start = length - available;
        }
        if (tui_move_cursor(&editor->terminal, editor->terminal.rows, 1U) != 0) return -1;
        if (tui_clear_line(&editor->terminal) != 0) return -1;
        if (tui_write_cstr(&editor->terminal, prompt) != 0) return -1;
        if (available > 0U && tui_write(&editor->terminal, buffer + visible_start, length - visible_start) != 0) return -1;
        if (tui_move_cursor(&editor->terminal, editor->terminal.rows, (unsigned int)(prompt_length + length - visible_start + 1U)) != 0) return -1;
        if (tui_show_cursor(&editor->terminal) != 0) return -1;

        if (tui_read_key(&editor->terminal, &event) != 0) {
            return -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ENTER) {
            if (length == 0U) {
                editor_set_status(editor, "Save canceled");
                return -1;
            }
            if (editor_set_path(editor, buffer) != 0) {
                editor_set_status(editor, "Could not store file name");
                return -1;
            }
            return 0;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_BACKSPACE) {
            if (length > 0U) {
                length -= 1U;
                while (length > 0U && ((unsigned char)buffer[length] & 0xc0U) == 0x80U) {
                    length -= 1U;
                }
                buffer[length] = '\0';
            }
        } else if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ESCAPE) {
            editor_set_status(editor, "Save canceled");
            return -1;
        } else if (event.type == TUI_KEY_CTRL && (event.codepoint == 'C' || event.codepoint == 'G')) {
            editor_set_status(editor, "Save canceled");
            return -1;
        } else if (event.type == TUI_KEY_CHARACTER) {
            if (event.codepoint == '\t') {
                continue;
            }
            if (length + event.text_length + 1U >= sizeof(buffer)) {
                editor_set_status(editor, "File name too long");
                continue;
            }
            memcpy(buffer + length, event.text, event.text_length);
            length += event.text_length;
            buffer[length] = '\0';
        }
    }
}

static int editor_save_with_prompt(EditorState *editor) {
    if (editor->path == 0 && editor_prompt_filename(editor) != 0) {
        return -1;
    }
    return editor_save(editor);
}

static int editor_prompt_text(EditorState *editor, const char *prompt, char *buffer, size_t buffer_size) {
    size_t length = rt_strlen(buffer);

    for (;;) {
        TuiKeyEvent event;
        size_t prompt_length = rt_strlen(prompt);
        size_t visible_start = 0U;
        size_t available = editor->terminal.columns > prompt_length ? (size_t)editor->terminal.columns - prompt_length : 0U;

        if (available > 0U && length > available) {
            visible_start = length - available;
        }
        if (tui_move_cursor(&editor->terminal, editor->terminal.rows, 1U) != 0) return -1;
        if (tui_clear_line(&editor->terminal) != 0) return -1;
        if (tui_write_cstr(&editor->terminal, prompt) != 0) return -1;
        if (available > 0U && tui_write(&editor->terminal, buffer + visible_start, length - visible_start) != 0) return -1;
        if (tui_move_cursor(&editor->terminal, editor->terminal.rows, (unsigned int)(prompt_length + length - visible_start + 1U)) != 0) return -1;
        if (tui_show_cursor(&editor->terminal) != 0) return -1;

        if (tui_read_key(&editor->terminal, &event) != 0) {
            return -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ENTER) {
            return length > 0U ? 0 : -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ESCAPE) {
            return -1;
        }
        if (event.type == TUI_KEY_CTRL && (event.codepoint == 'C' || event.codepoint == 'G')) {
            return -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_BACKSPACE) {
            if (length > 0U) {
                length -= 1U;
                while (length > 0U && ((unsigned char)buffer[length] & 0xc0U) == 0x80U) {
                    length -= 1U;
                }
                buffer[length] = '\0';
            }
        } else if (event.type == TUI_KEY_CHARACTER && event.codepoint != '\t') {
            if (length + event.text_length + 1U < buffer_size) {
                memcpy(buffer + length, event.text, event.text_length);
                length += event.text_length;
                buffer[length] = '\0';
            }
        }
    }
}

static int editor_find_next(EditorState *editor, const char *needle) {
    size_t needle_length = rt_strlen(needle);
    size_t pass;

    if (needle_length == 0U) {
        return -1;
    }
    for (pass = 0U; pass < 2U; ++pass) {
        size_t line_index = pass == 0U ? editor->cursor_line : 0U;
        while (line_index < editor->line_count && (pass == 0U || line_index <= editor->cursor_line)) {
            EditorLine *line = &editor->lines[line_index];
            size_t offset = (pass == 0U && line_index == editor->cursor_line) ? editor_next_char(line, editor->cursor_byte) : 0U;
            if (pass == 1U && line_index == editor->cursor_line) {
                break;
            }
            while (offset + needle_length <= line->length) {
                if (editor_line_contains_at(line, offset, needle, needle_length)) {
                    editor->cursor_line = line_index;
                    editor->cursor_byte = offset;
                    return 0;
                }
                offset = editor_next_char(line, offset);
            }
            line_index += 1U;
        }
    }
    return -1;
}

static int editor_search(EditorState *editor) {
    char query[EDITOR_SEARCH_CAPACITY];

    rt_copy_string(query, sizeof(query), editor->search);
    if (editor_prompt_text(editor, "Search: ", query, sizeof(query)) != 0) {
        editor_set_status(editor, "Search canceled");
        return -1;
    }
    rt_copy_string(editor->search, sizeof(editor->search), query);
    if (editor_find_next(editor, editor->search) != 0) {
        editor_set_status(editor, "Not found");
        return -1;
    }
    editor_set_status(editor, "Found");
    return 0;
}

static int editor_confirm_quit(EditorState *editor) {
    TuiKeyEvent event;
    const char *prompt = "Unsaved changes. Quit without saving? y/N";

    if (!editor->dirty) {
        return 1;
    }
    if (tui_move_cursor(&editor->terminal, editor->terminal.rows, 1U) != 0) return 0;
    if (tui_clear_line(&editor->terminal) != 0) return 0;
    if (tui_write_cstr(&editor->terminal, prompt) != 0) return 0;
    if (tui_read_key(&editor->terminal, &event) != 0) return 0;
    if (event.type == TUI_KEY_CHARACTER && (event.codepoint == 'y' || event.codepoint == 'Y')) {
        return 1;
    }
    editor_set_status(editor, "Quit canceled");
    return 0;
}

static int editor_undo(EditorState *editor) {
    EditorSnapshot snapshot;

    if (!editor->undo.valid) {
        editor_set_status(editor, "Nothing to undo");
        return -1;
    }
    snapshot = editor->undo;
    memset(&editor->undo, 0, sizeof(editor->undo));
    editor_free_lines(editor->lines, editor->line_count);
    editor->lines = snapshot.lines;
    editor->line_count = snapshot.line_count;
    editor->line_capacity = snapshot.line_count;
    editor->cursor_line = snapshot.cursor_line;
    editor->cursor_byte = snapshot.cursor_byte;
    editor->dirty = snapshot.dirty;
    editor->trailing_newline = snapshot.trailing_newline;
    editor_set_status(editor, "Undone");
    return 0;
}

static size_t editor_next_char(const EditorLine *line, size_t offset) {
    size_t next = offset;
    unsigned int codepoint;

    if (offset >= line->length) {
        return line->length;
    }
    if (rt_utf8_decode(line->data, line->length, &next, &codepoint) != 0 || next <= offset) {
        return offset + 1U;
    }
    return next;
}

static size_t editor_prev_char(const EditorLine *line, size_t offset) {
    size_t index = 0U;
    size_t previous = 0U;

    if (offset > line->length) {
        offset = line->length;
    }
    while (index < offset) {
        previous = index;
        index = editor_next_char(line, index);
    }
    return previous;
}

static unsigned long long editor_line_display_width_to_byte(const EditorLine *line, size_t offset) {
    size_t index = 0U;
    unsigned long long width = 0ULL;
    RtTextSegment segment;

    while (index < offset && rt_text_next_segment(line->data, line->length, index, &segment) == 0) {
        if (segment.end > offset) {
            break;
        }
        width = rt_text_apply_segment_width(width, &segment);
        index = segment.end;
    }
    return width;
}

static size_t editor_byte_for_display_width(const EditorLine *line, unsigned long long target) {
    size_t index = 0U;
    unsigned long long width = 0ULL;
    RtTextSegment segment;

    while (rt_text_next_segment(line->data, line->length, index, &segment) == 0) {
        unsigned long long next_width = rt_text_apply_segment_width(width, &segment);
        if (next_width > target) {
            break;
        }
        width = next_width;
        index = segment.end;
    }
    return index;
}

static void editor_clamp_cursor(EditorState *editor) {
    if (editor->line_count == 0U) {
        return;
    }
    if (editor->cursor_line >= editor->line_count) {
        editor->cursor_line = editor->line_count - 1U;
    }
    if (editor->cursor_byte > editor->lines[editor->cursor_line].length) {
        editor->cursor_byte = editor->lines[editor->cursor_line].length;
    }
}

static void editor_move_vertical(EditorState *editor, int direction) {
    unsigned long long target_column = editor_line_display_width_to_byte(&editor->lines[editor->cursor_line], editor->cursor_byte);

    if (direction < 0 && editor->cursor_line > 0U) {
        editor->cursor_line -= 1U;
    } else if (direction > 0 && editor->cursor_line + 1U < editor->line_count) {
        editor->cursor_line += 1U;
    }
    editor->cursor_byte = editor_byte_for_display_width(&editor->lines[editor->cursor_line], target_column);
}

static void editor_scroll_to_cursor(EditorState *editor) {
    unsigned int usable_rows = editor->terminal.rows > 2U ? editor->terminal.rows - 2U : 1U;
    unsigned int gutter_width = editor_gutter_width(editor);
    unsigned int usable_columns = editor->terminal.columns > gutter_width ? editor->terminal.columns - gutter_width : 1U;
    unsigned long long cursor_column;

    if (editor->cursor_line < editor->row_offset) {
        editor->row_offset = editor->cursor_line;
    }
    if (editor->cursor_line >= editor->row_offset + usable_rows) {
        editor->row_offset = editor->cursor_line - usable_rows + 1U;
    }

    cursor_column = editor_line_display_width_to_byte(&editor->lines[editor->cursor_line], editor->cursor_byte);
    if (cursor_column < (unsigned long long)editor->column_offset) {
        editor->column_offset = (size_t)cursor_column;
    }
    if (cursor_column >= (unsigned long long)(editor->column_offset + usable_columns)) {
        editor->column_offset = (size_t)(cursor_column - usable_columns + 1ULL);
    }
}

static unsigned int editor_decimal_width(size_t value) {
    unsigned int width = 1U;

    while (value >= 10U) {
        value /= 10U;
        width += 1U;
    }
    return width;
}

static unsigned int editor_gutter_width(const EditorState *editor) {
    if (!editor->show_line_numbers) {
        return 0U;
    }
    return editor_decimal_width(editor->line_count == 0U ? 1U : editor->line_count) + 2U;
}

static int editor_draw_gutter(EditorState *editor, size_t line_index, unsigned int gutter_width) {
    char number[32];
    size_t length;
    unsigned int padding;

    if (gutter_width == 0U) {
        return 0;
    }
    rt_unsigned_to_string((unsigned long long)(line_index + 1U), number, sizeof(number));
    length = rt_strlen(number);
    padding = gutter_width > length + 1U ? gutter_width - (unsigned int)length - 1U : 0U;
    if (tui_set_style(&editor->terminal, TUI_STYLE_COMMENT) != 0) return -1;
    while (padding > 0U) {
        if (tui_write_cstr(&editor->terminal, " ") != 0) return -1;
        padding -= 1U;
    }
    if (tui_write_cstr(&editor->terminal, number) != 0) return -1;
    if (tui_write_cstr(&editor->terminal, " ") != 0) return -1;
    return tui_set_style(&editor->terminal, TUI_STYLE_NORMAL);
}

static int editor_line_contains_at(const EditorLine *line, size_t offset, const char *needle, size_t needle_length) {
    if (needle_length == 0U || line->data == 0 || offset + needle_length > line->length) {
        return 0;
    }
    return rt_strncmp(line->data + offset, needle, needle_length) == 0;
}

static int editor_search_style_at(const EditorState *editor, const EditorLine *line, size_t offset) {
    size_t search_length = rt_strlen(editor->search);
    size_t start;

    if (search_length == 0U) {
        return 0;
    }
    start = offset >= search_length ? offset - search_length + 1U : 0U;
    while (start <= offset) {
        if (editor_line_contains_at(line, start, editor->search, search_length) && offset < start + search_length) {
            return 1;
        }
        start += 1U;
    }
    return 0;
}

static int editor_draw_line(EditorState *editor, const EditorLine *line, unsigned int screen_columns) {
    size_t index = 0U;
    unsigned long long width = 0ULL;
    unsigned int drawn = 0U;
    RtTextSegment segment;

    while (rt_text_next_segment(line->data, line->length, index, &segment) == 0) {
        unsigned long long next_width = rt_text_apply_segment_width(width, &segment);
        if (next_width <= (unsigned long long)editor->column_offset) {
            width = next_width;
            index = segment.end;
            continue;
        }
        if (drawn >= screen_columns) {
            break;
        }
        if (editor_search_style_at(editor, line, segment.start)) {
            if (tui_set_style(&editor->terminal, TUI_STYLE_MATCH) != 0) return -1;
        } else {
            if (tui_set_style(&editor->terminal, editor_highlight_style_at(editor->path, line->data, line->length, segment.start)) != 0) return -1;
        }
        if (segment.codepoint == '\t') {
            do {
                if (tui_write_cstr(&editor->terminal, " ") != 0) return -1;
                drawn += 1U;
            } while (drawn < screen_columns && (drawn % EDITOR_TAB_WIDTH) != 0U);
        } else if (segment.codepoint < 0x20U || segment.codepoint == 0x7fU || (segment.flags & RT_TEXT_SEGMENT_INVALID) != 0U) {
            if (drawn + 1U < screen_columns) {
                char caret[2];
                caret[0] = '^';
                caret[1] = segment.codepoint == 0x7fU ? '?' : (char)(segment.codepoint + 64U);
                if (tui_write(&editor->terminal, caret, sizeof(caret)) != 0) return -1;
                drawn += 2U;
            } else {
                break;
            }
        } else {
            unsigned int segment_width = segment.display_width == 0U ? 1U : segment.display_width;
            if (drawn + segment_width > screen_columns) {
                break;
            }
            if (tui_write(&editor->terminal, line->data + segment.start, segment.end - segment.start) != 0) return -1;
            drawn += segment_width;
        }
        width = next_width;
        index = segment.end;
    }
    if (tui_set_style(&editor->terminal, TUI_STYLE_NORMAL) != 0) return -1;
    return 0;
}

static int editor_draw_status(EditorState *editor) {
    char number[32];
    unsigned int written = 0U;
    const char *name = editor->path != 0 ? editor->path : "[No Name]";

    if (tui_set_inverse(&editor->terminal, 1) != 0) return -1;
    if (tui_write_cstr(&editor->terminal, " ") != 0) return -1;
    written += 1U;
    while (*name != '\0' && written + 1U < editor->terminal.columns) {
        if (tui_write(&editor->terminal, name, 1U) != 0) return -1;
        name += 1;
        written += 1U;
    }
    if (editor->dirty && written + 11U < editor->terminal.columns) {
        if (tui_write_cstr(&editor->terminal, " modified") != 0) return -1;
        written += 9U;
    }
    if (written + 8U < editor->terminal.columns) {
        if (tui_write_cstr(&editor->terminal, "  line ") != 0) return -1;
        written += 7U;
        rt_unsigned_to_string((unsigned long long)(editor->cursor_line + 1U), number, sizeof(number));
        if (tui_write_cstr(&editor->terminal, number) != 0) return -1;
        written += (unsigned int)rt_strlen(number);
    }
    while (written < editor->terminal.columns) {
        if (tui_write_cstr(&editor->terminal, " ") != 0) return -1;
        written += 1U;
    }
    return tui_set_inverse(&editor->terminal, 0);
}

static int editor_draw_message(EditorState *editor) {
    const char *help = "^S Save  ^Q Quit  ^F Find  ^L Lines  ^Z Undo  ^K Cut  ^U Paste";
    const char *text = editor->status[0] != '\0' ? editor->status : help;
    size_t prefix = 0U;

    while (text[prefix] != '\0' && prefix < (size_t)editor->terminal.columns) {
        prefix += 1U;
    }
    return tui_write(&editor->terminal, text, prefix);
}

static int editor_refresh(EditorState *editor) {
    unsigned int usable_rows;
    unsigned int row;
    unsigned int gutter_width;
    unsigned int text_columns;
    unsigned long long cursor_column;

    (void)tui_terminal_refresh_size(&editor->terminal);
    editor_clamp_cursor(editor);
    editor_scroll_to_cursor(editor);

    usable_rows = editor->terminal.rows > 2U ? editor->terminal.rows - 2U : 1U;
    gutter_width = editor_gutter_width(editor);
    text_columns = editor->terminal.columns > gutter_width ? editor->terminal.columns - gutter_width : 0U;
    if (tui_hide_cursor(&editor->terminal) != 0) return -1;
    if (tui_move_cursor(&editor->terminal, 1U, 1U) != 0) return -1;

    for (row = 0U; row < usable_rows; ++row) {
        size_t line_index = editor->row_offset + row;
        if (tui_clear_line(&editor->terminal) != 0) return -1;
        if (line_index < editor->line_count) {
            if (editor_draw_gutter(editor, line_index, gutter_width) != 0) return -1;
            if (text_columns > 0U && editor_draw_line(editor, &editor->lines[line_index], text_columns) != 0) return -1;
        } else if (line_index == editor->line_count && editor->line_count == 1U && editor->lines[0].length == 0U) {
            if (tui_write_cstr(&editor->terminal, "") != 0) return -1;
        }
        if (row + 1U < usable_rows) {
            if (tui_write_cstr(&editor->terminal, "\r\n") != 0) return -1;
        }
    }

    if (tui_move_cursor(&editor->terminal, usable_rows + 1U, 1U) != 0) return -1;
    if (editor_draw_status(editor) != 0) return -1;
    if (tui_move_cursor(&editor->terminal, usable_rows + 2U, 1U) != 0) return -1;
    if (tui_clear_line(&editor->terminal) != 0) return -1;
    if (editor_draw_message(editor) != 0) return -1;

    cursor_column = editor_line_display_width_to_byte(&editor->lines[editor->cursor_line], editor->cursor_byte);
    if (tui_move_cursor(&editor->terminal,
                        (unsigned int)(editor->cursor_line - editor->row_offset + 1U),
                        (unsigned int)(gutter_width + cursor_column - editor->column_offset + 1ULL)) != 0) return -1;
    if (tui_show_cursor(&editor->terminal) != 0) return -1;
    return 0;
}

static int editor_insert_text(EditorState *editor, const char *text, size_t length) {
    EditorLine *line = &editor->lines[editor->cursor_line];

    if (editor_capture_undo(editor) != 0) {
        return -1;
    }
    if (editor_line_insert(line, editor->cursor_byte, text, length) != 0) {
        editor_set_status(editor, "Out of memory");
        return -1;
    }
    editor->cursor_byte += length;
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor->status[0] = '\0';
    return 0;
}

static int editor_insert_newline(EditorState *editor) {
    EditorLine *line = &editor->lines[editor->cursor_line];
    EditorLine *next;
    size_t tail_length = line->length - editor->cursor_byte;

    if (editor_capture_undo(editor) != 0) {
        return -1;
    }
    if (editor_insert_empty_line(editor, editor->cursor_line + 1U) != 0) {
        editor_set_status(editor, "Out of memory");
        return -1;
    }
    line = &editor->lines[editor->cursor_line];
    next = &editor->lines[editor->cursor_line + 1U];
    if (tail_length > 0U && editor_line_insert(next, 0U, line->data + editor->cursor_byte, tail_length) != 0) {
        editor_set_status(editor, "Out of memory");
        return -1;
    }
    editor_line_delete_range(line, editor->cursor_byte, tail_length);
    editor->cursor_line += 1U;
    editor->cursor_byte = 0U;
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor->status[0] = '\0';
    return 0;
}

static void editor_delete_line(EditorState *editor, size_t index) {
    if (index >= editor->line_count) {
        return;
    }
    rt_free(editor->lines[index].data);
    memmove(editor->lines + index, editor->lines + index + 1U, (editor->line_count - index - 1U) * sizeof(EditorLine));
    editor->line_count -= 1U;
    memset(editor->lines + editor->line_count, 0, sizeof(EditorLine));
}

static int editor_cut_line(EditorState *editor) {
    EditorLine *line;
    char *copy;

    if (editor->line_count == 0U) {
        return -1;
    }
    line = &editor->lines[editor->cursor_line];
    copy = (char *)rt_malloc(line->length + 1U);
    if (copy == 0) {
        editor_set_status(editor, "Out of memory");
        return -1;
    }
    if (line->length > 0U) {
        memcpy(copy, line->data, line->length);
    }
    copy[line->length] = '\0';
    if (editor_capture_undo(editor) != 0) {
        rt_free(copy);
        return -1;
    }
    rt_free(editor->clipboard);
    editor->clipboard = copy;
    editor->clipboard_length = line->length;
    editor_delete_line(editor, editor->cursor_line);
    if (editor->line_count == 0U) {
        if (editor_insert_empty_line(editor, 0U) != 0) {
            editor_set_status(editor, "Out of memory");
            return -1;
        }
    }
    if (editor->cursor_line >= editor->line_count) {
        editor->cursor_line = editor->line_count - 1U;
    }
    editor->cursor_byte = 0U;
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor_set_status(editor, "Cut line");
    return 0;
}

static int editor_paste_clipboard(EditorState *editor) {
    EditorLine *line;

    if (editor->clipboard == 0) {
        editor_set_status(editor, "Clipboard empty");
        return -1;
    }
    if (editor_capture_undo(editor) != 0) {
        return -1;
    }
    line = &editor->lines[editor->cursor_line];
    if (editor_line_insert(line, editor->cursor_byte, editor->clipboard, editor->clipboard_length) != 0) {
        editor_set_status(editor, "Out of memory");
        return -1;
    }
    editor->cursor_byte += editor->clipboard_length;
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor_set_status(editor, "Pasted");
    return 0;
}

static int editor_backspace(EditorState *editor) {
    EditorLine *line = &editor->lines[editor->cursor_line];

    if (editor->cursor_byte == 0U && editor->cursor_line == 0U) {
        return 0;
    }
    if (editor_capture_undo(editor) != 0) {
        return -1;
    }
    if (editor->cursor_byte > 0U) {
        size_t previous = editor_prev_char(line, editor->cursor_byte);
        editor_line_delete_range(line, previous, editor->cursor_byte - previous);
        editor->cursor_byte = previous;
    } else if (editor->cursor_line > 0U) {
        EditorLine *previous_line = &editor->lines[editor->cursor_line - 1U];
        size_t join_offset = previous_line->length;
        if (editor_line_insert(previous_line, previous_line->length, line->data != 0 ? line->data : "", line->length) != 0) {
            editor_set_status(editor, "Out of memory");
            return -1;
        }
        editor_delete_line(editor, editor->cursor_line);
        editor->cursor_line -= 1U;
        editor->cursor_byte = join_offset;
    }
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor->status[0] = '\0';
    return 0;
}

static int editor_delete_forward(EditorState *editor) {
    EditorLine *line = &editor->lines[editor->cursor_line];

    if (editor->cursor_byte >= line->length && editor->cursor_line + 1U >= editor->line_count) {
        return 0;
    }
    if (editor_capture_undo(editor) != 0) {
        return -1;
    }
    if (editor->cursor_byte < line->length) {
        size_t next = editor_next_char(line, editor->cursor_byte);
        editor_line_delete_range(line, editor->cursor_byte, next - editor->cursor_byte);
    } else if (editor->cursor_line + 1U < editor->line_count) {
        EditorLine *next_line = &editor->lines[editor->cursor_line + 1U];
        if (editor_line_insert(line, line->length, next_line->data != 0 ? next_line->data : "", next_line->length) != 0) {
            editor_set_status(editor, "Out of memory");
            return -1;
        }
        editor_delete_line(editor, editor->cursor_line + 1U);
    }
    editor->dirty = 1;
    editor->trailing_newline = 0;
    editor->status[0] = '\0';
    return 0;
}

static void editor_move_left(EditorState *editor) {
    EditorLine *line = &editor->lines[editor->cursor_line];
    if (editor->cursor_byte > 0U) {
        editor->cursor_byte = editor_prev_char(line, editor->cursor_byte);
    } else if (editor->cursor_line > 0U) {
        editor->cursor_line -= 1U;
        editor->cursor_byte = editor->lines[editor->cursor_line].length;
    }
}

static void editor_move_right(EditorState *editor) {
    EditorLine *line = &editor->lines[editor->cursor_line];
    if (editor->cursor_byte < line->length) {
        editor->cursor_byte = editor_next_char(line, editor->cursor_byte);
    } else if (editor->cursor_line + 1U < editor->line_count) {
        editor->cursor_line += 1U;
        editor->cursor_byte = 0U;
    }
}

static void editor_move_left_count(EditorState *editor, unsigned int count) {
    while (count > 0U) {
        editor_move_left(editor);
        count -= 1U;
    }
}

static void editor_move_right_count(EditorState *editor, unsigned int count) {
    while (count > 0U) {
        editor_move_right(editor);
        count -= 1U;
    }
}

static void editor_move_vertical_count(EditorState *editor, int direction, unsigned int count) {
    while (count > 0U) {
        editor_move_vertical(editor, direction);
        count -= 1U;
    }
}

static void editor_move_word_left(EditorState *editor) {
    for (;;) {
        EditorLine *line = &editor->lines[editor->cursor_line];
        size_t previous;

        if (editor->cursor_byte == 0U) {
            if (editor->cursor_line == 0U) {
                return;
            }
            editor->cursor_line -= 1U;
            editor->cursor_byte = editor->lines[editor->cursor_line].length;
            continue;
        }
        previous = editor_prev_char(line, editor->cursor_byte);
        if (previous < line->length && tool_ascii_is_word_byte((unsigned char)line->data[previous])) {
            break;
        }
        editor->cursor_byte = previous;
    }
    for (;;) {
        EditorLine *line = &editor->lines[editor->cursor_line];
        size_t previous;

        if (editor->cursor_byte == 0U) {
            return;
        }
        previous = editor_prev_char(line, editor->cursor_byte);
        if (previous >= line->length || !tool_ascii_is_word_byte((unsigned char)line->data[previous])) {
            return;
        }
        editor->cursor_byte = previous;
    }
}

static void editor_move_word_right(EditorState *editor) {
    for (;;) {
        EditorLine *line = &editor->lines[editor->cursor_line];

        while (editor->cursor_byte < line->length && tool_ascii_is_word_byte((unsigned char)line->data[editor->cursor_byte])) {
            editor->cursor_byte = editor_next_char(line, editor->cursor_byte);
        }
        while (editor->cursor_byte < line->length && !tool_ascii_is_word_byte((unsigned char)line->data[editor->cursor_byte])) {
            editor->cursor_byte = editor_next_char(line, editor->cursor_byte);
        }
        if (editor->cursor_byte < line->length || editor->cursor_line + 1U >= editor->line_count) {
            return;
        }
        editor->cursor_line += 1U;
        editor->cursor_byte = 0U;
    }
}

static void editor_move_word_count(EditorState *editor, int direction, unsigned int count) {
    while (count > 0U) {
        if (direction < 0) {
            editor_move_word_left(editor);
        } else {
            editor_move_word_right(editor);
        }
        count -= 1U;
    }
}

static int editor_handle_key(EditorState *editor, const TuiKeyEvent *event, int *quit_out) {
    *quit_out = 0;
    if (event->type == TUI_KEY_CTRL) {
        if (event->codepoint == 'S') {
            (void)editor_save_with_prompt(editor);
        } else if (event->codepoint == 'Q') {
            *quit_out = editor_confirm_quit(editor);
        } else if (event->codepoint == 'F') {
            (void)editor_search(editor);
        } else if (event->codepoint == 'Z') {
            (void)editor_undo(editor);
        } else if (event->codepoint == 'K') {
            (void)editor_cut_line(editor);
        } else if (event->codepoint == 'U') {
            (void)editor_paste_clipboard(editor);
        } else if (event->codepoint == 'L') {
            editor->show_line_numbers = !editor->show_line_numbers;
            editor_set_status(editor, editor->show_line_numbers ? "Line numbers on" : "Line numbers off");
        }
    } else if (event->type == TUI_KEY_CHARACTER) {
        (void)editor_insert_text(editor, event->text, event->text_length);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ENTER) {
        (void)editor_insert_newline(editor);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_BACKSPACE) {
        (void)editor_backspace(editor);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_DELETE) {
        (void)editor_delete_forward(editor);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_LEFT) {
        if ((event->modifiers & TUI_MOD_CTRL) != 0U) {
            editor_move_word_count(editor, -1, (event->modifiers & TUI_MOD_SHIFT) != 0U ? 5U : 1U);
        } else if ((event->modifiers & TUI_MOD_SHIFT) != 0U) {
            editor_move_left_count(editor, 8U);
        } else {
            editor_move_left(editor);
        }
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_RIGHT) {
        if ((event->modifiers & TUI_MOD_CTRL) != 0U) {
            editor_move_word_count(editor, 1, (event->modifiers & TUI_MOD_SHIFT) != 0U ? 5U : 1U);
        } else if ((event->modifiers & TUI_MOD_SHIFT) != 0U) {
            editor_move_right_count(editor, 8U);
        } else {
            editor_move_right(editor);
        }
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_UP) {
        editor_move_vertical_count(editor, -1, (event->modifiers & (TUI_MOD_SHIFT | TUI_MOD_CTRL)) != 0U ? 5U : 1U);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_DOWN) {
        editor_move_vertical_count(editor, 1, (event->modifiers & (TUI_MOD_SHIFT | TUI_MOD_CTRL)) != 0U ? 5U : 1U);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_HOME) {
        editor->cursor_byte = 0U;
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_END) {
        editor->cursor_byte = editor->lines[editor->cursor_line].length;
    } else if (event->type == TUI_KEY_SPECIAL && (event->codepoint == TUI_KEY_PAGE_UP || event->codepoint == TUI_KEY_PAGE_DOWN)) {
        unsigned int count = editor->terminal.rows > 2U ? editor->terminal.rows - 2U : 1U;
        while (count > 0U) {
            editor_move_vertical(editor, event->codepoint == TUI_KEY_PAGE_UP ? -1 : 1);
            count -= 1U;
        }
    }
    return 0;
}

static void editor_write_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_cstr(2, " [file]\n");
}

int main(int argc, char **argv) {
    EditorState editor;
    int quit = 0;

    memset(&editor, 0, sizeof(editor));
    if (argc > 2 || (argc == 2 && rt_strcmp(argv[1], "--help") == 0)) {
        editor_write_usage(argv[0]);
        return argc > 2 ? 1 : 0;
    }
    editor.path = argc == 2 ? argv[1] : 0;
    if (editor_load_file(&editor, editor.path) != 0) {
        rt_write_cstr(2, "editor: could not load file\n");
        editor_free(&editor);
        return 1;
    }
    if (tui_terminal_open(&editor.terminal, 0, 1, 1) != 0) {
        rt_write_cstr(2, "editor: standard input and output must be a terminal\n");
        editor_free(&editor);
        return 1;
    }
    (void)tui_clear_screen(&editor.terminal);

    while (!quit) {
        TuiKeyEvent event;
        if (editor_refresh(&editor) != 0 || tui_read_key(&editor.terminal, &event) != 0) {
            break;
        }
        (void)editor_handle_key(&editor, &event, &quit);
    }

    tui_terminal_close(&editor.terminal);
    editor_free(&editor);
    return 0;
}