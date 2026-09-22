#include "whisper_tensor_index.h"

typedef struct JsonCursor {
    const unsigned char *at;
    const unsigned char *end;
} JsonCursor;

static void spaces(JsonCursor *cursor) {
    while (cursor->at < cursor->end &&
           (*cursor->at == ' ' || *cursor->at == '\t' || *cursor->at == '\r' || *cursor->at == '\n')) {
        ++cursor->at;
    }
}

static int consume(JsonCursor *cursor, unsigned char expected) {
    spaces(cursor);
    if (cursor->at == cursor->end || *cursor->at != expected) return 0;
    ++cursor->at;
    return 1;
}

static int string(JsonCursor *cursor, char *output, unsigned int capacity) {
    unsigned int used = 0;
    if (!consume(cursor, '"') || capacity == 0) return 0;
    while (cursor->at < cursor->end && *cursor->at != '"') {
        unsigned char letter = *cursor->at++;
        if (letter == '\\') {
            if (cursor->at == cursor->end) return 0;
            letter = *cursor->at++;
            if (letter != '"' && letter != '\\' && letter != '/') return 0;
        }
        if (letter < 0x20 || letter >= 0x80 || used + 1 >= capacity) return 0;
        output[used++] = (char)letter;
    }
    if (cursor->at == cursor->end) return 0;
    ++cursor->at;
    output[used] = 0;
    return 1;
}

static int same(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static int number(JsonCursor *cursor, unsigned long long *value) {
    unsigned long long result = 0;
    spaces(cursor);
    if (cursor->at == cursor->end || *cursor->at < '0' || *cursor->at > '9') return 0;
    if (*cursor->at == '0' && cursor->at + 1 < cursor->end &&
        cursor->at[1] >= '0' && cursor->at[1] <= '9') return 0;
    while (cursor->at < cursor->end && *cursor->at >= '0' && *cursor->at <= '9') {
        unsigned int digit = *cursor->at++ - '0';
        if (result > (0xffffffffffffffffULL - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}

static int pair(JsonCursor *cursor, WhisperTensorIndex *entry) {
    unsigned long long shape_count = 1;
    char field[32];
    char dtype[16];
    unsigned int seen = 0;
    if (!consume(cursor, '{')) return 0;
    do {
        if (!string(cursor, field, sizeof(field)) || !consume(cursor, ':')) return 0;
        if (same(field, "dtype")) {
            if ((seen & 1) || !string(cursor, dtype, sizeof(dtype))) return 0;
            entry->type = same(dtype, "F32") ? WHISPER_TENSOR_F32 :
                same(dtype, "F16") ? WHISPER_TENSOR_F16 : 0;
            if (entry->type == 0) return 0;
            seen |= 1;
        } else if (same(field, "shape")) {
            if ((seen & 2) || !consume(cursor, '[')) return 0;
            entry->rank = 0;
            spaces(cursor);
            if (!consume(cursor, ']')) {
                do {
                    unsigned long long dimension;
                    if (entry->rank == WHISPER_TENSOR_MAX_RANK ||
                        !number(cursor, &dimension) || dimension == 0 ||
                        shape_count > 0xffffffffffffffffULL / dimension) return 0;
                    entry->shape[entry->rank++] = dimension;
                    shape_count *= dimension;
                    spaces(cursor);
                    if (consume(cursor, ']')) break;
                } while (consume(cursor, ','));
                if (cursor->at == cursor->end || cursor->at[-1] != ']') return 0;
            }
            seen |= 2;
        } else if (same(field, "data_offsets")) {
            if ((seen & 4) || !consume(cursor, '[') || !number(cursor, &entry->start) ||
                !consume(cursor, ',') || !number(cursor, &entry->end) ||
                !consume(cursor, ']')) return 0;
            seen |= 4;
        } else return 0;
        spaces(cursor);
        if (consume(cursor, '}')) break;
    } while (consume(cursor, ','));
    if (cursor->at == cursor->end || cursor->at[-1] != '}' || seen != 7 ||
        entry->end <= entry->start || shape_count > 0xffffffffffffffffULL /
            (entry->type == WHISPER_TENSOR_F32 ? 4U : 2U)) return 0;
    return entry->end - entry->start == shape_count *
        (entry->type == WHISPER_TENSOR_F32 ? 4U : 2U);
}

int whisper_tensor_index_parse(
    const unsigned char *json, unsigned int length,
    WhisperTensorIndex *entries, unsigned int capacity, unsigned int *count_out
) {
    JsonCursor cursor;
    unsigned int count = 0;
    int saw_metadata = 0;
    int closed = 0;
    if (json == 0 || entries == 0 || count_out == 0 || length == 0 ||
        capacity == 0 || capacity > WHISPER_TENSOR_MAX_COUNT) return 0;
    cursor.at = json;
    cursor.end = json + length;
    if (!consume(&cursor, '{')) return 0;
    do {
        char name[WHISPER_TENSOR_NAME_SIZE];
        WhisperTensorIndex *entry;
        if (!string(&cursor, name, sizeof(name)) || name[0] == 0 ||
            !consume(&cursor, ':')) return 0;
        if (same(name, "__metadata__")) {
            char key[WHISPER_TENSOR_NAME_SIZE];
            char value[WHISPER_TENSOR_NAME_SIZE];
            if (saw_metadata || !consume(&cursor, '{')) return 0;
            spaces(&cursor);
            if (!consume(&cursor, '}')) {
                do {
                    if (!string(&cursor, key, sizeof(key)) || !consume(&cursor, ':') ||
                        !string(&cursor, value, sizeof(value))) return 0;
                    spaces(&cursor);
                    if (consume(&cursor, '}')) break;
                } while (consume(&cursor, ','));
                if (cursor.at == cursor.end || cursor.at[-1] != '}') return 0;
            }
            saw_metadata = 1;
        } else {
            if (count == capacity) return 0;
            entry = entries + count;
            for (unsigned int index = 0; index < WHISPER_TENSOR_MAX_RANK; ++index) entry->shape[index] = 0;
            unsigned int index = 0;
            while ((entry->name[index] = name[index]) != 0) ++index;
            if (!pair(&cursor, entry)) return 0;
            for (unsigned int previous = 0; previous < count; ++previous) {
                if (same(entry->name, entries[previous].name)) return 0;
            }
            ++count;
        }
        spaces(&cursor);
        if (consume(&cursor, '}')) { closed = 1; break; }
    } while (consume(&cursor, ','));
    if (!closed || count == 0) return 0;
    spaces(&cursor);
    if (cursor.at != cursor.end) return 0;
    *count_out = count;
    return 1;
}

int whisper_tensor_index_validate(
    const WhisperTensorIndex *entries, unsigned int count, unsigned long long data_size
) {
    unsigned long long covered = 0;
    if (entries == 0 || count == 0 || count > WHISPER_TENSOR_MAX_COUNT) return 0;
    for (unsigned int index = 0; index < count; ++index) {
        if (entries[index].end > data_size ||
            covered > data_size - (entries[index].end - entries[index].start)) return 0;
        covered += entries[index].end - entries[index].start;
        for (unsigned int other = 0; other < index; ++other) {
            if (entries[index].start < entries[other].end &&
                entries[other].start < entries[index].end) return 0;
        }
    }
    return covered == data_size;
}