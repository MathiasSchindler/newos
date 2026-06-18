#ifndef NEWOS_TOOL_UTIL_H
#define NEWOS_TOOL_UTIL_H

#include "platform.h"
#include "xml.h"

#include <stddef.h>

int tool_open_input(const char *path, int *fd_out, int *should_close_out);
void tool_close_input(int fd, int should_close);
int tool_read_all_input(const char *path, unsigned char **data_out, size_t *size_out);
int tool_read_all_input_report(const char *path, unsigned char **data_out, size_t *size_out, const char *tool_name);
void tool_write_usage(const char *program_name, const char *usage_suffix);
void tool_write_error(const char *tool_name, const char *message, const char *detail);
int tool_write_visible(int fd, const char *text, size_t length);
int tool_write_visible_line(int fd, const char *text);
int tool_write_record_text(int fd, const char *text, int zero_terminated);
int tool_write_file_all(const char *path, const unsigned char *data, size_t size);
int tool_write_file_all_report(const char *path, const unsigned char *data, size_t size, const char *tool_name);
int tool_validate_absolute_program_path(const char *tool_name, const char *path);
void tool_restore_terminal_mode_if_enabled(int fd, int *enabled_io, const PlatformTerminalState *state);
int tool_xml_name_stack_push(XmlNameStack *stack, XmlName name, const char *tool_name);
int tool_hex_value(char ch);
int tool_base64_value(char ch);
int tool_bytes_equal(const unsigned char *left, const unsigned char *right, size_t size);
int tool_bytes_equal_text(const unsigned char *bytes, const char *text, size_t size);

void tool_json_set_enabled(int enabled);
int tool_json_is_enabled(void);
unsigned long long tool_json_next_seq(void);
int tool_json_write_string(int fd, const char *text);
int tool_json_write_string_n(int fd, const char *text, size_t length);
int tool_json_write_base64(int fd, const unsigned char *data, size_t length);
int tool_json_begin_event(int fd, const char *tool_name, const char *stream_name, const char *event_name);
int tool_json_end_event(int fd);
int tool_json_field_string(int fd, const char *name, const char *value);
int tool_json_field_uint(int fd, const char *name, unsigned long long value);
int tool_json_field_bool(int fd, const char *name, int value);
int tool_json_write_diagnostic(const char *tool_name, const char *level, const char *message, const char *detail);
int tool_json_write_usage(const char *tool_name, const char *usage_suffix);

#define TOOL_OUTPUT_BUFFER_SIZE 16384U

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} ToolByteBuffer;

void tool_byte_buffer_init(ToolByteBuffer *buffer);
void tool_byte_buffer_free(ToolByteBuffer *buffer);
int tool_byte_buffer_reserve(ToolByteBuffer *buffer, size_t needed);
int tool_byte_buffer_reserve_extra(ToolByteBuffer *buffer, size_t extra);
int tool_byte_buffer_append(ToolByteBuffer *buffer, const void *data, size_t size);
int tool_byte_buffer_append_byte(ToolByteBuffer *buffer, unsigned int value);
int tool_byte_buffer_append_char(ToolByteBuffer *buffer, char ch);
int tool_byte_buffer_append_cstr(ToolByteBuffer *buffer, const char *text);
int tool_byte_buffer_append_u16_be(ToolByteBuffer *buffer, unsigned int value);
int tool_byte_buffer_append_u32_be(ToolByteBuffer *buffer, unsigned long long value);
int tool_byte_buffer_terminate(ToolByteBuffer *buffer);
int tool_byte_buffer_append_text(ToolByteBuffer *buffer, const char *text, size_t length);

typedef struct {
    int fd;
    size_t length;
    char buffer[TOOL_OUTPUT_BUFFER_SIZE];
} ToolOutputBuffer;

void tool_output_buffer_init(ToolOutputBuffer *output, int fd);
int tool_output_buffer_flush(ToolOutputBuffer *output);
int tool_output_buffer_write(ToolOutputBuffer *output, const char *text, size_t length);
int tool_output_buffer_write_char(ToolOutputBuffer *output, char ch);
int tool_output_buffer_write_cstr(ToolOutputBuffer *output, const char *text);

#define TOOL_RECORD_READER_BUFFER_SIZE 16384U

typedef struct {
    int fd;
    char delimiter;
    long chunk_len;
    long chunk_pos;
    int eof;
    char chunk[TOOL_RECORD_READER_BUFFER_SIZE];
} ToolRecordReader;

void tool_record_reader_init(ToolRecordReader *reader, int fd, char delimiter);
int tool_record_reader_next(ToolRecordReader *reader, char *record, size_t record_size, int *has_record_out);

typedef enum {
    TOOL_XML_KEY_ATTR = 1,
    TOOL_XML_KEY_TEXT,
    TOOL_XML_KEY_CHILD
} ToolXmlKeyKind;

typedef struct {
    ToolXmlKeyKind kind;
    const char *name;
} ToolXmlKeySpec;

typedef struct {
    const char *key;
    size_t key_length;
    unsigned int active_child_depth;
    int found;
} ToolXmlKeyState;

int tool_xml_key_parse(const char *text, ToolXmlKeySpec *spec, const char *tool_name);
int tool_xml_selector_compile(XmlSelector *selector_out, const char *selector, const char *tool_name);
void tool_xml_key_state_init(ToolXmlKeyState *state);
void tool_xml_key_start(const ToolXmlKeySpec *spec, const XmlToken *token, unsigned int depth, unsigned int capture_depth, ToolXmlKeyState *state);
void tool_xml_key_text(const ToolXmlKeySpec *spec, const XmlToken *token, unsigned int depth, unsigned int capture_depth, ToolXmlKeyState *state);
void tool_xml_key_end(const ToolXmlKeySpec *spec, unsigned int depth, ToolXmlKeyState *state);

typedef enum {
    TOOL_COLOR_NEVER = 0,
    TOOL_COLOR_AUTO = 1,
    TOOL_COLOR_ALWAYS = 2
} ToolColorMode;

typedef struct {
    int use_tls;
    int socket_fd;
    PlatformTlsClient tls;
} ToolHttpConnection;

int tool_http_connection_connect(ToolHttpConnection *connection, const char *host, unsigned int port, int use_tls);
unsigned int tool_http_default_port(int use_tls);
int tool_http_connection_fd(const ToolHttpConnection *connection);
long tool_http_connection_read(ToolHttpConnection *connection, void *buffer, size_t count);
int tool_http_connection_write_all(ToolHttpConnection *connection, const void *buffer, size_t count);
void tool_http_connection_close(ToolHttpConnection *connection);

typedef enum {
    TOOL_STYLE_PLAIN = 0,
    TOOL_STYLE_BOLD,
    TOOL_STYLE_RED,
    TOOL_STYLE_GREEN,
    TOOL_STYLE_YELLOW,
    TOOL_STYLE_BLUE,
    TOOL_STYLE_MAGENTA,
    TOOL_STYLE_CYAN,
    TOOL_STYLE_BOLD_RED,
    TOOL_STYLE_BOLD_GREEN,
    TOOL_STYLE_BOLD_YELLOW,
    TOOL_STYLE_BOLD_BLUE,
    TOOL_STYLE_BOLD_MAGENTA,
    TOOL_STYLE_BOLD_CYAN
} ToolTextStyle;

int tool_parse_color_mode(const char *text, int *mode_out);
void tool_set_global_color_mode(int mode);
int tool_get_global_color_mode(void);
int tool_should_use_color_fd(int fd, int mode);
void tool_style_begin(int fd, int mode, int style);
void tool_style_end(int fd, int mode);
void tool_write_styled(int fd, int mode, int style, const char *text);

/*
 * Lightweight iterative option parser.
 *
 * Pattern:
 *   ToolOptState s;
 *   int r;
 *   tool_opt_init(&s, argc, argv, "prog", "[-x] [-f FILE] ...");
 *   while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
 *       if (rt_strcmp(s.flag, "-f") == 0) {
 *           if (tool_opt_require_value(&s) != 0) return 1;
 *           path = s.value;
 *       } else if (rt_strcmp(s.flag, "-x") == 0) {
 *           opt_x = 1;
 *       } else {
 *           tool_write_error(s.prog, "unknown option: ", s.flag);
 *           tool_write_usage(s.prog, s.usage_suffix);
 *           return 1;
 *       }
 *   }
 *   if (r == TOOL_OPT_HELP) { ... print help ...; return 0; }
 *   if (r == TOOL_OPT_ERROR) return 1;
 *   // s.argi is index of first non-option argument
 */

typedef struct {
    int          argc;
    char       **argv;
    const char  *prog;
    const char  *usage_suffix;
    int          argi;   /* index of next argv entry to examine */
    const char  *flag;   /* current option string set by tool_opt_next */
    const char  *value;  /* option value set by tool_opt_require_value */
} ToolOptState;

#define TOOL_OPT_END   0  /* no more options; s.argi = first positional index */
#define TOOL_OPT_FLAG  1  /* s.flag holds the current option string */
#define TOOL_OPT_HELP  2  /* -h or --help was seen */
#define TOOL_OPT_ERROR 3  /* invalid option; error already written to stderr */

void tool_opt_init(ToolOptState *s, int argc, char **argv,
                   const char *prog, const char *usage_suffix);
int  tool_opt_next(ToolOptState *s);
int  tool_opt_require_value(ToolOptState *s);
int tool_should_print_file_header(int verbose, int quiet, int path_count);
int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what);
int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what);
int tool_parse_size_value(const char *text, unsigned long long *value_out);
int tool_parse_fixed_digits(const char *text, size_t start, size_t digits, unsigned int *value_out);
int tool_parse_numeric_timezone_offset(const char *text, size_t *index_io, int *offset_seconds_out);
int tool_parse_duration_ms(const char *text, unsigned long long *milliseconds_out);
int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out);
int tool_parse_signal_name(const char *text, int *signal_out);
const char *tool_signal_name(int signal_number);
void tool_write_signal_list(int fd);
unsigned long long tool_parse_decimal_field(const char *field, size_t field_size);
unsigned short tool_read_u16_le(const unsigned char *bytes);
unsigned short tool_read_u16_be(const unsigned char *bytes);
unsigned int tool_read_u24_le(const unsigned char *bytes);
unsigned int tool_read_u32_le(const unsigned char *bytes);
unsigned int tool_read_u32_be(const unsigned char *bytes);
unsigned long long tool_read_u64_le(const unsigned char *bytes);
unsigned long long tool_read_u64_be(const unsigned char *bytes);
void tool_store_u16_le(unsigned char *bytes, unsigned int value);
void tool_store_u16_be(unsigned char *bytes, unsigned int value);
void tool_store_u32_le(unsigned char *bytes, unsigned int value);
void tool_store_u32_be(unsigned char *bytes, unsigned int value);
void tool_store_u64_le(unsigned char *bytes, unsigned long long value);
void tool_store_u64_be(unsigned char *bytes, unsigned long long value);
void tool_copy_printable_bytes(char *dest, size_t dest_size, const unsigned char *src, size_t src_size);
int tool_str_equal(const char *left, const char *right);
int tool_contains_char(const char *text, char ch);
int tool_compare_text_slices(const char *left, size_t left_length, const char *right, size_t right_length);
char tool_ascii_tolower(char ch);
char tool_hex_digit(unsigned int value);
int tool_ascii_is_digit(char ch);
int tool_ascii_is_blank(char ch);
int tool_ascii_is_space(char ch);
int tool_ascii_is_word_byte(unsigned char ch);
int tool_ascii_is_token_space(char ch);
int tool_ascii_is_identifier_start(char ch);
int tool_ascii_is_identifier_char(char ch);
int tool_utf8_is_continuation_byte(unsigned char byte);
size_t tool_previous_utf8_codepoint_start(const char *text, size_t index);
int tool_text_match_has_word_boundaries(const char *text, size_t start, size_t end);
size_t tool_text_display_width_n(const char *text, size_t length);
int tool_str_equal_ignore_case_ascii(const char *left, const char *right);
int tool_contains_case_insensitive(const char *text, const char *needle);
int tool_text_is_decimal(const char *text);
size_t tool_count_decimal_digits(unsigned long long value);
int tool_starts_with(const char *text, const char *prefix);
int tool_token_equals(const char *text, size_t text_length, const char *token);
int tool_parse_http_status(const char *headers);
int tool_parse_pid_filter_list(const char *spec, int *pids_out, size_t max_count, size_t *count_out, const char *tool_name, int require_nonempty);
int tool_parse_tabstop_list(const char *text, unsigned long long *stops, size_t max_stops, size_t *count_out);
unsigned long long tool_next_tabstop(const unsigned long long *stops, size_t stop_count, unsigned long long column);
int tool_find_http_header_end(const char *buffer, size_t length, size_t *offset_out);
int tool_unicode_space_at(const char *text, size_t length, size_t index, size_t *advance_out);
void tool_format_uptime_compact(unsigned long long total_seconds, char *buffer, size_t buffer_size);
int tool_days_in_month(int year, unsigned int month);
long long tool_days_from_civil(int year, unsigned int month, unsigned int day);
void tool_civil_from_days(long long days, int *year_out, unsigned int *month_out, unsigned int *day_out);
int tool_build_epoch_timestamp(int year, unsigned int month, unsigned int day, unsigned int hour, unsigned int minute, unsigned int second, long long *epoch_out);
size_t tool_buffer_append_char(char *buffer, size_t buffer_size, size_t length, char ch);
size_t tool_buffer_append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text);
size_t tool_buffer_append_uint(char *buffer, size_t buffer_size, size_t length, unsigned long long value);
size_t tool_buffer_append_padded_base(char *buffer, size_t buffer_size, size_t length, unsigned long long value, unsigned int base, unsigned int width);
int tool_output_flush_buffer(int fd, unsigned char *buffer, size_t *length_io);
int tool_output_append_buffer(int fd, unsigned char *buffer, size_t buffer_size, size_t *length_io, const unsigned char *data, size_t data_size);
int tool_write_all_fd(int fd, const unsigned char *data, size_t size);
int tool_discard_input_bytes(int fd, unsigned long long count);
void tool_write_hex_value(int fd, unsigned long long value);
int tool_write_hex_bytes(int fd, const unsigned char *bytes, size_t size);
void tool_write_padding(int fd, size_t count);
unsigned int tool_pager_page_lines(unsigned int default_lines);
int tool_buffer_append_char_checked(char *buffer, size_t buffer_size, size_t *length_io, char ch);
int tool_buffer_append_text_checked(char *buffer, size_t buffer_size, size_t *length_io, const char *text);
int tool_buffer_append_uint_checked(char *buffer, size_t buffer_size, size_t *length_io, unsigned long long value);
void tool_trim_whitespace(char *text);
int tool_prompt_yes_no(const char *message, const char *path);

#define TOOL_SYMBOLIC_MODE_DIRECTORY          (1U << 0)
#define TOOL_SYMBOLIC_MODE_ALLOW_COPY         (1U << 1)
#define TOOL_SYMBOLIC_MODE_X_ALWAYS           (1U << 2)
#define TOOL_SYMBOLIC_MODE_REQUIRE_PERMISSION (1U << 3)

int tool_apply_symbolic_mode(const char *text, unsigned int current_mode, unsigned int flags, unsigned int *mode_out);
int tool_literal_prefix_matches(const char *pattern, const char *text, int ignore_case, size_t *consumed_out);
const char *tool_base_name(const char *path);
int tool_path_has_separator(const char *path);
int tool_path_is_dash(const char *path);
void tool_path_dirname(const char *path, char *buffer, size_t buffer_size);
void tool_path_copy_trimmed(char *buffer, size_t buffer_size, const char *path);
int tool_path_trimmed_equal(const char *left, const char *right);
void tool_path_build_temp_prefix(const char *target_path, const char *stem, char *buffer, size_t buffer_size);
int tool_path_append_suffix(const char *path, const char *suffix, char *buffer, size_t buffer_size);
int tool_path_replace_suffix_or_append(const char *path, const char *suffix, const char *fallback_suffix, char *buffer, size_t buffer_size);
void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size);
int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size);
int tool_build_sibling_program_path(const char *argv0, const char *program_name, char *buffer, size_t buffer_size);
void tool_resolve_host_program_path(char **argv_exec, char *buffer, size_t buffer_size);
int tool_wildcard_match(const char *pattern, const char *text);
int tool_regex_search(const char *pattern, const char *text, int ignore_case, size_t search_start, size_t *start_out, size_t *end_out);
int tool_regex_replace(const char *pattern, const char *replacement, const char *input, int ignore_case, int global, char *output, size_t output_size, int *changed_out);
int tool_regex_search_ex(const char *pattern, const char *text, int ignore_case, int extended, size_t search_start, size_t *start_out, size_t *end_out);
int tool_regex_replace_ex(const char *pattern, const char *replacement, const char *input, int ignore_case, int extended, int global, char *output, size_t output_size, int *changed_out);
int tool_resolve_destination(const char *source_path, const char *dest_path, char *buffer, size_t buffer_size);
int tool_canonicalize_path_policy(const char *path, int resolve_symlinks, int allow_missing, int logical_policy, char *buffer, size_t buffer_size);
int tool_canonicalize_path(const char *path, int resolve_symlinks, int allow_missing, char *buffer, size_t buffer_size);
int tool_path_exists(const char *path);
int tool_paths_equal(const char *left_path, const char *right_path);
int tool_path_is_root(const char *path);
int tool_path_is_same_or_child(const char *path, const char *prefix, char *scratch, size_t scratch_size);
int tool_ensure_parent_dirs(const char *path, char *scratch, size_t scratch_size);
int tool_decode_mount_field(const char *text, size_t text_length, char *buffer, size_t buffer_size);
int tool_next_mount_field(const char *line, size_t line_length, size_t *index_io, char *buffer, size_t buffer_size);
int tool_path_is_unsafe_relative(const char *path);
int tool_copy_file(const char *source_path, const char *dest_path);
int tool_copy_path(const char *source_path, const char *dest_path, int recursive, int preserve_mode, int preserve_symlinks);
int tool_remove_path(const char *path, int recursive);

typedef struct {
    int min_depth;
    int max_depth;
} ToolWalkOptions;

typedef struct {
    int prune;
} ToolWalkControl;

typedef int (*ToolWalkCallback)(const char *path, const PlatformDirEntry *entry, int depth, ToolWalkControl *control, void *user_data);

int tool_walk_path(const char *path, const ToolWalkOptions *options, ToolWalkCallback callback, void *user_data);

typedef struct {
    int exact;
    int ignore_case;
    int has_uid;
    unsigned int uid;
    int has_parent;
    int parent_pid;
    int skip_pid;
    const char *pattern;
} ToolProcessMatchOptions;

int tool_resolve_user_id(const char *text, unsigned int *uid_out);
int tool_resolve_group_id(const char *text, unsigned int *gid_out);
int tool_parse_pid(const char *text, int *pid_out);
int tool_process_matches(const PlatformProcessEntry *entry, const ToolProcessMatchOptions *options);

#endif
