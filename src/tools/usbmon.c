#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define USBMON_DEFAULT_SOURCE "/sys/kernel/debug/usb/usbmon/0u"
#define USBMON_LINE_CAPACITY 4096U
#define USBMON_DATA_CAPACITY 513U
#define USBMON_TOKEN_CAPACITY 128U

typedef struct {
    const char *input_path;
    unsigned int bus;
    unsigned int device;
    unsigned int endpoint;
    unsigned long long limit;
    char transfer_type;
    int has_bus;
    int has_device;
    int has_endpoint;
    int raw;
    int show_data;
    int json;
} UsbmonOptions;

typedef struct {
    char tag[32];
    char event;
    char transfer_type;
    char direction;
    unsigned long long timestamp;
    unsigned int bus;
    unsigned int device;
    unsigned int endpoint;
    char status[32];
    unsigned long long length;
    unsigned long long iso_descriptor_count;
    char iso_descriptors[5][48];
    size_t iso_descriptors_present;
    char setup[5][9];
    char setup_tag[8];
    int has_setup;
    int setup_captured;
    char data[USBMON_DATA_CAPACITY];
    int data_truncated;
} UsbmonRecord;

static const char *transfer_type_name(char transfer_type) {
    if (transfer_type == 'C') return "control";
    if (transfer_type == 'Z') return "isochronous";
    if (transfer_type == 'I') return "interrupt";
    if (transfer_type == 'B') return "bulk";
    return "unknown";
}

static const char *event_name(char event) {
    if (event == 'S') return "submit";
    if (event == 'C') return "complete";
    if (event == 'E') return "error";
    return "unknown";
}

static int parse_uint_value(const char *text, unsigned long long maximum, unsigned long long *value_out) {
    unsigned long long value;

    if (text == 0 || rt_parse_uint(text, &value) != 0 || value > maximum) return -1;
    *value_out = value;
    return 0;
}

static int parse_transfer_type(const char *text, char *type_out) {
    if (rt_strcmp(text, "control") == 0 || rt_strcmp(text, "c") == 0) *type_out = 'C';
    else if (rt_strcmp(text, "isochronous") == 0 || rt_strcmp(text, "iso") == 0 || rt_strcmp(text, "z") == 0) *type_out = 'Z';
    else if (rt_strcmp(text, "interrupt") == 0 || rt_strcmp(text, "int") == 0 || rt_strcmp(text, "i") == 0) *type_out = 'I';
    else if (rt_strcmp(text, "bulk") == 0 || rt_strcmp(text, "b") == 0) *type_out = 'B';
    else return -1;
    return 0;
}

static int parse_address(char *text, UsbmonRecord *record) {
    char *parts[4];
    size_t part_count = 0U;
    size_t index;
    unsigned long long value;

    if (text == 0 || text[0] == '\0' || text[1] == '\0' || text[2] != ':') return -1;
    record->transfer_type = text[0];
    record->direction = text[1];
    if ((record->transfer_type != 'C' && record->transfer_type != 'Z' && record->transfer_type != 'I' && record->transfer_type != 'B') ||
        (record->direction != 'i' && record->direction != 'o')) return -1;
    parts[part_count++] = text + 3;
    for (index = 3U; text[index] != '\0'; ++index) {
        if (text[index] == ':') {
            text[index] = '\0';
            if (part_count >= 3U) return -1;
            parts[part_count++] = text + index + 1U;
        }
    }
    if (part_count != 3U) return -1;
    if (parse_uint_value(parts[0], 255ULL, &value) != 0) return -1;
    record->bus = (unsigned int)value;
    if (parse_uint_value(parts[1], 255ULL, &value) != 0) return -1;
    record->device = (unsigned int)value;
    if (parse_uint_value(parts[2], 15ULL, &value) != 0) return -1;
    record->endpoint = (unsigned int)value;
    return 0;
}

static size_t split_tokens(char *line, char **tokens, int *overflow_out) {
    size_t count = 0U;
    size_t index = 0U;

    *overflow_out = 0;
    while (line[index] != '\0') {
        while (line[index] != '\0' && rt_is_space(line[index])) line[index++] = '\0';
        if (line[index] == '\0') break;
        if (count >= USBMON_TOKEN_CAPACITY) {
            *overflow_out = 1;
            break;
        }
        tokens[count++] = line + index;
        while (line[index] != '\0' && !rt_is_space(line[index])) index += 1U;
    }
    return count;
}

static void collect_data(UsbmonRecord *record, char **tokens, size_t start, size_t token_count, int token_overflow) {
    size_t used = 0U;
    size_t index;

    record->data[0] = '\0';
    record->data_truncated = token_overflow;
    if (start >= token_count || rt_strcmp(tokens[start], "=") != 0) return;
    start += 1U;
    for (index = start; index < token_count; ++index) {
        size_t token_index;
        for (token_index = 0U; tokens[index][token_index] != '\0'; ++token_index) {
            if (used + 1U >= sizeof(record->data)) {
                record->data_truncated = 1;
                record->data[used] = '\0';
                return;
            }
            record->data[used++] = tokens[index][token_index];
        }
    }
    record->data[used] = '\0';
}

static int parse_record(char *line, UsbmonRecord *record) {
    char *tokens[USBMON_TOKEN_CAPACITY];
    size_t token_count;
    size_t data_start;
    int token_overflow;

    rt_memset(record, 0, sizeof(*record));
    token_count = split_tokens(line, tokens, &token_overflow);
    if (token_count < 6U || tokens[2][0] == '\0' || tokens[2][1] != '\0') return -1;
    rt_copy_string(record->tag, sizeof(record->tag), tokens[0]);
    if (parse_uint_value(tokens[1], ~0ULL, &record->timestamp) != 0) return -1;
    record->event = tokens[2][0];
    if (record->event != 'S' && record->event != 'C' && record->event != 'E') return -1;
    if (parse_address(tokens[3], record) != 0) return -1;
    if (record->transfer_type == 'C' && tokens[4][0] != '-' && (tokens[4][0] < '0' || tokens[4][0] > '9')) {
        size_t setup_index;
        if (token_count < 11U || record->transfer_type != 'C') return -1;
        record->has_setup = 1;
        record->setup_captured = rt_strcmp(tokens[4], "s") == 0;
        rt_copy_string(record->setup_tag, sizeof(record->setup_tag), tokens[4]);
        rt_copy_string(record->status, sizeof(record->status), "setup");
        for (setup_index = 0U; setup_index < 5U; ++setup_index) rt_copy_string(record->setup[setup_index], sizeof(record->setup[setup_index]), tokens[5U + setup_index]);
        if (parse_uint_value(tokens[10], ~0ULL, &record->length) != 0) return -1;
        data_start = 11U;
    } else {
        unsigned long long iso_count;
        rt_copy_string(record->status, sizeof(record->status), tokens[4]);
        if (record->transfer_type == 'Z') {
            size_t descriptor_index;
            size_t descriptor_count;
            if (parse_uint_value(tokens[5], ~0ULL, &iso_count) != 0) return -1;
            descriptor_count = iso_count < 5ULL ? (size_t)iso_count : 5U;
            if (token_count < 7U + descriptor_count) return -1;
            record->iso_descriptor_count = iso_count;
            record->iso_descriptors_present = descriptor_count;
            for (descriptor_index = 0U; descriptor_index < descriptor_count; ++descriptor_index) {
                rt_copy_string(record->iso_descriptors[descriptor_index], sizeof(record->iso_descriptors[descriptor_index]), tokens[6U + descriptor_index]);
            }
            if (parse_uint_value(tokens[6U + descriptor_count], ~0ULL, &record->length) != 0) return -1;
            data_start = 7U + descriptor_count;
        } else {
            if (parse_uint_value(tokens[5], ~0ULL, &record->length) != 0) return -1;
            data_start = 6U;
        }
    }
    collect_data(record, tokens, data_start, token_count, token_overflow);
    return 0;
}

static int record_matches(const UsbmonRecord *record, const UsbmonOptions *options) {
    if (options->has_bus && record->bus != options->bus) return 0;
    if (options->has_device && record->device != options->device) return 0;
    if (options->has_endpoint && record->endpoint != options->endpoint) return 0;
    if (options->transfer_type != '\0' && record->transfer_type != options->transfer_type) return 0;
    return 1;
}

static int write_text_record(const UsbmonRecord *record, const UsbmonOptions *options) {
    size_t index;

    if (rt_write_uint(1, record->timestamp) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, record->tag) != 0) return -1;
    if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, event_name(record->event)) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, transfer_type_name(record->transfer_type)) != 0) return -1;
    if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, record->direction == 'i' ? "in" : "out") != 0 || rt_write_char(1, ' ') != 0) return -1;
    if (rt_write_uint(1, record->bus) != 0 || rt_write_char(1, ':') != 0 || rt_write_uint(1, record->device) != 0 || rt_write_char(1, ':') != 0 || rt_write_uint(1, record->endpoint) != 0) return -1;
    if (rt_write_cstr(1, " status ") != 0 || rt_write_cstr(1, record->status) != 0 || rt_write_cstr(1, " length ") != 0 || rt_write_uint(1, record->length) != 0) return -1;
    if (record->has_setup) {
        if (rt_write_cstr(1, record->setup_captured ? " setup" : " setup-uncaptured") != 0) return -1;
        for (index = 0U; index < 5U; ++index) {
            if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, record->setup[index]) != 0) return -1;
        }
    }
    if (record->transfer_type == 'Z') {
        if (rt_write_cstr(1, " iso-frames ") != 0 || rt_write_uint(1, record->iso_descriptor_count) != 0) return -1;
        for (index = 0U; index < record->iso_descriptors_present; ++index) {
            if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, record->iso_descriptors[index]) != 0) return -1;
        }
    }
    if (options->show_data && record->data[0] != '\0') {
        if (rt_write_cstr(1, " data ") != 0 || rt_write_cstr(1, record->data) != 0) return -1;
        if (record->data_truncated && rt_write_cstr(1, "...") != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int write_json_record(const UsbmonRecord *record, const UsbmonOptions *options) {
    size_t index;

    if (tool_json_begin_event(1, "usbmon", "stdout", "transfer") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"tag\":") != 0 || tool_json_write_string(1, record->tag) != 0) return -1;
    if (rt_write_cstr(1, ",\"timestamp_us\":") != 0 || rt_write_uint(1, record->timestamp) != 0) return -1;
    if (rt_write_cstr(1, ",\"event\":") != 0 || tool_json_write_string(1, event_name(record->event)) != 0) return -1;
    if (rt_write_cstr(1, ",\"transfer_type\":") != 0 || tool_json_write_string(1, transfer_type_name(record->transfer_type)) != 0) return -1;
    if (rt_write_cstr(1, ",\"direction\":") != 0 || tool_json_write_string(1, record->direction == 'i' ? "in" : "out") != 0) return -1;
    if (rt_write_cstr(1, ",\"bus\":") != 0 || rt_write_uint(1, record->bus) != 0) return -1;
    if (rt_write_cstr(1, ",\"device\":") != 0 || rt_write_uint(1, record->device) != 0) return -1;
    if (rt_write_cstr(1, ",\"endpoint\":") != 0 || rt_write_uint(1, record->endpoint) != 0) return -1;
    if (rt_write_cstr(1, ",\"status\":") != 0 || tool_json_write_string(1, record->status) != 0) return -1;
    if (rt_write_cstr(1, ",\"length\":") != 0 || rt_write_uint(1, record->length) != 0) return -1;
    if (rt_write_cstr(1, ",\"setup\":") != 0) return -1;
    if (record->has_setup) {
        if (rt_write_char(1, '[') != 0) return -1;
        for (index = 0U; index < 5U; ++index) {
            if (index > 0U && rt_write_char(1, ',') != 0) return -1;
            if (tool_json_write_string(1, record->setup[index]) != 0) return -1;
        }
        if (rt_write_char(1, ']') != 0) return -1;
    } else if (rt_write_cstr(1, "null") != 0) return -1;
    if (rt_write_cstr(1, ",\"setup_tag\":") != 0) return -1;
    if (record->has_setup) {
        if (tool_json_write_string(1, record->setup_tag) != 0) return -1;
    } else if (rt_write_cstr(1, "null") != 0) return -1;
    if (rt_write_cstr(1, ",\"setup_captured\":") != 0 || rt_write_cstr(1, record->setup_captured ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"iso_descriptor_count\":") != 0 || rt_write_uint(1, record->iso_descriptor_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"iso_descriptors\":[") != 0) return -1;
    for (index = 0U; index < record->iso_descriptors_present; ++index) {
        if (index > 0U && rt_write_char(1, ',') != 0) return -1;
        if (tool_json_write_string(1, record->iso_descriptors[index]) != 0) return -1;
    }
    if (rt_write_char(1, ']') != 0) return -1;
    if (rt_write_cstr(1, ",\"payload\":") != 0) return -1;
    if (options->show_data && record->data[0] != '\0') {
        if (tool_json_write_string(1, record->data) != 0) return -1;
    } else if (rt_write_cstr(1, "null") != 0) return -1;
    if (rt_write_cstr(1, ",\"payload_truncated\":") != 0 || rt_write_cstr(1, record->data_truncated ? "true" : "false") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int process_line(const char *line, const UsbmonOptions *options, unsigned long long *matched_out) {
    char parse_buffer[USBMON_LINE_CAPACITY];
    UsbmonRecord record;

    if (line[0] == '\0') return 0;
    rt_copy_string(parse_buffer, sizeof(parse_buffer), line);
    if (parse_record(parse_buffer, &record) != 0) return -1;
    if (!record_matches(&record, options)) return 0;
    if (options->raw) {
        if (rt_write_line(1, line) != 0) return -2;
    } else if (options->json) {
        if (write_json_record(&record, options) != 0) return -2;
    } else if (write_text_record(&record, options) != 0) return -2;
    *matched_out += 1ULL;
    return options->limit != 0ULL && *matched_out >= options->limit ? 1 : 0;
}

static int read_stream(int fd, const UsbmonOptions *options) {
    char input[4096];
    char line[USBMON_LINE_CAPACITY];
    size_t used = 0U;
    unsigned long long matched = 0ULL;
    unsigned long long line_number = 1ULL;
    int overflow = 0;
    int malformed = 0;

    for (;;) {
        long bytes = platform_read(fd, input, sizeof(input));
        size_t index = 0U;

        if (bytes < 0) return -1;
        if (bytes == 0) break;
        while (index < (size_t)bytes) {
            char ch = input[index++];
            if (ch == '\r') continue;
            if (ch == '\n') {
                int result;
                line[used] = '\0';
                if (overflow) {
                    tool_write_error("usbmon", "input record exceeds 4095 bytes", 0);
                    malformed = 1;
                } else {
                    result = process_line(line, options, &matched);
                    if (result == 1) return malformed ? 1 : 0;
                    if (result == -2) return -1;
                    if (result < 0) {
                        char number[32];
                        rt_unsigned_to_string(line_number, number, sizeof(number));
                        tool_write_error("usbmon", "malformed input record at line ", number);
                        malformed = 1;
                    }
                }
                used = 0U;
                overflow = 0;
                line_number += 1ULL;
            } else if (!overflow) {
                if (used + 1U < sizeof(line)) line[used++] = ch;
                else overflow = 1;
            }
        }
    }
    if (used > 0U || overflow) {
        int result;
        line[used] = '\0';
        if (overflow) {
            tool_write_error("usbmon", "input record exceeds 4095 bytes", 0);
            return 1;
        }
        result = process_line(line, options, &matched);
        if (result == -2) return -1;
        if (result < 0) {
            char number[32];
            rt_unsigned_to_string(line_number, number, sizeof(number));
            tool_write_error("usbmon", "malformed input record at line ", number);
            malformed = 1;
        }
    }
    return malformed ? 1 : 0;
}

static void print_help(void) {
    rt_write_line(1, "usbmon - monitor Linux USB traffic");
    tool_write_usage("usbmon", "[-i FILE] [-b BUS] [-d DEVICE] [-e ENDPOINT] [-t TYPE] [-n COUNT] [-x] [-r] [--json]");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -i, --input FILE       read usbmon text records from FILE or - for stdin");
    rt_write_line(1, "  -b, --bus BUS          select a bus number");
    rt_write_line(1, "  -d, --device DEVICE    select a device address");
    rt_write_line(1, "  -e, --endpoint ENDPOINT select an endpoint number");
    rt_write_line(1, "  -t, --type TYPE        control, bulk, interrupt, or isochronous");
    rt_write_line(1, "  -n, --count COUNT      stop after COUNT matching records");
    rt_write_line(1, "  -x, --data             include captured payload data");
    rt_write_line(1, "  -r, --raw              print matching input records unchanged");
    rt_write_line(1, "  --json                 emit JSON Lines transfer events");
}

static int parse_options(int argc, char **argv, UsbmonOptions *options) {
    ToolOptState state;
    int result;

    rt_memset(options, 0, sizeof(*options));
    options->input_path = USBMON_DEFAULT_SOURCE;
    tool_opt_init(&state, argc, argv, "usbmon", "[-i FILE] [-b BUS] [-d DEVICE] [-e ENDPOINT] [-t TYPE] [-n COUNT] [-x] [-r] [--json]");
    while ((result = tool_opt_next(&state)) == TOOL_OPT_FLAG) {
        unsigned long long value;
        if (rt_strcmp(state.flag, "-i") == 0 || rt_strcmp(state.flag, "--input") == 0) {
            if (tool_opt_require_value(&state) != 0) return -1;
            options->input_path = state.value;
        } else if (rt_strcmp(state.flag, "-b") == 0 || rt_strcmp(state.flag, "--bus") == 0) {
            if (tool_opt_require_value(&state) != 0 || parse_uint_value(state.value, 255ULL, &value) != 0) return -1;
            options->has_bus = 1;
            options->bus = (unsigned int)value;
        } else if (rt_strcmp(state.flag, "-d") == 0 || rt_strcmp(state.flag, "--device") == 0) {
            if (tool_opt_require_value(&state) != 0 || parse_uint_value(state.value, 255ULL, &value) != 0) return -1;
            options->has_device = 1;
            options->device = (unsigned int)value;
        } else if (rt_strcmp(state.flag, "-e") == 0 || rt_strcmp(state.flag, "--endpoint") == 0) {
            if (tool_opt_require_value(&state) != 0 || parse_uint_value(state.value, 15ULL, &value) != 0) return -1;
            options->has_endpoint = 1;
            options->endpoint = (unsigned int)value;
        } else if (rt_strcmp(state.flag, "-t") == 0 || rt_strcmp(state.flag, "--type") == 0) {
            if (tool_opt_require_value(&state) != 0 || parse_transfer_type(state.value, &options->transfer_type) != 0) return -1;
        } else if (rt_strcmp(state.flag, "-n") == 0 || rt_strcmp(state.flag, "--count") == 0) {
            if (tool_opt_require_value(&state) != 0 || parse_uint_value(state.value, ~0ULL, &options->limit) != 0 || options->limit == 0ULL) return -1;
        } else if (rt_strcmp(state.flag, "-x") == 0 || rt_strcmp(state.flag, "--data") == 0) {
            options->show_data = 1;
        } else if (rt_strcmp(state.flag, "-r") == 0 || rt_strcmp(state.flag, "--raw") == 0) {
            options->raw = 1;
        } else {
            tool_write_error("usbmon", "unknown option: ", state.flag);
            return -1;
        }
    }
    if (result == TOOL_OPT_HELP) {
        print_help();
        return 1;
    }
    if (result == TOOL_OPT_ERROR || state.argi < argc) return -1;
    options->json = tool_json_is_enabled();
    if (options->json && options->raw) {
        tool_write_error("usbmon", "--json and --raw cannot be combined", 0);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    UsbmonOptions options;
    int fd;
    int should_close;
    int parse_result = parse_options(argc, argv, &options);
    int result;

    if (parse_result > 0) return 0;
    if (parse_result < 0) {
        tool_write_usage("usbmon", "[-i FILE] [-b BUS] [-d DEVICE] [-e ENDPOINT] [-t TYPE] [-n COUNT] [-x] [-r] [--json]");
        return 1;
    }
    should_close = rt_strcmp(options.input_path, "-") != 0;
    fd = should_close ? platform_open_read(options.input_path) : 0;
    if (fd < 0) {
        tool_write_error("usbmon", "cannot open input: ", options.input_path);
        if (rt_strcmp(options.input_path, USBMON_DEFAULT_SOURCE) == 0) tool_write_error("usbmon", "mount debugfs and grant read access, or use --input FILE", 0);
        return 1;
    }
    result = read_stream(fd, &options);
    if (should_close) (void)platform_close(fd);
    if (result < 0) {
        tool_write_error("usbmon", "cannot read input", 0);
        return 1;
    }
    return result;
}