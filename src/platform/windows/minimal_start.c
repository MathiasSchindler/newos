__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

int main(int argc, char **argv);

__attribute__((noreturn, no_stack_protector))
void mainCRTStartup(void) {
    ExitProcess((unsigned int)main(0, 0));
    for (;;) {
    }
}
