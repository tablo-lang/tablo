#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <stdbool.h>
#include <stdio.h>

bool path_is_absolute(const char* path);

// Returns a newly allocated directory portion of `path`.
// If `path` has no directory separators, returns ".".
// Caller must free the returned string.
char* path_dirname_alloc(const char* path);

// Safely joins `sandbox_root` and an untrusted `user_path`.
//
// - Rejects absolute `user_path`.
// - Rejects attempts to escape the sandbox via "..".
// - On Windows, rejects ':' in `user_path` (drive letters / ADS).
//
// Returns a newly allocated full path, or NULL on error.
// If `out_error` is not NULL, it is set to a static error string.
// Caller must free the returned string.
char* path_sandbox_join_alloc(const char* sandbox_root, const char* user_path, const char** out_error);

// Resolves an untrusted path within a sandbox root and verifies that the
// canonical resolved path remains under the sandbox root (symlink/junction-safe).
//
// - `allow_missing_leaf` controls whether a missing final path component is
//   permitted (useful for create/write operations). The parent directory must
//   still exist and resolve inside the sandbox.
// - Returns a newly allocated resolved path (typically absolute/canonical), or
//   NULL on error.
// - If `out_error` is not NULL, it is set to a static error string.
// Caller must free the returned string.
char* path_sandbox_resolve_alloc(const char* sandbox_root,
                                 const char* user_path,
                                 bool allow_missing_leaf,
                                 const char** out_error);

// Opens a sandbox-resolved path using symlink-safe descriptor traversal when
// supported by the platform. `resolved_path` must already be under
// `sandbox_root` (e.g. from path_sandbox_resolve_alloc).
// Returns an open FILE* or NULL on error.
FILE* path_sandbox_fopen_resolved(const char* sandbox_root,
                                  const char* resolved_path,
                                  const char* mode,
                                  const char** out_error);

#endif
