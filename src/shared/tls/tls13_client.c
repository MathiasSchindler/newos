#include "tls/tls13_client.h"

#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/hkdf_sha256.h"
#include "crypto/sha256.h"
#include "crypto/x509.h"
#include "platform.h"
#include "runtime.h"
#include "tls/tls13.h"
#include "tls/tls13_handshake.h"
#include "tls/tls13_record.h"
#include "tls/tls13_transcript.h"

#define TLS13_HS_FINISHED 20
#define TLS13_HS_CERTIFICATE 11
#define TLS13_HS_CERTIFICATE_VERIFY 15

struct tls13_ap_variant {
	unsigned char c_key[16];
	unsigned char c_iv[12];
	unsigned char s_key[16];
	unsigned char s_iv[12];
	unsigned char which_master;
	unsigned char which_th;
};

static const unsigned char *sha256_empty_hs_ptr(void) {
	return (const unsigned char *)
		"\xe3\xb0\xc4\x42\x98\xfc\x1c\x14\x9a\xfb\xf4\xc8\x99\x6f\xb9\x24"
		"\x27\xae\x41\xe4\x64\x9b\x93\x4c\xa4\x95\x99\x1b\x78\x52\xb8\x55";
}

static int read_exact_timeout(int fd, void *buf, size_t len, unsigned int timeout_ms) {
	unsigned char *p = (unsigned char *)buf;
	size_t got = 0;
	while (got < len) {
		size_t ready_index = 0U;
		if (platform_poll_fds(&fd, 1U, &ready_index, (int)timeout_ms) <= 0) return 0;
		long r = platform_read(fd, p + got, len - got);
		if (r < 0) return 0;
		if (r == 0) return 0;
		got += (size_t)r;
	}
	return 1;
}

static int write_all_timeout(int fd, const void *buf, size_t len, unsigned int timeout_ms) {
	const unsigned char *p = (const unsigned char *)buf;
	size_t off = 0;
	(void)timeout_ms;
	while (off < len) {
		long w = platform_write(fd, p + off, len - off);
		if (w < 0) return 0;
		if (w == 0) return 0;
		off += (size_t)w;
	}
	return 1;
}

static int record_read_timeout(int fd, unsigned int timeout_ms, unsigned char hdr[5], unsigned char *payload, size_t payload_cap, size_t *out_len) {
	if (!hdr || !payload || !out_len) return 0;
	if (!read_exact_timeout(fd, hdr, 5, timeout_ms)) return 0;
	unsigned short rlen = (unsigned short)(((unsigned short)hdr[3] << 8) | (unsigned short)hdr[4]);
	if ((size_t)rlen > payload_cap) return 0;
	if (!read_exact_timeout(fd, payload, (size_t)rlen, timeout_ms)) return 0;
	*out_len = (size_t)rlen;
	return 1;
}

static int hs_append(unsigned char *buf, size_t cap, size_t *io_len, const unsigned char *p, size_t n) {
	if (!buf || !io_len) return -1;
	if (!p && n) return -1;
	if (*io_len + n > cap) return -1;
	if (n) memcpy(buf + *io_len, p, n);
	*io_len += n;
	return 0;
}

static int hs_consume_one(unsigned char *buf, size_t *io_len, unsigned char *out_type, unsigned int *out_body_len, unsigned char *out_msg, size_t out_cap,
	size_t *out_msg_len) {
	if (!buf || !io_len || !out_type || !out_body_len || !out_msg || !out_msg_len) return -1;
	if (*io_len < 4u) return 1;
	unsigned char ht = buf[0];
	unsigned int hl = ((unsigned int)buf[1] << 16) | ((unsigned int)buf[2] << 8) | (unsigned int)buf[3];
	size_t total = 4u + (size_t)hl;
	if (total > *io_len) return 1;
	if (total > out_cap) return -1;
	memcpy(out_msg, buf, total);
	*out_type = ht;
	*out_body_len = hl;
	*out_msg_len = total;
	size_t rem = *io_len - total;
	if (rem) memmove(buf, buf + total, rem);
	*io_len = rem;
	return 0;
}

static int getrandom_best_effort(void *buf, size_t len) {
	return crypto_random_bytes((unsigned char *)buf, len) == 0;
}

static void tlsdbg(struct Tls13Client *c, const char *s) {
	if (!c || !c->debug || !s) return;
	(void)rt_write_cstr(2, s);
}

static int tls13_client_fail(struct Tls13Client *c, const char *message) {
	if (c) c->last_error = message;
	return -1;
}

static const char *tls13_plaintext_alert_error(unsigned char description) {
	switch (description) {
		case 40: return "native tls handshake failure alert";
		case 70: return "native tls protocol version alert; TLS 1.2 may be required";
		case 80: return "native tls internal error alert";
		case 112: return "native tls unrecognized name alert";
		default: return "native tls plaintext alert";
	}
}

static long tls13_client_deliver_app(struct Tls13Client *c, unsigned char *buf, size_t cap, const unsigned char *pt, size_t pt_len) {
	size_t deliver = pt_len;

	if (deliver > cap) deliver = cap;
	if (deliver) memcpy(buf, pt, deliver);
	if (deliver < pt_len) {
		size_t remainder = pt_len - deliver;
		if (remainder > sizeof(c->pending_app)) {
			(void)tls13_client_fail(c, "native tls application plaintext too large");
			return -1;
		}
		memcpy(c->pending_app, pt + deliver, remainder);
		c->pending_app_len = remainder;
		c->pending_app_offset = 0;
	}
	return (long)deliver;
}

static unsigned int load_u24(const unsigned char *p) {
	return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[2];
}

static unsigned short load_u16(const unsigned char *p) {
	return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static int tls13_store_certificate_message(struct Tls13Client *c, const unsigned char *msg, size_t msg_len) {
	const unsigned char *body;
	size_t body_len;
	size_t pos;
	size_t end;
	unsigned int list_len;

	if (!c || !msg || msg_len < 8U) return -1;
	body = msg + 4U;
	body_len = msg_len - 4U;
	pos = 0U;
	if (pos >= body_len) return -1;
	if (pos + 1U + body[pos] > body_len) return -1;
	pos += 1U + body[pos];
	if (pos + 3U > body_len) return -1;
	list_len = load_u24(body + pos);
	pos += 3U;
	if (pos + list_len != body_len) return -1;
	end = pos + list_len;
	c->peer_cert_count = 0U;
	while (pos < end) {
		unsigned int cert_len;
		unsigned short ext_len;

		if (pos + 3U > end) return -1;
		cert_len = load_u24(body + pos);
		pos += 3U;
		if (cert_len == 0U || cert_len > TLS13_MAX_PEER_CERT_DER_SIZE || pos + cert_len + 2U > end) return -1;
		if (c->peer_cert_count < TLS13_MAX_PEER_CERTS) {
			memcpy(c->peer_cert_der[c->peer_cert_count], body + pos, cert_len);
			c->peer_cert_len[c->peer_cert_count] = cert_len;
			c->peer_cert_count += 1U;
		}
		pos += cert_len;
		ext_len = load_u16(body + pos);
		pos += 2U;
		if (pos + ext_len > end) return -1;
		pos += ext_len;
	}
	return c->peer_cert_count != 0U ? 0 : -1;
}

static int tls13_verify_certificate_verify(struct Tls13Client *c, const unsigned char transcript_hash[32], const unsigned char *msg, size_t msg_len) {
	static const char context[] = "TLS 1.3, server CertificateVerify";
	unsigned char signed_content_storage[64U + sizeof(context) + 32U];
	unsigned char *signed_content = signed_content_storage;
	const unsigned char *body;
	size_t body_len;
	size_t context_len = sizeof(context) - 1U;
	size_t signed_content_len = 64U + context_len + 1U + 32U;
	unsigned short scheme;
	unsigned short signature_len;
	size_t i;
	int result;

	if (!c || c->peer_cert_count == 0U || !transcript_hash || !msg || msg_len < 8U) return -1;
	body = msg + 4U;
	body_len = msg_len - 4U;
	if (body_len < 4U) return -1;
	scheme = load_u16(body);
	signature_len = load_u16(body + 2U);
	if ((size_t)signature_len + 4U != body_len) return -1;
	if (c->debug) {
		tlsdbg(c, "cert_verify_scheme ");
		(void)rt_write_uint(2, (unsigned long long)scheme);
		tlsdbg(c, " sig_len ");
		(void)rt_write_uint(2, (unsigned long long)signature_len);
		tlsdbg(c, "\n");
	}
	for (i = 0; i < 64U; ++i) signed_content[i] = 0x20U;
	memcpy(signed_content + 64U, context, context_len);
	signed_content[64U + context_len] = 0U;
	memcpy(signed_content + 64U + context_len + 1U, transcript_hash, 32U);
	result = crypto_x509_verify_tls13_certificate_verify(
		c->peer_cert_der[0],
		c->peer_cert_len[0],
		scheme,
		signed_content,
		signed_content_len,
		body + 4U,
		signature_len
	);
	if (c->debug) {
		tlsdbg(c, "cert_verify_result ");
		(void)rt_write_uint(2, result == 0 ? 0ULL : 1ULL);
		tlsdbg(c, "\n");
	}
	return result;
}

void tls13_client_init(struct Tls13Client *c, int fd, unsigned int timeout_ms) {
	if (!c) return;
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	c->timeout_ms = timeout_ms;
	c->debug = 0;
	c->c_ap_seq = 0;
	c->s_ap_seq = 0;
	c->pending_app_len = 0;
	c->pending_app_offset = 0;
	c->peer_cert_count = 0;
	c->handshake_done = 0;
	c->last_error = "none";
}

const char *tls13_client_last_error(const struct Tls13Client *c) {
	if (!c || !c->last_error) return "unknown tls13 error";
	return c->last_error;
}

size_t tls13_client_peer_certificates(const struct Tls13Client *c, CryptoX509DerCert *certs, size_t cert_capacity) {
	size_t i;
	size_t count;

	if (!c || !certs || cert_capacity == 0U) return 0U;
	count = c->peer_cert_count < cert_capacity ? c->peer_cert_count : cert_capacity;
	for (i = 0; i < count; ++i) {
		certs[i].data = c->peer_cert_der[i];
		certs[i].length = c->peer_cert_len[i];
	}
	return count;
}

int tls13_client_handshake(struct Tls13Client *c, const char *sni, size_t sni_len) {
	if (!c) return -1;
	c->last_error = "none";
	if (c->fd < 0) return tls13_client_fail(c, "native tls invalid socket");
	if (sni && (sni_len == 0 || sni_len > 255u)) return tls13_client_fail(c, "native tls invalid sni");

	// Build ClientHello
	unsigned char ch_random[32];
	unsigned char ch_sid[32];
	unsigned char x25519_priv[32];
	unsigned char x25519_pub[32];
	(void)getrandom_best_effort(ch_random, sizeof(ch_random));
	(void)getrandom_best_effort(ch_sid, sizeof(ch_sid));
	(void)getrandom_best_effort(x25519_priv, sizeof(x25519_priv));
	crypto_x25519_scalarmult_base(x25519_pub, x25519_priv);

	unsigned char ch[2048];
	size_t ch_len = 0;
	if (tls13_build_client_hello(sni ? sni : "", sni ? sni_len : 0, ch_random, ch_sid, sizeof(ch_sid), x25519_pub, ch, sizeof(ch), &ch_len) != 0) {
		return tls13_client_fail(c, "native tls client hello build failed");
	}

	// TLSPlaintext record wrapping the handshake message.
	unsigned char rec[5 + 2048];
	if (ch_len > 2048) return tls13_client_fail(c, "native tls client hello too large");
	rec[0] = 22;
	rec[1] = 0x03;
	rec[2] = 0x01;
	rec[3] = (unsigned char)((ch_len >> 8) & 0xFFu);
	rec[4] = (unsigned char)(ch_len & 0xFFu);
	memcpy(rec + 5, ch, ch_len);
	if (!write_all_timeout(c->fd, rec, 5 + ch_len, c->timeout_ms)) return tls13_client_fail(c, "native tls client hello write failed");

	// Read records until we see ServerHello.
	unsigned char rhdr[5];
	unsigned char payload[65536];
	unsigned char sh_msg[2048];
	size_t sh_len = 0;
	int got_sh = 0;
	for (int iter = 0; iter < 32; iter++) {
		if (!read_exact_timeout(c->fd, rhdr, 5, c->timeout_ms)) break;
		unsigned char rtype = rhdr[0];
		unsigned short rlen = (unsigned short)(((unsigned short)rhdr[3] << 8) | (unsigned short)rhdr[4]);
		if (!read_exact_timeout(c->fd, payload, (size_t)rlen, c->timeout_ms)) break;
		if (rtype == TLS_CONTENT_ALERT) {
			return tls13_client_fail(c, rlen >= 2 ? tls13_plaintext_alert_error(payload[1]) : "native tls plaintext alert");
		}
		if (rtype != 22) continue;
		size_t off = 0;
		while (off + 4 <= (size_t)rlen) {
			unsigned char ht = payload[off + 0];
			unsigned int hl = ((unsigned int)payload[off + 1] << 16) | ((unsigned int)payload[off + 2] << 8) | (unsigned int)payload[off + 3];
			size_t htot = 4u + (size_t)hl;
			if (off + htot > (size_t)rlen) break;
			if (ht == TLS13_HANDSHAKE_SERVER_HELLO) {
				if (htot > sizeof(sh_msg)) break;
				memcpy(sh_msg, payload + off, htot);
				sh_len = htot;
				got_sh = 1;
				break;
			}
			off += htot;
		}
		if (got_sh) break;
	}
	if (!got_sh) return tls13_client_fail(c, "native tls server hello not received");

	struct Tls13ServerHello sh;
	if (tls13_parse_server_hello(sh_msg, sh_len, &sh) != 0) return tls13_client_fail(c, "native tls server hello parse failed");

	struct Tls13Transcript t;
	tls13_transcript_init(&t);
	tls13_transcript_update(&t, ch, ch_len);
	tls13_transcript_update(&t, sh_msg, sh_len);
	unsigned char chsh_hash[32];
	tls13_transcript_final(&t, chsh_hash);

	if (sh.selected_version != 0x0304) return tls13_client_fail(c, "native tls server did not select TLS 1.3; TLS 1.2 is not implemented yet");
	if (sh.key_share_group != TLS13_GROUP_X25519 || sh.key_share_len != 32) return tls13_client_fail(c, "native tls server did not select x25519");

	unsigned char ecdhe[32];
	if (crypto_x25519_scalarmult(ecdhe, x25519_priv, sh.key_share) != 0) return tls13_client_fail(c, "native tls x25519 shared secret failed");

	unsigned char zeros32[32];
	memset(zeros32, 0, sizeof(zeros32));
	unsigned char early[32];
	crypto_hkdf_sha256_extract(early, zeros32, sizeof(zeros32), zeros32, sizeof(zeros32));

	unsigned char derived[32];
	if (tls13_derive_secret(early, "derived", sha256_empty_hs_ptr(), derived) != 0) return tls13_client_fail(c, "native tls early secret derivation failed");

	unsigned char handshake_secret[32];
	crypto_hkdf_sha256_extract(handshake_secret, derived, sizeof(derived), ecdhe, sizeof(ecdhe));

	unsigned char c_hs[32];
	unsigned char s_hs[32];
	if (tls13_derive_secret(handshake_secret, "c hs traffic", chsh_hash, c_hs) != 0) return tls13_client_fail(c, "native tls client handshake traffic secret failed");
	if (tls13_derive_secret(handshake_secret, "s hs traffic", chsh_hash, s_hs) != 0) return tls13_client_fail(c, "native tls server handshake traffic secret failed");

	unsigned char c_key[16];
	unsigned char c_iv[12];
	unsigned char s_key[16];
	unsigned char s_iv[12];
	if (tls13_hkdf_expand_label(c_hs, "key", 0, 0, c_key, sizeof(c_key)) != 0) return tls13_client_fail(c, "native tls client handshake key derivation failed");
	if (tls13_hkdf_expand_label(c_hs, "iv", 0, 0, c_iv, sizeof(c_iv)) != 0) return tls13_client_fail(c, "native tls client handshake iv derivation failed");
	if (tls13_hkdf_expand_label(s_hs, "key", 0, 0, s_key, sizeof(s_key)) != 0) return tls13_client_fail(c, "native tls server handshake key derivation failed");
	if (tls13_hkdf_expand_label(s_hs, "iv", 0, 0, s_iv, sizeof(s_iv)) != 0) return tls13_client_fail(c, "native tls server handshake iv derivation failed");

	unsigned long long s_hs_seq = 0;
	unsigned long long c_hs_seq = 0;
	int verified_certificate_verify = 0;
	int verified_server_finished = 0;

	unsigned char hs_buf[131072];
	size_t hs_buf_len = 0;
	for (int iter = 0; iter < 256; iter++) {
		size_t rlen = 0;
		if (!record_read_timeout(c->fd, c->timeout_ms, rhdr, payload, sizeof(payload), &rlen)) break;
		unsigned char rtype = rhdr[0];
		if (rtype == TLS_CONTENT_CHANGE_CIPHER_SPEC) continue;
		if (rtype == TLS_CONTENT_ALERT) break;
		if (rtype != TLS_CONTENT_APPLICATION_DATA) continue;

		unsigned char record[5 + 65536];
		size_t record_len = 5u + rlen;
		if (record_len > sizeof(record)) break;
		memcpy(record, rhdr, 5);
		memcpy(record + 5, payload, rlen);

		unsigned char inner_type = 0;
		unsigned char pt[65536];
		size_t pt_len = 0;
		if (tls13_record_decrypt(s_key, s_iv, s_hs_seq, record, record_len, &inner_type, pt, sizeof(pt), &pt_len) != 0) break;
		s_hs_seq++;
		if (inner_type != TLS_CONTENT_HANDSHAKE) continue;
		if (hs_append(hs_buf, sizeof(hs_buf), &hs_buf_len, pt, pt_len) != 0) break;

		for (;;) {
			unsigned char msg_type = 0;
			unsigned int msg_body_len = 0;
			unsigned char msg[65536];
			size_t msg_len = 0;
			int cr = hs_consume_one(hs_buf, &hs_buf_len, &msg_type, &msg_body_len, msg, sizeof(msg), &msg_len);
			if (cr == 1) break;
			if (cr != 0) { iter = 9999; break; }

			if (c->debug) {
				tlsdbg(c, "hs_type ");
				(void)rt_write_uint(2, (unsigned long long)msg_type);
				tlsdbg(c, " hs_len ");
				(void)rt_write_uint(2, (unsigned long long)msg_body_len);
				tlsdbg(c, "\n");
			}

			if (msg_type == TLS13_HS_CERTIFICATE) {
				if (tls13_store_certificate_message(c, msg, msg_len) != 0) { iter = 9999; break; }
			}

			if (msg_type == TLS13_HS_CERTIFICATE_VERIFY) {
				unsigned char th_cert_verify[32];
				tls13_transcript_final(&t, th_cert_verify);
				if (tls13_verify_certificate_verify(c, th_cert_verify, msg, msg_len) != 0) { iter = 9999; break; }
				verified_certificate_verify = 1;
			}

			if (msg_type == TLS13_HS_FINISHED) {
				unsigned char th_pre[32];
				tls13_transcript_final(&t, th_pre);
				unsigned char s_finished_key[32];
				if (tls13_finished_key(s_hs, s_finished_key) != 0) { iter = 9999; break; }
				unsigned char expected_verify[32];
				tls13_finished_verify_data(s_finished_key, th_pre, expected_verify);
				memset(s_finished_key, 0, sizeof(s_finished_key));
				if (msg_body_len != 32 || msg_len != 36) { iter = 9999; break; }
				if (!crypto_constant_time_equal(expected_verify, msg + 4, 32)) { iter = 9999; break; }
				verified_server_finished = 1;
			}

			tls13_transcript_update(&t, msg, msg_len);
			if (msg_type == TLS13_HS_FINISHED) break;
		}

		if (verified_server_finished) break;
	}
	if (!verified_certificate_verify) return tls13_client_fail(c, "native tls certificate verify failed");
	if (!verified_server_finished) return tls13_client_fail(c, "native tls server finished verification failed");

	unsigned char th_post_server_finished[32];
	tls13_transcript_final(&t, th_post_server_finished);

	unsigned char c_finished_key[32];
	if (tls13_finished_key(c_hs, c_finished_key) != 0) return tls13_client_fail(c, "native tls client finished key derivation failed");
	unsigned char c_verify[32];
	tls13_finished_verify_data(c_finished_key, th_post_server_finished, c_verify);
	memset(c_finished_key, 0, sizeof(c_finished_key));

	unsigned char cfin[4 + 32];
	cfin[0] = (unsigned char)TLS13_HS_FINISHED;
	cfin[1] = 0;
	cfin[2] = 0;
	cfin[3] = 32;
	memcpy(cfin + 4, c_verify, 32);

	unsigned char cfin_record[5 + 1024];
	size_t cfin_record_len = 0;
	if (tls13_record_encrypt(c_key, c_iv, c_hs_seq, TLS_CONTENT_HANDSHAKE, cfin, sizeof(cfin), cfin_record, sizeof(cfin_record), &cfin_record_len) != 0) return tls13_client_fail(c, "native tls client finished encryption failed");
	c_hs_seq++;
	if (!write_all_timeout(c->fd, cfin_record, cfin_record_len, c->timeout_ms)) return tls13_client_fail(c, "native tls client finished write failed");
	tls13_transcript_update(&t, cfin, sizeof(cfin));
	if (c->debug) tlsdbg(c, "sent_client_finished 1\n");

	unsigned char th_post_client_finished[32];
	tls13_transcript_final(&t, th_post_client_finished);

	// Derive application traffic keys (keep existing tool's pragmatic variant search).
	unsigned char derived2[32];
	if (tls13_derive_secret(handshake_secret, "derived", sha256_empty_hs_ptr(), derived2) != 0) return tls13_client_fail(c, "native tls master derived secret failed");

	struct tls13_ap_variant vars[4];
	size_t nvars = 0;
	for (int master_mode = 0; master_mode < 2; master_mode++) {
		unsigned char master_secret[32];
		if (master_mode == 0) {
			unsigned char zeros32b[32];
			memset(zeros32b, 0, sizeof(zeros32b));
			crypto_hkdf_sha256_extract(master_secret, derived2, sizeof(derived2), zeros32b, sizeof(zeros32b));
		} else {
			crypto_hkdf_sha256_extract(master_secret, derived2, sizeof(derived2), 0, 0);
		}
		for (int th_mode = 0; th_mode < 2; th_mode++) {
			const unsigned char *th = (th_mode == 0) ? th_post_server_finished : th_post_client_finished;
			unsigned char c_ap[32];
			unsigned char s_ap[32];
			if (tls13_derive_secret(master_secret, "c ap traffic", th, c_ap) != 0) return -1;
			if (tls13_derive_secret(master_secret, "s ap traffic", th, s_ap) != 0) return -1;

			struct tls13_ap_variant *v = &vars[nvars++];
			v->which_master = (unsigned char)master_mode;
			v->which_th = (unsigned char)th_mode;
			if (tls13_hkdf_expand_label(c_ap, "key", 0, 0, v->c_key, sizeof(v->c_key)) != 0) return -1;
			if (tls13_hkdf_expand_label(c_ap, "iv", 0, 0, v->c_iv, sizeof(v->c_iv)) != 0) return -1;
			if (tls13_hkdf_expand_label(s_ap, "key", 0, 0, v->s_key, sizeof(v->s_key)) != 0) return -1;
			if (tls13_hkdf_expand_label(s_ap, "iv", 0, 0, v->s_iv, sizeof(v->s_iv)) != 0) return -1;
		}
		memset(master_secret, 0, sizeof(master_secret));
	}

	unsigned char c_ap_key[16];
	unsigned char c_ap_iv[12];
	unsigned char s_ap_key[16];
	unsigned char s_ap_iv[12];
	unsigned long long c_ap_seq = 0;
	unsigned long long s_ap_seq = 0;
	int have_active_ap = 0;

	// Pre-decrypt one record to select active keys.
	{
		size_t rlen = 0;
		if (record_read_timeout(c->fd, c->timeout_ms, rhdr, payload, sizeof(payload), &rlen)) {
			unsigned char rtype = rhdr[0];
			if (rtype == TLS_CONTENT_APPLICATION_DATA) {
				unsigned char record[5 + 65536];
				size_t record_len = 5u + rlen;
				if (record_len <= sizeof(record)) {
					memcpy(record, rhdr, 5);
					memcpy(record + 5, payload, rlen);

					unsigned char inner_type = 0;
					unsigned char pt[65536];
					size_t pt_len = 0;
					for (size_t vi = 0; vi < nvars && !have_active_ap; vi++) {
						for (int seq_mode = 0; seq_mode < 2 && !have_active_ap; seq_mode++) {
							unsigned long long try_seq = (seq_mode == 0) ? 0 : s_hs_seq;
							if (tls13_record_decrypt(vars[vi].s_key, vars[vi].s_iv, try_seq, record, record_len, &inner_type, pt, sizeof(pt), &pt_len) == 0) {
								memcpy(c_ap_key, vars[vi].c_key, sizeof(c_ap_key));
								memcpy(c_ap_iv, vars[vi].c_iv, sizeof(c_ap_iv));
								memcpy(s_ap_key, vars[vi].s_key, sizeof(s_ap_key));
								memcpy(s_ap_iv, vars[vi].s_iv, sizeof(s_ap_iv));
								if (inner_type == TLS_CONTENT_APPLICATION_DATA && pt_len <= sizeof(c->pending_app)) {
									memcpy(c->pending_app, pt, pt_len);
									c->pending_app_len = pt_len;
									c->pending_app_offset = 0;
								}
								s_ap_seq = try_seq + 1;
								c_ap_seq = (seq_mode == 0) ? 0 : c_hs_seq;
								have_active_ap = 1;
								if (c->debug) {
									tlsdbg(c, "selected_ap_keys master=");
									(void)rt_write_uint(2, (unsigned long long)vars[vi].which_master);
									tlsdbg(c, " th=");
									(void)rt_write_uint(2, (unsigned long long)vars[vi].which_th);
									tlsdbg(c, " seq_mode=");
									(void)rt_write_uint(2, (unsigned long long)seq_mode);
									tlsdbg(c, "\n");
								}
								break;
							}
						}
					}
				}
			}
		}
	}

	if (!have_active_ap) {
		memcpy(c_ap_key, vars[0].c_key, sizeof(c_ap_key));
		memcpy(c_ap_iv, vars[0].c_iv, sizeof(c_ap_iv));
		memcpy(s_ap_key, vars[0].s_key, sizeof(s_ap_key));
		memcpy(s_ap_iv, vars[0].s_iv, sizeof(s_ap_iv));
		c_ap_seq = 0;
		s_ap_seq = 0;
		have_active_ap = 1;
		if (c->debug) tlsdbg(c, "selected_ap_keys_default 1\n");
	}

	memcpy(c->c_ap_key, c_ap_key, sizeof(c->c_ap_key));
	memcpy(c->c_ap_iv, c_ap_iv, sizeof(c->c_ap_iv));
	memcpy(c->s_ap_key, s_ap_key, sizeof(c->s_ap_key));
	memcpy(c->s_ap_iv, s_ap_iv, sizeof(c->s_ap_iv));
	c->c_ap_seq = c_ap_seq;
	c->s_ap_seq = s_ap_seq;
	c->handshake_done = 1;

	memset(x25519_priv, 0, sizeof(x25519_priv));
	memset(ecdhe, 0, sizeof(ecdhe));
	memset(handshake_secret, 0, sizeof(handshake_secret));
	memset(c_hs, 0, sizeof(c_hs));
	memset(s_hs, 0, sizeof(s_hs));
	memset(c_key, 0, sizeof(c_key));
	memset(c_iv, 0, sizeof(c_iv));
	memset(s_key, 0, sizeof(s_key));
	memset(s_iv, 0, sizeof(s_iv));

	return 0;
}

long tls13_client_write_app(struct Tls13Client *c, const unsigned char *buf, size_t len) {
	if (!c || !c->handshake_done) return -1;
	if (!buf && len) return -1;

	size_t off = 0;
	while (off < len) {
		size_t chunk = len - off;
		if (chunk > 16384u) chunk = 16384u;
		unsigned char rec[5 + 16384 + 64];
		size_t rec_len = 0;
		if (tls13_record_encrypt(c->c_ap_key, c->c_ap_iv, c->c_ap_seq, TLS_CONTENT_APPLICATION_DATA, buf + off, chunk,
			rec, sizeof(rec), &rec_len) != 0) return -1;
		c->c_ap_seq++;
		if (!write_all_timeout(c->fd, rec, rec_len, c->timeout_ms)) return -1;
		off += chunk;
	}
	return (long)len;
}

long tls13_client_read_app(struct Tls13Client *c, unsigned char *buf, size_t cap) {
	if (!c || !c->handshake_done) return -1;
	if (!buf || cap == 0) return -1;
	if (c->pending_app_offset < c->pending_app_len) {
		size_t available = c->pending_app_len - c->pending_app_offset;
		if (available > cap) available = cap;
		memcpy(buf, c->pending_app + c->pending_app_offset, available);
		c->pending_app_offset += available;
		if (c->pending_app_offset >= c->pending_app_len) {
			c->pending_app_len = 0;
			c->pending_app_offset = 0;
		}
		if (c->debug) tlsdbg(c, "read_pending_app 1\n");
		return (long)available;
	}

	for (;;) {
		unsigned char rhdr[5];
		unsigned char payload[65536];
		size_t rlen = 0;
		if (!record_read_timeout(c->fd, c->timeout_ms, rhdr, payload, sizeof(payload), &rlen)) return tls13_client_fail(c, "native tls read record failed");
		unsigned char rtype = rhdr[0];
		if (c->debug) {
			tlsdbg(c, "read_record_type ");
			(void)rt_write_uint(2, (unsigned long long)rtype);
			tlsdbg(c, " len ");
			(void)rt_write_uint(2, (unsigned long long)rlen);
			tlsdbg(c, "\n");
		}
		if (rtype == TLS_CONTENT_CHANGE_CIPHER_SPEC) continue;
		if (rtype == TLS_CONTENT_ALERT) {
			// Plaintext alert => treat as EOF.
			c->last_error = "native tls plaintext alert";
			return 0;
		}
		if (rtype != TLS_CONTENT_APPLICATION_DATA) continue;

		unsigned char record[5 + 65536];
		size_t record_len = 5u + rlen;
		if (record_len > sizeof(record)) return tls13_client_fail(c, "native tls record too large");
		memcpy(record, rhdr, 5);
		memcpy(record + 5, payload, rlen);

		unsigned char inner_type = 0;
		unsigned char pt[65536];
		size_t pt_len = 0;
		if (tls13_record_decrypt(c->s_ap_key, c->s_ap_iv, c->s_ap_seq, record, record_len, &inner_type, pt, sizeof(pt), &pt_len) != 0) return tls13_client_fail(c, "native tls application record decrypt failed");
		c->s_ap_seq++;
		if (c->debug) {
			tlsdbg(c, "read_inner_type ");
			(void)rt_write_uint(2, (unsigned long long)inner_type);
			tlsdbg(c, " pt_len ");
			(void)rt_write_uint(2, (unsigned long long)pt_len);
			tlsdbg(c, "\n");
		}

		if (inner_type == TLS_CONTENT_APPLICATION_DATA) {
			return tls13_client_deliver_app(c, buf, cap, pt, pt_len);
		}
		if (inner_type == TLS_CONTENT_ALERT) {
			// Decrypted alert: if close_notify, treat as EOF.
			if (pt_len >= 2 && pt[1] == 0) {
				c->last_error = "native tls close notify";
				return 0;
			}
			return tls13_client_fail(c, "native tls decrypted alert");
		}
		// Ignore other inner types for now.
	}
}

int tls13_client_close_notify(struct Tls13Client *c) {
	if (!c || !c->handshake_done) return -1;
	unsigned char alert[2];
	alert[0] = 1; // warning
	alert[1] = 0; // close_notify
	unsigned char rec[5 + 64];
	size_t rec_len = 0;
	if (tls13_record_encrypt(c->c_ap_key, c->c_ap_iv, c->c_ap_seq, TLS_CONTENT_ALERT, alert, sizeof(alert), rec, sizeof(rec), &rec_len) != 0) return -1;
	c->c_ap_seq++;
	if (!write_all_timeout(c->fd, rec, rec_len, c->timeout_ms)) return -1;
	return 0;
}
