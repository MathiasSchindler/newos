#ifndef NEWOS_CRYPTO_CHACHA20_POLY1305_H
#define NEWOS_CRYPTO_CHACHA20_POLY1305_H

#include <stddef.h>

void crypto_ssh_chachapoly_decrypt_length(
    const unsigned char key[64],
    unsigned int seqnr,
    const unsigned char encrypted_len[4],
    unsigned char plain_len[4]
);
void crypto_ssh_chachapoly_encrypt_packet(
    const unsigned char key[64],
    unsigned int seqnr,
    unsigned char *packet,
    size_t packet_len,
    unsigned char tag[16]
);
int crypto_ssh_chachapoly_decrypt_packet(
    const unsigned char key[64],
    unsigned int seqnr,
    unsigned char *packet,
    size_t packet_len,
    const unsigned char tag[16]
);

#endif
