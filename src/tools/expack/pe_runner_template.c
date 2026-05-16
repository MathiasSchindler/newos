typedef unsigned long size_t;

#define WIN_INVALID_HANDLE_VALUE ((void *)(long long)-1)
#define WIN_GENERIC_READ 0x80000000UL
#define WIN_GENERIC_WRITE 0x40000000UL
#define WIN_FILE_SHARE_READ 0x00000001UL
#define WIN_FILE_SHARE_WRITE 0x00000002UL
#define WIN_CREATE_ALWAYS 2UL
#define WIN_OPEN_EXISTING 3UL
#define WIN_FILE_ATTRIBUTE_NORMAL 0x00000080UL
#define WIN_FILE_ATTRIBUTE_TEMPORARY 0x00000100UL
#define WIN_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000UL
#define WIN_BEGIN 0UL
#define WIN_INFINITE 0xffffffffUL
#define WIN_MEM_COMMIT 0x1000UL
#define WIN_MEM_RESERVE 0x2000UL
#define WIN_PAGE_READWRITE 0x04UL
#define EXPACK_CODEC_LZSS 0U
#define EXPACK_CODEC_LZSS_BCJ 4U
#define EXPACK_CODEC_LZREP 3U
#define EXPACK_METADATA_SIZE 24U
#define EXPACK_MIN_MATCH 3U

#ifndef EXPACK_PE_RUNNER_CODEC
#define EXPACK_PE_RUNNER_CODEC EXPACK_CODEC_LZREP
#endif

typedef struct {
    unsigned long cb;
    char *reserved;
    char *desktop;
    char *title;
    unsigned long x;
    unsigned long y;
    unsigned long x_size;
    unsigned long y_size;
    unsigned long x_count_chars;
    unsigned long y_count_chars;
    unsigned long fill_attribute;
    unsigned long flags;
    unsigned short show_window;
    unsigned short cb_reserved2;
    unsigned char *reserved2;
    void *std_input;
    void *std_output;
    void *std_error;
} WinStartupInfoA;

typedef struct {
    void *process;
    void *thread;
    unsigned long process_id;
    unsigned long thread_id;
} WinProcessInformation;

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *buffer, unsigned long size);
__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
__declspec(dllimport) void *__stdcall CreateFileA(const char *file_name, unsigned long desired_access, unsigned long share_mode, void *security_attributes, unsigned long creation_disposition, unsigned long flags_and_attributes, void *template_file);
__declspec(dllimport) int __stdcall SetFilePointerEx(void *handle, long long distance_to_move, long long *new_file_pointer, unsigned long move_method);
__declspec(dllimport) int __stdcall ReadFile(void *handle, void *buffer, unsigned long count, unsigned long *read_out, void *overlapped);
__declspec(dllimport) int __stdcall WriteFile(void *handle, const void *buffer, unsigned long count, unsigned long *written_out, void *overlapped);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) unsigned long __stdcall GetTempPathA(unsigned long buffer_length, char *buffer);
__declspec(dllimport) int __stdcall CreateProcessA(const char *application_name, char *command_line, void *process_attributes, void *thread_attributes, int inherit_handles, unsigned long creation_flags, void *environment, const char *current_directory, WinStartupInfoA *startup_info, WinProcessInformation *process_information);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *handle, unsigned long milliseconds);
__declspec(dllimport) int __stdcall GetExitCodeProcess(void *process, unsigned long *exit_code);
__declspec(dllimport) int __stdcall DeleteFileA(const char *file_name);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long allocation_type, unsigned long protect);

unsigned long __stack_chk_guard;
const unsigned long long expack_metadata_file_offset = 0x1122334455667788ULL;

void __main(void) {
}

__attribute__((naked)) void ___chkstk_ms(void) {
    __asm__("ret");
}

static void zero_memory(void *ptr, unsigned long size) {
    unsigned char *p = (unsigned char *)ptr;
    while (size != 0UL) {
        *p++ = 0U;
        size -= 1UL;
    }
}

static unsigned int read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8U) |
           ((unsigned int)bytes[2] << 16U) |
           ((unsigned int)bytes[3] << 24U);
}

static unsigned long long read_u64_le(const unsigned char *bytes) {
    return (unsigned long long)read_u32_le(bytes) | ((unsigned long long)read_u32_le(bytes + 4) << 32U);
}

static int read_exact(void *handle, void *buffer, unsigned long long size) {
    unsigned char *out = (unsigned char *)buffer;
    while (size != 0ULL) {
        unsigned long chunk = size > 0x40000000ULL ? 0x40000000UL : (unsigned long)size;
        unsigned long got = 0UL;
        if (!ReadFile(handle, out, chunk, &got, 0) || got == 0UL) {
            return -1;
        }
        out += got;
        size -= (unsigned long long)got;
    }
    return 0;
}

static int write_exact(void *handle, const void *buffer, unsigned long long size) {
    const unsigned char *in = (const unsigned char *)buffer;
    while (size != 0ULL) {
        unsigned long chunk = size > 0x40000000ULL ? 0x40000000UL : (unsigned long)size;
        unsigned long wrote = 0UL;
        if (!WriteFile(handle, in, chunk, &wrote, 0) || wrote == 0UL) {
            return -1;
        }
        in += wrote;
        size -= (unsigned long long)wrote;
    }
    return 0;
}

#if EXPACK_PE_RUNNER_CODEC == EXPACK_CODEC_LZREP
static int decode_payload(const unsigned char *payload, unsigned long long payload_size, unsigned char *output, unsigned long long output_size) {
    unsigned long long input_offset = 0ULL;
    unsigned long long output_offset = 0ULL;
    unsigned int last_distance = 1U;

    while (output_offset < output_size) {
        unsigned char flags;
        unsigned int bit;
        if (input_offset >= payload_size) {
            return -1;
        }
        flags = payload[input_offset++];
        for (bit = 0U; bit < 8U && output_offset < output_size; ++bit) {
            if ((flags & (unsigned char)(1U << bit)) == 0U) {
                if (input_offset >= payload_size) {
                    return -1;
                }
                output[output_offset++] = payload[input_offset++];
            } else {
                unsigned int token;
                unsigned int distance;
                unsigned int length;
                unsigned int i;
                if (input_offset >= payload_size) {
                    return -1;
                }
                token = payload[input_offset++];
                if ((token & 0x80U) != 0U) {
                    distance = last_distance;
                    length = (token & 0x7fU) + EXPACK_MIN_MATCH;
                } else {
                    if (input_offset >= payload_size) {
                        return -1;
                    }
                    length = (token & 0x0fU) + EXPACK_MIN_MATCH;
                    distance = (((token >> 4U) << 8U) | payload[input_offset++]) + 1U;
                    last_distance = distance;
                }
                if ((unsigned long long)distance > output_offset || (unsigned long long)length > output_size - output_offset) {
                    return -1;
                }
                for (i = 0U; i < length; ++i) {
                    output[output_offset] = output[output_offset - (unsigned long long)distance];
                    output_offset += 1ULL;
                }
            }
        }
    }
    return input_offset == payload_size ? 0 : -1;
}
#elif EXPACK_PE_RUNNER_CODEC == EXPACK_CODEC_LZSS || EXPACK_PE_RUNNER_CODEC == EXPACK_CODEC_LZSS_BCJ
static int decode_payload(const unsigned char *payload, unsigned long long payload_size, unsigned char *output, unsigned long long output_size) {
    unsigned long long input_offset = 0ULL;
    unsigned long long output_offset = 0ULL;

    while (output_offset < output_size) {
        unsigned char flags;
        unsigned int bit;
        if (input_offset >= payload_size) {
            return -1;
        }
        flags = payload[input_offset++];
        for (bit = 0U; bit < 8U && output_offset < output_size; ++bit) {
            if ((flags & (unsigned char)(1U << bit)) == 0U) {
                if (input_offset >= payload_size) {
                    return -1;
                }
                output[output_offset++] = payload[input_offset++];
            } else {
                unsigned int token;
                unsigned int distance;
                unsigned int length;
                unsigned int i;
                if (input_offset + 2ULL > payload_size) {
                    return -1;
                }
                token = (unsigned int)payload[input_offset] | ((unsigned int)payload[input_offset + 1ULL] << 8U);
                input_offset += 2ULL;
                distance = ((token & 0x00ffU) | ((token >> 3U) & 0x1f00U)) + 1U;
                length = ((token >> 8U) & 0x07U) + EXPACK_MIN_MATCH;
                if ((unsigned long long)distance > output_offset || (unsigned long long)length > output_size - output_offset) {
                    return -1;
                }
                for (i = 0U; i < length; ++i) {
                    output[output_offset] = output[output_offset - (unsigned long long)distance];
                    output_offset += 1ULL;
                }
            }
        }
    }
    if (input_offset != payload_size) {
        return -1;
    }

#if EXPACK_PE_RUNNER_CODEC == EXPACK_CODEC_LZSS_BCJ
    {
        unsigned long long position = 0ULL;
        while (position + 4ULL < output_size) {
            if (output[position] == 0xe8U || output[position] == 0xe9U) {
                unsigned int value = read_u32_le(output + position + 1ULL);
                value -= (unsigned int)(position + 5ULL);
                output[position + 1ULL] = (unsigned char)(value & 0xffU);
                output[position + 2ULL] = (unsigned char)((value >> 8U) & 0xffU);
                output[position + 3ULL] = (unsigned char)((value >> 16U) & 0xffU);
                output[position + 4ULL] = (unsigned char)((value >> 24U) & 0xffU);
                position += 5ULL;
            } else if (position + 5ULL < output_size && output[position] == 0x0fU && (output[position + 1ULL] & 0xf0U) == 0x80U) {
                unsigned int value = read_u32_le(output + position + 2ULL);
                value -= (unsigned int)(position + 6ULL);
                output[position + 2ULL] = (unsigned char)(value & 0xffU);
                output[position + 3ULL] = (unsigned char)((value >> 8U) & 0xffU);
                output[position + 4ULL] = (unsigned char)((value >> 16U) & 0xffU);
                output[position + 5ULL] = (unsigned char)((value >> 24U) & 0xffU);
                position += 6ULL;
            } else {
                position += 1ULL;
            }
        }
    }
#endif
    return 0;
}
#else
#error unsupported EXPACK_PE_RUNNER_CODEC
#endif

static const char *skip_first_arg(const char *command_line) {
    int in_quotes = 0;
    while (*command_line == ' ' || *command_line == '\t') {
        command_line += 1;
    }
    while (*command_line != '\0') {
        if (*command_line == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (*command_line == ' ' || *command_line == '\t')) {
            break;
        }
        command_line += 1;
    }
    while (*command_line == ' ' || *command_line == '\t') {
        command_line += 1;
    }
    return command_line;
}

static int append_hex8(char *buffer, unsigned long *used, unsigned long buffer_size, unsigned long value) {
    static const char digits[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4) {
        if (*used + 1UL >= buffer_size) {
            return -1;
        }
        buffer[(*used)++] = digits[(value >> (unsigned int)shift) & 0x0fU];
    }
    return 0;
}

static int make_temp_path(char *buffer, unsigned long buffer_size) {
    unsigned long used = GetTempPathA(buffer_size, buffer);
    if (used == 0UL || used + 15UL >= buffer_size) {
        return -1;
    }
    buffer[used++] = 'e';
    buffer[used++] = 'x';
    if (append_hex8(buffer, &used, buffer_size, GetCurrentProcessId()) != 0) {
        return -1;
    }
    buffer[used++] = '.';
    buffer[used++] = 'e';
    buffer[used++] = 'x';
    buffer[used++] = 'e';
    buffer[used] = '\0';
    return 0;
}

static int make_child_command_line(const char *temp_path, char *buffer, unsigned long buffer_size) {
    const char *tail = skip_first_arg(GetCommandLineA());
    unsigned long used = 0UL;
    if (buffer_size < 4UL) {
        return -1;
    }
    buffer[used++] = '"';
    while (*temp_path != '\0') {
        if (used + 2UL >= buffer_size || *temp_path == '"') {
            return -1;
        }
        buffer[used++] = *temp_path++;
    }
    if (used + 2UL >= buffer_size) {
        return -1;
    }
    buffer[used++] = '"';
    if (*tail != '\0') {
        buffer[used++] = ' ';
        while (*tail != '\0') {
            if (used + 1UL >= buffer_size) {
                return -1;
            }
            buffer[used++] = *tail++;
        }
    }
    buffer[used] = '\0';
    return 0;
}

void mainCRTStartup(void) {
    char self_path[520];
    char temp_path[520];
    char command_line[2048];
    unsigned char metadata[EXPACK_METADATA_SIZE];
    unsigned char *payload = 0;
    unsigned char *image = 0;
    unsigned long long original_size;
    unsigned long long payload_size;
    WinStartupInfoA startup;
    WinProcessInformation process;
    void *input;
    void *output;
    long long new_pointer = 0;
    unsigned long exit_code = 127UL;

    if (GetModuleFileNameA(0, self_path, sizeof(self_path)) == 0) ExitProcess(127);
    input = CreateFileA(self_path, WIN_GENERIC_READ, WIN_FILE_SHARE_READ | WIN_FILE_SHARE_WRITE, 0, WIN_OPEN_EXISTING, WIN_FILE_ATTRIBUTE_NORMAL, 0);
    if (input == WIN_INVALID_HANDLE_VALUE) ExitProcess(127);
    if (!SetFilePointerEx(input, (long long)expack_metadata_file_offset, &new_pointer, WIN_BEGIN) || read_exact(input, metadata, sizeof(metadata)) != 0) {
        CloseHandle(input);
        ExitProcess(127);
    }
    if (metadata[0] != 'E' || metadata[1] != 'X' || metadata[2] != 'P' || metadata[3] != 'A' || metadata[4] != 'C' || metadata[5] != 'K' || metadata[6] != 'P' || metadata[7] != '1') {
        CloseHandle(input);
        ExitProcess(127);
    }
    original_size = read_u64_le(metadata + 8U);
    payload_size = read_u64_le(metadata + 16U);
    if (original_size == 0ULL || payload_size == 0ULL || payload_size > 0x7fffffffULL || original_size > 0x7fffffffULL) {
        CloseHandle(input);
        ExitProcess(127);
    }
    payload = (unsigned char *)VirtualAlloc(0, (size_t)payload_size, WIN_MEM_COMMIT | WIN_MEM_RESERVE, WIN_PAGE_READWRITE);
    image = (unsigned char *)VirtualAlloc(0, (size_t)original_size, WIN_MEM_COMMIT | WIN_MEM_RESERVE, WIN_PAGE_READWRITE);
    if (payload == 0 || image == 0) {
        CloseHandle(input);
        ExitProcess(127);
    }
    if (!SetFilePointerEx(input, (long long)(expack_metadata_file_offset + EXPACK_METADATA_SIZE), &new_pointer, WIN_BEGIN) || read_exact(input, payload, payload_size) != 0) {
        CloseHandle(input);
        ExitProcess(127);
    }
    CloseHandle(input);
    if (decode_payload(payload, payload_size, image, original_size) != 0) ExitProcess(127);
    if (make_temp_path(temp_path, sizeof(temp_path)) != 0) ExitProcess(127);
    output = CreateFileA(temp_path, WIN_GENERIC_WRITE, 0, 0, WIN_CREATE_ALWAYS, WIN_FILE_ATTRIBUTE_NORMAL | WIN_FILE_ATTRIBUTE_TEMPORARY | WIN_FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, 0);
    if (output == WIN_INVALID_HANDLE_VALUE) {
        DeleteFileA(temp_path);
        ExitProcess(127);
    }
    if (write_exact(output, image, original_size) != 0) {
        CloseHandle(output);
        DeleteFileA(temp_path);
        ExitProcess(127);
    }
    CloseHandle(output);
    if (make_child_command_line(temp_path, command_line, sizeof(command_line)) != 0) {
        DeleteFileA(temp_path);
        ExitProcess(127);
    }
    zero_memory(&startup, sizeof(startup));
    zero_memory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(temp_path, command_line, 0, 0, 1, 0, 0, 0, &startup, &process)) {
        DeleteFileA(temp_path);
        ExitProcess(127);
    }
    WaitForSingleObject(process.process, WIN_INFINITE);
    GetExitCodeProcess(process.process, &exit_code);
    CloseHandle(process.thread);
    CloseHandle(process.process);
    DeleteFileA(temp_path);
    ExitProcess(exit_code);
}
