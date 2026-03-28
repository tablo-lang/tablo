#include <stddef.h>
#include <stdint.h>

#include "artifact.h"
#include "fuzz_targets.h"

int fuzz_artifact_one_input(const uint8_t* data, size_t size) {
    if (!data) return 0;
    if (size > (1u << 20)) return 0;

    LoadedBytecodeArtifact artifact;
    char err[256];
    if (artifact_load_bytes(data, size, &artifact, err, sizeof(err))) {
        artifact_loaded_free(&artifact);
    }

    return 0;
}

#ifdef TABLO_LIBFUZZER_ENTRYPOINT
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_artifact_one_input(data, size);
}
#endif
