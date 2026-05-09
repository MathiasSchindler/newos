#include "runtime.h"
#include "tool_util.h"
#include "xml.h"
#include "xml_dtd.h"

static int info_from_dtd_file(const char *dtd_path) {
    XmlDtd dtd;
    int result;
    xml_dtd_init(&dtd);
    result = xml_dtd_load(&dtd, dtd_path, 0, 0U, "xmldtdinfo");
    if (result == 0) xml_dtd_write_info(1, &dtd);
    xml_dtd_free(&dtd);
    return result == 0 ? 0 : 1;
}

static int info_from_xml(const char *path) {
    XmlDtd dtd;
    char *input;
    size_t length;
    int result;
    if (xml_read_document(path, &input, &length, "xmldtdinfo") != 0) return 1;
    xml_dtd_init(&dtd);
    result = xml_dtd_load(&dtd, "auto", input, length, "xmldtdinfo");
    if (result == 0) xml_dtd_write_info(1, &dtd);
    xml_dtd_free(&dtd);
    xml_free_document(input);
    return result == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    const char *dtd_path = 0;
    int option_result;
    int exit_code = 0;
    int index;

    tool_opt_init(&opt, argc, argv, "xmldtdinfo", "[--dtd FILE] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--dtd") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            dtd_path = opt.value;
        } else {
            tool_write_error("xmldtdinfo", "unknown option: ", opt.flag);
            tool_write_usage("xmldtdinfo", "[--dtd FILE] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmldtdinfo", "[--dtd FILE] [FILE ...]");
        return 0;
    }
    if (dtd_path != 0) return info_from_dtd_file(dtd_path);
    if (opt.argi >= argc) return info_from_xml(0);
    for (index = opt.argi; index < argc; ++index) if (info_from_xml(argv[index]) != 0) exit_code = 1;
    return exit_code;
}