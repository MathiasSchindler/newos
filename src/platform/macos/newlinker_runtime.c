#include <stddef.h>

#include "../../arch/aarch64/macos/syscall.h"
#include "trace.h"

#define MACOS_NEWLINKER_TIOCGETA  1078490131UL
#define MACOS_NEWLINKER_TIOCSETA  2152231956UL
#define MACOS_NEWLINKER_TIOCSETAW 2152231957UL
#define MACOS_NEWLINKER_TIOCSETAF 2152231958UL
#define MACOS_NEWLINKER_F_GETPATH 50
#define MACOS_NEWLINKER_O_RDONLY 0
#define MACOS_NEWLINKER_O_DIRECTORY 0x100000
#define MACOS_NEWLINKER_MAXPATHLEN 1024U
#define MACOS_NEWLINKER_CTL_KERN 1
#define MACOS_NEWLINKER_KERN_HOSTNAME 10
#define MACOS_NEWLINKER_EXPORT __attribute__((visibility("default")))
#define MACOS_NEWLINKER_AF_UNSPEC 0
#define MACOS_NEWLINKER_AF_INET 2
#define MACOS_NEWLINKER_AF_INET6 30
#define MACOS_NEWLINKER_AF_LINK 18
#define MACOS_NEWLINKER_SOCK_STREAM 1
#define MACOS_NEWLINKER_SOCK_DGRAM 2
#define MACOS_NEWLINKER_IPPROTO_TCP 6
#define MACOS_NEWLINKER_IPPROTO_UDP 17
#define MACOS_NEWLINKER_POLLIN 1
#define MACOS_NEWLINKER_DNS_PORT 53U
#define MACOS_NEWLINKER_DNS_TYPE_A 1U
#define MACOS_NEWLINKER_DNS_CLASS_IN 1U
#define MACOS_NEWLINKER_SYS_GETDIRENTRIES64 344
#define MACOS_NEWLINKER_SIOCGIFCONF 3222038820UL
#define MACOS_NEWLINKER_SIOCGIFFLAGS 3223349521UL
#define MACOS_NEWLINKER_SIOCGIFNETMASK 3223349541UL
#define MACOS_NEWLINKER_IFNAMSIZ 16U
#define MACOS_NEWLINKER_ENV_CAPACITY 96U
#define MACOS_NEWLINKER_ENV_STORAGE_SIZE 8192U
#define MACOS_NEWLINKER_ID_FILE_SIZE 8192U
#define MACOS_NEWLINKER_DIR_BUFFER_SIZE 8192U
#define MACOS_NEWLINKER_DIR_COUNT 16U
#define MACOS_NEWLINKER_IFADDR_COUNT 64U

typedef unsigned int macos_newlinker_socklen_t;

/* Keep wrappers visible to the final project linker, but only force LTO
 * materialization for Darwin/libc-shaped names that ld64 otherwise drops. */
#define MACOS_NEWLINKER_RETAIN_EXPORT __attribute__((used, visibility("default")))

struct sockaddr {
	unsigned char sa_len;
	unsigned char sa_family;
	char sa_data[14];
};

struct macos_newlinker_sockaddr_in {
	unsigned char sin_len;
	unsigned char sin_family;
	unsigned short sin_port;
	unsigned char sin_addr[4];
	char sin_zero[8];
};

struct macos_newlinker_sockaddr_in6 {
	unsigned char sin6_len;
	unsigned char sin6_family;
	unsigned short sin6_port;
	unsigned int sin6_flowinfo;
	unsigned char sin6_addr[16];
	unsigned int sin6_scope_id;
};

struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	macos_newlinker_socklen_t ai_addrlen;
	char *ai_canonname;
	struct sockaddr *ai_addr;
	struct addrinfo *ai_next;
};

struct macos_newlinker_pollfd {
	int fd;
	short events;
	short revents;
};

struct macos_newlinker_timeval {
	long tv_sec;
	int tv_usec;
};

struct dirent {
	unsigned long long d_ino;
	unsigned long long d_seekoff;
	unsigned short d_reclen;
	unsigned short d_namlen;
	unsigned char d_type;
	char d_name[1024];
};

struct passwd {
	char *pw_name;
	char *pw_passwd;
	unsigned int pw_uid;
	unsigned int pw_gid;
	long pw_change;
	char *pw_class;
	char *pw_gecos;
	char *pw_dir;
	char *pw_shell;
	long pw_expire;
};

struct group {
	char *gr_name;
	char *gr_passwd;
	unsigned int gr_gid;
	char **gr_mem;
};

struct macos_newlinker_tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
	long tm_gmtoff;
	const char *tm_zone;
};

struct macos_newlinker_dir {
	int fd;
	long base;
	long offset;
	long end;
	int used;
	char buffer[MACOS_NEWLINKER_DIR_BUFFER_SIZE];
	struct dirent entry;
};

struct macos_newlinker_sockaddr_storage {
	unsigned char ss_len;
	unsigned char ss_family;
	char ss_data[126];
};

struct macos_newlinker_ifaddrs {
	struct macos_newlinker_ifaddrs *ifa_next;
	char *ifa_name;
	unsigned int ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_dstaddr;
	void *ifa_data;
};

struct macos_newlinker_ifconf {
	int ifc_len;
	char *ifc_buf;
} __attribute__((packed));

struct macos_newlinker_ifreq {
	char ifr_name[MACOS_NEWLINKER_IFNAMSIZ];
	union {
		struct sockaddr addr;
		short flags;
		int metric;
		char *data;
	} ifr_ifru;
};

static char *macos_newlinker_empty_environment[] = { 0 };
static char *macos_newlinker_environment[MACOS_NEWLINKER_ENV_CAPACITY + 1U];
static char macos_newlinker_environment_storage[MACOS_NEWLINKER_ENV_STORAGE_SIZE];
static size_t macos_newlinker_environment_storage_used;
static int macos_newlinker_environment_ready;
static struct macos_newlinker_dir macos_newlinker_dirs[MACOS_NEWLINKER_DIR_COUNT];
static int macos_newlinker_errno;

MACOS_NEWLINKER_EXPORT __attribute__((nocommon)) char **environ = 0;

int darwin_trace_fd;
int darwin_trace_fd_ready;
static int darwin_trace_pid;

static int macos_newlinker_return_int(long result) {
	if (result < 0) {
		macos_newlinker_errno = (int)(-result);
		return -1;
	}
	return (int)result;
}

static size_t macos_newlinker_strlen(const char *text) {
	size_t length = 0;
	if (text == 0) {
		return 0;
	}
	while (text[length] != '\0') {
		length += 1U;
	}
	return length;
}

static int macos_newlinker_copy_string(char *buffer, size_t buffer_size, const char *text) {
	size_t length = macos_newlinker_strlen(text);
	size_t index;
	if (buffer == 0 || buffer_size == 0U || length + 1U > buffer_size) {
		return -1;
	}
	for (index = 0; index <= length; ++index) {
		buffer[index] = text[index];
	}
	return 0;
}

static void macos_newlinker_zero(void *buffer, size_t size) {
	unsigned char *bytes = (unsigned char *)buffer;
	size_t index;
	for (index = 0; index < size; ++index) {
		bytes[index] = 0;
	}
}

static void macos_newlinker_copy_bytes(void *dst, const void *src, size_t size) {
	unsigned char *out = (unsigned char *)dst;
	const unsigned char *in = (const unsigned char *)src;
	size_t index;
	for (index = 0; index < size; ++index) {
		out[index] = in[index];
	}
}

static unsigned short macos_newlinker_htons(unsigned int value) {
	return (unsigned short)(((value & 0xffU) << 8U) | ((value >> 8U) & 0xffU));
}

static unsigned short macos_newlinker_ntohs(unsigned short value) {
	return macos_newlinker_htons(value);
}

static int macos_newlinker_format_uint(char *buffer, size_t buffer_size, unsigned int value) {
	char tmp[10];
	size_t used = 0;
	size_t index;

	if (buffer == 0 || buffer_size == 0U) {
		return -1;
	}
	if (value == 0U) {
		if (buffer_size < 2U) return -1;
		buffer[0] = '0';
		buffer[1] = '\0';
		return 0;
	}
	while (value != 0U && used < sizeof(tmp)) {
		tmp[used++] = (char)('0' + (value % 10U));
		value /= 10U;
	}
	if (value != 0U || used + 1U > buffer_size) {
		return -1;
	}
	for (index = 0; index < used; ++index) {
		buffer[index] = tmp[used - 1U - index];
	}
	buffer[used] = '\0';
	return 0;
}

static int macos_newlinker_streq(const char *left, const char *right) {
	size_t index = 0;
	if (left == 0 || right == 0) {
		return 0;
	}
	while (left[index] != '\0' && right[index] != '\0') {
		if (left[index] != right[index]) {
			return 0;
		}
		index += 1U;
	}
	return left[index] == '\0' && right[index] == '\0';
}

static int macos_newlinker_has_slash(const char *text) {
	size_t index = 0;
	if (text == 0) {
		return 0;
	}
	while (text[index] != '\0') {
		if (text[index] == '/') {
			return 1;
		}
		index += 1U;
	}
	return 0;
}

static int macos_newlinker_env_name_matches(const char *entry, const char *name) {
	size_t index = 0;
	if (entry == 0 || name == 0 || name[0] == '\0') {
		return 0;
	}
	while (name[index] != '\0') {
		if (entry[index] != name[index]) {
			return 0;
		}
		index += 1U;
	}
	return entry[index] == '=';
}

static int macos_newlinker_valid_env_name(const char *name) {
	size_t index = 0;
	if (name == 0 || name[0] == '\0') {
		return 0;
	}
	while (name[index] != '\0') {
		if (name[index] == '=') {
			return 0;
		}
		index += 1U;
	}
	return 1;
}

static int macos_newlinker_parse_uint_field(const char *text, size_t length, unsigned int *value_out) {
	unsigned int value = 0;
	size_t index;
	if (text == 0 || length == 0U || value_out == 0) {
		return -1;
	}
	for (index = 0; index < length; ++index) {
		if (text[index] < '0' || text[index] > '9') {
			return -1;
		}
		value = value * 10U + (unsigned int)(text[index] - '0');
	}
	*value_out = value;
	return 0;
}

static char *macos_newlinker_env_value(const char *name) {
	size_t index = 0;
	if (environ == 0) {
		return 0;
	}
	while (environ[index] != 0) {
		if (macos_newlinker_env_name_matches(environ[index], name)) {
			return environ[index] + macos_newlinker_strlen(name) + 1U;
		}
		index += 1U;
	}
	return 0;
}

static long macos_newlinker_trace_write_raw(int fd, const void *buffer, size_t count) {
	register long x16 __asm__("x16") = DARWIN_SYS_WRITE;
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long)buffer;
	register long x2 __asm__("x2") = (long)count;

	__asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16) : : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
	return x0;
}

static long macos_newlinker_trace_getpid_raw(void) {
	register long x16 __asm__("x16") = DARWIN_SYS_GETPID;
	register long x0 __asm__("x0");

	__asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "=r"(x0), "+r"(x16) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
	return x0;
}

static unsigned long long macos_newlinker_trace_time_raw(void) {
	struct macos_newlinker_timeval now;
	register long x16 __asm__("x16") = DARWIN_SYS_GETTIMEOFDAY;
	register long x0 __asm__("x0") = (long)&now;
	register long x1 __asm__("x1") = 0;

	__asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x16) : : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
	if (x0 < 0) return 0ULL;
	return ((unsigned long long)now.tv_sec * 1000000000ULL) + ((unsigned long long)now.tv_usec * 1000ULL);
}


static int macos_newlinker_trace_parse_fd(const char *value) {
	unsigned int result = 0;
	size_t index = 0;

	if (value == 0 || value[0] == '\0') return -1;
	while (value[index] != '\0') {
		if (value[index] < '0' || value[index] > '9') return -1;
		result = (result * 10U) + (unsigned int)(value[index] - '0');
		if (result > 1024U * 1024U) return -1;
		index += 1U;
	}
	return (int)result;
}

static int macos_newlinker_trace_fd(void) {
	if (!darwin_trace_fd_ready) {
		darwin_trace_fd_ready = 1;
		darwin_trace_fd = macos_newlinker_trace_parse_fd(macos_newlinker_env_value(MACOS_STRACE_ENV));
	}
	return darwin_trace_fd;
}

static void macos_newlinker_trace_copy_string(MacosStraceRecord *record, unsigned int arg_index, const char *text) {
	size_t index = 0;

	if (record == 0 || text == 0) return;
	while (index < MACOS_STRACE_DECODE_TEXT_CAPACITY && text[index] != '\0') {
		record->decoded[index] = text[index];
		index += 1U;
	}
	record->decoded_arg = arg_index;
	record->decoded_kind = MACOS_STRACE_DECODE_KIND_STRING;
	record->decoded_length = (unsigned int)index;
	record->decoded_truncated = text[index] != '\0';
}

static void macos_newlinker_trace_append_byte(MacosStraceRecord *record, char ch) {
	if (record->decoded_length + 1U >= MACOS_STRACE_DECODE_TEXT_CAPACITY) {
		record->decoded_truncated = 1U;
		return;
	}
	record->decoded[record->decoded_length++] = ch;
}

static void macos_newlinker_trace_append_text(MacosStraceRecord *record, const char *text, size_t length) {
	size_t index;
	for (index = 0U; index < length; ++index) {
		if (record->decoded_truncated) return;
		macos_newlinker_trace_append_byte(record, text[index]);
	}
}

static void macos_newlinker_trace_copy_dir_entries(MacosStraceRecord *record, unsigned int arg_index, const void *data, long length) {
	const char *buffer = (const char *)data;
	size_t offset = 0U;
	size_t total;
	unsigned int count = 0U;

	if (record == 0 || buffer == 0 || length <= 0) return;
	total = (size_t)length;
	record->decoded_arg = arg_index;
	record->decoded_kind = MACOS_STRACE_DECODE_KIND_LIST;
	while (offset + 21U <= total) {
		const struct dirent *entry = (const struct dirent *)(buffer + offset);
		unsigned short reclen = entry->d_reclen;
		unsigned short namlen = entry->d_namlen;
		if (reclen == 0U || offset + (size_t)reclen > total) break;
		if (count != 0U) macos_newlinker_trace_append_byte(record, ',');
		if (namlen > reclen) namlen = 0U;
		macos_newlinker_trace_append_text(record, entry->d_name, namlen);
		count += 1U;
		offset += (size_t)reclen;
		if (record->decoded_truncated) break;
	}
}

static void macos_newlinker_trace_decode(MacosStraceRecord *record) {
	if (record == 0) return;
 switch (record->number) {
	case DARWIN_SYS_OPEN:
	case DARWIN_SYS_ACCESS:
	case DARWIN_SYS_CHDIR:
	case DARWIN_SYS_CHMOD:
	case DARWIN_SYS_CHOWN:
	case DARWIN_SYS_LCHOWN:
	case DARWIN_SYS_MKDIR:
	case DARWIN_SYS_MKFIFO:
	case DARWIN_SYS_MKNOD:
	case DARWIN_SYS_RMDIR:
	case DARWIN_SYS_STAT64:
	case DARWIN_SYS_LSTAT64:
	case DARWIN_SYS_STATFS64:
	case DARWIN_SYS_UNLINK:
	case DARWIN_SYS_UNMOUNT:
	case DARWIN_SYS_UTIMES:
	case DARWIN_SYS_READLINK:
	case DARWIN_SYS_EXECVE:
	 macos_newlinker_trace_copy_string(record, 0U, (const char *)record->args[0]);
	 break;
	case DARWIN_SYS_LINK:
	case DARWIN_SYS_RENAME:
	case DARWIN_SYS_SYMLINK:
	 macos_newlinker_trace_copy_string(record, 0U, (const char *)record->args[0]);
	 break;
	case DARWIN_SYS_SYSCTLBYNAME:
	 macos_newlinker_trace_copy_string(record, 0U, (const char *)record->args[0]);
	 break;
	default:
	 break;
 }
	if (record->entering == 0U && record->result > 0 && record->number == MACOS_NEWLINKER_SYS_GETDIRENTRIES64) {
		macos_newlinker_trace_copy_dir_entries(record, 1U, (const void *)record->args[1], record->result);
	}
}

__attribute__((noinline)) void darwin_trace_record(MacosStraceRecord *record) {
	const char *cursor;
	size_t remaining;
	int fd = macos_newlinker_trace_fd();

	if (fd < 0 || record == 0) return;
	record->magic = MACOS_STRACE_RECORD_MAGIC;
	if (darwin_trace_pid == 0) darwin_trace_pid = (int)macos_newlinker_trace_getpid_raw();
	record->pid = darwin_trace_pid;
	if (record->duration_ns != 0ULL || record->timestamp_ns != 0ULL) {
		record->timestamp_ns = macos_newlinker_trace_time_raw();
	}
	macos_newlinker_trace_decode(record);
	cursor = (const char *)record;
	remaining = sizeof(*record);
	while (remaining > 0U) {
		long written = macos_newlinker_trace_write_raw(fd, cursor, remaining);
		if (written <= 0) {
			darwin_trace_fd = -1;
			return;
		}
		cursor += (size_t)written;
		remaining -= (size_t)written;
	}
}

static int macos_newlinker_prepare_environment(void) {
	size_t index = 0;
	if (macos_newlinker_environment_ready) {
		return 0;
	}
	if (environ != 0) {
		while (environ[index] != 0 && index < MACOS_NEWLINKER_ENV_CAPACITY) {
			macos_newlinker_environment[index] = environ[index];
			index += 1U;
		}
	}
	if (environ != 0 && environ[index] != 0) {
		return -1;
	}
	macos_newlinker_environment[index] = 0;
	environ = macos_newlinker_environment;
	macos_newlinker_environment_ready = 1;
	return 0;
}

static char *macos_newlinker_store_env_entry(const char *name, const char *value) {
	size_t name_length = macos_newlinker_strlen(name);
	size_t value_length = macos_newlinker_strlen(value);
	size_t total = name_length + 1U + value_length + 1U;
	char *entry;
	size_t index;
	if (macos_newlinker_environment_storage_used + total > sizeof(macos_newlinker_environment_storage)) {
		return 0;
	}
	entry = macos_newlinker_environment_storage + macos_newlinker_environment_storage_used;
	macos_newlinker_environment_storage_used += total;
	for (index = 0; index < name_length; ++index) entry[index] = name[index];
	entry[name_length] = '=';
	for (index = 0; index < value_length; ++index) entry[name_length + 1U + index] = value[index];
	entry[total - 1U] = '\0';
	return entry;
}

static int macos_newlinker_read_file(const char *path, char *buffer, size_t buffer_size) {
	long fd;
	long amount;
	if (buffer == 0 || buffer_size == 0U) {
		return -1;
	}
	fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, MACOS_NEWLINKER_O_RDONLY, 0);
	if (fd < 0) {
		return -1;
	}
	amount = darwin_syscall3(DARWIN_SYS_READ, fd, (long)buffer, (long)(buffer_size - 1U));
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, fd);
	if (amount < 0) {
		buffer[0] = '\0';
		return -1;
	}
	buffer[amount] = '\0';
	return 0;
}

static int macos_newlinker_days_in_year(long year) {
	return ((year % 4L == 0L && year % 100L != 0L) || (year % 400L == 0L)) ? 366 : 365;
}

static int macos_newlinker_days_in_month(long year, int month) {
	static const unsigned char days[] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };
	if (month == 1 && macos_newlinker_days_in_year(year) == 366) {
		return 29;
	}
	return days[month];
}

static void macos_newlinker_fill_gmtime(long seconds, struct macos_newlinker_tm *out) {
	long days;
	long rem;
	long year = 1970;
	int month = 0;
	if (seconds < 0) {
		seconds = 0;
	}
	days = seconds / 86400L;
	rem = seconds % 86400L;
	out->tm_hour = (int)(rem / 3600L);
	rem %= 3600L;
	out->tm_min = (int)(rem / 60L);
	out->tm_sec = (int)(rem % 60L);
	out->tm_wday = (int)((days + 4L) % 7L);
	while (days >= macos_newlinker_days_in_year(year)) {
		days -= macos_newlinker_days_in_year(year);
		year += 1L;
	}
	out->tm_yday = (int)days;
	while (month < 11 && days >= macos_newlinker_days_in_month(year, month)) {
		days -= macos_newlinker_days_in_month(year, month);
		month += 1;
	}
	out->tm_year = (int)(year - 1900L);
	out->tm_mon = month;
	out->tm_mday = (int)days + 1;
	out->tm_isdst = 0;
	out->tm_gmtoff = 0;
	out->tm_zone = "UTC";
}

static int macos_newlinker_append_char(char *buffer, size_t buffer_size, size_t *used, char ch) {
	if (buffer == 0 || used == 0 || *used + 1U >= buffer_size) {
		return -1;
	}
	buffer[*used] = ch;
	*used += 1U;
	buffer[*used] = '\0';
	return 0;
}

static int macos_newlinker_append_text(char *buffer, size_t buffer_size, size_t *used, const char *text) {
	size_t index = 0;
	while (text != 0 && text[index] != '\0') {
		if (macos_newlinker_append_char(buffer, buffer_size, used, text[index]) != 0) return -1;
		index += 1U;
	}
	return 0;
}

static int macos_newlinker_append_decimal(char *buffer, size_t buffer_size, size_t *used, unsigned int value, unsigned int width) {
	char tmp[10];
	unsigned int index;
	for (index = 0; index < width && index < sizeof(tmp); ++index) {
		tmp[width - 1U - index] = (char)('0' + (value % 10U));
		value /= 10U;
	}
	for (index = 0; index < width && index < sizeof(tmp); ++index) {
		if (macos_newlinker_append_char(buffer, buffer_size, used, tmp[index]) != 0) return -1;
	}
	return 0;
}

static const char *macos_newlinker_weekday_name(int weekday) {
	volatile int selected = weekday;
	if (selected == 0) return "Sun";
	if (selected == 1) return "Mon";
	if (selected == 2) return "Tue";
	if (selected == 3) return "Wed";
	if (selected == 4) return "Thu";
	if (selected == 5) return "Fri";
	if (selected == 6) return "Sat";
	return "Sun";
}

static const char *macos_newlinker_month_name(int month) {
	volatile int selected = month;
	if (selected == 0) return "Jan";
	if (selected == 1) return "Feb";
	if (selected == 2) return "Mar";
	if (selected == 3) return "Apr";
	if (selected == 4) return "May";
	if (selected == 5) return "Jun";
	if (selected == 6) return "Jul";
	if (selected == 7) return "Aug";
	if (selected == 8) return "Sep";
	if (selected == 9) return "Oct";
	if (selected == 10) return "Nov";
	if (selected == 11) return "Dec";
	return "Jan";
}

static int macos_newlinker_split_colon_line(char *line, char *fields[], size_t field_capacity, size_t *field_count_out) {
	size_t count = 0;
	size_t index = 0;
	if (line == 0 || fields == 0 || field_count_out == 0) {
		return -1;
	}
	fields[count++] = line;
	while (line[index] != '\0') {
		if (line[index] == ':') {
			line[index] = '\0';
			if (count >= field_capacity) {
				return -1;
			}
			fields[count++] = line + index + 1U;
		}
		index += 1U;
	}
	*field_count_out = count;
	return 0;
}

static int macos_newlinker_parse_passwd_line(char *line, const char *name, unsigned int uid, int match_uid, struct passwd *entry) {
	char *fields[10];
	size_t field_count = 0;
	unsigned int parsed_uid;
	unsigned int parsed_gid;
	if (macos_newlinker_split_colon_line(line, fields, 10U, &field_count) != 0 || field_count < 7U) {
		return -1;
	}
	if (macos_newlinker_parse_uint_field(fields[2], macos_newlinker_strlen(fields[2]), &parsed_uid) != 0 ||
	    macos_newlinker_parse_uint_field(fields[3], macos_newlinker_strlen(fields[3]), &parsed_gid) != 0) {
		return -1;
	}
	if ((match_uid && parsed_uid != uid) || (!match_uid && !macos_newlinker_streq(fields[0], name))) {
		return -1;
	}
	entry->pw_name = fields[0];
	entry->pw_passwd = fields[1];
	entry->pw_uid = parsed_uid;
	entry->pw_gid = parsed_gid;
	entry->pw_change = 0;
	entry->pw_class = (char *)"";
	entry->pw_gecos = fields[4];
	entry->pw_dir = fields[5];
	entry->pw_shell = fields[6];
	entry->pw_expire = 0;
	return 0;
}

static struct passwd *macos_newlinker_find_passwd(const char *name, unsigned int uid, int match_uid) {
	static char file_buffer[MACOS_NEWLINKER_ID_FILE_SIZE];
	static struct passwd entry;
	size_t line_start = 0;
	size_t index = 0;
	if (macos_newlinker_read_file("/etc/passwd", file_buffer, sizeof(file_buffer)) != 0) {
		return 0;
	}
	while (file_buffer[index] != '\0') {
		if (file_buffer[index] == '\n') {
			file_buffer[index] = '\0';
			if (file_buffer[line_start] != '#' && macos_newlinker_parse_passwd_line(file_buffer + line_start, name, uid, match_uid, &entry) == 0) {
				return &entry;
			}
			line_start = index + 1U;
		}
		index += 1U;
	}
	if (index > line_start && file_buffer[line_start] != '#' && macos_newlinker_parse_passwd_line(file_buffer + line_start, name, uid, match_uid, &entry) == 0) {
		return &entry;
	}
	return 0;
}

static struct passwd *macos_newlinker_fallback_passwd(const char *name, unsigned int uid, int match_uid) {
	static struct passwd entry;
	static char uid_name[32];
	char *env_user = macos_newlinker_env_value("USER");
	char *env_home = macos_newlinker_env_value("HOME");
	unsigned int current_uid = (unsigned int)darwin_syscall0(DARWIN_SYS_GETUID);
	if (match_uid && uid != current_uid) {
		return 0;
	}
	if (!match_uid && (env_user == 0 || !macos_newlinker_streq(name, env_user))) {
		return 0;
	}
	if (env_user == 0 || env_user[0] == '\0') {
		if (macos_newlinker_format_uint(uid_name, sizeof(uid_name), current_uid) != 0) {
			return 0;
		}
		env_user = uid_name;
	}
	entry.pw_name = env_user;
	entry.pw_passwd = (char *)"*";
	entry.pw_uid = current_uid;
	entry.pw_gid = (unsigned int)darwin_syscall0(DARWIN_SYS_GETGID);
	entry.pw_change = 0;
	entry.pw_class = (char *)"";
	entry.pw_gecos = env_user;
	entry.pw_dir = env_home != 0 ? env_home : (char *)"/";
	entry.pw_shell = (char *)"/bin/sh";
	entry.pw_expire = 0;
	return &entry;
}

static int macos_newlinker_parse_group_line(char *line, const char *name, unsigned int gid, int match_gid, struct group *entry) {
	static char *empty_members[] = { 0 };
	char *fields[5];
	size_t field_count = 0;
	unsigned int parsed_gid;
	if (macos_newlinker_split_colon_line(line, fields, 5U, &field_count) != 0 || field_count < 3U) {
		return -1;
	}
	if (macos_newlinker_parse_uint_field(fields[2], macos_newlinker_strlen(fields[2]), &parsed_gid) != 0) {
		return -1;
	}
	if ((match_gid && parsed_gid != gid) || (!match_gid && !macos_newlinker_streq(fields[0], name))) {
		return -1;
	}
	entry->gr_name = fields[0];
	entry->gr_passwd = fields[1];
	entry->gr_gid = parsed_gid;
	entry->gr_mem = empty_members;
	return 0;
}

static struct group *macos_newlinker_find_group(const char *name, unsigned int gid, int match_gid) {
	static char file_buffer[MACOS_NEWLINKER_ID_FILE_SIZE];
	static struct group entry;
	size_t line_start = 0;
	size_t index = 0;
	if (macos_newlinker_read_file("/etc/group", file_buffer, sizeof(file_buffer)) != 0) {
		return 0;
	}
	while (file_buffer[index] != '\0') {
		if (file_buffer[index] == '\n') {
			file_buffer[index] = '\0';
			if (file_buffer[line_start] != '#' && macos_newlinker_parse_group_line(file_buffer + line_start, name, gid, match_gid, &entry) == 0) {
				return &entry;
			}
			line_start = index + 1U;
		}
		index += 1U;
	}
	if (index > line_start && file_buffer[line_start] != '#' && macos_newlinker_parse_group_line(file_buffer + line_start, name, gid, match_gid, &entry) == 0) {
		return &entry;
	}
	return 0;
}

static size_t macos_newlinker_ifreq_size(const char *record) {
	const struct sockaddr *address = (const struct sockaddr *)(record + MACOS_NEWLINKER_IFNAMSIZ);
	size_t address_length = address->sa_len > sizeof(struct sockaddr) ? address->sa_len : sizeof(struct sockaddr);
	return MACOS_NEWLINKER_IFNAMSIZ + address_length;
}

static unsigned short macos_newlinker_dns_read_u16(const unsigned char *data) {
	return (unsigned short)(((unsigned int)data[0] << 8U) | (unsigned int)data[1]);
}

static int macos_newlinker_parse_uint16(const char *text, unsigned int *value_out) {
	unsigned int value = 0;
	size_t index = 0;

	if (text == 0 || text[0] == '\0' || value_out == 0) {
		return -1;
	}
	while (text[index] >= '0' && text[index] <= '9') {
		value = value * 10U + (unsigned int)(text[index] - '0');
		if (value > 65535U) {
			return -1;
		}
		index += 1U;
	}
	if (text[index] != '\0') {
		return -1;
	}
	*value_out = value;
	return 0;
}

static int macos_newlinker_parse_ipv4(const char *text, unsigned char out[4]) {
	unsigned int part = 0;
	unsigned int part_index = 0;
	int saw_digit = 0;
	size_t index = 0;

	if (text == 0 || text[0] == '\0' || out == 0) {
		return -1;
	}
	for (;;) {
		char ch = text[index];
		if (ch >= '0' && ch <= '9') {
			saw_digit = 1;
			part = part * 10U + (unsigned int)(ch - '0');
			if (part > 255U) {
				return -1;
			}
		} else if (ch == '.' || ch == '\0') {
			if (!saw_digit || part_index >= 4U) {
				return -1;
			}
			out[part_index++] = (unsigned char)part;
			if (ch == '\0') {
				return part_index == 4U ? 0 : -1;
			}
			part = 0;
			saw_digit = 0;
		} else {
			return -1;
		}
		index += 1U;
	}
}

static int macos_newlinker_dns_skip_name(const unsigned char *packet, size_t packet_size, size_t *offset_io) {
	size_t offset;
	size_t jumps = 0;

	if (packet == 0 || offset_io == 0) {
		return -1;
	}
	offset = *offset_io;
	while (offset < packet_size) {
		unsigned char length = packet[offset++];
		if (length == 0U) {
			*offset_io = offset;
			return 0;
		}
		if ((length & 0xc0U) == 0xc0U) {
			if (offset >= packet_size || jumps++ > 8U) {
				return -1;
			}
			offset += 1U;
			*offset_io = offset;
			return 0;
		}
		if ((length & 0xc0U) != 0U || offset + length > packet_size) {
			return -1;
		}
		offset += length;
	}
	return -1;
}

static int macos_newlinker_dns_write_name(unsigned char *packet, size_t packet_size, size_t *offset_io, const char *name) {
	size_t offset;
	size_t label_start = 0;
	size_t index = 0;

	if (packet == 0 || offset_io == 0 || name == 0 || name[0] == '\0') {
		return -1;
	}
	offset = *offset_io;
	for (;;) {
		char ch = name[index];
		if (ch == '.' || ch == '\0') {
			size_t label_length = index - label_start;
			if (label_length == 0U || label_length > 63U || offset + 1U + label_length >= packet_size) {
				return -1;
			}
			packet[offset++] = (unsigned char)label_length;
			macos_newlinker_copy_bytes(packet + offset, name + label_start, label_length);
			offset += label_length;
			label_start = index + 1U;
			if (ch == '\0') {
				if (offset >= packet_size) {
					return -1;
				}
				packet[offset++] = 0U;
				*offset_io = offset;
				return 0;
			}
		}
		index += 1U;
	}
}

static int macos_newlinker_read_resolver(unsigned char resolver[4]) {
	char buffer[1024];
	long fd;
	long amount;
	size_t index = 0;

	if (resolver == 0) {
		return -1;
	}
	fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)"/etc/resolv.conf", 0, 0);
	if (fd < 0) {
		return -1;
	}
	amount = darwin_syscall3(DARWIN_SYS_READ, fd, (long)buffer, (long)(sizeof(buffer) - 1U));
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, fd);
	if (amount <= 0) {
		return -1;
	}
	buffer[amount] = '\0';
	while (buffer[index] != '\0') {
		while (buffer[index] == ' ' || buffer[index] == '\t' || buffer[index] == '\r' || buffer[index] == '\n') {
			index += 1U;
		}
		if (buffer[index] == '#') {
			while (buffer[index] != '\0' && buffer[index] != '\n') {
				index += 1U;
			}
			continue;
		}
		if (buffer[index] == 'n' && buffer[index + 1U] == 'a' && buffer[index + 2U] == 'm' && buffer[index + 3U] == 'e' &&
		    buffer[index + 4U] == 's' && buffer[index + 5U] == 'e' && buffer[index + 6U] == 'r' && buffer[index + 7U] == 'v' &&
		    buffer[index + 8U] == 'e' && buffer[index + 9U] == 'r') {
			char address[32];
			size_t address_length = 0;
			index += 10U;
			while (buffer[index] == ' ' || buffer[index] == '\t') {
				index += 1U;
			}
			while (buffer[index] != '\0' && buffer[index] != ' ' && buffer[index] != '\t' && buffer[index] != '\r' && buffer[index] != '\n' && address_length + 1U < sizeof(address)) {
				address[address_length++] = buffer[index++];
			}
			address[address_length] = '\0';
			if (macos_newlinker_parse_ipv4(address, resolver) == 0) {
				return 0;
			}
		}
		while (buffer[index] != '\0' && buffer[index] != '\n') {
			index += 1U;
		}
	}
	return -1;
}

static int macos_newlinker_dns_query_a(const char *name, unsigned char out[4]) {
	unsigned char resolver[4] = { 1U, 1U, 1U, 1U };
	unsigned char packet[512];
	struct macos_newlinker_sockaddr_in server;
	struct macos_newlinker_pollfd pollfd;
	size_t offset = 12U;
	unsigned int txid = 0x4e4fU;
	unsigned int question;
	unsigned int answer;
	unsigned int answer_count;
	long sock;
	long amount;

	if (name == 0 || out == 0) {
		return -1;
	}
	(void)macos_newlinker_read_resolver(resolver);
	macos_newlinker_zero(packet, sizeof(packet));
	for (question = 0; name[question] != '\0'; ++question) {
		txid = (txid * 33U) ^ (unsigned char)name[question];
	}
	packet[0] = (unsigned char)((txid >> 8U) & 0xffU);
	packet[1] = (unsigned char)(txid & 0xffU);
	packet[2] = 0x01U;
	packet[5] = 0x01U;
	if (macos_newlinker_dns_write_name(packet, sizeof(packet), &offset, name) != 0 || offset + 4U > sizeof(packet)) {
		return -1;
	}
	packet[offset++] = 0U;
	packet[offset++] = MACOS_NEWLINKER_DNS_TYPE_A;
	packet[offset++] = 0U;
	packet[offset++] = MACOS_NEWLINKER_DNS_CLASS_IN;

	sock = darwin_syscall3(DARWIN_SYS_SOCKET, MACOS_NEWLINKER_AF_INET, MACOS_NEWLINKER_SOCK_DGRAM, MACOS_NEWLINKER_IPPROTO_UDP);
	if (sock < 0) {
		return -1;
	}
	macos_newlinker_zero(&server, sizeof(server));
	server.sin_len = (unsigned char)sizeof(server);
	server.sin_family = MACOS_NEWLINKER_AF_INET;
	server.sin_port = macos_newlinker_htons(MACOS_NEWLINKER_DNS_PORT);
	macos_newlinker_copy_bytes(server.sin_addr, resolver, sizeof(resolver));
	amount = darwin_syscall6(DARWIN_SYS_SENDTO, sock, (long)packet, (long)offset, 0, (long)&server, (long)sizeof(server));
	if (amount != (long)offset) {
		(void)darwin_syscall1(DARWIN_SYS_CLOSE, sock);
		return -1;
	}
	pollfd.fd = (int)sock;
	pollfd.events = MACOS_NEWLINKER_POLLIN;
	pollfd.revents = 0;
	if (darwin_syscall3(DARWIN_SYS_POLL, (long)&pollfd, 1, 3000) <= 0 || (pollfd.revents & MACOS_NEWLINKER_POLLIN) == 0) {
		(void)darwin_syscall1(DARWIN_SYS_CLOSE, sock);
		return -1;
	}
	amount = darwin_syscall6(DARWIN_SYS_RECVFROM, sock, (long)packet, (long)sizeof(packet), 0, 0, 0);
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, sock);
	if (amount < 12 || packet[0] != (unsigned char)((txid >> 8U) & 0xffU) || packet[1] != (unsigned char)(txid & 0xffU) || (packet[3] & 0x0fU) != 0U) {
		return -1;
	}
	question = macos_newlinker_dns_read_u16(packet + 4U);
	answer_count = macos_newlinker_dns_read_u16(packet + 6U);
	offset = 12U;
	for (; question > 0U; --question) {
		if (macos_newlinker_dns_skip_name(packet, (size_t)amount, &offset) != 0 || offset + 4U > (size_t)amount) {
			return -1;
		}
		offset += 4U;
	}
	for (answer = 0; answer < answer_count; ++answer) {
		unsigned int type;
		unsigned int klass;
		unsigned int length;
		if (macos_newlinker_dns_skip_name(packet, (size_t)amount, &offset) != 0 || offset + 10U > (size_t)amount) {
			return -1;
		}
		type = macos_newlinker_dns_read_u16(packet + offset);
		klass = macos_newlinker_dns_read_u16(packet + offset + 2U);
		length = macos_newlinker_dns_read_u16(packet + offset + 8U);
		offset += 10U;
		if (offset + length > (size_t)amount) {
			return -1;
		}
		if (type == MACOS_NEWLINKER_DNS_TYPE_A && klass == MACOS_NEWLINKER_DNS_CLASS_IN && length == 4U) {
			macos_newlinker_copy_bytes(out, packet + offset, 4U);
			return 0;
		}
		offset += length;
	}
	return -1;
}

static int macos_newlinker_fill_addrinfo4(const unsigned char address[4], unsigned int port, const void *hints, struct addrinfo **result) {
	static struct addrinfo entry;
	static struct macos_newlinker_sockaddr_in socket_address;
	const struct addrinfo *requested = (const struct addrinfo *)hints;

	if (address == 0 || result == 0) {
		return -1;
	}
	macos_newlinker_zero(&entry, sizeof(entry));
	macos_newlinker_zero(&socket_address, sizeof(socket_address));
	socket_address.sin_len = (unsigned char)sizeof(socket_address);
	socket_address.sin_family = MACOS_NEWLINKER_AF_INET;
	socket_address.sin_port = macos_newlinker_htons(port);
	macos_newlinker_copy_bytes(socket_address.sin_addr, address, 4U);
	entry.ai_family = MACOS_NEWLINKER_AF_INET;
	entry.ai_socktype = requested != 0 && requested->ai_socktype != 0 ? requested->ai_socktype : MACOS_NEWLINKER_SOCK_STREAM;
	entry.ai_protocol = requested != 0 && requested->ai_protocol != 0 ? requested->ai_protocol : MACOS_NEWLINKER_IPPROTO_TCP;
	entry.ai_addrlen = (macos_newlinker_socklen_t)sizeof(socket_address);
	entry.ai_addr = (struct sockaddr *)&socket_address;
	entry.ai_next = 0;
	*result = &entry;
	return 0;
}

static int macos_newlinker_fill_addrinfo6_loopback(unsigned int port, const void *hints, struct addrinfo **result) {
	static struct addrinfo entry;
	static struct macos_newlinker_sockaddr_in6 socket_address;
	const struct addrinfo *requested = (const struct addrinfo *)hints;

	if (result == 0) {
		return -1;
	}
	macos_newlinker_zero(&entry, sizeof(entry));
	macos_newlinker_zero(&socket_address, sizeof(socket_address));
	socket_address.sin6_len = (unsigned char)sizeof(socket_address);
	socket_address.sin6_family = MACOS_NEWLINKER_AF_INET6;
	socket_address.sin6_port = macos_newlinker_htons(port);
	socket_address.sin6_addr[15] = 1U;
	entry.ai_family = MACOS_NEWLINKER_AF_INET6;
	entry.ai_socktype = requested != 0 && requested->ai_socktype != 0 ? requested->ai_socktype : MACOS_NEWLINKER_SOCK_STREAM;
	entry.ai_protocol = requested != 0 && requested->ai_protocol != 0 ? requested->ai_protocol : MACOS_NEWLINKER_IPPROTO_TCP;
	entry.ai_addrlen = (macos_newlinker_socklen_t)sizeof(socket_address);
	entry.ai_addr = (struct sockaddr *)&socket_address;
	entry.ai_next = 0;
	*result = &entry;
	return 0;
}

MACOS_NEWLINKER_EXPORT int *__error(void) {
	(void)environ;
	return &macos_newlinker_errno;
}

MACOS_NEWLINKER_EXPORT long sysconf(int name) {
	if (name == 29) {
		return 16384L;
	}
	return -1L;
}

MACOS_NEWLINKER_EXPORT int gettimeofday(void *time_value, void *time_zone) {
	return darwin_syscall2(DARWIN_SYS_GETTIMEOFDAY, (long)time_value, (long)time_zone) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT void _exit(int status) {
	(void)darwin_syscall1(DARWIN_SYS_EXIT, (long)status);
	for (;;) {}
}

MACOS_NEWLINKER_EXPORT int rename(const char *old_path, const char *new_path) {
	return darwin_syscall2(DARWIN_SYS_RENAME, (long)old_path, (long)new_path) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT char *getcwd(char *buffer, size_t buffer_size) {
	char path[MACOS_NEWLINKER_MAXPATHLEN];
	long fd;
	long result;

	fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)".", 0, 0);
	if (fd < 0) {
		return 0;
	}
	result = darwin_syscall3(DARWIN_SYS_FCNTL, fd, MACOS_NEWLINKER_F_GETPATH, (long)path);
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, fd);
	if (result < 0 || macos_newlinker_copy_string(buffer, buffer_size, path) != 0) {
		return 0;
	}
	return buffer;
}

MACOS_NEWLINKER_EXPORT long readlink(const char *path, char *buffer, size_t buffer_size) {
	return darwin_syscall3(DARWIN_SYS_READLINK, (long)path, (long)buffer, (long)buffer_size);
}

MACOS_NEWLINKER_RETAIN_EXPORT int kill(int pid, int signal_number) {
	return darwin_syscall2(DARWIN_SYS_KILL, (long)pid, (long)signal_number) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int getentropy(void *buffer, size_t size) {
	return darwin_syscall2(DARWIN_SYS_GETENTROPY, (long)buffer, (long)size) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int fstat(int fd, void *stat_info) {
	return darwin_syscall2(DARWIN_SYS_FSTAT64, (long)fd, (long)stat_info) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int fork(void) {
	register long x16 __asm__("x16") = DARWIN_SYS_FORK;
	register long x0 __asm__("x0");
	register long x1 __asm__("x1");

	__asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "=r"(x0), "=r"(x1), "+r"(x16) : : "memory", "cc");
	if (x0 < 0) {
		return (int)x0;
	}
	return x1 != 0 ? 0 : (int)x0;
}

MACOS_NEWLINKER_EXPORT int dup2(int old_fd, int new_fd) {
	return (int)darwin_syscall2(DARWIN_SYS_DUP2, (long)old_fd, (long)new_fd);
}

MACOS_NEWLINKER_RETAIN_EXPORT int wait4(int pid, int *status, int options, void *usage) {
	long result = darwin_syscall4(DARWIN_SYS_WAIT4, (long)pid, (long)status, (long)options, (long)usage);
	if (result < 0 && pid > 0) {
		result = darwin_syscall4(DARWIN_SYS_WAIT4, -1, (long)status, (long)options, (long)usage);
	}
	return (int)result;
}

MACOS_NEWLINKER_RETAIN_EXPORT int waitpid(int pid, int *status, int options) {
	return wait4(pid, status, options, 0);
}

MACOS_NEWLINKER_EXPORT int chdir(const char *path) {
	return darwin_syscall1(DARWIN_SYS_CHDIR, (long)path) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int chmod(const char *path, unsigned int mode) {
	return darwin_syscall2(DARWIN_SYS_CHMOD, (long)path, (long)mode) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int chown(const char *path, unsigned int uid, unsigned int gid) {
	return darwin_syscall3(DARWIN_SYS_CHOWN, (long)path, (long)uid, (long)gid) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int link(const char *old_path, const char *new_path) {
	return darwin_syscall2(DARWIN_SYS_LINK, (long)old_path, (long)new_path) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int symlink(const char *old_path, const char *new_path) {
	return darwin_syscall2(DARWIN_SYS_SYMLINK, (long)old_path, (long)new_path) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int lchown(const char *path, unsigned int uid, unsigned int gid) {
	return darwin_syscall3(DARWIN_SYS_LCHOWN, (long)path, (long)uid, (long)gid) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int sysctl(int *name, unsigned int name_count, void *old_value, size_t *old_size, void *new_value, size_t new_size) {
	return darwin_syscall6(DARWIN_SYS_SYSCTL, (long)name, (long)name_count, (long)old_value, (long)old_size, (long)new_value, (long)new_size) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int sysctlbyname(const char *name, void *old_value, size_t *old_size, void *new_value, size_t new_size) {
	return darwin_syscall6(DARWIN_SYS_SYSCTLBYNAME, (long)name, (long)macos_newlinker_strlen(name), (long)old_value, (long)old_size, (long)new_value, (long)new_size) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int gethostname(char *name, size_t name_size) {
	int mib[2] = { MACOS_NEWLINKER_CTL_KERN, MACOS_NEWLINKER_KERN_HOSTNAME };
	size_t old_size = name_size;
	return sysctl(mib, 2, name, &old_size, 0, 0);
}

MACOS_NEWLINKER_EXPORT int sethostname(const char *name, int name_length) {
	int mib[2] = { MACOS_NEWLINKER_CTL_KERN, MACOS_NEWLINKER_KERN_HOSTNAME };
	return sysctl(mib, 2, 0, 0, (void *)name, (size_t)name_length);
}

MACOS_NEWLINKER_EXPORT int mkfifo(const char *path, unsigned int mode) {
	return darwin_syscall2(DARWIN_SYS_MKFIFO, (long)path, (long)mode) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int unmount(const char *path, int flags) {
	return darwin_syscall2(DARWIN_SYS_UNMOUNT, (long)path, (long)flags) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int mknod(const char *path, unsigned int mode, unsigned int device) {
	return darwin_syscall3(DARWIN_SYS_MKNOD, (long)path, (long)mode, (long)device) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_RETAIN_EXPORT int statfs(const char *path, void *buffer) {
	return darwin_syscall2(DARWIN_SYS_STATFS64, (long)path, (long)buffer) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_RETAIN_EXPORT int listen(int fd, int backlog) {
	return darwin_syscall2(DARWIN_SYS_LISTEN, (long)fd, (long)backlog) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int shutdown(int fd, int how) {
	return darwin_syscall2(DARWIN_SYS_SHUTDOWN, (long)fd, (long)how) < 0 ? -1 : 0;
}

MACOS_NEWLINKER_EXPORT int getsockopt(int fd, int level, int option_name, void *option_value, unsigned int *option_length) {
	long result = darwin_syscall5(DARWIN_SYS_GETSOCKOPT, (long)fd, (long)level, (long)option_name, (long)option_value, (long)option_length);
	return macos_newlinker_return_int(result);
}

MACOS_NEWLINKER_EXPORT int socket(int domain, int type, int protocol) {
	return macos_newlinker_return_int(darwin_syscall3(DARWIN_SYS_SOCKET, (long)domain, (long)type, (long)protocol));
}

MACOS_NEWLINKER_RETAIN_EXPORT int connect(int fd, const void *address, unsigned int address_length) {
	long result = darwin_syscall3(DARWIN_SYS_CONNECT, (long)fd, (long)address, (long)address_length);
	return macos_newlinker_return_int(result);
}

MACOS_NEWLINKER_RETAIN_EXPORT int bind(int fd, const void *address, unsigned int address_length) {
	long result = darwin_syscall3(DARWIN_SYS_BIND, (long)fd, (long)address, (long)address_length);
	return macos_newlinker_return_int(result);
}

MACOS_NEWLINKER_RETAIN_EXPORT int accept(int fd, void *address, unsigned int *address_length) {
	return (int)darwin_syscall3(DARWIN_SYS_ACCEPT, (long)fd, (long)address, (long)address_length);
}

MACOS_NEWLINKER_EXPORT int setsockopt(int fd, int level, int option_name, const void *option_value, unsigned int option_length) {
	long result = darwin_syscall5(DARWIN_SYS_SETSOCKOPT, (long)fd, (long)level, (long)option_name, (long)option_value, (long)option_length);
	return macos_newlinker_return_int(result);
}

MACOS_NEWLINKER_RETAIN_EXPORT long recv(int fd, void *buffer, size_t length, int flags) {
	return darwin_syscall6(DARWIN_SYS_RECVFROM, (long)fd, (long)buffer, (long)length, (long)flags, 0, 0);
}

MACOS_NEWLINKER_RETAIN_EXPORT long recvfrom(int fd, void *buffer, size_t length, int flags, void *address, unsigned int *address_length) {
	return darwin_syscall6(DARWIN_SYS_RECVFROM, (long)fd, (long)buffer, (long)length, (long)flags, (long)address, (long)address_length);
}

MACOS_NEWLINKER_RETAIN_EXPORT long sendto(int fd, const void *buffer, size_t length, int flags, const void *address, unsigned int address_length) {
	return darwin_syscall6(DARWIN_SYS_SENDTO, (long)fd, (long)buffer, (long)length, (long)flags, (long)address, (long)address_length);
}

MACOS_NEWLINKER_RETAIN_EXPORT long send(int fd, const void *buffer, size_t length, int flags) {
	return sendto(fd, buffer, length, flags, 0, 0);
}

MACOS_NEWLINKER_EXPORT int pipe(int fds[2]) {
	register long x16 __asm__("x16") = DARWIN_SYS_PIPE;
	register long x0 __asm__("x0");
	register long x1 __asm__("x1");

	__asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "=r"(x0), "=r"(x1), "+r"(x16) : : "memory", "cc");
	if (x0 < 0 || fds == 0) {
		return -1;
	}
	fds[0] = (int)x0;
	fds[1] = (int)x1;
	return 0;
}

static int macos_newlinker_execve_raw(const char *file, char *const argv[], char *const environment[]) {
	long result = darwin_syscall3(DARWIN_SYS_EXECVE, (long)file, (long)argv, (long)environment);
	if (result < 0) {
		macos_newlinker_errno = (int)(-result);
		return -1;
	}
	return 0;
}

MACOS_NEWLINKER_EXPORT int execvp(const char *file, char *const argv[]) {
	char **environment = environ != 0 ? environ : macos_newlinker_empty_environment;
	const char *path;
	size_t file_length;
	size_t path_index = 0U;

	if (file == 0 || file[0] == '\0' || argv == 0) {
		macos_newlinker_errno = 2;
		return -1;
	}
	if (macos_newlinker_has_slash(file)) {
		return macos_newlinker_execve_raw(file, argv, environment);
	}

	path = "/bin:/usr/bin";
	file_length = macos_newlinker_strlen(file);

	for (;;) {
		char candidate[MACOS_NEWLINKER_MAXPATHLEN];
		size_t component_start = path_index;
		size_t component_length;
		size_t used = 0U;
		size_t i;

		while (path[path_index] != '\0' && path[path_index] != ':') {
			path_index += 1U;
		}
		component_length = path_index - component_start;

		if ((component_length == 0U ? 1U : component_length) + 1U + file_length + 1U <= sizeof(candidate)) {
			if (component_length == 0U) {
				candidate[used++] = '.';
			} else {
				for (i = 0U; i < component_length; ++i) {
					candidate[used++] = path[component_start + i];
				}
			}
			if (used > 0U && candidate[used - 1U] != '/') {
				candidate[used++] = '/';
			}
			for (i = 0U; i < file_length; ++i) {
				candidate[used++] = file[i];
			}
			candidate[used] = '\0';
			(void)macos_newlinker_execve_raw(candidate, argv, environment);
		}

		if (path[path_index] == '\0') {
			break;
		}
		path_index += 1U;
	}
	return -1;
}

MACOS_NEWLINKER_EXPORT int setenv(const char *name, const char *value, int overwrite) {
	char *entry;
	size_t index = 0;
	if (!macos_newlinker_valid_env_name(name)) {
		return -1;
	}
	if (value == 0) {
		value = "";
	}
	if (macos_newlinker_prepare_environment() != 0) {
		return -1;
	}
	while (environ[index] != 0) {
		if (macos_newlinker_env_name_matches(environ[index], name)) {
			if (!overwrite) {
				return 0;
			}
			entry = macos_newlinker_store_env_entry(name, value);
			if (entry == 0) {
				return -1;
			}
			environ[index] = entry;
			return 0;
		}
		index += 1U;
	}
	if (index >= MACOS_NEWLINKER_ENV_CAPACITY) {
		return -1;
	}
	entry = macos_newlinker_store_env_entry(name, value);
	if (entry == 0) {
		return -1;
	}
	environ[index] = entry;
	environ[index + 1U] = 0;
	return 0;
}

MACOS_NEWLINKER_EXPORT int unsetenv(const char *name) {
	size_t index = 0;
	size_t write_index = 0;
	if (!macos_newlinker_valid_env_name(name)) {
		return -1;
	}
	if (macos_newlinker_prepare_environment() != 0) {
		return -1;
	}
	while (environ[index] != 0) {
		if (!macos_newlinker_env_name_matches(environ[index], name)) {
			environ[write_index++] = environ[index];
		}
		index += 1U;
	}
	environ[write_index] = 0;
	return 0;
}

MACOS_NEWLINKER_EXPORT void *getgrgid(unsigned int gid) {
	return macos_newlinker_find_group(0, gid, 1);
}

MACOS_NEWLINKER_EXPORT void *getpwuid(unsigned int uid) {
	struct passwd *entry = macos_newlinker_find_passwd(0, uid, 1);
	return entry != 0 ? entry : macos_newlinker_fallback_passwd(0, uid, 1);
}

MACOS_NEWLINKER_EXPORT void *getpwnam(const char *name) {
	struct passwd *entry = macos_newlinker_find_passwd(name, 0, 0);
	return entry != 0 ? entry : macos_newlinker_fallback_passwd(name, 0, 0);
}

MACOS_NEWLINKER_EXPORT void *getgrnam(const char *name) {
	return macos_newlinker_find_group(name, 0, 0);
}

MACOS_NEWLINKER_EXPORT int getaddrinfo(const char *node, const char *service, const void *hints, void **result) {
	unsigned int port = 0;
	unsigned char address[4];
	const struct addrinfo *requested = (const struct addrinfo *)hints;

	if (result != 0) {
		*result = 0;
	}
	if (result == 0) {
		return -1;
	}
	if (service != 0 && service[0] != '\0' && macos_newlinker_parse_uint16(service, &port) != 0) {
		return -1;
	}
	if (requested != 0 && requested->ai_family != MACOS_NEWLINKER_AF_UNSPEC &&
	    requested->ai_family != MACOS_NEWLINKER_AF_INET && requested->ai_family != MACOS_NEWLINKER_AF_INET6) {
		return -1;
	}
	if (requested != 0 && requested->ai_socktype != 0 && requested->ai_socktype != MACOS_NEWLINKER_SOCK_STREAM && requested->ai_socktype != MACOS_NEWLINKER_SOCK_DGRAM) {
		return -1;
	}
	if (requested != 0 && requested->ai_family == MACOS_NEWLINKER_AF_INET6) {
		if (node == 0 || node[0] == '\0' || macos_newlinker_streq(node, "::1") || macos_newlinker_streq(node, "localhost")) {
			return macos_newlinker_fill_addrinfo6_loopback(port, hints, (struct addrinfo **)result);
		}
		return -1;
	}
	if (node == 0 || node[0] == '\0') {
		address[0] = 0;
		address[1] = 0;
		address[2] = 0;
		address[3] = 0;
	} else if (macos_newlinker_streq(node, "localhost")) {
		address[0] = 127U;
		address[1] = 0;
		address[2] = 0;
		address[3] = 1U;
	} else if (macos_newlinker_parse_ipv4(node, address) != 0 && macos_newlinker_dns_query_a(node, address) != 0) {
		return -1;
	}
	return macos_newlinker_fill_addrinfo4(address, port, hints, (struct addrinfo **)result);
}

MACOS_NEWLINKER_EXPORT void freeaddrinfo(void *address_info) {
	(void)address_info;
}

MACOS_NEWLINKER_EXPORT void freeifaddrs(void *addresses) {
	(void)addresses;
}

MACOS_NEWLINKER_EXPORT int getifaddrs(void **addresses) {
	static struct macos_newlinker_ifaddrs entries[MACOS_NEWLINKER_IFADDR_COUNT];
	static struct macos_newlinker_sockaddr_storage addr_storage[MACOS_NEWLINKER_IFADDR_COUNT];
	static struct macos_newlinker_sockaddr_storage mask_storage[MACOS_NEWLINKER_IFADDR_COUNT];
	static char names[MACOS_NEWLINKER_IFADDR_COUNT][MACOS_NEWLINKER_IFNAMSIZ];
	char ifc_buffer[4096];
	struct macos_newlinker_ifconf config;
	int sock;
	int count = 0;
	size_t offset = 0;
	if (addresses != 0) {
		*addresses = 0;
	}
	if (addresses == 0) {
		return -1;
	}
	sock = socket(MACOS_NEWLINKER_AF_INET, MACOS_NEWLINKER_SOCK_DGRAM, 0);
	if (sock < 0) {
		return -1;
	}
	config.ifc_len = (int)sizeof(ifc_buffer);
	config.ifc_buf = ifc_buffer;
	if (darwin_syscall3(DARWIN_SYS_IOCTL, (long)sock, (long)MACOS_NEWLINKER_SIOCGIFCONF, (long)&config) < 0) {
		(void)darwin_syscall1(DARWIN_SYS_CLOSE, sock);
		return -1;
	}
	macos_newlinker_zero(entries, sizeof(entries));
	macos_newlinker_zero(addr_storage, sizeof(addr_storage));
	macos_newlinker_zero(mask_storage, sizeof(mask_storage));
	while (offset + sizeof(struct macos_newlinker_ifreq) <= (size_t)config.ifc_len && count < (int)MACOS_NEWLINKER_IFADDR_COUNT) {
		struct macos_newlinker_ifreq *record = (struct macos_newlinker_ifreq *)(ifc_buffer + offset);
		size_t record_size = macos_newlinker_ifreq_size(ifc_buffer + offset);
		struct macos_newlinker_ifreq request;
		if (record_size == 0U || offset + record_size > (size_t)config.ifc_len) {
			break;
		}
		macos_newlinker_copy_string(names[count], sizeof(names[count]), record->ifr_name);
		entries[count].ifa_name = names[count];
		macos_newlinker_copy_bytes(&addr_storage[count], &record->ifr_ifru.addr, record->ifr_ifru.addr.sa_len > sizeof(struct sockaddr) ? record->ifr_ifru.addr.sa_len : sizeof(struct sockaddr));
		entries[count].ifa_addr = (struct sockaddr *)&addr_storage[count];
		macos_newlinker_zero(&request, sizeof(request));
		macos_newlinker_copy_string(request.ifr_name, sizeof(request.ifr_name), record->ifr_name);
		if (darwin_syscall3(DARWIN_SYS_IOCTL, (long)sock, (long)MACOS_NEWLINKER_SIOCGIFFLAGS, (long)&request) >= 0) {
			entries[count].ifa_flags = (unsigned int)(unsigned short)request.ifr_ifru.flags;
		}
		macos_newlinker_zero(&request, sizeof(request));
		macos_newlinker_copy_string(request.ifr_name, sizeof(request.ifr_name), record->ifr_name);
		if (record->ifr_ifru.addr.sa_family == MACOS_NEWLINKER_AF_INET && darwin_syscall3(DARWIN_SYS_IOCTL, (long)sock, (long)MACOS_NEWLINKER_SIOCGIFNETMASK, (long)&request) >= 0) {
			macos_newlinker_copy_bytes(&mask_storage[count], &request.ifr_ifru.addr, request.ifr_ifru.addr.sa_len > sizeof(struct sockaddr) ? request.ifr_ifru.addr.sa_len : sizeof(struct sockaddr));
			entries[count].ifa_netmask = (struct sockaddr *)&mask_storage[count];
		}
		if (count > 0) {
			entries[count - 1].ifa_next = &entries[count];
		}
		count += 1;
		offset += record_size;
	}
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, sock);
	*addresses = count > 0 ? entries : 0;
	return 0;
}

MACOS_NEWLINKER_EXPORT unsigned int if_nametoindex(const char *name) {
	void *addresses = 0;
	struct macos_newlinker_ifaddrs *current;
	char seen[MACOS_NEWLINKER_IFADDR_COUNT][MACOS_NEWLINKER_IFNAMSIZ];
	unsigned int seen_count = 0;
	if (name == 0 || getifaddrs(&addresses) != 0) {
		return 0;
	}
	current = (struct macos_newlinker_ifaddrs *)addresses;
	while (current != 0) {
		unsigned int index;
		int known = 0;
		for (index = 0; index < seen_count; ++index) {
			if (macos_newlinker_streq(seen[index], current->ifa_name)) {
				known = 1;
				break;
			}
		}
		if (!known && seen_count < MACOS_NEWLINKER_IFADDR_COUNT) {
			macos_newlinker_copy_string(seen[seen_count], sizeof(seen[seen_count]), current->ifa_name);
			seen_count += 1U;
			if (macos_newlinker_streq(name, current->ifa_name)) {
				return seen_count;
			}
		}
		current = current->ifa_next;
	}
	return 0;
}

MACOS_NEWLINKER_EXPORT char *inet_ntop(int family, const void *address, char *buffer, unsigned int buffer_size) {
	const unsigned char *bytes = (const unsigned char *)address;
	size_t length = 0;
	char number[4];
	unsigned int part;
	unsigned int index;

	if (address == 0 || buffer == 0 || buffer_size == 0U) {
		return 0;
	}
	if (family == MACOS_NEWLINKER_AF_INET6) {
		int loopback = 1;
		for (index = 0; index < 15U; ++index) {
			if (bytes[index] != 0U) {
				loopback = 0;
				break;
			}
		}
		if (loopback && bytes[15] == 1U) {
			return macos_newlinker_copy_string(buffer, buffer_size, "::1") == 0 ? buffer : 0;
		}
		return 0;
	}
	if (family != MACOS_NEWLINKER_AF_INET) {
		return 0;
	}
	buffer[0] = '\0';
	for (index = 0; index < 4U; ++index) {
		if (index != 0U) {
			if (length + 1U >= buffer_size) return 0;
			buffer[length++] = '.';
			buffer[length] = '\0';
		}
		part = bytes[index];
		number[0] = (char)('0' + (part / 100U));
		number[1] = (char)('0' + ((part / 10U) % 10U));
		number[2] = (char)('0' + (part % 10U));
		number[3] = '\0';
		if (part >= 100U) {
			if (macos_newlinker_copy_string(buffer + length, buffer_size - length, number) != 0) return 0;
			length += 3U;
		} else if (part >= 10U) {
			if (macos_newlinker_copy_string(buffer + length, buffer_size - length, number + 1U) != 0) return 0;
			length += 2U;
		} else {
			if (length + 1U >= buffer_size) return 0;
			buffer[length++] = number[2];
			buffer[length] = '\0';
		}
	}
	return buffer;
}

MACOS_NEWLINKER_EXPORT int getnameinfo(const struct sockaddr *address, macos_newlinker_socklen_t address_length, char *host, macos_newlinker_socklen_t host_length, char *service, macos_newlinker_socklen_t service_length, int flags) {
	const struct macos_newlinker_sockaddr_in *ipv4 = (const struct macos_newlinker_sockaddr_in *)address;
	(void)flags;

	if (address == 0 || address->sa_family != MACOS_NEWLINKER_AF_INET || address_length < sizeof(*ipv4)) {
		return -1;
	}
	if (host != 0 && host_length != 0U) {
		if (inet_ntop(MACOS_NEWLINKER_AF_INET, ipv4->sin_addr, host, host_length) == 0) {
			return -1;
		}
	}
	if (service != 0 && service_length != 0U) {
		if (macos_newlinker_format_uint(service, service_length, (unsigned int)macos_newlinker_ntohs(ipv4->sin_port)) != 0) {
			return -1;
		}
	}
	return 0;
}

MACOS_NEWLINKER_EXPORT int inet_pton(int family, const char *text, void *address) {
	return family == MACOS_NEWLINKER_AF_INET && macos_newlinker_parse_ipv4(text, (unsigned char *)address) == 0 ? 1 : 0;
}

MACOS_NEWLINKER_RETAIN_EXPORT size_t strftime(char *buffer, size_t buffer_size, const char *format, const void *time_value) {
	const struct macos_newlinker_tm *time_info = (const struct macos_newlinker_tm *)time_value;
	size_t used = 0;
	size_t index = 0;
	if (buffer == 0 || buffer_size == 0U || format == 0 || time_info == 0) {
		return 0;
	}
	buffer[0] = '\0';
	while (format[index] != '\0') {
		if (format[index] != '%') {
			if (macos_newlinker_append_char(buffer, buffer_size, &used, format[index]) != 0) return 0;
			index += 1U;
			continue;
		}
		index += 1U;
		if (format[index] == '\0') {
			return 0;
		}
		switch (format[index]) {
			case '%': if (macos_newlinker_append_char(buffer, buffer_size, &used, '%') != 0) return 0; break;
			case 'Y': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)(time_info->tm_year + 1900), 4U) != 0) return 0; break;
			case 'y': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)((time_info->tm_year + 1900) % 100), 2U) != 0) return 0; break;
			case 'm': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)(time_info->tm_mon + 1), 2U) != 0) return 0; break;
			case 'd': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)time_info->tm_mday, 2U) != 0) return 0; break;
			case 'H': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)time_info->tm_hour, 2U) != 0) return 0; break;
			case 'M': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)time_info->tm_min, 2U) != 0) return 0; break;
			case 'S': if (macos_newlinker_append_decimal(buffer, buffer_size, &used, (unsigned int)time_info->tm_sec, 2U) != 0) return 0; break;
			case 'F': if (strftime(buffer + used, buffer_size - used, "%Y-%m-%d", time_info) == 0) return 0; used = macos_newlinker_strlen(buffer); break;
			case 'T': if (strftime(buffer + used, buffer_size - used, "%H:%M:%S", time_info) == 0) return 0; used = macos_newlinker_strlen(buffer); break;
			case 'a': if (macos_newlinker_append_text(buffer, buffer_size, &used, macos_newlinker_weekday_name(time_info->tm_wday)) != 0) return 0; break;
			case 'b': if (macos_newlinker_append_text(buffer, buffer_size, &used, macos_newlinker_month_name(time_info->tm_mon)) != 0) return 0; break;
			case 'z': if (macos_newlinker_append_text(buffer, buffer_size, &used, "+0000") != 0) return 0; break;
			case 'Z': if (macos_newlinker_append_text(buffer, buffer_size, &used, "UTC") != 0) return 0; break;
			default:
				if (macos_newlinker_append_char(buffer, buffer_size, &used, '%') != 0 || macos_newlinker_append_char(buffer, buffer_size, &used, format[index]) != 0) return 0;
				break;
		}
		index += 1U;
	}
	return used;
}

MACOS_NEWLINKER_EXPORT void *localtime_r(const long *time_value, void *result) {
	if (time_value == 0 || result == 0) {
		return 0;
	}
	macos_newlinker_fill_gmtime(*time_value, (struct macos_newlinker_tm *)result);
	return result;
}

MACOS_NEWLINKER_EXPORT void *gmtime_r(const long *time_value, void *result) {
	if (time_value == 0 || result == 0) {
		return 0;
	}
	macos_newlinker_fill_gmtime(*time_value, (struct macos_newlinker_tm *)result);
	return result;
}

MACOS_NEWLINKER_EXPORT int uname(void *name) {
	char *fields = (char *)name;
	char hostname[256];
	if (fields == 0) {
		return -1;
	}
	if (gethostname(hostname, sizeof(hostname)) != 0 || hostname[0] == '\0') {
		(void)macos_newlinker_copy_string(hostname, sizeof(hostname), "newos");
	}
	(void)macos_newlinker_copy_string(fields + 0, 256U, "Darwin");
	(void)macos_newlinker_copy_string(fields + 256U, 256U, hostname);
	(void)macos_newlinker_copy_string(fields + 512U, 256U, "0");
	(void)macos_newlinker_copy_string(fields + 768U, 256U, "newos");
	(void)macos_newlinker_copy_string(fields + 1024U, 256U, "arm64");
	return 0;
}

MACOS_NEWLINKER_RETAIN_EXPORT void *opendir(const char *path) {
	size_t index;
	long fd;
	if (path == 0) {
		return 0;
	}
	fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, MACOS_NEWLINKER_O_RDONLY | MACOS_NEWLINKER_O_DIRECTORY, 0);
	if (fd < 0) {
		return 0;
	}
	for (index = 0; index < MACOS_NEWLINKER_DIR_COUNT; ++index) {
		if (!macos_newlinker_dirs[index].used) {
			macos_newlinker_dirs[index].used = 1;
			macos_newlinker_dirs[index].fd = (int)fd;
			macos_newlinker_dirs[index].base = 0;
			macos_newlinker_dirs[index].offset = 0;
			macos_newlinker_dirs[index].end = 0;
			return &macos_newlinker_dirs[index];
		}
	}
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, fd);
	return 0;
}

MACOS_NEWLINKER_RETAIN_EXPORT void *readdir(void *directory) {
	struct macos_newlinker_dir *dir = (struct macos_newlinker_dir *)directory;
	if (dir == 0 || !dir->used) {
		return 0;
	}
	for (;;) {
		const struct dirent *source;
		if (dir->offset >= dir->end) {
			long amount = darwin_syscall4(MACOS_NEWLINKER_SYS_GETDIRENTRIES64, dir->fd, (long)dir->buffer, (long)sizeof(dir->buffer), (long)&dir->base);
			if (amount <= 0) {
				return 0;
			}
			dir->offset = 0;
			dir->end = amount;
		}
		source = (const struct dirent *)(dir->buffer + dir->offset);
		if (source->d_reclen == 0U || dir->offset + source->d_reclen > dir->end) {
			return 0;
		}
		dir->offset += source->d_reclen;
		dir->entry = *source;
		return &dir->entry;
	}
}

MACOS_NEWLINKER_RETAIN_EXPORT int closedir(void *directory) {
	struct macos_newlinker_dir *dir = (struct macos_newlinker_dir *)directory;
	if (dir == 0 || !dir->used) {
		return -1;
	}
	(void)darwin_syscall1(DARWIN_SYS_CLOSE, dir->fd);
	dir->used = 0;
	return 0;
}

MACOS_NEWLINKER_EXPORT int ioctl(int fd, unsigned long request, void *argument) {
	long result = darwin_syscall3(DARWIN_SYS_IOCTL, (long)fd, (long)request, (long)argument);
	return result < 0 ? -1 : (int)result;
}

MACOS_NEWLINKER_RETAIN_EXPORT int poll(void *fds, unsigned long nfds, int timeout) {
	long result = darwin_syscall3(DARWIN_SYS_POLL, (long)fds, (long)nfds, (long)timeout);
	return result < 0 ? -1 : (int)result;
}

MACOS_NEWLINKER_EXPORT int tcgetattr(int fd, void *termios_value) {
	return ioctl(fd, MACOS_NEWLINKER_TIOCGETA, termios_value);
}

MACOS_NEWLINKER_EXPORT int tcsetattr(int fd, int optional_actions, const void *termios_value) {
	unsigned long request = MACOS_NEWLINKER_TIOCSETA;
	if (optional_actions == 1) {
		request = MACOS_NEWLINKER_TIOCSETAW;
	} else if (optional_actions == 2) {
		request = MACOS_NEWLINKER_TIOCSETAF;
	} else if (optional_actions != 0) {
		return -1;
	}
	return ioctl(fd, request, (void *)termios_value);
}