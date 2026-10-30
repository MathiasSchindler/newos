#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_copy_file(const char *source_path, const char *dest_path) {
    int src_fd = platform_open_read(source_path);
    int dst_fd;
    char buffer[65536];

    if (tool_paths_equal(source_path, dest_path)) {
        return -1;
    }

    if (src_fd < 0) {
        return -1;
    }

    dst_fd = platform_open_write(dest_path, 0644U);
    if (dst_fd < 0) {
        platform_close(src_fd);
        return -1;
    }

    for (;;) {
        long bytes_read = platform_read(src_fd, buffer, sizeof(buffer));
        long offset = 0;

        if (bytes_read == 0) {
            break;
        }

        if (bytes_read < 0) {
            platform_close(src_fd);
            platform_close(dst_fd);
            return -1;
        }

        while (offset < bytes_read) {
            long bytes_written = platform_write(dst_fd, buffer + offset, (size_t)(bytes_read - offset));
            if (bytes_written <= 0) {
                platform_close(src_fd);
                platform_close(dst_fd);
                return -1;
            }
            offset += bytes_written;
        }
    }

    platform_close(src_fd);
    platform_close(dst_fd);
    return 0;
}

int tool_copy_path(const char *source_path, const char *dest_path, int recursive, int preserve_mode, int preserve_symlinks) {
    PlatformDirEntry source_info;
    char link_target[2048];

    if (source_path == 0 || dest_path == 0 || platform_get_path_info(source_path, &source_info) != 0) {
        return -1;
    }

    if (preserve_symlinks && platform_read_symlink(source_path, link_target, sizeof(link_target)) == 0) {
        if (tool_paths_equal(source_path, dest_path)) {
            return 0;
        }
        (void)platform_remove_file(dest_path);
        if (platform_create_symbolic_link(link_target, dest_path) != 0) {
            return -1;
        }
        return 0;
    }

    if (!source_info.is_dir) {
        if (tool_copy_file(source_path, dest_path) != 0) {
            return -1;
        }
        if (preserve_mode) {
            (void)platform_change_mode(dest_path, source_info.mode & 07777U);
        }
        return 0;
    }

    if (!recursive) {
        return -2;
    }

    {
        int dest_is_directory = 0;
        unsigned int dir_mode = preserve_mode ? (source_info.mode & 07777U) : 0755U;

        if (platform_path_is_directory(dest_path, &dest_is_directory) != 0) {
            if (platform_make_directory(dest_path, dir_mode) != 0) {
                return -1;
            }
        } else if (!dest_is_directory) {
            return -1;
        }
    }

    {
        enum { TOOL_COPY_ENTRY_CAPACITY = 1024, TOOL_COPY_PATH_CAPACITY = 2048 };
        PlatformDirEntry entries[TOOL_COPY_ENTRY_CAPACITY];
        size_t count = 0;
        size_t i;
        int path_is_directory = 0;

        if (platform_collect_entries(source_path, 1, entries, TOOL_COPY_ENTRY_CAPACITY, &count, &path_is_directory) != 0 ||
            !path_is_directory) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_source[TOOL_COPY_PATH_CAPACITY];
            char child_dest[TOOL_COPY_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(source_path, entries[i].name, child_source, sizeof(child_source)) != 0 ||
                tool_join_path(dest_path, entries[i].name, child_dest, sizeof(child_dest)) != 0 ||
                tool_copy_path(child_source, child_dest, recursive, preserve_mode, preserve_symlinks) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }

        platform_free_entries(entries, count);
    }

    if (preserve_mode) {
        (void)platform_change_mode(dest_path, source_info.mode & 07777U);
    }

    return 0;
}

int tool_remove_path(const char *path, int recursive) {
    enum { TOOL_REMOVE_ENTRY_CAPACITY = 1024, TOOL_REMOVE_PATH_CAPACITY = 2048 };
    PlatformDirEntry entries[TOOL_REMOVE_ENTRY_CAPACITY];
    size_t count = 0;
    size_t i;
    int is_directory = 0;

    if (path == 0 || platform_collect_entries(path, 1, entries, TOOL_REMOVE_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        return -1;
    }

    if (!is_directory) {
        platform_free_entries(entries, count);
        return platform_remove_file(path) == 0 ? 0 : -1;
    }

    if (!recursive) {
        platform_free_entries(entries, count);
        return -2;
    }

    for (i = 0; i < count; ++i) {
        char child_path[TOOL_REMOVE_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
            tool_remove_path(child_path, 1) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return platform_remove_directory(path) == 0 ? 0 : -1;
}

static int tool_walk_path_impl(const char *path,
                               const ToolWalkOptions *options,
                               ToolWalkCallback callback,
                               void *user_data,
                               int depth) {
    enum { TOOL_WALK_ENTRY_CAPACITY = 1024, TOOL_WALK_PATH_CAPACITY = 2048 };
    PlatformDirEntry current;
    ToolWalkControl control;
    PlatformDirEntry entries[TOOL_WALK_ENTRY_CAPACITY];
    size_t count = 0U;
    size_t i;
    int is_directory = 0;

    if (platform_get_path_info(path, &current) != 0) {
        return -1;
    }

    control.prune = 0;
    if (depth >= options->min_depth && callback(path, &current, depth, &control, user_data) != 0) {
        return -1;
    }

    if (!current.is_dir || control.prune || (options->max_depth >= 0 && depth >= options->max_depth)) {
        return 0;
    }

    if (platform_collect_entries(path, 1, entries, TOOL_WALK_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
        return -1;
    }

    for (i = 0U; i < count; ++i) {
        char child_path[TOOL_WALK_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }
        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
            tool_walk_path_impl(child_path, options, callback, user_data, depth + 1) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return 0;
}

int tool_walk_path(const char *path, const ToolWalkOptions *options, ToolWalkCallback callback, void *user_data) {
    ToolWalkOptions default_options;

    if (path == 0 || callback == 0) {
        return -1;
    }
    if (options == 0) {
        default_options.min_depth = 0;
        default_options.max_depth = -1;
        options = &default_options;
    }
    return tool_walk_path_impl(path, options, callback, user_data, 0);
}

static unsigned int tool_permission_mask_for_who(unsigned int who, char permission, unsigned int current_mode, unsigned int flags) {
    unsigned int mask = 0U;
    int allow_exec = (flags & TOOL_SYMBOLIC_MODE_X_ALWAYS) != 0U || (flags & TOOL_SYMBOLIC_MODE_DIRECTORY) != 0U || (current_mode & 0111U) != 0U;

    if (permission == 'r') {
        if ((who & 1U) != 0U) mask |= 0400U;
        if ((who & 2U) != 0U) mask |= 0040U;
        if ((who & 4U) != 0U) mask |= 0004U;
    } else if (permission == 'w') {
        if ((who & 1U) != 0U) mask |= 0200U;
        if ((who & 2U) != 0U) mask |= 0020U;
        if ((who & 4U) != 0U) mask |= 0002U;
    } else if (permission == 'x' || (permission == 'X' && allow_exec)) {
        if ((who & 1U) != 0U) mask |= 0100U;
        if ((who & 2U) != 0U) mask |= 0010U;
        if ((who & 4U) != 0U) mask |= 0001U;
    } else if (permission == 's') {
        if ((who & 1U) != 0U) mask |= 04000U;
        if ((who & 2U) != 0U) mask |= 02000U;
    } else if (permission == 't') {
        mask |= 01000U;
    }
    return mask;
}

static unsigned int tool_permission_copy_mask_for_who(unsigned int who, char source_class, unsigned int current_mode) {
    unsigned int source_bits = 0U;
    unsigned int mask = 0U;

    if (source_class == 'u') source_bits = (current_mode >> 6U) & 07U;
    else if (source_class == 'g') source_bits = (current_mode >> 3U) & 07U;
    else if (source_class == 'o') source_bits = current_mode & 07U;
    if ((source_bits & 4U) != 0U) mask |= tool_permission_mask_for_who(who, 'r', current_mode, 0U);
    if ((source_bits & 2U) != 0U) mask |= tool_permission_mask_for_who(who, 'w', current_mode, 0U);
    if ((source_bits & 1U) != 0U) mask |= tool_permission_mask_for_who(who, 'x', current_mode, 0U);
    return mask;
}

int tool_apply_symbolic_mode(const char *text, unsigned int current_mode, unsigned int flags, unsigned int *mode_out) {
    unsigned int result = current_mode & 07777U;
    size_t i = 0U;

    if (text == 0 || text[0] == '\0') return -1;
    while (text[i] != '\0') {
        unsigned int who = 0U;
        unsigned int set_mask = 0U;
        unsigned int clear_mask = 0U;
        char op;
        int saw_who = 0;
        int saw_permission = 0;

        while (text[i] == 'u' || text[i] == 'g' || text[i] == 'o' || text[i] == 'a') {
            saw_who = 1;
            if (text[i] == 'u') who |= 1U;
            else if (text[i] == 'g') who |= 2U;
            else if (text[i] == 'o') who |= 4U;
            else who |= 7U;
            i += 1U;
        }
        if (!saw_who) who = 7U;
        op = text[i];
        if (op != '+' && op != '-' && op != '=') return -1;
        i += 1U;
        while (text[i] != '\0' && text[i] != ',') {
            unsigned int mask;

            if ((flags & TOOL_SYMBOLIC_MODE_ALLOW_COPY) != 0U && (text[i] == 'u' || text[i] == 'g' || text[i] == 'o')) mask = tool_permission_copy_mask_for_who(who, text[i], result);
            else {
                mask = tool_permission_mask_for_who(who, text[i], result, flags);
                if (mask == 0U && text[i] != 'X') return -1;
            }
            set_mask |= mask;
            saw_permission = 1;
            i += 1U;
        }
        if (!saw_permission && op != '=' && (flags & TOOL_SYMBOLIC_MODE_REQUIRE_PERMISSION) != 0U) return -1;
        if ((who & 1U) != 0U) clear_mask |= 0700U | 04000U;
        if ((who & 2U) != 0U) clear_mask |= 0070U | 02000U;
        if ((who & 4U) != 0U) clear_mask |= 0007U;
        if (who == 7U) clear_mask |= 01000U;
        if (op == '+') result |= set_mask;
        else if (op == '-') result &= ~set_mask;
        else result = (result & ~clear_mask) | set_mask;
        if (text[i] == ',') i += 1U;
    }
    *mode_out = result & 07777U;
    return 0;
}

int tool_path_is_directory(const char *path) {
    int is_directory = 0;
    return platform_path_is_directory(path, &is_directory) == 0 && is_directory;
}

int tool_should_replace_path(const char *source_path, const char *target_path, int source_is_directory, int no_clobber, int update_only, int interactive, const char *prompt_prefix) {
    PlatformDirEntry source_info;
    PlatformDirEntry target_info;

    if (!tool_path_exists(target_path)) {
        return 1;
    }
    if (no_clobber) {
        return 0;
    }
    if (update_only &&
        !source_is_directory &&
        platform_get_path_info(source_path, &source_info) == 0 &&
        platform_get_path_info(target_path, &target_info) == 0 &&
        source_info.mtime <= target_info.mtime) {
        return 0;
    }
    if (interactive) {
        return tool_prompt_yes_no(prompt_prefix, target_path);
    }
    return 1;
}

int tool_ensure_parent_dirs(const char *path, char *scratch, size_t scratch_size) {
    size_t index = 0U;

    if (path == 0 || path[0] == '\0' || scratch == 0 || rt_strlen(path) >= scratch_size) return -1;
    while (path[index] != '\0') {
        PlatformDirEntry entry;
        size_t component_end;

        while (path[index] == '/') index += 1U;
        if (path[index] == '\0') break;
        while (path[index] != '\0' && path[index] != '/') index += 1U;
        if (path[index] == '\0') break;
        component_end = index;
        memcpy(scratch, path, component_end);
        scratch[component_end] = '\0';
        if (scratch[0] == '\0') continue;
        if (platform_get_path_info(scratch, &entry) == 0) {
            if ((entry.mode & TOOL_PATH_MODE_TYPE_MASK) == TOOL_PATH_MODE_SYMLINK || !entry.is_dir) return -1;
        } else if (platform_make_directory(scratch, 0755U) != 0) {
            if (platform_get_path_info(scratch, &entry) != 0 || !entry.is_dir) return -1;
        }
    }
    return 0;
}
