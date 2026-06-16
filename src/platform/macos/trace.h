#ifndef NEWOS_PLATFORM_MACOS_TRACE_H
#define NEWOS_PLATFORM_MACOS_TRACE_H

#define MACOS_STRACE_ENV "NEWOS_STRACE_FD"
#define MACOS_STRACE_RECORD_MAGIC 0x4e535452U

typedef struct {
    unsigned int magic;
    unsigned int entering;
    long number;
    long args[6];
    long result;
} MacosStraceRecord;

#endif