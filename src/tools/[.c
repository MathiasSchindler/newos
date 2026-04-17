#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"
#include "test_impl.h"

int main(int argc, char **argv) {
    int status;

    if (argc < 2 || rt_strcmp(argv[argc - 1], "]") != 0) {
        tool_write_error("[", "missing closing ]", 0);
        return 2;
    }

    status = test_run_expression(argc - 2, argv + 1);
    if (status == 2) {
        tool_write_usage("[", "EXPRESSION ]");
    }

    return status;
}
