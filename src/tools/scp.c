#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int is_remote_spec(const char *text) {
    size_t i = 0U;
    int saw_slash = 0;

    while (text != 0 && text[i] != '\0') {
        if (text[i] == '/') {
            saw_slash = 1;
        }
        if (text[i] == ':' && !saw_slash) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static void print_help(void) {
    rt_write_line(1, "scp - copy files using scp-style operands");
    rt_write_line(1, "Usage: scp [-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST");
    rt_write_line(1, "Current scope: local-to-local copies are implemented. Remote operands are parsed and rejected explicitly until SSH exec/SFTP support is available.");
}

int main(int argc, char **argv) {
    int recursive = 0;
    int preserve = 0;
    unsigned long long port = 22ULL;
    const char *identity = 0;
    ToolOptState opt;
    int r;
    int source_count;
    int i;

    tool_opt_init(&opt, argc, argv, "scp", "[-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST");
    while ((r = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-r") == 0 || rt_strcmp(opt.flag, "-R") == 0) {
            recursive = 1;
        } else if (rt_strcmp(opt.flag, "-p") == 0) {
            preserve = 1;
        } else if (rt_strcmp(opt.flag, "-P") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (tool_parse_uint_arg(opt.value, &port, "scp", "port") != 0 || port == 0ULL || port > 65535ULL) {
                tool_write_usage("scp", "[-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST");
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "-i") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            identity = opt.value;
        } else {
            tool_write_error("scp", "unknown option: ", opt.flag);
            tool_write_usage("scp", "[-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST");
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        print_help();
        return 0;
    }
    if (r == TOOL_OPT_ERROR) return 1;

    source_count = argc - opt.argi - 1;
    if (source_count < 1) {
        tool_write_usage("scp", "[-r] [-p] [-P PORT] [-i IDENTITY] SOURCE... DEST");
        return 1;
    }

    (void)port;
    (void)identity;

    for (i = opt.argi; i < argc; ++i) {
        if (is_remote_spec(argv[i])) {
            tool_write_error("scp", "remote transfers are not available yet: ", argv[i]);
            return 1;
        }
    }

    for (i = 0; i < source_count; ++i) {
        const char *source = argv[opt.argi + i];
        const char *dest = argv[argc - 1];
        char resolved_dest[PLATFORM_NAME_CAPACITY * 4U];

        if (source_count > 1) {
            if (tool_resolve_destination(source, dest, resolved_dest, sizeof(resolved_dest)) != 0) {
                tool_write_error("scp", "destination is not a directory: ", dest);
                return 1;
            }
            dest = resolved_dest;
        }
        if (tool_copy_path(source, dest, recursive, preserve, preserve) != 0) {
            tool_write_error("scp", "copy failed: ", source);
            return 1;
        }
    }
    return 0;
}