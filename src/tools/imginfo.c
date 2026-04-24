#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGINFO_READ_LIMIT (256U * 1024U)

typedef struct {
    int mime_only;
    int machine_output;
} ImginfoOptions;

static void print_usage(void) {
    tool_write_usage("imginfo", "[-m|--mime] [-p|--plain] [file ...]");
}

static int read_probe_data(const char *path, unsigned char *buffer, size_t buffer_size, size_t *size_out) {
    int fd;
    int should_close;
    long bytes_read;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    bytes_read = platform_read(fd, buffer, buffer_size);
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        tool_write_error("imginfo", "read failed: ", path ? path : "stdin");
        return -1;
    }
    *size_out = (size_t)bytes_read;
    return 0;
}

static int write_unknown_field(void) {
    return rt_write_char(1, '-');
}

static int write_uint_field(unsigned int value) {
    return rt_write_uint(1, (unsigned long long)value);
}

static int write_machine_line(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, label);
    rt_write_char(1, '\t');
    rt_write_cstr(1, image_format_extension(info->format));
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U) {
        write_uint_field(info->width);
        rt_write_char(1, '\t');
        write_uint_field(info->height);
    } else {
        write_unknown_field();
        rt_write_char(1, '\t');
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U) {
        write_uint_field(info->bit_depth);
    } else {
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U) {
        write_uint_field(info->channel_count);
    } else {
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    rt_write_line(1, image_format_mime(info->format));
    return 0;
}

static int write_human_line(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_cstr(1, image_format_name(info->format));
    rt_write_cstr(1, " image");
    if ((info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->width);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info->height);
    }
    if ((info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->bit_depth);
        rt_write_cstr(1, "-bit");
    }
    if ((info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, image_channel_description(info));
    }
    rt_write_cstr(1, ", ");
    rt_write_line(1, image_format_mime(info->format));
    return 0;
}

static int describe_path(const char *path, const ImginfoOptions *options) {
    unsigned char buffer[IMGINFO_READ_LIMIT];
    size_t size = 0U;
    ImageInfo info;
    const char *label = path ? path : "stdin";

    if (read_probe_data(path, buffer, sizeof(buffer), &size) != 0) {
        return -1;
    }
    if (image_probe(buffer, size, &info) != 0) {
        tool_write_error("imginfo", "unsupported image format: ", label);
        return -1;
    }
    if (options->mime_only) {
        rt_write_cstr(1, label);
        rt_write_cstr(1, ": ");
        rt_write_line(1, image_format_mime(info.format));
        return 0;
    }
    if (options->machine_output) {
        return write_machine_line(label, &info);
    }
    return write_human_line(label, &info);
}

static int parse_options(int argc, char **argv, ImginfoOptions *options, int *arg_index_out) {
    int argi = 1;

    options->mime_only = 0;
    options->machine_output = 0;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 1;
        }
        if (rt_strcmp(arg, "-m") == 0 || rt_strcmp(arg, "--mime") == 0) {
            options->mime_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--plain") == 0) {
            options->machine_output = 1;
            argi += 1;
            continue;
        }
        tool_write_error("imginfo", "unknown option: ", arg);
        print_usage();
        return -1;
    }
    *arg_index_out = argi;
    return 0;
}

int main(int argc, char **argv) {
    ImginfoOptions options;
    int argi;
    int parse_result;
    int status = 0;

    parse_result = parse_options(argc, argv, &options, &argi);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 1;
    }
    if (argi >= argc) {
        return describe_path(0, &options) == 0 ? 0 : 1;
    }
    while (argi < argc) {
        if (describe_path(argv[argi], &options) != 0) {
            status = 1;
        }
        argi += 1;
    }
    return status;
}
