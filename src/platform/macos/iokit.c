#include "mach.h"

#include "runtime.h"

#define MACOS_MACH_TRAP_HOST_SELF (-29)
#define MACOS_MACH_TRAP_MACH_MSG (-31)
#define MACOS_MACH_TRAP_MACH_MSG2 (-47)
#define MACOS_MACH_TRAP_THREAD_GET_SPECIAL_REPLY_PORT (-50)

typedef struct {
    MacosMachMsgHeader header;
} MacosIokitHostGetMainRequest;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachMsgBody body;
    MacosMachMsgPortDescriptor owning_task;
    MacosMachMsgOolDescriptor properties;
    MacosMachNdrRecord ndr;
    uint32_t connect_type;
    MacosMachNdrRecord properties_ndr;
    uint32_t properties_count;
} MacosIokitOpenExtendedRequest;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachMsgBody body;
    MacosMachMsgPortDescriptor connection;
    MacosMachNdrRecord ndr;
    int32_t result;
} MacosIokitOpenExtendedReply;

typedef union {
    MacosMachMsgHeader header;
    MacosIokitOpenExtendedReply success;
    MacosMachMigErrorReply error;
    unsigned char bytes[128];
} MacosIokitOpenExtendedBuffer;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachNdrRecord ndr;
    int32_t result;
} MacosIokitSimpleResultReply;

typedef union {
    MacosMachMsgHeader header;
    MacosIokitSimpleResultReply success;
    MacosMachMigErrorReply error;
    unsigned char bytes[128];
} MacosIokitSimpleResultBuffer;

typedef union {
    MacosMachMsgHeader header;
    unsigned char bytes[4608];
} MacosIokitConnectMethodBuffer;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachNdrRecord ndr;
    uint32_t string_offset;
    uint32_t string_count;
    char string[128];
} MacosIokitNameRequest;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachNdrRecord ndr;
    uint32_t result;
    uint32_t data_count;
    unsigned char data[4096];
} MacosIokitPropertyBytesReply;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachMsgBody body;
    MacosMachMsgPortDescriptor io_main;
} MacosIokitHostGetMainReply;

typedef union {
    MacosMachMsgHeader header;
    MacosIokitHostGetMainReply success;
    MacosMachMigErrorReply error;
    unsigned char bytes[128];
} MacosIokitHostGetMainReplyBuffer;

typedef struct {
    MacosMachMsgHeader header;
    MacosMachNdrRecord ndr;
    int32_t result;
    uint32_t class_name_offset;
    uint32_t class_name_count;
    char class_name[128];
} MacosIokitObjectGetClassReply;

typedef union {
    MacosMachMsgHeader header;
    MacosIokitObjectGetClassReply success;
    MacosMachMigErrorReply error;
    unsigned char bytes[256];
} MacosIokitObjectGetClassReplyBuffer;

typedef union {
    MacosMachMsgHeader header;
    MacosIokitPropertyBytesReply success;
    MacosMachMigErrorReply error;
    unsigned char bytes[4352];
} MacosIokitPropertyBytesBuffer;

static MacosMachNdrRecord macos_mach_ndr_record(void) {
    MacosMachNdrRecord ndr;
    ndr.mig_vers = 0;
    ndr.if_vers = 0;
    ndr.reserved1 = 0;
    ndr.mig_encoding = 0;
    ndr.int_rep = 1;
    ndr.char_rep = 0;
    ndr.float_rep = 0;
    ndr.reserved2 = 0;
    return ndr;
}

static uint32_t macos_mig_word_align(uint32_t value) {
    return (value + 3U) & ~3U;
}

static uint32_t macos_mig_copy_name(char *destination, const char *source) {
    uint32_t index;

    if (destination == 0) return 0;
    if (source == 0) {
        destination[0] = 0;
        return 1;
    }
    for (index = 0; index < 127U && source[index] != 0; index++) destination[index] = source[index];
    destination[index] = 0;
    return index + 1U;
}

MacosMachPort macos_mach_host_self(void) {
    register long x16 __asm__("x16") = MACOS_MACH_TRAP_HOST_SELF;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(x0), "+r"(x16) : : "memory", "cc");
    return (MacosMachPort)x0;
}

MacosMachPort macos_mach_task_self(void) {
    register long x16 __asm__("x16") = -28;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(x0), "+r"(x16) : : "memory", "cc");
    return (MacosMachPort)x0;
}

MacosMachPort macos_mach_special_reply_port(void) {
    register long x16 __asm__("x16") = MACOS_MACH_TRAP_THREAD_GET_SPECIAL_REPLY_PORT;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(x0), "+r"(x16) : : "memory", "cc");
    return (MacosMachPort)x0;
}

int macos_mach_msg(MacosMachMsgHeader *message, uint32_t option, uint32_t send_size, uint32_t receive_size, MacosMachPort receive_name, uint32_t timeout, MacosMachPort notify) {
    register long x16 __asm__("x16") = MACOS_MACH_TRAP_MACH_MSG;
    register long x0 __asm__("x0") = (long)message;
    register long x1 __asm__("x1") = (long)option;
    register long x2 __asm__("x2") = (long)send_size;
    register long x3 __asm__("x3") = (long)receive_size;
    register long x4 __asm__("x4") = (long)receive_name;
    register long x5 __asm__("x5") = (long)timeout;
    register long x6 __asm__("x6") = (long)notify;

    __asm__ volatile("svc #0x80" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x16) : : "memory", "cc");
    return (int)x0;
}

static int macos_mach_msg2(MacosMachMsgHeader *message, uint64_t option64, uint64_t bits_and_send_size, uint64_t remote_and_local_port, uint64_t voucher_and_id, uint64_t descriptor_count_and_receive_name, uint64_t receive_size_and_priority, uint64_t timeout) {
    register long x16 __asm__("x16") = MACOS_MACH_TRAP_MACH_MSG2;
    register long x0 __asm__("x0") = (long)message;
    register long x1 __asm__("x1") = (long)option64;
    register long x2 __asm__("x2") = (long)bits_and_send_size;
    register long x3 __asm__("x3") = (long)remote_and_local_port;
    register long x4 __asm__("x4") = (long)voucher_and_id;
    register long x5 __asm__("x5") = (long)descriptor_count_and_receive_name;
    register long x6 __asm__("x6") = (long)receive_size_and_priority;
    register long x7 __asm__("x7") = (long)timeout;

    __asm__ volatile("svc #0x80" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7), "+r"(x16) : : "memory", "cc");
    return (int)x0;
}

static int macos_iokit_call_port_reply(MacosMachPort remote_port, int32_t message_id, MacosMachPort *port_out) {
    MacosIokitHostGetMainReplyBuffer reply;
    MacosMachPort reply_port;
    uint32_t bits;
    int result;

    if (port_out == 0) return -1;
    *port_out = MACOS_MACH_PORT_NULL;
    rt_memset(&reply, 0, sizeof(reply));

    reply_port = macos_mach_special_reply_port();
    if (remote_port == MACOS_MACH_PORT_NULL || reply_port == MACOS_MACH_PORT_NULL) return -1;

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    reply.header.bits = bits;
    reply.header.size = (uint32_t)sizeof(MacosIokitHostGetMainRequest);
    reply.header.remote_port = remote_port;
    reply.header.local_port = reply_port;
    reply.header.voucher_port = MACOS_MACH_PORT_NULL;
    reply.header.id = message_id;

    result = macos_mach_msg2(&reply.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)sizeof(MacosIokitHostGetMainRequest) << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)remote_port,
                             ((uint64_t)(uint32_t)message_id << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             48U,
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;

    if ((reply.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U &&
        reply.header.id == message_id + 100 &&
        reply.header.size >= sizeof(MacosIokitHostGetMainReply) &&
        reply.success.body.descriptor_count == 1U &&
        reply.success.io_main.disposition == MACOS_MACH_MSG_TYPE_MOVE_SEND &&
        reply.success.io_main.type == MACOS_MACH_MSG_PORT_DESCRIPTOR) {
        *port_out = reply.success.io_main.name;
        return MACOS_KERN_SUCCESS;
    }

    if (reply.header.size >= sizeof(MacosMachMigErrorReply) && reply.error.result != MACOS_KERN_SUCCESS) {
        return reply.error.result;
    }
    return -1;
}

int macos_iokit_get_main_port(MacosMachPort *port_out) {
    return macos_iokit_call_port_reply(macos_mach_host_self(), MACOS_MACH_HOST_GET_IO_MAIN_ID, port_out);
}

int macos_iokit_get_root_entry(MacosMachPort main_port, MacosMachPort *root_out) {
    return macos_iokit_call_port_reply(main_port, MACOS_IOKIT_REGISTRY_GET_ROOT_ENTRY_ID, root_out);
}

int macos_iokit_service_open_extended(MacosMachPort service, uint32_t connect_type, MacosMachPort *connection_out) {
    MacosIokitOpenExtendedBuffer message;
    MacosMachPort reply_port;
    MacosMachPort task_port;
    uint32_t bits;
    uint32_t send_size;
    int result;

    if (connection_out == 0) return -1;
    *connection_out = MACOS_MACH_PORT_NULL;
    if (service == MACOS_MACH_PORT_NULL) return -1;
    rt_memset(&message, 0, sizeof(message));
    reply_port = macos_mach_special_reply_port();
    task_port = macos_mach_task_self();
    if (reply_port == MACOS_MACH_PORT_NULL || task_port == MACOS_MACH_PORT_NULL) return -1;

    send_size = (uint32_t)sizeof(MacosIokitOpenExtendedRequest);
    bits = MACOS_MACH_MSGH_BITS_COMPLEX | macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.success.header.bits = bits;
    message.success.header.size = send_size;
    message.success.header.remote_port = service;
    message.success.header.local_port = reply_port;
    message.success.header.voucher_port = MACOS_MACH_PORT_NULL;
    message.success.header.id = MACOS_IOKIT_SERVICE_OPEN_EXTENDED_ID;
    message.success.body.descriptor_count = 2U;
    message.success.connection.name = task_port;
    message.success.connection.disposition = MACOS_MACH_MSG_TYPE_COPY_SEND;
    message.success.connection.type = MACOS_MACH_MSG_PORT_DESCRIPTOR;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties.address = 0U;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties.deallocate = 0U;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties.copy = MACOS_MACH_MSG_PHYSICAL_COPY;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties.type = MACOS_MACH_MSG_OOL_DESCRIPTOR;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties.size = 0U;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->ndr = macos_mach_ndr_record();
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->connect_type = connect_type;
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties_ndr = macos_mach_ndr_record();
    ((MacosIokitOpenExtendedRequest *)(void *)&message)->properties_count = 0U;

    result = macos_mach_msg2(&message.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)send_size << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)service,
                             ((uint64_t)MACOS_IOKIT_SERVICE_OPEN_EXTENDED_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U) | 2U,
                             (uint64_t)sizeof(message),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;
    if (message.header.size >= sizeof(MacosMachMigErrorReply) && message.error.result != MACOS_KERN_SUCCESS) return message.error.result;
    if ((message.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) == 0U || message.header.id != MACOS_IOKIT_SERVICE_OPEN_EXTENDED_ID + 100) return -1;
    if (message.success.body.descriptor_count != 1U || message.success.connection.type != MACOS_MACH_MSG_PORT_DESCRIPTOR) return -1;
    if (message.success.result != MACOS_KERN_SUCCESS) return message.success.result;
    *connection_out = message.success.connection.name;
    return MACOS_KERN_SUCCESS;
}

int macos_iokit_service_close(MacosMachPort connection) {
    MacosIokitSimpleResultBuffer message;
    MacosMachPort reply_port;
    uint32_t bits;
    int result;

    if (connection == MACOS_MACH_PORT_NULL) return 0;
    rt_memset(&message, 0, sizeof(message));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;
    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.header.bits = bits;
    message.header.size = (uint32_t)sizeof(MacosIokitHostGetMainRequest);
    message.header.remote_port = connection;
    message.header.local_port = reply_port;
    message.header.voucher_port = MACOS_MACH_PORT_NULL;
    message.header.id = MACOS_IOKIT_SERVICE_CLOSE_ID;

    result = macos_mach_msg2(&message.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)sizeof(MacosIokitHostGetMainRequest) << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)connection,
                             ((uint64_t)MACOS_IOKIT_SERVICE_CLOSE_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             (uint64_t)sizeof(message),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;
    if (message.header.id != MACOS_IOKIT_SERVICE_CLOSE_ID + 100) return -1;
    if (message.header.size >= sizeof(MacosMachMigErrorReply) && message.error.result != MACOS_KERN_SUCCESS) return message.error.result;
    if (message.success.result != MACOS_KERN_SUCCESS) return message.success.result;
    return MACOS_KERN_SUCCESS;
}

int macos_iokit_connect_method(MacosMachPort connection,
                               uint32_t selector,
                               const uint64_t *scalar_input,
                               uint32_t scalar_input_count,
                               uint64_t *scalar_output,
                               uint32_t *scalar_output_count) {
    MacosIokitConnectMethodBuffer message;
    MacosMachPort reply_port;
    unsigned char *bytes;
    unsigned char *cursor;
    uint32_t requested_scalar_output_count;
    uint32_t inband_count;
    uint32_t returned_scalar_count;
    uint32_t index;
    uint32_t bits;
    uint32_t send_size;
    int32_t ret_code;
    int result;

    if (connection == MACOS_MACH_PORT_NULL || scalar_input_count > 16U) return -1;
    if (scalar_input_count != 0U && scalar_input == 0) return -1;
    requested_scalar_output_count = scalar_output_count == 0 ? 0U : *scalar_output_count;
    if (requested_scalar_output_count > 16U) requested_scalar_output_count = 16U;
    if (requested_scalar_output_count != 0U && scalar_output == 0) return -1;
    if (scalar_output_count != 0) *scalar_output_count = 0U;

    rt_memset(&message, 0, sizeof(message));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;

    bytes = message.bytes;
    ((MacosMachMsgHeader *)(void *)bytes)->remote_port = connection;
    ((MacosMachMsgHeader *)(void *)bytes)->local_port = reply_port;
    ((MacosMachMsgHeader *)(void *)bytes)->voucher_port = MACOS_MACH_PORT_NULL;
    ((MacosMachMsgHeader *)(void *)bytes)->id = MACOS_IOKIT_CONNECT_METHOD_ID;
    *((MacosMachNdrRecord *)(void *)(bytes + sizeof(MacosMachMsgHeader))) = macos_mach_ndr_record();
    cursor = bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord);
    *(uint32_t *)(void *)cursor = selector;
    cursor += 4U;
    *(uint32_t *)(void *)cursor = scalar_input_count;
    cursor += 4U;
    for (index = 0; index < scalar_input_count; index++) {
        *(uint64_t *)(void *)cursor = scalar_input[index];
        cursor += 8U;
    }
    *(uint32_t *)(void *)cursor = 0U;
    cursor += 4U;
    *(uint64_t *)(void *)cursor = 0U;
    cursor += 8U;
    *(uint64_t *)(void *)cursor = 0U;
    cursor += 8U;
    *(uint32_t *)(void *)cursor = 0U;
    cursor += 4U;
    *(uint32_t *)(void *)cursor = requested_scalar_output_count;
    cursor += 4U;
    *(uint64_t *)(void *)cursor = 0U;
    cursor += 8U;
    *(uint64_t *)(void *)cursor = 0U;
    cursor += 8U;
    send_size = (uint32_t)(cursor - bytes);

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.header.bits = bits;
    message.header.size = send_size;

    result = macos_mach_msg2(&message.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)send_size << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)connection,
                             ((uint64_t)MACOS_IOKIT_CONNECT_METHOD_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             (uint64_t)sizeof(message),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;
    if (message.header.id != MACOS_IOKIT_CONNECT_METHOD_ID + 100) return -1;
    if ((message.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U) return -1;
    if (message.header.size < sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord) + 12U) return -1;
    cursor = message.bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord);
    ret_code = *(int32_t *)(void *)cursor;
    cursor += 4U;
    if (ret_code != MACOS_KERN_SUCCESS) return ret_code;
    inband_count = *(uint32_t *)(void *)cursor;
    cursor += 4U + macos_mig_word_align(inband_count);
    if (cursor + 4U > message.bytes + message.header.size) return -1;
    returned_scalar_count = *(uint32_t *)(void *)cursor;
    cursor += 4U;
    if (returned_scalar_count > 16U || cursor + returned_scalar_count * 8U > message.bytes + message.header.size) return -1;
    if (returned_scalar_count > requested_scalar_output_count) return -1;
    for (index = 0; index < returned_scalar_count; index++) {
        scalar_output[index] = *(uint64_t *)(void *)cursor;
        cursor += 8U;
    }
    if (scalar_output_count != 0) *scalar_output_count = returned_scalar_count;
    return MACOS_KERN_SUCCESS;
}

int macos_iokit_iterator_next(MacosMachPort iterator, MacosMachPort *object_out) {
    int result;

    if (object_out == 0) return -1;
    *object_out = MACOS_MACH_PORT_NULL;
    result = macos_iokit_call_port_reply(iterator, MACOS_IOKIT_ITERATOR_NEXT_ID, object_out);
    if ((uint32_t)result == MACOS_KIORETURN_NO_DEVICE) {
        *object_out = MACOS_MACH_PORT_NULL;
        return MACOS_KERN_SUCCESS;
    }
    return result;
}

int macos_iokit_registry_entry_get_child_iterator(MacosMachPort entry, const char *plane, MacosMachPort *iterator_out) {
    union {
        MacosIokitNameRequest request;
        MacosIokitHostGetMainReplyBuffer reply;
        unsigned char bytes[256];
    } message;
    MacosMachPort reply_port;
    uint32_t bits;
    uint32_t send_size;
    int result;

    if (iterator_out == 0) return -1;
    *iterator_out = MACOS_MACH_PORT_NULL;
    if (entry == MACOS_MACH_PORT_NULL) return -1;

    rt_memset(&message, 0, sizeof(message));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;

    message.request.ndr = macos_mach_ndr_record();
    message.request.string_offset = 0;
    message.request.string_count = macos_mig_copy_name(message.request.string, plane);
    send_size = (uint32_t)(sizeof(message.request) - sizeof(message.request.string)) + macos_mig_word_align(message.request.string_count);

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.request.header.bits = bits;
    message.request.header.size = send_size;
    message.request.header.remote_port = entry;
    message.request.header.local_port = reply_port;
    message.request.header.voucher_port = MACOS_MACH_PORT_NULL;
    message.request.header.id = MACOS_IOKIT_REGISTRY_ENTRY_GET_CHILD_ITERATOR_ID;

    result = macos_mach_msg2(&message.request.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)send_size << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)entry,
                             ((uint64_t)MACOS_IOKIT_REGISTRY_ENTRY_GET_CHILD_ITERATOR_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             48U,
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;

    if ((message.reply.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U &&
        message.reply.header.id == MACOS_IOKIT_REGISTRY_ENTRY_GET_CHILD_ITERATOR_ID + 100 &&
        message.reply.header.size >= sizeof(MacosIokitHostGetMainReply) &&
        message.reply.success.body.descriptor_count == 1U &&
        message.reply.success.io_main.disposition == MACOS_MACH_MSG_TYPE_MOVE_SEND &&
        message.reply.success.io_main.type == MACOS_MACH_MSG_PORT_DESCRIPTOR) {
        *iterator_out = message.reply.success.io_main.name;
        return MACOS_KERN_SUCCESS;
    }

    if (message.reply.header.size >= sizeof(MacosMachMigErrorReply) && message.reply.error.result != MACOS_KERN_SUCCESS) return message.reply.error.result;
    return -1;
}

int macos_iokit_object_get_class(MacosMachPort object, char *class_out, uint32_t class_out_size) {
    MacosIokitObjectGetClassReplyBuffer reply;
    MacosMachPort reply_port;
    uint32_t bits;
    uint32_t index;
    uint32_t copy_count;
    int result;

    if (class_out == 0 || class_out_size == 0U) return -1;
    class_out[0] = 0;
    if (object == MACOS_MACH_PORT_NULL) return -1;

    rt_memset(&reply, 0, sizeof(reply));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    reply.header.bits = bits;
    reply.header.size = (uint32_t)sizeof(MacosIokitHostGetMainRequest);
    reply.header.remote_port = object;
    reply.header.local_port = reply_port;
    reply.header.voucher_port = MACOS_MACH_PORT_NULL;
    reply.header.id = MACOS_IOKIT_OBJECT_GET_CLASS_ID;

    result = macos_mach_msg2(&reply.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)sizeof(MacosIokitHostGetMainRequest) << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)object,
                             ((uint64_t)MACOS_IOKIT_OBJECT_GET_CLASS_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             (uint64_t)sizeof(reply),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;

    if (reply.header.id != MACOS_IOKIT_OBJECT_GET_CLASS_ID + 100) return -1;
    if ((reply.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U) return -1;
    if (reply.header.size >= sizeof(MacosMachMigErrorReply) && reply.error.result != MACOS_KERN_SUCCESS) return reply.error.result;
    if (reply.header.size < (sizeof(MacosIokitObjectGetClassReply) - sizeof(reply.success.class_name))) return -1;
    if (reply.success.class_name_count > sizeof(reply.success.class_name)) return -1;

    copy_count = reply.success.class_name_count;
    if (copy_count >= class_out_size) copy_count = class_out_size - 1U;
    for (index = 0; index < copy_count; index++) class_out[index] = reply.success.class_name[index];
    class_out[copy_count] = 0;
    return MACOS_KERN_SUCCESS;
}

int macos_iokit_registry_entry_get_name(MacosMachPort entry, char *name_out, uint32_t name_out_size) {
    MacosIokitObjectGetClassReplyBuffer reply;
    MacosMachPort reply_port;
    uint32_t bits;
    uint32_t index;
    uint32_t copy_count;
    int result;

    if (name_out == 0 || name_out_size == 0U) return -1;
    name_out[0] = 0;
    if (entry == MACOS_MACH_PORT_NULL) return -1;

    rt_memset(&reply, 0, sizeof(reply));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    reply.header.bits = bits;
    reply.header.size = (uint32_t)sizeof(MacosIokitHostGetMainRequest);
    reply.header.remote_port = entry;
    reply.header.local_port = reply_port;
    reply.header.voucher_port = MACOS_MACH_PORT_NULL;
    reply.header.id = MACOS_IOKIT_REGISTRY_ENTRY_GET_NAME_ID;

    result = macos_mach_msg2(&reply.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)sizeof(MacosIokitHostGetMainRequest) << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)entry,
                             ((uint64_t)MACOS_IOKIT_REGISTRY_ENTRY_GET_NAME_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             (uint64_t)sizeof(reply),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;

    if (reply.header.id != MACOS_IOKIT_REGISTRY_ENTRY_GET_NAME_ID + 100) return -1;
    if ((reply.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U) return -1;
    if (reply.header.size >= sizeof(MacosMachMigErrorReply) && reply.error.result != MACOS_KERN_SUCCESS) return reply.error.result;
    if (reply.header.size < (sizeof(MacosIokitObjectGetClassReply) - sizeof(reply.success.class_name))) return -1;
    if (reply.success.class_name_count > sizeof(reply.success.class_name)) return -1;

    copy_count = reply.success.class_name_count;
    if (copy_count >= name_out_size) copy_count = name_out_size - 1U;
    for (index = 0; index < copy_count; index++) name_out[index] = reply.success.class_name[index];
    name_out[copy_count] = 0;
    return MACOS_KERN_SUCCESS;
}

int macos_iokit_registry_entry_get_property_bytes(MacosMachPort entry, const char *property_name, unsigned char *data_out, uint32_t data_capacity, uint32_t *data_length_out) {
    MacosIokitPropertyBytesBuffer message;
    MacosMachPort reply_port;
    uint32_t bits;
    uint32_t name_count;
    uint32_t name_aligned;
    uint32_t send_size;
    uint32_t index;
    uint32_t copy_count;
    unsigned char *request_bytes;
    int result;

    if (data_length_out != 0) *data_length_out = 0U;
    if (entry == MACOS_MACH_PORT_NULL || property_name == 0 || data_length_out == 0 || (data_capacity != 0U && data_out == 0)) return -1;
    rt_memset(&message, 0, sizeof(message));
    reply_port = macos_mach_special_reply_port();
    if (reply_port == MACOS_MACH_PORT_NULL) return -1;

    request_bytes = message.bytes;
    ((MacosMachMsgHeader *)(void *)request_bytes)->remote_port = entry;
    ((MacosMachMsgHeader *)(void *)request_bytes)->local_port = reply_port;
    ((MacosMachMsgHeader *)(void *)request_bytes)->voucher_port = MACOS_MACH_PORT_NULL;
    ((MacosMachMsgHeader *)(void *)request_bytes)->id = MACOS_IOKIT_REGISTRY_ENTRY_GET_PROPERTY_BYTES_ID;
    *((MacosMachNdrRecord *)(void *)(request_bytes + sizeof(MacosMachMsgHeader))) = macos_mach_ndr_record();
    *(uint32_t *)(void *)(request_bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord)) = 0U;
    name_count = macos_mig_copy_name((char *)(void *)(request_bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord) + 8U), property_name);
    *(uint32_t *)(void *)(request_bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord) + 4U) = name_count;
    name_aligned = macos_mig_word_align(name_count);
    *(uint32_t *)(void *)(request_bytes + sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord) + 8U + name_aligned) = data_capacity > 4096U ? 4096U : data_capacity;
    send_size = (uint32_t)(sizeof(MacosMachMsgHeader) + sizeof(MacosMachNdrRecord) + 8U + name_aligned + 4U);

    bits = macos_mach_msg_bits(MACOS_MACH_MSG_TYPE_COPY_SEND, MACOS_MACH_MSG_TYPE_MAKE_SEND_ONCE);
    message.header.bits = bits;
    message.header.size = send_size;

    result = macos_mach_msg2(&message.header,
                             ((uint64_t)MACOS_MACH_RCV_MSG << 32U) | (MACOS_MACH_SEND_MSG | MACOS_MACH_RCV_MSG),
                             ((uint64_t)send_size << 32U) | (uint64_t)bits,
                             ((uint64_t)reply_port << 32U) | (uint64_t)entry,
                             ((uint64_t)MACOS_IOKIT_REGISTRY_ENTRY_GET_PROPERTY_BYTES_ID << 32U) | (uint64_t)MACOS_MACH_PORT_NULL,
                             ((uint64_t)reply_port << 32U),
                             (uint64_t)sizeof(message),
                             MACOS_MACH_MSG_TIMEOUT_NONE);
    if (result != MACOS_MACH_MSG_SUCCESS) return result;

    if (message.header.id != MACOS_IOKIT_REGISTRY_ENTRY_GET_PROPERTY_BYTES_ID + 100) return -1;
    if ((message.header.bits & MACOS_MACH_MSGH_BITS_COMPLEX) != 0U) return -1;
    if (message.header.size >= sizeof(MacosMachMigErrorReply) && message.error.result != MACOS_KERN_SUCCESS) return message.error.result;
    if (message.header.size < (sizeof(MacosIokitPropertyBytesReply) - sizeof(message.success.data))) return -1;
    if (message.success.data_count > sizeof(message.success.data)) return -1;

    copy_count = message.success.data_count;
    if (copy_count > data_capacity) copy_count = data_capacity;
    for (index = 0; index < copy_count; index++) data_out[index] = message.success.data[index];
    *data_length_out = message.success.data_count;
    return MACOS_KERN_SUCCESS;
}