#include "runtime.h"
#include "tool_util.h"

static int check(int condition, const char *description) {
    if (condition) return 0;
    rt_write_cstr(2, "FAIL: ");
    rt_write_line(2, description);
    return 1;
}

int main(void) {
    ToolByteBuffer bytes;
    ToolOutputBuffer output;
    static const char expected[] = "prefix:0,18446744073709551615";
    int failures = 0;

    tool_byte_buffer_init(&bytes);
    failures += check(tool_byte_buffer_append_cstr(&bytes, "prefix:") == 0 &&
                      tool_byte_buffer_append_uint(&bytes, 0ULL) == 0 &&
                      tool_byte_buffer_append_char(&bytes, ',') == 0 &&
                      tool_byte_buffer_append_uint(&bytes, ~0ULL) == 0 &&
                      bytes.size == sizeof(expected) - 1U &&
                      memcmp(bytes.data, expected, bytes.size) == 0,
                      "byte buffer decimal boundaries retain prefix without a terminator");
    tool_byte_buffer_free(&bytes);

    tool_output_buffer_init(&output, -1);
    failures += check(tool_output_buffer_write_cstr(&output, "prefix:") == 0 &&
                      tool_output_buffer_write_uint(&output, 0ULL) == 0 &&
                      tool_output_buffer_write_char(&output, ',') == 0 &&
                      tool_output_buffer_write_uint(&output, ~0ULL) == 0 &&
                      output.length == sizeof(expected) - 1U &&
                      memcmp(output.buffer, expected, output.length) == 0,
                      "stream decimal output stays buffered without touching the fd");
    output.length = sizeof(output.buffer);
    failures += check(tool_output_buffer_write_uint(&output, 1ULL) != 0,
                      "stream decimal flush errors propagate");

    if (failures == 0) rt_write_line(1, "shared tool tests passed");
    return failures != 0;
}
