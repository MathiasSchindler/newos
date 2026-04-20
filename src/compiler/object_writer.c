#include "object_writer.h"

#include "platform.h"
#include "runtime.h"
#include "source.h"

typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define OBJECT_WRITER_MAX_TEXT 262144
#define OBJECT_WRITER_MAX_DATA 65536
#define OBJECT_WRITER_MAX_SYMBOLS 1024
#define OBJECT_WRITER_MAX_LABELS 2048
#define OBJECT_WRITER_MAX_FIXUPS 4096
#define OBJECT_WRITER_MAX_RELOCS 4096
#define OBJECT_WRITER_MAX_OUTPUT (OBJECT_WRITER_MAX_TEXT + OBJECT_WRITER_MAX_DATA + 65536)

#define ELFCLASS64 2U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define ET_REL 1U
#define EM_X86_64 62U

#define SHT_NULL 0U
#define SHT_PROGBITS 1U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_RELA 4U

#define SHF_WRITE 0x1ULL
#define SHF_ALLOC 0x2ULL
#define SHF_EXECINSTR 0x4ULL

#define STB_LOCAL 0U
#define STB_GLOBAL 1U
#define STT_NOTYPE 0U
#define STT_OBJECT 1U
#define STT_FUNC 2U
#define STT_SECTION 3U

#define R_X86_64_PC32 2U
#define R_X86_64_PLT32 4U

typedef enum {
    OBJECT_SECTION_NONE = 0,
    OBJECT_SECTION_TEXT = 1,
    OBJECT_SECTION_DATA = 2
} ObjectSection;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    ObjectSection section;
    size_t offset;
    int defined;
    int global;
    int is_function;
    uint32_t name_offset;
    unsigned int sym_index;
} ObjectSymbol;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    ObjectSection section;
    size_t offset;
} ObjectLabel;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    size_t offset;
    uint32_t type;
    int64_t addend;
} ObjectFixup;

typedef struct {
    size_t offset;
    char name[COMPILER_IR_NAME_CAPACITY];
    uint32_t type;
    int64_t addend;
} ObjectRelocation;

typedef struct {
    CompilerObjectWriter *writer;
    unsigned char text[OBJECT_WRITER_MAX_TEXT];
    unsigned char data[OBJECT_WRITER_MAX_DATA];
    size_t text_size;
    size_t data_size;
    ObjectSymbol symbols[OBJECT_WRITER_MAX_SYMBOLS];
    size_t symbol_count;
    ObjectLabel labels[OBJECT_WRITER_MAX_LABELS];
    size_t label_count;
    ObjectFixup fixups[OBJECT_WRITER_MAX_FIXUPS];
    size_t fixup_count;
    ObjectRelocation relocs[OBJECT_WRITER_MAX_RELOCS];
    size_t reloc_count;
    ObjectSection current_section;
} ObjectAssembler;

static void set_error(CompilerObjectWriter *writer, const char *message) {
    rt_copy_string(writer->error_message, sizeof(writer->error_message), message != 0 ? message : "object writer error");
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

static int names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

static const char *find_top_level_comma(const char *text) {
    int depth = 0;

    while (*text != '\0') {
        if (*text == '(') {
            depth += 1;
        } else if (*text == ')') {
            if (depth > 0) {
                depth -= 1;
            }
        } else if (*text == ',' && depth == 0) {
            return text;
        }
        text += 1;
    }

    return 0;
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static size_t string_length(const char *text) {
    return rt_strlen(text);
}

static int parse_signed_value(const char *text, long long *value_out) {
    unsigned long long magnitude = 0;
    int negative = 0;

    text = skip_spaces(text);
    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (rt_parse_uint(text, &magnitude) != 0) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

static void write_u16_le(unsigned char *buffer, uint16_t value) {
    buffer[0] = (unsigned char)(value & 0xffU);
    buffer[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void write_u32_le(unsigned char *buffer, uint32_t value) {
    buffer[0] = (unsigned char)(value & 0xffU);
    buffer[1] = (unsigned char)((value >> 8) & 0xffU);
    buffer[2] = (unsigned char)((value >> 16) & 0xffU);
    buffer[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void write_u64_le(unsigned char *buffer, uint64_t value) {
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        buffer[i] = (unsigned char)((value >> (8U * i)) & 0xffU);
    }
}

static void write_i64_le(unsigned char *buffer, int64_t value) {
    write_u64_le(buffer, (uint64_t)value);
}

static void patch_i32_le(unsigned char *buffer, int32_t value) {
    write_u32_le(buffer, (uint32_t)value);
}

static int append_bytes(unsigned char *buffer, size_t *size, size_t capacity, const unsigned char *data, size_t count) {
    size_t i;

    if (*size + count > capacity) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        buffer[*size + i] = data[i];
    }
    *size += count;
    return 0;
}

static int append_byte(ObjectAssembler *assembler, ObjectSection section, unsigned char byte) {
    if (section == OBJECT_SECTION_TEXT) {
        return append_bytes(assembler->text, &assembler->text_size, sizeof(assembler->text), &byte, 1U);
    }
    if (section == OBJECT_SECTION_DATA) {
        return append_bytes(assembler->data, &assembler->data_size, sizeof(assembler->data), &byte, 1U);
    }
    return -1;
}

static int append_u32(ObjectAssembler *assembler, ObjectSection section, uint32_t value) {
    unsigned char bytes[4];
    write_u32_le(bytes, value);
    return append_bytes(
        section == OBJECT_SECTION_TEXT ? assembler->text : assembler->data,
        section == OBJECT_SECTION_TEXT ? &assembler->text_size : &assembler->data_size,
        section == OBJECT_SECTION_TEXT ? sizeof(assembler->text) : sizeof(assembler->data),
        bytes,
        sizeof(bytes)
    );
}

static int append_u64(ObjectAssembler *assembler, ObjectSection section, uint64_t value) {
    unsigned char bytes[8];
    write_u64_le(bytes, value);
    return append_bytes(
        section == OBJECT_SECTION_TEXT ? assembler->text : assembler->data,
        section == OBJECT_SECTION_TEXT ? &assembler->text_size : &assembler->data_size,
        section == OBJECT_SECTION_TEXT ? sizeof(assembler->text) : sizeof(assembler->data),
        bytes,
        sizeof(bytes)
    );
}

static int find_symbol(const ObjectAssembler *assembler, const char *name) {
    size_t i;

    for (i = 0; i < assembler->symbol_count; ++i) {
        if (names_equal(assembler->symbols[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int get_symbol(ObjectAssembler *assembler, const char *name) {
    int existing = find_symbol(assembler, name);

    if (existing >= 0) {
        return existing;
    }
    if (assembler->symbol_count >= OBJECT_WRITER_MAX_SYMBOLS) {
        set_error(assembler->writer, "too many symbols for current object writer");
        return -1;
    }

    rt_copy_string(assembler->symbols[assembler->symbol_count].name, sizeof(assembler->symbols[assembler->symbol_count].name), name);
    assembler->symbols[assembler->symbol_count].section = OBJECT_SECTION_NONE;
    assembler->symbols[assembler->symbol_count].offset = 0;
    assembler->symbols[assembler->symbol_count].defined = 0;
    assembler->symbols[assembler->symbol_count].global = 0;
    assembler->symbols[assembler->symbol_count].is_function = 0;
    assembler->symbols[assembler->symbol_count].name_offset = 0U;
    assembler->symbols[assembler->symbol_count].sym_index = 0;
    assembler->symbol_count += 1U;
    return (int)(assembler->symbol_count - 1U);
}

static int add_label(ObjectAssembler *assembler, const char *name, ObjectSection section, size_t offset) {
    size_t i;

    for (i = 0; i < assembler->label_count; ++i) {
        if (names_equal(assembler->labels[i].name, name)) {
            assembler->labels[i].section = section;
            assembler->labels[i].offset = offset;
            return 0;
        }
    }

    if (assembler->label_count >= OBJECT_WRITER_MAX_LABELS) {
        set_error(assembler->writer, "too many labels for current object writer");
        return -1;
    }

    rt_copy_string(assembler->labels[assembler->label_count].name, sizeof(assembler->labels[assembler->label_count].name), name);
    assembler->labels[assembler->label_count].section = section;
    assembler->labels[assembler->label_count].offset = offset;
    assembler->label_count += 1U;
    return 0;
}

static int find_label(const ObjectAssembler *assembler, const char *name) {
    size_t i;

    for (i = 0; i < assembler->label_count; ++i) {
        if (names_equal(assembler->labels[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_fixup(ObjectAssembler *assembler, const char *name, size_t offset, uint32_t type, int64_t addend) {
    if (assembler->fixup_count >= OBJECT_WRITER_MAX_FIXUPS) {
        set_error(assembler->writer, "too many relocations/fixups for current object writer");
        return -1;
    }

    rt_copy_string(assembler->fixups[assembler->fixup_count].name, sizeof(assembler->fixups[assembler->fixup_count].name), name);
    assembler->fixups[assembler->fixup_count].offset = offset;
    assembler->fixups[assembler->fixup_count].type = type;
    assembler->fixups[assembler->fixup_count].addend = addend;
    assembler->fixup_count += 1U;
    return 0;
}

static int add_relocation(ObjectAssembler *assembler, const char *name, size_t offset, uint32_t type, int64_t addend) {
    if (assembler->reloc_count >= OBJECT_WRITER_MAX_RELOCS) {
        set_error(assembler->writer, "too many relocation entries for current object writer");
        return -1;
    }

    assembler->relocs[assembler->reloc_count].offset = offset;
    assembler->relocs[assembler->reloc_count].type = type;
    assembler->relocs[assembler->reloc_count].addend = addend;
    rt_copy_string(assembler->relocs[assembler->reloc_count].name, sizeof(assembler->relocs[assembler->reloc_count].name), name);
    assembler->reloc_count += 1U;
    return 0;
}

static int try_parse_register(const char *text,
                              const char *name,
                              int reg,
                              int is_byte_reg,
                              int *reg_out,
                              int *is_byte_reg_out,
                              const char **rest_out) {
    size_t len = string_length(name);
    size_t j;

    for (j = 0; j < len; ++j) {
        if (text[j] != name[j]) {
            return 0;
        }
    }

    *reg_out = reg;
    *is_byte_reg_out = is_byte_reg;
    *rest_out = text + len;
    return 1;
}

static int parse_register(const char *text, int *reg_out, int *is_byte_reg_out, const char **rest_out) {
    if (try_parse_register(text, "%rax", 0, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rcx", 1, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rdx", 2, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rbx", 3, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rsp", 4, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rbp", 5, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rsi", 6, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%rdi", 7, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%r8", 8, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%r9", 9, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%r10", 10, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%r11", 11, 0, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%al", 0, 1, reg_out, is_byte_reg_out, rest_out)) return 0;
    if (try_parse_register(text, "%cl", 1, 1, reg_out, is_byte_reg_out, rest_out)) return 0;
    return -1;
}

static int append_rex(ObjectAssembler *assembler, int w, int reg, int index, int base) {
    unsigned char rex = 0x40U;

    if (w) rex |= 0x08U;
    if (reg >= 8) rex |= 0x04U;
    if (index >= 8) rex |= 0x02U;
    if (base >= 8) rex |= 0x01U;

    if (rex != 0x40U) {
        return append_byte(assembler, OBJECT_SECTION_TEXT, rex);
    }

    return 0;
}

static int append_modrm(ObjectAssembler *assembler, unsigned int mod, unsigned int reg, unsigned int rm) {
    unsigned char byte = (unsigned char)(((mod & 0x3U) << 6U) | ((reg & 0x7U) << 3U) | (rm & 0x7U));
    return append_byte(assembler, OBJECT_SECTION_TEXT, byte);
}

static int append_mem_modrm(ObjectAssembler *assembler, unsigned int mod, unsigned int reg, int base_reg) {
    if ((base_reg & 7) == 4) {
        return append_modrm(assembler, mod, reg, 4U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT,
                           (unsigned char)(0x20U | (unsigned int)(base_reg & 7))) == 0 ? 0 : -1;
    }

    return append_modrm(assembler, mod, reg, (unsigned int)base_reg);
}

static int encode_push_reg(ObjectAssembler *assembler, int reg) {
    if (reg >= 8 && append_byte(assembler, OBJECT_SECTION_TEXT, 0x41U) != 0) {
        return -1;
    }
    return append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)(0x50U + (reg & 7)));
}

static int encode_pop_reg(ObjectAssembler *assembler, int reg) {
    if (reg >= 8 && append_byte(assembler, OBJECT_SECTION_TEXT, 0x41U) != 0) {
        return -1;
    }
    return append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)(0x58U + (reg & 7)));
}

static int encode_mov_reg_reg(ObjectAssembler *assembler, int src, int dst) {
    return append_rex(assembler, 1, src, 0, dst) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0x89U) == 0 &&
           append_modrm(assembler, 3U, (unsigned int)src, (unsigned int)dst) == 0 ? 0 : -1;
}

static int encode_mov_imm_reg(ObjectAssembler *assembler, long long value, int dst) {
    if (append_rex(assembler, 1, 0, 0, dst) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0xC7U) != 0 ||
        append_modrm(assembler, 3U, 0U, (unsigned int)dst) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)value) != 0) {
        return -1;
    }
    return 0;
}

static int encode_alu_imm_reg(ObjectAssembler *assembler, unsigned int alu_op, int dst_reg, long long value) {
    if (value >= -128 && value <= 127) {
        return append_rex(assembler, 1, 0, 0, dst_reg) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x83U) == 0 &&
               append_modrm(assembler, 3U, alu_op, (unsigned int)dst_reg) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)value)) == 0 ? 0 : -1;
    }

    return append_rex(assembler, 1, 0, 0, dst_reg) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0x81U) == 0 &&
           append_modrm(assembler, 3U, alu_op, (unsigned int)dst_reg) == 0 &&
           append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)value) == 0 ? 0 : -1;
}

static int parse_local_mem(const char *operand, int *disp_out, int *base_reg_out) {
    const char *cursor = operand;
    char disp_text[64];
    size_t disp_length = 0;
    long long disp = 0;
    int reg;
    int is_byte;
    const char *rest;

    cursor = skip_spaces(cursor);

    if (*cursor == '(') {
        disp = 0;
    } else {
        while (cursor[disp_length] != '\0' && cursor[disp_length] != '(' && disp_length + 1 < sizeof(disp_text)) {
            disp_text[disp_length] = cursor[disp_length];
            disp_length += 1U;
        }
        disp_text[disp_length] = '\0';
        if (disp_text[0] == '\0' || parse_signed_value(disp_text, &disp) != 0) {
            return -1;
        }
        cursor += disp_length;
    }

    if (*cursor != '(' || cursor[1] != '%') {
        return -1;
    }

    if (parse_register(cursor + 1, &reg, &is_byte, &rest) != 0 || *rest != ')') {
        return -1;
    }

    *disp_out = (int)disp;
    *base_reg_out = reg;
    return 0;
}

static int parse_immediate_operand(const char *text, long long *value_out) {
    char buffer[64];
    size_t i = 0;

    text = skip_spaces(text);
    while (text[i] != '\0' && text[i] != ',' && text[i] != ' ' && text[i] != '\t' && i + 1 < sizeof(buffer)) {
        buffer[i] = text[i];
        i += 1U;
    }
    buffer[i] = '\0';

    if (buffer[0] == '\0') {
        return -1;
    }

    return parse_signed_value(buffer, value_out);
}

static int encode_mov_mem_reg(ObjectAssembler *assembler, int base_reg, int disp, int dst_reg) {
    if (append_rex(assembler, 1, dst_reg, 0, base_reg) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0x8BU) != 0) {
        return -1;
    }

    if (disp == 0 && base_reg != 5) {
        return append_mem_modrm(assembler, 0U, (unsigned int)dst_reg, base_reg);
    }

    if (disp >= -128 && disp <= 127) {
        if (append_mem_modrm(assembler, 1U, (unsigned int)dst_reg, base_reg) != 0 ||
            append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)disp)) != 0) {
            return -1;
        }
        return 0;
    }

    if (append_mem_modrm(assembler, 2U, (unsigned int)dst_reg, base_reg) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)disp) != 0) {
        return -1;
    }
    return 0;
}

static int encode_mov_reg_mem(ObjectAssembler *assembler, int src_reg, int base_reg, int disp) {
    if (append_rex(assembler, 1, src_reg, 0, base_reg) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0x89U) != 0) {
        return -1;
    }

    if (disp == 0 && base_reg != 5) {
        return append_mem_modrm(assembler, 0U, (unsigned int)src_reg, base_reg);
    }

    if (disp >= -128 && disp <= 127) {
        if (append_mem_modrm(assembler, 1U, (unsigned int)src_reg, base_reg) != 0 ||
            append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)disp)) != 0) {
            return -1;
        }
        return 0;
    }

    if (append_mem_modrm(assembler, 2U, (unsigned int)src_reg, base_reg) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)disp) != 0) {
        return -1;
    }
    return 0;
}

static int encode_movzx_mem_reg(ObjectAssembler *assembler, int base_reg, int disp, int dst_reg) {
    if (append_rex(assembler, 1, dst_reg, 0, base_reg) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0x0FU) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0xB6U) != 0) {
        return -1;
    }

    if (disp == 0 && base_reg != 5) {
        return append_mem_modrm(assembler, 0U, (unsigned int)dst_reg, base_reg);
    }

    if (disp >= -128 && disp <= 127) {
        if (append_mem_modrm(assembler, 1U, (unsigned int)dst_reg, base_reg) != 0 ||
            append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)disp)) != 0) {
            return -1;
        }
        return 0;
    }

    if (append_mem_modrm(assembler, 2U, (unsigned int)dst_reg, base_reg) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)disp) != 0) {
        return -1;
    }
    return 0;
}

static int encode_mov_reg_mem_byte(ObjectAssembler *assembler, int src_reg, int base_reg, int disp) {
    if (append_rex(assembler, 0, src_reg, 0, base_reg) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0x88U) != 0) {
        return -1;
    }

    if (disp == 0 && base_reg != 5) {
        return append_mem_modrm(assembler, 0U, (unsigned int)src_reg, base_reg);
    }

    if (disp >= -128 && disp <= 127) {
        if (append_mem_modrm(assembler, 1U, (unsigned int)src_reg, base_reg) != 0 ||
            append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)disp)) != 0) {
            return -1;
        }
        return 0;
    }

    if (append_mem_modrm(assembler, 2U, (unsigned int)src_reg, base_reg) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)disp) != 0) {
        return -1;
    }
    return 0;
}

static int parse_indexed_mem(const char *operand, int *base_reg_out, int *index_reg_out, int *scale_out) {
    const char *cursor = skip_spaces(operand);
    int reg;
    int is_byte;
    const char *rest;
    unsigned long long scale = 1ULL;

    if (*cursor != '(' || cursor[1] != '%') {
        return -1;
    }

    if (parse_register(cursor + 1, &reg, &is_byte, &rest) != 0 || *rest != ',') {
        return -1;
    }
    *base_reg_out = reg;

    cursor = skip_spaces(rest + 1);
    if (parse_register(cursor, &reg, &is_byte, &rest) != 0) {
        return -1;
    }
    *index_reg_out = reg;

    if (*rest == ',') {
        cursor = skip_spaces(rest + 1);
        scale = 0ULL;
        if (*cursor < '0' || *cursor > '9') {
            return -1;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            scale = (scale * 10ULL) + (unsigned long long)(*cursor - '0');
            cursor += 1;
        }
        rest = cursor;
    }

    if (*rest != ')' || (scale != 1ULL && scale != 2ULL && scale != 4ULL && scale != 8ULL)) {
        return -1;
    }

    *scale_out = (int)scale;
    return 0;
}

static int encode_lea_indexed_reg(ObjectAssembler *assembler, int base_reg, int index_reg, int scale, int dst_reg) {
    unsigned int scale_bits = 0U;

    if (scale == 2) scale_bits = 1U;
    else if (scale == 4) scale_bits = 2U;
    else if (scale == 8) scale_bits = 3U;
    else if (scale != 1) return -1;

    if ((index_reg & 7) == 4 || (base_reg & 7) == 5) {
        return -1;
    }

    return append_rex(assembler, 1, dst_reg, index_reg, base_reg) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0x8DU) == 0 &&
           append_modrm(assembler, 0U, (unsigned int)dst_reg, 4U) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT,
                       (unsigned char)((scale_bits << 6U) | ((unsigned int)(index_reg & 7) << 3U) | (unsigned int)(base_reg & 7))) == 0 ? 0 : -1;
}

static int encode_lea_mem_reg(ObjectAssembler *assembler, int base_reg, int disp, int dst_reg) {
    if (append_rex(assembler, 1, dst_reg, 0, base_reg) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, 0x8DU) != 0) {
        return -1;
    }
    if (disp >= -128 && disp <= 127) {
        if (append_mem_modrm(assembler, 1U, (unsigned int)dst_reg, base_reg) != 0 ||
            append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)disp)) != 0) {
            return -1;
        }
        return 0;
    }
    if (append_mem_modrm(assembler, 2U, (unsigned int)dst_reg, base_reg) != 0 ||
        append_u32(assembler, OBJECT_SECTION_TEXT, (uint32_t)(int32_t)disp) != 0) {
        return -1;
    }
    return 0;
}

static int parse_symbol_rip(const char *operand, char *name_out, size_t name_size) {
    const char *rip = operand;
    size_t length = 0;

    while (*rip != '\0' && *rip != '(' && length + 1 < name_size) {
        name_out[length++] = *rip++;
    }
    name_out[length] = '\0';

    return starts_with(rip, "(%rip)") ? 0 : -1;
}

static int encode_riprel(ObjectAssembler *assembler, unsigned char opcode, int reg, const char *name, uint32_t reloc_type) {
    size_t disp_offset;

    if (append_rex(assembler, 1, reg, 0, 5) != 0 ||
        append_byte(assembler, OBJECT_SECTION_TEXT, opcode) != 0 ||
        append_modrm(assembler, 0U, (unsigned int)reg, 5U) != 0) {
        return -1;
    }

    disp_offset = assembler->text_size;
    if (append_u32(assembler, OBJECT_SECTION_TEXT, 0U) != 0 ||
        add_fixup(assembler, name, disp_offset, reloc_type, -4) != 0) {
        return -1;
    }

    return 0;
}

static int encode_setcc_al(ObjectAssembler *assembler, unsigned char opcode) {
    return append_byte(assembler, OBJECT_SECTION_TEXT, 0x0FU) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, opcode) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0xC0U) == 0 ? 0 : -1;
}

static int encode_call_reg(ObjectAssembler *assembler, int reg) {
    return append_rex(assembler, 0, 0, 0, reg) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0xFFU) == 0 &&
           append_modrm(assembler, 3U, 2U, (unsigned int)reg) == 0 ? 0 : -1;
}

static int encode_jmp_reg(ObjectAssembler *assembler, int reg) {
    return append_rex(assembler, 0, 0, 0, reg) == 0 &&
           append_byte(assembler, OBJECT_SECTION_TEXT, 0xFFU) == 0 &&
           append_modrm(assembler, 3U, 4U, (unsigned int)reg) == 0 ? 0 : -1;
}

static int assemble_instruction(ObjectAssembler *assembler, const char *line) {
    if (names_equal(line, "pushq %rbp")) return encode_push_reg(assembler, 5);
    if (names_equal(line, "pushq %rax")) return encode_push_reg(assembler, 0);
    if (names_equal(line, "popq %rax")) return encode_pop_reg(assembler, 0);
    if (names_equal(line, "leave")) return append_byte(assembler, OBJECT_SECTION_TEXT, 0xC9U);
    if (names_equal(line, "ret")) return append_byte(assembler, OBJECT_SECTION_TEXT, 0xC3U);
    if (names_equal(line, "cqto")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x99U) == 0 ? 0 : -1;
    }

    if (starts_with(line, "pushq ") || starts_with(line, "popq ")) {
        int reg;
        int is_byte;
        const char *rest;
        const char *operand = skip_spaces(line + (starts_with(line, "pushq ") ? 6 : 5));

        if (parse_register(operand, &reg, &is_byte, &rest) != 0 || *rest != '\0') {
            set_error(assembler->writer, "unsupported register push/pop form");
            return -1;
        }
        return starts_with(line, "pushq ") ? encode_push_reg(assembler, reg) : encode_pop_reg(assembler, reg);
    }

    if (starts_with(line, "subq $") || starts_with(line, "addq $")) {
        char operand[64];
        const char *args = line + 5;
        const char *comma = find_top_level_comma(args);
        size_t i = 0;
        long long value = 0;
        int dst_reg;
        int is_byte;
        const char *rest;
        unsigned int alu_op = starts_with(line, "addq $") ? 0U : 5U;

        if (comma == 0) {
            set_error(assembler->writer, "unsupported stack adjustment form");
            return -1;
        }
        while (args + i < comma && i + 1 < sizeof(operand)) {
            operand[i] = args[i];
            i += 1U;
        }
        operand[i] = '\0';

        if (parse_immediate_operand(operand + 1, &value) != 0 ||
            parse_register(skip_spaces(comma + 1), &dst_reg, &is_byte, &rest) != 0 || *rest != '\0') {
            set_error(assembler->writer, "unsupported stack adjustment immediate");
            return -1;
        }
        return encode_alu_imm_reg(assembler, alu_op, dst_reg, value);
    }

    if (starts_with(line, "call ")) {
        size_t disp_offset;
        const char *name = skip_spaces(line + 5);
        int reg;
        int is_byte;
        const char *rest;

        if (*name == '*' && parse_register(skip_spaces(name + 1), &reg, &is_byte, &rest) == 0 && *rest == '\0') {
            return encode_call_reg(assembler, reg);
        }

        if (append_byte(assembler, OBJECT_SECTION_TEXT, 0xE8U) != 0) {
            return -1;
        }
        disp_offset = assembler->text_size;
        if (append_u32(assembler, OBJECT_SECTION_TEXT, 0U) != 0 ||
            add_fixup(assembler, name, disp_offset, R_X86_64_PLT32, -4) != 0) {
            return -1;
        }
        return 0;
    }

    if (starts_with(line, "je ") || starts_with(line, "jne ") || starts_with(line, "jmp ")) {
        size_t disp_offset;
        const char *name = skip_spaces(line + (starts_with(line, "jmp ") ? 4 : 3));

        if (starts_with(line, "jmp ")) {
            int reg;
            int is_byte;
            const char *rest;

            if (*name == '*' && parse_register(skip_spaces(name + 1), &reg, &is_byte, &rest) == 0 && *rest == '\0') {
                return encode_jmp_reg(assembler, reg);
            }
            if (append_byte(assembler, OBJECT_SECTION_TEXT, 0xE9U) != 0) {
                return -1;
            }
        } else {
            if (append_byte(assembler, OBJECT_SECTION_TEXT, 0x0FU) != 0 ||
                append_byte(assembler, OBJECT_SECTION_TEXT, starts_with(line, "je ") ? 0x84U : 0x85U) != 0) {
                return -1;
            }
        }

        disp_offset = assembler->text_size;
        if (append_u32(assembler, OBJECT_SECTION_TEXT, 0U) != 0 ||
            add_fixup(assembler, name, disp_offset, R_X86_64_PC32, -4) != 0) {
            return -1;
        }
        return 0;
    }

    if (starts_with(line, "movq ")) {
        char left[128];
        char right[128];
        const char *operands = line + 5;
        const char *comma = find_top_level_comma(operands);
        size_t i = 0;
        int src_reg;
        int dst_reg;
        int is_byte;
        const char *rest;
        int disp;
        int base_reg;
        char symbol[COMPILER_IR_NAME_CAPACITY];
        long long imm = 0;

        if (comma == 0) {
            set_error(assembler->writer, "unsupported movq form");
            return -1;
        }
        while (operands + i < comma && i + 1 < sizeof(left)) {
            left[i] = operands[i];
            i += 1U;
        }
        left[i] = '\0';
        if (*comma != ',') {
            set_error(assembler->writer, "unsupported movq form");
            return -1;
        }
        rt_copy_string(right, sizeof(right), skip_spaces(comma + 1));

        if (left[0] == '$') {
            if (parse_signed_value(left + 1, &imm) != 0 ||
                parse_register(right, &dst_reg, &is_byte, &rest) != 0 || *rest != '\0') {
                set_error(assembler->writer, "unsupported immediate movq form");
                return -1;
            }
            return encode_mov_imm_reg(assembler, imm, dst_reg);
        }

        if (parse_register(left, &src_reg, &is_byte, &rest) == 0 && *rest == '\0') {
            if (parse_register(right, &dst_reg, &is_byte, &rest) == 0 && *rest == '\0') {
                return encode_mov_reg_reg(assembler, src_reg, dst_reg);
            }
            if (parse_local_mem(right, &disp, &base_reg) == 0) {
                return encode_mov_reg_mem(assembler, src_reg, base_reg, disp);
            }
            if (parse_symbol_rip(right, symbol, sizeof(symbol)) == 0) {
                return encode_riprel(assembler, 0x89U, src_reg, symbol, R_X86_64_PC32);
            }
        }

        if (parse_local_mem(left, &disp, &base_reg) == 0) {
            if (parse_register(right, &dst_reg, &is_byte, &rest) == 0 && *rest == '\0') {
                return encode_mov_mem_reg(assembler, base_reg, disp, dst_reg);
            }
        }

        if (parse_symbol_rip(left, symbol, sizeof(symbol)) == 0) {
            if (parse_register(right, &dst_reg, &is_byte, &rest) == 0 && *rest == '\0') {
                return encode_riprel(assembler, 0x8BU, dst_reg, symbol, R_X86_64_PC32);
            }
        }

        set_error(assembler->writer, "unsupported movq instruction in object writer");
        return -1;
    }

    if (starts_with(line, "movb ")) {
        char left[64];
        char right[128];
        const char *operands = line + 5;
        const char *comma = find_top_level_comma(operands);
        size_t i = 0;
        int src_reg;
        int is_byte;
        const char *rest;
        int disp;
        int base_reg;

        if (comma == 0) {
            set_error(assembler->writer, "unsupported movb form");
            return -1;
        }
        while (operands + i < comma && i + 1 < sizeof(left)) {
            left[i] = operands[i];
            i += 1U;
        }
        left[i] = '\0';
        if (*comma != ',') {
            set_error(assembler->writer, "unsupported movb form");
            return -1;
        }
        rt_copy_string(right, sizeof(right), skip_spaces(comma + 1));

        if (parse_register(left, &src_reg, &is_byte, &rest) == 0 && is_byte && *rest == '\0' &&
            parse_local_mem(right, &disp, &base_reg) == 0) {
            return encode_mov_reg_mem_byte(assembler, src_reg, base_reg, disp);
        }

        set_error(assembler->writer, "unsupported movb instruction in object writer");
        return -1;
    }

    if (starts_with(line, "movzbq ")) {
        char left[128];
        char right[64];
        const char *operands = line + 7;
        const char *comma = find_top_level_comma(operands);
        size_t i = 0;
        int dst_reg;
        int is_byte;
        const char *rest;
        int disp;
        int base_reg;

        if (comma == 0) {
            set_error(assembler->writer, "unsupported movzbq form");
            return -1;
        }
        while (operands + i < comma && i + 1 < sizeof(left)) {
            left[i] = operands[i];
            i += 1U;
        }
        left[i] = '\0';
        if (*comma != ',') {
            set_error(assembler->writer, "unsupported movzbq form");
            return -1;
        }
        rt_copy_string(right, sizeof(right), skip_spaces(comma + 1));

        if (parse_local_mem(left, &disp, &base_reg) == 0 &&
            parse_register(right, &dst_reg, &is_byte, &rest) == 0 && *rest == '\0') {
            return encode_movzx_mem_reg(assembler, base_reg, disp, dst_reg);
        }

        if (names_equal(left, "%al") && names_equal(right, "%rax")) {
            return append_rex(assembler, 1, 0, 0, 0) == 0 &&
                   append_byte(assembler, OBJECT_SECTION_TEXT, 0x0FU) == 0 &&
                   append_byte(assembler, OBJECT_SECTION_TEXT, 0xB6U) == 0 &&
                   append_byte(assembler, OBJECT_SECTION_TEXT, 0xC0U) == 0 ? 0 : -1;
        }

        set_error(assembler->writer, "unsupported movzbq instruction in object writer");
        return -1;
    }

    if (starts_with(line, "leaq ")) {
        char left[128];
        char right[64];
        const char *operands = line + 5;
        const char *comma = find_top_level_comma(operands);
        size_t i = 0;
        int dst_reg;
        int is_byte;
        const char *rest;
        int disp;
        int base_reg;
        int index_reg;
        int scale;
        char symbol[COMPILER_IR_NAME_CAPACITY];

        if (comma == 0) {
            set_error(assembler->writer, "unsupported leaq form");
            return -1;
        }
        while (operands + i < comma && i + 1 < sizeof(left)) {
            left[i] = operands[i];
            i += 1U;
        }
        left[i] = '\0';
        rt_copy_string(right, sizeof(right), skip_spaces(comma + 1));

        if (parse_register(right, &dst_reg, &is_byte, &rest) != 0 || *rest != '\0') {
            set_error(assembler->writer, "unsupported lea destination");
            return -1;
        }
        if (parse_local_mem(left, &disp, &base_reg) == 0) {
            return encode_lea_mem_reg(assembler, base_reg, disp, dst_reg);
        }
        if (parse_indexed_mem(left, &base_reg, &index_reg, &scale) == 0) {
            return encode_lea_indexed_reg(assembler, base_reg, index_reg, scale, dst_reg);
        }
        if (parse_symbol_rip(left, symbol, sizeof(symbol)) == 0) {
            return encode_riprel(assembler, 0x8DU, dst_reg, symbol, R_X86_64_PC32);
        }

        set_error(assembler->writer, "unsupported leaq instruction in object writer");
        return -1;
    }

    if (starts_with(line, "addq %") || starts_with(line, "subq %") || starts_with(line, "andq %") ||
        starts_with(line, "orq %") || starts_with(line, "xorq %")) {
        char left[32];
        char right[32];
        const char *operands = line;
        const char *comma;
        size_t i = 0;
        unsigned char opcode = 0x01U;
        int src_reg;
        int dst_reg;
        int is_byte;
        const char *rest;

        while (*operands != '\0' && *operands != ' ' && *operands != '\t') {
            operands += 1;
        }
        operands = skip_spaces(operands);
        comma = find_top_level_comma(operands);
        if (comma == 0) {
            set_error(assembler->writer, "unsupported binary register form");
            return -1;
        }

        while (operands + i < comma && i + 1 < sizeof(left)) {
            left[i] = operands[i];
            i += 1U;
        }
        left[i] = '\0';
        rt_copy_string(right, sizeof(right), skip_spaces(comma + 1));

        if (parse_register(left, &src_reg, &is_byte, &rest) != 0 || *rest != '\0' ||
            parse_register(right, &dst_reg, &is_byte, &rest) != 0 || *rest != '\0') {
            rt_copy_string(assembler->writer->error_message,
                           sizeof(assembler->writer->error_message),
                           "unsupported binary register instruction: ");
            rt_copy_string(assembler->writer->error_message + rt_strlen(assembler->writer->error_message),
                           sizeof(assembler->writer->error_message) - rt_strlen(assembler->writer->error_message),
                           line);
            return -1;
        }

        if (line[0] == 's') opcode = 0x29U;
        else if (line[0] == 'a' && line[1] == 'n') opcode = 0x21U;
        else if (line[0] == 'o') opcode = 0x09U;
        else if (line[0] == 'x') opcode = 0x31U;

        return append_rex(assembler, 1, src_reg, 0, dst_reg) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, opcode) == 0 &&
               append_modrm(assembler, 3U, (unsigned int)src_reg, (unsigned int)dst_reg) == 0 ? 0 : -1;
    }

    if (starts_with(line, "imulq ")) {
        int src_reg;
        int dst_reg;
        int is_byte;
        const char *rest;
        if (parse_register(line + 6, &src_reg, &is_byte, &rest) != 0 || !starts_with(rest, ", %") ||
            parse_register(rest + 2, &dst_reg, &is_byte, &rest) != 0 || *rest != '\0') {
            set_error(assembler->writer, "unsupported imulq form");
            return -1;
        }
        return append_rex(assembler, 1, dst_reg, 0, src_reg) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x0FU) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xAFU) == 0 &&
               append_modrm(assembler, 3U, (unsigned int)dst_reg, (unsigned int)src_reg) == 0 ? 0 : -1;
    }

    if (starts_with(line, "salq %cl, %rax")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xD3U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xE0U) == 0 ? 0 : -1;
    }

    if (starts_with(line, "sarq %cl, %rax")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xD3U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF8U) == 0 ? 0 : -1;
    }

    if (starts_with(line, "cmpq $")) {
        long long value;
        if (parse_immediate_operand(line + 6, &value) != 0) {
            set_error(assembler->writer, "unsupported compare immediate");
            return -1;
        }
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x83U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF8U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, (unsigned char)((int8_t)value)) == 0 ? 0 : -1;
    }

    if (names_equal(line, "cmpq %rcx, %rax")) {
        return append_rex(assembler, 1, 1, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x39U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xC8U) == 0 ? 0 : -1;
    }

    if (names_equal(line, "sete %al")) return encode_setcc_al(assembler, 0x94U);
    if (names_equal(line, "setne %al")) return encode_setcc_al(assembler, 0x95U);
    if (names_equal(line, "setl %al")) return encode_setcc_al(assembler, 0x9CU);
    if (names_equal(line, "setle %al")) return encode_setcc_al(assembler, 0x9EU);
    if (names_equal(line, "setg %al")) return encode_setcc_al(assembler, 0x9FU);
    if (names_equal(line, "setge %al")) return encode_setcc_al(assembler, 0x9DU);

    if (names_equal(line, "idivq %rcx")) {
        return append_rex(assembler, 1, 0, 0, 1) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF7U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF9U) == 0 ? 0 : -1;
    }

    if (names_equal(line, "negq %rax")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF7U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xD8U) == 0 ? 0 : -1;
    }

    if (names_equal(line, "notq %rax")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xF7U) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0xD0U) == 0 ? 0 : -1;
    }

    if (names_equal(line, "movq (%rax), %rax")) {
        return append_rex(assembler, 1, 0, 0, 0) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x8BU) == 0 &&
               append_byte(assembler, OBJECT_SECTION_TEXT, 0x00U) == 0 ? 0 : -1;
    }

    set_error(assembler->writer, "unsupported assembly pattern in object writer");
    return -1;
}

static int append_asciz_directive(ObjectAssembler *assembler, const char *text) {
    const char *cursor = skip_spaces(text);

    if (assembler->current_section != OBJECT_SECTION_DATA || *cursor != '"') {
        return -1;
    }

    cursor += 1;
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char byte = (unsigned char)*cursor;

        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 1;
            if (*cursor == 'n') byte = (unsigned char)'\n';
            else if (*cursor == 'r') byte = (unsigned char)'\r';
            else if (*cursor == 't') byte = (unsigned char)'\t';
            else if (*cursor == '0') byte = 0U;
            else byte = (unsigned char)*cursor;
        }

        if (append_byte(assembler, OBJECT_SECTION_DATA, byte) != 0) {
            return -1;
        }
        cursor += 1;
    }

    if (*cursor != '"') {
        return -1;
    }

    return append_byte(assembler, OBJECT_SECTION_DATA, 0U);
}

static int assemble_from_source(ObjectAssembler *assembler, const CompilerSource *source) {
    size_t pos = 0;

    assembler->current_section = OBJECT_SECTION_NONE;

    while (pos < source->size) {
        char raw_line[COMPILER_IR_LINE_CAPACITY];
        const char *line;
        size_t length = 0;

        while (pos < source->size && source->data[pos] != '\n' && length + 1 < sizeof(raw_line)) {
            raw_line[length++] = source->data[pos++];
        }
        raw_line[length] = '\0';
        if (pos < source->size && source->data[pos] == '\n') {
            pos += 1U;
        }

        line = skip_spaces(raw_line);
        if (line[0] == '\0') {
            continue;
        }

        if (names_equal(line, ".text")) {
            assembler->current_section = OBJECT_SECTION_TEXT;
            continue;
        }
        if (names_equal(line, ".data")) {
            assembler->current_section = OBJECT_SECTION_DATA;
            continue;
        }
        if (starts_with(line, ".globl ")) {
            int symbol_index = get_symbol(assembler, line + 7);
            if (symbol_index < 0) {
                return -1;
            }
            assembler->symbols[symbol_index].global = 1;
            continue;
        }
        if (line[0] == '.') {
            size_t line_len = string_length(line);
            if (line_len > 0 && line[line_len - 1] == ':') {
                char name[COMPILER_IR_NAME_CAPACITY];
                size_t i;
                size_t offset = (assembler->current_section == OBJECT_SECTION_TEXT) ? assembler->text_size : assembler->data_size;

                for (i = 0; i + 1 < line_len && i + 1 < sizeof(name); ++i) {
                    name[i] = line[i];
                }
                name[i] = '\0';
                if (add_label(assembler, name, assembler->current_section, offset) != 0) {
                    return -1;
                }
                if (assembler->current_section == OBJECT_SECTION_DATA) {
                    int symbol_index = get_symbol(assembler, name);
                    if (symbol_index < 0) {
                        return -1;
                    }
                    assembler->symbols[symbol_index].section = OBJECT_SECTION_DATA;
                    assembler->symbols[symbol_index].offset = assembler->data_size;
                    assembler->symbols[symbol_index].defined = 1;
                    assembler->symbols[symbol_index].is_function = 0;
                }
                continue;
            }
        }
        if (line[string_length(line) - 1] == ':') {
            char name[COMPILER_IR_NAME_CAPACITY];
            int symbol_index;
            size_t i;
            size_t line_len = string_length(line);

            for (i = 0; i + 1 < line_len && i + 1 < sizeof(name); ++i) {
                name[i] = line[i];
            }
            name[i] = '\0';

            symbol_index = get_symbol(assembler, name);
            if (symbol_index < 0) {
                return -1;
            }
            assembler->symbols[symbol_index].section = assembler->current_section;
            assembler->symbols[symbol_index].offset = (assembler->current_section == OBJECT_SECTION_TEXT) ? assembler->text_size : assembler->data_size;
            assembler->symbols[symbol_index].defined = 1;
            assembler->symbols[symbol_index].is_function = assembler->current_section == OBJECT_SECTION_TEXT;
            continue;
        }
        if (starts_with(line, ".quad ")) {
            long long value = 0;
            if (assembler->current_section != OBJECT_SECTION_DATA || parse_signed_value(line + 6, &value) != 0 ||
                append_u64(assembler, OBJECT_SECTION_DATA, (uint64_t)value) != 0) {
                set_error(assembler->writer, "unsupported .quad initializer");
                return -1;
            }
            continue;
        }
        if (starts_with(line, ".asciz ")) {
            if (append_asciz_directive(assembler, line + 7) != 0) {
                set_error(assembler->writer, "unsupported .asciz initializer");
                return -1;
            }
            continue;
        }

        if (assembler->current_section != OBJECT_SECTION_TEXT || assemble_instruction(assembler, line) != 0) {
            if (assembler->writer->error_message[0] == '\0') {
                set_error(assembler->writer, "unsupported assembly in object writer");
            }
            return -1;
        }
    }

    return 0;
}

static int resolve_fixups(ObjectAssembler *assembler) {
    size_t i;

    for (i = 0; i < assembler->fixup_count; ++i) {
        ObjectFixup *fixup = &assembler->fixups[i];
        int label_index = find_label(assembler, fixup->name);
        int symbol_index = find_symbol(assembler, fixup->name);

        if (label_index >= 0 && assembler->labels[label_index].section == OBJECT_SECTION_TEXT) {
            int64_t delta = (int64_t)assembler->labels[label_index].offset + fixup->addend - (int64_t)fixup->offset;
            patch_i32_le(assembler->text + fixup->offset, (int32_t)delta);
            continue;
        }

        if (symbol_index >= 0 &&
            assembler->symbols[symbol_index].defined &&
            assembler->symbols[symbol_index].section == OBJECT_SECTION_TEXT) {
            int64_t delta = (int64_t)assembler->symbols[symbol_index].offset + fixup->addend - (int64_t)fixup->offset;
            patch_i32_le(assembler->text + fixup->offset, (int32_t)delta);
            continue;
        }

        if (symbol_index < 0) {
            symbol_index = get_symbol(assembler, fixup->name);
            if (symbol_index < 0) {
                return -1;
            }
        }

        if (!assembler->symbols[symbol_index].defined) {
            assembler->symbols[symbol_index].global = 1;
        }

        if (add_relocation(assembler, fixup->name, fixup->offset, fixup->type, fixup->addend) != 0) {
            return -1;
        }
    }

    return 0;
}

static int add_string(unsigned char *buffer, size_t *size, size_t capacity, const char *text, uint32_t *offset_out) {
    size_t i = 0;
    size_t start = *size;

    if (*size + string_length(text) + 1 > capacity) {
        return -1;
    }

    while (text[i] != '\0') {
        buffer[*size] = (unsigned char)text[i];
        *size += 1U;
        i += 1U;
    }
    buffer[*size] = 0U;
    *size += 1U;
    *offset_out = (uint32_t)start;
    return 0;
}

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int build_elf_object(ObjectAssembler *assembler, int fd) {
    unsigned char file[OBJECT_WRITER_MAX_OUTPUT];
    unsigned char strtab[16384];
    unsigned char shstrtab[256];
    unsigned char symtab[OBJECT_WRITER_MAX_SYMBOLS * 24];
    unsigned char rela[OBJECT_WRITER_MAX_RELOCS * 24];
    size_t file_size = 0;
    size_t strtab_size = 1;
    size_t shstrtab_size = 1;
    size_t symtab_size = 0;
    size_t rela_size = 0;
    size_t text_offset;
    size_t data_offset;
    size_t rela_offset;
    size_t symtab_offset;
    size_t strtab_offset;
    size_t shstrtab_offset;
    size_t shoff;
    uint32_t sh_name_text;
    uint32_t sh_name_data;
    uint32_t sh_name_rela;
    uint32_t sh_name_symtab;
    uint32_t sh_name_strtab;
    uint32_t sh_name_shstrtab;
    size_t i;
    unsigned int sym_index = 3U;
    unsigned int local_symbol_count = 3U;
    const size_t section_count = 7U;

    file[0] = 0U;
    strtab[0] = 0U;
    shstrtab[0] = 0U;

    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".text", &sh_name_text) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }
    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".data", &sh_name_data) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }
    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".rela.text", &sh_name_rela) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }
    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".symtab", &sh_name_symtab) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }
    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".strtab", &sh_name_strtab) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }
    if (add_string(shstrtab, &shstrtab_size, sizeof(shstrtab), ".shstrtab", &sh_name_shstrtab) != 0) {
        set_error(assembler->writer, "failed to build ELF string tables");
        return -1;
    }

    if (resolve_fixups(assembler) != 0) {
        return -1;
    }

    for (i = 0; i < assembler->symbol_count; ++i) {
        if (add_string(strtab, &strtab_size, sizeof(strtab), assembler->symbols[i].name, &assembler->symbols[i].name_offset) != 0) {
            set_error(assembler->writer, "failed to build symbol string table");
            return -1;
        }
    }

    /* null symbol */
    rt_memset(symtab + symtab_size, 0, 24U);
    symtab_size += 24U;
    /* .text section symbol */
    rt_memset(symtab + symtab_size, 0, 24U);
    symtab[symtab_size + 4] = (unsigned char)((STB_LOCAL << 4) | STT_SECTION);
    write_u16_le(symtab + symtab_size + 6, 1U);
    symtab_size += 24U;
    /* .data section symbol */
    rt_memset(symtab + symtab_size, 0, 24U);
    symtab[symtab_size + 4] = (unsigned char)((STB_LOCAL << 4) | STT_SECTION);
    write_u16_le(symtab + symtab_size + 6, 2U);
    symtab_size += 24U;

    for (i = 0; i < assembler->symbol_count; ++i) {
        if (!assembler->symbols[i].global) {
            unsigned char *entry = symtab + symtab_size;
            ObjectSymbol *symbol = &assembler->symbols[i];

            symbol->sym_index = sym_index;
            sym_index += 1U;
            local_symbol_count += 1U;
            rt_memset(entry, 0, 24U);
            write_u32_le(entry + 0, symbol->name_offset);
            entry[4] = (unsigned char)((STB_LOCAL << 4) | (symbol->is_function ? STT_FUNC : STT_OBJECT));
            write_u16_le(entry + 6, symbol->defined ? (uint16_t)symbol->section : 0U);
            write_u64_le(entry + 8, (uint64_t)symbol->offset);
            write_u64_le(entry + 16, 0U);
            symtab_size += 24U;
        }
    }

    for (i = 0; i < assembler->symbol_count; ++i) {
        if (assembler->symbols[i].global) {
            unsigned char *entry = symtab + symtab_size;
            ObjectSymbol *symbol = &assembler->symbols[i];

            symbol->sym_index = sym_index;
            sym_index += 1U;
            rt_memset(entry, 0, 24U);
            write_u32_le(entry + 0, symbol->name_offset);
            entry[4] = (unsigned char)((STB_GLOBAL << 4) | (symbol->is_function ? STT_FUNC : STT_OBJECT));
            write_u16_le(entry + 6, symbol->defined ? (uint16_t)symbol->section : 0U);
            write_u64_le(entry + 8, (uint64_t)symbol->offset);
            write_u64_le(entry + 16, 0U);
            symtab_size += 24U;
        }
    }

    for (i = 0; i < assembler->reloc_count; ++i) {
        int symbol_index = find_symbol(assembler, assembler->relocs[i].name);
        if (symbol_index < 0) {
            set_error(assembler->writer, "relocation refers to unknown symbol");
            return -1;
        }

        write_u64_le(rela + rela_size + 0, (uint64_t)assembler->relocs[i].offset);
        write_u64_le(
            rela + rela_size + 8,
            (((uint64_t)assembler->symbols[symbol_index].sym_index) << 32U) | (uint64_t)assembler->relocs[i].type
        );
        write_i64_le(rela + rela_size + 16, assembler->relocs[i].addend);
        rela_size += 24U;
    }

    file_size = 64U;
    text_offset = align_up(file_size, 16U);
    while (file_size < text_offset) file[file_size++] = 0U;
    if (append_bytes(file, &file_size, sizeof(file), assembler->text, assembler->text_size) != 0) {
        set_error(assembler->writer, "ELF text section exceeded object writer capacity");
        return -1;
    }

    data_offset = align_up(file_size, 8U);
    while (file_size < data_offset) file[file_size++] = 0U;
    if (append_bytes(file, &file_size, sizeof(file), assembler->data, assembler->data_size) != 0) {
        set_error(assembler->writer, "ELF data section exceeded object writer capacity");
        return -1;
    }

    rela_offset = align_up(file_size, 8U);
    while (file_size < rela_offset) file[file_size++] = 0U;
    if (append_bytes(file, &file_size, sizeof(file), rela, rela_size) != 0) {
        set_error(assembler->writer, "ELF relocation section exceeded object writer capacity");
        return -1;
    }

    symtab_offset = align_up(file_size, 8U);
    while (file_size < symtab_offset) file[file_size++] = 0U;
    if (append_bytes(file, &file_size, sizeof(file), symtab, symtab_size) != 0) {
        set_error(assembler->writer, "ELF symbol table exceeded object writer capacity");
        return -1;
    }

    strtab_offset = file_size;
    if (append_bytes(file, &file_size, sizeof(file), strtab, strtab_size) != 0) {
        set_error(assembler->writer, "ELF string table exceeded object writer capacity");
        return -1;
    }

    shstrtab_offset = file_size;
    if (append_bytes(file, &file_size, sizeof(file), shstrtab, shstrtab_size) != 0) {
        set_error(assembler->writer, "ELF section-string table exceeded object writer capacity");
        return -1;
    }

    shoff = align_up(file_size, 8U);
    while (file_size < shoff) file[file_size++] = 0U;

    /* ELF header */
    rt_memset(file, 0, 64U);
    file[0] = 0x7fU;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = ELFCLASS64;
    file[5] = ELFDATA2LSB;
    file[6] = EV_CURRENT;
    file[7] = 0U;
    write_u16_le(file + 16, ET_REL);
    write_u16_le(file + 18, EM_X86_64);
    write_u32_le(file + 20, EV_CURRENT);
    write_u64_le(file + 24, 0U);
    write_u64_le(file + 32, 0U);
    write_u64_le(file + 40, (uint64_t)shoff);
    write_u32_le(file + 48, 0U);
    write_u16_le(file + 52, 64U);
    write_u16_le(file + 54, 0U);
    write_u16_le(file + 56, 0U);
    write_u16_le(file + 58, 64U);
    write_u16_le(file + 60, (uint16_t)section_count);
    write_u16_le(file + 62, 6U);

    /* section headers */
    for (i = 0; i < section_count; ++i) {
        rt_memset(file + shoff + (i * 64U), 0, 64U);
    }

    /* .text */
    write_u32_le(file + shoff + 64U + 0, sh_name_text);
    write_u32_le(file + shoff + 64U + 4, SHT_PROGBITS);
    write_u64_le(file + shoff + 64U + 8, SHF_ALLOC | SHF_EXECINSTR);
    write_u64_le(file + shoff + 64U + 24, (uint64_t)text_offset);
    write_u64_le(file + shoff + 64U + 32, (uint64_t)assembler->text_size);
    write_u64_le(file + shoff + 64U + 48, 16U);

    /* .data */
    write_u32_le(file + shoff + 128U + 0, sh_name_data);
    write_u32_le(file + shoff + 128U + 4, SHT_PROGBITS);
    write_u64_le(file + shoff + 128U + 8, SHF_ALLOC | SHF_WRITE);
    write_u64_le(file + shoff + 128U + 24, (uint64_t)data_offset);
    write_u64_le(file + shoff + 128U + 32, (uint64_t)assembler->data_size);
    write_u64_le(file + shoff + 128U + 48, 8U);

    /* .rela.text */
    write_u32_le(file + shoff + 192U + 0, sh_name_rela);
    write_u32_le(file + shoff + 192U + 4, SHT_RELA);
    write_u64_le(file + shoff + 192U + 24, (uint64_t)rela_offset);
    write_u64_le(file + shoff + 192U + 32, (uint64_t)rela_size);
    write_u32_le(file + shoff + 192U + 40, 4U);
    write_u32_le(file + shoff + 192U + 44, 1U);
    write_u64_le(file + shoff + 192U + 48, 8U);
    write_u64_le(file + shoff + 192U + 56, 24U);

    /* .symtab */
    write_u32_le(file + shoff + 256U + 0, sh_name_symtab);
    write_u32_le(file + shoff + 256U + 4, SHT_SYMTAB);
    write_u64_le(file + shoff + 256U + 24, (uint64_t)symtab_offset);
    write_u64_le(file + shoff + 256U + 32, (uint64_t)symtab_size);
    write_u32_le(file + shoff + 256U + 40, 5U);
    write_u32_le(file + shoff + 256U + 44, local_symbol_count);
    write_u64_le(file + shoff + 256U + 48, 8U);
    write_u64_le(file + shoff + 256U + 56, 24U);

    /* .strtab */
    write_u32_le(file + shoff + 320U + 0, sh_name_strtab);
    write_u32_le(file + shoff + 320U + 4, SHT_STRTAB);
    write_u64_le(file + shoff + 320U + 24, (uint64_t)strtab_offset);
    write_u64_le(file + shoff + 320U + 32, (uint64_t)strtab_size);
    write_u64_le(file + shoff + 320U + 48, 1U);

    /* .shstrtab */
    write_u32_le(file + shoff + 384U + 0, sh_name_shstrtab);
    write_u32_le(file + shoff + 384U + 4, SHT_STRTAB);
    write_u64_le(file + shoff + 384U + 24, (uint64_t)shstrtab_offset);
    write_u64_le(file + shoff + 384U + 32, (uint64_t)shstrtab_size);
    write_u64_le(file + shoff + 384U + 48, 1U);

    file_size = shoff + (section_count * 64U);
    return rt_write_all(fd, file, file_size);
}

void compiler_object_writer_init(CompilerObjectWriter *writer) {
    rt_memset(writer, 0, sizeof(*writer));
}

static int emit_backend_assembly_to_temp(
    CompilerObjectWriter *writer,
    CompilerTarget target,
    const CompilerIr *ir,
    char *temp_path,
    size_t temp_path_size
) {
    CompilerBackend backend;
    int temp_fd;

    compiler_backend_init(&backend, target);
    temp_fd = platform_create_temp_file(temp_path, temp_path_size, "/tmp/newos-ncc-asm-", 0600U);
    if (temp_fd < 0) {
        set_error(writer, "failed to create temporary assembly file");
        return -1;
    }

    if (compiler_backend_emit_assembly(&backend, ir, temp_fd) != 0) {
        (void)platform_close(temp_fd);
        (void)platform_remove_file(temp_path);
        set_error(writer, compiler_backend_error_message(&backend));
        return -1;
    }

    (void)platform_close(temp_fd);
    return 0;
}

int compiler_object_write_elf64_x86_64(CompilerObjectWriter *writer, const CompilerIr *ir, int fd) {
    CompilerSource source;
    ObjectAssembler assembler;
    char temp_path[COMPILER_PATH_CAPACITY];

    compiler_object_writer_init(writer);
    rt_memset(&assembler, 0, sizeof(assembler));
    assembler.writer = writer;

    if (emit_backend_assembly_to_temp(writer, COMPILER_TARGET_LINUX_X86_64, ir, temp_path, sizeof(temp_path)) != 0) {
        return -1;
    }

    if (compiler_load_source(temp_path, &source) != 0) {
        (void)platform_remove_file(temp_path);
        set_error(writer, "failed to read temporary assembly for object writing");
        return -1;
    }
    (void)platform_remove_file(temp_path);

    if (assemble_from_source(&assembler, &source) != 0) {
        return -1;
    }

    return build_elf_object(&assembler, fd);
}

int compiler_object_write_macho64_aarch64(CompilerObjectWriter *writer, const CompilerIr *ir, int fd) {
    CompilerSource object_source;
    char asm_path[COMPILER_PATH_CAPACITY];
    char object_path[COMPILER_PATH_CAPACITY];
    char *argv[] = {
        "clang",
        "-target",
        "arm64-apple-darwin",
        "-x",
        "assembler",
        "-c",
        asm_path,
        "-o",
        object_path,
        0
    };
    int object_fd;
    int pid;
    int exit_status = 0;

    compiler_object_writer_init(writer);

    if (emit_backend_assembly_to_temp(writer, COMPILER_TARGET_MACOS_AARCH64, ir, asm_path, sizeof(asm_path)) != 0) {
        return -1;
    }

    object_fd = platform_create_temp_file(object_path, sizeof(object_path), "/tmp/newos-ncc-obj-", 0600U);
    if (object_fd < 0) {
        (void)platform_remove_file(asm_path);
        set_error(writer, "failed to create temporary Mach-O object file");
        return -1;
    }
    (void)platform_close(object_fd);

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        (void)platform_remove_file(asm_path);
        (void)platform_remove_file(object_path);
        set_error(writer, "failed to invoke clang for Mach-O object emission");
        return -1;
    }
    if (platform_wait_process(pid, &exit_status) != 0) {
        (void)platform_remove_file(asm_path);
        (void)platform_remove_file(object_path);
        set_error(writer, "failed while waiting for Mach-O object emission");
        return -1;
    }
    (void)platform_remove_file(asm_path);

    if (exit_status != 0) {
        (void)platform_remove_file(object_path);
        set_error(writer, "clang could not assemble macOS AArch64 output");
        return -1;
    }

    if (compiler_load_source(object_path, &object_source) != 0) {
        (void)platform_remove_file(object_path);
        set_error(writer, "failed to read generated Mach-O object");
        return -1;
    }
    (void)platform_remove_file(object_path);

    if (rt_write_all(fd, object_source.data, object_source.size) != 0) {
        set_error(writer, "failed while writing Mach-O object output");
        return -1;
    }

    return 0;
}

int compiler_object_write_target(CompilerObjectWriter *writer, CompilerTarget target, const CompilerIr *ir, int fd) {
    const CompilerTargetInfo *info = compiler_target_get_info(target);

    if (info == 0) {
        set_error(writer, "unknown compilation target");
        return -1;
    }

    if (info->object_format == COMPILER_OBJECT_FORMAT_ELF64 && !info->is_aarch64) {
        return compiler_object_write_elf64_x86_64(writer, ir, fd);
    }
    if (info->object_format == COMPILER_OBJECT_FORMAT_MACHO64 && info->is_aarch64) {
        return compiler_object_write_macho64_aarch64(writer, ir, fd);
    }

    set_error(writer, "object emission is not implemented yet for this target");
    return -1;
}

const char *compiler_object_writer_error_message(const CompilerObjectWriter *writer) {
    return writer->error_message;
}
