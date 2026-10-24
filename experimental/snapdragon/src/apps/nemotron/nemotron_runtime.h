#ifndef NEWOS_NEMOTRON_RUNTIME_H
#define NEWOS_NEMOTRON_RUNTIME_H
#include "nemotron_windows.h"
typedef struct NmRuntime {
    NmArtifact artifact;
    u8 *data;
    u64 bytes;
    char path[4096];
} NmRuntime;
int nm_runtime_open(NmRuntime *runtime, const char *path);
void nm_runtime_close(NmRuntime *runtime);
int nm_runtime_path(const NmRuntime *runtime, u32 role, char path[4096]);
int nm_runtime_verify(const NmRuntime *runtime, u32 role);
u8 *nm_runtime_read(const NmRuntime *runtime, u32 role, u64 *bytes);
int nm_runtime_check(const NmRuntime *runtime);
#endif
