#include "platform.h"
#include "runtime.h"
#include "usb_descriptor.h"

#include "mach.h"

#define MACOS_USB_IOKIT_CONNECT_TYPE 1U
#define MACOS_USB_SELECTOR_OPEN 0U
#define MACOS_USB_SELECTOR_CLOSE 1U
#define MACOS_USB_SELECTOR_DEVICE_REQUEST_OUT 6U
#define MACOS_USB_SELECTOR_DEVICE_REQUEST_IN 7U
#define MACOS_USB_SELECTOR_READ_PIPE 6U
#define MACOS_USB_SELECTOR_WRITE_PIPE 7U
#define MACOS_USB_SELECTOR_GET_PIPE_PROPERTIES 0x19U

#define MACOS_USB_DESCRIPTOR_CONFIGURATION 0x02U
#define MACOS_USB_REQUEST_GET_DESCRIPTOR 0x06U
#define MACOS_USB_DIR_IN 0x80U
#define MACOS_USB_TYPE_STANDARD 0x00U
#define MACOS_USB_RECIP_DEVICE 0x00U
#define MACOS_USB_DEFAULT_TIMEOUT_MS 5000U

static int macos_usb_cstr_equal(const char *left, const char *right) {
    size_t index = 0U;

    if (left == 0 || right == 0) return 0;
    while (left[index] != 0 && right[index] != 0) {
        if (left[index] != right[index]) return 0;
        index += 1U;
    }
    return left[index] == right[index];
}

static unsigned int macos_usb_read_le_property_uint(const unsigned char *data, uint32_t length) {
    unsigned int value = 0U;
    uint32_t index;

    if (length > 4U) length = 4U;
    for (index = 0; index < length; index++) value |= ((unsigned int)data[index]) << (8U * index);
    return value;
}

static unsigned int macos_usb_get_uint_property(MacosMachPort entry, const char *name, unsigned int fallback) {
    unsigned char data[16];
    uint32_t data_length = 0U;

    if (macos_iokit_registry_entry_get_property_bytes(entry, name, data, sizeof(data), &data_length) != 0 || data_length == 0U) return fallback;
    return macos_usb_read_le_property_uint(data, data_length);
}

static void macos_usb_append_uint(char *buffer, size_t capacity, unsigned int value) {
    char digits[16];
    size_t digit_count = 0U;
    size_t length;

    if (buffer == 0 || capacity == 0U) return;
    length = rt_strlen(buffer);
    if (length >= capacity - 1U) return;
    if (value == 0U) {
        buffer[length] = '0';
        buffer[length + 1U] = 0;
        return;
    }
    while (value != 0U && digit_count < sizeof(digits)) {
        digits[digit_count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (digit_count > 0U && length < capacity - 1U) {
        digit_count -= 1U;
        buffer[length++] = digits[digit_count];
    }
    buffer[length] = 0;
}

static void macos_usb_append_cstr(char *buffer, size_t capacity, const char *text) {
    size_t length;
    size_t index = 0U;

    if (buffer == 0 || text == 0 || capacity == 0U) return;
    length = rt_strlen(buffer);
    while (text[index] != 0 && length < capacity - 1U) buffer[length++] = text[index++];
    buffer[length] = 0;
}

static void macos_usb_format_bcd(unsigned int value, char *buffer, size_t capacity) {
    unsigned int digits[4];
    size_t output = 0U;

    if (buffer == 0 || capacity < 5U) return;
    digits[0] = (value >> 12U) & 0x0fU;
    digits[1] = (value >> 8U) & 0x0fU;
    digits[2] = (value >> 4U) & 0x0fU;
    digits[3] = value & 0x0fU;
    if (digits[0] != 0U) buffer[output++] = (char)('0' + digits[0]);
    buffer[output++] = (char)('0' + digits[1]);
    buffer[output++] = '.';
    buffer[output++] = (char)('0' + digits[2]);
    buffer[output++] = (char)('0' + digits[3]);
    buffer[output] = 0;
}

static void macos_usb_format_speed(unsigned int speed, char *buffer, size_t capacity) {
    const char *text = 0;

    if (speed == 0U) text = "1.5";
    else if (speed == 1U) text = "12";
    else if (speed == 2U) text = "480";
    else if (speed == 3U) text = "5000";
    else if (speed == 4U) text = "10000";
    if (text != 0) rt_copy_string(buffer, capacity, text);
}

static void macos_usb_fill_path(char *path, size_t path_capacity, const char *name, unsigned int location_id, unsigned int address) {
    if (path == 0 || path_capacity == 0U) return;
    rt_copy_string(path, path_capacity, "iokit:");
    macos_usb_append_cstr(path, path_capacity, name);
    macos_usb_append_cstr(path, path_capacity, "@");
    macos_usb_append_uint(path, path_capacity, location_id);
    macos_usb_append_cstr(path, path_capacity, ":");
    macos_usb_append_uint(path, path_capacity, address);
}

static int macos_usb_add_device(MacosMachPort entry, PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_io) {
    char name[128];
    unsigned int location_id;
    unsigned int address;
    unsigned int vendor_id;
    unsigned int product_id;
    unsigned int usb_version;
    unsigned int device_version;
    char path[PLATFORM_USB_PATH_CAPACITY];
    size_t index;

    if (count_io == 0) return -1;
    if (macos_iokit_registry_entry_get_name(entry, name, sizeof(name)) != 0) name[0] = 0;
    location_id = macos_usb_get_uint_property(entry, "locationID", 0U);
    address = macos_usb_get_uint_property(entry, "USB Address", 0U);
    vendor_id = macos_usb_get_uint_property(entry, "idVendor", 0U);
    product_id = macos_usb_get_uint_property(entry, "idProduct", 0U);
    usb_version = macos_usb_get_uint_property(entry, "bcdUSB", 0U);
    device_version = macos_usb_get_uint_property(entry, "bcdDevice", 0U);
    macos_usb_fill_path(path, sizeof(path), name, location_id, address);
    for (index = 0U; index < *count_io && index < entry_capacity; index++) {
        PlatformUsbDevice *existing = &entries_out[index];
        if (existing->device_address == address && existing->vendor_id == vendor_id && existing->product_id == product_id && macos_usb_cstr_equal(existing->path, path)) return 0;
    }
    if (*count_io < entry_capacity) {
        PlatformUsbDevice *device = &entries_out[*count_io];
        rt_memset(device, 0, sizeof(*device));
        rt_copy_string(device->path, sizeof(device->path), path);
        rt_copy_string(device->product, sizeof(device->product), name);
        rt_copy_string(device->topology, sizeof(device->topology), "location:");
        macos_usb_append_uint(device->topology, sizeof(device->topology), location_id);
        device->bus_number = (location_id >> 24U) & 0xffU;
        device->device_address = address;
        device->vendor_id = vendor_id;
        device->product_id = product_id;
        device->device_class = macos_usb_get_uint_property(entry, "bDeviceClass", 0U);
        device->device_subclass = macos_usb_get_uint_property(entry, "bDeviceSubClass", 0U);
        device->device_protocol = macos_usb_get_uint_property(entry, "bDeviceProtocol", 0U);
        device->configuration_count = macos_usb_get_uint_property(entry, "bNumConfigurations", 0U);
        if (usb_version != 0U) macos_usb_format_bcd(usb_version, device->usb_version, sizeof(device->usb_version));
        if (device_version != 0U) macos_usb_format_bcd(device_version, device->device_version, sizeof(device->device_version));
        macos_usb_format_speed(macos_usb_get_uint_property(entry, "Device Speed", 0xffffffffU), device->speed, sizeof(device->speed));
        device->active_configuration = macos_usb_get_uint_property(entry, "bConfigurationValue", 0U);
        device->has_active_configuration = device->active_configuration != 0U;
    }
    *count_io += 1U;
    return 0;
}

static int macos_usb_walk_ioreg(MacosMachPort entry, PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_io, unsigned int depth) {
    MacosMachPort iterator = 0U;
    MacosMachPort child = 0U;
    char class_name[128];
    int result;

    if (depth > 32U) return 0;
    if (macos_iokit_object_get_class(entry, class_name, sizeof(class_name)) == 0 && macos_usb_cstr_equal(class_name, "IOUSBHostDevice")) {
        if (macos_usb_add_device(entry, entries_out, entry_capacity, count_io) != 0) return -1;
    }
    result = macos_iokit_registry_entry_get_child_iterator(entry, "IOUSB", &iterator);
    if (result != 0) return 0;
    for (;;) {
        result = macos_iokit_iterator_next(iterator, &child);
        if (result != 0) return -1;
        if (child == MACOS_MACH_PORT_NULL) break;
        result = macos_usb_walk_ioreg(child, entries_out, entry_capacity, count_io, depth + 1U);
        if (result != 0) return result;
    }
    return 0;
}

static int macos_usb_path_matches_entry(MacosMachPort entry, const char *path) {
    char name[128];
    char candidate[PLATFORM_USB_PATH_CAPACITY];
    unsigned int location_id;
    unsigned int address;

    if (path == 0) return 0;
    if (macos_iokit_registry_entry_get_name(entry, name, sizeof(name)) != 0) name[0] = 0;
    location_id = macos_usb_get_uint_property(entry, "locationID", 0U);
    address = macos_usb_get_uint_property(entry, "USB Address", 0U);
    macos_usb_fill_path(candidate, sizeof(candidate), name, location_id, address);
    return macos_usb_cstr_equal(candidate, path);
}

static int macos_usb_find_device_entry(MacosMachPort entry, const char *path, MacosMachPort *entry_out, unsigned int depth) {
    MacosMachPort iterator = 0U;
    MacosMachPort child = 0U;
    char class_name[128];
    int result;

    if (entry_out == 0) return -1;
    if (depth > 32U) return 0;
    if (macos_iokit_object_get_class(entry, class_name, sizeof(class_name)) == 0 &&
        macos_usb_cstr_equal(class_name, "IOUSBHostDevice") &&
        macos_usb_path_matches_entry(entry, path)) {
        *entry_out = entry;
        return 1;
    }
    result = macos_iokit_registry_entry_get_child_iterator(entry, "IOUSB", &iterator);
    if (result != 0) return 0;
    for (;;) {
        result = macos_iokit_iterator_next(iterator, &child);
        if (result != 0) return -1;
        if (child == MACOS_MACH_PORT_NULL) break;
        result = macos_usb_find_device_entry(child, path, entry_out, depth + 1U);
        if (result != 0) return result;
    }
    return 0;
}

static int macos_usb_find_interface_entry(MacosMachPort entry, unsigned int interface_number, MacosMachPort *entry_out, unsigned int depth) {
    MacosMachPort iterator = 0U;
    MacosMachPort child = 0U;
    char class_name[128];
    int result;

    if (entry_out == 0) return -1;
    if (depth > 8U) return 0;
    if (macos_iokit_object_get_class(entry, class_name, sizeof(class_name)) == 0 &&
        macos_usb_cstr_equal(class_name, "IOUSBHostInterface") &&
        macos_usb_get_uint_property(entry, "bInterfaceNumber", 0xffffffffU) == interface_number) {
        *entry_out = entry;
        return 1;
    }
    result = macos_iokit_registry_entry_get_child_iterator(entry, "IOService", &iterator);
    if (result != 0) return 0;
    for (;;) {
        result = macos_iokit_iterator_next(iterator, &child);
        if (result != 0) return -1;
        if (child == MACOS_MACH_PORT_NULL) break;
        result = macos_usb_find_interface_entry(child, interface_number, entry_out, depth + 1U);
        if (result != 0) return result;
    }
    return 0;
}

static int macos_usb_open_connection(MacosMachPort service, MacosMachPort *connection_out) {
    uint64_t input[1];
    uint32_t output_count = 0U;
    int result;

    if (connection_out == 0) return -1;
    *connection_out = MACOS_MACH_PORT_NULL;
    result = macos_iokit_service_open_extended(service, MACOS_USB_IOKIT_CONNECT_TYPE, connection_out);
    if (result != 0) return result;
    input[0] = 0U;
    result = macos_iokit_connect_method(*connection_out, MACOS_USB_SELECTOR_OPEN, input, 1U, 0, &output_count);
    if (result == 0) return 0;
    input[0] = 1U;
    result = macos_iokit_connect_method(*connection_out, MACOS_USB_SELECTOR_OPEN, input, 1U, 0, &output_count);
    if (result == 0) return 0;
    (void)macos_iokit_service_close(*connection_out);
    *connection_out = MACOS_MACH_PORT_NULL;
    return result;
}

static int macos_usb_close_connection(MacosMachPort connection) {
    uint32_t output_count = 0U;

    if (connection == MACOS_MACH_PORT_NULL) return 0;
    (void)macos_iokit_connect_method(connection, MACOS_USB_SELECTOR_CLOSE, 0, 0U, 0, &output_count);
    return macos_iokit_service_close(connection);
}

static int macos_usb_endpoint_to_pipe(PlatformUsbHandle *handle, unsigned int endpoint, unsigned int *pipe_out) {
    uint64_t input[1];
    uint64_t output[8];
    uint32_t output_count;
    unsigned int pipe;
    unsigned int endpoint_count;

    if (pipe_out == 0) return -1;
    *pipe_out = 0U;
    if (handle == 0 || !handle->claimed || handle->interface_handle == 0U) return -1;
    endpoint_count = handle->interface_endpoint_count == 0U ? 16U : handle->interface_endpoint_count;
    for (pipe = 1U; pipe <= endpoint_count; pipe++) {
        input[0] = pipe;
        output_count = sizeof(output) / sizeof(output[0]);
        rt_memset(output, 0, sizeof(output));
        if (macos_iokit_connect_method((MacosMachPort)handle->interface_handle,
                                       MACOS_USB_SELECTOR_GET_PIPE_PROPERTIES,
                                       input,
                                       1U,
                                       output,
                                       &output_count) == 0 &&
            output_count >= 3U) {
            unsigned int direction = output[0] != 0U ? USB_ENDPOINT_DIRECTION_IN : 0U;
            unsigned int number = (unsigned int)output[1] & 0x0fU;
            unsigned int transfer_type = (unsigned int)output[2] & USB_ENDPOINT_TRANSFER_TYPE_MASK;
            if ((direction | number) == endpoint && transfer_type == USB_ENDPOINT_TRANSFER_TYPE_BULK) {
                *pipe_out = pipe;
                return 0;
            }
        }
    }
    return -1;
}

int platform_usb_list_devices(PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_out) {
    MacosMachPort main_port = 0U;
    MacosMachPort root = 0U;
    size_t count = 0U;

    if (entries_out == 0 && entry_capacity != 0U) return -1;
    if (count_out == 0) return -1;
    *count_out = 0U;
    if (macos_iokit_get_main_port(&main_port) != 0) return -1;
    if (macos_iokit_get_root_entry(main_port, &root) != 0) return -1;
    if (macos_usb_walk_ioreg(root, entries_out, entry_capacity, &count, 0U) != 0) return -1;
    *count_out = count < entry_capacity ? count : entry_capacity;
    return 0;
}

int platform_usb_open(const PlatformUsbDevice *device, PlatformUsbHandle *handle_out) {
    MacosMachPort main_port = 0U;
    MacosMachPort root = 0U;
    MacosMachPort device_entry = 0U;
    MacosMachPort device_connection = 0U;
    int result;

    if (device == 0 || handle_out == 0) return -1;
    rt_memset(handle_out, 0, sizeof(*handle_out));
    if (macos_iokit_get_main_port(&main_port) != 0) return -1;
    if (macos_iokit_get_root_entry(main_port, &root) != 0) return -1;
    result = macos_usb_find_device_entry(root, device->path, &device_entry, 0U);
    if (result <= 0 || device_entry == MACOS_MACH_PORT_NULL) return -1;
    if (macos_usb_open_connection(device_entry, &device_connection) != 0) return -1;
    handle_out->device_service = device_entry;
    handle_out->handle = device_connection;
    handle_out->active = 1;
    return 0;
}

int platform_usb_close(PlatformUsbHandle *handle) {
    if (handle == 0) return 0;
    if (handle->claimed) (void)platform_usb_release_interface(handle, handle->claimed_interface);
    if (handle->handle != 0U) (void)macos_usb_close_connection((MacosMachPort)handle->handle);
    rt_memset(handle, 0, sizeof(*handle));
    return 0;
}

int platform_usb_claim_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    MacosMachPort interface_entry = 0U;
    MacosMachPort interface_connection = 0U;
    int result;

    if (handle == 0 || !handle->active || handle->device_service == 0U || interface_number > 255U) return -1;
    if (handle->claimed) return handle->claimed_interface == interface_number ? 0 : -1;
    result = macos_usb_find_interface_entry((MacosMachPort)handle->device_service, interface_number, &interface_entry, 0U);
    if (result <= 0 || interface_entry == MACOS_MACH_PORT_NULL) return -1;
    if (macos_usb_open_connection(interface_entry, &interface_connection) != 0) return -1;
    handle->interface_service = interface_entry;
    handle->interface_handle = interface_connection;
    handle->claimed_interface = interface_number;
    handle->interface_endpoint_count = macos_usb_get_uint_property(interface_entry, "bNumEndpoints", 0U);
    handle->claimed = 1;
    return 0;
}

int platform_usb_release_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    if (handle == 0 || !handle->claimed || handle->claimed_interface != interface_number) return -1;
    if (handle->interface_handle != 0U) (void)macos_usb_close_connection((MacosMachPort)handle->interface_handle);
    handle->interface_service = 0U;
    handle->interface_handle = 0U;
    handle->claimed_interface = 0U;
    handle->interface_endpoint_count = 0U;
    handle->claimed = 0;
    return 0;
}

int platform_usb_control_transfer(PlatformUsbHandle *handle,
                                  unsigned int request_type,
                                  unsigned int request,
                                  unsigned int value,
                                  unsigned int index,
                                  unsigned char *data,
                                  size_t length,
                                  unsigned int timeout_milliseconds,
                                  size_t *transferred_out) {
    uint64_t input[8];
    uint64_t output[2];
    uint32_t output_count;
    uint32_t selector;
    int result;

    if (transferred_out != 0) *transferred_out = 0U;
    if (handle == 0 || !handle->active || handle->handle == 0U || length > 65535U || (length != 0U && data == 0)) return -1;
    input[0] = 0U;
    input[1] = request_type & 0xffU;
    input[2] = request & 0xffU;
    input[3] = value & 0xffffU;
    input[4] = index & 0xffffU;
    input[5] = length & 0xffffU;
    input[6] = (uint64_t)(uintptr_t)data;
    input[7] = timeout_milliseconds;
    selector = (request_type & MACOS_USB_DIR_IN) != 0U ? MACOS_USB_SELECTOR_DEVICE_REQUEST_IN : MACOS_USB_SELECTOR_DEVICE_REQUEST_OUT;
    output_count = (selector == MACOS_USB_SELECTOR_DEVICE_REQUEST_IN) ? 2U : 0U;
    output[0] = 0U;
    output[1] = 0U;
    result = macos_iokit_connect_method((MacosMachPort)handle->handle, selector, input, 8U, output, &output_count);
    if (result != 0) return -1;
    if (selector == MACOS_USB_SELECTOR_DEVICE_REQUEST_IN) {
        if (output_count >= 2U) {
            if (transferred_out != 0) *transferred_out = (size_t)output[1];
        }
    } else if (transferred_out != 0) {
        *transferred_out = length;
    }
    return 0;
}

int platform_usb_bulk_transfer(PlatformUsbHandle *handle, unsigned int endpoint, unsigned char *data, size_t length, unsigned int timeout_milliseconds, size_t *transferred_out) {
    uint64_t input[7];
    uint64_t output[1];
    uint32_t output_count;
    unsigned int pipe = 0U;
    unsigned int selector;
    int result;

    if (transferred_out != 0) *transferred_out = 0U;
    if (handle == 0 || !handle->claimed || handle->interface_handle == 0U || endpoint > 255U || length > 65535U || (length != 0U && data == 0)) return -1;
    if (macos_usb_endpoint_to_pipe(handle, endpoint, &pipe) != 0 || pipe == 0U) return -1;
    selector = (endpoint & USB_ENDPOINT_DIRECTION_IN) != 0U ? MACOS_USB_SELECTOR_READ_PIPE : MACOS_USB_SELECTOR_WRITE_PIPE;
    input[0] = pipe;
    input[1] = 0U;
    input[2] = timeout_milliseconds;
    input[3] = timeout_milliseconds;
    input[4] = (uint64_t)(uintptr_t)data;
    input[5] = length;
    input[6] = (endpoint & USB_ENDPOINT_DIRECTION_IN) != 0U ? 0U : 1U;
    output[0] = 0U;
    output_count = (endpoint & USB_ENDPOINT_DIRECTION_IN) != 0U ? 1U : 0U;
    result = macos_iokit_connect_method((MacosMachPort)handle->interface_handle, selector, input, 7U, output, &output_count);
    if (result != 0) return -1;
    if ((endpoint & USB_ENDPOINT_DIRECTION_IN) != 0U) {
        if (output_count < 1U) return -1;
        if (transferred_out != 0) *transferred_out = (size_t)output[0];
    } else if (transferred_out != 0) {
        *transferred_out = length;
    }
    return 0;
}

int platform_usb_read_configuration_descriptor(PlatformUsbHandle *handle, unsigned int configuration_index, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    unsigned char header[9];
    UsbConfigurationDescriptor descriptor;
    size_t transferred = 0U;
    size_t wanted;

    if (length_out != 0) *length_out = 0U;
    if (handle == 0 || !handle->active || buffer == 0 || buffer_size == 0U || configuration_index > 255U || length_out == 0) return -1;
    if (platform_usb_control_transfer(handle,
                                      MACOS_USB_DIR_IN | MACOS_USB_TYPE_STANDARD | MACOS_USB_RECIP_DEVICE,
                                      MACOS_USB_REQUEST_GET_DESCRIPTOR,
                                      (unsigned int)((MACOS_USB_DESCRIPTOR_CONFIGURATION << 8U) | configuration_index),
                                      0U,
                                      header,
                                      sizeof(header),
                                      MACOS_USB_DEFAULT_TIMEOUT_MS,
                                      &transferred) != 0 || transferred != sizeof(header)) {
        return -1;
    }
    if (usb_parse_configuration_descriptor(header, sizeof(header), &descriptor) != 0) return -1;
    wanted = descriptor.total_length;
    if (wanted > buffer_size) wanted = buffer_size;
    if (platform_usb_control_transfer(handle,
                                      MACOS_USB_DIR_IN | MACOS_USB_TYPE_STANDARD | MACOS_USB_RECIP_DEVICE,
                                      MACOS_USB_REQUEST_GET_DESCRIPTOR,
                                      (unsigned int)((MACOS_USB_DESCRIPTOR_CONFIGURATION << 8U) | configuration_index),
                                      0U,
                                      buffer,
                                      wanted,
                                      MACOS_USB_DEFAULT_TIMEOUT_MS,
                                      &transferred) != 0 || transferred < sizeof(header)) {
        return -1;
    }
    *length_out = transferred;
    return 0;
}