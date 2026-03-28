#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "fuzz_targets.h"

#ifndef TABLO_SOURCE_DIR
#define TABLO_SOURCE_DIR "."
#endif

typedef int (*FuzzEntryFn)(const uint8_t* data, size_t size);

typedef struct FuzzCorpusTarget {
    const char* name;
    const char* corpus_subdir;
    FuzzEntryFn entry;
} FuzzCorpusTarget;

static const FuzzCorpusTarget FUZZ_CORPUS_TARGETS[] = {
    {"lexer", "lexer", fuzz_lexer_one_input},
    {"parser", "parser", fuzz_parser_one_input},
    {"compile", "compile", fuzz_compile_one_input},
    {"http", "http", fuzz_http_one_input},
    {"artifact", "artifact", fuzz_artifact_one_input},
};

static int read_file_bytes(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* file = NULL;
    long file_size = 0;
    uint8_t* data = NULL;
    size_t read_size = 0;

    if (!path || !out_data || !out_size) {
        return -1;
    }

    *out_data = NULL;
    *out_size = 0;

    file = fopen(path, "rb");
    if (!file) {
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return -1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    if (file_size > 0) {
        data = (uint8_t*)malloc((size_t)file_size);
        if (!data) {
            fclose(file);
            return -1;
        }
        read_size = fread(data, 1, (size_t)file_size, file);
        if (read_size != (size_t)file_size) {
            free(data);
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    *out_data = data;
    *out_size = (size_t)file_size;
    return 0;
}

static int run_corpus_target(const FuzzCorpusTarget* target) {
    char corpus_dir[1024];
    int file_count = 0;

    if (!target || !target->entry) {
        return 1;
    }

    snprintf(corpus_dir,
             sizeof(corpus_dir),
             "%s/tests/fuzz/corpus/%s",
             TABLO_SOURCE_DIR,
             target->corpus_subdir);

#ifdef _WIN32
    {
        char pattern[1100];
        WIN32_FIND_DATAA find_data;
        HANDLE find_handle;

        snprintf(pattern, sizeof(pattern), "%s/*", corpus_dir);
        find_handle = FindFirstFileA(pattern, &find_data);
        if (find_handle == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Failed to open corpus directory '%s'\n", corpus_dir);
            return 1;
        }

        do {
            char file_path[1400];
            uint8_t* data = NULL;
            size_t size = 0;

            if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
                continue;
            }
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            snprintf(file_path, sizeof(file_path), "%s/%s", corpus_dir, find_data.cFileName);
            if (read_file_bytes(file_path, &data, &size) != 0) {
                fprintf(stderr, "Failed to read corpus file '%s'\n", file_path);
                FindClose(find_handle);
                return 1;
            }

            target->entry(data, size);
            free(data);
            file_count++;
        } while (FindNextFileA(find_handle, &find_data) != 0);

        FindClose(find_handle);
    }
#else
    {
        char file_path[1400];
        DIR* dir = NULL;
        struct dirent* entry = NULL;

        dir = opendir(corpus_dir);
        if (!dir) {
            fprintf(stderr, "Failed to open corpus directory '%s': %s\n", corpus_dir, strerror(errno));
            return 1;
        }

        while ((entry = readdir(dir)) != NULL) {
        uint8_t* data = NULL;
        size_t size = 0;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            snprintf(file_path, sizeof(file_path), "%s/%s", corpus_dir, entry->d_name);
            if (read_file_bytes(file_path, &data, &size) != 0) {
                fprintf(stderr, "Failed to read corpus file '%s'\n", file_path);
                closedir(dir);
                return 1;
            }

            target->entry(data, size);
            free(data);
            file_count++;
        }

        closedir(dir);
    }
#endif

    if (file_count == 0) {
        fprintf(stderr, "Corpus directory '%s' is empty\n", corpus_dir);
        return 1;
    }

    printf("  %s: %d corpus files replayed\n", target->name, file_count);
    return 0;
}

static const FuzzCorpusTarget* find_corpus_target(const char* name) {
    size_t count = sizeof(FUZZ_CORPUS_TARGETS) / sizeof(FUZZ_CORPUS_TARGETS[0]);
    size_t i;
    for (i = 0; i < count; i++) {
        if (strcmp(FUZZ_CORPUS_TARGETS[i].name, name) == 0) {
            return &FUZZ_CORPUS_TARGETS[i];
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    int failures = 0;
    int ran = 0;
    int i;

    printf("Running fuzz corpus replay...\n");

    if (argc <= 1) {
        size_t count = sizeof(FUZZ_CORPUS_TARGETS) / sizeof(FUZZ_CORPUS_TARGETS[0]);
        size_t index;
        for (index = 0; index < count; index++) {
            failures += run_corpus_target(&FUZZ_CORPUS_TARGETS[index]);
            ran++;
        }
    } else {
        for (i = 1; i < argc; i++) {
            const FuzzCorpusTarget* target = find_corpus_target(argv[i]);
            if (!target) {
                fprintf(stderr, "Unknown fuzz corpus target '%s'\n", argv[i]);
                return 1;
            }
            failures += run_corpus_target(target);
            ran++;
        }
    }

    printf("Fuzz corpus replay results:\n");
    printf("  Targets run: %d\n", ran);
    printf("  Failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
