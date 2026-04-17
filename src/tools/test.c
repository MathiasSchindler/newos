#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"
#include "test_impl.h"

int main(int argc, char **argv) {
    int status = test_run_expression(argc - 1, argv + 1);

    if (status == 2) {
        tool_write_usage(argv[0], "EXPRESSION");
    }

    return status;
}
