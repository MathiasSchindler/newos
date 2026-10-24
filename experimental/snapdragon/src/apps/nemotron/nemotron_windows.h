#ifndef NEWOS_NEMOTRON_WINDOWS_H
#define NEWOS_NEMOTRON_WINDOWS_H
#include "nemotron_reader.h"
#include "nemotron_wav.h"
typedef struct NmWindowsFile { void *handle; u64 bytes; } NmWindowsFile;
typedef struct NmModuleStage {
    char directory[4096], module[4096], catalog[4096];
    int created, module_created, catalog_created;
} NmModuleStage;
int nm_file_open(NmWindowsFile *file, NmInput *input, const char *path, int writing);
int nm_file_read(NmWindowsFile *file, u64 offset, void *data, u32 bytes);
int nm_file_write(NmWindowsFile *file, u64 offset, const void *data, u32 bytes);
int nm_file_close(NmWindowsFile *file);
int nm_file_remove(const char *path);
int nm_absolute_path(const char *path, char absolute[4096]);
int nm_environment_set(const char *name, const char *value);
void nm_diagnostics_to_stderr(int enabled);
int nm_stdout(const void *data, u32 bytes);
int nm_module_stage(NmModuleStage *stage, const char *module, const char *catalog,
                    const u8 module_sha[32], const u8 catalog_sha[32]);
int nm_module_stage_close(NmModuleStage *stage);
void nm_text(const char *text);
void nm_number(u64 value);
void nm_boundary(const char *operation, const char *name, const char *type);
void nm_debug_init(void);
int nm_error(const char *text);
void *nm_alloc(u64 bytes);
void nm_free(void *data, u64 bytes);
u8 *nm_read_file(const char *path, u64 *bytes);
int nm_write_file(const char *path, const void *data, u64 bytes);
/* QPC ticks use RESULT's frequency at header offset200, not CPU/DSP cycle units. */
u64 nm_clock(void);
u64 nm_frequency(void);
/* Windows peak working-set bytes. */
u64 nm_peak_rss(void);
u64 nm_working_set(void);
int nm_process_cpu(u64 *user_100ns, u64 *kernel_100ns);
void *nm_load_qnn(void);
void *nm_symbol(void *module, const char *name);
void nm_unload(void *module);
int nm_modules(void);
/* Returns 1 for a complete, hash-verified absolute package path, 0 if absent, -1 on error. */
int nm_package_path(const char *path_variable, const char *hash_variable, char path[4096]);
int nm_arguments(char args[8][4096]);
int nm_arguments_n(char (*args)[4096], u32 capacity);
__declspec(dllimport) void ExitProcess(u32 code);
#endif
