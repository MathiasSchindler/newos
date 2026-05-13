#ifndef NEWOS_TLS13_HANDSHAKE_H
#define NEWOS_TLS13_HANDSHAKE_H

#include <stddef.h>

// Minimal TLS 1.3 handshake message encode/decode helpers.

#define TLS13_HANDSHAKE_CLIENT_HELLO 1
#define TLS13_HANDSHAKE_SERVER_HELLO 2

#define TLS13_EXT_SERVER_NAME 0x0000
#define TLS13_EXT_SUPPORTED_GROUPS 0x000a
#define TLS13_EXT_SIGNATURE_ALGORITHMS 0x000d
#define TLS13_EXT_SUPPORTED_VERSIONS 0x002b
#define TLS13_EXT_PSK_KEY_EXCHANGE_MODES 0x002d
#define TLS13_EXT_KEY_SHARE 0x0033
#define TLS13_EXT_SESSION_TICKET 0x0023
#define TLS13_EXT_RENEGOTIATION_INFO 0xff01
#define TLS13_EXT_RECORD_SIZE_LIMIT 0x001c

#define TLS13_GROUP_X25519 0x001d

struct Tls13ServerHello {
	unsigned short legacy_version;
	unsigned char random[32];
	unsigned char legacy_session_id_echo_len;
	unsigned short cipher_suite;
	unsigned char legacy_compression_method;

	unsigned short selected_version; // from supported_versions
	unsigned short key_share_group;  // from key_share
	unsigned char key_share[32];
	unsigned short key_share_len;
};

// Builds the RFC 8448 Section 3 ClientHello (196 bytes) with provided random and key_share.
// Returns 0 on success, -1 on error.
int tls13_build_client_hello_rfc8448_1rtt(
	const unsigned char random32[32],
	const unsigned char x25519_pub[32],
	unsigned char *out, size_t out_cap, size_t *out_len
);

// Builds a minimal TLS 1.3 ClientHello suitable for real servers.
// Includes: server_name (SNI), supported_versions (TLS 1.3), supported_groups (x25519),
// signature_algorithms (RFC 8448 list), key_share (x25519), psk_key_exchange_modes (psk_dhe_ke).
// Returns 0 on success, -1 on error.
int tls13_build_client_hello(
	const char *sni, size_t sni_len,
	const unsigned char random32[32],
	const unsigned char *legacy_session_id, size_t legacy_session_id_len,
	const unsigned char x25519_pub[32],
	unsigned char *out, size_t out_cap, size_t *out_len
);

// Parses a TLS 1.3 ServerHello handshake message (handshake header included).
// Only extracts supported_versions and key_share (x25519) extensions.
// Returns 0 on success, -1 on parse error.
int tls13_parse_server_hello(
	const unsigned char *msg, size_t msg_len,
	struct Tls13ServerHello *out
);

#endif
