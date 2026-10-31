#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup shared_predicates

cat > "$WORK_DIR/predicates_test.c" <<'C'
#include "runtime.h"
#include "tool_util.h"
#include "xml/xml.h"
#include "xml/xml_dtd.h"
#include "pdf.h"
#include "image/image_internal.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        rt_write_cstr(2, "shared predicate check failed at line "); \
        rt_write_uint(2, __LINE__); \
        rt_write_cstr(2, ": "); \
        rt_write_line(2, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    static const unsigned int windows1252[32] = {
        0x20acU, 0x0081U, 0x201aU, 0x0192U, 0x201eU, 0x2026U, 0x2020U, 0x2021U,
        0x02c6U, 0x2030U, 0x0160U, 0x2039U, 0x0152U, 0x008dU, 0x017dU, 0x008fU,
        0x0090U, 0x2018U, 0x2019U, 0x201cU, 0x201dU, 0x2022U, 0x2013U, 0x2014U,
        0x02dcU, 0x2122U, 0x0161U, 0x203aU, 0x0153U, 0x009dU, 0x017eU, 0x0178U
    };
    static const unsigned char sof_markers[] = {
        0xc0U, 0xc1U, 0xc2U, 0xc3U, 0xc5U, 0xc6U,
        0xc7U, 0xc9U, 0xcaU, 0xcbU, 0xcdU, 0xceU, 0xcfU
    };
    static const struct {
        unsigned long first;
        unsigned long last;
    } xml_ranges[] = {
        {9UL, 9UL}, {10UL, 10UL}, {13UL, 13UL},
        {0x20UL, 0xd7ffUL}, {0xe000UL, 0xfffdUL}, {0x10000UL, 0x10ffffUL}
    };
    static const struct {
        const char *text;
        int found;
        unsigned long long value;
    } pdf_cases[] = {
        {"<< /Length 17 >>", 1, 17ULL},
        {"<< /Length 0 >>", 1, 0ULL},
        {"<< /Length 18446744073709551615 >>", 1, ~0ULL},
        {"<< /Nested << /Length 99 >> /Length 7 >>", 1, 7ULL},
        {"<< /Nested << /Length 99 >> >>", 0, 0ULL},
        {"<< /Other [/Length 99] /Length 11 >>", 1, 11ULL},
        {"<< /Other (/Length 99) /Length 12 >>", 1, 12ULL},
        {"<< /Length 5 0 R >>", 0, 0ULL},
        {"<< /Length 5 % object\n 0 % generation\n R >>", 0, 0ULL},
        {"<< /Length 18446744073709551616 >>", 0, 0ULL},
        {"<< /Length -1 >>", 0, 0ULL},
        {"<< /LengthExtra 5 >>", 0, 0ULL},
        {"<< /Length (5) >>", 0, 0ULL},
        {"<< /Length >>", 0, 0ULL},
        {"", 0, 0ULL}
    };
    static const char dtd_text[] = "<!ELEMENT\troot\r\n(item*) \t\r\n>\n<!ELEMENT item (#PCDATA)>\n<!ATTLIST\troot\r\nmode\tCDATA\r\n#IMPLIED>";
    static const char dtd_document[] = "<root mode=\"A B\"><item>ok</item></root>";
    unsigned char bytes[256];
    char *utf8 = 0;
    unsigned char *roundtrip = 0;
    size_t utf8_length = 0U;
    size_t roundtrip_length = 0U;
    size_t index = 0U;
    size_t i;
    unsigned int byte;
    unsigned long codepoint;
    PlatformProcessEntry process;
    ToolProcessMatchOptions process_options;
    RtTextSegment segment;
    XmlDtd dtd;
    size_t match_start = 0U;
    size_t match_end = 0U;

    for (byte = 0U; byte < 256U; ++byte) {
        char ch = (char)(unsigned char)byte;
        char lower = byte >= 'A' && byte <= 'Z' ? (char)(byte + ('a' - 'A')) : ch;
        size_t lead_length = byte <= 0x7fU ? 1U :
            byte >= 0xc0U && byte <= 0xdfU ? 2U :
            byte >= 0xe0U && byte <= 0xefU ? 3U :
            byte >= 0xf0U && byte <= 0xf7U ? 4U : 0U;
        unsigned int mapped = byte >= 0x80U && byte <= 0x9fU ? windows1252[byte - 0x80U] : byte;
        int space = byte == 9U || byte == 10U || byte == 13U || byte == 32U;
        int name_start = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            byte == '_' || byte == ':' || byte >= 0x80U;
        int name_char = name_start || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.';
        int sof = 0;

        for (i = 0U; i < sizeof(sof_markers) / sizeof(sof_markers[0]); ++i) {
            if (byte == sof_markers[i]) sof = 1;
        }
        CHECK(tool_ascii_tolower(ch) == lower);
        CHECK(rt_utf8_sequence_length((unsigned char)byte) == lead_length);
        CHECK(rt_windows_1252_decode((unsigned char)byte) == mapped);
        CHECK(xml_is_space(ch) == space);
        CHECK(xml_is_name_start(ch) == name_start);
        CHECK(xml_is_name_char(ch) == name_char);
        CHECK(xml_is_ascii_char_allowed(ch) == (byte >= 32U || space));
        CHECK(image_jpeg_is_sof_marker((unsigned char)byte) == sof);
        bytes[byte] = (unsigned char)byte;
    }
    for (codepoint = 0UL; codepoint <= 0x110000UL; ++codepoint) {
        int allowed = 0;
        for (i = 0U; i < sizeof(xml_ranges) / sizeof(xml_ranges[0]); ++i) {
            if (codepoint >= xml_ranges[i].first && codepoint <= xml_ranges[i].last) allowed = 1;
        }
        CHECK(xml_is_codepoint_allowed(codepoint) == allowed);
    }
    CHECK(!xml_is_codepoint_allowed(~0UL));
    xml_dtd_init(&dtd);
    CHECK(xml_dtd_parse_subset(&dtd, dtd_text, sizeof(dtd_text) - 1U, "shared_predicates") == 0);
    CHECK(dtd.element_count == 2U);
    CHECK(dtd.attribute_count == 1U);
    CHECK(xml_dtd_validate_document(&dtd, "predicate-fixture", dtd_document, sizeof(dtd_document) - 1U, "shared_predicates") == 0);
    xml_dtd_free(&dtd);
    for (i = 0U; i < sizeof(pdf_cases) / sizeof(pdf_cases[0]); ++i) {
        unsigned long long value = 123456ULL;
        int found = pdf_find_top_u64_direct_value((const unsigned char *)pdf_cases[i].text,
            rt_strlen(pdf_cases[i].text), "/Length", &value);
        CHECK(found == pdf_cases[i].found);
        CHECK(value == (found ? pdf_cases[i].value : 123456ULL));
    }

    CHECK(rt_transcode_to_utf8(bytes, sizeof(bytes), RT_TEXT_ENCODING_WINDOWS_1252, &utf8, &utf8_length) == 0);
    for (byte = 0U; byte < 256U; ++byte) {
        unsigned int decoded = 0U;
        CHECK(rt_utf8_decode(utf8, utf8_length, &index, &decoded) == 0);
        CHECK(decoded == rt_windows_1252_decode((unsigned char)byte));
    }
    CHECK(index == utf8_length);
    CHECK(rt_transcode_from_utf8(utf8, utf8_length, RT_TEXT_ENCODING_WINDOWS_1252, &roundtrip, &roundtrip_length) == 0);
    CHECK(roundtrip_length == sizeof(bytes));
    CHECK(memcmp(bytes, roundtrip, sizeof(bytes)) == 0);
    rt_free(utf8);
    rt_free(roundtrip);

    CHECK(rt_text_next_segment("\342\202", 2U, 0U, &segment) == 0);
    CHECK(segment.end == 1U);
    CHECK(segment.flags == (RT_TEXT_SEGMENT_INVALID | RT_TEXT_SEGMENT_INCOMPLETE));
    CHECK(rt_text_next_segment("\200", 1U, 0U, &segment) == 0);
    CHECK(segment.flags == RT_TEXT_SEGMENT_INVALID);
    CHECK(rt_utf8_decode("\300\257", 2U, &match_start, &byte) != 0);

    CHECK(tool_regex_search("[A-C]", "b", 1, 0U, &match_start, &match_end) == 1);
    CHECK(match_start == 0U && match_end == 1U);
    CHECK(tool_regex_search("[a-c]", "B", 1, 0U, &match_start, &match_end) == 1);
    CHECK(tool_regex_search("[^A-C]", "b", 1, 0U, &match_start, &match_end) == 0);
    CHECK(tool_regex_search("[A-C]", "b", 0, 0U, &match_start, &match_end) == 0);
    rt_memset(&process, 0, sizeof(process));
    rt_memset(&process_options, 0, sizeof(process_options));
    rt_copy_string(process.name, sizeof(process.name), "/usr/bin/AbC");
    process_options.pattern = "abc";
    process_options.exact = 1;
    process_options.ignore_case = 1;
    CHECK(tool_process_matches(&process, &process_options) == 1);
    process_options.ignore_case = 0;
    CHECK(tool_process_matches(&process, &process_options) == 0);
    process_options.pattern = "/USR/BIN/ABC";
    process_options.ignore_case = 1;
    CHECK(tool_process_matches(&process, &process_options) == 1);
    return 0;
}
C

make --no-print-directory -s -C "$ROOT_DIR" -f - PREDICATE_TEST_DIR="$WORK_DIR" check-predicates <<'MAKE'
include Makefile

PREDICATE_TEST_DEPS := $(SHARED_DEPS) src/shared/runtime.h src/shared/tool_util.h src/shared/pdf.h src/shared/xml/xml.h src/shared/image/image_internal.h
PREDICATE_TEST_OBJECTS := $(sort $(MACOS_COMMON_OBJECTS) $(MACOS_XML_OBJECTS) $(MACOS_PDF_OBJECTS) $(MACOS_IMAGE_OBJECTS))

.PHONY: check-predicates
check-predicates: $(PREDICATE_TEST_DIR)/predicates_host $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),$(PREDICATE_TEST_DIR)/predicates_native)
	$(PREDICATE_TEST_DIR)/predicates_host
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
	$(PREDICATE_TEST_DIR)/predicates_native
endif

$(PREDICATE_TEST_DIR)/predicates_host: $(PREDICATE_TEST_DIR)/predicates_test.c $(PREDICATE_TEST_DEPS) $(PDF_SOURCES) $(IMAGE_SOURCES) $(HOST_PLATFORM_SOURCES)
	$(CC) $(HOST_CFLAGS) $< $(sort $(SHARED_SOURCES) $(PDF_SOURCES) $(IMAGE_SOURCES) $(HOST_PLATFORM_SOURCES)) -o $@

ifeq ($(LOCAL_MACOS_FREESTANDING),1)
$(PREDICATE_TEST_DIR)/predicates_test.lto.o: $(PREDICATE_TEST_DIR)/predicates_test.c $(PREDICATE_TEST_DEPS)
	$(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -Isrc/shared -c $< -o $@

$(PREDICATE_TEST_DIR)/predicates_native: $(PREDICATE_TEST_DIR)/predicates_test.lto.o $(PREDICATE_TEST_OBJECTS) $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(PREDICATE_TEST_DIR)/predicates_test.lto.o $(PREDICATE_TEST_OBJECTS)
endif
MAKE
