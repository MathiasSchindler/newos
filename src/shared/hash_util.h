#ifndef NEWOS_HASH_UTIL_H
#define NEWOS_HASH_UTIL_H

#include <stddef.h>
#include "crypto/md5.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"

#define HASH_MD5_SIZE CRYPTO_MD5_DIGEST_SIZE
#define HASH_SHA1_SIZE CRYPTO_SHA1_DIGEST_SIZE
#define HASH_SHA256_SIZE CRYPTO_SHA256_DIGEST_SIZE
#define HASH_SHA512_SIZE CRYPTO_SHA512_DIGEST_SIZE

int hash_md5_stream(int fd, unsigned char out[HASH_MD5_SIZE]);
int hash_sha1_stream(int fd, unsigned char out[HASH_SHA1_SIZE]);
int hash_sha256_stream(int fd, unsigned char out[HASH_SHA256_SIZE]);
int hash_sha512_stream(int fd, unsigned char out[HASH_SHA512_SIZE]);
void hash_to_hex(const unsigned char *digest, size_t digest_size, char *hex_out);
typedef int (*HashStreamFunction)(int fd, unsigned char *out);
int hash_print_digest_line(const unsigned char *digest, size_t digest_size, const char *label, int binary_mode, int zero_terminated);
int hash_compare_digest(const unsigned char *lhs, const unsigned char *rhs, size_t digest_size);
int hash_read_record(int fd, int zero_terminated, char *buffer, size_t buffer_size, int *has_record_out);
int hash_parse_check_record(const char *record, size_t digest_size, unsigned char *expected_digest, const char **path_out);
int hash_verify_manifest(const char *tool_name, int fd, size_t digest_size, HashStreamFunction hash_stream, int zero_terminated, int quiet, int status_only);
int hash_sum_main(int argc, char **argv, const char *tool_name, size_t digest_size, HashStreamFunction hash_stream, int allow_binary_mode);

#endif
