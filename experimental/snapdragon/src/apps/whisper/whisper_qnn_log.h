#ifndef WHISPER_QNN_LOG_H
#define WHISPER_QNN_LOG_H

static u32 qnn_log_lock;
static int qnn_log_restoring;
static char qnn_restore_messages[8192];
static u32 qnn_restore_size;
static int qnn_restore_overflow;

static void qnn_log_acquire(void) {
    while (__atomic_exchange_n(&qnn_log_lock, 1U, __ATOMIC_ACQUIRE)) {}
}

static void qnn_log_release(void) {
    __atomic_store_n(&qnn_log_lock, 0U, __ATOMIC_RELEASE);
}

static u32 qnn_log_format(char *output, u32 capacity, const char *format, va_list args) {
    u32 used = 0U;
    const char *original = format;
    while (*format != 0 && used + 1U < capacity) {
        if (*format != '%') {
            output[used++] = *format++;
            continue;
        }
        ++format;
        int zero = 0;
        int left = 0;
        while (*format == '0' || *format == '-') {
            if (*format == '0') zero = 1;
            if (*format == '-') left = 1;
            ++format;
        }
        u32 width = 0U;
        while (*format >= '0' && *format <= '9') {
            width = width < capacity ? width * 10U + (u32)(*format - '0') : capacity;
            ++format;
        }
        int precision = -1;
        if (*format == '.') {
            ++format;
            precision = 0;
            if (*format == '*') { precision = va_arg(args, int); ++format; }
            else while (*format >= '0' && *format <= '9') {
                if (precision < (int)capacity) precision = precision * 10 + *format - '0';
                ++format;
            }
        }
        int length = 0;
        if (*format == 'l') {
            length = 1;
            ++format;
            if (*format == 'l') { length = 2; ++format; }
        } else if (*format == 'z' || *format == 't' || *format == 'j') {
            length = 2;
            ++format;
        } else if (*format == 'h') {
            ++format;
            if (*format == 'h') ++format;
        }
        char spec = *format;
        if (spec != 0) ++format;
        char digits[66];
        const char *text = digits;
        u32 count = 0U;
        if (spec == 's') {
            text = va_arg(args, const char *);
            if (text == 0) text = "(null)";
            while (count < capacity && (precision < 0 || count < (u32)precision) && text[count]) ++count;
        } else if (spec == 'c' || spec == '%') {
            digits[count++] = spec == '%' ? '%' : (char)va_arg(args, int);
        } else if (spec == 'd' || spec == 'i' || spec == 'u' ||
                   spec == 'x' || spec == 'X' || spec == 'p' || spec == 'o') {
            u64 value;
            int negative = 0;
            if (spec == 'p') value = (usize)va_arg(args, void *);
            else if (spec == 'd' || spec == 'i') {
                long long signed_value = length == 2 ? va_arg(args, long long) :
                    (length == 1 ? va_arg(args, long) : va_arg(args, int));
                negative = signed_value < 0;
                value = negative ? 0U - (u64)signed_value : (u64)signed_value;
            } else value = length == 2 ? va_arg(args, unsigned long long) :
                (length == 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned int));
            u32 base = spec == 'o' ? 8U :
                (spec == 'x' || spec == 'X' || spec == 'p' ? 16U : 10U);
            const char *alphabet = spec == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            u32 end = sizeof(digits);
            do { digits[--end] = alphabet[value % base]; value /= base; } while (value);
            if (negative) digits[--end] = '-';
            text = digits + end;
            count = sizeof(digits) - end;
        } else {
            used = 0U;
            const char *notice = "[unformatted QNN diagnostic] ";
            while (*notice && used + 1U < capacity) output[used++] = *notice++;
            while (*original && used + 1U < capacity) output[used++] = *original++;
            break;
        }
        if (width > capacity) width = capacity;
        if (!left) while (width > count && used + 1U < capacity) {
            output[used++] = zero ? '0' : ' ';
            --width;
        }
        for (u32 index = 0U; index < count && used + 1U < capacity; ++index) output[used++] = text[index];
        if (left) while (width > count && used + 1U < capacity) { output[used++] = ' '; --width; }
    }
    if (capacity != 0U) output[used] = 0;
    return used;
}

static int qnn_log_contains(const char *text, const char *part) {
    for (; *text; ++text) {
        u32 index = 0U;
        while (part[index] && text[index] == part[index]) ++index;
        if (!part[index]) return 1;
    }
    return 0;
}

static int qnn_log_placement(const char *text) {
    if (qnn_log_contains(text, "[unformatted QNN diagnostic]")) return 0;
    return qnn_log_contains(text, "contextFromBin (submit) Failed code:") ||
        qnn_log_contains(text, "Skel failed to process context binary.") ||
        qnn_log_contains(text, "Context create from binary failed for deviceId") ||
        (qnn_log_contains(text, "Context ") && qnn_log_contains(text, " failed on pd "));
}

static void qnn_log_emit(const char *text, u32 size) {
    u32 written;
    WriteFile(original_stderr_handle, text, size, &written, 0);
}

static void qnn_log_callback(const char *format, u32 level, u64 timestamp, va_list args) {
    char message[2048];
    (void)timestamp;
    message[0] = '<';
    message[1] = level == QNN_LOG_LEVEL_ERROR ? 'E' : 'W';
    message[2] = '>';
    message[3] = ' ';
    u32 size = 4U + qnn_log_format(message + 4U, sizeof(message) - 5U, format, args);
    u32 body = 4U;
    while (body < size && message[body] == ' ') ++body;
    if (body + 3U <= size && message[body] == '<' &&
        message[body + 1U] == message[1] && message[body + 2U] == '>') {
        body += 3U;
        while (body < size && message[body] == ' ') ++body;
        for (u32 index = body; index < size; ++index) message[4U + index - body] = message[index];
        size = 4U + size - body;
    }
    if (message[size - 1U] != '\n') message[size++] = '\n';
    message[size] = 0;
    qnn_log_acquire();
    if (qnn_log_restoring && level == QNN_LOG_LEVEL_ERROR && qnn_log_placement(message)) {
        if (qnn_restore_size + size <= sizeof(qnn_restore_messages)) {
            for (u32 index = 0U; index < size; ++index) qnn_restore_messages[qnn_restore_size++] = message[index];
        } else {
            qnn_restore_overflow = 1;
            qnn_log_emit(message, size);
        }
    } else qnn_log_emit(message, size);
    qnn_log_release();
}

static void qnn_log_begin_restore(void) {
    qnn_log_acquire();
    qnn_restore_size = 0U;
    qnn_restore_overflow = 0;
    qnn_log_restoring = 1;
    qnn_log_release();
}

static void qnn_log_end_restore(u64 status) {
    qnn_log_acquire();
    qnn_log_restoring = 0;
    if (status != 0U || qnn_restore_overflow) qnn_log_emit(qnn_restore_messages, qnn_restore_size);
    else if (qnn_restore_size != 0U && !quiet_output) {
        static const char notice[] = "<W> QNN recovered an initial context placement failure using another process domain.\n";
        qnn_log_emit(notice, sizeof(notice) - 1U);
    }
    qnn_log_release();
}

#endif