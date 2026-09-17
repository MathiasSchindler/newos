#include <stddef.h>
#include "ocr_image.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long Result;
typedef struct { int left, top, right, bottom; } Rect;
typedef struct { void *window; u32 message; u64 word; Result data; u32 time; int x, y; u32 private_data; } Message;
typedef struct {
    u32 size, style; Result (*procedure)(void *,u32,u64,Result); int class_bytes, window_bytes;
    void *instance, *icon, *cursor, *background; const u16 *menu, *name; void *small_icon;
} WindowClass;
typedef struct {
    u32 size; void *owner, *instance; const u16 *filter; u16 *custom_filter; u32 custom_max, filter_index;
    u16 *file; u32 file_max; u16 *file_title; u32 title_max; const u16 *directory, *title;
    u32 flags; u16 file_offset, extension_offset; const u16 *extension; Result custom;
    void *hook; const u16 *template_name; void *reserved; u32 reserved_value, extended_flags;
} OpenFile;
typedef struct { u32 size; void *descriptor; int inherit; } Security;
typedef struct {
    u32 size; u16 *reserved, *desktop, *title; u32 x,y,width,height,cols,rows,fill,flags;
    u16 show, reserved_size; u8 *reserved_data; void *input, *output, *error;
} Startup;
typedef struct { void *process, *thread; u32 process_id, thread_id; } Process;
typedef struct { void *dc; int erase; Rect rect; int restore, update; u8 reserved[32]; } Paint;
typedef struct { u32 size; int width,height; u16 planes,bits; u32 compression,bytes; int x,y; u32 used,important; } Bitmap;

#define IMPORT __declspec(dllimport)
#define W(value) ((const u16 *)L##value)
IMPORT u16 RegisterClassExW(const WindowClass *);
IMPORT void *CreateWindowExW(u32,const u16 *,const u16 *,u32,int,int,int,int,void *,void *,void *,void *);
IMPORT Result DefWindowProcW(void *,u32,u64,Result);
IMPORT int GetMessageW(Message *,void *,u32,u32);
IMPORT int TranslateMessage(const Message *);
IMPORT Result DispatchMessageW(const Message *);
IMPORT int IsDialogMessageW(void *,Message *);
IMPORT Result SendMessageW(void *,u32,u64,Result);
IMPORT int SetWindowTextW(void *,const u16 *);
IMPORT int GetWindowTextW(void *,u16 *,int);
IMPORT int EnableWindow(void *,int);
IMPORT int MoveWindow(void *,int,int,int,int,int);
IMPORT int GetClientRect(void *,Rect *);
IMPORT int ShowWindow(void *,int);
IMPORT int DestroyWindow(void *);
IMPORT void PostQuitMessage(int);
IMPORT void *LoadCursorW(void *,const u16 *);
IMPORT void *LoadIconW(void *,const u16 *);
IMPORT int SetProcessDpiAwarenessContext(void *);
IMPORT u32 GetDpiForWindow(void *);
IMPORT int SetWindowPos(void *,void *,int,int,int,int,u32);
IMPORT u64 SetTimer(void *,u64,u32,void *);
IMPORT int KillTimer(void *,u64);
IMPORT void *BeginPaint(void *,Paint *);
IMPORT int EndPaint(void *,const Paint *);
IMPORT int InvalidateRect(void *,const Rect *,int);
IMPORT int FillRect(void *,const Rect *,void *);
IMPORT void *CreateFontW(int,int,int,int,int,u32,u32,u32,u32,u32,u32,u32,u32,const u16 *);
IMPORT int DeleteObject(void *);
IMPORT int SetStretchBltMode(void *,int);
IMPORT int StretchDIBits(void *,int,int,int,int,int,int,int,int,const void *,const Bitmap *,u32,u32);
IMPORT int GetOpenFileNameW(OpenFile *);
IMPORT void ExitProcess(u32);
IMPORT void *VirtualAlloc(void *,u64,u32,u32);
IMPORT int VirtualFree(void *,u64,u32);
IMPORT int CloseHandle(void *);
IMPORT void *CreateFileW(const u16 *,u32,u32,void *,u32,u32,void *);
IMPORT int GetFileSizeEx(void *,Result *);
IMPORT int ReadFile(void *,void *,u32,u32 *,void *);
IMPORT u32 GetModuleFileNameW(void *,u16 *,u32);
IMPORT u32 GetCurrentProcessId(void);
IMPORT u64 GetTickCount64(void);
IMPORT int CreateDirectoryW(const u16 *,void *);
IMPORT int CreateProcessW(const u16 *,u16 *,void *,void *,int,u32,void *,const u16 *,Startup *,Process *);
IMPORT int GetExitCodeProcess(void *,u32 *);
IMPORT int TerminateProcess(void *,u32);
IMPORT u32 WaitForSingleObject(void *,u32);
IMPORT int MultiByteToWideChar(u32,u32,const char *,int,u16 *,int);

void *memset(void *destination,int value,size_t size) {
    u8 *bytes = destination; for (size_t index = 0; index < size; ++index) bytes[index] = (u8)value; return destination;
}
void *memcpy(void *destination,const void *source,size_t size) {
    u8 *output = destination; const u8 *input = source;
    for (size_t index = 0; index < size; ++index) output[index] = input[index]; return destination;
}

static void *window, *path_box, *open_button, *mode_box, *run_button, *cancel_button, *copy_button, *output_box, *status_box, *font;
static void *labels[3], *process;
static u16 base[32768], image_path[32768], capture_path[32768], stdout_path[32768], command[32768];
static u8 text_bytes[98304];
static u16 text_wide[196608];
static u8 *pixels;
static u32 image_width, image_height, dpi = 96, command_size, displayed_bytes;
static u64 started;
static Rect preview;
static int cancelled;

static int append(u16 *target,const u16 *source,u32 capacity) {
    u32 length = 0; while (length < capacity && target[length]) ++length;
    for (u32 index = 0;; ++index) {
        if (length+1 >= capacity) return 0;
        target[length++] = source[index]; if (!source[index]) return 1;
    }
}
static int path(u16 *target,const u16 *suffix) { target[0] = 0; return append(target,base,32768) && append(target,suffix,32768); }
static void number(u16 *target,u64 value) {
    u16 reversed[24]; u32 count = 0;
    do { reversed[count++] = (u16)('0'+value%10); value /= 10; } while (value);
    for (u32 index = 0; index < count; ++index) target[index] = reversed[count-index-1]; target[count] = 0;
}
static int argument(const u16 *value) {
    if (command_size+4 >= 32768) return 0;
    if (command_size) command[command_size++] = ' ';
    command[command_size++] = '"';
    for (u32 index = 0;;) {
        u32 slashes = 0; while (value[index] == '\\') { ++slashes; ++index; }
        u32 copies = value[index] == '"' || !value[index] ? slashes*2 : slashes;
        if (command_size+copies+4 >= 32768) return 0;
        while (copies--) command[command_size++] = '\\';
        if (!value[index]) break;
        if (value[index] == '"') command[command_size++] = '\\';
        command[command_size++] = value[index++];
    }
    command[command_size++] = '"'; command[command_size] = 0; return 1;
}
static int scale(int value) { return value*(int)dpi/96; }
static void controls(void) {
    EnableWindow(path_box,!process); EnableWindow(open_button,!process); EnableWindow(mode_box,!process);
    EnableWindow(run_button,!process && pixels != 0); EnableWindow(cancel_button,process != 0 && !cancelled);
    EnableWindow(copy_button,displayed_bytes != 0);
}
static void layout(void) {
    Rect bounds; if (!GetClientRect(window,&bounds)) return;
    int margin = scale(20), gap = scale(12), width = bounds.right-2*margin, button = scale(112);
    MoveWindow(labels[0],margin,scale(14),width,scale(24),1);
    MoveWindow(path_box,margin,scale(40),width-button-gap,scale(30),1);
    MoveWindow(open_button,margin+width-button,scale(40),button,scale(30),1);
    preview = (Rect){margin,scale(82),margin+width,scale(270)};
    MoveWindow(labels[1],margin,scale(282),scale(68),scale(26),1);
    MoveWindow(mode_box,margin+scale(70),scale(280),width-scale(70),scale(180),1);
    MoveWindow(run_button,margin,scale(320),button,scale(34),1);
    MoveWindow(cancel_button,margin+button+gap,scale(320),button,scale(34),1);
    MoveWindow(copy_button,margin+width-button,scale(320),button,scale(34),1);
    MoveWindow(labels[2],margin,scale(368),width,scale(24),1);
    MoveWindow(output_box,margin,scale(394),width,bounds.bottom-scale(440),1);
    MoveWindow(status_box,margin,bounds.bottom-scale(34),width,scale(28),1);
    InvalidateRect(window,0,1);
}
static void set_font(void) {
    void *old = font; font = CreateFontW(-scale(16),0,0,0,400,0,0,0,1,0,0,5,0,W("Segoe UI"));
    void *items[] = {path_box,open_button,mode_box,run_button,cancel_button,copy_button,output_box,status_box,labels[0],labels[1],labels[2]};
    for (u32 index = 0; index < sizeof(items)/sizeof(items[0]); ++index) SendMessageW(items[index],0x30,(u64)font,1);
    if (old) DeleteObject(old);
}
static int load_image(void) {
    if (pixels) VirtualFree(pixels,0,0x8000); pixels = 0; image_width = image_height = 0;
    GetWindowTextW(path_box,image_path,32768);
    void *file = CreateFileW(image_path,0x80000000,1,0,3,0x80,0);
    Result size = 0; u8 *encoded = 0; u32 read = 0; int good = 0;
    if (file == (void *)-1) goto done;
    if (!GetFileSizeEx(file,&size) || size < 54 || size > 64*1024*1024) goto done;
    encoded = VirtualAlloc(0,(u64)size,0x3000,4);
    if (!encoded || !ReadFile(file,encoded,(u32)size,&read,0) || read != size) goto done;
    int (*decode)(const u8 *,u64,u8 *,u64,u32 *,u32 *) = encoded[0] == 137 ? ocr_image_png : ocr_image_bmp;
    if (!decode(encoded,(u64)size,0,0,&image_height,&image_width)) goto done;
    OcrImageShape shape;
        if (!ocr_image_fit_shape(image_height,image_width,1,&shape)) {
                SetWindowTextW(status_box,W("Image dimensions exceed supported limits.")); good = -1; goto done;
    }
    u64 bytes = (u64)image_width*image_height*3, stride = ((u64)image_width*3+3)&~3ULL;
    pixels = VirtualAlloc(0,stride*image_height,0x3000,4);
    if (!pixels || !decode(encoded,(u64)size,pixels,bytes,&image_height,&image_width)) goto done;
    for (u32 row = image_height; row-- > 0;) {
        for (u32 column = image_width; column-- > 0;) {
            u64 source = ((u64)row*image_width+column)*3, target = row*stride+column*3;
            u8 red = pixels[source], green = pixels[source+1], blue = pixels[source+2];
            pixels[target] = blue; pixels[target+1] = green; pixels[target+2] = red;
        }
    }
    good = 1;
done:
    if (file != (void *)-1) CloseHandle(file);
    if (encoded) VirtualFree(encoded,0,0x8000);
    if (good != 1 && pixels) { VirtualFree(pixels,0,0x8000); pixels = 0; }
    if (!good) SetWindowTextW(status_box,W("Cannot open image. Static PNG or BMP24 required."));
    if (good == 1) SetWindowTextW(status_box,W("Ready"));
    controls(); InvalidateRect(window,&preview,1); return good == 1;
}
static void open_image(void) {
    u16 selected[32768] = {0}; OpenFile dialog = {0}; dialog.size = sizeof(dialog); dialog.owner = window;
    dialog.filter = W("Images (*.png;*.bmp)\0*.png;*.bmp\0All files\0*.*\0");
    dialog.file = selected; dialog.file_max = 32768; dialog.flags = 0x1808;
    if (GetOpenFileNameW(&dialog)) { SetWindowTextW(path_box,selected); load_image(); }
}
static void refresh_output(void) {
    void *file = CreateFileW(stdout_path,0x80000000,7,0,3,0x80,0);
    Result size = 0; u32 read = 0;
    if (file == (void *)-1) return;
    if (GetFileSizeEx(file,&size) && size > 0 && (u64)size < sizeof(text_bytes) && size != displayed_bytes &&
        ReadFile(file,text_bytes,(u32)size,&read,0) && read == size) {
        int count = MultiByteToWideChar(65001,8,(const char *)text_bytes,read,text_wide,98304);
        if (count > 0) {
            u32 expanded = (u32)count;
            for (int index = 0; index < count; ++index) if (text_wide[index] == '\n' && (!index || text_wide[index-1] != '\r')) ++expanded;
            text_wide[expanded] = 0;
            for (int index = count; index > 0;) {
                u16 value = text_wide[--index]; text_wide[--expanded] = value;
                if (value == '\n' && (!index || text_wide[index-1] != '\r')) text_wide[--expanded] = '\r';
            }
            SetWindowTextW(output_box,text_wide); displayed_bytes = read;
        }
    }
    CloseHandle(file);
}
static void tick(void) {
    if (!process) return;
    refresh_output();
    if (WaitForSingleObject(process,0) == 0) {
        u32 code = 1; GetExitCodeProcess(process,&code); CloseHandle(process); process = 0; KillTimer(window,1);
        SetWindowTextW(status_box,cancelled ? W("Cancelled; partial output.") : code == 0 ? W("Ready") :
                       code == 3 ? W("Incomplete: token/context limit.") : W("OCR failed. See the run log."));
    } else {
        u16 status[96] = {0}, elapsed[24]; number(elapsed,(GetTickCount64()-started)/1000);
        append(status,W("Recognizing... "),96); append(status,elapsed,96); append(status,W(" s"),96); SetWindowTextW(status_box,status);
    }
    controls();
}
static void submit(void) {
    if (process || !load_image()) return;
    u16 engine[32768], asset[32768], suffix[96] = {0}, serial[24], log_path[32768];
    path(engine,W("ocr-generate.exe")); path(capture_path,W("runs")); CreateDirectoryW(capture_path,0);
    number(serial,GetTickCount64()); append(suffix,W("runs\\"),96); append(suffix,serial,96);
    append(suffix,W("-"),96); number(serial,GetCurrentProcessId()); append(suffix,serial,96);
    if (!path(capture_path,suffix) || !CreateDirectoryW(capture_path,0)) { SetWindowTextW(status_box,W("Cannot create run directory.")); return; }
    stdout_path[0] = 0; log_path[0] = 0;
    append(stdout_path,capture_path,32768); append(stdout_path,W("\\stdout.txt"),32768);
    append(log_path,capture_path,32768); append(log_path,W("\\execution.log"),32768);
    command_size = 0; Result mode = SendMessageW(mode_box,0x147,0,0);
    int good = argument(engine) && argument(mode == 1 ? W("--generate-formula") : mode == 2 ? W("--generate-table") : W("--generate-text"));
    const u16 *assets[] = {W("..\\QnnHtp.dll"),W("..\\..\\models\\glm-ocr-vision-v2"),W("..\\..\\models\\glm-ocr-text-v1"),W("..\\..\\models\\glm-ocr-generation-v2")};
    for (u32 index = 0; good && index < 4; ++index) good = path(asset,assets[index]) && argument(asset);
    good = good && argument(image_path) && argument(capture_path) && argument(W("256"));
    Security security = {sizeof(security),0,1};
    void *output = CreateFileW(stdout_path,0x40000000,1,&security,1,0x80,0);
    void *error = CreateFileW(log_path,0x40000000,1,&security,1,0x80,0);
    void *input = CreateFileW(W("NUL"),0x80000000,1,&security,3,0x80,0);
    Startup startup = {0}; Process child = {0}; startup.size = sizeof(startup); startup.flags = 0x100;
    startup.input = input; startup.output = output; startup.error = error;
    good = good && output != (void *)-1 && error != (void *)-1 && input != (void *)-1 &&
           CreateProcessW(engine,command,0,0,1,0x08000000,0,0,&startup,&child);
    if (output != (void *)-1) CloseHandle(output);
    if (error != (void *)-1) CloseHandle(error);
    if (input != (void *)-1) CloseHandle(input);
    if (!good) { SetWindowTextW(status_box,W("Cannot start OCR. Check engine and run directory.")); return; }
    CloseHandle(child.thread); process = child.process; cancelled = 0; started = GetTickCount64(); displayed_bytes = 0;
    SetWindowTextW(output_box,W("")); SetWindowTextW(status_box,W("Recognizing...")); controls();
    if (!SetTimer(window,1,150,0)) { TerminateProcess(process,1); WaitForSingleObject(process,0xffffffff); tick(); }
}
static Result procedure(void *handle,u32 message,u64 word,Result data) {
    switch (message) {
        case 5: if (path_box) layout(); return 0;
        case 0x24: { int *limits = (int *)data; limits[6] = scale(460); limits[7] = scale(620); return 0; }
        case 0x2e0: {
            dpi = (u32)word&65535; Rect *rect = (Rect *)data; set_font();
            SetWindowPos(handle,0,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,0x14); layout(); return 0;
        }
        case 0xf: {
            Paint paint; void *dc = BeginPaint(handle,&paint); FillRect(dc,&preview,(void *)6);
            if (pixels && image_width && image_height) {
                int width = preview.right-preview.left, height = preview.bottom-preview.top;
                if ((u64)width*image_height > (u64)height*image_width) width = (int)((u64)height*image_width/image_height);
                else height = (int)((u64)width*image_height/image_width);
                Bitmap bitmap = {40,(int)image_width,-(int)image_height,1,24,0,0,0,0,0,0}; SetStretchBltMode(dc,4);
                StretchDIBits(dc,(preview.left+preview.right-width)/2,(preview.top+preview.bottom-height)/2,width,height,
                               0,0,image_width,image_height,pixels,&bitmap,0,0x00cc0020);
            }
            EndPaint(handle,&paint); return 0;
        }
        case 0x111:
            if ((word&65535) == 101 && word>>16 == 0x200 && !process) load_image();
            if (word>>16 != 0) return 0;
            if ((word&65535) == 102 && !process) open_image();
            if ((word&65535) == 105) submit();
            if ((word&65535) == 107 && process && TerminateProcess(process,4)) { cancelled = 1; controls(); }
            if ((word&65535) == 108) { SendMessageW(output_box,0xb1,0,-1); SendMessageW(output_box,0x301,0,0); }
            return 0;
        case 0x113: tick(); return 0;
        case 0x10:
            if (process) { TerminateProcess(process,4); WaitForSingleObject(process,0xffffffff); CloseHandle(process); process = 0; }
            DestroyWindow(handle); return 0;
        case 2: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(handle,message,word,data);
}
static void *control(const u16 *kind,const u16 *label,u32 style,u32 id) {
    return CreateWindowExW(id == 101 || id == 104 ? 0x200 : 0,kind,label,0x50000000|style,0,0,10,10,window,(void *)(u64)id,0,0);
}
void mainCRTStartup(void) {
    u32 count = GetModuleFileNameW(0,base,32768); if (!count || count >= 32768) ExitProcess(1);
    while (count && base[count-1] != '\\') --count; base[count] = 0;
    SetProcessDpiAwarenessContext((void *)-4);
    WindowClass definition = {0}; definition.size = sizeof(definition); definition.procedure = procedure;
    definition.name = W("NewosGlmOcr"); definition.background = (void *)16;
    definition.cursor = LoadCursorW(0,(const u16 *)32512); definition.icon = LoadIconW(0,(const u16 *)32512);
    if (!RegisterClassExW(&definition)) ExitProcess(1);
    window = CreateWindowExW(0x10000,definition.name,W("OCR"),0x02cf0000,(int)0x80000000,(int)0x80000000,720,760,0,0,0,0);
    if (!window) ExitProcess(1); dpi = GetDpiForWindow(window);
    labels[0] = control(W("STATIC"),W("&Image"),0,201);
    path_box = control(W("EDIT"),W(""),0x10080,101);
    open_button = control(W("BUTTON"),W("&Open..."),0x10000,102);
    labels[1] = control(W("STATIC"),W("&Mode"),0,202);
    mode_box = control(W("COMBOBOX"),W(""),0x210003,103);
    const u16 *modes[] = {W("Text"),W("Formula"),W("Table")};
    for (u32 index = 0; index < 3; ++index) SendMessageW(mode_box,0x143,0,(Result)modes[index]);
    SendMessageW(mode_box,0x14e,0,0);
    run_button = control(W("BUTTON"),W("&Recognize"),0x10001,105);
    cancel_button = control(W("BUTTON"),W("&Cancel"),0x10000,107);
    copy_button = control(W("BUTTON"),W("&Copy"),0x10000,108);
    labels[2] = control(W("STATIC"),W("&Output"),0,203);
    output_box = control(W("EDIT"),W(""),0x211844,104);
    status_box = control(W("STATIC"),W("Ready"),0,106);
    if (!path_box || !open_button || !mode_box || !run_button || !cancel_button || !copy_button || !output_box || !status_box) ExitProcess(1);
    SendMessageW(path_box,0xc5,32767,0); SendMessageW(output_box,0xc5,196607,0);
    set_font(); SetWindowPos(window,0,0,0,scale(720),scale(760),0x16); layout(); controls(); ShowWindow(window,5);
    Message message; int received;
    while ((received = GetMessageW(&message,0,0,0)) > 0) {
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (pixels) VirtualFree(pixels,0,0x8000); if (font) DeleteObject(font); ExitProcess(received < 0 ? 1 : 0);
}