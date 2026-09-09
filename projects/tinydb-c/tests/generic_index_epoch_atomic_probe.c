#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "../src/generic_index_epoch.c"

static int make_dir(const char* path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int remove_dir(const char* path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

int main(void) {
    const char* path = "generic_index_epoch_atomic_probe.bin";
    const char* temp = "generic_index_epoch_atomic_probe.bin.tmp";
    remove(path);
    remove(temp);
    remove_dir(temp);

    if (!write_epoch_file(path, 41u)) {
        fprintf(stderr, "initial epoch write failed\n");
        return 1;
    }

    uint64_t value = 0u;
    if (!read_epoch_file(path, &value) || value != 41u) {
        fprintf(stderr, "initial epoch readback failed\n");
        return 2;
    }
    FILE* leftover = fopen(temp, "rb");
    if (leftover != NULL) {
        fclose(leftover);
        fprintf(stderr, "temporary epoch file leaked after initial publish\n");
        return 3;
    }

    if (!write_epoch_file(path, 42u) ||
        !read_epoch_file(path, &value) || value != 42u) {
        fprintf(stderr, "atomic replacement of existing epoch failed\n");
        return 4;
    }

    if (make_dir(temp) != 0) {
        fprintf(stderr, "unable to create blocking temp directory\n");
        return 5;
    }
    if (write_epoch_file(path, 43u)) {
        remove_dir(temp);
        fprintf(stderr, "epoch write unexpectedly succeeded with blocked temp path\n");
        return 6;
    }
    if (!read_epoch_file(path, &value) || value != 42u) {
        remove_dir(temp);
        fprintf(stderr, "failed replacement damaged previously durable epoch\n");
        return 7;
    }
    if (remove_dir(temp) != 0) {
        fprintf(stderr, "unable to clean blocking temp directory\n");
        return 8;
    }

    remove(path);
    printf("GENERIC_INDEX_EPOCH_ATOMIC_OK replace=yes failure_preserves_old=yes temp_cleanup=yes\n");
    return 0;
}
