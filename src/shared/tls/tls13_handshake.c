#include "tls/tls13_handshake.h"

#include "runtime.h"

static const char *tls13_client_hello_error = "none";
static unsigned char *tls13_client_hello_storage;

const char *tls13_client_hello_last_error(void) {
	return tls13_client_hello_error;
}

struct wbuf {
	unsigned char *p;
	size_t cap;
	size_t len;
};

static int wb_init(struct wbuf *b, unsigned char *p, size_t cap) {
	if (!b || !p || cap == 0) return -1;
	b->p = p;
	b->cap = cap;
	b->len = 0;
	return 0;
}

static int wb_put_u8(struct wbuf *b, unsigned char v) {
	if (!b || b->len + 1u > b->cap) return -1;
	b->p[b->len++] = v;
	return 0;
}

static int wb_put_u16(struct wbuf *b, unsigned short v) {
	if (!b || b->len + 2u > b->cap) return -1;
	b->p[b->len++] = (unsigned char)(v >> 8);
	b->p[b->len++] = (unsigned char)(v >> 0);
	return 0;
}

static int wb_put_u24(struct wbuf *b, unsigned int v) {
	if (!b || b->len + 3u > b->cap) return -1;
	b->p[b->len++] = (unsigned char)(v >> 16);
	b->p[b->len++] = (unsigned char)(v >> 8);
	b->p[b->len++] = (unsigned char)(v >> 0);
	return 0;
}

static int wb_put_bytes(struct wbuf *b, const unsigned char *p, size_t n) {
	if (!b) { tls13_client_hello_error = "client hello write buffer missing"; return -1; }
	if (!p && n) { tls13_client_hello_error = "client hello write source missing"; return -1; }
	if (b->len + n > b->cap) {
		tls13_client_hello_error = "client hello write capacity exceeded";
		return -1;
	}
	if (n) memcpy(b->p + b->len, p, n);
	b->len += n;
	return 0;
}

static int wb_put_cstr(struct wbuf *b, const char *s) {
	if (!s) return -1;
	size_t n = rt_strlen(s);
	return wb_put_bytes(b, (const unsigned char *)s, n);
}

static void wb_patch_u16_at(struct wbuf *b, size_t off, unsigned short v) {
	if (!b) return;
	if (off + 2u > b->cap) return;
	b->p[off + 0u] = (unsigned char)(v >> 8);
	b->p[off + 1u] = (unsigned char)(v >> 0);
}

static void wb_patch_u24_at(struct wbuf *b, size_t off, unsigned int v) {
	if (!b) return;
	if (off + 3u > b->cap) return;
	b->p[off + 0u] = (unsigned char)(v >> 16);
	b->p[off + 1u] = (unsigned char)(v >> 8);
	b->p[off + 2u] = (unsigned char)(v >> 0);
}

// RFC 8448 ClientHello uses a fixed signature_algorithms list (15 pairs).
static const unsigned char *rfc8448_sig_algs_ptr(void) {
	return (const unsigned char *)
		"\x04\x03\x05\x03\x06\x03\x02\x03\x08\x04\x08\x05\x08\x06\x04\x01"
		"\x05\x01\x06\x01\x02\x01\x04\x02\x05\x02\x06\x02\x02\x02";
}

static const unsigned char *tls13_x25519_key_share_prefix_ptr(void) {
	return (const unsigned char *)"\x00\x0a\x00\x04\x00\x02\x00\x1d\x00\x33\x00\x26\x00\x24\x00\x1d\x00\x20";
}

#define RFC8448_SIG_ALGS_LEN 30u
#define TLS13_X25519_KEY_SHARE_PREFIX_LEN 18u

const unsigned char *tls13_build_client_hello(
	const char *sni, size_t sni_len,
	const unsigned char random32[32],
	const unsigned char *legacy_session_id, size_t legacy_session_id_len,
	const unsigned char x25519_pub[32],
	size_t *out_len
) {
    unsigned char *out;

	tls13_client_hello_error = "none";
	if (tls13_client_hello_storage == 0) {
		tls13_client_hello_storage = (unsigned char *)rt_malloc(TLS13_CLIENT_HELLO_CAPACITY);
		if (tls13_client_hello_storage == 0) { tls13_client_hello_error = "client hello allocation failed"; return 0; }
	}
	out = tls13_client_hello_storage;
	if (!out_len) { tls13_client_hello_error = "missing output"; return 0; }
	if (!sni || sni_len == 0) { tls13_client_hello_error = "missing sni"; return 0; }
	if (!random32 || !x25519_pub) { tls13_client_hello_error = "missing random or key share"; return 0; }
	if (!legacy_session_id && legacy_session_id_len) { tls13_client_hello_error = "missing session id"; return 0; }
	if (legacy_session_id_len > 32u) { tls13_client_hello_error = "session id too long"; return 0; }
	if (sni_len > 0xffffu) { tls13_client_hello_error = "sni too long"; return 0; }

	size_t pos = 0;
#define CH_ENSURE(count, message) do { if ((count) > TLS13_CLIENT_HELLO_CAPACITY || pos > TLS13_CLIENT_HELLO_CAPACITY - (count)) { tls13_client_hello_error = (message); return 0; } } while (0)
#define CH_U8(value, message) do { CH_ENSURE(1U, (message)); out[pos++] = (unsigned char)(value); } while (0)
#define CH_U16(value, message) do { unsigned int v__ = (unsigned int)(value); CH_ENSURE(2U, (message)); out[pos++] = (unsigned char)(v__ >> 8); out[pos++] = (unsigned char)v__; } while (0)
#define CH_U24(value, message) do { unsigned int v__ = (unsigned int)(value); CH_ENSURE(3U, (message)); out[pos++] = (unsigned char)(v__ >> 16); out[pos++] = (unsigned char)(v__ >> 8); out[pos++] = (unsigned char)v__; } while (0)
#define CH_BYTES(bytes, count, message) do { size_t n__ = (count); const unsigned char *p__ = (const unsigned char *)(bytes); if (!p__ && n__ != 0U) { tls13_client_hello_error = (message); return 0; } CH_ENSURE(n__, (message)); if (n__ != 0U) memcpy(out + pos, p__, n__); pos += n__; } while (0)

	CH_U8(TLS13_HANDSHAKE_CLIENT_HELLO, "client hello handshake type failed");
	size_t hs_len_off = pos;
	CH_U24(0, "client hello handshake length failed");
	CH_U16(0x0303, "client hello legacy version failed");
	CH_BYTES(random32, 32U, "client hello random failed");
	CH_U8(legacy_session_id_len, "client hello session id length failed");
	CH_BYTES(legacy_session_id, legacy_session_id_len, "client hello session id bytes failed");
	CH_U16(2, "client hello cipher suites length failed");
	CH_U16(0x1301, "client hello cipher suite failed");
	CH_U8(1, "client hello compression length failed");
	CH_U8(0, "client hello compression method failed");

	size_t exts_len_off = pos;
	CH_U16(0, "client hello extensions length failed");
	size_t exts_start = pos;
	CH_U16(TLS13_EXT_SERVER_NAME, "client hello server name extension type failed");
	size_t sn_len_off = pos;
	CH_U16(0, "client hello server name extension length failed");
	size_t sn_start = pos;
	CH_U16(0, "client hello server name list length failed");
	size_t snlist_start = pos;
	CH_U8(0, "client hello server name type failed");
	CH_U16(sni_len, "client hello server name length failed");
	CH_BYTES(sni, sni_len, "client hello server name failed");
	out[sn_start + 0U] = (unsigned char)((pos - snlist_start) >> 8);
	out[sn_start + 1U] = (unsigned char)(pos - snlist_start);
	out[sn_len_off + 0U] = (unsigned char)((pos - sn_start) >> 8);
	out[sn_len_off + 1U] = (unsigned char)(pos - sn_start);

	CH_BYTES(tls13_x25519_key_share_prefix_ptr(), TLS13_X25519_KEY_SHARE_PREFIX_LEN, "client hello key share prefix failed");
	CH_BYTES(x25519_pub, 32U, "client hello key exchange failed");
	CH_U16(TLS13_EXT_SUPPORTED_VERSIONS, "client hello supported versions extension type failed");
	CH_U16(3, "client hello supported versions extension length failed");
	CH_U8(2, "client hello supported versions list length failed");
	CH_U16(0x0304, "client hello supported version failed");
	CH_U16(TLS13_EXT_SIGNATURE_ALGORITHMS, "client hello signature algorithms extension type failed");
	CH_U16(0x0020, "client hello signature algorithms extension length failed");
	CH_U16(0x001e, "client hello signature algorithms list length failed");
	CH_BYTES(rfc8448_sig_algs_ptr(), RFC8448_SIG_ALGS_LEN, "client hello signature algorithms failed");
	CH_U16(TLS13_EXT_PSK_KEY_EXCHANGE_MODES, "client hello psk modes extension type failed");
	CH_U16(2, "client hello psk modes extension length failed");
	CH_U8(1, "client hello psk modes list length failed");
	CH_U8(1, "client hello psk mode failed");

	size_t exts_len = pos - exts_start;
	if (exts_len > 0xffffu) { tls13_client_hello_error = "client hello extensions too long"; return 0; }
	out[exts_len_off + 0U] = (unsigned char)(exts_len >> 8);
	out[exts_len_off + 1U] = (unsigned char)exts_len;
	size_t hs_len = pos - (hs_len_off + 3U);
	if (hs_len > 0xffffffu) { tls13_client_hello_error = "client hello handshake too long"; return 0; }
	out[hs_len_off + 0U] = (unsigned char)(hs_len >> 16);
	out[hs_len_off + 1U] = (unsigned char)(hs_len >> 8);
	out[hs_len_off + 2U] = (unsigned char)hs_len;

	*out_len = pos;
	tls13_client_hello_error = "none";

#undef CH_BYTES
#undef CH_U24
#undef CH_U16
#undef CH_U8
#undef CH_ENSURE
	return tls13_client_hello_storage;
}

int tls13_build_client_hello_rfc8448_1rtt(
	const unsigned char random32[32],
	const unsigned char x25519_pub[32],
	unsigned char *out, size_t out_cap, size_t *out_len
) {
	if (!out || !out_len) return -1;
	if (!random32 || !x25519_pub) return -1;

	struct wbuf b;
	if (wb_init(&b, out, out_cap) != 0) return -1;

	// Handshake header
	if (wb_put_u8(&b, TLS13_HANDSHAKE_CLIENT_HELLO) != 0) return -1;
	size_t hs_len_off = b.len;
	if (wb_put_u24(&b, 0) != 0) return -1;

	// legacy_version
	if (wb_put_u16(&b, 0x0303) != 0) return -1;
	// random
	if (wb_put_bytes(&b, random32, 32) != 0) return -1;
	// legacy_session_id (empty in RFC 8448 trace)
	if (wb_put_u8(&b, 0) != 0) return -1;

	// cipher_suites
	// length = 6, suites: 1301, 1303, 1302
	if (wb_put_u16(&b, 6) != 0) return -1;
	if (wb_put_u16(&b, 0x1301) != 0) return -1;
	if (wb_put_u16(&b, 0x1303) != 0) return -1;
	if (wb_put_u16(&b, 0x1302) != 0) return -1;

	// legacy_compression_methods: length=1, method=0
	if (wb_put_u8(&b, 1) != 0) return -1;
	if (wb_put_u8(&b, 0) != 0) return -1;

	// extensions (patch length later)
	size_t exts_len_off = b.len;
	if (wb_put_u16(&b, 0) != 0) return -1;
	size_t exts_start = b.len;

	// server_name (type 0)
	if (wb_put_u16(&b, TLS13_EXT_SERVER_NAME) != 0) return -1;
	size_t sn_len_off = b.len;
	if (wb_put_u16(&b, 0) != 0) return -1;
	size_t sn_start = b.len;
	// ServerNameList length
	if (wb_put_u16(&b, 0) != 0) return -1;
	size_t snlist_start = b.len;
	// name_type + name
	if (wb_put_u8(&b, 0) != 0) return -1;
	// host_name length
	if (wb_put_u16(&b, 6) != 0) return -1;
	if (wb_put_cstr(&b, "server") != 0) return -1;
	// patch ServerNameList length
	wb_patch_u16_at(&b, sn_start, (unsigned short)(b.len - snlist_start));
	// patch extension length
	wb_patch_u16_at(&b, sn_len_off, (unsigned short)(b.len - sn_start));

	// renegotiation_info (ff01), length 1, value 00
	if (wb_put_u16(&b, TLS13_EXT_RENEGOTIATION_INFO) != 0) return -1;
	if (wb_put_u16(&b, 1) != 0) return -1;
	if (wb_put_u8(&b, 0) != 0) return -1;

	// supported_groups (0x000a)
	if (wb_put_u16(&b, TLS13_EXT_SUPPORTED_GROUPS) != 0) return -1;
	if (wb_put_u16(&b, 0x0014) != 0) return -1; // ext len 20
	if (wb_put_u16(&b, 0x0012) != 0) return -1; // list len 18
	// groups list
	if (wb_put_u16(&b, 0x001d) != 0) return -1;
	if (wb_put_u16(&b, 0x0017) != 0) return -1;
	if (wb_put_u16(&b, 0x0018) != 0) return -1;
	if (wb_put_u16(&b, 0x0019) != 0) return -1;
	if (wb_put_u16(&b, 0x0100) != 0) return -1;
	if (wb_put_u16(&b, 0x0101) != 0) return -1;
	if (wb_put_u16(&b, 0x0102) != 0) return -1;
	if (wb_put_u16(&b, 0x0103) != 0) return -1;
	if (wb_put_u16(&b, 0x0104) != 0) return -1;

	// session_ticket (0x0023) empty
	if (wb_put_u16(&b, TLS13_EXT_SESSION_TICKET) != 0) return -1;
	if (wb_put_u16(&b, 0) != 0) return -1;

	// key_share (0x0033)
	if (wb_put_u16(&b, TLS13_EXT_KEY_SHARE) != 0) return -1;
	if (wb_put_u16(&b, 0x0026) != 0) return -1; // ext len 38
	if (wb_put_u16(&b, 0x0024) != 0) return -1; // client_shares len 36
	if (wb_put_u16(&b, TLS13_GROUP_X25519) != 0) return -1;
	if (wb_put_u16(&b, 0x0020) != 0) return -1; // key_exchange len 32
	if (wb_put_bytes(&b, x25519_pub, 32) != 0) return -1;

	// supported_versions (0x002b)
	if (wb_put_u16(&b, TLS13_EXT_SUPPORTED_VERSIONS) != 0) return -1;
	if (wb_put_u16(&b, 3) != 0) return -1;
	if (wb_put_u8(&b, 2) != 0) return -1;
	if (wb_put_u16(&b, 0x0304) != 0) return -1;

	// signature_algorithms (0x000d)
	if (wb_put_u16(&b, TLS13_EXT_SIGNATURE_ALGORITHMS) != 0) return -1;
	if (wb_put_u16(&b, 0x0020) != 0) return -1;
	if (wb_put_u16(&b, 0x001e) != 0) return -1;
	if (wb_put_bytes(&b, rfc8448_sig_algs_ptr(), RFC8448_SIG_ALGS_LEN) != 0) return -1;

	// psk_key_exchange_modes (0x002d)
	if (wb_put_u16(&b, TLS13_EXT_PSK_KEY_EXCHANGE_MODES) != 0) return -1;
	if (wb_put_u16(&b, 2) != 0) return -1;
	if (wb_put_u8(&b, 1) != 0) return -1;
	if (wb_put_u8(&b, 1) != 0) return -1;

	// record_size_limit (0x001c)
	if (wb_put_u16(&b, TLS13_EXT_RECORD_SIZE_LIMIT) != 0) return -1;
	if (wb_put_u16(&b, 2) != 0) return -1;
	if (wb_put_u16(&b, 0x4001) != 0) return -1;

	size_t exts_len = b.len - exts_start;
	if (exts_len > 0xffffu) return -1;
	wb_patch_u16_at(&b, exts_len_off, (unsigned short)exts_len);

	size_t hs_len = b.len - (hs_len_off + 3u);
	if (hs_len > 0xffffffu) return -1;
	wb_patch_u24_at(&b, hs_len_off, (unsigned int)hs_len);

	*out_len = b.len;
	return 0;
}

struct rbuf {
	const unsigned char *p;
	size_t len;
	size_t off;
};

static int rb_init(struct rbuf *r, const unsigned char *p, size_t len) {
	if (!r || (!p && len)) return -1;
	r->p = p;
	r->len = len;
	r->off = 0;
	return 0;
}

static int rb_need(struct rbuf *r, size_t n) {
	if (!r) return -1;
	return (r->off + n <= r->len) ? 0 : -1;
}

static int rb_get_u8(struct rbuf *r, unsigned char *out) {
	if (!out) return -1;
	if (rb_need(r, 1) != 0) return -1;
	*out = r->p[r->off++];
	return 0;
}

static int rb_get_u16(struct rbuf *r, unsigned short *out) {
	if (!out) return -1;
	if (rb_need(r, 2) != 0) return -1;
	unsigned short v = (unsigned short)((unsigned short)r->p[r->off] << 8);
	v |= (unsigned short)r->p[r->off + 1u];
	r->off += 2;
	*out = v;
	return 0;
}

static int rb_get_u24(struct rbuf *r, unsigned int *out) {
	if (!out) return -1;
	if (rb_need(r, 3) != 0) return -1;
	unsigned int v = 0;
	v |= ((unsigned int)r->p[r->off + 0u] << 16);
	v |= ((unsigned int)r->p[r->off + 1u] << 8);
	v |= ((unsigned int)r->p[r->off + 2u] << 0);
	r->off += 3;
	*out = v;
	return 0;
}

static int rb_get_bytes(struct rbuf *r, unsigned char *out, size_t n) {
	if (!out && n) return -1;
	if (rb_need(r, n) != 0) return -1;
	if (n) memcpy(out, r->p + r->off, n);
	r->off += n;
	return 0;
}

static int rb_skip(struct rbuf *r, size_t n) {
	if (rb_need(r, n) != 0) return -1;
	r->off += n;
	return 0;
}

int tls13_parse_server_hello(
	const unsigned char *msg, size_t msg_len,
	struct Tls13ServerHello *out
) {
	if (!msg || !out) return -1;
	memset(out, 0, sizeof(*out));

	struct rbuf r;
	if (rb_init(&r, msg, msg_len) != 0) return -1;

	unsigned char hs_type = 0;
	unsigned int hs_len = 0;
	if (rb_get_u8(&r, &hs_type) != 0) return -1;
	if (rb_get_u24(&r, &hs_len) != 0) return -1;
	if (hs_type != TLS13_HANDSHAKE_SERVER_HELLO) return -1;
	if (r.off + (size_t)hs_len != r.len) return -1;

	if (rb_get_u16(&r, &out->legacy_version) != 0) return -1;
	if (rb_get_bytes(&r, out->random, 32) != 0) return -1;
	if (rb_get_u8(&r, &out->legacy_session_id_echo_len) != 0) return -1;
	if (rb_skip(&r, (size_t)out->legacy_session_id_echo_len) != 0) return -1;
	if (rb_get_u16(&r, &out->cipher_suite) != 0) return -1;
	if (rb_get_u8(&r, &out->legacy_compression_method) != 0) return -1;

	unsigned short exts_len = 0;
	if (rb_get_u16(&r, &exts_len) != 0) return -1;
	if (rb_need(&r, exts_len) != 0) return -1;
	size_t exts_end = r.off + (size_t)exts_len;

	while (r.off < exts_end) {
		unsigned short ext_type = 0;
		unsigned short ext_len = 0;
		if (rb_get_u16(&r, &ext_type) != 0) return -1;
		if (rb_get_u16(&r, &ext_len) != 0) return -1;
		if (rb_need(&r, ext_len) != 0) return -1;

		size_t ext_start = r.off;
		if (ext_type == TLS13_EXT_SUPPORTED_VERSIONS) {
			unsigned short v = 0;
			if (ext_len != 2) return -1;
			if (rb_get_u16(&r, &v) != 0) return -1;
			out->selected_version = v;
		} else if (ext_type == TLS13_EXT_KEY_SHARE) {
			unsigned short group = 0;
			unsigned short klen = 0;
			if (rb_get_u16(&r, &group) != 0) return -1;
			if (rb_get_u16(&r, &klen) != 0) return -1;
			if (klen > sizeof(out->key_share)) return -1;
			if (rb_get_bytes(&r, out->key_share, klen) != 0) return -1;
			out->key_share_group = group;
			out->key_share_len = klen;
		} else {
			if (rb_skip(&r, ext_len) != 0) return -1;
		}

		// Ensure we consumed exactly ext_len bytes.
		if (r.off != ext_start + (size_t)ext_len) return -1;
	}
	if (r.off != exts_end) return -1;

	return 0;
}
