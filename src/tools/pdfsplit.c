#include "pdf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PDFTOOL_INITIAL_CAPACITY 65536U
#define PDFTOOL_PATH_CAPACITY 512U

typedef struct {
    unsigned int first;
    unsigned int last;
} PdfPageRange;

static void print_usage(void) {
    tool_write_usage("pdfsplit", "--every N -o PREFIX PDF | --pages A-B [-o OUTPUT] PDF");
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = PDFTOOL_INITIAL_CAPACITY;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) return -1;
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            size_t next_capacity = capacity * 2U;
            unsigned char *next;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                return -1;
            }
            next = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (next == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                return -1;
            }
            buffer = next;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            return -1;
        }
        if (bytes_read == 0) break;
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static int write_all_output(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    size_t written = 0U;

    if (fd < 0) return -1;
    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);

        if (chunk <= 0) {
            platform_close(fd);
            return -1;
        }
        written += (size_t)chunk;
    }
    return platform_close(fd) == 0 ? 0 : -1;
}

static unsigned int parse_uint_text(const char *text, size_t *offset_io) {
    unsigned int value = 0U;
    size_t offset = *offset_io;

    while (text[offset] >= '0' && text[offset] <= '9') {
        unsigned int digit = (unsigned int)(text[offset] - '0');
        if (value > (4294967295U - digit) / 10U) return 0U;
        value = value * 10U + digit;
        offset += 1U;
    }
    *offset_io = offset;
    return value;
}

static int parse_range(const char *text, PdfPageRange *range) {
    size_t offset = 0U;
    unsigned int first = parse_uint_text(text, &offset);
    unsigned int last;

    if (first == 0U) return -1;
    if (text[offset] == '\0') {
        last = first;
    } else if (text[offset] == '-') {
        offset += 1U;
        last = parse_uint_text(text, &offset);
        if (last == 0U || text[offset] != '\0') return -1;
    } else {
        return -1;
    }
    if (last < first) return -1;
    range->first = first;
    range->last = last;
    return 0;
}

static void append_three_digits(char *buffer, size_t buffer_size, unsigned int value) {
    size_t used = rt_strlen(buffer);

    if (used + 3U >= buffer_size) return;
    buffer[used++] = (char)('0' + ((value / 100U) % 10U));
    buffer[used++] = (char)('0' + ((value / 10U) % 10U));
    buffer[used++] = (char)('0' + (value % 10U));
    buffer[used] = '\0';
}

static void make_part_path(char *buffer, size_t buffer_size, const char *prefix, unsigned int part) {
    rt_copy_string(buffer, buffer_size, prefix);
    if (rt_strlen(buffer) + 8U < buffer_size) {
        size_t used = rt_strlen(buffer);
        buffer[used++] = '-';
        buffer[used] = '\0';
        append_three_digits(buffer, buffer_size, part);
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), ".pdf");
    }
}

static int write_part(const PdfDocument *document, size_t first_page_zero, size_t page_count, const char *path) {
    PdfBuffer output;
    int result;

    pdf_buffer_init(&output);
    result = pdf_write_split_part(document, first_page_zero, page_count, &document->info.document_info, &output);
    if (result == 0) result = write_all_output(path, output.data, output.size);
    pdf_buffer_free(&output);
    return result;
}

int main(int argc, char **argv) {
    const char *output = 0;
    const char *input_path = 0;
    unsigned int every = 0U;
    PdfPageRange range;
    int have_range = 0;
    int index;
    unsigned char *data = 0;
    size_t size = 0U;
    PdfDocument document;
    int status = 0;

    range.first = 0U;
    range.last = 0U;
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) {
            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            output = argv[++index];
        } else if (rt_strcmp(arg, "--every") == 0) {
            size_t offset = 0U;
            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            every = parse_uint_text(argv[++index], &offset);
            if (every == 0U || argv[index][offset] != '\0') {
                tool_write_error("pdfsplit", "invalid --every value: ", argv[index]);
                return 2;
            }
        } else if (rt_strcmp(arg, "--pages") == 0) {
            if (index + 1 >= argc || parse_range(argv[++index], &range) != 0) {
                tool_write_error("pdfsplit", "invalid --pages range", 0);
                return 2;
            }
            have_range = 1;
        } else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfsplit", "unknown option: ", arg);
            return 2;
        } else {
            input_path = arg;
        }
    }
    if (input_path == 0 || ((every != 0U) == have_range)) {
        print_usage();
        return 2;
    }
    if (read_all_input(input_path, &data, &size) != 0) {
        tool_write_error("pdfsplit", "read failed: ", input_path);
        return 1;
    }
    if (pdf_document_parse(data, size, &document) != 0) {
        tool_write_error("pdfsplit", "unsupported or unreadable PDF: ", input_path);
        rt_free(data);
        return 1;
    }
    if (have_range) {
        const char *path = output != 0 ? output : "split.pdf";

        if ((size_t)range.first > document.pages_len || (size_t)range.last > document.pages_len) {
            tool_write_error("pdfsplit", "page range outside document", 0);
            status = 1;
        } else if (write_part(&document, (size_t)range.first - 1U, (size_t)(range.last - range.first + 1U), path) != 0) {
            tool_write_error("pdfsplit", "write failed: ", path);
            status = 1;
        }
    } else {
        char path[PDFTOOL_PATH_CAPACITY];
        const char *prefix = output != 0 ? output : "split";
        size_t first = 0U;
        unsigned int part = 1U;

        while (first < document.pages_len) {
            size_t count = every;

            if (count > document.pages_len - first) count = document.pages_len - first;
            make_part_path(path, sizeof(path), prefix, part);
            if (write_part(&document, first, count, path) != 0) {
                tool_write_error("pdfsplit", "write failed: ", path);
                status = 1;
                break;
            }
            first += count;
            part += 1U;
        }
    }
    pdf_document_free(&document);
    rt_free(data);
    return status;
}
