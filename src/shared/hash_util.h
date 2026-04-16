#ifndef NEWOS_HASH_UTIL_H
#define NEWOS_HASH_UTIL_H

#include <stddef.h>

#define HASH_MD5_SIZE 16
#define HASH_SHA256_SIZE 32
#define HASH_SHA512_SIZE 64

int hash_md5_stream(int fd, unsigned char out[HASH_MD5_SIZE]);
int hash_sha256_stream(int fd, unsigned char out[HASH_SHA256_SIZE]);
int hash_sha512_stream(int fd, unsigned char out[HASH_SHA512_SIZE]);
void hash_to_hex(const unsigned char *digest, size_t digest_size, char *hex_out);

#endif
