#include "usb_descriptor.h"

void rt_memset(void *buffer, int byte_value, size_t count) {
    unsigned char *bytes = (unsigned char *)buffer;
    size_t index;

    for (index = 0U; index < count; ++index) bytes[index] = (unsigned char)byte_value;
}

static int test_device_descriptor(void) {
    static const uint8_t descriptor[18] = {
        18U, USB_DESCRIPTOR_TYPE_DEVICE, 0x10U, 0x03U,
        0xefU, 0x02U, 0x01U, 64U,
        0x34U, 0x12U, 0x78U, 0x56U, 0x00U, 0x02U,
        1U, 2U, 3U, 2U
    };
    UsbDeviceDescriptor parsed;

    if (usb_parse_device_descriptor(descriptor, sizeof(descriptor), &parsed) != 0) return 1;
    if (parsed.usb_version != 0x0310U || parsed.vendor_id != 0x1234U || parsed.product_id != 0x5678U) return 2;
    if (parsed.device_class != 0xefU || parsed.configuration_count != 2U) return 3;
    if (usb_parse_device_descriptor(descriptor, sizeof(descriptor) - 1U, &parsed) == 0) return 4;
    if (usb_parse_device_descriptor(descriptor, sizeof(descriptor), 0) == 0) return 5;
    return 0;
}

static int test_configuration_descriptor(void) {
    static const uint8_t descriptor[9] = {
        9U, USB_DESCRIPTOR_TYPE_CONFIGURATION, 9U, 0U,
        1U, 1U, 0U, 0x80U, 250U
    };
    uint8_t malformed[9];
    UsbConfigurationDescriptor parsed;
    size_t index;

    if (usb_parse_configuration_descriptor(descriptor, sizeof(descriptor), &parsed) != 0) return 6;
    if (parsed.total_length != 9U || parsed.max_power_ma != 500U) return 7;
    for (index = 0U; index < sizeof(malformed); ++index) malformed[index] = descriptor[index];
    malformed[2] = 8U;
    if (usb_parse_configuration_descriptor(malformed, sizeof(malformed), &parsed) == 0) return 8;
    return 0;
}

static int test_ccid_interface(void) {
    static const uint8_t configuration[] = {
        9U, USB_DESCRIPTOR_TYPE_CONFIGURATION, 44U, 0U, 1U, 1U, 0U, 0x80U, 50U,
        9U, USB_DESCRIPTOR_TYPE_INTERFACE, 2U, 0U, 3U, USB_CLASS_SMART_CARD, 0U, 0U, 0U,
        5U, USB_DESCRIPTOR_TYPE_CCID, 0x10U, 0x01U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x02U, USB_ENDPOINT_TRANSFER_TYPE_BULK, 64U, 0U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x81U, USB_ENDPOINT_TRANSFER_TYPE_BULK, 64U, 0U, 0U,
        7U, USB_DESCRIPTOR_TYPE_ENDPOINT, 0x83U, USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT, 8U, 0U, 10U
    };
    UsbCcidInterface parsed;

    if (usb_find_ccid_interface(configuration, sizeof(configuration), &parsed) != 0) return 9;
    if (parsed.interface_descriptor.number != 2U || parsed.bulk_out.address != 0x02U || parsed.bulk_in.address != 0x81U) return 10;
    if (!parsed.has_interrupt_in || parsed.interrupt_in.address != 0x83U || parsed.class_descriptor_length != 5U) return 11;
    if (usb_find_ccid_interface(configuration, sizeof(configuration) - 1U, &parsed) == 0) return 12;
    return 0;
}

int main(void) {
    int result = test_device_descriptor();

    if (result != 0) return result;
    result = test_configuration_descriptor();
    if (result != 0) return result;
    return test_ccid_interface();
}