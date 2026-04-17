#include "source.h"

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int compiler_load_source(const char *path, CompilerSource *source_out) {
    int fd;
    int should_close;
    size_t total = 0;

    if (source_out == 0) {
        return -1;
    }

    rt_memset(source_out, 0, sizeof(*source_out));
    rt_copy_string(source_out->path, sizeof(source_out->path), (path != 0) ? path : "-");

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    for (;;) {
        size_t remaining = sizeof(source_out->data) - total - 1U;
        long bytes_read;

        if (remaining == 0) {
            tool_close_input(fd, should_close);
            return -2;
        }

        bytes_read = platform_read(fd, source_out->data + total, remaining);
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            tool_close_input(fd, should_close);
            return -1;
        }

        total += (size_t)bytes_read;
    }

    source_out->data[total] = '\0';
    source_out->size = total;
    tool_close_input(fd, should_close);
    return 0;
}
