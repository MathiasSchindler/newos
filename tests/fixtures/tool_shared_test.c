#include "runtime.h"
#include "tool_util.h"
#include "tool_json_parser.h"
#include "hash_util.h"

static int check(int condition, const char *description) {
    if (condition) return 0;
    rt_write_cstr(2, "FAIL: ");
    rt_write_line(2, description);
    return 1;
}

static int json_case(void) {
    const char *text = "{\"name\":\"\\uD83C\\uDF33\",\"nul\":\"\\u0000\","
                       "\"n\":18446744073709551615,\"bad\":18446744073709551616}";
    static const char *invalid[] = {
        "[1,]", "01", "1e+", "\"\\q\"", "\"\\uD800\"", "\"\xc0\xaf\""
    };
    ToolJson json;
    ToolByteBuffer quoted;
    char decoded[8], long_text[301];
    unsigned long long number;
    size_t i;
    int failures = 0;
    if (tool_json_parse(&json, text, rt_strlen(text)) != 0)
        return check(0, "strict nested JSON parses");
    failures += check(tool_json_string(&json, tool_json_get(&json, 0, "name"),
                                      decoded, sizeof(decoded)) == 0 &&
                      rt_strcmp(decoded, "\xf0\x9f\x8c\xb3") == 0 &&
                      tool_json_string(&json, tool_json_get(&json, 0, "name"), decoded, 4U) != 0,
                      "surrogate pairs decode and require space for the terminator");
    failures += check(tool_json_string(&json, tool_json_get(&json, 0, "nul"),
                                      decoded, sizeof(decoded)) != 0 &&
                      tool_json_uint(&json, tool_json_get(&json, 0, "n"), &number) == 0 &&
                      number == ~0ULL &&
                      tool_json_uint(&json, tool_json_get(&json, 0, "bad"), &number) != 0,
                      "decoded NUL and unsigned overflow are rejected");
    tool_json_free(&json);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int result = tool_json_parse(&json, invalid[i], rt_strlen(invalid[i]));
        failures += check(result != 0 && json.tokens.data == 0 && json.tokens.count == 0,
                          "invalid JSON frees owned tokens");
        tool_json_free(&json);
    }
    rt_memset(long_text, 'x', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    tool_byte_buffer_init(&quoted);
    if (tool_json_append_string(&quoted, long_text) != 0 ||
        tool_json_parse(&json, (const char *)quoted.data, quoted.size) != 0) {
        failures += check(0, "long JSON value fixture");
    } else {
        char *copy = tool_json_dup_string(&json, 0, 300U);
        failures += check(!tool_json_equals(&json, 0, long_text) &&
                          tool_json_equals_alloc(&json, 0, long_text) &&
                          copy != 0 && rt_strcmp(copy, long_text) == 0,
                          "short and allocating comparisons retain distinct contracts");
        rt_free(copy);
        copy = tool_json_dup_string(&json, 0, (size_t)-1);
        failures += check(copy != 0 && tool_json_dup_string(&json, 0, 299U) == 0,
                          "string byte budgets reject excess without multiplication overflow");
        rt_free(copy);
        tool_json_free(&json);
    }
    quoted.size = 0;
    failures += check(tool_json_append_string(&quoted, "\"\\\n\r\t\b\f") == 0 &&
                      quoted.size == sizeof("\"\\\"\\\\\\n\\r\\t\\u0008\\u000c\"") - 1U &&
                      memcmp(quoted.data, "\"\\\"\\\\\\n\\r\\t\\u0008\\u000c\"", quoted.size) == 0,
                      "buffered JSON escaping preserves the exact wire format");
    {
        char nested[132];
        for (i = 0; i < 64U; i++) nested[i] = '[';
        nested[64] = '0';
        for (i = 65U; i < 129U; i++) nested[i] = ']';
        failures += check(tool_json_parse(&json, nested, 129U) == 0,
                          "JSON depth 64 is accepted");
        tool_json_free(&json);
        nested[64] = '[';
        nested[65] = '0';
        for (i = 66U; i < 131U; i++) nested[i] = ']';
        failures += check(tool_json_parse(&json, nested, 131U) != 0,
                          "JSON depth 65 is rejected");
        tool_json_free(&json);
    }
    quoted.size = 0;
    if (tool_byte_buffer_append_char(&quoted, '[') != 0) {
        failures += check(0, "token budget fixture");
    } else {
        for (i = 0; i < 1999999U; i++) {
            if (tool_byte_buffer_append_cstr(&quoted, i == 0 ? "0" : ",0") != 0) break;
        }
        if (i != 1999999U || tool_byte_buffer_append_char(&quoted, ']') != 0) {
            failures += check(0, "token budget fixture");
        } else {
            failures += check(tool_json_parse(&json, (const char *)quoted.data, quoted.size) == 0 &&
                              json.tokens.count == 2000000U, "two million JSON tokens are accepted");
            tool_json_free(&json);
            quoted.size--;
            if (tool_byte_buffer_append_cstr(&quoted, ",0]") != 0)
                failures += check(0, "token overflow fixture");
            else
                failures += check(tool_json_parse(&json, (const char *)quoted.data, quoted.size) != 0,
                                  "token two million and one is rejected");
            tool_json_free(&json);
        }
    }
    tool_byte_buffer_free(&quoted);
    return failures;
}

static int directory_case(void) {
    char root[96], parent[128], leaf[160], file[160], link[160], through[192], scratch[256];
    PlatformDirEntry info;
    int failures = 0, fd;
    rt_memset(&info, 0, sizeof(info));
    size_t length = tool_buffer_append_cstr(root, sizeof(root), 0, ".toolsharedtest-");
    tool_buffer_append_uint(root, sizeof(root), length, (unsigned int)platform_get_process_id());
    if (platform_make_directory(root, 0700U) != 0)
        return check(0, "create unique directory test root");
    rt_join_path(root, "parent", parent, sizeof(parent));
    rt_join_path(parent, "leaf", leaf, sizeof(leaf));
    rt_join_path(root, "file", file, sizeof(file));
    rt_join_path(root, "link", link, sizeof(link));
    rt_join_path(link, "child", through, sizeof(through));
    failures += check(tool_ensure_directory(leaf, 0700U, scratch, sizeof(scratch)) == 0 &&
                      tool_ensure_directory(leaf, 0755U, scratch, sizeof(scratch)) == 0 &&
                      platform_get_path_info(leaf, &info) == 0 && info.is_dir,
                      "create parents and accept an existing directory");
#if !defined(_WIN32)
    failures += check((info.mode & 0777U) == 0700U,
                      "existing leaf permissions are not changed");
    failures += check(platform_create_symbolic_link("parent", link) == 0 &&
                      tool_ensure_directory(link, 0700U, scratch, sizeof(scratch)) == -2 &&
                      tool_ensure_directory(through, 0700U, scratch, sizeof(scratch)) == -1,
                      "Unix leaf and parent symlinks are rejected");
    failures += check(platform_remove_file(link) == 0, "remove test symlink");
#endif
    fd = platform_open_create_exclusive(file, 0600U);
    failures += check(fd >= 0 && platform_close(fd) == 0 &&
                      tool_ensure_directory(file, 0700U, scratch, sizeof(scratch)) == -2 &&
                      tool_ensure_directory(through, 0700U, scratch, 1U) == -1,
                      "existing files and insufficient path scratch are rejected");
    failures += check(platform_remove_file(file) == 0 &&
                      platform_remove_directory(leaf) == 0 &&
                      platform_remove_directory(parent) == 0 &&
                      platform_remove_directory(root) == 0, "remove directory fixtures");
    return failures;
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

    failures += json_case();
    failures += directory_case();
    failures += check(hash_fnv1a_string("") == 0xcbf29ce484222325ULL &&
                      hash_fnv1a_string("foobar") == 0x85944171f73967e8ULL,
                      "FNV-1a standard vectors preserve unsigned 64-bit wraparound");

    if (failures == 0) rt_write_line(1, "shared tool tests passed");
    return failures != 0;
}
