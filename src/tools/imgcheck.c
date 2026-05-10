#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGCHECK_INITIAL_CAPACITY (64U * 1024U)

typedef struct {
    int quiet;
    int verbose;
    int plain;
} ImgcheckOptions;

static void print_usage(void) {
    tool_write_usage("imgcheck", "[-q|--quiet] [-v|--verbose] [-p|--plain] [file ...]");
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = IMGCHECK_INITIAL_CAPACITY;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        tool_write_error("imgcheck", "out of memory: ", path ? path : "stdin");
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            unsigned char *resized;
            size_t next_capacity = capacity * 2U;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgcheck", "input too large: ", path ? path : "stdin");
                return -1;
            }
            resized = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (resized == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgcheck", "out of memory: ", path ? path : "stdin");
                return -1;
            }
            buffer = resized;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            tool_write_error("imgcheck", "read failed: ", path ? path : "stdin");
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static void write_plain_result(const char *label, const ImageValidation *validation) {
    rt_write_cstr(1, label);
    rt_write_char(1, '\t');
    rt_write_cstr(1, image_format_extension(validation->format));
    rt_write_char(1, '\t');
    rt_write_cstr(1, validation->valid ? "ok" : "fail");
    rt_write_char(1, '\t');
    rt_write_line(1, validation->message);
}

static void write_human_result(const char *label, const ImageValidation *validation, const ImgcheckOptions *options) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, validation->valid ? ": OK" : ": FAIL");
    rt_write_cstr(1, " (");
    rt_write_cstr(1, image_format_extension(validation->format));
    rt_write_char(1, ')');
    if (options->verbose || !validation->valid) {
        rt_write_cstr(1, ": ");
        rt_write_cstr(1, validation->message);
    }
    rt_write_char(1, '\n');
}

static int check_path(const char *path, const ImgcheckOptions *options) {
    unsigned char *data;
    size_t size;
    ImageValidation validation;
    const char *label = path ? path : "stdin";
    int result;

    if (read_all_input(path, &data, &size) != 0) {
        return -1;
    }
    result = image_validate(data, size, &validation);
    rt_free(data);
    if (!options->quiet) {
        if (options->plain) {
            write_plain_result(label, &validation);
        } else {
            write_human_result(label, &validation, options);
        }
    }
    return result == 0 && validation.valid ? 0 : -1;
}

static int parse_options(int argc, char **argv, ImgcheckOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->quiet = 0;
    options->verbose = 0;
    options->plain = 0;
    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *arg = argv[arg_index];

        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 1;
        }
        if (rt_strcmp(arg, "-q") == 0 || rt_strcmp(arg, "--quiet") == 0) {
            options->quiet = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "-v") == 0 || rt_strcmp(arg, "--verbose") == 0) {
            options->verbose = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--plain") == 0) {
            options->plain = 1;
            arg_index += 1;
            continue;
        }
        tool_write_error("imgcheck", "unknown option: ", arg);
        print_usage();
        return -1;
    }
    *arg_index_out = arg_index;
    return 0;
}

int main(int argc, char **argv) {
    ImgcheckOptions options;
    int arg_index;
    int parse_result;
    int status = 0;

    parse_result = parse_options(argc, argv, &options, &arg_index);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 1;
    }
    if (arg_index >= argc) {
        return check_path(0, &options) == 0 ? 0 : 1;
    }
    while (arg_index < argc) {
        if (check_path(argv[arg_index], &options) != 0) {
            status = 1;
        }
        arg_index += 1;
    }
    return status;
}
