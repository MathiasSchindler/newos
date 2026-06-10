#include "pdf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PDFTOOL_PATH_CAPACITY 512U

typedef struct {
    unsigned int first;
    unsigned int last; /* 0 means through the final page. */
} PdfPageSelector;

typedef struct {
    PdfPageSelector *items;
    size_t len;
} PdfPageSelectorList;

static void print_usage(void) {
    tool_write_usage("pdfsplit", "--every N -o PREFIX PDF | --pages LIST [-o OUTPUT] PDF | --odd|--even [-o OUTPUT] PDF");
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

static int parse_selector_uint(const char *text, size_t *offset_io, unsigned int *value_out) {
    size_t start = *offset_io;
    unsigned int value;

    value = parse_uint_text(text, offset_io);
    if (*offset_io == start || value == 0U) return -1;
    *value_out = value;
    return 0;
}

static size_t count_selector_slots(const char *text) {
    size_t count = 1U;
    size_t index;

    if (text == 0 || text[0] == '\0') return 0U;
    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == ',') count += 1U;
    }
    return count;
}

static void free_page_selectors(PdfPageSelectorList *selectors) {
    rt_free(selectors->items);
    selectors->items = 0;
    selectors->len = 0U;
}

static int parse_page_selectors(const char *text, PdfPageSelectorList *selectors) {
    size_t slot_count = count_selector_slots(text);
    size_t offset = 0U;

    free_page_selectors(selectors);
    if (slot_count == 0U) return -1;
    selectors->items = (PdfPageSelector *)rt_malloc_array(slot_count, sizeof(selectors->items[0]));
    if (selectors->items == 0) return -2;
    selectors->len = 0U;
    while (text[offset] != '\0') {
        PdfPageSelector selector;
        int have_first = 0;
        int have_last = 0;

        selector.first = 0U;
        selector.last = 0U;
        if (text[offset] >= '0' && text[offset] <= '9') {
            if (parse_selector_uint(text, &offset, &selector.first) != 0) goto invalid;
            have_first = 1;
        }
        if (text[offset] == '-') {
            offset += 1U;
            if (text[offset] >= '0' && text[offset] <= '9') {
                if (parse_selector_uint(text, &offset, &selector.last) != 0) goto invalid;
                have_last = 1;
            }
            if (!have_first && !have_last) goto invalid;
            if (!have_first) selector.first = 1U;
            if (have_last && selector.last < selector.first) goto invalid;
        } else if (have_first) {
            selector.last = selector.first;
        } else {
            goto invalid;
        }
        selectors->items[selectors->len++] = selector;
        if (text[offset] == ',') {
            offset += 1U;
            if (text[offset] == '\0') goto invalid;
        } else if (text[offset] != '\0') {
            goto invalid;
        }
    }
    return 0;
invalid:
    free_page_selectors(selectors);
    return -1;
}

static void write_page_selector_outside_error(size_t page_count) {
    rt_write_cstr(2, "pdfsplit: page selector outside document (pages: ");
    rt_write_uint(2, (unsigned long long)page_count);
    rt_write_cstr(2, ")\n");
}

static int build_selector_selections(const PdfDocument *document, const PdfPageSelectorList *selectors, PdfPageSelection **selections_out, size_t *selection_count_out) {
    PdfPageSelection *selections;
    size_t index;

    if (selectors->len == 0U) return -1;
    selections = (PdfPageSelection *)rt_malloc_array(selectors->len, sizeof(selections[0]));
    if (selections == 0) return -2;
    for (index = 0U; index < selectors->len; ++index) {
        size_t first = (size_t)selectors->items[index].first;
        size_t last = selectors->items[index].last == 0U ? document->pages_len : (size_t)selectors->items[index].last;

        if (first == 0U || first > document->pages_len || last < first || last > document->pages_len) {
            rt_free(selections);
            return -1;
        }
        selections[index].document = document;
        selections[index].first_page = first - 1U;
        selections[index].page_count = last - first + 1U;
    }
    *selections_out = selections;
    *selection_count_out = selectors->len;
    return 0;
}

static int build_parity_selections(const PdfDocument *document, int want_odd, PdfPageSelection **selections_out, size_t *selection_count_out) {
    PdfPageSelection *selections;
    size_t count = 0U;
    size_t index;
    size_t out_index;

    for (index = 0U; index < document->pages_len; ++index) {
        int is_odd = ((index + 1U) % 2U) == 1U;

        if (is_odd == want_odd) count += 1U;
    }
    if (count == 0U) return -1;
    selections = (PdfPageSelection *)rt_malloc_array(count, sizeof(selections[0]));
    if (selections == 0) return -2;
    out_index = 0U;
    for (index = 0U; index < document->pages_len; ++index) {
        int is_odd = ((index + 1U) % 2U) == 1U;

        if (is_odd == want_odd) {
            selections[out_index].document = document;
            selections[out_index].first_page = index;
            selections[out_index].page_count = 1U;
            out_index += 1U;
        }
    }
    *selections_out = selections;
    *selection_count_out = count;
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
    if (result == 0) result = tool_write_file_all(path, output.data, output.size);
    pdf_buffer_free(&output);
    return result;
}

static int write_selections(const PdfPageSelection *selections, size_t selection_count, const PdfDocumentInfo *metadata, const char *path) {
    PdfBuffer output;
    int result;

    pdf_buffer_init(&output);
    result = pdf_write_join(selections, selection_count, metadata, &output);
    if (result == 0) result = tool_write_file_all(path, output.data, output.size);
    pdf_buffer_free(&output);
    return result;
}

int main(int argc, char **argv) {
    const char *output = 0;
    const char *input_path = 0;
    unsigned int every = 0U;
    PdfPageSelectorList selectors;
    int select_odd = 0;
    int select_even = 0;
    int index;
    unsigned char *data = 0;
    size_t size = 0U;
    PdfDocument document;
    int status = 0;
    int mode_count;

    selectors.items = 0;
    selectors.len = 0U;
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
            int parse_status;

            if (index + 1 >= argc) {
                print_usage();
                free_page_selectors(&selectors);
                return 2;
            }
            parse_status = parse_page_selectors(argv[++index], &selectors);
            if (parse_status != 0) {
                tool_write_error("pdfsplit", parse_status == -2 ? "out of memory" : "invalid --pages selector: ", parse_status == -2 ? 0 : argv[index]);
                return parse_status == -2 ? 1 : 2;
            }
        } else if (rt_strcmp(arg, "--odd") == 0) {
            select_odd = 1;
        } else if (rt_strcmp(arg, "--even") == 0) {
            select_even = 1;
        } else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            free_page_selectors(&selectors);
            return 0;
        } else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfsplit", "unknown option: ", arg);
            free_page_selectors(&selectors);
            return 2;
        } else {
            input_path = arg;
        }
    }
    mode_count = (every != 0U ? 1 : 0) + (selectors.len != 0U ? 1 : 0) + (select_odd ? 1 : 0) + (select_even ? 1 : 0);
    if (input_path == 0 || mode_count != 1) {
        print_usage();
        free_page_selectors(&selectors);
        return 2;
    }
    if (tool_read_all_input(input_path, &data, &size) != 0) {
        tool_write_error("pdfsplit", "read failed: ", input_path);
        free_page_selectors(&selectors);
        return 1;
    }
    if (pdf_document_parse(data, size, &document) != 0) {
        tool_write_error("pdfsplit", "unsupported or unreadable PDF: ", input_path);
        rt_free(data);
        free_page_selectors(&selectors);
        return 1;
    }
    if (selectors.len != 0U || select_odd || select_even) {
        const char *path = output != 0 ? output : "split.pdf";
        PdfPageSelection *selections = 0;
        size_t selection_count = 0U;
        int build_status;

        if (selectors.len != 0U) {
            build_status = build_selector_selections(&document, &selectors, &selections, &selection_count);
        } else {
            build_status = build_parity_selections(&document, select_odd, &selections, &selection_count);
        }
        if (build_status == -2) {
            tool_write_error("pdfsplit", "out of memory", 0);
            status = 1;
        } else if (build_status != 0) {
            if (selectors.len != 0U) write_page_selector_outside_error(document.pages_len);
            else tool_write_error("pdfsplit", "no pages selected", 0);
            status = 1;
        } else if (write_selections(selections, selection_count, &document.info.document_info, path) != 0) {
            tool_write_error("pdfsplit", "write failed: ", path);
            status = 1;
        }
        rt_free(selections);
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
    free_page_selectors(&selectors);
    return status;
}
