#include "hash_util.h"

int main(int argc, char **argv) {
    return hash_sum_main(argc, argv, "sha256sum", HASH_SHA256_SIZE, hash_sha256_stream, 0);
}
