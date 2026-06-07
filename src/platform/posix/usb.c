#include "platform.h"

#include "runtime.h"

int platform_usb_list_devices(PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_out) {
    if (entries_out == 0 && entry_capacity != 0U) return -1;
    if (count_out == 0) return -1;
    *count_out = 0U;
    return 0;
}

int platform_usb_open(const PlatformUsbDevice *device, PlatformUsbHandle *handle_out) {
    if (device == 0 || handle_out == 0) return -1;
    rt_memset(handle_out, 0, sizeof(*handle_out));
    return -1;
}

int platform_usb_close(PlatformUsbHandle *handle) {
    if (handle != 0) rt_memset(handle, 0, sizeof(*handle));
    return 0;
}

int platform_usb_claim_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    (void)interface_number;
    return handle != 0 && handle->active ? -1 : -1;
}

int platform_usb_release_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    (void)interface_number;
    return handle != 0 && handle->active ? -1 : 0;
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
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout_milliseconds;
    if (transferred_out != 0) *transferred_out = 0U;
    return handle != 0 && handle->active ? -1 : -1;
}

int platform_usb_bulk_transfer(PlatformUsbHandle *handle, unsigned int endpoint, unsigned char *data, size_t length, unsigned int timeout_milliseconds, size_t *transferred_out) {
    (void)endpoint;
    (void)data;
    (void)length;
    (void)timeout_milliseconds;
    if (transferred_out != 0) *transferred_out = 0U;
    return handle != 0 && handle->active ? -1 : -1;
}

int platform_usb_read_configuration_descriptor(PlatformUsbHandle *handle, unsigned int configuration_index, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    (void)configuration_index;
    (void)buffer;
    (void)buffer_size;
    if (length_out != 0) *length_out = 0U;
    return handle != 0 && handle->active ? -1 : -1;
}