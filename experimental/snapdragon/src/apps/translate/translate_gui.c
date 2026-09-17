#define GEMMA_GUI 1
#include "translate.c"
#undef mainCRTStartup

typedef long long GuiResult;
typedef struct GuiRect { i32 left, top, right, bottom; } GuiRect;
typedef struct GuiMessage { void *window; u32 message; u64 word; long long data; u32 time; i32 x, y; u32 private_data; } GuiMessage;
typedef struct GuiClass {
    u32 size, style;
    GuiResult (*procedure)(void *, u32, u64, long long);
    i32 class_bytes, window_bytes;
    void *instance, *icon, *cursor, *background;
    const u16 *menu, *name;
    void *small_icon;
} GuiClass;

__declspec(dllimport) u16 RegisterClassExW(const GuiClass *);
__declspec(dllimport) void *CreateWindowExW(u32, const u16 *, const u16 *, u32, i32, i32, i32, i32, void *, void *, void *, void *);
__declspec(dllimport) GuiResult DefWindowProcW(void *, u32, u64, long long);
__declspec(dllimport) int GetMessageW(GuiMessage *, void *, u32, u32);
__declspec(dllimport) int TranslateMessage(const GuiMessage *);
__declspec(dllimport) GuiResult DispatchMessageW(const GuiMessage *);
__declspec(dllimport) int IsDialogMessageW(void *, GuiMessage *);
__declspec(dllimport) int PostMessageW(void *, u32, u64, long long);
__declspec(dllimport) GuiResult SendMessageW(void *, u32, u64, long long);
__declspec(dllimport) int SetWindowTextW(void *, const u16 *);
__declspec(dllimport) int GetWindowTextW(void *, u16 *, int);
__declspec(dllimport) int GetWindowTextLengthW(void *);
__declspec(dllimport) int EnableWindow(void *, int);
__declspec(dllimport) int MoveWindow(void *, i32, i32, i32, i32, int);
__declspec(dllimport) int GetClientRect(void *, GuiRect *);
__declspec(dllimport) int ShowWindow(void *, int);
__declspec(dllimport) int DestroyWindow(void *);
__declspec(dllimport) void PostQuitMessage(int);
__declspec(dllimport) void *LoadCursorW(void *, const u16 *);
__declspec(dllimport) void *LoadIconW(void *, const u16 *);
__declspec(dllimport) void *SetFocus(void *);
__declspec(dllimport) int SetProcessDpiAwarenessContext(void *);
__declspec(dllimport) u32 GetDpiForWindow(void *);
__declspec(dllimport) int SetWindowPos(void *, void *, i32, i32, i32, i32, u32);
__declspec(dllimport) void *CreateFontW(i32, i32, i32, i32, i32, u32, u32, u32, u32, u32, u32, u32, u32, const u16 *);
__declspec(dllimport) int DeleteObject(void *);

#define GW(text) ((const u16 *)L##text)
enum { GUI_READY = 0x8001, GUI_TEXT, GUI_DONE, GUI_FINISHED };
static void *gui_window, *gui_input, *gui_output, *gui_from, *gui_to, *gui_button, *gui_status;
static void *gui_labels[4], *gui_font, *gui_signal, *gui_thread;
static u32 gui_dpi = 96, gui_stop, gui_exit_code;
static int gui_loaded, gui_busy, gui_closing, gui_exited;
static char gui_codes[581][32], gui_source[32], gui_target[32], gui_request[GEMMA_TOKENIZER_MAX_BYTES];
static u16 gui_names[581][256], gui_input_wide[GEMMA_TOKENIZER_MAX_BYTES];
static u32 gui_language_count;

static int gui_cancelled(void) { return (int)__atomic_load_n(&gui_stop, __ATOMIC_ACQUIRE); }

static int gui_next(void) {
    if (WaitForSingleObject(gui_signal, 0xffffffffU) != 0 || gui_cancelled()) return 0;
    source_language = gui_source; target_language = gui_target; source_text = gui_request;
    return 1;
}

static int gui_write(const u8 *bytes, u32 size) {
    if (gui_cancelled()) return 0;
    u16 *wide = VirtualAlloc(0, ((u64)size * 2 + 1) * sizeof(u16), 0x3000U, 4U);
    if (!wide) return 0;
    int count = MultiByteToWideChar(65001, 8, (const char *)bytes, (int)size, wide, (int)size);
    if (count <= 0) { VirtualFree(wide, 0, 0x8000U); return 0; }
    u32 expanded = (u32)count;
    for (int index = 0; index < count; ++index) if (wide[index] == '\n' && (!index || wide[index - 1] != '\r')) ++expanded;
    wide[expanded] = 0;
    for (int index = count; index > 0;) {
        u16 value = wide[--index]; wide[--expanded] = value;
        if (value == '\n' && (!index || wide[index - 1] != '\r')) wide[--expanded] = '\r';
    }
    if (!PostMessageW(gui_window, GUI_TEXT, 0, (long long)wide)) { VirtualFree(wide, 0, 0x8000U); return 0; }
    return 1;
}

static void gui_ready(void) {
    gui_language_count = tokenizer.language_count;
    for (u32 index = 0; index < gui_language_count; ++index) {
        const u8 *record = tokenizer.languages + index * 16;
        u32 code_size = read32(record + 4), name_size = read32(record + 12);
        if (code_size >= 32) { gui_language_count = 0; break; }
        memcpy(gui_codes[index], tokenizer.strings + read32(record), code_size);
        int count = MultiByteToWideChar(65001, 8, (const char *)tokenizer.strings + read32(record + 8),
                                       (int)name_size, gui_names[index], 210);
        if (count <= 0) { gui_language_count = 0; break; }
        gui_names[index][count++] = ' '; gui_names[index][count++] = '(';
        for (u32 offset = 0; offset < code_size; ++offset) gui_names[index][count++] = (u16)gui_codes[index][offset];
        gui_names[index][count++] = ')'; gui_names[index][count] = 0;
    }
    PostMessageW(gui_window, GUI_READY, 0, 0);
}

static void gui_done(int result) { PostMessageW(gui_window, GUI_DONE, (u64)(long long)result, 0); }
static void gui_finished(u32 result) { PostMessageW(gui_window, GUI_FINISHED, result, 0); }
static u32 gui_worker(void *unused) { (void)unused; translate_engine_main(); return 0; }
static i32 gui_scale(i32 value) { return (i32)((u32)value * gui_dpi / 96); }

static void gui_controls(int enabled) {
    EnableWindow(gui_from, enabled); EnableWindow(gui_to, enabled);
    EnableWindow(gui_input, !gui_closing);
    EnableWindow(gui_button, enabled && GetWindowTextLengthW(gui_input) > 0);
}

static void gui_layout(void) {
    GuiRect rect; if (!GetClientRect(gui_window, &rect)) return;
    i32 margin = gui_scale(20), gap = gui_scale(12), width = rect.right - margin * 2;
    i32 half_width = (width - gap) / 2, top = gui_scale(112);
    i32 editor_height = (rect.bottom - gui_scale(234)) / 2;
    if (editor_height < gui_scale(40)) editor_height = gui_scale(40);
    MoveWindow(gui_labels[0], margin, gui_scale(16), half_width, gui_scale(24), 1);
    MoveWindow(gui_labels[1], margin + half_width + gap, gui_scale(16), half_width, gui_scale(24), 1);
    MoveWindow(gui_from, margin, gui_scale(42), half_width, gui_scale(320), 1);
    MoveWindow(gui_to, margin + half_width + gap, gui_scale(42), half_width, gui_scale(320), 1);
    MoveWindow(gui_labels[2], margin, gui_scale(84), width, gui_scale(24), 1);
    MoveWindow(gui_input, margin, top, width, editor_height, 1);
    top += editor_height + gap;
    MoveWindow(gui_button, margin + width - gui_scale(120), top, gui_scale(120), gui_scale(34), 1);
    top += gui_scale(44);
    MoveWindow(gui_labels[3], margin, top, width, gui_scale(24), 1);
    top += gui_scale(26);
    MoveWindow(gui_output, margin, top, width, editor_height, 1);
    MoveWindow(gui_status, margin, rect.bottom - gui_scale(30), width, gui_scale(24), 1);
}

static void gui_set_font(void) {
    void *old = gui_font;
    gui_font = CreateFontW(-gui_scale(16), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, GW("Segoe UI"));
    void *controls[] = {gui_input, gui_output, gui_from, gui_to, gui_button, gui_status,
                       gui_labels[0], gui_labels[1], gui_labels[2], gui_labels[3]};
    for (u32 index = 0; index < 10; ++index) SendMessageW(controls[index], 0x30, (u64)gui_font, 1);
    if (old) DeleteObject(old);
}

static void gui_submit(void) {
    if (!gui_loaded || gui_busy || gui_closing) return;
    int size = GetWindowTextW(gui_input, gui_input_wide, GEMMA_TOKENIZER_MAX_BYTES);
    if (size <= 0) return;
    GuiResult from = SendMessageW(gui_from, 0x147, 0, 0), to = SendMessageW(gui_to, 0x147, 0, 0);
    if (from < 0 || to < 0) return;
    from = SendMessageW(gui_from, 0x150, (u64)from, 0); to = SendMessageW(gui_to, 0x150, (u64)to, 0);
    if ((u64)from >= gui_language_count || (u64)to >= gui_language_count) return;
    if (!WideCharToMultiByte(65001, 0x80, gui_input_wide, -1, gui_request, sizeof(gui_request), 0, 0)) {
        SetWindowTextW(gui_status, GW("Input too long or invalid Unicode.")); return;
    }
    join(gui_source, gui_codes[from], ""); join(gui_target, gui_codes[to], "");
    gui_busy = 1; gui_controls(0); SetWindowTextW(gui_output, GW(""));
    SetWindowTextW(gui_status, GW("Translating..."));
    if (!ReleaseSemaphore(gui_signal, 1, 0)) {
        gui_busy = 0; gui_controls(1); SetWindowTextW(gui_status, GW("Cannot submit translation."));
    }
}

static GuiResult gui_procedure(void *window, u32 message, u64 word, long long data) {
    switch (message) {
        case 0x5: gui_layout(); return 0;
        case 0x24: {
            i32 *limits = (i32 *)data; limits[6] = gui_scale(420); limits[7] = gui_scale(460); return 0;
        }
        case 0x2e0: {
            gui_dpi = (u32)word & 65535; GuiRect *rect = (GuiRect *)data;
            gui_set_font(); SetWindowPos(window, 0, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, 0x14);
            gui_layout(); return 0;
        }
        case 0x111:
            if ((word & 65535) == 105 && (word >> 16) == 0) gui_submit();
            if ((word & 65535) == 103 && (word >> 16) == 0x300) gui_controls(gui_loaded && !gui_busy && !gui_closing);
            return 0;
        case GUI_READY:
            if (gui_closing) return 0;
            if (!gui_language_count) { SetWindowTextW(gui_status, GW("Invalid language table. Restart required.")); return 0; }
            for (u32 index = 0; index < gui_language_count; ++index) {
                void *combos[] = {gui_from, gui_to};
                for (u32 side = 0; side < 2; ++side) {
                    GuiResult item = SendMessageW(combos[side], 0x143, 0, (long long)gui_names[index]);
                    SendMessageW(combos[side], 0x151, (u64)item, index);
                }
            }
            for (u32 side = 0; side < 2; ++side) {
                void *combo = side ? gui_to : gui_from;
                for (u32 item = 0; item < gui_language_count; ++item) {
                    u64 index = (u64)SendMessageW(combo, 0x150, item, 0);
                    if (index < gui_language_count && equal(gui_codes[index], side ? "en" : "de")) SendMessageW(combo, 0x14e, item, 0);
                }
            }
            gui_loaded = 1; gui_controls(1); SetWindowTextW(gui_status, GW("Ready")); return 0;
        case GUI_TEXT:
            if (!gui_closing) {
                SendMessageW(gui_output, 0xb1, (u64)-1, -1);
                SendMessageW(gui_output, 0xc2, 0, data);
            }
            VirtualFree((void *)data, 0, 0x8000U); return 0;
        case GUI_DONE:
            gui_busy = 0;
            if (!gui_closing) {
                if (!word) gui_loaded = 0;
                gui_controls(gui_loaded);
                SetWindowTextW(gui_status, (long long)word == -1 ? GW("Invalid or oversized input.") :
                               word == 2 ? GW("Cannot complete a text segment.") :
                               word == 1 ? GW("Ready") : GW("Translation failed. Restart required."));
            }
            return 0;
        case GUI_FINISHED:
            WaitForSingleObject(gui_thread, 0xffffffffU); gui_exited = 1; gui_loaded = 0; gui_exit_code = (u32)word;
            if (gui_closing) DestroyWindow(window);
            else { gui_controls(0); SetWindowTextW(gui_status, GW("Model failed. Check model/QNN assets.")); }
            return 0;
        case 0x10:
            if (gui_exited) { DestroyWindow(window); return 0; }
            gui_closing = 1; __atomic_store_n(&gui_stop, 1, __ATOMIC_RELEASE);
            gui_controls(0); SetWindowTextW(gui_status, GW("Closing...")); ReleaseSemaphore(gui_signal, 1, 0); return 0;
        case 0x2: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, word, data);
}

static void *gui_control(const u16 *kind, const u16 *label, u32 style, u32 id) {
    return CreateWindowExW(id == 103 || id == 104 ? 0x200 : 0,
                           kind, label, 0x50000000U | style, 0, 0, 10, 10, gui_window, (void *)(u64)id, 0, 0);
}

void mainCRTStartup(void) {
    SetProcessDpiAwarenessContext((void *)(long long)-4);
    GuiClass definition = {0}; definition.size = sizeof(definition); definition.procedure = gui_procedure;
    definition.name = GW("NewosTranslateGemma"); definition.background = (void *)16;
    definition.cursor = LoadCursorW(0, (const u16 *)32512); definition.icon = LoadIconW(0, (const u16 *)32512);
    if (!RegisterClassExW(&definition)) ExitProcess(1);
    gui_window = CreateWindowExW(0x10000, definition.name, GW("Translate"), 0x00cf0000U,
                                 (i32)0x80000000U, (i32)0x80000000U, 720, 680, 0, 0, 0, 0);
    if (!gui_window) ExitProcess(1);
    gui_dpi = GetDpiForWindow(gui_window);
    SetWindowPos(gui_window, 0, 0, 0, gui_scale(720), gui_scale(680), 0x16);
    gui_labels[0] = gui_control(GW("STATIC"), GW("&From"), 0, 201);
    gui_from = gui_control(GW("COMBOBOX"), GW(""), 0x00210303, 101);
    gui_labels[1] = gui_control(GW("STATIC"), GW("&To"), 0, 202);
    gui_to = gui_control(GW("COMBOBOX"), GW(""), 0x00210303, 102);
    gui_labels[2] = gui_control(GW("STATIC"), GW("&Input"), 0, 203);
    gui_input = gui_control(GW("EDIT"), GW(""), 0x00211044, 103);
    gui_button = gui_control(GW("BUTTON"), GW("&Translate"), 0x10001, 105);
    gui_labels[3] = gui_control(GW("STATIC"), GW("&Output"), 0, 204);
    gui_output = gui_control(GW("EDIT"), GW(""), 0x00210844, 104);
    gui_status = gui_control(GW("STATIC"), GW("Loading model..."), 0, 106);
    if (!gui_input || !gui_output || !gui_from || !gui_to || !gui_button || !gui_status) ExitProcess(1);
    SendMessageW(gui_input, 0xc5, GEMMA_TOKENIZER_MAX_BYTES - 1, 0);
    SendMessageW(gui_output, 0xc5, sizeof(document_output) * 2 + 2, 0);
    gui_set_font(); gui_layout(); gui_controls(0);
    gui_signal = CreateSemaphoreA(0, 0, 2, 0); if (!gui_signal) ExitProcess(1);
    ShowWindow(gui_window, 5); SetFocus(gui_input);
    gui_thread = CreateThread(0, 0, gui_worker, 0, 0, 0); if (!gui_thread) ExitProcess(1);
    GuiMessage message; int received;
    while ((received = GetMessageW(&message, 0, 0, 0)) > 0) {
        if (!IsDialogMessageW(gui_window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (!gui_exited) {
        __atomic_store_n(&gui_stop, 1, __ATOMIC_RELEASE); ReleaseSemaphore(gui_signal, 1, 0);
        WaitForSingleObject(gui_thread, 0xffffffffU);
    }
    CloseHandle(gui_thread); CloseHandle(gui_signal); if (gui_font) DeleteObject(gui_font);
    ExitProcess(received < 0 ? 1 : gui_closing ? 0 : gui_exit_code);
}