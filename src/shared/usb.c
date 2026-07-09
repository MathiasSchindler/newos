#include "usb_descriptor.h"

#include "runtime.h"

static uint16_t usb_read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static int usb_descriptor_header_valid(const uint8_t *bytes, size_t length, uint8_t expected_type, size_t minimum_length) {
    if (bytes == 0 || length < minimum_length || bytes[0] < minimum_length || bytes[0] > length) return 0;
    return bytes[1] == expected_type;
}

int usb_parse_device_descriptor(const uint8_t *bytes, size_t length, UsbDeviceDescriptor *descriptor_out) {
    if (!usb_descriptor_header_valid(bytes, length, USB_DESCRIPTOR_TYPE_DEVICE, 18U) || descriptor_out == 0) return -1;
    rt_memset(descriptor_out, 0, sizeof(*descriptor_out));
    descriptor_out->usb_version = usb_read_le16(bytes + 2U);
    descriptor_out->device_class = bytes[4];
    descriptor_out->device_subclass = bytes[5];
    descriptor_out->device_protocol = bytes[6];
    descriptor_out->max_packet_size0 = bytes[7];
    descriptor_out->vendor_id = usb_read_le16(bytes + 8U);
    descriptor_out->product_id = usb_read_le16(bytes + 10U);
    descriptor_out->device_version = usb_read_le16(bytes + 12U);
    descriptor_out->manufacturer_index = bytes[14];
    descriptor_out->product_index = bytes[15];
    descriptor_out->serial_number_index = bytes[16];
    descriptor_out->configuration_count = bytes[17];
    return 0;
}

int usb_parse_configuration_descriptor(const uint8_t *bytes, size_t length, UsbConfigurationDescriptor *descriptor_out) {
    if (!usb_descriptor_header_valid(bytes, length, USB_DESCRIPTOR_TYPE_CONFIGURATION, 9U) || descriptor_out == 0) return -1;
    rt_memset(descriptor_out, 0, sizeof(*descriptor_out));
    descriptor_out->total_length = usb_read_le16(bytes + 2U);
    if (descriptor_out->total_length < 9U) return -1;
    descriptor_out->interface_count = bytes[4];
    descriptor_out->configuration_value = bytes[5];
    descriptor_out->configuration_index = bytes[6];
    descriptor_out->attributes = bytes[7];
    descriptor_out->max_power_ma = (uint16_t)((uint16_t)bytes[8] * 2U);
    return 0;
}

int usb_parse_interface_descriptor(const uint8_t *bytes, size_t length, UsbInterfaceDescriptor *descriptor_out) {
    if (!usb_descriptor_header_valid(bytes, length, USB_DESCRIPTOR_TYPE_INTERFACE, 9U) || descriptor_out == 0) return -1;
    rt_memset(descriptor_out, 0, sizeof(*descriptor_out));
    descriptor_out->number = bytes[2];
    descriptor_out->alternate_setting = bytes[3];
    descriptor_out->endpoint_count = bytes[4];
    descriptor_out->interface_class = bytes[5];
    descriptor_out->interface_subclass = bytes[6];
    descriptor_out->interface_protocol = bytes[7];
    descriptor_out->interface_index = bytes[8];
    return 0;
}

int usb_parse_endpoint_descriptor(const uint8_t *bytes, size_t length, UsbEndpointDescriptor *descriptor_out) {
    if (!usb_descriptor_header_valid(bytes, length, USB_DESCRIPTOR_TYPE_ENDPOINT, 7U) || descriptor_out == 0) return -1;
    rt_memset(descriptor_out, 0, sizeof(*descriptor_out));
    descriptor_out->address = bytes[2];
    descriptor_out->attributes = bytes[3];
    descriptor_out->max_packet_size = usb_read_le16(bytes + 4U);
    descriptor_out->interval = bytes[6];
    return 0;
}

int usb_find_ccid_interface(const uint8_t *configuration, size_t configuration_length, UsbCcidInterface *interface_out) {
    size_t offset = 0U;
    UsbCcidInterface found;
    int in_ccid_interface = 0;

    if (configuration == 0 || interface_out == 0 || configuration_length < 9U) return -1;
    rt_memset(&found, 0, sizeof(found));

    while (offset + 2U <= configuration_length) {
        const uint8_t *descriptor = configuration + offset;
        size_t descriptor_length = descriptor[0];
        uint8_t descriptor_type = descriptor[1];

        if (descriptor_length < 2U || offset + descriptor_length > configuration_length) return -1;

        if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE) {
            UsbInterfaceDescriptor iface;
            if (usb_parse_interface_descriptor(descriptor, descriptor_length, &iface) != 0) return -1;
            if (in_ccid_interface && found.has_bulk_in && found.has_bulk_out) {
                *interface_out = found;
                return 0;
            }
            rt_memset(&found, 0, sizeof(found));
            in_ccid_interface = iface.interface_class == USB_CLASS_SMART_CARD;
            if (in_ccid_interface) {
                found.interface_descriptor = iface;
            }
        } else if (in_ccid_interface && descriptor_type == USB_DESCRIPTOR_TYPE_ENDPOINT) {
            UsbEndpointDescriptor endpoint;
            if (usb_parse_endpoint_descriptor(descriptor, descriptor_length, &endpoint) != 0) return -1;
            if ((endpoint.attributes & USB_ENDPOINT_TRANSFER_TYPE_MASK) == USB_ENDPOINT_TRANSFER_TYPE_BULK) {
                if ((endpoint.address & USB_ENDPOINT_DIRECTION_IN) != 0U) {
                    found.bulk_in = endpoint;
                    found.has_bulk_in = 1;
                } else {
                    found.bulk_out = endpoint;
                    found.has_bulk_out = 1;
                }
            } else if ((endpoint.attributes & USB_ENDPOINT_TRANSFER_TYPE_MASK) == USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT &&
                       (endpoint.address & USB_ENDPOINT_DIRECTION_IN) != 0U) {
                found.interrupt_in = endpoint;
                found.has_interrupt_in = 1;
            }
        } else if (in_ccid_interface && descriptor_type == USB_DESCRIPTOR_TYPE_CCID) {
            found.class_descriptor = descriptor;
            found.class_descriptor_length = descriptor_length;
        }

        offset += descriptor_length;
    }

    if (in_ccid_interface && found.has_bulk_in && found.has_bulk_out) {
        *interface_out = found;
        return 0;
    }
    return -1;
}

const char *usb_descriptor_type_name(uint8_t descriptor_type) {
    switch (descriptor_type) {
        case USB_DESCRIPTOR_TYPE_DEVICE: return "device";
        case USB_DESCRIPTOR_TYPE_CONFIGURATION: return "configuration";
        case USB_DESCRIPTOR_TYPE_INTERFACE: return "interface";
        case USB_DESCRIPTOR_TYPE_ENDPOINT: return "endpoint";
        case USB_DESCRIPTOR_TYPE_CCID: return "ccid";
        default: break;
    }
    return "unknown USB descriptor";
}

const char *usb_endpoint_transfer_type_name(uint8_t attributes) {
    switch (attributes & USB_ENDPOINT_TRANSFER_TYPE_MASK) {
        case USB_ENDPOINT_TRANSFER_TYPE_CONTROL: return "control";
        case USB_ENDPOINT_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
        case USB_ENDPOINT_TRANSFER_TYPE_BULK: return "bulk";
        case USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT: return "interrupt";
        default: break;
    }
    return "unknown USB transfer type";
}