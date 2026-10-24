#include <stdint.h>

uint32_t __hexagon_udivsi3(uint32_t, uint32_t);
uint32_t __hexagon_umodsi3(uint32_t, uint32_t);
uint64_t __hexagon_udivdi3(uint64_t, uint64_t);
__declspec(dllimport) void ExitProcess(uint32_t);
__declspec(dllimport) void *GetStdHandle(uint32_t);
__declspec(dllimport) int WriteFile(void *, const void *, uint32_t, uint32_t *, void *);

void mainCRTStartup(void) {
    uint64_t state = 0x2e14527b1c834fedULL;
    uint32_t i, written;
    int ok = __hexagon_udivsi3(1, 0) == UINT32_MAX &&
             __hexagon_udivdi3(1, 0) == UINT64_MAX;
    for (i = 0; i < 20000; ++i) {
        uint64_t a, b;
        state = state * 6364136223846793005ULL + 1;
        a = state;
        state = state * 6364136223846793005ULL + 1;
        b = i < 64 ? (uint64_t)1 << i : state;
        if (!b) b = 1;
        if (__hexagon_udivdi3(a, b) != a / b) ok = 0;
        {
            uint32_t x = (uint32_t)a, y = (uint32_t)b;
            if (!y) y = 1;
            if (__hexagon_udivsi3(x, y) != x / y ||
                __hexagon_umodsi3(x, y) != x % y) ok = 0;
        }
    }
    {
        static const char pass[] = "FP32 compiler-helper tests: 60002 checks PASS\n";
        static const char fail[] = "FP32 compiler-helper tests FAIL\n";
        WriteFile(GetStdHandle((uint32_t)-11), ok ? pass : fail,
                  ok ? sizeof(pass) - 1 : sizeof(fail) - 1, &written, 0);
    }
    ExitProcess(ok ? 0 : 1);
}
