#ifndef NEWOS_PLATFORM_MACOS_TRACE_H
#define NEWOS_PLATFORM_MACOS_TRACE_H

#define MACOS_STRACE_ENV "NEWOS_STRACE_FD"
#define MACOS_STRACE_RECORD_MAGIC 0x4e535452U
#define MACOS_STRACE_DECODE_TEXT_CAPACITY 96U
#define MACOS_STRACE_DECODE_KIND_NONE 0U
#define MACOS_STRACE_DECODE_KIND_STRING 1U
#define MACOS_STRACE_DECODE_KIND_BYTES 2U

typedef struct {
    unsigned int magic;
    unsigned int entering;
    unsigned int decoded_arg;
    unsigned int decoded_kind;
    unsigned int decoded_length;
    unsigned int decoded_truncated;
    long number;
    long args[6];
    long result;
    char decoded[MACOS_STRACE_DECODE_TEXT_CAPACITY];
} MacosStraceRecord;

#endif