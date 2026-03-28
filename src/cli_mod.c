#include "cli.h"
#include "cJSON.h"
#include "crypto_hash.h"
#include "path_utils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define CLI_MOD_MANIFEST_FILE "tablo.mod"
#define CLI_MOD_LOCK_FILE "tablo.lock"
#define CLI_MOD_VENDOR_DIR "vendor"
#define CLI_MOD_REGISTRY_ENV "TABLO_REGISTRY_ROOT"
#define CLI_MOD_REGISTRY_DEFAULT_ROOT ".tablo_registry"
#define CLI_MOD_KEYS_ENV "TABLO_KEYS_FILE"
#define CLI_MOD_KEYS_DEFAULT_FILE "tablo.keys"
#define CLI_MOD_SOURCE_SIGNATURE_FILE ".tablo.sig"
#define CLI_MOD_SOURCE_SIGNATURE_DEFAULT "unsigned"
#define CLI_MOD_SIGNATURE_SCHEME_HMAC_SHA256 "hmac-sha256"

typedef struct {
    char* name;
    char* version;
    char* constraint;
    char* source;
    char* checksum;
    char* source_hash;
    char* source_signature;
} CliModDependency;

typedef struct {
    char* module_name;
    CliModDependency* deps;
    int dep_count;
    int dep_capacity;
} CliModManifest;

typedef struct {
    char** items;
    int count;
    int capacity;
} CliModStringList;

typedef struct {
    char* id;
    char* algorithm;
    char* secret;
} CliModTrustedKey;

typedef struct {
    CliModTrustedKey* items;
    int count;
    int capacity;
} CliModTrustedKeyList;

static void cli_mod_print_usage(const char* program_name);
static const char* cli_mod_dependency_constraint_or_version(const CliModDependency* dep);

static char* cli_mod_strdup(const char* text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

static void cli_mod_set_error(char* out_error, size_t out_error_size, const char* message) {
    if (!out_error || out_error_size == 0) return;
    if (!message) message = "Unknown error";
    snprintf(out_error, out_error_size, "%s", message);
    out_error[out_error_size - 1] = '\0';
}

static int cli_mod_has_prefix(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    size_t prefix_len = strlen(prefix);
    return strncmp(text, prefix, prefix_len) == 0;
}

static int cli_mod_has_suffix(const char* text, const char* suffix) {
    if (!text || !suffix) return 0;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) return 0;
    return strcmp(text + (text_len - suffix_len), suffix) == 0;
}

static int cli_mod_is_path_sep(char c) {
    return c == '/' || c == '\\';
}

static int cli_mod_is_dep_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/';
}

static int cli_mod_is_version_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '+';
}

static int cli_mod_validate_module_name(const char* module_name) {
    if (!module_name || module_name[0] == '\0') return 0;
    if (module_name[0] == '/' || module_name[0] == '\\') return 0;
    for (const char* p = module_name; *p; p++) {
        if (!cli_mod_is_dep_name_char(*p)) return 0;
    }
    if (strstr(module_name, "..") != NULL) return 0;
    return 1;
}

static int cli_mod_validate_dependency_name(const char* dep_name) {
    if (!dep_name || dep_name[0] == '\0') return 0;
    if (dep_name[0] == '/' || dep_name[0] == '\\') return 0;
    for (const char* p = dep_name; *p; p++) {
        if (!cli_mod_is_dep_name_char(*p)) return 0;
    }
    if (strstr(dep_name, "..") != NULL) return 0;
    return 1;
}

static int cli_mod_validate_version(const char* version) {
    if (!version || version[0] == '\0') return 0;
    for (const char* p = version; *p; p++) {
        if (!cli_mod_is_version_char(*p)) return 0;
    }
    return 1;
}

typedef struct {
    int major;
    int minor;
    int patch;
    const char* prerelease;
    size_t prerelease_len;
} CliModSemver;

typedef enum {
    CLI_MOD_CMP_EQ = 0,
    CLI_MOD_CMP_GT = 1,
    CLI_MOD_CMP_GTE = 2,
    CLI_MOD_CMP_LT = 3,
    CLI_MOD_CMP_LTE = 4
} CliModSemverComparator;

static const char* cli_mod_skip_spaces(const char* text) {
    if (!text) return NULL;
    while (*text && isspace((unsigned char)*text)) text++;
    return text;
}

static size_t cli_mod_trimmed_span_len(const char* start, const char* end) {
    if (!start || !end || end < start) return 0;
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    return (size_t)(end - start);
}

static int cli_mod_parse_semver_number(const char* text, size_t* io_pos, int* out_value) {
    if (!text || !io_pos || !out_value) return 0;
    size_t pos = *io_pos;
    if (!isdigit((unsigned char)text[pos])) return 0;
    long long value = 0;
    while (isdigit((unsigned char)text[pos])) {
        value = (value * 10) + (long long)(text[pos] - '0');
        if (value > INT_MAX) return 0;
        pos++;
    }
    *out_value = (int)value;
    *io_pos = pos;
    return 1;
}

static int cli_mod_is_semver_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '-';
}

static int cli_mod_is_semver_build_char(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '.';
}

static int cli_mod_parse_semver(const char* text, CliModSemver* out) {
    if (!text || !out) return 0;
    size_t pos = 0;
    int major = 0;
    int minor = 0;
    int patch = 0;
    if (!cli_mod_parse_semver_number(text, &pos, &major)) return 0;
    if (text[pos] != '.') return 0;
    pos++;
    if (!cli_mod_parse_semver_number(text, &pos, &minor)) return 0;
    if (text[pos] != '.') return 0;
    pos++;
    if (!cli_mod_parse_semver_number(text, &pos, &patch)) return 0;

    const char* prerelease = NULL;
    size_t prerelease_len = 0;
    if (text[pos] == '-') {
        pos++;
        size_t start = pos;
        if (text[pos] == '\0' || text[pos] == '+') return 0;
        int prev_dot = 1;
        while (text[pos] && text[pos] != '+') {
            char c = text[pos];
            if (c == '.') {
                if (prev_dot) return 0;
                prev_dot = 1;
                pos++;
                continue;
            }
            if (!cli_mod_is_semver_ident_char(c)) return 0;
            prev_dot = 0;
            pos++;
        }
        if (prev_dot) return 0;
        prerelease = text + start;
        prerelease_len = pos - start;
    }

    if (text[pos] == '+') {
        pos++;
        if (text[pos] == '\0') return 0;
        int prev_dot = 1;
        while (text[pos]) {
            char c = text[pos];
            if (c == '.') {
                if (prev_dot) return 0;
                prev_dot = 1;
                pos++;
                continue;
            }
            if (!cli_mod_is_semver_build_char(c)) return 0;
            prev_dot = 0;
            pos++;
        }
        if (prev_dot) return 0;
    }

    if (text[pos] != '\0') return 0;

    out->major = major;
    out->minor = minor;
    out->patch = patch;
    out->prerelease = prerelease;
    out->prerelease_len = prerelease_len;
    return 1;
}

static int cli_mod_prerelease_identifier_is_numeric(const char* text, size_t len) {
    if (!text || len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int cli_mod_compare_numeric_identifiers(const char* a, size_t a_len, const char* b, size_t b_len) {
    size_t ai = 0;
    size_t bi = 0;
    while (ai + 1 < a_len && a[ai] == '0') ai++;
    while (bi + 1 < b_len && b[bi] == '0') bi++;
    size_t a_trim = a_len - ai;
    size_t b_trim = b_len - bi;
    if (a_trim != b_trim) return (a_trim < b_trim) ? -1 : 1;
    int cmp = memcmp(a + ai, b + bi, a_trim);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
}

static int cli_mod_compare_prerelease(const CliModSemver* a, const CliModSemver* b) {
    if (!a || !b) return 0;
    if (a->prerelease_len == 0 && b->prerelease_len == 0) return 0;
    if (a->prerelease_len == 0) return 1;
    if (b->prerelease_len == 0) return -1;

    size_t ai = 0;
    size_t bi = 0;
    while (ai < a->prerelease_len || bi < b->prerelease_len) {
        if (ai >= a->prerelease_len) return -1;
        if (bi >= b->prerelease_len) return 1;

        size_t a_end = ai;
        while (a_end < a->prerelease_len && a->prerelease[a_end] != '.') a_end++;
        size_t b_end = bi;
        while (b_end < b->prerelease_len && b->prerelease[b_end] != '.') b_end++;

        const char* a_seg = a->prerelease + ai;
        const char* b_seg = b->prerelease + bi;
        size_t a_len = a_end - ai;
        size_t b_len = b_end - bi;

        int a_num = cli_mod_prerelease_identifier_is_numeric(a_seg, a_len);
        int b_num = cli_mod_prerelease_identifier_is_numeric(b_seg, b_len);
        if (a_num && b_num) {
            int cmp_num = cli_mod_compare_numeric_identifiers(a_seg, a_len, b_seg, b_len);
            if (cmp_num != 0) return cmp_num;
        } else if (a_num != b_num) {
            return a_num ? -1 : 1;
        } else {
            size_t min_len = (a_len < b_len) ? a_len : b_len;
            int cmp = memcmp(a_seg, b_seg, min_len);
            if (cmp < 0) return -1;
            if (cmp > 0) return 1;
            if (a_len != b_len) return (a_len < b_len) ? -1 : 1;
        }

        ai = (a_end < a->prerelease_len) ? (a_end + 1) : a_end;
        bi = (b_end < b->prerelease_len) ? (b_end + 1) : b_end;
    }
    return 0;
}

static int cli_mod_semver_compare(const CliModSemver* a, const CliModSemver* b) {
    if (!a || !b) return 0;
    if (a->major != b->major) return (a->major < b->major) ? -1 : 1;
    if (a->minor != b->minor) return (a->minor < b->minor) ? -1 : 1;
    if (a->patch != b->patch) return (a->patch < b->patch) ? -1 : 1;
    return cli_mod_compare_prerelease(a, b);
}

static int cli_mod_parse_semver_comparator_token(const char* token,
                                                 CliModSemverComparator* out_cmp,
                                                 CliModSemver* out_version) {
    if (!token || !out_cmp || !out_version) return 0;
    const char* p = cli_mod_skip_spaces(token);
    if (!p || p[0] == '\0') return 0;

    CliModSemverComparator cmp = CLI_MOD_CMP_EQ;
    if (p[0] == '>' && p[1] == '=') {
        cmp = CLI_MOD_CMP_GTE;
        p += 2;
    } else if (p[0] == '<' && p[1] == '=') {
        cmp = CLI_MOD_CMP_LTE;
        p += 2;
    } else if (p[0] == '>') {
        cmp = CLI_MOD_CMP_GT;
        p += 1;
    } else if (p[0] == '<') {
        cmp = CLI_MOD_CMP_LT;
        p += 1;
    } else if (p[0] == '=') {
        cmp = CLI_MOD_CMP_EQ;
        p += 1;
    }

    p = cli_mod_skip_spaces(p);
    if (!p || p[0] == '\0') return 0;
    if (!cli_mod_parse_semver(p, out_version)) return 0;
    *out_cmp = cmp;
    return 1;
}

static int cli_mod_semver_compare_with_op(int cmp, CliModSemverComparator op) {
    switch (op) {
        case CLI_MOD_CMP_EQ:
            return cmp == 0;
        case CLI_MOD_CMP_GT:
            return cmp > 0;
        case CLI_MOD_CMP_GTE:
            return cmp >= 0;
        case CLI_MOD_CMP_LT:
            return cmp < 0;
        case CLI_MOD_CMP_LTE:
            return cmp <= 0;
        default:
            return 0;
    }
}

static int cli_mod_constraint_matches_semver(const char* constraint, const CliModSemver* version) {
    if (!constraint || !version) return 0;
    const char* start = cli_mod_skip_spaces(constraint);
    if (!start || start[0] == '\0') return 0;
    const char* end = constraint + strlen(constraint);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end <= start) return 0;

    if (start[0] == '^' || start[0] == '~') {
        char op = start[0];
        start++;
        start = cli_mod_skip_spaces(start);
        if (!start || start >= end) return 0;
        size_t base_len = cli_mod_trimmed_span_len(start, end);
        if (base_len == 0) return 0;

        char* base_text = (char*)malloc(base_len + 1);
        if (!base_text) return 0;
        memcpy(base_text, start, base_len);
        base_text[base_len] = '\0';

        CliModSemver base;
        int ok = cli_mod_parse_semver(base_text, &base);
        if (!ok) {
            free(base_text);
            return 0;
        }

        CliModSemver upper = {0};
        if (op == '^') {
            if (base.major > 0) {
                upper.major = base.major + 1;
                upper.minor = 0;
                upper.patch = 0;
            } else if (base.minor > 0) {
                upper.major = 0;
                upper.minor = base.minor + 1;
                upper.patch = 0;
            } else {
                upper.major = 0;
                upper.minor = 0;
                upper.patch = base.patch + 1;
            }
        } else {
            upper.major = base.major;
            upper.minor = base.minor + 1;
            upper.patch = 0;
        }

        int cmp_base = cli_mod_semver_compare(version, &base);
        int cmp_upper = cli_mod_semver_compare(version, &upper);
        free(base_text);
        return cmp_base >= 0 && cmp_upper < 0;
    }

    const char* token_start = start;
    while (token_start < end) {
        const char* token_end = token_start;
        while (token_end < end && *token_end != ',') token_end++;

        size_t token_len = cli_mod_trimmed_span_len(token_start, token_end);
        if (token_len == 0) return 0;
        char* token = (char*)malloc(token_len + 1);
        if (!token) return 0;

        const char* real_start = token_start;
        while (real_start < token_end && isspace((unsigned char)*real_start)) real_start++;
        const char* real_end = token_end;
        while (real_end > real_start && isspace((unsigned char)real_end[-1])) real_end--;
        memcpy(token, real_start, (size_t)(real_end - real_start));
        token[real_end - real_start] = '\0';

        CliModSemverComparator cmp_op = CLI_MOD_CMP_EQ;
        CliModSemver cmp_ver;
        int ok = cli_mod_parse_semver_comparator_token(token, &cmp_op, &cmp_ver);
        free(token);
        if (!ok) return 0;

        int cmp = cli_mod_semver_compare(version, &cmp_ver);
        if (!cli_mod_semver_compare_with_op(cmp, cmp_op)) return 0;

        if (token_end == end) break;
        token_start = token_end + 1;
    }
    return 1;
}

static int cli_mod_validate_constraint(const char* constraint) {
    if (!constraint) return 0;
    const char* start = cli_mod_skip_spaces(constraint);
    if (!start || start[0] == '\0') return 0;
    const char* end = constraint + strlen(constraint);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end <= start) return 0;

    size_t len = (size_t)(end - start);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, start, len);
    copy[len] = '\0';

    if (cli_mod_validate_version(copy) &&
        copy[0] != '^' &&
        copy[0] != '~' &&
        copy[0] != '>' &&
        copy[0] != '<' &&
        copy[0] != '=' &&
        strchr(copy, ',') == NULL) {
        free(copy);
        return 1;
    }

    int valid = 0;
    if (copy[0] == '^' || copy[0] == '~') {
        const char* base = cli_mod_skip_spaces(copy + 1);
        CliModSemver parsed;
        valid = base && base[0] != '\0' && cli_mod_parse_semver(base, &parsed);
    } else {
        const char* token = copy;
        valid = 1;
        while (*token) {
            const char* comma = strchr(token, ',');
            size_t token_len = comma ? (size_t)(comma - token) : strlen(token);
            size_t trimmed_len = token_len;
            while (trimmed_len > 0 && isspace((unsigned char)token[trimmed_len - 1])) trimmed_len--;
            size_t start_off = 0;
            while (start_off < trimmed_len && isspace((unsigned char)token[start_off])) start_off++;
            if (trimmed_len <= start_off) {
                valid = 0;
                break;
            }
            char* piece = (char*)malloc((trimmed_len - start_off) + 1);
            if (!piece) {
                valid = 0;
                break;
            }
            memcpy(piece, token + start_off, trimmed_len - start_off);
            piece[trimmed_len - start_off] = '\0';
            CliModSemverComparator cmp_op;
            CliModSemver cmp_ver;
            if (!cli_mod_parse_semver_comparator_token(piece, &cmp_op, &cmp_ver)) {
                free(piece);
                valid = 0;
                break;
            }
            free(piece);
            if (!comma) break;
            token = comma + 1;
        }
    }

    free(copy);
    return valid;
}

static int cli_mod_extract_exact_version_from_constraint(const char* constraint, char** out_version) {
    if (!constraint || !out_version) return 0;
    *out_version = NULL;
    if (!cli_mod_validate_constraint(constraint)) return 0;

    const char* start = cli_mod_skip_spaces(constraint);
    if (!start || start[0] == '\0') return 0;
    if (start[0] == '^' || start[0] == '~' || start[0] == '>' || start[0] == '<') return 0;
    if (strchr(start, ',') != NULL) return 0;

    CliModSemverComparator cmp = CLI_MOD_CMP_EQ;
    CliModSemver parsed;
    if (!cli_mod_parse_semver_comparator_token(start, &cmp, &parsed)) return 0;
    if (cmp != CLI_MOD_CMP_EQ) return 0;

    const char* p = start;
    if (*p == '=') p++;
    p = cli_mod_skip_spaces(p);
    if (!p || p[0] == '\0') return 0;

    char* version = cli_mod_strdup(p);
    if (!version) return 0;
    *out_version = version;
    return 1;
}

static char* cli_mod_path_join(const char* left, const char* right) {
    if (!left || !right) return NULL;
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);

    int need_sep = 1;
    if (left_len == 0) {
        need_sep = 0;
    } else if (cli_mod_is_path_sep(left[left_len - 1])) {
        need_sep = 0;
    }

    size_t total = left_len + (need_sep ? 1 : 0) + right_len + 1;
    char* out = (char*)malloc(total);
    if (!out) return NULL;

    size_t pos = 0;
    if (left_len > 0) {
        memcpy(out + pos, left, left_len);
        pos += left_len;
    }
    if (need_sep) {
#ifdef _WIN32
        out[pos++] = '\\';
#else
        out[pos++] = '/';
#endif
    }
    if (right_len > 0) {
        memcpy(out + pos, right, right_len);
        pos += right_len;
    }
    out[pos] = '\0';
    return out;
}

static int cli_mod_path_exists(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static int cli_mod_path_is_dir(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
#endif
}

static int cli_mod_path_is_file(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
#endif
}

static int cli_mod_make_single_dir(const char* path) {
    if (!path || path[0] == '\0') return 0;
    if (cli_mod_path_is_dir(path)) return 1;
#ifdef _WIN32
    if (_mkdir(path) == 0) return 1;
#else
    if (mkdir(path, 0777) == 0) return 1;
#endif
    if (errno == EEXIST && cli_mod_path_is_dir(path)) return 1;
    return 0;
}

static int cli_mod_mkdir_p(const char* path) {
    if (!path || path[0] == '\0') return 0;

    size_t len = strlen(path);
    char* tmp = (char*)malloc(len + 1);
    if (!tmp) return 0;
    memcpy(tmp, path, len + 1);

    size_t start = 0;
#ifdef _WIN32
    if (len >= 2 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':') {
        start = 2;
    }
#endif

    for (size_t i = start; i < len; i++) {
        if (!cli_mod_is_path_sep(tmp[i])) continue;
        if (i == 0) continue;
        char saved = tmp[i];
        tmp[i] = '\0';
        if (tmp[0] != '\0' && !cli_mod_make_single_dir(tmp)) {
            free(tmp);
            return 0;
        }
        tmp[i] = saved;
    }

    int ok = cli_mod_make_single_dir(tmp);
    free(tmp);
    return ok;
}

static int cli_mod_read_file_all(const char* path, char** out_text, size_t* out_len) {
    if (!path || !out_text) return 0;
    *out_text = NULL;
    if (out_len) *out_len = 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    char* data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return 0;
    }

    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';
    if (got != (size_t)size) {
        free(data);
        return 0;
    }

    *out_text = data;
    if (out_len) *out_len = got;
    return 1;
}

static int cli_mod_write_file_all(const char* path, const char* text) {
    if (!path || !text) return 0;

    char* parent = path_dirname_alloc(path);
    if (!parent) return 0;
    int ok_parent = cli_mod_mkdir_p(parent);
    free(parent);
    if (!ok_parent) return 0;

    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(text);
    size_t wrote = fwrite(text, 1, len, f);
    fclose(f);
    return wrote == len;
}

static void cli_mod_string_list_init(CliModStringList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_mod_string_list_free(CliModStringList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            free(list->items[i]);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cli_mod_string_list_push(CliModStringList* list, const char* value) {
    if (!list || !value) return 0;
    if (list->count >= list->capacity) {
        int new_capacity = (list->capacity <= 0) ? 16 : (list->capacity * 2);
        char** resized = (char**)realloc(list->items, (size_t)new_capacity * sizeof(char*));
        if (!resized) return 0;
        list->items = resized;
        list->capacity = new_capacity;
    }
    char* copy = cli_mod_strdup(value);
    if (!copy) return 0;
    list->items[list->count++] = copy;
    return 1;
}

static int cli_mod_string_list_push_unique(CliModStringList* list, const char* value) {
    if (!list || !value) return 0;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0) return 1;
    }
    return cli_mod_string_list_push(list, value);
}

static int cli_mod_str_cmp(const void* a, const void* b) {
    const char* const* aa = (const char* const*)a;
    const char* const* bb = (const char* const*)b;
    if (!aa || !bb) return 0;
    if (!*aa && !*bb) return 0;
    if (!*aa) return -1;
    if (!*bb) return 1;
    return strcmp(*aa, *bb);
}

static void cli_mod_string_list_sort(CliModStringList* list) {
    if (!list || list->count <= 1) return;
    qsort(list->items, (size_t)list->count, sizeof(char*), cli_mod_str_cmp);
}

static void cli_mod_trusted_key_list_init(CliModTrustedKeyList* list) {
    if (!list) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void cli_mod_trusted_key_free(CliModTrustedKey* key) {
    if (!key) return;
    free(key->id);
    free(key->algorithm);
    free(key->secret);
    key->id = NULL;
    key->algorithm = NULL;
    key->secret = NULL;
}

static void cli_mod_trusted_key_list_free(CliModTrustedKeyList* list) {
    if (!list) return;
    if (list->items) {
        for (int i = 0; i < list->count; i++) {
            cli_mod_trusted_key_free(&list->items[i]);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cli_mod_trusted_key_list_push(CliModTrustedKeyList* list,
                                         const char* id,
                                         const char* algorithm,
                                         const char* secret) {
    if (!list || !id || !algorithm || !secret) return 0;
    if (list->count >= list->capacity) {
        int new_capacity = (list->capacity <= 0) ? 8 : (list->capacity * 2);
        CliModTrustedKey* resized =
            (CliModTrustedKey*)realloc(list->items, (size_t)new_capacity * sizeof(CliModTrustedKey));
        if (!resized) return 0;
        for (int i = list->capacity; i < new_capacity; i++) {
            resized[i].id = NULL;
            resized[i].algorithm = NULL;
            resized[i].secret = NULL;
        }
        list->items = resized;
        list->capacity = new_capacity;
    }

    CliModTrustedKey* dst = &list->items[list->count];
    dst->id = cli_mod_strdup(id);
    dst->algorithm = cli_mod_strdup(algorithm);
    dst->secret = cli_mod_strdup(secret);
    if (!dst->id || !dst->algorithm || !dst->secret) {
        cli_mod_trusted_key_free(dst);
        return 0;
    }
    list->count++;
    return 1;
}

static const CliModTrustedKey* cli_mod_trusted_key_list_find(const CliModTrustedKeyList* list,
                                                             const char* id,
                                                             const char* algorithm) {
    if (!list || !id || !algorithm) return NULL;
    for (int i = 0; i < list->count; i++) {
        const CliModTrustedKey* key = &list->items[i];
        if (!key->id || !key->algorithm) continue;
        if (strcmp(key->id, id) == 0 && strcmp(key->algorithm, algorithm) == 0) {
            return key;
        }
    }
    return NULL;
}

static int cli_mod_list_dir_sorted(const char* dir_path, CliModStringList* names, char* out_error, size_t out_error_size) {
    if (!dir_path || !names) return 0;

#ifdef _WIN32
    char* pattern = cli_mod_path_join(dir_path, "*");
    if (!pattern) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while listing directory");
        return 0;
    }

    struct _finddata64i32_t fd;
    intptr_t h = _findfirst64i32(pattern, &fd);
    free(pattern);
    if (h == -1) {
        if (errno == ENOENT) {
            return 1;
        }
        cli_mod_set_error(out_error, out_error_size, "Failed to list directory");
        return 0;
    }

    do {
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
        if (!cli_mod_string_list_push(names, fd.name)) {
            _findclose(h);
            cli_mod_set_error(out_error, out_error_size, "Out of memory while listing directory");
            return 0;
        }
    } while (_findnext64i32(h, &fd) == 0);

    _findclose(h);
#else
    DIR* dir = opendir(dir_path);
    if (!dir) {
        cli_mod_set_error(out_error, out_error_size, "Failed to list directory");
        return 0;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!cli_mod_string_list_push(names, entry->d_name)) {
            closedir(dir);
            cli_mod_set_error(out_error, out_error_size, "Out of memory while listing directory");
            return 0;
        }
    }
    closedir(dir);
#endif

    cli_mod_string_list_sort(names);
    return 1;
}

static void cli_mod_dependency_init(CliModDependency* dep) {
    if (!dep) return;
    dep->name = NULL;
    dep->version = NULL;
    dep->constraint = NULL;
    dep->source = NULL;
    dep->checksum = NULL;
    dep->source_hash = NULL;
    dep->source_signature = NULL;
}

static void cli_mod_dependency_free(CliModDependency* dep) {
    if (!dep) return;
    free(dep->name);
    free(dep->version);
    free(dep->constraint);
    free(dep->source);
    free(dep->checksum);
    free(dep->source_hash);
    free(dep->source_signature);
    dep->name = NULL;
    dep->version = NULL;
    dep->constraint = NULL;
    dep->source = NULL;
    dep->checksum = NULL;
    dep->source_hash = NULL;
    dep->source_signature = NULL;
}

static int cli_mod_dependency_set(CliModDependency* dep,
                                  const char* name,
                                  const char* version,
                                  const char* constraint,
                                  const char* source,
                                  const char* checksum,
                                  const char* source_hash,
                                  const char* source_signature) {
    if (!dep || !name || !version || !source) return 0;
    if (!constraint || constraint[0] == '\0') {
        constraint = version;
    }
    char* name_copy = cli_mod_strdup(name);
    char* version_copy = cli_mod_strdup(version);
    char* constraint_copy = cli_mod_strdup(constraint);
    char* source_copy = cli_mod_strdup(source);
    char* checksum_copy = checksum ? cli_mod_strdup(checksum) : NULL;
    char* source_hash_copy = source_hash ? cli_mod_strdup(source_hash) : NULL;
    char* source_signature_copy = source_signature ? cli_mod_strdup(source_signature) : NULL;
    if (!name_copy || !version_copy || !constraint_copy || !source_copy ||
        (checksum && !checksum_copy) ||
        (source_hash && !source_hash_copy) ||
        (source_signature && !source_signature_copy)) {
        free(name_copy);
        free(version_copy);
        free(constraint_copy);
        free(source_copy);
        free(checksum_copy);
        free(source_hash_copy);
        free(source_signature_copy);
        return 0;
    }

    cli_mod_dependency_free(dep);
    dep->name = name_copy;
    dep->version = version_copy;
    dep->constraint = constraint_copy;
    dep->source = source_copy;
    dep->checksum = checksum_copy;
    dep->source_hash = source_hash_copy;
    dep->source_signature = source_signature_copy;
    return 1;
}

static void cli_mod_manifest_init(CliModManifest* manifest) {
    if (!manifest) return;
    manifest->module_name = NULL;
    manifest->deps = NULL;
    manifest->dep_count = 0;
    manifest->dep_capacity = 0;
}

static void cli_mod_manifest_free(CliModManifest* manifest) {
    if (!manifest) return;
    free(manifest->module_name);
    manifest->module_name = NULL;
    if (manifest->deps) {
        for (int i = 0; i < manifest->dep_count; i++) {
            cli_mod_dependency_free(&manifest->deps[i]);
        }
        free(manifest->deps);
    }
    manifest->deps = NULL;
    manifest->dep_count = 0;
    manifest->dep_capacity = 0;
}

static int cli_mod_manifest_set_module_name(CliModManifest* manifest, const char* module_name) {
    if (!manifest || !module_name) return 0;
    char* copy = cli_mod_strdup(module_name);
    if (!copy) return 0;
    free(manifest->module_name);
    manifest->module_name = copy;
    return 1;
}

static int cli_mod_manifest_find_dep_index(const CliModManifest* manifest, const char* dep_name) {
    if (!manifest || !dep_name) return -1;
    for (int i = 0; i < manifest->dep_count; i++) {
        if (manifest->deps[i].name && strcmp(manifest->deps[i].name, dep_name) == 0) return i;
    }
    return -1;
}

static int cli_mod_manifest_ensure_capacity(CliModManifest* manifest, int required_count) {
    if (!manifest) return 0;
    if (required_count <= manifest->dep_capacity) return 1;
    int new_capacity = manifest->dep_capacity <= 0 ? 8 : manifest->dep_capacity;
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }
    CliModDependency* resized = (CliModDependency*)realloc(manifest->deps, (size_t)new_capacity * sizeof(CliModDependency));
    if (!resized) return 0;
    for (int i = manifest->dep_capacity; i < new_capacity; i++) {
        cli_mod_dependency_init(&resized[i]);
    }
    manifest->deps = resized;
    manifest->dep_capacity = new_capacity;
    return 1;
}

static int cli_mod_manifest_add_or_replace_dep(CliModManifest* manifest,
                                               const char* dep_name,
                                               const char* version,
                                               const char* constraint,
                                               const char* source,
                                               const char* checksum,
                                               const char* source_hash,
                                               const char* source_signature) {
    if (!manifest || !dep_name || !version || !source) return 0;
    if (!constraint || constraint[0] == '\0') {
        constraint = version;
    }

    int existing = cli_mod_manifest_find_dep_index(manifest, dep_name);
    if (existing >= 0) {
        return cli_mod_dependency_set(&manifest->deps[existing],
                                      dep_name,
                                      version,
                                      constraint,
                                      source,
                                      checksum,
                                      source_hash,
                                      source_signature);
    }

    int new_index = manifest->dep_count;
    if (!cli_mod_manifest_ensure_capacity(manifest, new_index + 1)) return 0;
    if (!cli_mod_dependency_set(&manifest->deps[new_index],
                                dep_name,
                                version,
                                constraint,
                                source,
                                checksum,
                                source_hash,
                                source_signature)) return 0;
    manifest->dep_count++;
    return 1;
}

static int cli_mod_dep_cmp(const void* a, const void* b) {
    const CliModDependency* aa = (const CliModDependency*)a;
    const CliModDependency* bb = (const CliModDependency*)b;
    if (!aa || !bb) return 0;
    if (!aa->name && !bb->name) return 0;
    if (!aa->name) return -1;
    if (!bb->name) return 1;
    int name_cmp = strcmp(aa->name, bb->name);
    if (name_cmp != 0) return name_cmp;
    if (!aa->version && !bb->version) return 0;
    if (!aa->version) return -1;
    if (!bb->version) return 1;
    return strcmp(aa->version, bb->version);
}

static void cli_mod_manifest_sort_deps(CliModManifest* manifest) {
    if (!manifest || manifest->dep_count <= 1) return;
    qsort(manifest->deps, (size_t)manifest->dep_count, sizeof(CliModDependency), cli_mod_dep_cmp);
}

static int cli_mod_load_json_file(const char* path, cJSON** out_root, char* out_error, size_t out_error_size) {
    if (!path || !out_root) return 0;
    *out_root = NULL;

    char* text = NULL;
    if (!cli_mod_read_file_all(path, &text, NULL)) {
        cli_mod_set_error(out_error, out_error_size, "Failed to read JSON file");
        return 0;
    }

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        cli_mod_set_error(out_error, out_error_size, "Failed to parse JSON file");
        return 0;
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cli_mod_set_error(out_error, out_error_size, "JSON root must be an object");
        return 0;
    }

    *out_root = root;
    return 1;
}

static int cli_mod_parse_manifest_json(cJSON* root, CliModManifest* manifest, int require_checksum, char* out_error, size_t out_error_size) {
    if (!root || !manifest) return 0;

    cJSON* module_item = cJSON_GetObjectItemCaseSensitive(root, "module");
    if (!module_item || !cJSON_IsString(module_item) || !module_item->valuestring || module_item->valuestring[0] == '\0') {
        cli_mod_set_error(out_error, out_error_size, "Manifest is missing string field 'module'");
        return 0;
    }
    if (!cli_mod_validate_module_name(module_item->valuestring)) {
        cli_mod_set_error(out_error, out_error_size, "Invalid module name in manifest");
        return 0;
    }
    if (!cli_mod_manifest_set_module_name(manifest, module_item->valuestring)) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while loading manifest");
        return 0;
    }

    cJSON* deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    if (!deps) return 1;
    if (!cJSON_IsArray(deps)) {
        cli_mod_set_error(out_error, out_error_size, "Manifest field 'dependencies' must be an array");
        return 0;
    }

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, deps) {
        if (!cJSON_IsObject(item)) {
            cli_mod_set_error(out_error, out_error_size, "Manifest dependency entries must be objects");
            return 0;
        }
        cJSON* name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON* version_item = cJSON_GetObjectItemCaseSensitive(item, "version");
        cJSON* constraint_item = cJSON_GetObjectItemCaseSensitive(item, "constraint");
        cJSON* source_item = cJSON_GetObjectItemCaseSensitive(item, "source");
        cJSON* checksum_item = cJSON_GetObjectItemCaseSensitive(item, "checksum");
        cJSON* source_hash_item = cJSON_GetObjectItemCaseSensitive(item, "sourceHash");
        cJSON* source_signature_item = cJSON_GetObjectItemCaseSensitive(item, "sourceSignature");

        if (!name_item || !cJSON_IsString(name_item) || !name_item->valuestring ||
            !version_item || !cJSON_IsString(version_item) || !version_item->valuestring ||
            !source_item || !cJSON_IsString(source_item) || !source_item->valuestring) {
            cli_mod_set_error(out_error, out_error_size, "Dependency entries require string fields 'name', 'version', and 'source'");
            return 0;
        }
        if (!cli_mod_validate_dependency_name(name_item->valuestring) || !cli_mod_validate_version(version_item->valuestring)) {
            cli_mod_set_error(out_error, out_error_size, "Dependency entry has invalid name/version");
            return 0;
        }
        const char* constraint = version_item->valuestring;
        if (constraint_item) {
            if (!cJSON_IsString(constraint_item) || !constraint_item->valuestring || constraint_item->valuestring[0] == '\0') {
                cli_mod_set_error(out_error, out_error_size, "Dependency entry field 'constraint' must be a non-empty string");
                return 0;
            }
            constraint = constraint_item->valuestring;
        }
        if (!cli_mod_validate_constraint(constraint)) {
            cli_mod_set_error(out_error, out_error_size, "Dependency entry has invalid version constraint");
            return 0;
        }
        if (require_checksum) {
            if (!checksum_item || !cJSON_IsString(checksum_item) || !checksum_item->valuestring || checksum_item->valuestring[0] == '\0') {
                cli_mod_set_error(out_error, out_error_size, "Lock dependency entries require string field 'checksum'");
                return 0;
            }
            if (!source_hash_item || !cJSON_IsString(source_hash_item) || !source_hash_item->valuestring || source_hash_item->valuestring[0] == '\0') {
                cli_mod_set_error(out_error, out_error_size, "Lock dependency entries require string field 'sourceHash'");
                return 0;
            }
            if (!source_signature_item || !cJSON_IsString(source_signature_item) || !source_signature_item->valuestring ||
                source_signature_item->valuestring[0] == '\0') {
                cli_mod_set_error(out_error, out_error_size, "Lock dependency entries require string field 'sourceSignature'");
                return 0;
            }
        }

        const char* checksum = (checksum_item && cJSON_IsString(checksum_item)) ? checksum_item->valuestring : NULL;
        const char* source_hash = (source_hash_item && cJSON_IsString(source_hash_item)) ? source_hash_item->valuestring : NULL;
        const char* source_signature =
            (source_signature_item && cJSON_IsString(source_signature_item)) ? source_signature_item->valuestring : NULL;
        if (!cli_mod_manifest_add_or_replace_dep(manifest,
                                                 name_item->valuestring,
                                                 version_item->valuestring,
                                                 constraint,
                                                 source_item->valuestring,
                                                 checksum,
                                                 source_hash,
                                                 source_signature)) {
            cli_mod_set_error(out_error, out_error_size, "Out of memory while loading dependencies");
            return 0;
        }
    }

    cli_mod_manifest_sort_deps(manifest);
    return 1;
}

static int cli_mod_load_manifest_file(CliModManifest* manifest, char* out_error, size_t out_error_size) {
    if (!manifest) return 0;
    cJSON* root = NULL;
    if (!cli_mod_load_json_file(CLI_MOD_MANIFEST_FILE, &root, out_error, out_error_size)) {
        return 0;
    }
    int ok = cli_mod_parse_manifest_json(root, manifest, 0, out_error, out_error_size);
    cJSON_Delete(root);
    return ok;
}

static int cli_mod_load_lock_file(CliModManifest* manifest, char* out_error, size_t out_error_size) {
    if (!manifest) return 0;
    cJSON* root = NULL;
    if (!cli_mod_load_json_file(CLI_MOD_LOCK_FILE, &root, out_error, out_error_size)) {
        return 0;
    }
    int ok = cli_mod_parse_manifest_json(root, manifest, 1, out_error, out_error_size);
    cJSON_Delete(root);
    return ok;
}

static int cli_mod_write_manifest_json(const char* path,
                                       const CliModManifest* manifest,
                                       int include_checksum,
                                       char* out_error,
                                       size_t out_error_size) {
    if (!path || !manifest || !manifest->module_name) return 0;

    cJSON* root = cJSON_CreateObject();
    cJSON* deps = cJSON_CreateArray();
    if (!root || !deps) {
        cJSON_Delete(root);
        cJSON_Delete(deps);
        cli_mod_set_error(out_error, out_error_size, "Out of memory while serializing manifest");
        return 0;
    }

    cJSON_AddNumberToObject(root, "schemaVersion", 1);
    cJSON_AddStringToObject(root, "module", manifest->module_name);
    cJSON_AddItemToObject(root, "dependencies", deps);

    for (int i = 0; i < manifest->dep_count; i++) {
        const CliModDependency* dep = &manifest->deps[i];
        cJSON* item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            cli_mod_set_error(out_error, out_error_size, "Out of memory while serializing dependency");
            return 0;
        }
        cJSON_AddStringToObject(item, "name", dep->name ? dep->name : "");
        cJSON_AddStringToObject(item, "version", dep->version ? dep->version : "");
        cJSON_AddStringToObject(item, "constraint", dep->constraint ? dep->constraint : (dep->version ? dep->version : ""));
        cJSON_AddStringToObject(item, "source", dep->source ? dep->source : "");
        if (include_checksum) {
            cJSON_AddStringToObject(item, "checksum", dep->checksum ? dep->checksum : "");
            cJSON_AddStringToObject(item, "sourceHash", dep->source_hash ? dep->source_hash : "");
            cJSON_AddStringToObject(item, "sourceSignature", dep->source_signature ? dep->source_signature : "");
        }
        cJSON_AddItemToArray(deps, item);
    }

    char* rendered = cJSON_Print(root);
    cJSON_Delete(root);
    if (!rendered) {
        cli_mod_set_error(out_error, out_error_size, "Failed to render manifest JSON");
        return 0;
    }

    int ok = cli_mod_write_file_all(path, rendered);
    cJSON_free(rendered);
    if (!ok) {
        cli_mod_set_error(out_error, out_error_size, "Failed to write manifest file");
    }
    return ok;
}

static uint64_t cli_mod_fnv64_update(uint64_t hash, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t cli_mod_fnv64_text(uint64_t hash, const char* text) {
    if (!text) return cli_mod_fnv64_update(hash, "", 0);
    return cli_mod_fnv64_update(hash, text, strlen(text));
}

static int cli_mod_hex_value(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int cli_mod_is_hex_string(const char* text, size_t expected_len) {
    if (!text) return 0;
    size_t len = strlen(text);
    if (expected_len != 0 && len != expected_len) return 0;
    for (size_t i = 0; i < len; i++) {
        if (cli_mod_hex_value(text[i]) < 0) return 0;
    }
    return 1;
}

static void cli_mod_hex_encode_lower(const unsigned char* bytes, size_t len, char* out, size_t out_size) {
    static const char table[] = "0123456789abcdef";
    if (!out || out_size == 0) return;
    if (!bytes || out_size < (len * 2 + 1)) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = table[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = table[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int cli_mod_hash_file_bytes(const char* path, uint64_t* io_hash) {
    if (!path || !io_hash) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[4096];
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), f);
        if (got > 0) {
            *io_hash = cli_mod_fnv64_update(*io_hash, buf, got);
        }
        if (got < sizeof(buf)) {
            if (ferror(f)) {
                fclose(f);
                return 0;
            }
            break;
        }
    }
    fclose(f);
    return 1;
}

static int cli_mod_hash_tree_recursive(const char* root_path,
                                       const char* rel_path,
                                       uint64_t* io_hash,
                                       int* io_file_count,
                                       char* out_error,
                                       size_t out_error_size) {
    if (!root_path || !io_hash || !io_file_count) return 0;

    char* current = NULL;
    if (!rel_path || rel_path[0] == '\0') {
        current = cli_mod_strdup(root_path);
    } else {
        current = cli_mod_path_join(root_path, rel_path);
    }
    if (!current) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while hashing tree");
        return 0;
    }

    if (cli_mod_path_is_file(current)) {
        const char* rel = (rel_path && rel_path[0] != '\0') ? rel_path : current;
        *io_hash = cli_mod_fnv64_text(*io_hash, rel);
        *io_hash = cli_mod_fnv64_update(*io_hash, "\0", 1);
        if (!cli_mod_hash_file_bytes(current, io_hash)) {
            free(current);
            cli_mod_set_error(out_error, out_error_size, "Failed reading dependency file while hashing");
            return 0;
        }
        *io_hash = cli_mod_fnv64_update(*io_hash, "\0", 1);
        (*io_file_count)++;
        free(current);
        return 1;
    }

    if (!cli_mod_path_is_dir(current)) {
        free(current);
        cli_mod_set_error(out_error, out_error_size, "Dependency source path is missing");
        return 0;
    }

    CliModStringList names;
    cli_mod_string_list_init(&names);
    if (!cli_mod_list_dir_sorted(current, &names, out_error, out_error_size)) {
        free(current);
        cli_mod_string_list_free(&names);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < names.count; i++) {
        const char* name = names.items[i];
        if (!name) continue;
        if (strcmp(name, CLI_MOD_SOURCE_SIGNATURE_FILE) == 0) {
            continue;
        }
        size_t rel_len = rel_path ? strlen(rel_path) : 0;
        size_t name_len = strlen(name);
        size_t child_len = rel_len + (rel_len ? 1 : 0) + name_len + 1;
        char* child_rel = (char*)malloc(child_len);
        if (!child_rel) {
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Out of memory while hashing dependency");
            break;
        }
        if (rel_len > 0) {
            memcpy(child_rel, rel_path, rel_len);
            child_rel[rel_len] = '/';
            memcpy(child_rel + rel_len + 1, name, name_len);
            child_rel[rel_len + 1 + name_len] = '\0';
        } else {
            memcpy(child_rel, name, name_len + 1);
        }

        if (!cli_mod_hash_tree_recursive(root_path, child_rel, io_hash, io_file_count, out_error, out_error_size)) {
            free(child_rel);
            ok = 0;
            break;
        }
        free(child_rel);
    }

    free(current);
    cli_mod_string_list_free(&names);
    return ok;
}

static void cli_mod_format_checksum(uint64_t hash, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "fnv64:%016" PRIx64, hash);
    out[out_size - 1] = '\0';
}

static char* cli_mod_get_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, (int)sizeof(buf))) return NULL;
#else
    if (!getcwd(buf, sizeof(buf))) return NULL;
#endif
    return cli_mod_strdup(buf);
}

static int cli_mod_source_is_path(const char* source) {
    return source && cli_mod_has_prefix(source, "path:");
}

static int cli_mod_source_is_registry(const char* source) {
    return source && cli_mod_has_prefix(source, "registry:");
}

static char* cli_mod_resolve_path_source_path(const char* source) {
    if (!cli_mod_source_is_path(source)) return NULL;
    const char* value = source + 5;
    if (!value || value[0] == '\0') return NULL;
    if (path_is_absolute(value)) return cli_mod_strdup(value);

    char* cwd = cli_mod_get_cwd();
    if (!cwd) return NULL;
    char* resolved = cli_mod_path_join(cwd, value);
    free(cwd);
    return resolved;
}

static char* cli_mod_get_registry_root(void) {
    const char* root = getenv(CLI_MOD_REGISTRY_ENV);
    if (!root || root[0] == '\0') {
        root = CLI_MOD_REGISTRY_DEFAULT_ROOT;
    }
    if (path_is_absolute(root)) {
        return cli_mod_strdup(root);
    }

    char* cwd = cli_mod_get_cwd();
    if (!cwd) return NULL;
    char* resolved = cli_mod_path_join(cwd, root);
    free(cwd);
    return resolved;
}

static char* cli_mod_get_keys_file_path(void) {
    const char* keys_path = getenv(CLI_MOD_KEYS_ENV);
    if (!keys_path || keys_path[0] == '\0') {
        keys_path = CLI_MOD_KEYS_DEFAULT_FILE;
    }
    if (path_is_absolute(keys_path)) {
        return cli_mod_strdup(keys_path);
    }
    char* cwd = cli_mod_get_cwd();
    if (!cwd) return NULL;
    char* resolved = cli_mod_path_join(cwd, keys_path);
    free(cwd);
    return resolved;
}

static int cli_mod_load_trusted_keys(CliModTrustedKeyList* out_keys,
                                     char* out_error,
                                     size_t out_error_size) {
    if (!out_keys) return 0;
    cli_mod_trusted_key_list_init(out_keys);

    char* keys_path = cli_mod_get_keys_file_path();
    if (!keys_path) {
        cli_mod_set_error(out_error, out_error_size, "Failed to resolve trusted keys path");
        return 0;
    }
    if (!cli_mod_path_is_file(keys_path)) {
        free(keys_path);
        return 1;
    }

    cJSON* root = NULL;
    if (!cli_mod_load_json_file(keys_path, &root, out_error, out_error_size)) {
        free(keys_path);
        return 0;
    }
    free(keys_path);

    cJSON* keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
    if (!keys) {
        cJSON_Delete(root);
        return 1;
    }
    if (!cJSON_IsArray(keys)) {
        cJSON_Delete(root);
        cli_mod_set_error(out_error, out_error_size, "Trusted keys file field 'keys' must be an array");
        return 0;
    }

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, keys) {
        if (!cJSON_IsObject(item)) {
            cJSON_Delete(root);
            cli_mod_set_error(out_error, out_error_size, "Trusted keys entries must be objects");
            cli_mod_trusted_key_list_free(out_keys);
            return 0;
        }
        cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON* algorithm_item = cJSON_GetObjectItemCaseSensitive(item, "algorithm");
        cJSON* secret_item = cJSON_GetObjectItemCaseSensitive(item, "secret");
        if (!id_item || !cJSON_IsString(id_item) || !id_item->valuestring || id_item->valuestring[0] == '\0' ||
            !algorithm_item || !cJSON_IsString(algorithm_item) || !algorithm_item->valuestring ||
            !secret_item || !cJSON_IsString(secret_item) || !secret_item->valuestring || secret_item->valuestring[0] == '\0') {
            cJSON_Delete(root);
            cli_mod_set_error(out_error, out_error_size, "Trusted keys require non-empty string fields 'id', 'algorithm', and 'secret'");
            cli_mod_trusted_key_list_free(out_keys);
            return 0;
        }
        if (!cli_mod_trusted_key_list_push(out_keys,
                                           id_item->valuestring,
                                           algorithm_item->valuestring,
                                           secret_item->valuestring)) {
            cJSON_Delete(root);
            cli_mod_set_error(out_error, out_error_size, "Out of memory while loading trusted keys");
            cli_mod_trusted_key_list_free(out_keys);
            return 0;
        }
    }

    cJSON_Delete(root);
    return 1;
}

static int cli_mod_load_signature_descriptor(const char* source_path,
                                             int* out_has_signature,
                                             char** out_key_id,
                                             char** out_algorithm,
                                             char** out_signature_hex,
                                             char* out_error,
                                             size_t out_error_size) {
    if (!out_has_signature || !out_key_id || !out_algorithm || !out_signature_hex) return 0;
    *out_has_signature = 0;
    *out_key_id = NULL;
    *out_algorithm = NULL;
    *out_signature_hex = NULL;

    if (!source_path || source_path[0] == '\0') return 1;
    if (!cli_mod_path_is_dir(source_path) && !cli_mod_path_is_file(source_path)) return 1;

    char* sig_path = cli_mod_path_join(source_path, CLI_MOD_SOURCE_SIGNATURE_FILE);
    if (!sig_path) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while resolving signature file path");
        return 0;
    }
    if (!cli_mod_path_is_file(sig_path)) {
        free(sig_path);
        return 1;
    }

    char* sig_text = NULL;
    if (!cli_mod_read_file_all(sig_path, &sig_text, NULL)) {
        free(sig_path);
        cli_mod_set_error(out_error, out_error_size, "Failed to read dependency signature file");
        return 0;
    }
    free(sig_path);

    cJSON* sig_root = cJSON_Parse(sig_text);
    free(sig_text);
    if (!sig_root || !cJSON_IsObject(sig_root)) {
        if (sig_root) cJSON_Delete(sig_root);
        cli_mod_set_error(out_error, out_error_size, "Dependency signature file must be a JSON object");
        return 0;
    }

    cJSON* key_id_item = cJSON_GetObjectItemCaseSensitive(sig_root, "keyId");
    cJSON* algorithm_item = cJSON_GetObjectItemCaseSensitive(sig_root, "algorithm");
    cJSON* signature_item = cJSON_GetObjectItemCaseSensitive(sig_root, "signature");
    if (!key_id_item || !cJSON_IsString(key_id_item) || !key_id_item->valuestring || key_id_item->valuestring[0] == '\0' ||
        !algorithm_item || !cJSON_IsString(algorithm_item) || !algorithm_item->valuestring || algorithm_item->valuestring[0] == '\0' ||
        !signature_item || !cJSON_IsString(signature_item) || !signature_item->valuestring || signature_item->valuestring[0] == '\0') {
        cJSON_Delete(sig_root);
        cli_mod_set_error(out_error, out_error_size, "Dependency signature requires non-empty 'keyId', 'algorithm', and 'signature' fields");
        return 0;
    }

    if (!cli_mod_is_hex_string(signature_item->valuestring, 64)) {
        cJSON_Delete(sig_root);
        cli_mod_set_error(out_error, out_error_size, "Dependency signature field 'signature' must be a 64-character hex SHA-256 HMAC");
        return 0;
    }

    char* key_id = cli_mod_strdup(key_id_item->valuestring);
    char* algorithm = cli_mod_strdup(algorithm_item->valuestring);
    char* signature_hex = cli_mod_strdup(signature_item->valuestring);
    if (!key_id || !algorithm || !signature_hex) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cJSON_Delete(sig_root);
        cli_mod_set_error(out_error, out_error_size, "Out of memory while loading dependency signature");
        return 0;
    }
    for (char* p = signature_hex; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    cJSON_Delete(sig_root);
    *out_has_signature = 1;
    *out_key_id = key_id;
    *out_algorithm = algorithm;
    *out_signature_hex = signature_hex;
    return 1;
}

static int cli_mod_build_signature_payload(const CliModDependency* dep,
                                           const char* dependency_checksum,
                                           char** out_payload,
                                           char* out_error,
                                           size_t out_error_size) {
    if (!dep || !out_payload) return 0;
    *out_payload = NULL;
    const char* dep_constraint = cli_mod_dependency_constraint_or_version(dep);
    if (!dep_constraint) dep_constraint = "";
    if (!dependency_checksum) dependency_checksum = "";

    size_t needed = snprintf(NULL,
                             0,
                             "tablo-mod-signature-v1\nname=%s\nversion=%s\nconstraint=%s\nsource=%s\nchecksum=%s\n",
                             dep->name ? dep->name : "",
                             dep->version ? dep->version : "",
                             dep_constraint,
                             dep->source ? dep->source : "",
                             dependency_checksum) + 1;
    char* payload = (char*)malloc(needed);
    if (!payload) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while preparing dependency signature payload");
        return 0;
    }
    snprintf(payload,
             needed,
             "tablo-mod-signature-v1\nname=%s\nversion=%s\nconstraint=%s\nsource=%s\nchecksum=%s\n",
             dep->name ? dep->name : "",
             dep->version ? dep->version : "",
             dep_constraint,
             dep->source ? dep->source : "",
             dependency_checksum);
    payload[needed - 1] = '\0';
    *out_payload = payload;
    return 1;
}

static const char* cli_mod_registry_source_name(const CliModDependency* dep) {
    if (!dep) return NULL;
    const char* source_name = NULL;
    if (dep->source && cli_mod_source_is_registry(dep->source)) {
        source_name = dep->source + 9;
    }
    if (!source_name || source_name[0] == '\0') {
        source_name = dep->name;
    }
    if (!source_name || !cli_mod_validate_dependency_name(source_name)) return NULL;
    return source_name;
}

static const char* cli_mod_dependency_constraint_or_version(const CliModDependency* dep) {
    if (!dep) return NULL;
    if (dep->constraint && dep->constraint[0] != '\0') return dep->constraint;
    return dep->version;
}

static int cli_mod_find_highest_registry_version(const CliModDependency* dep,
                                                 const char* constraint,
                                                 char** out_version,
                                                 char* out_error,
                                                 size_t out_error_size) {
    if (!dep || !out_version) return 0;
    *out_version = NULL;

    const char* source_name = cli_mod_registry_source_name(dep);
    if (!source_name) {
        cli_mod_set_error(out_error, out_error_size, "Invalid registry source name");
        return 0;
    }

    const char* effective_constraint = constraint;
    if (!effective_constraint || effective_constraint[0] == '\0') {
        effective_constraint = cli_mod_dependency_constraint_or_version(dep);
    }
    if (!effective_constraint || !cli_mod_validate_constraint(effective_constraint)) {
        cli_mod_set_error(out_error, out_error_size, "Invalid version constraint");
        return 0;
    }

    char* root = cli_mod_get_registry_root();
    if (!root) {
        cli_mod_set_error(out_error, out_error_size, "Failed to resolve registry root");
        return 0;
    }

    char* pkg_root = cli_mod_path_join(root, source_name);
    free(root);
    if (!pkg_root) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while resolving registry package path");
        return 0;
    }
    if (!cli_mod_path_is_dir(pkg_root)) {
        free(pkg_root);
        cli_mod_set_error(out_error, out_error_size, "Registry package not found");
        return 0;
    }

    CliModStringList versions;
    cli_mod_string_list_init(&versions);
    if (!cli_mod_list_dir_sorted(pkg_root, &versions, out_error, out_error_size)) {
        free(pkg_root);
        cli_mod_string_list_free(&versions);
        return 0;
    }

    int found = 0;
    CliModSemver best_semver;
    char* best_text = NULL;
    for (int i = 0; i < versions.count; i++) {
        const char* candidate = versions.items[i];
        if (!candidate || candidate[0] == '\0') continue;

        char* candidate_path = cli_mod_path_join(pkg_root, candidate);
        if (!candidate_path) {
            cli_mod_set_error(out_error, out_error_size, "Out of memory while scanning registry versions");
            free(best_text);
            cli_mod_string_list_free(&versions);
            free(pkg_root);
            return 0;
        }
        int candidate_is_dir = cli_mod_path_is_dir(candidate_path);
        free(candidate_path);
        if (!candidate_is_dir) continue;

        CliModSemver candidate_semver;
        if (!cli_mod_parse_semver(candidate, &candidate_semver)) continue;
        if (!cli_mod_constraint_matches_semver(effective_constraint, &candidate_semver)) continue;

        if (!found || cli_mod_semver_compare(&candidate_semver, &best_semver) > 0) {
            char* copy = cli_mod_strdup(candidate);
            if (!copy) {
                cli_mod_set_error(out_error, out_error_size, "Out of memory while selecting registry version");
                free(best_text);
                cli_mod_string_list_free(&versions);
                free(pkg_root);
                return 0;
            }
            free(best_text);
            best_text = copy;
            best_semver = candidate_semver;
            found = 1;
        }
    }

    cli_mod_string_list_free(&versions);
    free(pkg_root);

    if (!found) {
        cli_mod_set_error(out_error, out_error_size, "No registry version satisfies constraint");
        return 0;
    }

    *out_version = best_text;
    return 1;
}

static char* cli_mod_resolve_registry_source_path(const CliModDependency* dep,
                                                  char* out_error,
                                                  size_t out_error_size) {
    if (!dep || !dep->source || !cli_mod_source_is_registry(dep->source)) return NULL;
    const char* source_name = cli_mod_registry_source_name(dep);
    if (!source_name) {
        cli_mod_set_error(out_error, out_error_size, "Invalid registry source name");
        return NULL;
    }

    char* root = cli_mod_get_registry_root();
    if (!root) {
        cli_mod_set_error(out_error, out_error_size, "Failed to resolve registry root");
        return NULL;
    }

    char* pkg_root = cli_mod_path_join(root, source_name);
    free(root);
    if (!pkg_root) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while resolving registry path");
        return NULL;
    }
    char* pkg_ver = cli_mod_path_join(pkg_root, dep->version ? dep->version : "");
    free(pkg_root);
    if (!pkg_ver) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while resolving registry version path");
        return NULL;
    }
    return pkg_ver;
}

static char* cli_mod_resolve_dependency_source_path(const CliModDependency* dep,
                                                    char* out_error,
                                                    size_t out_error_size) {
    if (!dep || !dep->source) return NULL;
    if (cli_mod_source_is_path(dep->source)) {
        char* resolved = cli_mod_resolve_path_source_path(dep->source);
        if (!resolved) {
            cli_mod_set_error(out_error, out_error_size, "Failed to resolve path dependency");
        }
        return resolved;
    }
    if (cli_mod_source_is_registry(dep->source)) {
        return cli_mod_resolve_registry_source_path(dep, out_error, out_error_size);
    }
    cli_mod_set_error(out_error, out_error_size, "Unsupported dependency source");
    return NULL;
}

static int cli_mod_compute_dependency_checksum(const CliModDependency* dep,
                                               char* out_checksum,
                                               size_t out_checksum_size,
                                               char* out_error,
                                               size_t out_error_size) {
    if (!dep || !dep->name || !dep->version || !dep->source || !out_checksum || out_checksum_size == 0) return 0;

    uint64_t hash = 1469598103934665603ull;
    hash = cli_mod_fnv64_text(hash, dep->name);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep->version);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep->source);
    hash = cli_mod_fnv64_update(hash, "\0", 1);

    int require_tree = cli_mod_source_is_path(dep->source);
    int try_tree = require_tree || cli_mod_source_is_registry(dep->source);
    if (try_tree) {
        char resolve_error[256];
        resolve_error[0] = '\0';
        char* source_path = cli_mod_resolve_dependency_source_path(dep, resolve_error, sizeof(resolve_error));
        if (source_path && cli_mod_path_exists(source_path)) {
            int file_count = 0;
            char hash_error[256];
            hash_error[0] = '\0';
            if (!cli_mod_hash_tree_recursive(source_path, "", &hash, &file_count, hash_error, sizeof(hash_error))) {
                free(source_path);
                if (hash_error[0] != '\0') {
                    cli_mod_set_error(out_error, out_error_size, hash_error);
                } else {
                    cli_mod_set_error(out_error, out_error_size, "Failed to hash dependency source");
                }
                return 0;
            }
            free(source_path);

            hash = cli_mod_fnv64_text(hash, "files");
            hash = cli_mod_fnv64_update(hash, &file_count, sizeof(file_count));
        } else {
            if (source_path) free(source_path);
            if (require_tree) {
                if (resolve_error[0] != '\0') {
                    cli_mod_set_error(out_error, out_error_size, resolve_error);
                } else {
                    cli_mod_set_error(out_error, out_error_size, "Path dependency source does not exist");
                }
                return 0;
            }
        }
    }

    cli_mod_format_checksum(hash, out_checksum, out_checksum_size);
    return 1;
}

static int cli_mod_verify_dependency_source_signature(const CliModDependency* dep,
                                                      const char* dependency_checksum,
                                                      const char* source_path_or_null,
                                                      char* out_signature,
                                                      size_t out_signature_size,
                                                      char* out_error,
                                                      size_t out_error_size) {
    if (!dep || !out_signature || out_signature_size == 0) return 0;
    snprintf(out_signature, out_signature_size, "%s", CLI_MOD_SOURCE_SIGNATURE_DEFAULT);
    out_signature[out_signature_size - 1] = '\0';

    int has_signature = 0;
    char* key_id = NULL;
    char* algorithm = NULL;
    char* signature_hex = NULL;
    if (!cli_mod_load_signature_descriptor(source_path_or_null,
                                           &has_signature,
                                           &key_id,
                                           &algorithm,
                                           &signature_hex,
                                           out_error,
                                           out_error_size)) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        return 0;
    }
    if (!has_signature) {
        return 1;
    }

    if (!algorithm || strcmp(algorithm, CLI_MOD_SIGNATURE_SCHEME_HMAC_SHA256) != 0) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cli_mod_set_error(out_error, out_error_size, "Unsupported dependency signature algorithm");
        return 0;
    }

    CliModTrustedKeyList trusted_keys;
    cli_mod_trusted_key_list_init(&trusted_keys);
    if (!cli_mod_load_trusted_keys(&trusted_keys, out_error, out_error_size)) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cli_mod_trusted_key_list_free(&trusted_keys);
        return 0;
    }

    const CliModTrustedKey* trusted =
        cli_mod_trusted_key_list_find(&trusted_keys, key_id ? key_id : "", algorithm ? algorithm : "");
    if (!trusted || !trusted->secret) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cli_mod_trusted_key_list_free(&trusted_keys);
        cli_mod_set_error(out_error, out_error_size, "Dependency signature key is not trusted");
        return 0;
    }

    char* payload = NULL;
    if (!cli_mod_build_signature_payload(dep, dependency_checksum, &payload, out_error, out_error_size)) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cli_mod_trusted_key_list_free(&trusted_keys);
        return 0;
    }

    unsigned char mac[32];
    crypto_hmac_sha256((const uint8_t*)trusted->secret,
                       strlen(trusted->secret),
                       (const uint8_t*)payload,
                       strlen(payload),
                       mac);
    free(payload);

    char computed_hex[65];
    cli_mod_hex_encode_lower(mac, sizeof(mac), computed_hex, sizeof(computed_hex));
    if (strcmp(computed_hex, signature_hex ? signature_hex : "") != 0) {
        free(key_id);
        free(algorithm);
        free(signature_hex);
        cli_mod_trusted_key_list_free(&trusted_keys);
        cli_mod_set_error(out_error, out_error_size, "Dependency signature verification failed");
        return 0;
    }

    int wrote = snprintf(out_signature,
                         out_signature_size,
                         "sig:%s:%s:%s",
                         algorithm,
                         key_id ? key_id : "",
                         signature_hex ? signature_hex : "");
    out_signature[out_signature_size - 1] = '\0';
    free(key_id);
    free(algorithm);
    free(signature_hex);
    cli_mod_trusted_key_list_free(&trusted_keys);
    if (wrote < 0 || (size_t)wrote >= out_signature_size) {
        cli_mod_set_error(out_error, out_error_size, "Dependency source signature string is too large");
        return 0;
    }
    return 1;
}

static int cli_mod_compute_dependency_source_provenance(const CliModDependency* dep,
                                                        const char* dependency_checksum,
                                                        char* out_source_hash,
                                                        size_t out_source_hash_size,
                                                        char* out_source_signature,
                                                        size_t out_source_signature_size,
                                                        char* out_error,
                                                        size_t out_error_size) {
    if (!dep || !dep->name || !dep->version || !dep->source ||
        !out_source_hash || out_source_hash_size == 0 ||
        !out_source_signature || out_source_signature_size == 0) {
        return 0;
    }
    if (!dependency_checksum) dependency_checksum = "";

    const char* dep_constraint = cli_mod_dependency_constraint_or_version(dep);
    if (!dep_constraint) dep_constraint = "";

    char resolve_error[256];
    resolve_error[0] = '\0';
    char* source_path = cli_mod_resolve_dependency_source_path(dep, resolve_error, sizeof(resolve_error));
    const char* source_path_identity = source_path ? source_path : "<unresolved>";

    if (!cli_mod_verify_dependency_source_signature(dep,
                                                    dependency_checksum,
                                                    (source_path && cli_mod_path_exists(source_path) && cli_mod_path_is_dir(source_path))
                                                        ? source_path
                                                        : NULL,
                                                    out_source_signature,
                                                    out_source_signature_size,
                                                    out_error,
                                                    out_error_size)) {
        if (source_path) free(source_path);
        return 0;
    }

    uint64_t hash = 1469598103934665603ull;
    hash = cli_mod_fnv64_text(hash, "source-provenance-v2");
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep->name);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep->version);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep_constraint);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dep->source);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, dependency_checksum);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    if (cli_mod_source_is_registry(dep->source)) {
        const char* source_name = cli_mod_registry_source_name(dep);
        hash = cli_mod_fnv64_text(hash, source_name ? source_name : "<invalid-registry-source>");
        hash = cli_mod_fnv64_update(hash, "\0", 1);
    }
    hash = cli_mod_fnv64_text(hash, source_path_identity);
    hash = cli_mod_fnv64_update(hash, "\0", 1);
    hash = cli_mod_fnv64_text(hash, out_source_signature);
    hash = cli_mod_fnv64_update(hash, "\0", 1);

    cli_mod_format_checksum(hash, out_source_hash, out_source_hash_size);
    if (source_path) free(source_path);
    return 1;
}

static int cli_mod_write_manifest_file(const CliModManifest* manifest, char* out_error, size_t out_error_size) {
    return cli_mod_write_manifest_json(CLI_MOD_MANIFEST_FILE, manifest, 0, out_error, out_error_size);
}

static int cli_mod_write_lock_file(const CliModManifest* manifest, char* out_error, size_t out_error_size) {
    if (!manifest || !manifest->module_name) return 0;

    CliModManifest with_checksums;
    cli_mod_manifest_init(&with_checksums);
    if (!cli_mod_manifest_set_module_name(&with_checksums, manifest->module_name)) {
        cli_mod_set_error(out_error, out_error_size, "Out of memory while preparing lock file");
        cli_mod_manifest_free(&with_checksums);
        return 0;
    }

    for (int i = 0; i < manifest->dep_count; i++) {
        const CliModDependency* dep = &manifest->deps[i];
        char checksum[64];
        char source_hash[64];
        char source_signature[512];
        checksum[0] = '\0';
        source_hash[0] = '\0';
        source_signature[0] = '\0';
        if (!cli_mod_compute_dependency_checksum(dep, checksum, sizeof(checksum), out_error, out_error_size)) {
            cli_mod_manifest_free(&with_checksums);
            return 0;
        }
        if (!cli_mod_compute_dependency_source_provenance(dep,
                                                          checksum,
                                                          source_hash,
                                                          sizeof(source_hash),
                                                          source_signature,
                                                          sizeof(source_signature),
                                                          out_error,
                                                          out_error_size)) {
            cli_mod_manifest_free(&with_checksums);
            return 0;
        }
        if (!cli_mod_manifest_add_or_replace_dep(&with_checksums,
                                                 dep->name,
                                                 dep->version,
                                                 dep->constraint,
                                                 dep->source,
                                                 checksum,
                                                 source_hash,
                                                 source_signature)) {
            cli_mod_set_error(out_error, out_error_size, "Out of memory while preparing lock dependencies");
            cli_mod_manifest_free(&with_checksums);
            return 0;
        }
    }
    cli_mod_manifest_sort_deps(&with_checksums);

    int ok = cli_mod_write_manifest_json(CLI_MOD_LOCK_FILE, &with_checksums, 1, out_error, out_error_size);
    cli_mod_manifest_free(&with_checksums);
    return ok;
}

static int cli_mod_remove_tree(const char* path, char* out_error, size_t out_error_size) {
    if (!path || path[0] == '\0') return 1;
    if (!cli_mod_path_exists(path)) return 1;

    if (cli_mod_path_is_file(path)) {
        if (remove(path) != 0) {
            cli_mod_set_error(out_error, out_error_size, "Failed to remove file");
            return 0;
        }
        return 1;
    }

    if (!cli_mod_path_is_dir(path)) {
        cli_mod_set_error(out_error, out_error_size, "Path is not a regular file or directory");
        return 0;
    }

    CliModStringList names;
    cli_mod_string_list_init(&names);
    if (!cli_mod_list_dir_sorted(path, &names, out_error, out_error_size)) {
        cli_mod_string_list_free(&names);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < names.count; i++) {
        char* child = cli_mod_path_join(path, names.items[i]);
        if (!child) {
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Out of memory while removing tree");
            break;
        }
        if (!cli_mod_remove_tree(child, out_error, out_error_size)) {
            free(child);
            ok = 0;
            break;
        }
        free(child);
    }
    cli_mod_string_list_free(&names);
    if (!ok) return 0;

#ifdef _WIN32
    if (_rmdir(path) != 0) {
        cli_mod_set_error(out_error, out_error_size, "Failed to remove directory");
        return 0;
    }
#else
    if (rmdir(path) != 0) {
        cli_mod_set_error(out_error, out_error_size, "Failed to remove directory");
        return 0;
    }
#endif
    return 1;
}

static int cli_mod_copy_file(const char* source, const char* target, char* out_error, size_t out_error_size) {
    if (!source || !target) return 0;
    FILE* in = fopen(source, "rb");
    if (!in) {
        cli_mod_set_error(out_error, out_error_size, "Failed to open source file for vendoring");
        return 0;
    }

    char* parent = path_dirname_alloc(target);
    if (!parent || !cli_mod_mkdir_p(parent)) {
        if (parent) free(parent);
        fclose(in);
        cli_mod_set_error(out_error, out_error_size, "Failed to create destination directory");
        return 0;
    }
    free(parent);

    FILE* out = fopen(target, "wb");
    if (!out) {
        fclose(in);
        cli_mod_set_error(out_error, out_error_size, "Failed to open target file for vendoring");
        return 0;
    }

    unsigned char buf[4096];
    int ok = 1;
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), in);
        if (got > 0) {
            size_t wrote = fwrite(buf, 1, got, out);
            if (wrote != got) {
                ok = 0;
                cli_mod_set_error(out_error, out_error_size, "Failed while writing vendored file");
                break;
            }
        }
        if (got < sizeof(buf)) {
            if (ferror(in)) {
                ok = 0;
                cli_mod_set_error(out_error, out_error_size, "Failed while reading source file");
            }
            break;
        }
    }

    fclose(in);
    fclose(out);
    return ok;
}

static int cli_mod_copy_tree(const char* source, const char* target, char* out_error, size_t out_error_size) {
    if (!source || !target) return 0;

    if (cli_mod_path_is_file(source)) {
        return cli_mod_copy_file(source, target, out_error, out_error_size);
    }
    if (!cli_mod_path_is_dir(source)) {
        cli_mod_set_error(out_error, out_error_size, "Dependency source path does not exist");
        return 0;
    }

    if (!cli_mod_mkdir_p(target)) {
        cli_mod_set_error(out_error, out_error_size, "Failed to create vendor destination");
        return 0;
    }

    CliModStringList names;
    cli_mod_string_list_init(&names);
    if (!cli_mod_list_dir_sorted(source, &names, out_error, out_error_size)) {
        cli_mod_string_list_free(&names);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < names.count; i++) {
        char* src_child = cli_mod_path_join(source, names.items[i]);
        char* dst_child = cli_mod_path_join(target, names.items[i]);
        if (!src_child || !dst_child) {
            free(src_child);
            free(dst_child);
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Out of memory while vendoring dependency");
            break;
        }
        if (!cli_mod_copy_tree(src_child, dst_child, out_error, out_error_size)) {
            free(src_child);
            free(dst_child);
            ok = 0;
            break;
        }
        free(src_child);
        free(dst_child);
    }
    cli_mod_string_list_free(&names);
    return ok;
}

static int cli_mod_is_skipped_scan_dir(const char* name) {
    if (!name) return 0;
    if (strcmp(name, ".git") == 0) return 1;
    if (strcmp(name, "build") == 0) return 1;
    if (strcmp(name, "Testing") == 0) return 1;
    if (strcmp(name, ".claude") == 0) return 1;
    if (strcmp(name, ".tmp") == 0) return 1;
    if (strcmp(name, CLI_MOD_VENDOR_DIR) == 0) return 1;
    return 0;
}

static int cli_mod_collect_source_files_recursive(const char* dir_path,
                                               CliModStringList* files,
                                               char* out_error,
                                               size_t out_error_size) {
    if (!dir_path || !files) return 0;
    CliModStringList names;
    cli_mod_string_list_init(&names);
    if (!cli_mod_list_dir_sorted(dir_path, &names, out_error, out_error_size)) {
        cli_mod_string_list_free(&names);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < names.count; i++) {
        const char* name = names.items[i];
        char* full = cli_mod_path_join(dir_path, name);
        if (!full) {
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Out of memory while scanning project files");
            break;
        }

        if (cli_mod_path_is_dir(full)) {
            if (!cli_mod_is_skipped_scan_dir(name)) {
                if (!cli_mod_collect_source_files_recursive(full, files, out_error, out_error_size)) {
                    free(full);
                    ok = 0;
                    break;
                }
            }
        } else if (cli_mod_path_is_file(full) && cli_mod_has_suffix(name, ".tblo")) {
            if (!cli_mod_string_list_push(files, full)) {
                free(full);
                ok = 0;
                cli_mod_set_error(out_error, out_error_size, "Out of memory while collecting project files");
                break;
            }
        }

        free(full);
    }

    cli_mod_string_list_free(&names);
    return ok;
}

static int cli_mod_extract_imports(const char* text, CliModStringList* imports) {
    if (!text || !imports) return 0;
    const char* p = text;
    while ((p = strstr(p, "import")) != NULL) {
        if (p > text) {
            char prev = p[-1];
            if (isalnum((unsigned char)prev) || prev == '_') {
                p++;
                continue;
            }
        }

        const char* q = p + 6;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '"') {
            p = q;
            continue;
        }
        q++;
        const char* start = q;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1] != '\0') {
                q += 2;
            } else {
                q++;
            }
        }
        if (*q != '"') break;

        size_t len = (size_t)(q - start);
        char* import_path = (char*)malloc(len + 1);
        if (!import_path) return 0;
        memcpy(import_path, start, len);
        import_path[len] = '\0';
        if (!cli_mod_string_list_push_unique(imports, import_path)) {
            free(import_path);
            return 0;
        }
        free(import_path);
        p = q + 1;
    }
    return 1;
}

static int cli_mod_collect_project_imports(CliModStringList* imports, char* out_error, size_t out_error_size) {
    if (!imports) return 0;
    CliModStringList files;
    cli_mod_string_list_init(&files);
    if (!cli_mod_collect_source_files_recursive(".", &files, out_error, out_error_size)) {
        cli_mod_string_list_free(&files);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < files.count; i++) {
        char* text = NULL;
        if (!cli_mod_read_file_all(files.items[i], &text, NULL)) {
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Failed reading project source during tidy");
            break;
        }
        if (!cli_mod_extract_imports(text, imports)) {
            free(text);
            ok = 0;
            cli_mod_set_error(out_error, out_error_size, "Failed extracting imports during tidy");
            break;
        }
        free(text);
    }

    cli_mod_string_list_free(&files);
    return ok;
}

static int cli_mod_dependency_is_used(const char* dep_name, const CliModStringList* imports) {
    if (!dep_name || !imports) return 0;

    size_t dep_len = strlen(dep_name);
    size_t vendor_prefix_len = strlen("vendor/") + dep_len;
    char* vendor_prefix = (char*)malloc(vendor_prefix_len + 1);
    if (!vendor_prefix) return 0;
    snprintf(vendor_prefix, vendor_prefix_len + 1, "vendor/%s", dep_name);

    int used = 0;
    for (int i = 0; i < imports->count; i++) {
        const char* imp = imports->items[i];
        if (!imp) continue;

        size_t imp_len = strlen(imp);
        char* normalized = (char*)malloc(imp_len + 1);
        if (!normalized) continue;
        for (size_t k = 0; k < imp_len; k++) {
            char c = imp[k];
            normalized[k] = (c == '\\') ? '/' : c;
        }
        normalized[imp_len] = '\0';

        int vendor_match = (strncmp(normalized, vendor_prefix, vendor_prefix_len) == 0 &&
                            (normalized[vendor_prefix_len] == '\0' || normalized[vendor_prefix_len] == '/'));
        int direct_match = (strncmp(normalized, dep_name, dep_len) == 0 &&
                            (normalized[dep_len] == '\0' || normalized[dep_len] == '/'));
        if (vendor_match || direct_match) {
            used = 1;
            free(normalized);
            break;
        }
        free(normalized);
    }

    free(vendor_prefix);
    return used;
}

static int cli_mod_cmd_init(int argc, char** argv) {
    const char* module_name = NULL;
    if (argc >= 4) {
        module_name = argv[3];
    } else {
        char* cwd = cli_mod_get_cwd();
        if (!cwd) {
            fprintf(stderr, "Error: failed to determine current directory\n");
            return 1;
        }
        const char* base = cwd;
        const char* last_slash = strrchr(cwd, '/');
        const char* last_backslash = strrchr(cwd, '\\');
        if (last_slash && last_backslash) {
            base = (last_slash > last_backslash) ? (last_slash + 1) : (last_backslash + 1);
        } else if (last_slash) {
            base = last_slash + 1;
        } else if (last_backslash) {
            base = last_backslash + 1;
        }
        if (!base || base[0] == '\0') base = "app";
        module_name = cli_mod_strdup(base);
        free(cwd);
    }

    if (!module_name || !cli_mod_validate_module_name(module_name)) {
        fprintf(stderr, "Error: invalid module name. Use letters, digits, '_', '-', '.', '/'\n");
        if (argc < 4 && module_name) free((void*)module_name);
        return 1;
    }
    if (argc > 4) {
        fprintf(stderr, "Error: usage: tablo mod init [module]\n");
        if (argc < 4) free((void*)module_name);
        return 1;
    }
    if (cli_mod_path_exists(CLI_MOD_MANIFEST_FILE)) {
        fprintf(stderr, "Error: %s already exists\n", CLI_MOD_MANIFEST_FILE);
        if (argc < 4) free((void*)module_name);
        return 1;
    }

    CliModManifest manifest;
    cli_mod_manifest_init(&manifest);
    if (!cli_mod_manifest_set_module_name(&manifest, module_name)) {
        fprintf(stderr, "Error: out of memory while initializing module\n");
        cli_mod_manifest_free(&manifest);
        if (argc < 4) free((void*)module_name);
        return 1;
    }
    if (argc < 4) free((void*)module_name);

    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_write_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write manifest");
        cli_mod_manifest_free(&manifest);
        return 1;
    }
    if (!cli_mod_write_lock_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write lock file");
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    printf("Initialized %s for module '%s'.\n", CLI_MOD_MANIFEST_FILE, manifest.module_name);
    printf("Created %s.\n", CLI_MOD_LOCK_FILE);
    cli_mod_manifest_free(&manifest);
    return 0;
}

static int cli_mod_parse_add_spec(const char* spec, char** out_name, char** out_constraint) {
    if (!spec || !out_name || !out_constraint) return 0;
    *out_name = NULL;
    *out_constraint = NULL;
    const char* at = strrchr(spec, '@');
    if (!at || at == spec || at[1] == '\0') return 0;

    size_t name_len = (size_t)(at - spec);
    size_t constraint_len = strlen(at + 1);
    char* name = (char*)malloc(name_len + 1);
    char* constraint = (char*)malloc(constraint_len + 1);
    if (!name || !constraint) {
        free(name);
        free(constraint);
        return 0;
    }
    memcpy(name, spec, name_len);
    name[name_len] = '\0';
    memcpy(constraint, at + 1, constraint_len + 1);

    *out_name = name;
    *out_constraint = constraint;
    return 1;
}

static int cli_mod_cmd_add(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Error: usage: tablo mod add <dependency@constraint> [--source <source>]\n");
        return 1;
    }

    char* dep_name = NULL;
    char* dep_constraint = NULL;
    if (!cli_mod_parse_add_spec(argv[3], &dep_name, &dep_constraint)) {
        fprintf(stderr, "Error: dependency spec must be '<name>@<constraint>'\n");
        return 1;
    }
    if (!cli_mod_validate_dependency_name(dep_name) || !cli_mod_validate_constraint(dep_constraint)) {
        fprintf(stderr, "Error: invalid dependency name or version constraint\n");
        free(dep_name);
        free(dep_constraint);
        return 1;
    }

    const char* source = NULL;
    for (int i = 4; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--source") == 0) {
            if ((i + 1) >= argc) {
                fprintf(stderr, "Error: --source requires a value\n");
                free(dep_name);
                free(dep_constraint);
                return 1;
            }
            source = argv[++i];
        } else if (strncmp(arg, "--source=", 9) == 0) {
            source = arg + 9;
        } else {
            fprintf(stderr, "Error: unknown add option '%s'\n", arg);
            free(dep_name);
            free(dep_constraint);
            return 1;
        }
    }

    char default_source[512];
    if (!source || source[0] == '\0') {
        snprintf(default_source, sizeof(default_source), "registry:%s", dep_name);
        default_source[sizeof(default_source) - 1] = '\0';
        source = default_source;
    }
    if (!cli_mod_source_is_path(source) && !cli_mod_source_is_registry(source)) {
        fprintf(stderr, "Error: unsupported source '%s' (expected path:... or registry:...)\n", source);
        free(dep_name);
        free(dep_constraint);
        return 1;
    }

    char* resolved_version = NULL;
    if (cli_mod_source_is_registry(source)) {
        CliModDependency probe;
        cli_mod_dependency_init(&probe);
        if (!cli_mod_dependency_set(&probe, dep_name, "0.0.0", dep_constraint, source, NULL, NULL, NULL)) {
            fprintf(stderr, "Error: out of memory while resolving dependency version\n");
            cli_mod_dependency_free(&probe);
            free(dep_name);
            free(dep_constraint);
            return 1;
        }
        char resolve_error[256];
        resolve_error[0] = '\0';
        if (!cli_mod_find_highest_registry_version(&probe, dep_constraint, &resolved_version, resolve_error, sizeof(resolve_error))) {
            fprintf(stderr,
                    "Error: %s for dependency '%s' and constraint '%s'\n",
                    resolve_error[0] ? resolve_error : "failed resolving registry version",
                    dep_name,
                    dep_constraint);
            cli_mod_dependency_free(&probe);
            free(dep_name);
            free(dep_constraint);
            return 1;
        }
        cli_mod_dependency_free(&probe);
    } else {
        if (!cli_mod_extract_exact_version_from_constraint(dep_constraint, &resolved_version)) {
            fprintf(stderr, "Error: path dependencies require an exact semver constraint (for example 1.2.3)\n");
            free(dep_name);
            free(dep_constraint);
            return 1;
        }
    }
    if (!resolved_version || !cli_mod_validate_version(resolved_version)) {
        fprintf(stderr, "Error: resolved dependency version is invalid\n");
        free(dep_name);
        free(dep_constraint);
        free(resolved_version);
        return 1;
    }

    CliModManifest manifest;
    cli_mod_manifest_init(&manifest);
    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s. Run 'tablo mod init <module>' first.\n",
                error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        free(dep_name);
        free(dep_constraint);
        free(resolved_version);
        return 1;
    }

    if (!cli_mod_manifest_add_or_replace_dep(&manifest,
                                             dep_name,
                                             resolved_version,
                                             dep_constraint,
                                             source,
                                             NULL,
                                             NULL,
                                             NULL)) {
        fprintf(stderr, "Error: out of memory while adding dependency\n");
        cli_mod_manifest_free(&manifest);
        free(dep_name);
        free(dep_constraint);
        free(resolved_version);
        return 1;
    }
    cli_mod_manifest_sort_deps(&manifest);

    if (!cli_mod_write_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.mod");
        cli_mod_manifest_free(&manifest);
        free(dep_name);
        free(dep_constraint);
        free(resolved_version);
        return 1;
    }
    if (!cli_mod_write_lock_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.lock");
        cli_mod_manifest_free(&manifest);
        free(dep_name);
        free(dep_constraint);
        free(resolved_version);
        return 1;
    }

    printf("Added dependency %s@%s (constraint %s, %s).\n", dep_name, resolved_version, dep_constraint, source);
    printf("Updated %s and %s.\n", CLI_MOD_MANIFEST_FILE, CLI_MOD_LOCK_FILE);

    cli_mod_manifest_free(&manifest);
    free(dep_name);
    free(dep_constraint);
    free(resolved_version);
    return 0;
}

static int cli_mod_cmd_update(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Error: usage: tablo mod update [dependency]\n");
        return 1;
    }

    const char* target_dep = NULL;
    if (argc == 4) {
        target_dep = argv[3];
        if (!cli_mod_validate_dependency_name(target_dep)) {
            fprintf(stderr, "Error: invalid dependency name '%s'\n", target_dep);
            return 1;
        }
    }

    CliModManifest manifest;
    cli_mod_manifest_init(&manifest);
    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    if (target_dep && cli_mod_manifest_find_dep_index(&manifest, target_dep) < 0) {
        fprintf(stderr, "Error: dependency '%s' not found in %s\n", target_dep, CLI_MOD_MANIFEST_FILE);
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    int updated = 0;
    int skipped = 0;
    int selected = 0;
    for (int i = 0; i < manifest.dep_count; i++) {
        CliModDependency* dep = &manifest.deps[i];
        if (target_dep && strcmp(dep->name ? dep->name : "", target_dep) != 0) {
            continue;
        }
        selected++;
        if (!dep->source || !cli_mod_source_is_registry(dep->source)) {
            skipped++;
            continue;
        }

        const char* constraint = cli_mod_dependency_constraint_or_version(dep);
        char* best_version = NULL;
        if (!cli_mod_find_highest_registry_version(dep, constraint, &best_version, error_text, sizeof(error_text))) {
            fprintf(stderr,
                    "Error: %s for dependency '%s' (%s)\n",
                    error_text[0] ? error_text : "failed to resolve registry update",
                    dep->name ? dep->name : "<unknown>",
                    constraint ? constraint : "<none>");
            cli_mod_manifest_free(&manifest);
            return 1;
        }

        if (!dep->version || strcmp(dep->version, best_version) != 0) {
            if (!cli_mod_dependency_set(dep,
                                        dep->name ? dep->name : "",
                                        best_version,
                                        constraint,
                                        dep->source ? dep->source : "",
                                        NULL,
                                        NULL,
                                        NULL)) {
                free(best_version);
                fprintf(stderr, "Error: out of memory while updating dependency '%s'\n", dep->name ? dep->name : "<unknown>");
                cli_mod_manifest_free(&manifest);
                return 1;
            }
            updated++;
        }
        free(best_version);
    }

    cli_mod_manifest_sort_deps(&manifest);
    if (!cli_mod_write_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.mod");
        cli_mod_manifest_free(&manifest);
        return 1;
    }
    if (!cli_mod_write_lock_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.lock");
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    printf("Updated %d dependency version(s), skipped %d non-registry dependency(s), selected %d.\n",
           updated,
           skipped,
           selected);
    printf("Updated %s and %s.\n", CLI_MOD_MANIFEST_FILE, CLI_MOD_LOCK_FILE);
    cli_mod_manifest_free(&manifest);
    return 0;
}

static int cli_mod_cmd_tidy(int argc, char** argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Error: usage: tablo mod tidy\n");
        return 1;
    }

    CliModManifest manifest;
    cli_mod_manifest_init(&manifest);
    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    CliModStringList imports;
    cli_mod_string_list_init(&imports);
    if (!cli_mod_collect_project_imports(&imports, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to scan imports");
        cli_mod_manifest_free(&manifest);
        cli_mod_string_list_free(&imports);
        return 1;
    }

    int removed = 0;
    for (int i = 0; i < manifest.dep_count;) {
        if (cli_mod_dependency_is_used(manifest.deps[i].name, &imports)) {
            i++;
            continue;
        }
        cli_mod_dependency_free(&manifest.deps[i]);
        for (int k = i; k + 1 < manifest.dep_count; k++) {
            manifest.deps[k] = manifest.deps[k + 1];
            cli_mod_dependency_init(&manifest.deps[k + 1]);
        }
        manifest.dep_count--;
        removed++;
    }

    cli_mod_manifest_sort_deps(&manifest);
    if (!cli_mod_write_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.mod");
        cli_mod_manifest_free(&manifest);
        cli_mod_string_list_free(&imports);
        return 1;
    }
    if (!cli_mod_write_lock_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to write tablo.lock");
        cli_mod_manifest_free(&manifest);
        cli_mod_string_list_free(&imports);
        return 1;
    }

    printf("Tidy complete: removed %d unused dependencies, %d remaining.\n", removed, manifest.dep_count);

    cli_mod_manifest_free(&manifest);
    cli_mod_string_list_free(&imports);
    return 0;
}

static int cli_mod_cmd_vendor(int argc, char** argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Error: usage: tablo mod vendor\n");
        return 1;
    }

    CliModManifest lock;
    cli_mod_manifest_init(&lock);
    char error_text[256];
    error_text[0] = '\0';

    if (!cli_mod_load_lock_file(&lock, error_text, sizeof(error_text))) {
        CliModManifest manifest;
        cli_mod_manifest_init(&manifest);
        if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!cli_mod_write_lock_file(&manifest, error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to generate tablo.lock");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        cli_mod_manifest_free(&manifest);
        if (!cli_mod_load_lock_file(&lock, error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to reload tablo.lock");
            cli_mod_manifest_free(&lock);
            return 1;
        }
    }

    if (!cli_mod_mkdir_p(CLI_MOD_VENDOR_DIR)) {
        fprintf(stderr, "Error: failed to create '%s' directory\n", CLI_MOD_VENDOR_DIR);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    int vendored = 0;
    for (int i = 0; i < lock.dep_count; i++) {
        const CliModDependency* dep = &lock.deps[i];
        if (!dep->source || (!cli_mod_source_is_path(dep->source) && !cli_mod_source_is_registry(dep->source))) {
            fprintf(stderr,
                    "Error: dependency '%s' has unsupported source '%s'\n",
                    dep->name ? dep->name : "<unknown>",
                    dep->source ? dep->source : "<none>");
            cli_mod_manifest_free(&lock);
            return 1;
        }

        char* source_path = cli_mod_resolve_dependency_source_path(dep, error_text, sizeof(error_text));
        if (!source_path) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to resolve dependency source");
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!cli_mod_path_exists(source_path)) {
            fprintf(stderr, "Error: source path '%s' does not exist for dependency '%s'\n", source_path, dep->name);
            free(source_path);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        char* target_path = cli_mod_path_join(CLI_MOD_VENDOR_DIR, dep->name);
        if (!target_path) {
            free(source_path);
            fprintf(stderr, "Error: out of memory while preparing vendor destination\n");
            cli_mod_manifest_free(&lock);
            return 1;
        }

        if (cli_mod_path_exists(target_path)) {
            if (!cli_mod_remove_tree(target_path, error_text, sizeof(error_text))) {
                fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to clean existing vendor path");
                free(source_path);
                free(target_path);
                cli_mod_manifest_free(&lock);
                return 1;
            }
        }
        if (!cli_mod_copy_tree(source_path, target_path, error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to vendor dependency");
            free(source_path);
            free(target_path);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        free(source_path);
        free(target_path);
        vendored++;
    }

    printf("Vendored %d dependency(s) into '%s/'.\n", vendored, CLI_MOD_VENDOR_DIR);
    cli_mod_manifest_free(&lock);
    return 0;
}

static int cli_mod_cmd_list(int argc, char** argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Error: usage: tablo mod list\n");
        return 1;
    }

    CliModManifest manifest;
    cli_mod_manifest_init(&manifest);
    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        return 1;
    }

    printf("module %s\n", manifest.module_name ? manifest.module_name : "<unknown>");
    if (manifest.dep_count == 0) {
        printf("(no dependencies)\n");
    } else {
        for (int i = 0; i < manifest.dep_count; i++) {
            CliModDependency* dep = &manifest.deps[i];
            const char* constraint = cli_mod_dependency_constraint_or_version(dep);
            printf("- %s@%s [constraint: %s] (%s)\n",
                   dep->name ? dep->name : "<unknown>",
                   dep->version ? dep->version : "<unknown>",
                   constraint ? constraint : "<unknown>",
                   dep->source ? dep->source : "<unknown>");
        }
    }

    cli_mod_manifest_free(&manifest);
    return 0;
}

static int cli_mod_cmd_fetch(int argc, char** argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Error: usage: tablo mod fetch\n");
        return 1;
    }

    CliModManifest manifest;
    CliModManifest lock;
    cli_mod_manifest_init(&manifest);
    cli_mod_manifest_init(&lock);
    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }
    if (!cli_mod_load_lock_file(&lock, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.lock");
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    if ((!manifest.module_name && lock.module_name) ||
        (manifest.module_name && !lock.module_name) ||
        (manifest.module_name && lock.module_name && strcmp(manifest.module_name, lock.module_name) != 0)) {
        fprintf(stderr, "Error: module name mismatch between %s and %s\n", CLI_MOD_MANIFEST_FILE, CLI_MOD_LOCK_FILE);
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    if (lock.dep_count != manifest.dep_count) {
        fprintf(stderr,
                "Error: dependency count mismatch between %s (%d) and %s (%d)\n",
                CLI_MOD_MANIFEST_FILE,
                manifest.dep_count,
                CLI_MOD_LOCK_FILE,
                lock.dep_count);
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    for (int i = 0; i < manifest.dep_count; i++) {
        const CliModDependency* dep = &manifest.deps[i];
        int idx = cli_mod_manifest_find_dep_index(&lock, dep->name);
        if (idx < 0) {
            fprintf(stderr, "Error: dependency '%s' is missing in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        const CliModDependency* locked = &lock.deps[idx];
        const char* dep_constraint = cli_mod_dependency_constraint_or_version(dep);
        const char* lock_constraint = cli_mod_dependency_constraint_or_version(locked);
        if (!locked->version || strcmp(locked->version, dep->version ? dep->version : "") != 0 ||
            !lock_constraint || strcmp(lock_constraint, dep_constraint ? dep_constraint : "") != 0 ||
            !locked->source || strcmp(locked->source, dep->source ? dep->source : "") != 0) {
            fprintf(stderr, "Error: dependency '%s' metadata mismatch between %s and %s\n",
                    dep->name,
                    CLI_MOD_MANIFEST_FILE,
                    CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
    }

    if (!cli_mod_mkdir_p(CLI_MOD_VENDOR_DIR)) {
        fprintf(stderr, "Error: failed to create '%s' directory\n", CLI_MOD_VENDOR_DIR);
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    int fetched = 0;
    int skipped = 0;
    for (int i = 0; i < manifest.dep_count; i++) {
        const CliModDependency* dep = &manifest.deps[i];
        int idx = cli_mod_manifest_find_dep_index(&lock, dep->name);
        const CliModDependency* locked = (idx >= 0) ? &lock.deps[idx] : NULL;
        if (!locked) {
            fprintf(stderr, "Error: dependency '%s' is missing in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!dep->source || !cli_mod_source_is_registry(dep->source)) {
            skipped++;
            continue;
        }

        char expected_checksum[64];
        expected_checksum[0] = '\0';
        if (!cli_mod_compute_dependency_checksum(dep, expected_checksum, sizeof(expected_checksum), error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to compute dependency checksum");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->checksum || locked->checksum[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty checksum in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (strcmp(expected_checksum, locked->checksum) != 0) {
            fprintf(stderr,
                    "Error: checksum mismatch for '%s': lock=%s expected=%s\n",
                    dep->name ? dep->name : "<unknown>",
                    locked->checksum,
                    expected_checksum);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        char expected_source_hash[64];
        char expected_source_signature[512];
        expected_source_hash[0] = '\0';
        expected_source_signature[0] = '\0';
        if (!cli_mod_compute_dependency_source_provenance(dep,
                                                          expected_checksum,
                                                          expected_source_hash,
                                                          sizeof(expected_source_hash),
                                                          expected_source_signature,
                                                          sizeof(expected_source_signature),
                                                          error_text,
                                                          sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to compute dependency source provenance");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->source_hash || locked->source_hash[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty sourceHash in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->source_signature || locked->source_signature[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty sourceSignature in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (strcmp(expected_source_hash, locked->source_hash) != 0 ||
            strcmp(expected_source_signature, locked->source_signature) != 0) {
            fprintf(stderr,
                    "Error: source provenance mismatch for '%s': lockHash=%s expectedHash=%s lockSignature=%s expectedSignature=%s\n",
                    dep->name ? dep->name : "<unknown>",
                    locked->source_hash,
                    expected_source_hash,
                    locked->source_signature,
                    expected_source_signature);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        char* source_path = cli_mod_resolve_dependency_source_path(dep, error_text, sizeof(error_text));
        if (!source_path) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to resolve registry source");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!cli_mod_path_exists(source_path)) {
            fprintf(stderr,
                    "Error: registry package path '%s' not found for dependency '%s@%s'\n",
                    source_path,
                    dep->name ? dep->name : "<unknown>",
                    dep->version ? dep->version : "<unknown>");
            free(source_path);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        char* target_path = cli_mod_path_join(CLI_MOD_VENDOR_DIR, dep->name ? dep->name : "");
        if (!target_path) {
            free(source_path);
            fprintf(stderr, "Error: out of memory while preparing fetch destination\n");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        if (cli_mod_path_exists(target_path)) {
            if (!cli_mod_remove_tree(target_path, error_text, sizeof(error_text))) {
                fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed cleaning existing vendored package");
                free(source_path);
                free(target_path);
                cli_mod_manifest_free(&manifest);
                cli_mod_manifest_free(&lock);
                return 1;
            }
        }
        if (!cli_mod_copy_tree(source_path, target_path, error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed copying registry package");
            free(source_path);
            free(target_path);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        free(source_path);
        free(target_path);
        fetched++;
    }

    printf("Fetched %d registry dependency(s), skipped %d non-registry dependency(s).\n", fetched, skipped);
    printf("Validated fetched dependencies against %s.\n", CLI_MOD_LOCK_FILE);
    cli_mod_manifest_free(&manifest);
    cli_mod_manifest_free(&lock);
    return 0;
}

static int cli_mod_cmd_verify(int argc, char** argv) {
    (void)argv;
    if (argc != 3) {
        fprintf(stderr, "Error: usage: tablo mod verify\n");
        return 1;
    }

    CliModManifest manifest;
    CliModManifest lock;
    cli_mod_manifest_init(&manifest);
    cli_mod_manifest_init(&lock);

    char error_text[256];
    error_text[0] = '\0';
    if (!cli_mod_load_manifest_file(&manifest, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.mod");
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }
    if (!cli_mod_load_lock_file(&lock, error_text, sizeof(error_text))) {
        fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to load tablo.lock");
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    if ((!manifest.module_name && lock.module_name) ||
        (manifest.module_name && !lock.module_name) ||
        (manifest.module_name && lock.module_name && strcmp(manifest.module_name, lock.module_name) != 0)) {
        fprintf(stderr, "Error: module name mismatch between %s and %s\n", CLI_MOD_MANIFEST_FILE, CLI_MOD_LOCK_FILE);
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    if (lock.dep_count != manifest.dep_count) {
        fprintf(stderr,
                "Error: dependency count mismatch between %s (%d) and %s (%d)\n",
                CLI_MOD_MANIFEST_FILE,
                manifest.dep_count,
                CLI_MOD_LOCK_FILE,
                lock.dep_count);
        cli_mod_manifest_free(&manifest);
        cli_mod_manifest_free(&lock);
        return 1;
    }

    int verified = 0;
    for (int i = 0; i < manifest.dep_count; i++) {
        const CliModDependency* dep = &manifest.deps[i];
        int idx = cli_mod_manifest_find_dep_index(&lock, dep->name);
        if (idx < 0) {
            fprintf(stderr, "Error: dependency '%s' is missing in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        const CliModDependency* locked = &lock.deps[idx];
        const char* dep_constraint = cli_mod_dependency_constraint_or_version(dep);
        const char* lock_constraint = cli_mod_dependency_constraint_or_version(locked);
        if (!locked->version || strcmp(locked->version, dep->version ? dep->version : "") != 0 ||
            !lock_constraint || strcmp(lock_constraint, dep_constraint ? dep_constraint : "") != 0 ||
            !locked->source || strcmp(locked->source, dep->source ? dep->source : "") != 0) {
            fprintf(stderr, "Error: dependency '%s' metadata mismatch between %s and %s\n",
                    dep->name,
                    CLI_MOD_MANIFEST_FILE,
                    CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }

        char expected_checksum[64];
        expected_checksum[0] = '\0';
        if (!cli_mod_compute_dependency_checksum(dep, expected_checksum, sizeof(expected_checksum), error_text, sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to compute dependency checksum");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->checksum || locked->checksum[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty checksum in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (strcmp(expected_checksum, locked->checksum) != 0) {
            fprintf(stderr,
                    "Error: checksum mismatch for '%s': lock=%s expected=%s\n",
                    dep->name,
                    locked->checksum,
                    expected_checksum);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        char expected_source_hash[64];
        char expected_source_signature[512];
        expected_source_hash[0] = '\0';
        expected_source_signature[0] = '\0';
        if (!cli_mod_compute_dependency_source_provenance(dep,
                                                          expected_checksum,
                                                          expected_source_hash,
                                                          sizeof(expected_source_hash),
                                                          expected_source_signature,
                                                          sizeof(expected_source_signature),
                                                          error_text,
                                                          sizeof(error_text))) {
            fprintf(stderr, "Error: %s\n", error_text[0] ? error_text : "failed to compute dependency source provenance");
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->source_hash || locked->source_hash[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty sourceHash in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (!locked->source_signature || locked->source_signature[0] == '\0') {
            fprintf(stderr, "Error: dependency '%s' has empty sourceSignature in %s\n", dep->name, CLI_MOD_LOCK_FILE);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        if (strcmp(expected_source_hash, locked->source_hash) != 0 ||
            strcmp(expected_source_signature, locked->source_signature) != 0) {
            fprintf(stderr,
                    "Error: source provenance mismatch for '%s': lockHash=%s expectedHash=%s lockSignature=%s expectedSignature=%s\n",
                    dep->name,
                    locked->source_hash,
                    expected_source_hash,
                    locked->source_signature,
                    expected_source_signature);
            cli_mod_manifest_free(&manifest);
            cli_mod_manifest_free(&lock);
            return 1;
        }
        verified++;
    }

    printf("Verified %d dependency checksum(s) from %s.\n", verified, CLI_MOD_LOCK_FILE);
    cli_mod_manifest_free(&manifest);
    cli_mod_manifest_free(&lock);
    return 0;
}

static void cli_mod_print_usage(const char* program_name) {
    printf("Usage: %s mod <subcommand> [options]\n", program_name ? program_name : "tablo");
    printf("Subcommands:\n");
    printf("  init [module]                         Create tablo.mod and tablo.lock\n");
    printf("  add <dependency@constraint> [--source] Add or update a dependency\n");
    printf("  update [dependency]                   Refresh resolved registry versions from constraints\n");
    printf("  tidy                                  Remove unused dependencies\n");
    printf("  fetch                                 Materialize lock-verified registry dependencies into vendor/\n");
    printf("  verify                                Verify lock checksums and source provenance\n");
    printf("  vendor                                Vendor path/registry dependencies into vendor/\n");
    printf("  list                                  List module dependencies\n");
    printf("  --help                                Show this help\n");
    printf("\n");
    printf("Source formats:\n");
    printf("  path:<dir>       Local directory source\n");
    printf("  registry:<name>  Registry package source (resolved via %s or %s)\n",
           CLI_MOD_REGISTRY_ENV,
           CLI_MOD_REGISTRY_DEFAULT_ROOT);
    printf("\n");
    printf("Import convention for vendored deps:\n");
    printf("  import \"vendor/<dependency-name>/path/to/module.tblo\";\n");
}

int cli_mod(int argc, char** argv) {
    const char* program_name = (argc > 0 && argv && argv[0]) ? argv[0] : "tablo";
    if (argc < 3) {
        cli_mod_print_usage(program_name);
        return 1;
    }

    const char* sub = argv[2];
    if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0 || strcmp(sub, "help") == 0) {
        cli_mod_print_usage(program_name);
        return 0;
    }
    if (strcmp(sub, "init") == 0) {
        return cli_mod_cmd_init(argc, argv);
    }
    if (strcmp(sub, "add") == 0) {
        return cli_mod_cmd_add(argc, argv);
    }
    if (strcmp(sub, "update") == 0) {
        return cli_mod_cmd_update(argc, argv);
    }
    if (strcmp(sub, "tidy") == 0) {
        return cli_mod_cmd_tidy(argc, argv);
    }
    if (strcmp(sub, "fetch") == 0) {
        return cli_mod_cmd_fetch(argc, argv);
    }
    if (strcmp(sub, "verify") == 0) {
        return cli_mod_cmd_verify(argc, argv);
    }
    if (strcmp(sub, "vendor") == 0) {
        return cli_mod_cmd_vendor(argc, argv);
    }
    if (strcmp(sub, "list") == 0) {
        return cli_mod_cmd_list(argc, argv);
    }

    fprintf(stderr, "Error: unknown mod subcommand '%s'\n", sub);
    cli_mod_print_usage(program_name);
    return 1;
}
