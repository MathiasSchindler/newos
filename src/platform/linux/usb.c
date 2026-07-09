#include "common.h"
#include "usb_descriptor.h"

#define LINUX_USB_ROOT "/dev/bus/usb"
#define LINUX_USB_SYSFS_ROOT "/sys/bus/usb/devices"
#define LINUX_O_RDWR 2
#define LINUX_SEEK_SET 0
#define LINUX_USB_DESCRIPTOR_STREAM_CAPACITY 4096U

#define LINUX_IOC_NRBITS 8U
#define LINUX_IOC_TYPEBITS 8U
#define LINUX_IOC_SIZEBITS 14U
#define LINUX_IOC_NRSHIFT 0U
#define LINUX_IOC_TYPESHIFT (LINUX_IOC_NRSHIFT + LINUX_IOC_NRBITS)
#define LINUX_IOC_SIZESHIFT (LINUX_IOC_TYPESHIFT + LINUX_IOC_TYPEBITS)
#define LINUX_IOC_DIRSHIFT (LINUX_IOC_SIZESHIFT + LINUX_IOC_SIZEBITS)
#define LINUX_IOC_WRITE 1U
#define LINUX_IOC_READ 2U
#define LINUX_IOC(dir, type, nr, size) (((unsigned long)(dir) << LINUX_IOC_DIRSHIFT) | ((unsigned long)(type) << LINUX_IOC_TYPESHIFT) | ((unsigned long)(nr) << LINUX_IOC_NRSHIFT) | ((unsigned long)(size) << LINUX_IOC_SIZESHIFT))

struct linux_usbdevfs_ctrltransfer {
    unsigned char request_type;
    unsigned char request;
    unsigned short value;
    unsigned short index;
    unsigned short length;
    unsigned int timeout;
    void *data;
};

struct linux_usbdevfs_bulktransfer {
    unsigned int endpoint;
    unsigned int length;
    unsigned int timeout;
    void *data;
};

#define LINUX_USBDEVFS_CONTROL LINUX_IOC(LINUX_IOC_READ | LINUX_IOC_WRITE, 'U', 0U, sizeof(struct linux_usbdevfs_ctrltransfer))
#define LINUX_USBDEVFS_BULK LINUX_IOC(LINUX_IOC_READ | LINUX_IOC_WRITE, 'U', 2U, sizeof(struct linux_usbdevfs_bulktransfer))
#define LINUX_USBDEVFS_CLAIMINTERFACE LINUX_IOC(LINUX_IOC_READ, 'U', 15U, sizeof(unsigned int))
#define LINUX_USBDEVFS_RELEASEINTERFACE LINUX_IOC(LINUX_IOC_READ, 'U', 16U, sizeof(unsigned int))

static int platform_usb_control_transfer_fd(int fd,
                                            unsigned int request_type,
                                            unsigned int request,
                                            unsigned int value,
                                            unsigned int index,
                                            unsigned char *data,
                                            size_t length,
                                            unsigned int timeout_milliseconds,
                                            size_t *transferred_out);

static int linux_usb_is_decimal_name(const char *text) {
    size_t index = 0U;
    if (text == 0 || text[0] == '\0' || text[0] == '.') return 0;
    while (text[index] != '\0') {
        if (text[index] < '0' || text[index] > '9') return 0;
        index += 1U;
    }
    return 1;
}

static void linux_usb_trim_text(char *text) {
    size_t start = 0U;
    size_t end;
    size_t index;

    if (text == 0) return;
    end = rt_strlen(text);
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) start += 1U;
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t' || text[end - 1U] == '\r' || text[end - 1U] == '\n')) end -= 1U;
    for (index = start; index < end; ++index) text[index - start] = text[index];
    text[end - start] = '\0';
}

static int linux_usb_read_text_file(const char *path, char *buffer, size_t buffer_size) {
    long fd;
    long bytes;

    if (path == 0 || buffer == 0 || buffer_size < 2U) return -1;
    buffer[0] = '\0';
    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, LINUX_O_RDONLY | LINUX_O_CLOEXEC, 0);
    if (fd < 0) return -1;
    do {
        bytes = linux_syscall3(LINUX_SYS_READ, fd, (long)buffer, (long)(buffer_size - 1U));
    } while (bytes == -LINUX_EINTR);
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
    if (bytes <= 0) return -1;
    buffer[(size_t)bytes] = '\0';
    linux_usb_trim_text(buffer);
    return buffer[0] == '\0' ? -1 : 0;
}

static int linux_usb_read_attribute(const char *device_path, const char *name, char *buffer, size_t buffer_size) {
    char path[PLATFORM_USB_PATH_CAPACITY];

    if (rt_join_path(device_path, name, path, sizeof(path)) != 0) return -1;
    return linux_usb_read_text_file(path, buffer, buffer_size);
}

static int linux_usb_read_uint_attribute(const char *device_path, const char *name, unsigned int *value_out) {
    char text[32];
    unsigned long long value;

    if (value_out == 0 || linux_usb_read_attribute(device_path, name, text, sizeof(text)) != 0 || rt_parse_uint(text, &value) != 0 || value > 0xffffffffULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static void linux_usb_read_driver(const char *device_path, char *driver, size_t driver_size) {
    char path[PLATFORM_USB_PATH_CAPACITY];
    char target[PLATFORM_USB_PATH_CAPACITY];
    long length;
    size_t start = 0U;
    size_t index;

    if (driver == 0 || driver_size == 0U) return;
    driver[0] = '\0';
    if (rt_join_path(device_path, "driver", path, sizeof(path)) != 0) return;
    length = linux_syscall4(LINUX_SYS_READLINKAT, LINUX_AT_FDCWD, (long)path, (long)target, (long)(sizeof(target) - 1U));
    if (length <= 0 || (size_t)length >= sizeof(target)) return;
    target[(size_t)length] = '\0';
    for (index = 0U; index < (size_t)length; ++index) {
        if (target[index] == '/') start = index + 1U;
    }
    rt_copy_string(driver, driver_size, target + start);
}

static void linux_usb_enrich_device(PlatformUsbDevice *device) {
    char entries[4096];
    long fd;

    if (device == 0) return;
    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)LINUX_USB_SYSFS_ROOT, LINUX_O_RDONLY | LINUX_O_DIRECTORY | LINUX_O_CLOEXEC, 0);
    if (fd < 0) return;
    for (;;) {
        long bytes = linux_syscall3(LINUX_SYS_GETDENTS64, fd, (long)entries, sizeof(entries));
        long offset = 0;

        if (bytes <= 0) break;
        while (offset < bytes) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(void *)(entries + offset);
            char device_path[PLATFORM_USB_PATH_CAPACITY];
            unsigned int bus_number;
            unsigned int device_address;

            if (entry->d_reclen == 0) break;
            if (entry->d_name[0] != '.' && rt_join_path(LINUX_USB_SYSFS_ROOT, entry->d_name, device_path, sizeof(device_path)) == 0 &&
                linux_usb_read_uint_attribute(device_path, "busnum", &bus_number) == 0 &&
                linux_usb_read_uint_attribute(device_path, "devnum", &device_address) == 0 &&
                bus_number == device->bus_number && device_address == device->device_address) {
                rt_copy_string(device->topology, sizeof(device->topology), entry->d_name);
                (void)linux_usb_read_attribute(device_path, "speed", device->speed, sizeof(device->speed));
                (void)linux_usb_read_attribute(device_path, "version", device->usb_version, sizeof(device->usb_version));
                (void)linux_usb_read_attribute(device_path, "bcdDevice", device->device_version, sizeof(device->device_version));
                (void)linux_usb_read_attribute(device_path, "manufacturer", device->manufacturer, sizeof(device->manufacturer));
                (void)linux_usb_read_attribute(device_path, "product", device->product, sizeof(device->product));
                (void)linux_usb_read_attribute(device_path, "serial", device->serial, sizeof(device->serial));
                if (linux_usb_read_uint_attribute(device_path, "bConfigurationValue", &device->active_configuration) == 0) device->has_active_configuration = 1;
                if (linux_usb_read_uint_attribute(device_path, "authorized", &device->authorized) == 0) device->has_authorized = 1;
                linux_usb_read_driver(device_path, device->driver, sizeof(device->driver));
                (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
                return;
            }
            offset += entry->d_reclen;
        }
    }
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
}

static int linux_usb_open_device_path(const char *path, int flags) {
    long fd;
    if (path == 0) return -1;
    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, flags | LINUX_O_CLOEXEC, 0);
    return fd < 0 ? -1 : (int)fd;
}

static int linux_usb_read_descriptor_stream_fd(int fd, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    size_t used = 0U;

    if (length_out != 0) *length_out = 0U;
    if (fd < 0 || buffer == 0 || buffer_size == 0U || length_out == 0) return -1;
    if (linux_syscall3(LINUX_SYS_LSEEK, fd, 0, LINUX_SEEK_SET) < 0) return -1;
    while (used < buffer_size) {
        long bytes = linux_syscall3(LINUX_SYS_READ, fd, (long)(buffer + used), (long)(buffer_size - used));
        if (bytes == -LINUX_EINTR) continue;
        if (bytes < 0) return -1;
        if (bytes == 0) break;
        used += (size_t)bytes;
    }
    *length_out = used;
    return 0;
}

static int linux_usb_get_device_descriptor_fd(int fd, UsbDeviceDescriptor *descriptor_out) {
    unsigned char descriptor_bytes[18];
    size_t descriptor_length = 0U;

    if (linux_usb_read_descriptor_stream_fd(fd, descriptor_bytes, sizeof(descriptor_bytes), &descriptor_length) != 0 || descriptor_length != sizeof(descriptor_bytes)) return -1;
    return usb_parse_device_descriptor(descriptor_bytes, sizeof(descriptor_bytes), descriptor_out);
}

static int linux_usb_add_device(const char *path, unsigned int bus_number, unsigned int device_address, PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_io) {
    int fd;
    UsbDeviceDescriptor descriptor;

    if (path == 0 || count_io == 0) return -1;
    fd = linux_usb_open_device_path(path, LINUX_O_RDONLY);
    if (fd < 0) return 0;
    if (linux_usb_get_device_descriptor_fd(fd, &descriptor) == 0) {
        if (*count_io < entry_capacity) {
            PlatformUsbDevice *entry = &entries_out[*count_io];
            rt_memset(entry, 0, sizeof(*entry));
            rt_copy_string(entry->path, sizeof(entry->path), path);
            entry->bus_number = bus_number;
            entry->device_address = device_address;
            entry->vendor_id = descriptor.vendor_id;
            entry->product_id = descriptor.product_id;
            entry->device_class = descriptor.device_class;
            entry->device_subclass = descriptor.device_subclass;
            entry->device_protocol = descriptor.device_protocol;
            entry->configuration_count = descriptor.configuration_count;
            linux_usb_enrich_device(entry);
        }
        *count_io += 1U;
    }
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
    return 0;
}

static int linux_usb_scan_bus(const char *bus_path, unsigned int bus_number, PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_io) {
    char buffer[4096];
    long fd;

    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)bus_path, LINUX_O_RDONLY | LINUX_O_DIRECTORY | LINUX_O_CLOEXEC, 0);
    if (fd < 0) return 0;
    for (;;) {
        long bytes = linux_syscall3(LINUX_SYS_GETDENTS64, fd, (long)buffer, sizeof(buffer));
        long offset = 0;
        if (bytes < 0) {
            (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
            return -1;
        }
        if (bytes == 0) break;
        while (offset < bytes) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(void *)(buffer + offset);
            if (entry->d_reclen == 0) {
                (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
                return -1;
            }
            if (linux_usb_is_decimal_name(entry->d_name)) {
                unsigned long long device_address = 0U;
                char device_path[PLATFORM_USB_PATH_CAPACITY];
                if (rt_parse_uint(entry->d_name, &device_address) == 0 && device_address <= 255ULL && rt_join_path(bus_path, entry->d_name, device_path, sizeof(device_path)) == 0) {
                    (void)linux_usb_add_device(device_path, bus_number, (unsigned int)device_address, entries_out, entry_capacity, count_io);
                }
            }
            offset += entry->d_reclen;
        }
    }
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
    return 0;
}

static int platform_usb_control_transfer_fd(int fd,
                                            unsigned int request_type,
                                            unsigned int request,
                                            unsigned int value,
                                            unsigned int index,
                                            unsigned char *data,
                                            size_t length,
                                            unsigned int timeout_milliseconds,
                                            size_t *transferred_out) {
    struct linux_usbdevfs_ctrltransfer transfer;
    long result;

    if (transferred_out != 0) *transferred_out = 0U;
    if (fd < 0 || length > 65535U || (length > 0U && data == 0)) return -1;
    rt_memset(&transfer, 0, sizeof(transfer));
    transfer.request_type = (unsigned char)request_type;
    transfer.request = (unsigned char)request;
    transfer.value = (unsigned short)value;
    transfer.index = (unsigned short)index;
    transfer.length = (unsigned short)length;
    transfer.timeout = timeout_milliseconds;
    transfer.data = data;
    result = linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_USBDEVFS_CONTROL, (long)&transfer);
    if (result < 0) return -1;
    if (transferred_out != 0) *transferred_out = (size_t)result;
    return 0;
}

int platform_usb_list_devices(PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_out) {
    char buffer[4096];
    long fd;
    size_t count = 0U;

    if (count_out == 0 || (entries_out == 0 && entry_capacity != 0U)) return -1;
    *count_out = 0U;
    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)LINUX_USB_ROOT, LINUX_O_RDONLY | LINUX_O_DIRECTORY | LINUX_O_CLOEXEC, 0);
    if (fd < 0) return -1;
    for (;;) {
        long bytes = linux_syscall3(LINUX_SYS_GETDENTS64, fd, (long)buffer, sizeof(buffer));
        long offset = 0;
        if (bytes < 0) {
            (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
            return -1;
        }
        if (bytes == 0) break;
        while (offset < bytes) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(void *)(buffer + offset);
            if (entry->d_reclen == 0) {
                (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
                return -1;
            }
            if (linux_usb_is_decimal_name(entry->d_name)) {
                unsigned long long bus_number = 0U;
                char bus_path[PLATFORM_USB_PATH_CAPACITY];
                if (rt_parse_uint(entry->d_name, &bus_number) == 0 && bus_number <= 255ULL && rt_join_path(LINUX_USB_ROOT, entry->d_name, bus_path, sizeof(bus_path)) == 0) {
                    if (linux_usb_scan_bus(bus_path, (unsigned int)bus_number, entries_out, entry_capacity, &count) != 0) {
                        (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
                        return -1;
                    }
                }
            }
            offset += entry->d_reclen;
        }
    }
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
    *count_out = count < entry_capacity ? count : entry_capacity;
    return 0;
}

int platform_usb_open(const PlatformUsbDevice *device, PlatformUsbHandle *handle_out) {
    int fd;

    if (device == 0 || handle_out == 0) return -1;
    rt_memset(handle_out, 0, sizeof(*handle_out));
    fd = linux_usb_open_device_path(device->path, LINUX_O_RDWR);
    if (fd < 0) fd = linux_usb_open_device_path(device->path, LINUX_O_RDONLY);
    if (fd < 0) return -1;
    handle_out->handle = (unsigned long long)(unsigned int)fd;
    handle_out->active = 1;
    return 0;
}

int platform_usb_close(PlatformUsbHandle *handle) {
    int fd;

    if (handle == 0 || !handle->active) return 0;
    if (handle->claimed) (void)platform_usb_release_interface(handle, handle->claimed_interface);
    fd = (int)(unsigned int)handle->handle;
    (void)linux_syscall1(LINUX_SYS_CLOSE, fd);
    rt_memset(handle, 0, sizeof(*handle));
    return 0;
}

int platform_usb_claim_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    unsigned int value = interface_number;
    int fd;

    if (handle == 0 || !handle->active || interface_number > 255U) return -1;
    fd = (int)(unsigned int)handle->handle;
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_USBDEVFS_CLAIMINTERFACE, (long)&value) < 0) return -1;
    handle->claimed_interface = interface_number;
    handle->claimed = 1;
    return 0;
}

int platform_usb_release_interface(PlatformUsbHandle *handle, unsigned int interface_number) {
    unsigned int value = interface_number;
    int fd;

    if (handle == 0 || !handle->active || interface_number > 255U) return -1;
    fd = (int)(unsigned int)handle->handle;
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_USBDEVFS_RELEASEINTERFACE, (long)&value) < 0) return -1;
    if (handle->claimed_interface == interface_number) handle->claimed = 0;
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
    if (handle == 0 || !handle->active) return -1;
    return platform_usb_control_transfer_fd((int)(unsigned int)handle->handle, request_type, request, value, index, data, length, timeout_milliseconds, transferred_out);
}

int platform_usb_bulk_transfer(PlatformUsbHandle *handle, unsigned int endpoint, unsigned char *data, size_t length, unsigned int timeout_milliseconds, size_t *transferred_out) {
    struct linux_usbdevfs_bulktransfer transfer;
    long result;
    int fd;

    if (transferred_out != 0) *transferred_out = 0U;
    if (handle == 0 || !handle->active || endpoint > 255U || length > 65535U || (length > 0U && data == 0)) return -1;
    fd = (int)(unsigned int)handle->handle;
    rt_memset(&transfer, 0, sizeof(transfer));
    transfer.endpoint = endpoint;
    transfer.length = (unsigned int)length;
    transfer.timeout = timeout_milliseconds;
    transfer.data = data;
    result = linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_USBDEVFS_BULK, (long)&transfer);
    if (result < 0) return -1;
    if (transferred_out != 0) *transferred_out = (size_t)result;
    return 0;
}

int platform_usb_read_configuration_descriptor(PlatformUsbHandle *handle, unsigned int configuration_index, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    unsigned char descriptors[LINUX_USB_DESCRIPTOR_STREAM_CAPACITY];
    size_t descriptors_length = 0U;
    size_t offset = 0U;
    unsigned int current_configuration = 0U;

    if (length_out != 0) *length_out = 0U;
    if (handle == 0 || !handle->active || buffer == 0 || buffer_size == 0U || configuration_index > 255U || length_out == 0) return -1;
    if (linux_usb_read_descriptor_stream_fd((int)(unsigned int)handle->handle, descriptors, sizeof(descriptors), &descriptors_length) != 0) return -1;
    while (offset + 2U <= descriptors_length) {
        size_t descriptor_length = descriptors[offset];
        unsigned int descriptor_type = descriptors[offset + 1U];

        if (descriptor_length < 2U || offset + descriptor_length > descriptors_length) return -1;
        if (descriptor_type == USB_DESCRIPTOR_TYPE_CONFIGURATION) {
            UsbConfigurationDescriptor descriptor;
            size_t available;
            size_t wanted;

            if (usb_parse_configuration_descriptor(descriptors + offset, descriptors_length - offset, &descriptor) != 0) return -1;
            if (current_configuration != configuration_index) {
                current_configuration += 1U;
                offset += descriptor_length;
                continue;
            }
            available = descriptors_length - offset;
            wanted = descriptor.total_length;
            if (wanted > available) wanted = available;
            if (wanted > buffer_size) wanted = buffer_size;
            if (wanted < 9U) return -1;
            memcpy(buffer, descriptors + offset, wanted);
            *length_out = wanted;
            return 0;
        }
        offset += descriptor_length;
    }
    return -1;
}