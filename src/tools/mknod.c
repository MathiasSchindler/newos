#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void mknod_usage(const char *program_name) {
    tool_write_usage(program_name, "[-m MODE] NAME TYPE [MAJOR MINOR]");
}

static int parse_octal_mode(const char *text, unsigned int *mode_out) {
    unsigned int mode = 0U;
    size_t i;

    if (text == 0 || text[0] == '\0' || mode_out == 0) {
        return -1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
        mode = (mode << 3) | (unsigned int)(text[i] - '0');
        if (mode > 07777U) {
            return -1;
        }
    }

    *mode_out = mode;
    return 0;
}

static int parse_node_type(const char *text, unsigned int *type_out) {
    if (rt_strcmp(text, "p") == 0 || rt_strcmp(text, "fifo") == 0) {
        *type_out = PLATFORM_NODE_FIFO;
        return 0;
    }
    if (rt_strcmp(text, "c") == 0 || rt_strcmp(text, "u") == 0 || rt_strcmp(text, "char") == 0) {
        *type_out = PLATFORM_NODE_CHAR;
        return 0;
    }
    if (rt_strcmp(text, "b") == 0 || rt_strcmp(text, "block") == 0) {
        *type_out = PLATFORM_NODE_BLOCK;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *program_name = tool_base_name(argv[0]);
    unsigned int mode = 0666U;
    unsigned int node_type = 0U;
    unsigned long long major_value = 0ULL;
    unsigned long long minor_value = 0ULL;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            mknod_usage(program_name);
            return 0;
        }
        if (rt_strcmp(argv[argi], "-m") == 0) {
            argi += 1;
            if (argi >= argc || parse_octal_mode(argv[argi], &mode) != 0) {
                tool_write_error(program_name, "invalid mode", 0);
                mknod_usage(program_name);
                return 1;
            }
            argi += 1;
            continue;
        }
        tool_write_error(program_name, "unknown option: ", argv[argi]);
        mknod_usage(program_name);
        return 1;
    }

    if (argi + 2 > argc || parse_node_type(argv[argi + 1], &node_type) != 0) {
        mknod_usage(program_name);
        return 1;
    }

    if (node_type == PLATFORM_NODE_FIFO) {
        if (argi + 2 != argc) {
            mknod_usage(program_name);
            return 1;
        }
    } else {
        if (argi + 4 != argc ||
            rt_parse_uint(argv[argi + 2], &major_value) != 0 ||
            rt_parse_uint(argv[argi + 3], &minor_value) != 0 ||
            major_value > 0xffffffffULL ||
            minor_value > 0xffffffffULL) {
            tool_write_error(program_name, "invalid device number", 0);
            mknod_usage(program_name);
            return 1;
        }
    }

    if (platform_create_node(argv[argi], node_type, mode, (unsigned int)major_value, (unsigned int)minor_value) != 0) {
        tool_write_error(program_name, "cannot create ", argv[argi]);
        return 1;
    }

    return 0;
}
