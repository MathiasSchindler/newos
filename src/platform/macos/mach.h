#ifndef NEWOS_PLATFORM_MACOS_MACH_H
#define NEWOS_PLATFORM_MACOS_MACH_H

#include <stdint.h>

#define MACOS_MACH_PORT_NULL 0U
#define MACOS_MACH_MSG_SUCCESS 0
#define MACOS_KERN_SUCCESS 0
#define MACOS_KIORETURN_NO_DEVICE 0xe00002c0U

#define MACOS_MACH_SEND_MSG 0x00000001U
#define MACOS_MACH_RCV_MSG 0x00000002U
#define MACOS_MACH_RCV_GUARDED_DESC 0x00001000U
#define MACOS_MACH_MSG_TIMEOUT_NONE 0U

#define MACOS_MACH_MSGH_BITS_COMPLEX 0x80000000U
#define MACOS_MACH_MSG_TYPE_COPY_SEND 19U
#define MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE 21U
#define MACOS_MACH_MSG_TYPE_MOVE_SEND 17U
#define MACOS_MACH_MSG_PORT_DESCRIPTOR 0U
#define MACOS_MACH_MSG_OOL_DESCRIPTOR 1U
#define MACOS_MACH_MSG_PHYSICAL_COPY 0U

#define MACOS_MACH_HOST_GET_IO_MAIN_ID 205
#define MACOS_IOKIT_OBJECT_GET_CLASS_ID 2800
#define MACOS_IOKIT_ITERATOR_NEXT_ID 2802
#define MACOS_IOKIT_REGISTRY_ENTRY_GET_NAME_ID 2810
#define MACOS_IOKIT_REGISTRY_ENTRY_GET_PROPERTY_BYTES_ID 2812
#define MACOS_IOKIT_REGISTRY_ENTRY_GET_CHILD_ITERATOR_ID 2813
#define MACOS_IOKIT_SERVICE_CLOSE_ID 2816
#define MACOS_IOKIT_REGISTRY_GET_ROOT_ENTRY_ID 2827
#define MACOS_IOKIT_SERVICE_OPEN_EXTENDED_ID 2862
#define MACOS_IOKIT_CONNECT_METHOD_ID 2865

typedef uint32_t MacosMachPort;

typedef struct {
    uint32_t bits;
    uint32_t size;
    MacosMachPort remote_port;
    MacosMachPort local_port;
    MacosMachPort voucher_port;
    int32_t id;
} MacosMachMsgHeader;

typedef struct {
    uint32_t descriptor_count;
} MacosMachMsgBody;

typedef struct {
    MacosMachPort name;
    uint32_t pad1;
    uint16_t pad2;
    uint8_t disposition;
    uint8_t type;
} MacosMachMsgPortDescriptor;

typedef struct {
    uint64_t address;
    uint8_t deallocate;
    uint8_t copy;
    uint8_t pad1;
    uint8_t type;
    uint32_t size;
} MacosMachMsgOolDescriptor;

typedef struct {
    uint8_t mig_vers;
    uint8_t if_vers;
    uint8_t reserved1;
    uint8_t mig_encoding;
    uint8_t int_rep;
    uint8_t char_rep;
    uint8_t float_rep;
    uint8_t reserved2;
} MacosMachNdrRecord;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachNdrRecord ndr;
    int32_t result;
} MacosMachMigErrorReply;

static inline uint32_t macos_mach_msg_bits(uint32_t remote, uint32_t local) {
    return remote | (local << 8U);
}

MacosMachPort macos_mach_host_self(void);
MacosMachPort macos_mach_task_self(void);
MacosMachPort macos_mach_special_reply_port(void);
int macos_mach_msg(MacosMachMsgHeader *message, uint32_t option, uint32_t send_size, uint32_t receive_size, MacosMachPort receive_name, uint32_t timeout, MacosMachPort notify);
int macos_iokit_get_main_port(MacosMachPort *port_out);
int macos_iokit_get_root_entry(MacosMachPort main_port, MacosMachPort *root_out);
int macos_iokit_service_open_extended(MacosMachPort service, uint32_t connect_type, MacosMachPort *connection_out);
int macos_iokit_service_close(MacosMachPort connection);
int macos_iokit_connect_method(MacosMachPort connection,
                               uint32_t selector,
                               const uint64_t *scalar_input,
                               uint32_t scalar_input_count,
                               uint64_t *scalar_output,
                               uint32_t *scalar_output_count);
int macos_iokit_object_get_class(MacosMachPort object, char *class_out, uint32_t class_out_size);
int macos_iokit_registry_entry_get_name(MacosMachPort entry, char *name_out, uint32_t name_out_size);
int macos_iokit_registry_entry_get_property_bytes(MacosMachPort entry, const char *property_name, unsigned char *data_out, uint32_t data_capacity, uint32_t *data_length_out);
int macos_iokit_registry_entry_get_child_iterator(MacosMachPort entry, const char *plane, MacosMachPort *iterator_out);
int macos_iokit_iterator_next(MacosMachPort iterator, MacosMachPort *object_out);

#endif