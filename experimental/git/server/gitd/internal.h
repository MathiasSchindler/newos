#define GITD_REQUEST_HEADER_CAPACITY 16384U
#define GITD_DEFAULT_MAX_BODY_SIZE (64U * 1024U * 1024U)
#define GITD_DEFAULT_MAX_WANTS 1024U
#define GITD_DEFAULT_MAX_HAVES 4096U
#define GITD_DEFAULT_MAX_SHALLOWS 256U
#define GITD_DEFAULT_MAX_REF_PREFIXES 64U
#define GITD_DEFAULT_MAX_COMMANDS 64U
#define GITD_DEFAULT_MAX_OBJECTS 200000U
#define GITD_DEFAULT_MAX_PACK_BYTES (256U * 1024U * 1024U)
#define GITD_MAX_DELTA_BASES 64U
#define GITD_MAX_DELTA_BASE_BYTES (8U * 1024U * 1024U)
#define GITD_DELTA_MIN_COPY 16U
#define GITD_DELTA_SAMPLE_STEP 8U
#define GITD_DELTA_HASH_LIMIT 32768U
#define GITD_DELTA_PROBE_LIMIT 8U
#define GITD_DELTA_SIMILARITY_SAMPLES 64U
#define GITD_DELTA_CANDIDATES 16U
#define GITD_IO_CHUNK 8192U
#define GITD_SIDEBAND_CHUNK 60000U
#define GITD_PACKET_DELIM 1U
#define GITD_PACKET_RESPONSE_END 2U

typedef enum {
    GITD_UPLOAD_COMMAND_NONE = 0,
    GITD_UPLOAD_COMMAND_LS_REFS,
    GITD_UPLOAD_COMMAND_FETCH,
    GITD_UPLOAD_COMMAND_OBJECT_INFO,
    GITD_UPLOAD_COMMAND_BUNDLE_URI,
    GITD_UPLOAD_COMMAND_UNSUPPORTED
} GitdUploadCommand;

typedef struct {
    char bind_host[PLATFORM_NETWORK_TEXT_CAPACITY];
    char repo_root[GIT_PATH_CAPACITY];
    char tls_cert_path[GIT_PATH_CAPACITY];
    char tls_key_path[GIT_PATH_CAPACITY];
    unsigned int port;
    size_t max_body_size;
    size_t max_wants;
    size_t max_haves;
    size_t max_shallows;
    size_t max_ref_prefixes;
    size_t max_commands;
    size_t max_objects;
    size_t max_pack_bytes;
    int quiet;
    int once;
    int read_only;
    int allow_delete_refs;
    int allow_tags;
    int allow_notes;
    int allow_custom_refs;
} GitdOptions;

typedef struct {
    unsigned char *cert_der;
    size_t cert_der_len;
    CryptoRsaPrivateKey rsa_key;
    int enabled;
} GitdTlsConfig;

typedef struct {
    int fd;
    int use_tls;
    int head_only;
    int last_status;
    size_t last_response_bytes;
    Tls13Server tls;
} GitdTransport;

typedef struct {
    char method[8];
    char target[GIT_PATH_CAPACITY];
    char path[GIT_PATH_CAPACITY];
    char query[512];
    char content_type[128];
    char content_encoding[64];
    char git_protocol[128];
    size_t content_length;
    int has_content_length;
} GitdRequest;

typedef struct {
    char name[GIT_REF_CAPACITY];
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char peeled_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int has_peeled;
} GitdRef;

typedef struct {
    GitdRef *refs;
    size_t count;
    size_t capacity;
} GitdRefList;

typedef struct {
    unsigned char old_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char ref_name[GIT_REF_CAPACITY];
} GitdReceiveCommand;

typedef struct {
    GitdReceiveCommand *commands;
    size_t count;
    size_t capacity;
    const unsigned char *pack_data;
    size_t pack_size;
    int report_status;
    int sideband;
    const char *parse_error;
} GitdReceiveRequest;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} GitdStringList;

typedef struct {
    GitdUploadCommand command;
    unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitOidList wants;
    GitOidList haves;
    GitOidList shallow_oids;
    GitOidList object_info_oids;
    GitdStringList ref_prefixes;
    size_t deepen;
    int have_want;
    int done;
    int sideband;
    int filter_blob_none;
    int ls_refs_symrefs;
    int ls_refs_peel;
    int object_info_size;
    const char *parse_error;
    char unsupported_command[64];
} GitdUploadRequest;

typedef struct {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *data;
    size_t size;
} GitdBlobBase;

typedef struct {
    GitdBlobBase items[GITD_MAX_DELTA_BASES];
    size_t count;
    size_t total_bytes;
} GitdBlobBaseList;

typedef struct {
    unsigned int hash;
    size_t offset;
    int used;
} GitdDeltaSlot;

typedef struct {
    RtIoLoop loop;
    GitdOptions options;
    GitdTlsConfig tls_config;
    int listener_fd;
    size_t handled_connections;
} GitdServer;

typedef struct {
    GitdServer *server;
    GitdTransport transport;
    GitBuffer raw;
    GitBuffer body;
    GitdRequest request;
    size_t header_end;
    int saw_header;
} GitdConnection;
