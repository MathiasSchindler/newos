#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "usb_descriptor.h"

#define LSUSB_DEVICE_CAPACITY 256U
#define LSUSB_CONFIG_BUFFER_SIZE 4096U
typedef struct {
    int verbose;
    int tree;
    int show_path;
    int show_all;
    int has_vendor_filter;
    int has_product_filter;
    int has_class_filter;
    unsigned int vendor_filter;
    unsigned int product_filter;
    unsigned int class_filter;
} LsusbOptions;

static PlatformUsbDevice lsusb_devices[LSUSB_DEVICE_CAPACITY];
static unsigned char lsusb_config_buffer[LSUSB_CONFIG_BUFFER_SIZE];

static void write_hex_digit(unsigned int value) {
    rt_write_char(1, (char)(value < 10U ? ('0' + value) : ('a' + value - 10U)));
}

static void write_hex_width(unsigned int value, unsigned int width) {
    unsigned int shift = width * 4U;
    while (shift > 0U) {
        shift -= 4U;
        write_hex_digit((value >> shift) & 0x0fU);
    }
}

static const char *usb_class_name(unsigned int class_code) {
    switch (class_code) {
        case 0x00U: return "per-interface";
        case 0x01U: return "audio";
        case 0x02U: return "communications";
        case 0x03U: return "hid";
        case 0x05U: return "physical";
        case 0x06U: return "image";
        case 0x07U: return "printer";
        case 0x08U: return "mass-storage";
        case 0x09U: return "hub";
        case 0x0aU: return "cdc-data";
        case 0x0bU: return "smart-card";
        case 0x0dU: return "content-security";
        case 0x0eU: return "video";
        case 0x0fU: return "personal-healthcare";
        case 0x10U: return "audio-video";
        case 0x11U: return "billboard";
        case 0xdcU: return "diagnostic";
        case 0xe0U: return "wireless";
        case 0xefU: return "miscellaneous";
        case 0xfeU: return "application-specific";
        case 0xffU: return "vendor-specific";
        default: break;
    }
    return "unknown";
}

static int parse_hex_value(const char *text, unsigned int max_digits, unsigned int max_value, unsigned int *value_out) {
    unsigned int value = 0U;
    unsigned int digits = 0U;

    if (text == 0 || text[0] == '\0' || value_out == 0) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    while (text[digits] != '\0') {
        int nibble = tool_hex_value(text[digits]);
        if (digits >= max_digits || nibble < 0) return -1;
        value = (value << 4U) | (unsigned int)nibble;
        digits += 1U;
    }
    if (digits == 0U || value > max_value) return -1;
    *value_out = value;
    return 0;
}

static int parse_device_filter(const char *text, LsusbOptions *options) {
    char vendor_text[8];
    char product_text[8];
    size_t index = 0U;
    size_t vendor_length = 0U;
    size_t product_length = 0U;
    unsigned int value;

    if (text == 0 || options == 0) return -1;
    while (text[index] != '\0' && text[index] != ':') {
        if (vendor_length + 1U >= sizeof(vendor_text)) return -1;
        vendor_text[vendor_length++] = text[index++];
    }
    vendor_text[vendor_length] = '\0';
    if (vendor_length == 0U || parse_hex_value(vendor_text, 4U, 0xffffU, &value) != 0) return -1;
    options->has_vendor_filter = 1;
    options->vendor_filter = value;
    if (text[index] == ':') {
        index += 1U;
        while (text[index] != '\0') {
            if (product_length + 1U >= sizeof(product_text)) return -1;
            product_text[product_length++] = text[index++];
        }
        product_text[product_length] = '\0';
        if (product_length == 0U || parse_hex_value(product_text, 4U, 0xffffU, &value) != 0) return -1;
        options->has_product_filter = 1;
        options->product_filter = value;
    }
    return 0;
}

static int device_matches(const PlatformUsbDevice *device, const LsusbOptions *options) {
    if (device == 0 || options == 0) return 0;
    if (options->has_vendor_filter && device->vendor_id != options->vendor_filter) return 0;
    if (options->has_product_filter && device->product_id != options->product_filter) return 0;
    if (options->has_class_filter && device->device_class != options->class_filter) return 0;
    return 1;
}

static int compare_devices(const void *left_ptr, const void *right_ptr) {
    const PlatformUsbDevice *left = (const PlatformUsbDevice *)left_ptr;
    const PlatformUsbDevice *right = (const PlatformUsbDevice *)right_ptr;

    if (left->bus_number < right->bus_number) return -1;
    if (left->bus_number > right->bus_number) return 1;
    if (left->device_address < right->device_address) return -1;
    if (left->device_address > right->device_address) return 1;
    if (left->vendor_id < right->vendor_id) return -1;
    if (left->vendor_id > right->vendor_id) return 1;
    if (left->product_id < right->product_id) return -1;
    if (left->product_id > right->product_id) return 1;
    return rt_strcmp(left->path, right->path);
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-v] [-t] [-p] [-a] [-d VID[:PID]] [-c CLASS]");
}

static void print_help(const char *program_name) {
    rt_write_line(1, "lsusb - list USB devices");
    print_usage(program_name);
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -v            show configuration, interface, and endpoint descriptors");
    rt_write_line(1, "  -t            group devices by bus");
    rt_write_line(1, "  -p            show backend path");
    rt_write_line(1, "  -a            include all matching rows even if descriptor reads fail");
    rt_write_line(1, "  -d VID[:PID]  filter by hexadecimal vendor and optional product id");
    rt_write_line(1, "  -c CLASS      filter by hexadecimal USB device class");
}

static int parse_options(int argc, char **argv, LsusbOptions *options) {
    ToolOptState state;
    int result;

    rt_memset(options, 0, sizeof(*options));
    tool_opt_init(&state, argc, argv, "lsusb", "[-v] [-t] [-p] [-a] [-d VID[:PID]] [-c CLASS]");
    while ((result = tool_opt_next(&state)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(state.flag, "-v") == 0 || rt_strcmp(state.flag, "--verbose") == 0) {
            options->verbose = 1;
        } else if (rt_strcmp(state.flag, "-t") == 0 || rt_strcmp(state.flag, "--tree") == 0) {
            options->tree = 1;
        } else if (rt_strcmp(state.flag, "-p") == 0 || rt_strcmp(state.flag, "--path") == 0) {
            options->show_path = 1;
        } else if (rt_strcmp(state.flag, "-a") == 0 || rt_strcmp(state.flag, "--all") == 0) {
            options->show_all = 1;
        } else if (rt_strcmp(state.flag, "-d") == 0 || rt_strcmp(state.flag, "--device") == 0) {
            if (tool_opt_require_value(&state) != 0) return -1;
            if (parse_device_filter(state.value, options) != 0) {
                tool_write_error("lsusb", "invalid device filter: ", state.value);
                return -1;
            }
        } else if (rt_strcmp(state.flag, "-c") == 0 || rt_strcmp(state.flag, "--class") == 0) {
            unsigned int class_value;
            if (tool_opt_require_value(&state) != 0) return -1;
            if (parse_hex_value(state.value, 2U, 0xffU, &class_value) != 0) {
                tool_write_error("lsusb", "invalid class filter: ", state.value);
                return -1;
            }
            options->has_class_filter = 1;
            options->class_filter = class_value;
        } else {
            tool_write_error("lsusb", "unknown option: ", state.flag);
            print_usage("lsusb");
            return -1;
        }
    }
    if (result == TOOL_OPT_HELP) {
        print_help("lsusb");
        return 1;
    }
    if (result == TOOL_OPT_ERROR) return -1;
    if (state.argi < argc) {
        tool_write_error("lsusb", "unexpected operand: ", argv[state.argi]);
        print_usage("lsusb");
        return -1;
    }
    return 0;
}

static void print_id(const PlatformUsbDevice *device) {
    write_hex_width(device->vendor_id, 4U);
    rt_write_char(1, ':');
    write_hex_width(device->product_id, 4U);
}

static void print_device_summary(const PlatformUsbDevice *device, const LsusbOptions *options) {
    char number[32];

    rt_write_cstr(1, "Bus ");
    rt_unsigned_to_string(device->bus_number, number, sizeof(number));
    rt_write_cstr(1, number);
    rt_write_cstr(1, " Device ");
    rt_unsigned_to_string(device->device_address, number, sizeof(number));
    rt_write_cstr(1, number);
    rt_write_cstr(1, ": ID ");
    print_id(device);
    rt_write_cstr(1, " class ");
    write_hex_width(device->device_class, 2U);
    rt_write_cstr(1, " ");
    rt_write_cstr(1, usb_class_name(device->device_class));
    rt_write_cstr(1, " cfg ");
    rt_unsigned_to_string(device->configuration_count, number, sizeof(number));
    rt_write_cstr(1, number);
    if (options->show_path) {
        rt_write_cstr(1, " path ");
        rt_write_cstr(1, device->path);
    }
    rt_write_char(1, '\n');
}

static void print_tree_device(const PlatformUsbDevice *device, const LsusbOptions *options) {
    char number[32];

    rt_write_cstr(1, "  |__ Dev ");
    rt_unsigned_to_string(device->device_address, number, sizeof(number));
    rt_write_cstr(1, number);
    rt_write_cstr(1, ", ID ");
    print_id(device);
    rt_write_cstr(1, ", Class ");
    write_hex_width(device->device_class, 2U);
    rt_write_cstr(1, " ");
    rt_write_cstr(1, usb_class_name(device->device_class));
    if (options->show_path) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, device->path);
    }
    rt_write_char(1, '\n');
}

static int read_configuration(const PlatformUsbDevice *device, unsigned int configuration_index, size_t *length_out) {
    PlatformUsbHandle handle;
    int result;

    if (length_out == 0) return -1;
    *length_out = 0U;
    if (platform_usb_open(device, &handle) != 0) return -1;
    result = platform_usb_read_configuration_descriptor(&handle, configuration_index, lsusb_config_buffer, sizeof(lsusb_config_buffer), length_out);
    (void)platform_usb_close(&handle);
    return result;
}

static void print_endpoint_descriptor(const UsbEndpointDescriptor *endpoint) {
    rt_write_cstr(1, "      endpoint ");
    write_hex_width(endpoint->address, 2U);
    rt_write_cstr(1, " ");
    rt_write_cstr(1, (endpoint->address & USB_ENDPOINT_DIRECTION_IN) != 0U ? "in " : "out ");
    rt_write_cstr(1, usb_endpoint_transfer_type_name(endpoint->attributes));
    rt_write_cstr(1, " max-packet ");
    rt_write_uint(1, endpoint->max_packet_size);
    rt_write_cstr(1, " interval ");
    rt_write_uint(1, endpoint->interval);
    rt_write_char(1, '\n');
}

static const char *descriptor_name_for_context(unsigned int descriptor_type, unsigned int interface_class) {
    switch (descriptor_type) {
        case 0x06U: return "device-qualifier";
        case 0x07U: return "other-speed-configuration";
        case 0x08U: return "interface-power";
        case 0x0bU: return "interface-association";
        case 0x0fU: return "bos";
        case 0x10U: return "device-capability";
        case 0x21U:
            if (interface_class == 0x03U) return "hid";
            if (interface_class == USB_CLASS_SMART_CARD) return "ccid-class";
            return "class-specific";
        case 0x22U: return "report";
        case 0x23U: return "physical";
        case 0x24U: return "cs-interface";
        case 0x25U: return "cs-endpoint";
        case 0x29U: return "hub";
        case 0x2aU: return "superspeed-hub";
        case 0x30U: return "ss-endpoint-companion";
        case 0x31U: return "ssp-isochronous-endpoint-companion";
        default: break;
    }
    return usb_descriptor_type_name((uint8_t)descriptor_type);
}

static void print_extra_descriptor(const unsigned char *descriptor, size_t descriptor_length, unsigned int interface_class) {
    unsigned int descriptor_type = descriptor[1];

    rt_write_cstr(1, "      descriptor type 0x");
    write_hex_width(descriptor_type, 2U);
    rt_write_char(1, ' ');
    rt_write_cstr(1, descriptor_name_for_context(descriptor_type, interface_class));
    rt_write_cstr(1, " length ");
    rt_write_uint(1, descriptor_length);
    if ((descriptor_type == 0x24U || descriptor_type == 0x25U) && descriptor_length >= 3U) {
        rt_write_cstr(1, " subtype 0x");
        write_hex_width(descriptor[2], 2U);
    }
    rt_write_char(1, '\n');
}

static void print_configuration_details(const unsigned char *configuration, size_t configuration_length) {
    UsbConfigurationDescriptor config;
    size_t offset;
    unsigned int current_interface_class = 0U;

    if (usb_parse_configuration_descriptor(configuration, configuration_length, &config) != 0) return;
    rt_write_cstr(1, "    configuration ");
    rt_write_uint(1, config.configuration_value);
    rt_write_cstr(1, ": interfaces ");
    rt_write_uint(1, config.interface_count);
    rt_write_cstr(1, ", attributes 0x");
    write_hex_width(config.attributes, 2U);
    rt_write_cstr(1, ", max-power ");
    rt_write_uint(1, config.max_power_ma);
    rt_write_line(1, "mA");

    offset = 0U;
    while (offset + 2U <= configuration_length) {
        const unsigned char *descriptor = configuration + offset;
        size_t descriptor_length = descriptor[0];
        unsigned int descriptor_type = descriptor[1];
        if (descriptor_length < 2U || offset + descriptor_length > configuration_length) break;
        if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE) {
            UsbInterfaceDescriptor iface;
            if (usb_parse_interface_descriptor(descriptor, descriptor_length, &iface) == 0) {
                rt_write_cstr(1, "      interface ");
                rt_write_uint(1, iface.number);
                rt_write_cstr(1, " alt ");
                rt_write_uint(1, iface.alternate_setting);
                rt_write_cstr(1, " class ");
                write_hex_width(iface.interface_class, 2U);
                rt_write_cstr(1, " ");
                rt_write_cstr(1, usb_class_name(iface.interface_class));
                rt_write_cstr(1, " endpoints ");
                rt_write_uint(1, iface.endpoint_count);
                rt_write_char(1, '\n');
                current_interface_class = iface.interface_class;
            }
        } else if (descriptor_type == USB_DESCRIPTOR_TYPE_ENDPOINT) {
            UsbEndpointDescriptor endpoint;
            if (usb_parse_endpoint_descriptor(descriptor, descriptor_length, &endpoint) == 0) print_endpoint_descriptor(&endpoint);
        } else if (descriptor_type != USB_DESCRIPTOR_TYPE_CONFIGURATION) {
            print_extra_descriptor(descriptor, descriptor_length, current_interface_class);
        }
        offset += descriptor_length;
    }
}

static void print_verbose_details(const PlatformUsbDevice *device, const LsusbOptions *options) {
    unsigned int config_index;
    int printed_any = 0;

    for (config_index = 0U; config_index < device->configuration_count && config_index < 16U; ++config_index) {
        size_t config_length = 0U;
        if (read_configuration(device, config_index, &config_length) == 0 && config_length >= 9U) {
            print_configuration_details(lsusb_config_buffer, config_length);
            printed_any = 1;
        }
    }
    if (!printed_any && options->show_all) {
        rt_write_line(1, "    descriptors unavailable");
    }
}

static void print_tree(const PlatformUsbDevice *devices, size_t count, const LsusbOptions *options) {
    unsigned int current_bus = 0xffffffffU;
    size_t index;
    int printed = 0;

    for (index = 0U; index < count; ++index) {
        const PlatformUsbDevice *device = &devices[index];
        if (!device_matches(device, options)) continue;
        printed = 1;
        if (device->bus_number != current_bus) {
            current_bus = device->bus_number;
            rt_write_cstr(1, "/:  Bus ");
            rt_write_uint(1, current_bus);
            rt_write_line(1, ": USB host/root hub view");
        }
        print_tree_device(device, options);
        if (options->verbose) print_verbose_details(device, options);
    }
    if (!printed) rt_write_line(1, "lsusb: no USB devices found");
}

static void print_flat(const PlatformUsbDevice *devices, size_t count, const LsusbOptions *options) {
    size_t index;
    int printed = 0;
    for (index = 0U; index < count; ++index) {
        const PlatformUsbDevice *device = &devices[index];
        if (!device_matches(device, options)) continue;
        printed = 1;
        print_device_summary(device, options);
        if (options->verbose) print_verbose_details(device, options);
    }
    if (!printed) rt_write_line(1, "lsusb: no USB devices found");
}

int main(int argc, char **argv) {
    LsusbOptions options;
    size_t count = 0U;
    int parse_result;

    parse_result = parse_options(argc, argv, &options);
    if (parse_result > 0) return 0;
    if (parse_result < 0) return 1;

    if (platform_usb_list_devices(lsusb_devices, LSUSB_DEVICE_CAPACITY, &count) != 0) {
        tool_write_error("lsusb", "cannot list USB devices", 0);
        return 1;
    }
    rt_sort(lsusb_devices, count, sizeof(lsusb_devices[0]), compare_devices);
    if (options.tree) {
        print_tree(lsusb_devices, count, &options);
    } else {
        print_flat(lsusb_devices, count, &options);
    }
    return 0;
}
