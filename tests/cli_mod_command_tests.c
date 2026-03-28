#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                         \
    do {                                               \
        if (cond) {                                    \
            printf("  PASS: %s\n", msg);               \
            tests_passed++;                            \
        } else {                                       \
            printf("  FAIL: %s\n", msg);               \
            tests_failed++;                            \
        }                                              \
    } while (0)

static int file_exists(const char* path) {
    if (!path || path[0] == '\0') return 0;
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static int is_directory(const char* path) {
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

static int make_dir(const char* path) {
    if (is_directory(path)) return 1;
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

static int write_text_file(const char* path, const char* content) {
    if (!path || !content) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(content);
    size_t wrote = fwrite(content, 1, len, f);
    fclose(f);
    return wrote == len;
}

static int read_text_file(const char* path, char** out_text) {
    if (!path || !out_text) return 0;
    *out_text = NULL;
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
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (got != (size_t)size) {
        free(buf);
        return 0;
    }
    *out_text = buf;
    return 1;
}

static int extract_dep_checksum_from_lock(const char* lock_path,
                                          const char* dep_name,
                                          char* out_checksum,
                                          size_t out_checksum_size) {
    if (!lock_path || !dep_name || !out_checksum || out_checksum_size == 0) return 0;
    char* text = NULL;
    if (!read_text_file(lock_path, &text)) return 0;

    const char* dep_pos = strstr(text, dep_name);
    if (!dep_pos) {
        free(text);
        return 0;
    }
    const char* checksum_key = strstr(dep_pos, "\"checksum\"");
    if (!checksum_key) {
        free(text);
        return 0;
    }
    const char* colon = strchr(checksum_key, ':');
    if (!colon) {
        free(text);
        return 0;
    }
    const char* first_quote = strchr(colon, '"');
    if (!first_quote) {
        free(text);
        return 0;
    }
    first_quote++;
    const char* second_quote = strchr(first_quote, '"');
    if (!second_quote || second_quote <= first_quote) {
        free(text);
        return 0;
    }
    size_t len = (size_t)(second_quote - first_quote);
    if (len + 1 > out_checksum_size) {
        free(text);
        return 0;
    }
    memcpy(out_checksum, first_quote, len);
    out_checksum[len] = '\0';
    free(text);
    return 1;
}

typedef struct {
    unsigned char data[64];
    unsigned int datalen;
    unsigned long long bitlen;
    unsigned int state[8];
} TestSha256Ctx;

static unsigned int test_sha256_rotr(unsigned int value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static unsigned int test_sha256_ch(unsigned int x, unsigned int y, unsigned int z) {
    return (x & y) ^ (~x & z);
}

static unsigned int test_sha256_maj(unsigned int x, unsigned int y, unsigned int z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static unsigned int test_sha256_sigma0(unsigned int x) {
    return test_sha256_rotr(x, 2) ^ test_sha256_rotr(x, 13) ^ test_sha256_rotr(x, 22);
}

static unsigned int test_sha256_sigma1(unsigned int x) {
    return test_sha256_rotr(x, 6) ^ test_sha256_rotr(x, 11) ^ test_sha256_rotr(x, 25);
}

static unsigned int test_sha256_gamma0(unsigned int x) {
    return test_sha256_rotr(x, 7) ^ test_sha256_rotr(x, 18) ^ (x >> 3);
}

static unsigned int test_sha256_gamma1(unsigned int x) {
    return test_sha256_rotr(x, 17) ^ test_sha256_rotr(x, 19) ^ (x >> 10);
}

static void test_sha256_transform(TestSha256Ctx* ctx, const unsigned char data[64]) {
    static const unsigned int k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    unsigned int m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((unsigned int)data[i * 4] << 24) |
               ((unsigned int)data[i * 4 + 1] << 16) |
               ((unsigned int)data[i * 4 + 2] << 8) |
               ((unsigned int)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = test_sha256_gamma1(m[i - 2]) + m[i - 7] + test_sha256_gamma0(m[i - 15]) + m[i - 16];
    }

    unsigned int a = ctx->state[0];
    unsigned int b = ctx->state[1];
    unsigned int c = ctx->state[2];
    unsigned int d = ctx->state[3];
    unsigned int e = ctx->state[4];
    unsigned int f = ctx->state[5];
    unsigned int g = ctx->state[6];
    unsigned int h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        unsigned int t1 = h + test_sha256_sigma1(e) + test_sha256_ch(e, f, g) + k[i] + m[i];
        unsigned int t2 = test_sha256_sigma0(a) + test_sha256_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void test_sha256_init(TestSha256Ctx* ctx) {
    if (!ctx) return;
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void test_sha256_update(TestSha256Ctx* ctx, const unsigned char* data, size_t len) {
    if (!ctx || (!data && len != 0)) return;
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            test_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void test_sha256_final(TestSha256Ctx* ctx, unsigned char out[32]) {
    unsigned int i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        test_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (unsigned long long)ctx->datalen * 8ull;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    test_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        out[i] = (unsigned char)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        out[i + 4] = (unsigned char)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        out[i + 8] = (unsigned char)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        out[i + 12] = (unsigned char)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        out[i + 16] = (unsigned char)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        out[i + 20] = (unsigned char)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        out[i + 24] = (unsigned char)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        out[i + 28] = (unsigned char)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

static void test_hmac_sha256(const unsigned char* key,
                             size_t key_len,
                             const unsigned char* data,
                             size_t data_len,
                             unsigned char out[32]) {
    unsigned char key_block[64];
    memset(key_block, 0, sizeof(key_block));
    if (key_len > 64) {
        TestSha256Ctx key_ctx;
        unsigned char key_hash[32];
        test_sha256_init(&key_ctx);
        test_sha256_update(&key_ctx, key, key_len);
        test_sha256_final(&key_ctx, key_hash);
        memcpy(key_block, key_hash, sizeof(key_hash));
    } else if (key && key_len > 0) {
        memcpy(key_block, key, key_len);
    }

    unsigned char ipad[64];
    unsigned char opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(key_block[i] ^ 0x36u);
        opad[i] = (unsigned char)(key_block[i] ^ 0x5cu);
    }

    TestSha256Ctx inner_ctx;
    unsigned char inner_hash[32];
    test_sha256_init(&inner_ctx);
    test_sha256_update(&inner_ctx, ipad, sizeof(ipad));
    test_sha256_update(&inner_ctx, data, data_len);
    test_sha256_final(&inner_ctx, inner_hash);

    TestSha256Ctx outer_ctx;
    test_sha256_init(&outer_ctx);
    test_sha256_update(&outer_ctx, opad, sizeof(opad));
    test_sha256_update(&outer_ctx, inner_hash, sizeof(inner_hash));
    test_sha256_final(&outer_ctx, out);
}

static void test_hex_encode_lower(const unsigned char* bytes, size_t len, char* out, size_t out_size) {
    static const char table[] = "0123456789abcdef";
    if (!bytes || !out || out_size < (len * 2 + 1)) return;
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = table[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = table[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int compute_dep_signature_hex(const char* dep_name,
                                     const char* dep_version,
                                     const char* dep_constraint,
                                     const char* dep_source,
                                     const char* dep_checksum,
                                     const char* key_secret,
                                     char* out_hex,
                                     size_t out_hex_size) {
    if (!dep_name || !dep_version || !dep_constraint || !dep_source || !dep_checksum || !key_secret || !out_hex || out_hex_size < 65) {
        return 0;
    }
    char payload[4096];
    int wrote = snprintf(payload,
                         sizeof(payload),
                         "tablo-mod-signature-v1\nname=%s\nversion=%s\nconstraint=%s\nsource=%s\nchecksum=%s\n",
                         dep_name,
                         dep_version,
                         dep_constraint,
                         dep_source,
                         dep_checksum);
    if (wrote < 0 || wrote >= (int)sizeof(payload)) {
        return 0;
    }

    unsigned char mac[32];
    test_hmac_sha256((const unsigned char*)key_secret,
                     strlen(key_secret),
                     (const unsigned char*)payload,
                     strlen(payload),
                     mac);
    test_hex_encode_lower(mac, sizeof(mac), out_hex, out_hex_size);
    return 1;
}

static int write_dep_signature_file(const char* dep_dir, const char* key_id, const char* signature_hex) {
    if (!dep_dir || !key_id || !signature_hex) return 0;
#ifdef _WIN32
    char sig_file[1024];
    snprintf(sig_file, sizeof(sig_file), "%s\\.tablo.sig", dep_dir);
#else
    char sig_file[1024];
    snprintf(sig_file, sizeof(sig_file), "%s/.tablo.sig", dep_dir);
#endif
    char json[2048];
    snprintf(json,
             sizeof(json),
             "{\n"
             "  \"keyId\": \"%s\",\n"
             "  \"algorithm\": \"hmac-sha256\",\n"
             "  \"signature\": \"%s\"\n"
             "}\n",
             key_id,
             signature_hex);
    return write_text_file(sig_file, json);
}

static int file_contains_text(const char* path, const char* needle) {
    if (!path || !needle) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    int found = strstr(buf, needle) != NULL;
    free(buf);
    return found;
}

static int file_not_contains_text(const char* path, const char* needle) {
    return !file_contains_text(path, needle);
}

static int decode_system_exit_code(int status) {
#ifdef _WIN32
    return status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

static int build_sibling_executable_path(const char* filename, char* out, size_t out_size) {
    char exe_path[1024];
    char* last_slash = NULL;
    char* last_backslash = NULL;
    char* last_sep = NULL;
    int wrote = 0;

    if (!filename || !out || out_size == 0) return 0;

#ifdef _WIN32
    DWORD got = GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path));
    if (got == 0 || got >= (DWORD)sizeof(exe_path)) return 0;
#elif defined(__APPLE__)
    uint32_t path_size = (uint32_t)sizeof(exe_path);
    char resolved[1024];
    if (_NSGetExecutablePath(exe_path, &path_size) != 0) return 0;
    if (!realpath(exe_path, resolved)) return 0;
    memcpy(exe_path, resolved, sizeof(resolved));
#else
    ssize_t got = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (got <= 0 || got >= (ssize_t)sizeof(exe_path)) return 0;
    exe_path[got] = '\0';
#endif

    last_slash = strrchr(exe_path, '/');
    last_backslash = strrchr(exe_path, '\\');
    last_sep = last_slash;
    if (!last_sep || (last_backslash && last_backslash > last_sep)) {
        last_sep = last_backslash;
    }
    if (!last_sep) return 0;
    *last_sep = '\0';

#ifdef _WIN32
    wrote = snprintf(out, out_size, "%s\\%s", exe_path, filename);
#else
    wrote = snprintf(out, out_size, "%s/%s", exe_path, filename);
#endif
    return wrote > 0 && (size_t)wrote < out_size;
}

static const char* find_tablo_executable(void) {
    static char resolved[1024];
    static int initialized = 0;

    if (initialized) {
        return resolved[0] != '\0' ? resolved : NULL;
    }
    initialized = 1;
    resolved[0] = '\0';

#ifdef _WIN32
    if (build_sibling_executable_path("tablo.exe", resolved, sizeof(resolved)) &&
        file_exists(resolved)) {
        return resolved;
    }
    static const char* candidates[] = {
        "..\\build-tablo\\Debug\\tablo.exe",
        "..\\build-tablo\\Release\\tablo.exe",
        "..\\build-tablo\\tablo.exe",
        "..\\build-wsl-ci\\Debug\\tablo.exe",
        "..\\build-wsl-ci\\Release\\tablo.exe",
        "..\\build-wsl-ci\\tablo.exe",
        "..\\build\\Debug\\tablo.exe",
        "..\\build\\Release\\tablo.exe",
        "..\\build\\tablo.exe",
        "../build-tablo/Debug/tablo.exe",
        "../build-tablo/Release/tablo.exe",
        "../build-tablo/tablo.exe",
        "../build-wsl-ci/Debug/tablo.exe",
        "../build-wsl-ci/Release/tablo.exe",
        "../build-wsl-ci/tablo.exe",
        "../build/Debug/tablo.exe",
        "../build/Release/tablo.exe",
        "../build/tablo.exe"
    };
#else
    if (build_sibling_executable_path("tablo", resolved, sizeof(resolved)) &&
        file_exists(resolved)) {
        return resolved;
    }
    static const char* candidates[] = {
        "../build-tablo/Debug/tablo",
        "../build-tablo/Release/tablo",
        "../build-tablo/tablo",
        "../build-wsl-ci/Debug/tablo",
        "../build-wsl-ci/Release/tablo",
        "../build-wsl-ci/tablo",
        "../build/tablo",
        "../build/Debug/tablo",
        "../build/Release/tablo"
    };
#endif

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (file_exists(candidates[i])) {
            strncpy(resolved, candidates[i], sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
            return resolved;
        }
    }
    return NULL;
}

static int to_absolute_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return 0;
#ifdef _WIN32
    return _fullpath(out, path, out_size) != NULL;
#else
    return realpath(path, out) != NULL;
#endif
}

static int run_cli_command_in_dir_with_env(const char* exe,
                                           const char* workdir,
                                           const char* env_assignment,
                                           const char* args,
                                           int expect_success,
                                           const char* label);

static int run_cli_command_in_dir(const char* exe,
                                  const char* workdir,
                                  const char* args,
                                  int expect_success,
                                  const char* label) {
    return run_cli_command_in_dir_with_env(exe, workdir, NULL, args, expect_success, label);
}

static int run_cli_command_in_dir_with_env(const char* exe,
                                           const char* workdir,
                                           const char* env_assignment,
                                           const char* args,
                                           int expect_success,
                                           const char* label) {
    char cmd[4096];
#ifdef _WIN32
    if (env_assignment && env_assignment[0] != '\0') {
        snprintf(cmd,
                 sizeof(cmd),
                 "cmd /c \"cd /d \"\"%s\"\" && set \"%s\" && \"\"%s\"\" %s > NUL 2>&1\"",
                 workdir,
                 env_assignment,
                 exe,
                 args);
    } else {
        snprintf(cmd,
                 sizeof(cmd),
                 "cmd /c \"cd /d \"\"%s\"\" && \"\"%s\"\" %s > NUL 2>&1\"",
                 workdir,
                 exe,
                 args);
    }
#else
    if (env_assignment && env_assignment[0] != '\0') {
        snprintf(cmd,
                 sizeof(cmd),
                 "sh -c 'cd \"%s\" && %s \"%s\" %s > /dev/null 2>&1'",
                 workdir,
                 env_assignment,
                 exe,
                 args);
    } else {
        snprintf(cmd,
                 sizeof(cmd),
                 "sh -c 'cd \"%s\" && \"%s\" %s > /dev/null 2>&1'",
                 workdir,
                 exe,
                 args);
    }
#endif

    int raw = system(cmd);
    int code = decode_system_exit_code(raw);
    int ok = expect_success ? (code == 0) : (code != 0);
    TEST_ASSERT(ok, label);
    if (!ok) {
        printf("    command: %s\n", cmd);
        printf("    exit code: %d\n", code);
    }
    return ok;
}

static int remove_tree(const char* path) {
    if (!path || path[0] == '\0') return 1;
    if (!file_exists(path)) return 1;

#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 1;
    if ((st.st_mode & _S_IFDIR) == 0) {
        return remove(path) == 0;
    }

    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    struct _finddata64i32_t fd;
    intptr_t h = _findfirst64i32(pattern, &fd);
    if (h != -1) {
        do {
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s\\%s", path, fd.name);
            if (!remove_tree(child)) {
                _findclose(h);
                return 0;
            }
        } while (_findnext64i32(h, &fd) == 0);
        _findclose(h);
    }
    return _rmdir(path) == 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    if (!S_ISDIR(st.st_mode)) {
        return remove(path) == 0;
    }
    DIR* dir = opendir(path);
    if (!dir) return 0;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (!remove_tree(child)) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return rmdir(path) == 0;
#endif
}

static void test_cli_mod_command(void) {
    printf("Testing CLI 'mod' command...\n");

    const char* exe = find_tablo_executable();
    TEST_ASSERT(exe != NULL, "Locate tablo executable");
    if (!exe) return;
    char exe_abs[1024];
    TEST_ASSERT(to_absolute_path(exe, exe_abs, sizeof(exe_abs)), "Resolve absolute path to tablo executable");
    if (!to_absolute_path(exe, exe_abs, sizeof(exe_abs))) return;

    srand((unsigned int)time(NULL));
    char root[512];
    snprintf(root, sizeof(root), ".tmp_cli_mod_%u", (unsigned)rand());
    TEST_ASSERT(make_dir(root), "Create temp root directory");
    if (!is_directory(root)) return;

#ifdef _WIN32
    char app_dir[512];
    char dep_dir[512];
    char registry_root[512];
    char registry_mirror_root[512];
    char registry_pkg_dir[512];
    char registry_mirror_pkg_dir[512];
    char registry_pkg_ver_010_dir[512];
    char registry_pkg_ver_015_dir[512];
    char registry_pkg_ver_020_dir[512];
    char registry_pkg_ver_019_dir[512];
    char registry_mirror_pkg_ver_010_dir[512];
    char registry_mirror_pkg_ver_015_dir[512];
    char registry_mirror_pkg_ver_020_dir[512];
    char registry_mirror_pkg_ver_019_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s\\app", root);
    snprintf(dep_dir, sizeof(dep_dir), "%s\\dep_pkg", root);
    snprintf(registry_root, sizeof(registry_root), "%s\\registry", root);
    snprintf(registry_mirror_root, sizeof(registry_mirror_root), "%s\\registry_mirror", root);
    snprintf(registry_pkg_dir, sizeof(registry_pkg_dir), "%s\\acme\\clock", registry_root);
    snprintf(registry_mirror_pkg_dir, sizeof(registry_mirror_pkg_dir), "%s\\acme\\clock", registry_mirror_root);
    snprintf(registry_pkg_ver_010_dir, sizeof(registry_pkg_ver_010_dir), "%s\\0.1.0", registry_pkg_dir);
    snprintf(registry_pkg_ver_015_dir, sizeof(registry_pkg_ver_015_dir), "%s\\0.1.5", registry_pkg_dir);
    snprintf(registry_pkg_ver_020_dir, sizeof(registry_pkg_ver_020_dir), "%s\\0.2.0", registry_pkg_dir);
    snprintf(registry_pkg_ver_019_dir, sizeof(registry_pkg_ver_019_dir), "%s\\0.1.9", registry_pkg_dir);
    snprintf(registry_mirror_pkg_ver_010_dir, sizeof(registry_mirror_pkg_ver_010_dir), "%s\\0.1.0", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_015_dir, sizeof(registry_mirror_pkg_ver_015_dir), "%s\\0.1.5", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_020_dir, sizeof(registry_mirror_pkg_ver_020_dir), "%s\\0.2.0", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_019_dir, sizeof(registry_mirror_pkg_ver_019_dir), "%s\\0.1.9", registry_mirror_pkg_dir);
#else
    char app_dir[512];
    char dep_dir[512];
    char registry_root[512];
    char registry_mirror_root[512];
    char registry_pkg_dir[512];
    char registry_mirror_pkg_dir[512];
    char registry_pkg_ver_010_dir[512];
    char registry_pkg_ver_015_dir[512];
    char registry_pkg_ver_020_dir[512];
    char registry_pkg_ver_019_dir[512];
    char registry_mirror_pkg_ver_010_dir[512];
    char registry_mirror_pkg_ver_015_dir[512];
    char registry_mirror_pkg_ver_020_dir[512];
    char registry_mirror_pkg_ver_019_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/app", root);
    snprintf(dep_dir, sizeof(dep_dir), "%s/dep_pkg", root);
    snprintf(registry_root, sizeof(registry_root), "%s/registry", root);
    snprintf(registry_mirror_root, sizeof(registry_mirror_root), "%s/registry_mirror", root);
    snprintf(registry_pkg_dir, sizeof(registry_pkg_dir), "%s/acme/clock", registry_root);
    snprintf(registry_mirror_pkg_dir, sizeof(registry_mirror_pkg_dir), "%s/acme/clock", registry_mirror_root);
    snprintf(registry_pkg_ver_010_dir, sizeof(registry_pkg_ver_010_dir), "%s/0.1.0", registry_pkg_dir);
    snprintf(registry_pkg_ver_015_dir, sizeof(registry_pkg_ver_015_dir), "%s/0.1.5", registry_pkg_dir);
    snprintf(registry_pkg_ver_020_dir, sizeof(registry_pkg_ver_020_dir), "%s/0.2.0", registry_pkg_dir);
    snprintf(registry_pkg_ver_019_dir, sizeof(registry_pkg_ver_019_dir), "%s/0.1.9", registry_pkg_dir);
    snprintf(registry_mirror_pkg_ver_010_dir, sizeof(registry_mirror_pkg_ver_010_dir), "%s/0.1.0", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_015_dir, sizeof(registry_mirror_pkg_ver_015_dir), "%s/0.1.5", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_020_dir, sizeof(registry_mirror_pkg_ver_020_dir), "%s/0.2.0", registry_mirror_pkg_dir);
    snprintf(registry_mirror_pkg_ver_019_dir, sizeof(registry_mirror_pkg_ver_019_dir), "%s/0.1.9", registry_mirror_pkg_dir);
#endif
    TEST_ASSERT(make_dir(app_dir), "Create app directory");
    TEST_ASSERT(make_dir(dep_dir), "Create dependency directory");
    TEST_ASSERT(make_dir(registry_root), "Create registry root directory");
    TEST_ASSERT(make_dir(registry_mirror_root), "Create mirror registry root directory");
#ifdef _WIN32
    {
        char registry_acme[512];
        char registry_mirror_acme[512];
        snprintf(registry_acme, sizeof(registry_acme), "%s\\acme", registry_root);
        snprintf(registry_mirror_acme, sizeof(registry_mirror_acme), "%s\\acme", registry_mirror_root);
        TEST_ASSERT(make_dir(registry_acme), "Create registry namespace directory");
        TEST_ASSERT(make_dir(registry_mirror_acme), "Create mirror registry namespace directory");
    }
#else
    {
        char registry_acme[512];
        char registry_mirror_acme[512];
        snprintf(registry_acme, sizeof(registry_acme), "%s/acme", registry_root);
        snprintf(registry_mirror_acme, sizeof(registry_mirror_acme), "%s/acme", registry_mirror_root);
        TEST_ASSERT(make_dir(registry_acme), "Create registry namespace directory");
        TEST_ASSERT(make_dir(registry_mirror_acme), "Create mirror registry namespace directory");
    }
#endif
    TEST_ASSERT(make_dir(registry_pkg_dir), "Create registry package directory");
    TEST_ASSERT(make_dir(registry_mirror_pkg_dir), "Create mirror registry package directory");
    TEST_ASSERT(make_dir(registry_pkg_ver_010_dir), "Create registry package version 0.1.0 directory");
    TEST_ASSERT(make_dir(registry_pkg_ver_015_dir), "Create registry package version 0.1.5 directory");
    TEST_ASSERT(make_dir(registry_pkg_ver_020_dir), "Create registry package version 0.2.0 directory");
    TEST_ASSERT(make_dir(registry_mirror_pkg_ver_010_dir), "Create mirror registry package version 0.1.0 directory");
    TEST_ASSERT(make_dir(registry_mirror_pkg_ver_015_dir), "Create mirror registry package version 0.1.5 directory");
    TEST_ASSERT(make_dir(registry_mirror_pkg_ver_020_dir), "Create mirror registry package version 0.2.0 directory");

#ifdef _WIN32
    char dep_file[512];
    char registry_pkg_file_010[512];
    char registry_pkg_file_015[512];
    char registry_pkg_file_020[512];
    char registry_pkg_file_019[512];
    char registry_mirror_pkg_file_010[512];
    char registry_mirror_pkg_file_015[512];
    char registry_mirror_pkg_file_020[512];
    char registry_mirror_pkg_file_019[512];
    snprintf(dep_file, sizeof(dep_file), "%s\\util.tblo", dep_dir);
    snprintf(registry_pkg_file_010, sizeof(registry_pkg_file_010), "%s\\clock.tblo", registry_pkg_ver_010_dir);
    snprintf(registry_pkg_file_015, sizeof(registry_pkg_file_015), "%s\\clock.tblo", registry_pkg_ver_015_dir);
    snprintf(registry_pkg_file_020, sizeof(registry_pkg_file_020), "%s\\clock.tblo", registry_pkg_ver_020_dir);
    snprintf(registry_pkg_file_019, sizeof(registry_pkg_file_019), "%s\\clock.tblo", registry_pkg_ver_019_dir);
    snprintf(registry_mirror_pkg_file_010, sizeof(registry_mirror_pkg_file_010), "%s\\clock.tblo", registry_mirror_pkg_ver_010_dir);
    snprintf(registry_mirror_pkg_file_015, sizeof(registry_mirror_pkg_file_015), "%s\\clock.tblo", registry_mirror_pkg_ver_015_dir);
    snprintf(registry_mirror_pkg_file_020, sizeof(registry_mirror_pkg_file_020), "%s\\clock.tblo", registry_mirror_pkg_ver_020_dir);
    snprintf(registry_mirror_pkg_file_019, sizeof(registry_mirror_pkg_file_019), "%s\\clock.tblo", registry_mirror_pkg_ver_019_dir);
#else
    char dep_file[512];
    char registry_pkg_file_010[512];
    char registry_pkg_file_015[512];
    char registry_pkg_file_020[512];
    char registry_pkg_file_019[512];
    char registry_mirror_pkg_file_010[512];
    char registry_mirror_pkg_file_015[512];
    char registry_mirror_pkg_file_020[512];
    char registry_mirror_pkg_file_019[512];
    snprintf(dep_file, sizeof(dep_file), "%s/util.tblo", dep_dir);
    snprintf(registry_pkg_file_010, sizeof(registry_pkg_file_010), "%s/clock.tblo", registry_pkg_ver_010_dir);
    snprintf(registry_pkg_file_015, sizeof(registry_pkg_file_015), "%s/clock.tblo", registry_pkg_ver_015_dir);
    snprintf(registry_pkg_file_020, sizeof(registry_pkg_file_020), "%s/clock.tblo", registry_pkg_ver_020_dir);
    snprintf(registry_pkg_file_019, sizeof(registry_pkg_file_019), "%s/clock.tblo", registry_pkg_ver_019_dir);
    snprintf(registry_mirror_pkg_file_010, sizeof(registry_mirror_pkg_file_010), "%s/clock.tblo", registry_mirror_pkg_ver_010_dir);
    snprintf(registry_mirror_pkg_file_015, sizeof(registry_mirror_pkg_file_015), "%s/clock.tblo", registry_mirror_pkg_ver_015_dir);
    snprintf(registry_mirror_pkg_file_020, sizeof(registry_mirror_pkg_file_020), "%s/clock.tblo", registry_mirror_pkg_ver_020_dir);
    snprintf(registry_mirror_pkg_file_019, sizeof(registry_mirror_pkg_file_019), "%s/clock.tblo", registry_mirror_pkg_ver_019_dir);
#endif
    TEST_ASSERT(write_text_file(dep_file,
                                "func utilValue(): int {\n"
                                "    return 7;\n"
                                "}\n"),
                "Create dependency source file");
    TEST_ASSERT(write_text_file(registry_pkg_file_010,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-01T00:00:00Z\";\n"
                                "}\n"),
                "Create registry package source file (0.1.0)");
    TEST_ASSERT(write_text_file(registry_pkg_file_015,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-05T00:00:00Z\";\n"
                                "}\n"),
                "Create registry package source file (0.1.5)");
    TEST_ASSERT(write_text_file(registry_pkg_file_020,
                                "func clockNow(): string {\n"
                                "    return \"2026-02-01T00:00:00Z\";\n"
                                "}\n"),
                "Create registry package source file (0.2.0)");
    TEST_ASSERT(write_text_file(registry_mirror_pkg_file_010,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-01T00:00:00Z\";\n"
                                "}\n"),
                "Create mirror registry package source file (0.1.0)");
    TEST_ASSERT(write_text_file(registry_mirror_pkg_file_015,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-05T00:00:00Z\";\n"
                                "}\n"),
                "Create mirror registry package source file (0.1.5)");
    TEST_ASSERT(write_text_file(registry_mirror_pkg_file_020,
                                "func clockNow(): string {\n"
                                "    return \"2026-02-01T00:00:00Z\";\n"
                                "}\n"),
                "Create mirror registry package source file (0.2.0)");

    run_cli_command_in_dir(exe_abs, app_dir, "mod init example/app", 1, "tablo mod init succeeds");

#ifdef _WIN32
    char mod_file[512];
    char lock_file[512];
    char keys_file[512];
    char registry_env[512];
    char registry_mirror_env[512];
    snprintf(mod_file, sizeof(mod_file), "%s\\tablo.mod", app_dir);
    snprintf(lock_file, sizeof(lock_file), "%s\\tablo.lock", app_dir);
    snprintf(keys_file, sizeof(keys_file), "%s\\tablo.keys", app_dir);
    snprintf(registry_env, sizeof(registry_env), "TABLO_REGISTRY_ROOT=..\\registry");
    snprintf(registry_mirror_env, sizeof(registry_mirror_env), "TABLO_REGISTRY_ROOT=..\\registry_mirror");
#else
    char mod_file[512];
    char lock_file[512];
    char keys_file[512];
    char registry_env[512];
    char registry_mirror_env[512];
    snprintf(mod_file, sizeof(mod_file), "%s/tablo.mod", app_dir);
    snprintf(lock_file, sizeof(lock_file), "%s/tablo.lock", app_dir);
    snprintf(keys_file, sizeof(keys_file), "%s/tablo.keys", app_dir);
    snprintf(registry_env, sizeof(registry_env), "TABLO_REGISTRY_ROOT=../registry");
    snprintf(registry_mirror_env, sizeof(registry_mirror_env), "TABLO_REGISTRY_ROOT=../registry_mirror");
#endif
    TEST_ASSERT(file_exists(mod_file), "tablo.mod is created");
    TEST_ASSERT(file_exists(lock_file), "tablo.lock is created");

    run_cli_command_in_dir(exe_abs,
                           app_dir,
                           "mod add acme/util@1.2.3 --source path:../dep_pkg",
                           1,
                           "tablo mod add path dependency succeeds");
    TEST_ASSERT(file_contains_text(mod_file, "\"name\":\t\"acme/util\"") || file_contains_text(mod_file, "\"name\": \"acme/util\""),
                "tablo.mod contains dependency name");
    TEST_ASSERT(file_contains_text(mod_file, "path:../dep_pkg"), "tablo.mod contains dependency source");

#ifdef _WIN32
    char main_file[512];
    snprintf(main_file, sizeof(main_file), "%s\\main.tblo", app_dir);
#else
    char main_file[512];
    snprintf(main_file, sizeof(main_file), "%s/main.tblo", app_dir);
#endif
    TEST_ASSERT(write_text_file(main_file,
                                "import \"vendor/acme/util/util.tblo\";\n"
                                "func main(): void {\n"
                                "    println(utilValue());\n"
                                "}\n"),
                "Create main.tblo with vendored import");

    run_cli_command_in_dir(exe_abs, app_dir, "mod vendor", 1, "tablo mod vendor succeeds");

#ifdef _WIN32
    char vendored_file[512];
    snprintf(vendored_file, sizeof(vendored_file), "%s\\vendor\\acme\\util\\util.tblo", app_dir);
#else
    char vendored_file[512];
    snprintf(vendored_file, sizeof(vendored_file), "%s/vendor/acme/util/util.tblo", app_dir);
#endif
    TEST_ASSERT(file_exists(vendored_file), "Vendor output contains copied dependency file");

#ifdef _WIN32
    const char* add_registry_constraint_cmd = "mod add acme/clock@^^0.1.0 --source registry:acme/clock";
#else
    const char* add_registry_constraint_cmd = "mod add acme/clock@^0.1.0 --source registry:acme/clock";
#endif
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    add_registry_constraint_cmd,
                                    1,
                                    "tablo mod add registry dependency with constraint succeeds");
    TEST_ASSERT(file_contains_text(mod_file, "\"acme/clock\""), "tablo.mod contains registry dependency");
    TEST_ASSERT(file_contains_text(mod_file, "\"constraint\":\t\"^0.1.0\"") || file_contains_text(mod_file, "\"constraint\": \"^0.1.0\""),
                "tablo.mod contains registry dependency constraint");
    TEST_ASSERT(file_contains_text(mod_file, "\"version\":\t\"0.1.5\"") || file_contains_text(mod_file, "\"version\": \"0.1.5\""),
                "tablo.mod resolves registry dependency to highest matching version");
    TEST_ASSERT(file_contains_text(lock_file, "\"sourceHash\""), "tablo.lock contains sourceHash provenance");
    TEST_ASSERT(file_contains_text(lock_file, "\"sourceSignature\""), "tablo.lock contains sourceSignature provenance");

    const char* signing_key_id = "acme-test-key";
    const char* signing_secret = "acme-test-secret-2026";
    char checksum_015[128];
    char sig_hex_015[128];
    memset(checksum_015, 0, sizeof(checksum_015));
    memset(sig_hex_015, 0, sizeof(sig_hex_015));
    TEST_ASSERT(extract_dep_checksum_from_lock(lock_file, "acme/clock", checksum_015, sizeof(checksum_015)),
                "Extract lock checksum for acme/clock@0.1.5");
    TEST_ASSERT(compute_dep_signature_hex("acme/clock",
                                          "0.1.5",
                                          "^0.1.0",
                                          "registry:acme/clock",
                                          checksum_015,
                                          signing_secret,
                                          sig_hex_015,
                                          sizeof(sig_hex_015)),
                "Compute detached signature for acme/clock@0.1.5");
    {
        char keys_json[2048];
        snprintf(keys_json,
                 sizeof(keys_json),
                 "{\n"
                 "  \"schemaVersion\": 1,\n"
                 "  \"keys\": [\n"
                 "    {\"id\": \"%s\", \"algorithm\": \"hmac-sha256\", \"secret\": \"%s\"}\n"
                 "  ]\n"
                 "}\n",
                 signing_key_id,
                 signing_secret);
        TEST_ASSERT(write_text_file(keys_file, keys_json), "Write trusted keyring file");
    }
    TEST_ASSERT(write_dep_signature_file(registry_pkg_ver_015_dir, signing_key_id, sig_hex_015),
                "Write detached signature for registry package 0.1.5");
    TEST_ASSERT(write_dep_signature_file(registry_mirror_pkg_ver_015_dir, signing_key_id, sig_hex_015),
                "Write detached signature for mirror package 0.1.5");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod update acme/clock",
                                    1,
                                    "tablo mod update refreshes signed provenance for 0.1.5");
    TEST_ASSERT(file_contains_text(lock_file, "sig:hmac-sha256"),
                "tablo.lock stores canonical signed sourceSignature");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod verify",
                                    1,
                                    "tablo mod verify accepts valid detached signature");
    TEST_ASSERT(write_dep_signature_file(registry_pkg_ver_015_dir,
                                         signing_key_id,
                                         "0000000000000000000000000000000000000000000000000000000000000000"),
                "Corrupt detached signature for registry package 0.1.5");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod verify",
                                    0,
                                    "tablo mod verify rejects invalid detached signature");
    TEST_ASSERT(write_dep_signature_file(registry_pkg_ver_015_dir, signing_key_id, sig_hex_015),
                "Restore detached signature for registry package 0.1.5");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod verify",
                                    1,
                                    "tablo mod verify succeeds after signature restore");

    TEST_ASSERT(write_text_file(main_file,
                                "import \"vendor/acme/util/util.tblo\";\n"
                                "import \"vendor/acme/clock/clock.tblo\";\n"
                                "func main(): void {\n"
                                "    println(utilValue());\n"
                                "    println(clockNow());\n"
                                "}\n"),
                "Rewrite main.tblo with path and registry imports");

#ifdef _WIN32
    char vendored_clock_file[512];
    snprintf(vendored_clock_file, sizeof(vendored_clock_file), "%s\\vendor\\acme\\clock\\clock.tblo", app_dir);
#else
    char vendored_clock_file[512];
    snprintf(vendored_clock_file, sizeof(vendored_clock_file), "%s/vendor/acme/clock/clock.tblo", app_dir);
#endif

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod fetch",
                                    1,
                                    "tablo mod fetch materializes registry dependency");
    TEST_ASSERT(file_exists(vendored_clock_file), "Vendor output contains fetched registry dependency file");
    TEST_ASSERT(file_contains_text(vendored_clock_file, "2026-01-05T00:00:00Z"),
                "Fetched registry dependency matches resolved version 0.1.5");

    TEST_ASSERT(make_dir(registry_pkg_ver_019_dir), "Create registry package version 0.1.9 directory");
    TEST_ASSERT(write_text_file(registry_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-09T00:00:00Z\";\n"
                                "}\n"),
                "Create registry package source file (0.1.9)");
    TEST_ASSERT(make_dir(registry_mirror_pkg_ver_019_dir), "Create mirror registry package version 0.1.9 directory");
    TEST_ASSERT(write_text_file(registry_mirror_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-09T00:00:00Z\";\n"
                                "}\n"),
                "Create mirror registry package source file (0.1.9)");

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod update acme/clock",
                                    1,
                                    "tablo mod update resolves newer matching registry version");
    TEST_ASSERT(file_contains_text(mod_file, "\"version\":\t\"0.1.9\"") || file_contains_text(mod_file, "\"version\": \"0.1.9\""),
                "tablo.mod updates registry dependency to 0.1.9");
    TEST_ASSERT(file_contains_text(mod_file, "\"constraint\":\t\"^0.1.0\"") || file_contains_text(mod_file, "\"constraint\": \"^0.1.0\""),
                "tablo.mod preserves registry dependency constraint after update");

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod update missing/dep",
                                    0,
                                    "tablo mod update rejects unknown dependency");

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod fetch",
                                    1,
                                    "tablo mod fetch refreshes vendor after update");
    TEST_ASSERT(file_contains_text(vendored_clock_file, "2026-01-09T00:00:00Z"),
                "Fetched registry dependency matches updated version 0.1.9");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_mirror_env,
                                    "mod verify",
                                    0,
                                    "tablo mod verify rejects alternate registry root with same package bytes");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_mirror_env,
                                    "mod fetch",
                                    0,
                                    "tablo mod fetch rejects alternate registry root with same package bytes");

    TEST_ASSERT(write_text_file(registry_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"tampered-fetch\";\n"
                                "}\n"),
                "Mutate registry package source to force fetch checksum mismatch");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod fetch",
                                    0,
                                    "tablo mod fetch detects checksum mismatch against lock");
    TEST_ASSERT(write_text_file(registry_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-09T00:00:00Z\";\n"
                                "}\n"),
                "Restore registry package source after fetch mismatch");
    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod fetch",
                                    1,
                                    "tablo mod fetch succeeds after restoring source");

    TEST_ASSERT(write_text_file(main_file,
                                "import \"acme/util/util.tblo\";\n"
                                "import \"acme/clock/clock.tblo\";\n"
                                "func main(): void {\n"
                                "    println(utilValue());\n"
                                "    println(clockNow());\n"
                                "}\n"),
                "Rewrite main.tblo with lock-aware dependency imports");
    run_cli_command_in_dir(exe_abs,
                           app_dir,
                           "run main.tblo",
                           1,
                           "tablo run resolves dependency imports via lock + vendor");

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod verify",
                                    1,
                                    "tablo mod verify succeeds after fetch");

    TEST_ASSERT(write_text_file(registry_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"tampered\";\n"
                                "}\n"),
                "Mutate registry package source to force checksum mismatch");

    run_cli_command_in_dir_with_env(exe_abs,
                                    app_dir,
                                    registry_env,
                                    "mod verify",
                                    0,
                                    "tablo mod verify detects checksum mismatch");

    TEST_ASSERT(write_text_file(registry_pkg_file_019,
                                "func clockNow(): string {\n"
                                "    return \"2026-01-09T00:00:00Z\";\n"
                                "}\n"),
                "Restore registry package source");

    run_cli_command_in_dir(exe_abs, app_dir, "mod tidy", 1, "tablo mod tidy keeps used dependency");
    TEST_ASSERT(file_contains_text(mod_file, "\"acme/util\""), "used dependency remains after tidy");
    TEST_ASSERT(file_contains_text(mod_file, "\"acme/clock\""), "used registry dependency remains after tidy");

    TEST_ASSERT(write_text_file(main_file,
                                "func main(): void {\n"
                                "    println(\"hello\");\n"
                                "}\n"),
                "Rewrite main.tblo without dependency import");

    run_cli_command_in_dir(exe_abs, app_dir, "mod tidy", 1, "tablo mod tidy removes unused dependencies");
    TEST_ASSERT(file_not_contains_text(mod_file, "\"acme/util\""), "unused dependency removed by tidy");
    TEST_ASSERT(file_not_contains_text(mod_file, "\"acme/clock\""), "unused registry dependency removed by tidy");

    run_cli_command_in_dir(exe_abs, app_dir, "mod add broken-spec", 0, "tablo mod add rejects invalid spec");
    run_cli_command_in_dir(exe_abs, app_dir, "mod list", 1, "tablo mod list succeeds");

    remove_tree(root);
}

int main(void) {
    printf("Running CLI Mod Command Tests...\n\n");
    test_cli_mod_command();

    printf("\nCLI Mod Command Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
