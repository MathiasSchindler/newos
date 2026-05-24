extern const char *helper_message(void);

__attribute__((noreturn)) static void sys_exit(long code) {
    register long x0 __asm__("x0") = code;
    register long x16 __asm__("x16") = 1;
    __asm__ volatile("svc #0x80" : : "r"(x0), "r"(x16) : "memory");
    for (;;) {}
}

static long sys_write(long fd, const void *buf, unsigned long len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x16 __asm__("x16") = 4;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}

static unsigned long c_strlen(const char *text) {
    unsigned long len = 0;
    while (text[len] != '\0') {
        len += 1;
    }
    return len;
}

__attribute__((noreturn)) void _start(void) {
    const char *msg = helper_message();
    (void)sys_write(1, msg, c_strlen(msg));
    sys_exit(0);
}