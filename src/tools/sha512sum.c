#include "hash_util.h"

int main(int argc, char **argv) {
    return hash_sum_main(argc, argv, "sha512sum", HASH_SHA512_SIZE, hash_sha512_stream, 0);
}
