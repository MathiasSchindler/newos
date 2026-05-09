#include "runtime.h"
#include "tool_util.h"
#include "xml.h"
#include "xml_dtd.h"

static int apply_one(const char *dtd_path, int strip_doctype, const char *path) {
    XmlDtd dtd;
    XmlParser parser;
    XmlToken token;
    char *input;
    size_t length;
    int result;

    if (xml_read_document(path, &input, &length, "xmldtdapply") != 0) return 1;
    xml_dtd_init(&dtd);
    if (xml_dtd_load(&dtd, dtd_path, input, length, "xmldtdapply") != 0) {
        xml_free_document(input);
        xml_dtd_free(&dtd);
        return 1;
    }
    if (xml_dtd_validate_document(&dtd, path, input, length, "xmldtdapply") != 0) {
        xml_free_document(input);
        xml_dtd_free(&dtd);
        return 1;
    }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (strip_doctype && token.type == XML_TOKEN_DOCTYPE) continue;
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) xml_dtd_write_defaulted_start(1, &dtd, &token);
        else xml_write_raw(1, token.raw, token.raw_length);
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmldtdapply", path, &parser);
        xml_free_document(input);
        xml_dtd_free(&dtd);
        return 1;
    }
    xml_free_document(input);
    xml_dtd_free(&dtd);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    const char *dtd_path = "auto";
    int strip_doctype = 0;
    int option_result;
    int exit_code = 0;
    int index;

    tool_opt_init(&opt, argc, argv, "xmldtdapply", "[--dtd FILE|auto] [--strip-doctype] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--dtd") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            dtd_path = opt.value;
        } else if (rt_strcmp(opt.flag, "--strip-doctype") == 0) strip_doctype = 1;
        else {
            tool_write_error("xmldtdapply", "unknown option: ", opt.flag);
            tool_write_usage("xmldtdapply", "[--dtd FILE|auto] [--strip-doctype] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmldtdapply", "[--dtd FILE|auto] [--strip-doctype] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return apply_one(dtd_path, strip_doctype, 0);
    for (index = opt.argi; index < argc; ++index) if (apply_one(dtd_path, strip_doctype, argv[index]) != 0) exit_code = 1;
    return exit_code;
}