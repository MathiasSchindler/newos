#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    const char *description;
    const char *mime;
} FileTypeInfo;

static unsigned char ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (unsigned char)(ch - 'A' + 'a');
    }
    return ch;
}

static int starts_with_ci(const unsigned char *buffer, size_t length, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (i >= length || ascii_lower(buffer[i]) != ascii_lower((unsigned char)text[i])) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int find_text_ci(const unsigned char *buffer, size_t length, const char *needle) {
    size_t i;
    size_t needle_length = rt_strlen(needle);

    if (needle_length == 0U || needle_length > length) {
        return 0;
    }

    for (i = 0; i + needle_length <= length; ++i) {
        size_t j;
        int match = 1;
        for (j = 0; j < needle_length; ++j) {
            if (ascii_lower(buffer[i + j]) != ascii_lower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

static int looks_like_text(const unsigned char *buffer, size_t length) {
    size_t i;

    if (length == 0U) {
        return 1;
    }

    for (i = 0; i < length; ++i) {
        unsigned char ch = buffer[i];
        if (ch == 0U) {
            return 0;
        }
        if ((ch < 32U || ch > 126U) && ch != '\n' && ch != '\r' && ch != '\t') {
            return 0;
        }
    }

    return 1;
}

static int looks_like_json(const unsigned char *buffer, size_t length) {
    size_t start = 0;
    size_t end = length;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }
    while (end > start && (buffer[end - 1U] == ' ' || buffer[end - 1U] == '\n' || buffer[end - 1U] == '\r' || buffer[end - 1U] == '\t')) {
        end -= 1U;
    }

    if (end <= start) {
        return 0;
    }

    return (buffer[start] == '{' && buffer[end - 1U] == '}') || (buffer[start] == '[' && buffer[end - 1U] == ']');
}

static int looks_like_html(const unsigned char *buffer, size_t length) {
    size_t start = 0;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }

    if (start >= length) {
        return 0;
    }

    return starts_with_ci(buffer + start, length - start, "<!doctype html") ||
           starts_with_ci(buffer + start, length - start, "<html") ||
           starts_with_ci(buffer + start, length - start, "<head") ||
           starts_with_ci(buffer + start, length - start, "<body");
}

static int looks_like_xml(const unsigned char *buffer, size_t length) {
    size_t start = 0;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }

    if (start >= length) {
        return 0;
    }

    return starts_with_ci(buffer + start, length - start, "<?xml") || starts_with_ci(buffer + start, length - start, "<svg");
}

static FileTypeInfo detect_type(const unsigned char *buffer, size_t length) {
    FileTypeInfo info;

    info.description = "data";
    info.mime = "application/octet-stream";

    if (length == 0U) {
        info.description = "empty";
        info.mime = "application/x-empty";
        return info;
    }
    if (length >= 4U && buffer[0] == 0x7fU && buffer[1] == 'E' && buffer[2] == 'L' && buffer[3] == 'F') {
        info.description = "ELF binary";
        info.mime = "application/x-executable";
        return info;
    }
    if (length >= 2U && buffer[0] == 0x1fU && buffer[1] == 0x8bU) {
        info.description = "gzip compressed data";
        info.mime = "application/gzip";
        return info;
    }
    if (length >= 6U && buffer[0] == '7' && buffer[1] == 'z' && buffer[2] == 0xbcU && buffer[3] == 0xafU && buffer[4] == 0x27U && buffer[5] == 0x1cU) {
        info.description = "7-zip archive data";
        info.mime = "application/x-7z-compressed";
        return info;
    }
    if (length >= 4U && buffer[0] == 'B' && buffer[1] == 'Z' && buffer[2] == 'h') {
        info.description = "bzip2 compressed data";
        info.mime = "application/x-bzip2";
        return info;
    }
    if (length >= 6U && buffer[0] == 0xfdU && buffer[1] == '7' && buffer[2] == 'z' && buffer[3] == 'X' && buffer[4] == 'Z' && buffer[5] == 0x00U) {
        info.description = "XZ compressed data";
        info.mime = "application/x-xz";
        return info;
    }
    if (length >= 2U && buffer[0] == '#' && buffer[1] == '!') {
        if (find_text_ci(buffer, length, "python")) {
            info.description = "Python script text executable";
            info.mime = "text/x-python; charset=us-ascii";
        } else if (find_text_ci(buffer, length, "perl")) {
            info.description = "Perl script text executable";
            info.mime = "text/x-perl; charset=us-ascii";
        } else if (find_text_ci(buffer, length, "awk")) {
            info.description = "AWK script text executable";
            info.mime = "text/x-awk; charset=us-ascii";
        } else {
            info.description = "script text executable";
            info.mime = "text/x-shellscript; charset=us-ascii";
        }
        return info;
    }
    if (length >= 4U && buffer[0] == 'R' && buffer[1] == 'I' && buffer[2] == 'F' && buffer[3] == 'F') {
        if (length >= 12U && buffer[8] == 'W' && buffer[9] == 'A' && buffer[10] == 'V' && buffer[11] == 'E') {
            info.description = "WAV audio data";
            info.mime = "audio/wav";
            return info;
        }
        if (length >= 12U && buffer[8] == 'W' && buffer[9] == 'E' && buffer[10] == 'B' && buffer[11] == 'P') {
            info.description = "WebP image data";
            info.mime = "image/webp";
            return info;
        }
        if (length >= 12U && buffer[8] == 'A' && buffer[9] == 'V' && buffer[10] == 'I' && buffer[11] == ' ') {
            info.description = "AVI video data";
            info.mime = "video/x-msvideo";
            return info;
        }
    }
    if (length > 262U && buffer[257] == 'u' && buffer[258] == 's' && buffer[259] == 't' && buffer[260] == 'a' && buffer[261] == 'r') {
        info.description = "tar archive";
        info.mime = "application/x-tar";
        return info;
    }
    if (length >= 8U && buffer[0] == 0x89U && buffer[1] == 'P' && buffer[2] == 'N' && buffer[3] == 'G' &&
        buffer[4] == '\r' && buffer[5] == '\n' && buffer[6] == 0x1aU && buffer[7] == '\n') {
        info.description = "PNG image data";
        info.mime = "image/png";
        return info;
    }
    if (length >= 3U && buffer[0] == 0xffU && buffer[1] == 0xd8U && buffer[2] == 0xffU) {
        info.description = "JPEG image data";
        info.mime = "image/jpeg";
        return info;
    }
    if (length >= 6U &&
        buffer[0] == 'G' && buffer[1] == 'I' && buffer[2] == 'F' &&
        buffer[3] == '8' && (buffer[4] == '7' || buffer[4] == '9') && buffer[5] == 'a') {
        info.description = "GIF image data";
        info.mime = "image/gif";
        return info;
    }
    if (length >= 5U && buffer[0] == '%' && buffer[1] == 'P' && buffer[2] == 'D' && buffer[3] == 'F' && buffer[4] == '-') {
        info.description = "PDF document";
        info.mime = "application/pdf";
        return info;
    }
    if (length >= 10U && buffer[0] == '%' && buffer[1] == '!' && buffer[2] == 'P' && buffer[3] == 'S') {
        info.description = "PostScript document";
        info.mime = "application/postscript";
        return info;
    }
    if (length >= 4U && buffer[0] == 'P' && buffer[1] == 'K' && (buffer[2] == 0x03U || buffer[2] == 0x05U || buffer[2] == 0x07U)) {
        info.description = "ZIP archive data";
        info.mime = "application/zip";
        return info;
    }
    if (length >= 8U && buffer[0] == '!' && buffer[1] == '<' && buffer[2] == 'a' && buffer[3] == 'r' &&
        buffer[4] == 'c' && buffer[5] == 'h' && buffer[6] == '>' && buffer[7] == '\n') {
        info.description = "ar archive";
        info.mime = "application/x-archive";
        return info;
    }
    if (length >= 2U && buffer[0] == 'B' && buffer[1] == 'M') {
        info.description = "BMP image data";
        info.mime = "image/bmp";
        return info;
    }
    if (length >= 4U && buffer[0] == 'O' && buffer[1] == 'g' && buffer[2] == 'g' && buffer[3] == 'S') {
        info.description = "Ogg data";
        info.mime = "application/ogg";
        return info;
    }
    if (length >= 4U && buffer[0] == 'f' && buffer[1] == 'L' && buffer[2] == 'a' && buffer[3] == 'C') {
        info.description = "FLAC audio bitstream data";
        info.mime = "audio/flac";
        return info;
    }
    if (length >= 3U && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
        info.description = "MP3 audio with ID3 tag";
        info.mime = "audio/mpeg";
        return info;
    }
    if (length >= 2U && buffer[0] == 'M' && buffer[1] == 'Z') {
        info.description = "DOS/PE executable";
        info.mime = "application/vnd.microsoft.portable-executable";
        return info;
    }
    if (length >= 4U &&
        ((buffer[0] == 0xfeU && buffer[1] == 0xedU && buffer[2] == 0xfaU && (buffer[3] == 0xceU || buffer[3] == 0xcfU)) ||
         (buffer[0] == 0xceU && buffer[1] == 0xfaU && buffer[2] == 0xedU && buffer[3] == 0xfeU) ||
          (buffer[0] == 0xcfU && buffer[1] == 0xfaU && buffer[2] == 0xedU && buffer[3] == 0xfeU))) {
        info.description = "Mach-O binary";
        info.mime = "application/x-mach-binary";
        return info;
    }
    if (length >= 16U &&
        buffer[0] == 'S' && buffer[1] == 'Q' && buffer[2] == 'L' && buffer[3] == 'i' &&
        buffer[4] == 't' && buffer[5] == 'e' && buffer[6] == ' ' && buffer[7] == 'f' &&
        buffer[8] == 'o' && buffer[9] == 'r' && buffer[10] == 'm' && buffer[11] == 'a' &&
        buffer[12] == 't' && buffer[13] == ' ' && buffer[14] == '3' && buffer[15] == 0x00U) {
        info.description = "SQLite 3.x database";
        info.mime = "application/vnd.sqlite3";
        return info;
    }
    if (length >= 4U && buffer[0] == 0x00U && buffer[1] == 'a' && buffer[2] == 's' && buffer[3] == 'm') {
        info.description = "WebAssembly binary module";
        info.mime = "application/wasm";
        return info;
    }
    if (length >= 3U && buffer[0] == 0xefU && buffer[1] == 0xbbU && buffer[2] == 0xbfU) {
        info.description = "UTF-8 Unicode text";
        info.mime = "text/plain; charset=utf-8";
        return info;
    }
    if (length >= 2U &&
        ((buffer[0] == 0xffU && buffer[1] == 0xfeU) || (buffer[0] == 0xfeU && buffer[1] == 0xffU))) {
        info.description = "UTF-16 Unicode text";
        info.mime = "text/plain; charset=utf-16";
        return info;
    }
    if (looks_like_text(buffer, length)) {
        if (looks_like_json(buffer, length)) {
            info.description = "JSON text";
            info.mime = "application/json";
        } else if (looks_like_html(buffer, length)) {
            info.description = "HTML document text";
            info.mime = "text/html; charset=us-ascii";
        } else if (looks_like_xml(buffer, length)) {
            if (find_text_ci(buffer, length, "<svg")) {
                info.description = "SVG Scalable Vector Graphics image";
                info.mime = "image/svg+xml";
            } else {
                info.description = "XML document text";
                info.mime = "application/xml";
            }
        } else {
            info.description = "ASCII text";
            info.mime = "text/plain; charset=us-ascii";
        }
    }
    return info;
}

static int describe_path(const char *path, int mime_only, int follow_symlinks) {
    int is_directory = 0;
    int fd;
    int should_close;
    unsigned char buffer[512];
    long bytes_read;
    char target[1024];
    char resolved[1024];
    const char *lookup_path = path;
    FileTypeInfo info;

    if (path != 0 && !follow_symlinks && platform_read_symlink(path, target, sizeof(target)) == 0) {
        rt_write_cstr(1, path);
        rt_write_cstr(1, ": ");
        if (mime_only) {
            rt_write_line(1, "inode/symlink");
        } else {
            rt_write_cstr(1, "symbolic link to ");
            rt_write_line(1, target);
        }
        return 0;
    }

    if (path != 0 && follow_symlinks && tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) == 0) {
        lookup_path = resolved;
    }

    if (lookup_path != 0 && platform_path_is_directory(lookup_path, &is_directory) == 0 && is_directory) {
        rt_write_cstr(1, path);
        rt_write_cstr(1, ": ");
        rt_write_line(1, mime_only ? "inode/directory" : "directory");
        return 0;
    }

    if (tool_open_input(lookup_path, &fd, &should_close) != 0) {
        return -1;
    }

    bytes_read = platform_read(fd, buffer, sizeof(buffer));
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        return -1;
    }

    info = detect_type(buffer, (size_t)bytes_read);
    rt_write_cstr(1, path ? path : "stdin");
    rt_write_cstr(1, ": ");
    rt_write_line(1, mime_only ? info.mime : info.description);
    return 0;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int mime_only = 0;
    int follow_symlinks = 0;
    int argi = 1;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-i") == 0 || rt_strcmp(argv[argi], "--mime") == 0 || rt_strcmp(argv[argi], "--mime-type") == 0) {
            mime_only = 1;
        } else if (rt_strcmp(argv[argi], "-L") == 0 || rt_strcmp(argv[argi], "--dereference") == 0) {
            follow_symlinks = 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--no-dereference") == 0) {
            follow_symlinks = 0;
        } else {
            tool_write_usage("file", "[-i] [-L|-h] [file ...]");
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        return describe_path(NULL, mime_only, follow_symlinks) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        if (describe_path(argv[i], mime_only, follow_symlinks) != 0) {
            rt_write_cstr(2, "file: cannot inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
