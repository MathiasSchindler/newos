#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PATCH_TEXT_CAPACITY 262144
#define PATCH_MAX_LINES 4096
#define PATCH_LINE_CAPACITY 512
#define PATCH_MAX_HUNK_LINES 2048
#define PATCH_PATH_CAPACITY 512

static char patch_text[PATCH_TEXT_CAPACITY];
static char *patch_lines[PATCH_MAX_LINES];
static char patch_file_lines[PATCH_MAX_LINES][PATCH_LINE_CAPACITY];
static char patch_result_lines[PATCH_MAX_LINES][PATCH_LINE_CAPACITY];
static char patch_hunk_lines[PATCH_MAX_HUNK_LINES][PATCH_LINE_CAPACITY];
static char patch_hunk_kinds[PATCH_MAX_HUNK_LINES];

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static int load_patch_text(const char *path) {
    int fd;
    int should_close = 0;
    size_t used = 0;
    long bytes_read;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    while ((bytes_read = platform_read(fd, patch_text + used, sizeof(patch_text) - used - 1U)) > 0) {
        used += (size_t)bytes_read;
        if (used + 1U >= sizeof(patch_text)) {
            tool_close_input(fd, should_close);
            return -1;
        }
    }

    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        return -1;
    }

    patch_text[used] = '\0';
    return 0;
}

static int split_patch_lines(size_t *count_out) {
    size_t count = 0;
    size_t i;

    patch_lines[count++] = patch_text;
    for (i = 0; patch_text[i] != '\0'; ++i) {
        if (patch_text[i] == '\n') {
            patch_text[i] = '\0';
            if (count >= PATCH_MAX_LINES) {
                return -1;
            }
            patch_lines[count++] = patch_text + i + 1U;
        }
    }

    *count_out = count;
    return 0;
}

static void copy_line(char *dst, size_t dst_size, const char *src) {
    rt_copy_string(dst, dst_size, src);
}

static int build_backup_path(const char *path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(path);

    if (len + 6U > buffer_size) {
        return -1;
    }

    memcpy(buffer, path, len);
    memcpy(buffer + len, ".orig", 6);
    return 0;
}

static int parse_path_header(const char *text, int strip_components, char *out, size_t out_size) {
    char raw[PATCH_PATH_CAPACITY];
    size_t raw_len = 0;
    size_t pos = 0;
    int extra_strip = 0;

    while (text[pos] == ' ' || text[pos] == '\t') {
        pos += 1U;
    }

    while (text[pos] != '\0' && text[pos] != '\t' && text[pos] != ' ' && raw_len + 1U < sizeof(raw)) {
        raw[raw_len++] = text[pos++];
    }
    raw[raw_len] = '\0';

    if (rt_strcmp(raw, "/dev/null") == 0) {
        rt_copy_string(out, out_size, raw);
        return 0;
    }

    if (strip_components == 0 && ((raw[0] == 'a' || raw[0] == 'b') && raw[1] == '/')) {
        extra_strip = 1;
    }

    pos = 0;
    while (strip_components > 0 && raw[pos] != '\0') {
        if (raw[pos] == '/') {
            strip_components -= 1;
        }
        pos += 1U;
    }
    while (extra_strip > 0 && raw[pos] != '\0') {
        if (raw[pos] == '/') {
            extra_strip -= 1;
        }
        pos += 1U;
    }

    if (raw[pos] == '\0') {
        rt_copy_string(out, out_size, raw);
    } else {
        rt_copy_string(out, out_size, raw + pos);
    }
    return 0;
}

static int load_target_file(const char *path, size_t *count_out) {
    int fd;
    char chunk[512];
    char line[PATCH_LINE_CAPACITY];
    size_t count = 0;
    size_t line_len = 0;
    long bytes_read;

    *count_out = 0;

    if (rt_strcmp(path, "/dev/null") == 0) {
        return 0;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;
        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];
            if (ch == '\n') {
                line[line_len] = '\0';
                if (count >= PATCH_MAX_LINES) {
                    platform_close(fd);
                    return -1;
                }
                copy_line(patch_file_lines[count++], sizeof(patch_file_lines[0]), line);
                line_len = 0;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    platform_close(fd);
    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (count >= PATCH_MAX_LINES) {
            return -1;
        }
        copy_line(patch_file_lines[count++], sizeof(patch_file_lines[0]), line);
    }

    *count_out = count;
    return 0;
}

static int write_target_file(const char *path, size_t count) {
    int fd;
    size_t i;

    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        size_t len = rt_strlen(patch_file_lines[i]);
        if (rt_write_all(fd, patch_file_lines[i], len) != 0 || rt_write_char(fd, '\n') != 0) {
            platform_close(fd);
            return -1;
        }
    }

    platform_close(fd);
    return 0;
}

static int parse_range_value(const char *text, size_t *pos, unsigned long long *start_out, unsigned long long *count_out) {
    unsigned long long start = 0;
    unsigned long long count = 1;
    int saw_digit = 0;

    while (text[*pos] >= '0' && text[*pos] <= '9') {
        start = (start * 10ULL) + (unsigned long long)(text[*pos] - '0');
        *pos += 1U;
        saw_digit = 1;
    }

    if (!saw_digit) {
        return -1;
    }

    if (text[*pos] == ',') {
        int count_saw_digit = 0;
        *pos += 1U;
        count = 0;
        while (text[*pos] >= '0' && text[*pos] <= '9') {
            count = (count * 10ULL) + (unsigned long long)(text[*pos] - '0');
            *pos += 1U;
            count_saw_digit = 1;
        }
        if (!count_saw_digit) {
            return -1;
        }
    }

    *start_out = start;
    *count_out = count;
    return 0;
}

static int parse_hunk_header(
    const char *line,
    unsigned long long *old_start,
    unsigned long long *old_count,
    unsigned long long *new_start_out,
    unsigned long long *new_count
) {
    size_t pos = 0;
    unsigned long long new_start;
    unsigned long long parsed_new_count;

    if (!starts_with(line, "@@ -")) {
        return -1;
    }

    pos = 4;
    if (parse_range_value(line, &pos, old_start, old_count) != 0) {
        return -1;
    }

    while (line[pos] != '\0' && line[pos] != '+') {
        pos += 1U;
    }
    if (line[pos] != '+') {
        return -1;
    }
    pos += 1U;

    if (parse_range_value(line, &pos, &new_start, &parsed_new_count) != 0) {
        return -1;
    }

    *new_start_out = new_start;
    *new_count = parsed_new_count;
    return 0;
}

static int hunk_matches_at(size_t position, size_t file_count, size_t hunk_count) {
    size_t file_pos = position;
    size_t i;

    for (i = 0; i < hunk_count; ++i) {
        if (patch_hunk_kinds[i] == '+' ) {
            continue;
        }
        if (file_pos >= file_count) {
            return 0;
        }
        if (rt_strcmp(patch_file_lines[file_pos], patch_hunk_lines[i]) != 0) {
            return 0;
        }
        file_pos += 1U;
    }

    return 1;
}

static size_t find_hunk_position(unsigned long long old_start, size_t file_count, size_t hunk_count) {
    size_t expected = (old_start > 0) ? (size_t)(old_start - 1ULL) : 0U;
    size_t offset;

    if (expected <= file_count && hunk_matches_at(expected, file_count, hunk_count)) {
        return expected;
    }

    for (offset = 0; offset <= file_count; ++offset) {
        if (expected >= offset && hunk_matches_at(expected - offset, file_count, hunk_count)) {
            return expected - offset;
        }
        if (expected + offset <= file_count && hunk_matches_at(expected + offset, file_count, hunk_count)) {
            return expected + offset;
        }
    }

    return (size_t)-1;
}

static int apply_hunk(unsigned long long old_start, size_t hunk_count, size_t *file_count) {
    size_t position = find_hunk_position(old_start, *file_count, hunk_count);
    size_t input_pos;
    size_t output_count = 0;
    size_t hunk_index = 0;
    size_t old_index = 0;

    while (hunk_index < hunk_count && patch_hunk_kinds[hunk_index] != '\0') {
        hunk_index += 1U;
    }

    if (position == (size_t)-1) {
        return -1;
    }

    for (input_pos = 0; input_pos < position; ++input_pos) {
        if (output_count >= PATCH_MAX_LINES) {
            return -1;
        }
        copy_line(patch_result_lines[output_count++], sizeof(patch_result_lines[0]), patch_file_lines[input_pos]);
    }

    old_index = position;
    for (input_pos = 0; input_pos < hunk_index; ++input_pos) {
        if (patch_hunk_kinds[input_pos] == ' ') {
            if (old_index >= *file_count || output_count >= PATCH_MAX_LINES) {
                return -1;
            }
            copy_line(patch_result_lines[output_count++], sizeof(patch_result_lines[0]), patch_file_lines[old_index++]);
        } else if (patch_hunk_kinds[input_pos] == '-') {
            if (old_index >= *file_count) {
                return -1;
            }
            old_index += 1U;
        } else if (patch_hunk_kinds[input_pos] == '+') {
            if (output_count >= PATCH_MAX_LINES) {
                return -1;
            }
            copy_line(patch_result_lines[output_count++], sizeof(patch_result_lines[0]), patch_hunk_lines[input_pos]);
        }
    }

    while (old_index < *file_count) {
        if (output_count >= PATCH_MAX_LINES) {
            return -1;
        }
        copy_line(patch_result_lines[output_count++], sizeof(patch_result_lines[0]), patch_file_lines[old_index++]);
    }

    for (input_pos = 0; input_pos < output_count; ++input_pos) {
        copy_line(patch_file_lines[input_pos], sizeof(patch_file_lines[0]), patch_result_lines[input_pos]);
    }
    *file_count = output_count;
    return 0;
}

int main(int argc, char **argv) {
    const char *patch_path = "-";
    const char *output_path = 0;
    int strip_components = 0;
    int reverse_mode = 0;
    int backup_mode = 0;
    int output_used = 0;
    size_t line_count = 0;
    size_t i = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "--help") == 0) {
            tool_write_usage(tool_base_name(argv[0]), "[-pN] [-R] [-b] [-o outfile] [-i patchfile] [patchfile]");
            return 0;
        } else if (rt_strcmp(argv[argi], "-R") == 0) {
            reverse_mode = 1;
        } else if (rt_strcmp(argv[argi], "-b") == 0 || rt_strcmp(argv[argi], "--backup") == 0) {
            backup_mode = 1;
        } else if (rt_strcmp(argv[argi], "-i") == 0 && argi + 1 < argc) {
            patch_path = argv[++argi];
        } else if (rt_strcmp(argv[argi], "-o") == 0 && argi + 1 < argc) {
            output_path = argv[++argi];
        } else if (rt_strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) {
            unsigned long long value = 0;
            if (rt_parse_uint(argv[++argi], &value) != 0) {
                tool_write_error("patch", "invalid strip count", 0);
                return 1;
            }
            strip_components = (int)value;
        } else if (argv[argi][0] == '-' && argv[argi][1] == 'p') {
            unsigned long long value = 0;
            if (rt_parse_uint(argv[argi] + 2, &value) != 0) {
                tool_write_error("patch", "invalid strip count", 0);
                return 1;
            }
            strip_components = (int)value;
        } else {
            patch_path = argv[argi];
        }
    }

    if (load_patch_text(patch_path) != 0 || split_patch_lines(&line_count) != 0) {
        tool_write_error("patch", "cannot read patch input", 0);
        return 1;
    }

    while (i < line_count) {
        char old_path[PATCH_PATH_CAPACITY];
        char new_path[PATCH_PATH_CAPACITY];
        char target_path[PATCH_PATH_CAPACITY];
        size_t file_count = 0;
        int deleting_file = 0;
        int have_changes = 0;

        while (i < line_count && !starts_with(patch_lines[i], "--- ")) {
            i += 1U;
        }
        if (i >= line_count) {
            break;
        }

        if (parse_path_header(patch_lines[i] + 4, strip_components, old_path, sizeof(old_path)) != 0) {
            tool_write_error("patch", "invalid old file header", 0);
            return 1;
        }
        i += 1U;

        if (i >= line_count || !starts_with(patch_lines[i], "+++ ")) {
            tool_write_error("patch", "missing new file header", 0);
            return 1;
        }

        if (parse_path_header(patch_lines[i] + 4, strip_components, new_path, sizeof(new_path)) != 0) {
            tool_write_error("patch", "invalid new file header", 0);
            return 1;
        }
        i += 1U;

        deleting_file = (rt_strcmp(new_path, "/dev/null") == 0);
        if (reverse_mode) {
            deleting_file = (rt_strcmp(old_path, "/dev/null") == 0);
            if (deleting_file) {
                rt_copy_string(target_path, sizeof(target_path), new_path);
            } else {
                rt_copy_string(target_path, sizeof(target_path), old_path);
            }
        } else if (deleting_file) {
            rt_copy_string(target_path, sizeof(target_path), old_path);
        } else {
            rt_copy_string(target_path, sizeof(target_path), new_path);
        }

        if (output_path != 0 && output_used) {
            tool_write_error("patch", "multiple file patches are not supported with -o", 0);
            return 1;
        }

        if ((!reverse_mode && rt_strcmp(old_path, "/dev/null") == 0) ||
            (reverse_mode && rt_strcmp(new_path, "/dev/null") == 0)) {
            file_count = 0;
        } else if (load_target_file(target_path, &file_count) != 0) {
            tool_write_error("patch", "cannot open target ", target_path);
            return 1;
        }

        while (i < line_count && !starts_with(patch_lines[i], "--- ")) {
            if (starts_with(patch_lines[i], "@@ ")) {
                unsigned long long old_start = 0;
                unsigned long long old_count = 0;
                unsigned long long new_start = 0;
                unsigned long long new_count = 0;
                size_t hunk_count = 0;
                unsigned long long old_seen = 0;
                unsigned long long new_seen = 0;
                unsigned long long apply_start;

                if (parse_hunk_header(patch_lines[i], &old_start, &old_count, &new_start, &new_count) != 0) {
                    tool_write_error("patch", "invalid hunk header", 0);
                    return 1;
                }
                i += 1U;

                rt_memset(patch_hunk_kinds, 0, sizeof(patch_hunk_kinds));

                while (i < line_count) {
                    char prefix = patch_lines[i][0];
                    if (prefix != ' ' && prefix != '+' && prefix != '-' && prefix != '\\') {
                        break;
                    }
                    if (prefix == '\\') {
                        i += 1U;
                        continue;
                    }
                    if (hunk_count >= PATCH_MAX_HUNK_LINES) {
                        tool_write_error("patch", "hunk too large", 0);
                        return 1;
                    }
                    if (reverse_mode) {
                        if (prefix == '+') {
                            prefix = '-';
                        } else if (prefix == '-') {
                            prefix = '+';
                        }
                    }
                    patch_hunk_kinds[hunk_count] = prefix;
                    copy_line(patch_hunk_lines[hunk_count], sizeof(patch_hunk_lines[0]), patch_lines[i] + 1);
                    hunk_count += 1U;
                    if (prefix != '+') {
                        old_seen += 1ULL;
                    }
                    if (prefix != '-') {
                        new_seen += 1ULL;
                    }
                    i += 1U;

                    if (old_seen >= old_count && new_seen >= new_count) {
                        break;
                    }
                }

                if (hunk_count == 0 && old_count == 0) {
                    continue;
                }

                apply_start = reverse_mode ? new_start : old_start;
                if (apply_hunk(apply_start, hunk_count, &file_count) != 0) {
                    tool_write_error("patch", "failed to apply hunk to ", target_path);
                    return 1;
                }
                have_changes = 1;
            } else if (starts_with(patch_lines[i], "diff --git ")) {
                break;
            } else {
                i += 1U;
            }
        }

        if (have_changes || deleting_file || output_path != 0) {
            if (output_path != 0) {
                output_used = 1;
                if (write_target_file(output_path, deleting_file ? 0U : file_count) != 0) {
                    tool_write_error("patch", "cannot write ", output_path);
                    return 1;
                }
                rt_write_cstr(1, "patched ");
                rt_write_line(1, output_path);
            } else {
                if (backup_mode && tool_path_exists(target_path)) {
                    char backup_path[PATCH_PATH_CAPACITY];
                    if (build_backup_path(target_path, backup_path, sizeof(backup_path)) != 0 ||
                        tool_copy_file(target_path, backup_path) != 0) {
                        tool_write_error("patch", "cannot write backup for ", target_path);
                        return 1;
                    }
                }
                if (deleting_file) {
                    if (platform_remove_file(target_path) != 0) {
                        tool_write_error("patch", "cannot remove ", target_path);
                        return 1;
                    }
                } else if (write_target_file(target_path, file_count) != 0) {
                    tool_write_error("patch", "cannot write ", target_path);
                    return 1;
                }

                rt_write_cstr(1, "patched ");
                rt_write_line(1, target_path);
            }
        }
    }

    return 0;
}
