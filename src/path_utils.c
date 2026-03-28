#include "path_utils.h"
#include "safe_alloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static bool path_is_drive_root(const char* path, size_t len) {
#ifdef _WIN32
    return path &&
           len == 3 &&
           isalpha((unsigned char)path[0]) &&
           path[1] == ':' &&
           path_is_sep(path[2]);
#else
    (void)path;
    return len == 1;
#endif
}

static void path_trim_trailing_seps(char* path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path_is_sep(path[len - 1]) && !path_is_drive_root(path, len)) {
        path[len - 1] = '\0';
        len--;
    }
}

static bool path_prefix_matches(const char* path, const char* root) {
    if (!path || !root) return false;
    size_t path_len = strlen(path);
    size_t root_len = strlen(root);
    if (root_len == 0 || path_len < root_len) return false;

    for (size_t i = 0; i < root_len; i++) {
        char a = path[i];
        char b = root[i];
#ifdef _WIN32
        if (path_is_sep(a) && path_is_sep(b)) continue;
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return false;
#else
        if (a != b) return false;
#endif
    }

    if (path_len == root_len) return true;
    return path_is_sep(path[root_len]);
}

#ifdef _WIN32
static char* path_strip_windows_extended_prefix_alloc(const char* text) {
    if (!text) return NULL;
    const char* unc_prefix = "\\\\?\\UNC\\";
    const char* ext_prefix = "\\\\?\\";
    if (strncmp(text, unc_prefix, strlen(unc_prefix)) == 0) {
        const char* rest = text + strlen(unc_prefix);
        size_t rest_len = strlen(rest);
        char* out = (char*)safe_malloc(rest_len + 3);
        out[0] = '\\';
        out[1] = '\\';
        memcpy(out + 2, rest, rest_len + 1);
        return out;
    }
    if (strncmp(text, ext_prefix, strlen(ext_prefix)) == 0) {
        return safe_strdup(text + strlen(ext_prefix));
    }
    return safe_strdup(text);
}

static char* path_canonical_existing_alloc(const char* path) {
    if (!path || path[0] == '\0') return NULL;
    HANDLE h = CreateFileA(path,
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    DWORD need = GetFinalPathNameByHandleA(h, NULL, 0, FILE_NAME_NORMALIZED);
    if (need == 0) {
        CloseHandle(h);
        return NULL;
    }

    char* raw = (char*)safe_malloc((size_t)need + 1);
    DWORD got = GetFinalPathNameByHandleA(h, raw, need + 1, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (got == 0 || got > need) {
        free(raw);
        return NULL;
    }
    raw[got] = '\0';

    char* stripped = path_strip_windows_extended_prefix_alloc(raw);
    free(raw);
    if (!stripped) return NULL;
    for (char* p = stripped; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    return stripped;
}
#else
static char* path_canonical_existing_alloc(const char* path) {
    if (!path || path[0] == '\0') return NULL;
    return realpath(path, NULL);
}
#endif

bool path_is_absolute(const char* path) {
    if (!path || path[0] == '\0') return false;

#ifdef _WIN32
    if (path[0] == '/' || path[0] == '\\') return true;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return true;
    return false;
#else
    return path[0] == '/';
#endif
}

char* path_dirname_alloc(const char* path) {
    if (!path || path[0] == '\0') return safe_strdup(".");

    const char* last_slash = strrchr(path, '/');
    const char* last_backslash = strrchr(path, '\\');
    const char* sep = NULL;
    if (last_slash && last_backslash) {
        sep = last_slash > last_backslash ? last_slash : last_backslash;
    } else {
        sep = last_slash ? last_slash : last_backslash;
    }

    if (!sep) return safe_strdup(".");

#ifdef _WIN32
    // "C:\file.tblo" -> "C:\"
    // Check path length first to avoid reading beyond bounds
    size_t path_len = strlen(path);
    if (path_len >= 3 && sep == path + 2 && isalpha((unsigned char)path[0]) && path[1] == ':' && path_is_sep(path[2])) {
        char* out = (char*)safe_malloc(4);
        out[0] = path[0];
        out[1] = ':';
        out[2] = '\\';
        out[3] = '\0';
        return out;
    }
#endif

    size_t dir_len = (size_t)(sep - path);
    // "/file.tblo" -> "/"
    if (dir_len == 0) {
        char* out = (char*)safe_malloc(2);
        out[0] = sep[0];
        out[1] = '\0';
        return out;
    }

    char* out = (char*)safe_malloc(dir_len + 1);
    memcpy(out, path, dir_len);
    out[dir_len] = '\0';
    return out;
}

static char pick_join_sep(const char* root) {
#ifdef _WIN32
    if (root && isalpha((unsigned char)root[0]) && root[1] == ':') return '\\';
    if (root && strchr(root, '\\')) return '\\';
    return '/';
#else
    (void)root;
    return '/';
#endif
}

char* path_sandbox_join_alloc(const char* sandbox_root, const char* user_path, const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!sandbox_root || sandbox_root[0] == '\0') {
        if (out_error) *out_error = "Sandbox root not set";
        return NULL;
    }
    if (!user_path || user_path[0] == '\0') {
        if (out_error) *out_error = "Empty path";
        return NULL;
    }

#ifdef _WIN32
    if (strchr(user_path, ':') != NULL) {
        if (out_error) *out_error = "Drive paths are not allowed";
        return NULL;
    }
#endif

    if (path_is_absolute(user_path)) {
        if (out_error) *out_error = "Absolute paths are not allowed";
        return NULL;
    }

    int seg_capacity = 8;
    int seg_count = 0;
    char** segments = (char**)safe_malloc((size_t)seg_capacity * sizeof(char*));

    const char* p = user_path;
    while (*p) {
        while (*p && path_is_sep(*p)) p++;
        const char* start = p;
        while (*p && !path_is_sep(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0) break;

        if (len == 1 && start[0] == '.') {
            continue;
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (seg_count == 0) {
                if (out_error) *out_error = "Path escapes sandbox";
                goto fail;
            }
            free(segments[--seg_count]);
            segments[seg_count] = NULL;
            continue;
        }

        if (seg_count + 1 > seg_capacity) {
            seg_capacity *= 2;
            segments = (char**)safe_realloc(segments, (size_t)seg_capacity * sizeof(char*));
        }

        char* seg = (char*)safe_malloc(len + 1);
        memcpy(seg, start, len);
        seg[len] = '\0';
        segments[seg_count++] = seg;
    }

    if (seg_count == 0) {
        if (out_error) *out_error = "Path resolves to empty";
        goto fail;
    }

    char sep = pick_join_sep(sandbox_root);

    size_t root_len = strlen(sandbox_root);
    size_t trimmed_root_len = root_len;
    while (trimmed_root_len > 1 && path_is_sep(sandbox_root[trimmed_root_len - 1])) {
        trimmed_root_len--;
    }
    bool root_has_sep = trimmed_root_len > 0 && path_is_sep(sandbox_root[trimmed_root_len - 1]);

    size_t result_len = trimmed_root_len;
    if (!root_has_sep) result_len += 1;
    for (int i = 0; i < seg_count; i++) {
        result_len += strlen(segments[i]);
        if (i + 1 < seg_count) result_len += 1;
    }
    result_len += 1;

    char* result = (char*)safe_malloc(result_len);
    size_t pos = 0;
    memcpy(result + pos, sandbox_root, trimmed_root_len);
    pos += trimmed_root_len;
    if (!root_has_sep) {
        result[pos++] = sep;
    }
    for (int i = 0; i < seg_count; i++) {
        size_t seg_len = strlen(segments[i]);
        memcpy(result + pos, segments[i], seg_len);
        pos += seg_len;
        if (i + 1 < seg_count) {
            result[pos++] = sep;
        }
    }
    result[pos] = '\0';

    for (int i = 0; i < seg_count; i++) {
        free(segments[i]);
    }
    free(segments);
    return result;

fail:
    for (int i = 0; i < seg_count; i++) {
        free(segments[i]);
    }
    free(segments);
    return NULL;
}

char* path_sandbox_resolve_alloc(const char* sandbox_root,
                                 const char* user_path,
                                 bool allow_missing_leaf,
                                 const char** out_error) {
    if (out_error) *out_error = NULL;

    const char* join_err = NULL;
    char* joined = path_sandbox_join_alloc(sandbox_root, user_path, &join_err);
    if (!joined) {
        if (out_error) *out_error = join_err ? join_err : "Path not allowed";
        return NULL;
    }

    char* root_real = path_canonical_existing_alloc(sandbox_root);
    if (!root_real) {
        free(joined);
        if (out_error) *out_error = "Sandbox root does not exist";
        return NULL;
    }
    path_trim_trailing_seps(root_real);

    char* target_real = path_canonical_existing_alloc(joined);
    if (target_real) {
        path_trim_trailing_seps(target_real);
        if (!path_prefix_matches(target_real, root_real)) {
            free(target_real);
            free(root_real);
            free(joined);
            if (out_error) *out_error = "Path escapes sandbox";
            return NULL;
        }
        free(target_real);
        free(root_real);
        return joined;
    }

    if (!allow_missing_leaf) {
        free(root_real);
        free(joined);
        if (out_error) *out_error = "Path does not exist";
        return NULL;
    }

    char* parent = path_dirname_alloc(joined);
    if (!parent) {
        free(root_real);
        free(joined);
        if (out_error) *out_error = "Invalid path";
        return NULL;
    }

    const char* last_slash = strrchr(joined, '/');
    const char* last_backslash = strrchr(joined, '\\');
    const char* sep = NULL;
    if (last_slash && last_backslash) {
        sep = last_slash > last_backslash ? last_slash : last_backslash;
    } else {
        sep = last_slash ? last_slash : last_backslash;
    }
    const char* leaf = sep ? (sep + 1) : joined;
    if (!leaf || leaf[0] == '\0') {
        free(parent);
        free(root_real);
        free(joined);
        if (out_error) *out_error = "Invalid file name";
        return NULL;
    }

    char* parent_real = path_canonical_existing_alloc(parent);
    free(parent);
    if (!parent_real) {
        free(root_real);
        free(joined);
        if (out_error) *out_error = "Parent path does not exist";
        return NULL;
    }
    path_trim_trailing_seps(parent_real);

    if (!path_prefix_matches(parent_real, root_real)) {
        free(parent_real);
        free(root_real);
        free(joined);
        if (out_error) *out_error = "Path escapes sandbox";
        return NULL;
    }

    free(parent_real);
    free(root_real);
    return joined;
}

#ifndef _WIN32
static bool path_mode_to_open_flags(const char* mode, int* out_flags) {
    if (out_flags) *out_flags = 0;
    if (!mode || !out_flags || mode[0] == '\0') return false;

    char primary = mode[0];
    bool plus = false;
    bool exclusive = false;
    for (const char* p = mode + 1; *p; p++) {
        char c = *p;
        if (c == '+') {
            plus = true;
            continue;
        }
        if (c == 'b' || c == 't') {
            continue;
        }
        if (c == 'x') {
            exclusive = true;
            continue;
        }
        return false;
    }

    int flags = 0;
    switch (primary) {
        case 'r':
            flags = plus ? O_RDWR : O_RDONLY;
            break;
        case 'w':
            flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
            break;
        case 'a':
            flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
            break;
        default:
            return false;
    }

    if (exclusive) {
        flags |= O_CREAT | O_EXCL;
    }

    *out_flags = flags;
    return true;
}
#endif

#ifndef _WIN32
static bool path_validate_opened_sandbox_file_fd(int file_fd, const char** out_error) {
    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        if (out_error) *out_error = "Failed to inspect opened file";
        return false;
    }

    // Reject hardlinked regular files to prevent sandbox aliasing via external links.
    if (S_ISREG(st.st_mode) && st.st_nlink > 1) {
        if (out_error) *out_error = "Hardlinked files are not allowed in sandbox";
        return false;
    }

    return true;
}

static int path_open_sandbox_resolved_fd(const char* sandbox_root_real,
                                         const char* resolved_path,
                                         int open_flags,
                                         const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!sandbox_root_real || !resolved_path) {
        if (out_error) *out_error = "Invalid sandbox open arguments";
        return -1;
    }

    if (!path_prefix_matches(resolved_path, sandbox_root_real)) {
        if (out_error) *out_error = "Path escapes sandbox";
        return -1;
    }

    const char* rel = resolved_path + strlen(sandbox_root_real);
    while (*rel && path_is_sep(*rel)) rel++;
    if (!*rel) {
        if (out_error) *out_error = "Invalid file name";
        return -1;
    }

    int dir_flags = O_RDONLY;
#ifdef O_DIRECTORY
    dir_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    dir_flags |= O_CLOEXEC;
#endif

    int dir_fd = open(sandbox_root_real, dir_flags);
    if (dir_fd < 0) {
        if (out_error) *out_error = "Failed to open sandbox root";
        return -1;
    }

    const char* p = rel;
    while (*p) {
        while (*p && path_is_sep(*p)) p++;
        if (!*p) break;

        const char* start = p;
        while (*p && !path_is_sep(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;

        const char* q = p;
        while (*q && path_is_sep(*q)) q++;
        bool is_last = (*q == '\0');

        char* component = (char*)safe_malloc(len + 1);
        memcpy(component, start, len);
        component[len] = '\0';

        if (!is_last) {
            int next_dir_flags = O_RDONLY;
#ifdef O_DIRECTORY
            next_dir_flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            next_dir_flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            next_dir_flags |= O_CLOEXEC;
#endif
            int next_fd = openat(dir_fd, component, next_dir_flags);
            free(component);
            if (next_fd < 0) {
                close(dir_fd);
                if (out_error) *out_error = "Path changed during secure open";
                return -1;
            }
            close(dir_fd);
            dir_fd = next_fd;
            p = q;
            continue;
        }

        int final_flags = open_flags;
#ifdef O_NOFOLLOW
        final_flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        final_flags |= O_CLOEXEC;
#endif
        int file_fd = openat(dir_fd, component, final_flags, 0666);
        free(component);
        close(dir_fd);
        if (file_fd < 0) {
            if (out_error) *out_error = "Failed to open file";
            return -1;
        }
        if (!path_validate_opened_sandbox_file_fd(file_fd, out_error)) {
            close(file_fd);
            return -1;
        }
        return file_fd;
    }

    close(dir_fd);
    if (out_error) *out_error = "Invalid file name";
    return -1;
}
#endif

FILE* path_sandbox_fopen_resolved(const char* sandbox_root,
                                  const char* resolved_path,
                                  const char* mode,
                                  const char** out_error) {
    if (out_error) *out_error = NULL;
    if (!sandbox_root || sandbox_root[0] == '\0' || !resolved_path || !mode || mode[0] == '\0') {
        if (out_error) *out_error = "Invalid sandbox open arguments";
        return NULL;
    }

    char* root_real = path_canonical_existing_alloc(sandbox_root);
    if (!root_real) {
        if (out_error) *out_error = "Sandbox root does not exist";
        return NULL;
    }
    path_trim_trailing_seps(root_real);

#ifndef _WIN32
    int open_flags = 0;
    if (!path_mode_to_open_flags(mode, &open_flags)) {
        free(root_real);
        if (out_error) *out_error = "Unsupported file mode";
        return NULL;
    }

    int file_fd = path_open_sandbox_resolved_fd(root_real, resolved_path, open_flags, out_error);
    free(root_real);
    if (file_fd < 0) return NULL;

    FILE* file = fdopen(file_fd, mode);
    if (!file) {
        close(file_fd);
        if (out_error && !*out_error) *out_error = "Failed to wrap file handle";
        return NULL;
    }
    return file;
#else
    if (!path_prefix_matches(resolved_path, root_real)) {
        free(root_real);
        if (out_error) *out_error = "Path escapes sandbox";
        return NULL;
    }
    free(root_real);

    FILE* file = fopen(resolved_path, mode);
    if (!file) {
        if (out_error) *out_error = "Failed to open file";
        return NULL;
    }
    return file;
#endif
}
