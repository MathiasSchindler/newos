#include "hash_util.h"

int main(int argc, char **argv) {
    return hash_sum_main(argc, argv, "md5sum", HASH_MD5_SIZE, hash_md5_stream, 0);
}
