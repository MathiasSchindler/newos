#ifndef NEWOS_USB_DESCRIPTOR_H
#define NEWOS_USB_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

#define USB_DESCRIPTOR_TYPE_DEVICE 0x01U
#define USB_DESCRIPTOR_TYPE_CONFIGURATION 0x02U
#define USB_DESCRIPTOR_TYPE_INTERFACE 0x04U
#define USB_DESCRIPTOR_TYPE_ENDPOINT 0x05U
#define USB_DESCRIPTOR_TYPE_CCID 0x21U

#define USB_CLASS_SMART_CARD 0x0bU
#define USB_ENDPOINT_DIRECTION_IN 0x80U
#define USB_ENDPOINT_TRANSFER_TYPE_MASK 0x03U
#define USB_ENDPOINT_TRANSFER_TYPE_CONTROL 0x00U
#define USB_ENDPOINT_TRANSFER_TYPE_ISOCHRONOUS 0x01U
#define USB_ENDPOINT_TRANSFER_TYPE_BULK 0x02U
#define USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT 0x03U

typedef struct {
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_number_index;
    uint8_t configuration_count;
} UsbDeviceDescriptor;

typedef struct {
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint16_t max_power_ma;
} UsbConfigurationDescriptor;

typedef struct {
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
} UsbInterfaceDescriptor;

typedef struct {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} UsbEndpointDescriptor;

typedef struct {
    UsbInterfaceDescriptor interface_descriptor;
    UsbEndpointDescriptor bulk_in;
    UsbEndpointDescriptor bulk_out;
    UsbEndpointDescriptor interrupt_in;
    const uint8_t *class_descriptor;
    size_t class_descriptor_length;
    int has_bulk_in;
    int has_bulk_out;
    int has_interrupt_in;
} UsbCcidInterface;

int usb_parse_device_descriptor(const uint8_t *bytes, size_t length, UsbDeviceDescriptor *descriptor_out);
int usb_parse_configuration_descriptor(const uint8_t *bytes, size_t length, UsbConfigurationDescriptor *descriptor_out);
int usb_parse_interface_descriptor(const uint8_t *bytes, size_t length, UsbInterfaceDescriptor *descriptor_out);
int usb_parse_endpoint_descriptor(const uint8_t *bytes, size_t length, UsbEndpointDescriptor *descriptor_out);
int usb_find_ccid_interface(const uint8_t *configuration, size_t configuration_length, UsbCcidInterface *interface_out);
const char *usb_descriptor_type_name(uint8_t descriptor_type);
const char *usb_endpoint_transfer_type_name(uint8_t attributes);

#endif