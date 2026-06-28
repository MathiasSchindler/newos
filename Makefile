CC ?= cc
ifeq ($(origin TARGET_CC), undefined)
ifneq ($(origin CC), file)
TARGET_CC := $(CC)
else
TARGET_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo $(CC); fi)
endif
endif
ifeq ($(shell if command -v "$(TARGET_CC)" >/dev/null 2>&1 || [ -x "$(TARGET_CC)" ]; then echo yes; else echo no; fi),no)
TARGET_CC := $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; elif command -v "$(CC)" >/dev/null 2>&1; then command -v "$(CC)"; elif command -v clang >/dev/null 2>&1; then command -v clang; else echo clang; fi)
endif
HOST_OS := $(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH := $(shell uname -m 2>/dev/null || echo unknown)
COMMA := ,
TARGET_ARCH ?= $(if $(filter Linux,$(HOST_OS)),$(if $(filter x86_64,$(HOST_ARCH)),x86_64,aarch64),aarch64)
TARGET_ARCH_DIR := src/arch/$(TARGET_ARCH)/linux
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common -I$(TARGET_ARCH_DIR)

MACOS_FREESTANDING_ARCH ?= aarch64
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_LTO_FLAGS ?= -flto
HOST_SECTION_CFLAGS ?= -ffunction-sections -fdata-sections
HOST_GC_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-dead_strip,-Wl$(COMMA)--gc-sections)
FREESTANDING_SECTION_CFLAGS ?= -ffunction-sections -fdata-sections
FREESTANDING_STACK_CFLAGS ?=
FREESTANDING_PACK_SIZE ?= 0
ifeq ($(FREESTANDING_PACK_SIZE),1)
FREESTANDING_PIE_CFLAGS ?=
else
FREESTANDING_PIE_CFLAGS ?= -fPIE
endif
FREESTANDING_OPT_CFLAGS ?= $(shell printf 'int x;\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -Oz -x c - -c -o /tmp/newos-oz-check.o >/dev/null 2>&1 && echo -Oz || echo -Os; rm -f /tmp/newos-oz-check.o)
FREESTANDING_SIZE_CFLAGS ?= $(shell printf 'int x;\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -fcf-protection=none -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 -fomit-frame-pointer -fno-ident -x c - -c -o /tmp/newos-size-flags-check.o >/dev/null 2>&1 && echo -fcf-protection=none -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 -fomit-frame-pointer -fno-ident; rm -f /tmp/newos-size-flags-check.o)
FREESTANDING_LTO_FLAGS ?= -flto
PROFILE ?= 0
LINKER_REPORTS ?= 0
PROFILE_CFLAGS ?= $(if $(filter 1 yes true,$(PROFILE)),-finstrument-functions -fno-omit-frame-pointer -fno-inline)
PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),src/platform/linux/profiler_runtime.c)
MACOS_PROFILE_RUNTIME_SOURCE := $(if $(filter 1 yes true,$(PROFILE)),src/platform/macos/profiler_runtime.c)
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 $(FREESTANDING_STACK_CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables $(FREESTANDING_SECTION_CFLAGS) $(FREESTANDING_PIE_CFLAGS) $(FREESTANDING_OPT_CFLAGS) $(FREESTANDING_SIZE_CFLAGS) $(FREESTANDING_LTO_FLAGS)
FREESTANDING_CFLAGS += $(PROFILE_CFLAGS)
FREESTANDING_DEBUG ?= 0
TARGET_CC_TARGET_FLAG ?= $(shell printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" --target=$(TARGET_TRIPLE) -x c - -c -o /tmp/newos-target-check.o >/dev/null 2>&1 && echo --target=$(TARGET_TRIPLE); rm -f /tmp/newos-target-check.o)
TARGET_LINKER_FLAG ?= $(shell printf 'void _start(void){}\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -nostdlib -fuse-ld=lld -Wl$(COMMA)-e$(COMMA)_start -x c - -o /tmp/newos-lld-check >/dev/null 2>&1 && echo -fuse-ld=lld; rm -f /tmp/newos-lld-check)
ifeq ($(FREESTANDING_PACK_SIZE),1)
FREESTANDING_COMPACT_LDFLAGS ?=
FREESTANDING_BUILD_ID_LDFLAGS ?= -Wl$(COMMA)--build-id=none
FREESTANDING_LINK_MODE_LDFLAGS ?= -static
else
FREESTANDING_COMPACT_LDFLAGS ?= $(shell printf 'int main(void){return 0;}\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -nostdlib -static-pie -Wl$(COMMA)-z$(COMMA)noseparate-code -x c - -o /tmp/newos-compact-link-check >/dev/null 2>&1 && echo -Wl$(COMMA)-z$(COMMA)noseparate-code; rm -f /tmp/newos-compact-link-check)
FREESTANDING_BUILD_ID_LDFLAGS ?= $(shell printf 'void _start(void){}\n' | "$(TARGET_CC)" $(TARGET_CC_TARGET_FLAG) -nostdlib -static-pie -Wl$(COMMA)--build-id=none -Wl$(COMMA)-e$(COMMA)_start -x c - -o /tmp/newos-build-id-check >/dev/null 2>&1 && echo -Wl$(COMMA)--build-id=none; rm -f /tmp/newos-build-id-check)
FREESTANDING_LINK_MODE_LDFLAGS ?= -static-pie
endif
TARGET_BUILTINS_LIB ?= $(if $(filter MSYS_NT% MINGW% CYGWIN%,$(HOST_OS)),,$(shell "$(TARGET_CC)" -print-libgcc-file-name >/dev/null 2>&1 && echo -lgcc || true))
FREESTANDING_PIE_LDFLAGS ?= $(FREESTANDING_LINK_MODE_LDFLAGS)
FREESTANDING_GC_LDFLAGS ?= -Wl,--gc-sections
ifeq ($(FREESTANDING_DEBUG),1)
FREESTANDING_STRIP_LDFLAGS ?=
else
FREESTANDING_STRIP_LDFLAGS ?= -Wl,-s
endif
SELFHOST_SECTION_CFLAGS ?= -ffunction-sections -fdata-sections
SELFHOST_COMPACT_LDFLAGS ?= $(if $(filter Linux,$(HOST_OS)),-Wl$(COMMA)--build-id=none -Wl$(COMMA)-z$(COMMA)noseparate-code)
SELFHOST_GC_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-dead_strip,-Wl$(COMMA)--gc-sections)
SELFHOST_STRIP_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-x,-Wl$(COMMA)-s)
SELFHOST_SIZE_FLAGS ?= $(SELFHOST_SECTION_CFLAGS) $(SELFHOST_COMPACT_LDFLAGS) $(SELFHOST_GC_LDFLAGS) $(SELFHOST_STRIP_LDFLAGS)
TARGET_LDFLAGS ?= -nostdlib $(FREESTANDING_PIE_LDFLAGS) $(TARGET_LINKER_FLAG) $(FREESTANDING_COMPACT_LDFLAGS) $(FREESTANDING_BUILD_ID_LDFLAGS) $(FREESTANDING_GC_LDFLAGS) $(FREESTANDING_STRIP_LDFLAGS) $(TARGET_BUILTINS_LIB) $(FREESTANDING_LTO_FLAGS)
BUILD_ROOT ?= build
HOST_OS_NAME := $(if $(filter Darwin,$(HOST_OS)),macos,$(shell printf '%s' "$(HOST_OS)" | tr A-Z a-z))
HOST_ARCH_NAME := $(if $(filter arm64 aarch64,$(HOST_ARCH)),aarch64,$(HOST_ARCH))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH_NAME)),1,0),0)
DEFAULT_HOST_BUILD_DIR := $(BUILD_ROOT)/host-$(HOST_OS_NAME)-$(HOST_ARCH_NAME)
BUILD_DIR ?= $(DEFAULT_HOST_BUILD_DIR)
TARGET_BUILD_DIR ?= $(BUILD_ROOT)/freestanding-linux-$(TARGET_ARCH)

MACOS_BUILD_DIR ?= $(BUILD_ROOT)/macos-$(MACOS_FREESTANDING_ARCH)
TEST_FREESTANDING_BUILD_DIR ?= $(if $(filter 1,$(LOCAL_MACOS_FREESTANDING)),$(MACOS_BUILD_DIR),$(TARGET_BUILD_DIR))
MAOS_SMOKE_TOOLS ?= true false echo printf rev seq cat basename dirname cut tr wc
MAOS_SMOKE_TARGETS := $(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_SMOKE_TOOLS))

HOST_SIZE_FLAGS ?= $(if $(filter $(BUILD_DIR),$(DEFAULT_HOST_BUILD_DIR)),$(HOST_SECTION_CFLAGS) $(HOST_GC_LDFLAGS))
ifneq ($(findstring ncc,$(CC)),)
  HOST_SHARED_INC_FLAG := -Isrc/shared
else
  HOST_SHARED_INC_FLAG := -idirafter src/shared
endif
HOST_CFLAGS ?= $(filter-out -Isrc/shared,$(CFLAGS)) $(HOST_SIZE_FLAGS) $(HOST_SHARED_INC_FLAG)
HOST_CFLAGS += $(PROFILE_CFLAGS)
SELFHOST_CC_DEP := $(if $(filter $(BUILD_DIR),$(SELFHOST_BUILD_DIR)),$(DEFAULT_HOST_BUILD_DIR)/ncc)
ifeq ($(TARGET_ARCH),x86_64)
TARGET_TRIPLE ?= x86_64-linux-none
else ifeq ($(TARGET_ARCH),aarch64)
TARGET_TRIPLE ?= aarch64-linux-none
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)'; expected x86_64 or aarch64)
endif
PARALLEL_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
PHASE1_JOBS ?= $(PARALLEL_JOBS)
PARALLEL_MAKEFLAGS := $(filter -j,$(MAKEFLAGS)) $(filter -j%,$(MAKEFLAGS)) $(filter --jobserver%,$(MAKEFLAGS))
# Primary goal: build statically linked, dependency-free binaries
# On macOS: freestanding Mach-O binaries using our linker
# On Linux: freestanding ELF binaries using our linker
FREESTANDING_USE_NEWLINKER ?= $(if $(filter Linux,$(HOST_OS)),$(if $(filter x86_64,$(TARGET_ARCH)),1,0),0)
DEFAULT_ALL_TARGETS := freestanding
TOOLS := sh ls cat clear echo pwd mkdir mount umount rm rmdir cp mv ln chmod chown chgrp mknod uname hostname init getty login dmesg logger stty touch gzip gunzip bzip2 bunzip2 xz unxz zip unzip tar cpio base64 md5sum sha1sum sha256sum sha512sum sleep env kill pgrep pkill shutdown wc head tail ps top sort cut tr grep ripgrep rg ping ping6 ip ss host id whoami find sed awk date tee xargs dd od hexdump basename dirname realpath cmp diff file strings ar nm size readelf readapk objdump strip strace linker expack printf which readlink stat du df tree netcat portscan dhcp nslookup dig ssh scp sshd traceroute whois lsof lsusb sql jq git ncc man test [ true false expr uniq seq mktemp yes less more watch wget wtf mail editor patch make tac nl paste join comm split csplit shuf fold fmt tsort sync truncate timeout time profiler perf expand unexpand printenv ed bc solve pstree free uptime who users groups column rev httpd service imginfo imgcheck imgmeta c2pa pgpkey pgpmsg pgpquery pdfinfo pdfjoin pdfsplit pdfinfoedit pdfextract pdfgrep pdfcheck xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo

MACOS_FREESTANDING_TOOLS ?= true false echo printf basename dirname yes rev seq expr test [ nl tac expand unexpand fold wc head tail cat cut tr uniq cmp comm join paste printenv pwd mkdir rmdir tee which readlink realpath sleep file strings hexdump od base64 md5sum sha1sum sha256sum sha512sum dd touch truncate sync bc solve split shuf fmt column tsort mktemp clear date uname hostname whoami id groups ls du stat df rm cp mv ln chmod chown chgrp free kill csplit sort env time timeout profiler watch find ps pgrep pkill stty more less xargs grep sed ed patch diff logger wtf awk gzip gunzip bzip2 bunzip2 xz unxz zip unzip tar cpio ar nm size readelf readapk objdump strip strace expack imgmeta imginfo imgcheck c2pa pgpkey pgpmsg pgpquery pdfinfo pdfjoin pdfsplit pdfinfoedit pdfextract pdfgrep pdfcheck xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo wget sql jq git man pstree ncc tree netcat portscan nslookup dig host ssh scp sshd traceroute whois lsof lsusb httpd ip ss ping ping6 sh mail editor make dhcp dmesg getty init login mknod mount rg ripgrep service shutdown top umount uptime users who
MACOS_FREESTANDING_HASH_TOOLS := md5sum sha1sum sha256sum sha512sum
MACOS_FREESTANDING_GIT_TOOLS := git
GIT_PRIVATE_SOURCES := $(wildcard src/tools/git/*.c)
MACOS_FREESTANDING_TLS_TOOLS := wtf wget portscan
MACOS_FREESTANDING_AWK_TOOLS := awk
MACOS_FREESTANDING_IMAGE_TOOLS := imgmeta imginfo imgcheck c2pa
MACOS_FREESTANDING_PGP_TOOLS := pgpkey pgpmsg
MACOS_FREESTANDING_PGPQUERY_TOOLS := pgpquery
MACOS_FREESTANDING_PDF_TOOLS := pdfinfo pdfjoin pdfsplit pdfinfoedit pdfextract pdfgrep pdfcheck
MACOS_FREESTANDING_ARCHIVE_TOOLS := gzip gunzip bzip2 bunzip2 xz unxz zip unzip tar ar nm size readelf readapk objdump strip expack
MACOS_FREESTANDING_XML_TOOLS := xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo
MACOS_FREESTANDING_NCC_TOOLS := ncc
MACOS_FREESTANDING_SSH_TOOLS := ssh
MACOS_FREESTANDING_SSHD_TOOLS := sshd
MACOS_FREESTANDING_HTTPD_TOOLS := httpd
MACOS_FREESTANDING_PING6_TOOLS := ping6
MACOS_FREESTANDING_SHELL_TOOLS := sh
MACOS_FREESTANDING_MAIL_TOOLS := mail
MACOS_FREESTANDING_TUI_TOOLS := editor
MACOS_FREESTANDING_MAKE_TOOLS := make
MACOS_FREESTANDING_SERVICE_TOOLS := service
MACOS_FREESTANDING_USB_TOOLS := lsusb
MACOS_FREESTANDING_ALIAS_TOOLS := rg
MACOS_FREESTANDING_GENERIC_TOOLS := $(filter-out $(MACOS_FREESTANDING_HASH_TOOLS) $(MACOS_FREESTANDING_GIT_TOOLS) $(MACOS_FREESTANDING_TLS_TOOLS) $(MACOS_FREESTANDING_AWK_TOOLS) $(MACOS_FREESTANDING_IMAGE_TOOLS) $(MACOS_FREESTANDING_PGP_TOOLS) $(MACOS_FREESTANDING_PGPQUERY_TOOLS) $(MACOS_FREESTANDING_PDF_TOOLS) $(MACOS_FREESTANDING_ARCHIVE_TOOLS) $(MACOS_FREESTANDING_XML_TOOLS) $(MACOS_FREESTANDING_NCC_TOOLS) $(MACOS_FREESTANDING_SSH_TOOLS) $(MACOS_FREESTANDING_SSHD_TOOLS) $(MACOS_FREESTANDING_HTTPD_TOOLS) $(MACOS_FREESTANDING_PING6_TOOLS) $(MACOS_FREESTANDING_SHELL_TOOLS) $(MACOS_FREESTANDING_MAIL_TOOLS) $(MACOS_FREESTANDING_TUI_TOOLS) $(MACOS_FREESTANDING_MAKE_TOOLS) $(MACOS_FREESTANDING_SERVICE_TOOLS) $(MACOS_FREESTANDING_USB_TOOLS) $(MACOS_FREESTANDING_ALIAS_TOOLS),$(MACOS_FREESTANDING_TOOLS))
FREESTANDING_OBJECT_BUILD_DIR ?= $(TARGET_BUILD_DIR)/.obj
TOOL_SOURCE_TOOLS := $(filter-out rg,$(TOOLS))
TOOL_SOURCES := $(addprefix src/tools/,$(addsuffix .c,$(TOOL_SOURCE_TOOLS)))
COMPILER_SOURCES := $(shell grep -oE '"src/compiler/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
COMPILER_IMPL_INCLUDES := \
	src/compiler/backend_internal.h \
	src/compiler/parser_internal.h \
	src/compiler/targets/target_info.h
SHARED_SOURCES := $(shell grep -oE '"src/shared/(runtime/[^"]+|compression/[^"]+|tool_[^"]+|archive_util|object_util|archive_zip|bignum|simple_config|server_log|xml|xml_stream|xml_dtd)\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
SHARED_DEPS := $(SHARED_SOURCES) src/compiler/source_manifest.h
IMAGE_SOURCES := $(shell grep -oE '"src/shared/(image/[^"]+|crypto/(sha256|p256))\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
IMAGE_TOOLS := imginfo imgcheck imgmeta c2pa
PDF_SOURCES := $(shell grep -oE '"src/shared/pdf(_writer)?\.c"' src/compiler/source_manifest.h | tr -d '"')
PDF_TOOLS := pdfinfo pdfjoin pdfsplit pdfinfoedit pdfextract pdfgrep pdfcheck
PGP_SOURCES := $(shell grep -oE '"src/shared/(pgp|crypto/(aes128|aes128_gcm|sha1|sha256|sha512|hmac_sha256|hkdf_sha256|crypto_util|curve25519|ed25519|rsa)|compression/zlib)\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
PGP_TOOLS := pgpkey pgpmsg
CRYPTO_SOURCES := $(shell grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
TLS_SOURCES := $(shell grep -oE '"src/shared/tls/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
PGPQUERY_TOOLS := pgpquery
PGPQUERY_SOURCES := $(sort $(PGP_SOURCES) $(TLS_SOURCES) $(CRYPTO_SOURCES))
USB_SOURCES := $(shell grep -oE '"src/shared/usb\.c"' src/compiler/source_manifest.h | tr -d '"')
HOST_USB_PLATFORM_SOURCES := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter arm64 aarch64,$(HOST_ARCH)),src/platform/macos/iokit.c src/platform/macos/usb.c,src/platform/posix/usb.c),$(if $(filter Linux,$(HOST_OS)),src/platform/linux/usb.c,src/platform/posix/usb.c))
TARGET_USB_PLATFORM_SOURCES := src/platform/linux/usb.c
MACOS_USB_PLATFORM_SOURCES := src/platform/macos/iokit.c src/platform/macos/usb.c

TUI_SOURCES := $(shell grep -oE '"src/shared/tui\.c"' src/compiler/source_manifest.h | tr -d '"')
FONTRENDER_SOURCES := $(shell grep -oE '"src/shared/(fontrender_runtime|fontrender/[^"]+)\.c"' src/compiler/source_manifest.h | tr -d '"')
FONTRENDER_DEPS := $(FONTRENDER_SOURCES) src/shared/fontrender_runtime.h $(wildcard src/shared/fontrender/*.h src/shared/fontrender/fontrender/*.h)
FONTTEST_SOURCE := tests/fixtures/fontrender/fonttest.c
THREADTEST_SOURCE := tests/fixtures/platform/threadtest.c
CRYPTO_USB_TEST_SOURCE := tests/fixtures/crypto_usb_test.c
HASH_SOURCES := \
	$(shell grep -oE '"src/shared/hash_util\.c"' src/compiler/source_manifest.h | tr -d '"') \
	src/shared/crypto/md5.c \
	src/shared/crypto/sha1.c \
	src/shared/crypto/sha256.c \
	src/shared/crypto/sha512.c
SSH_TRANSPORT_SOURCES := $(shell grep -oE '"src/tools/ssh/ssh_(core|client_io)\.c"' src/compiler/source_manifest.h | tr -d '"')
SSH_CLIENT_SOURCES := $(shell grep -oE '"src/tools/ssh/ssh_(core|known_hosts|client[^"]*)\.c"' src/compiler/source_manifest.h | tr -d '"')
SSHD_TOOL_SOURCES := $(shell grep -oE '"src/tools/sshd/sshd_[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
EXPACK_SIGNING_SOURCE := src/shared/crypto/sha256.c
LINKER_SIGNING_SOURCE := $(EXPACK_SIGNING_SOURCE)
EXPACK_PRIVATE_SOURCES := src/tools/expack/internal.h src/tools/expack/codecs.c src/tools/expack/deflate.c src/tools/expack/deflate_stub.inc src/tools/expack/deflate_stub_template.c src/tools/expack/formats.c src/tools/expack/outputs.c src/tools/expack/outputs_elf.c src/tools/expack/outputs_pe.c src/tools/expack/macho_arm64_runner_template.c src/tools/expack/macho_arm64_lzrep_runner.inc src/tools/expack/macho_arm64_lzss_runner.inc src/tools/expack/macho_arm64_lz4_runner.inc
SSH_CRYPTO_SOURCES := \
	$(CRYPTO_SOURCES) \
	src/shared/crypto/curve25519.c \
	src/shared/crypto/ed25519.c \
	src/shared/crypto/chacha20_poly1305.c \
	src/shared/crypto/ssh_kdf.c
SSH_CRYPTO_SOURCES := $(sort $(SSH_CRYPTO_SOURCES))
SHELL_SOURCES := $(shell grep -oE '"src/tools/sh/shell_[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
HOST_PLATFORM_SOURCES := $(shell grep -oE '"src/platform/posix/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
MACOS_FREESTANDING_RUNTIME_SOURCES := src/shared/runtime/memory.c src/shared/runtime/string.c src/shared/runtime/parse.c src/shared/runtime/io.c src/shared/runtime/concurrency.c src/shared/runtime/io_loop.c src/shared/runtime/unicode_utf8.c src/shared/runtime/unicode.c src/shared/tool_json.c src/shared/tool_cli.c src/shared/tool_file.c src/shared/tool_io.c src/shared/tool_path.c src/shared/tool_fs.c src/shared/tool_regex.c src/shared/tool_process.c src/shared/bignum.c src/platform/macos/freestanding.c
MACOS_FREESTANDING_HASH_SOURCES := src/shared/hash_util.c src/shared/crypto/md5.c src/shared/crypto/sha1.c src/shared/crypto/sha256.c src/shared/crypto/sha512.c
MACOS_FREESTANDING_GIT_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) src/shared/compression/crc32.c src/shared/compression/zlib.c src/platform/macos/tls.c
MACOS_FREESTANDING_TLS_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) src/platform/macos/tls.c
MACOS_FREESTANDING_AWK_SOURCES := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c
MACOS_FREESTANDING_ARCHIVE_SOURCES := src/shared/archive_util.c src/shared/object_util.c src/shared/archive_zip.c src/shared/compression/bzip2.c src/shared/compression/crc32.c src/shared/compression/lzss.c src/shared/compression/zlib.c $(EXPACK_SIGNING_SOURCE)
MACOS_FREESTANDING_IMAGE_SOURCES := $(IMAGE_SOURCES) src/shared/compression/crc32.c src/shared/compression/zlib.c
MACOS_FREESTANDING_PGP_SOURCES := $(PGP_SOURCES)
MACOS_FREESTANDING_PGPQUERY_SOURCES := $(PGPQUERY_SOURCES) src/platform/macos/tls.c
MACOS_FREESTANDING_PDF_SOURCES := $(PDF_SOURCES) src/shared/compression/zlib.c
MACOS_FREESTANDING_XML_SOURCES := src/shared/xml.c src/shared/xml_stream.c src/shared/xml_dtd.c src/shared/tool_xml.c
MACOS_FREESTANDING_NCC_SOURCES := $(COMPILER_SOURCES) $(LINKER_SIGNING_SOURCE) $(SHARED_SOURCES)
MACOS_FREESTANDING_SSH_SOURCES := $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_SSHD_SOURCES := $(SSHD_TOOL_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_HTTPD_SOURCES = $(HTTPD_TOOL_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_SHELL_SOURCES := $(SHELL_SOURCES)
MACOS_FREESTANDING_EDITOR_SOURCES = $(EDITOR_TOOL_SOURCES) $(TUI_SOURCES)
MACOS_FREESTANDING_MAIL_SOURCES = $(MAIL_TOOL_SOURCES) $(TUI_SOURCES) $(MACOS_FREESTANDING_TLS_SOURCES)
MACOS_FREESTANDING_MAKE_SOURCES = $(MAKE_TOOL_SOURCES)
MACOS_FREESTANDING_SERVICE_SOURCES = $(SERVICE_TOOL_SOURCES) src/shared/simple_config.c
MACOS_FREESTANDING_USB_SOURCES := $(USB_SOURCES) $(MACOS_USB_PLATFORM_SOURCES)
MACOS_FREESTANDING_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=3 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections $(MACOS_FREESTANDING_LTO_FLAGS) -Isrc/shared -Isrc/platform/macos -Isrc/arch/aarch64/macos
MACOS_FREESTANDING_LDFLAGS ?= -nodefaultlibs -lSystem -Wl$(COMMA)-dead_strip -Wl$(COMMA)-x -Wl$(COMMA)-no_function_starts -Wl$(COMMA)-adhoc_codesign $(MACOS_FREESTANDING_LTO_FLAGS)
MACOS_FREESTANDING_NO_LTO_CFLAGS := $(filter-out $(MACOS_FREESTANDING_LTO_FLAGS),$(MACOS_FREESTANDING_CFLAGS))
MACOS_FREESTANDING_NO_LTO_LDFLAGS := $(filter-out $(MACOS_FREESTANDING_LTO_FLAGS),$(MACOS_FREESTANDING_LDFLAGS))
MACOS_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-common -fno-jump-tables -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions -DNEWOS_MACOS_NEWLINKER -DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_RUNTIME_ALLOC_LOCK=3 -DNEWOS_CRYPTO_CURVE25519_FORCE_64BIT=1 -DNEWOS_TOOL_DEFAULT_COLOR_NEVER=1 -flto $(PROFILE_CFLAGS)
MACOS_LDFLAGS ?= --macho-compact --gc-sections
MACOS_MAP_FLAG = $(if $(MACOS_MAP_DIR),--map $(MACOS_MAP_DIR)/$(@F).map,)
MACOS_RUNTIME_OBJECTS := $(patsubst %.c,$(MACOS_BUILD_DIR)/.obj/%.lto.o,$(MACOS_FREESTANDING_RUNTIME_SOURCES))
MACOS_TOOL_TARGETS := $(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_TOOLS))
MACOS_ALL_TOOL_TARGETS = $(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_TOOLS))
# Keep this shell extraction comma-free for compatibility with older GNU make on macOS.
TARGET_PLATFORM_MANIFEST_SOURCES := $(shell grep -oE '"src/platform/linux/[^"]+\.c"|"src/arch/[^"]+/linux/[^"]+\.(c|S)"' src/compiler/source_manifest.h | tr -d '"')
TARGET_PLATFORM_SOURCES := $(filter src/platform/linux/% $(TARGET_ARCH_DIR)/%,$(TARGET_PLATFORM_MANIFEST_SOURCES))
TARGET_PLATFORM_ASM_SOURCES := $(filter %.S,$(TARGET_PLATFORM_SOURCES))
SELFHOST_PLATFORM_SOURCES := $(if $(filter Linux,$(HOST_OS)),$(TARGET_PLATFORM_SOURCES),$(HOST_PLATFORM_SOURCES))
TARGET_CRT := $(TARGET_ARCH_DIR)/crt0.S
FREESTANDING_REUSABLE_SOURCES := $(filter %.c,$(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES))
FREESTANDING_REUSABLE_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(FREESTANDING_REUSABLE_SOURCES))
FREESTANDING_REUSABLE_INPUTS := $(FREESTANDING_REUSABLE_OBJECTS) $(TARGET_PLATFORM_ASM_SOURCES)
FREESTANDING_IMAGE_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(IMAGE_SOURCES))
FREESTANDING_PGP_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(PGP_SOURCES))
FREESTANDING_PDF_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(PDF_SOURCES))
FREESTANDING_CRYPTO_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(CRYPTO_SOURCES))
FREESTANDING_SSH_CRYPTO_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(SSH_CRYPTO_SOURCES))
FREESTANDING_TLS_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(TLS_SOURCES))
FREESTANDING_PGPQUERY_OBJECTS := $(sort $(FREESTANDING_PGP_OBJECTS) $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS))
FREESTANDING_TUI_OBJECT = $(FREESTANDING_OBJECT_BUILD_DIR)/$(TUI_SOURCES:.c=.o)
FREESTANDING_TARGET_TLS_PLATFORM_OBJECT = $(FREESTANDING_OBJECT_BUILD_DIR)/$(TARGET_TLS_PLATFORM_SOURCE:.c=.o)
HOST_OUTPUTS := $(BUILD_DIR)/.ssh_core_check $(addprefix $(BUILD_DIR)/,$(TOOLS))
HOST_COMPAT_TARGETS := $(if $(filter $(BUILD_DIR),$(DEFAULT_HOST_BUILD_DIR)),$(BUILD_ROOT)/.ssh_core_check $(addprefix $(BUILD_ROOT)/,$(TOOLS)))

.DEFAULT_GOAL := all
.SECONDEXPANSION:

.PHONY: all host freestanding test benchmark compiler-benchmark clean

test: $(DEFAULT_ALL_TARGETS)
	NEWOS_TEST_BUILD_DIR="$(abspath $(TEST_FREESTANDING_BUILD_DIR))" PHASE1_JOBS=$(PHASE1_JOBS) sh ./tests/phase1/run_phase1_tests.sh
	NEWOS_TEST_BUILD_DIR="$(abspath $(TEST_FREESTANDING_BUILD_DIR))" sh ./tests/suites/freestanding.sh

benchmark: host compiler-benchmark
	sh ./tests/benchmarks/run_benchmarks.sh

compiler-benchmark: $(BUILD_DIR)/ncc
	COMPILER_BENCH_NCC="$(abspath $(BUILD_DIR))/ncc" sh ./tests/compiler/run_compiler_benchmarks.sh

# Core build rules for freestanding binaries (both Linux and macOS)
ifeq ($(AUTO_PARALLEL),1)
all: $(DEFAULT_ALL_TARGETS)
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
all: $(DEFAULT_ALL_TARGETS)
else
all:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) $(DEFAULT_ALL_TARGETS)
endif

ifeq ($(AUTO_PARALLEL),1)
host: $(HOST_OUTPUTS) $(HOST_COMPAT_TARGETS)
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
freestanding: $(MACOS_ALL_TOOL_TARGETS)
else ifeq ($(FREESTANDING_USE_NEWLINKER),1)
freestanding: $(BUILD_DIR)/linker
	WORK="$(abspath $(TARGET_BUILD_DIR))" LINKER="$(abspath $(BUILD_DIR)/linker)" NEWLINKER_CC="$(TARGET_CC)" NEWLINKER_LINK_JOBS="$(PARALLEL_JOBS)" NEWLINKER_PROFILE="$(PROFILE)" LINKER_REPORTS="$(LINKER_REPORTS)" bash scripts/build-freestanding-newlinker.sh
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
host: $(HOST_OUTPUTS) $(HOST_COMPAT_TARGETS)
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
freestanding: $(MACOS_ALL_TOOL_TARGETS)
else ifeq ($(FREESTANDING_USE_NEWLINKER),1)
freestanding: $(BUILD_DIR)/linker
	WORK="$(abspath $(TARGET_BUILD_DIR))" LINKER="$(abspath $(BUILD_DIR)/linker)" NEWLINKER_CC="$(TARGET_CC)" NEWLINKER_LINK_JOBS="$(PARALLEL_JOBS)" NEWLINKER_PROFILE="$(PROFILE)" LINKER_REPORTS="$(LINKER_REPORTS)" bash scripts/build-freestanding-newlinker.sh
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else
host:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host
freestanding:
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
else ifeq ($(FREESTANDING_USE_NEWLINKER),1)
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
else
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif
endif

$(sort $(BUILD_ROOT) $(BUILD_DIR) $(TARGET_BUILD_DIR) $(MACOS_BUILD_DIR) $(MACOS_BUILD_DIR)/.obj $(FREESTANDING_OBJECT_BUILD_DIR)):
	mkdir -p $@

$(BUILD_ROOT)/.ssh_core_check: $(BUILD_DIR)/.ssh_core_check | $(BUILD_ROOT)
	rm -f $@ && ln -sfn $(patsubst $(BUILD_ROOT)/%,%,$<) $@

$(BUILD_ROOT)/%: $(BUILD_DIR)/% | $(BUILD_ROOT)
	rm -f $@ && ln -sfn $(patsubst $(BUILD_ROOT)/%,%,$<) $@

$(BUILD_DIR)/ping6: $(BUILD_DIR)/ping | $(BUILD_DIR)
	rm -f $@ && ln -sfn ping $@

$(TARGET_BUILD_DIR)/ping6: $(TARGET_BUILD_DIR)/ping | $(TARGET_BUILD_DIR)
	rm -f $@ && ln -sfn ping $@

$(BUILD_DIR)/rg: $(BUILD_DIR)/ripgrep | $(BUILD_DIR)
	rm -f $@ && ln -sfn ripgrep $@

$(TARGET_BUILD_DIR)/rg: $(TARGET_BUILD_DIR)/ripgrep | $(TARGET_BUILD_DIR)
	rm -f $@ && ln -sfn ripgrep $@

$(BUILD_DIR)/.ssh_core_check: $(SSH_CLIENT_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/tools/ssh/ssh_transport.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $(HOST_SECTION_CFLAGS) -fsyntax-only $(SSH_CLIENT_SOURCES) && : > $@

$(TARGET_BUILD_DIR)/.ssh_core_check: $(SSH_CLIENT_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/tools/ssh/ssh_transport.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -fsyntax-only $(SSH_CLIENT_SOURCES) && : > $@

$(BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sh: src/tools/sh.c $(TARGET_REUSABLE_OBJECTS) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHELL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(LINKER_SIGNING_SOURCE) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_NCC_CFLAGS) $< $(COMPILER_SOURCES) $(LINKER_SIGNING_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(LINKER_SIGNING_SOURCE) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=0 $< $(COMPILER_SOURCES) $(LINKER_SIGNING_SOURCE) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/md5sum $(BUILD_DIR)/sha1sum $(BUILD_DIR)/sha256sum $(BUILD_DIR)/sha512sum: $(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) $(HASH_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha1.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(HASH_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/md5sum $(TARGET_BUILD_DIR)/sha1sum $(TARGET_BUILD_DIR)/sha256sum $(TARGET_BUILD_DIR)/sha512sum: $(TARGET_BUILD_DIR)/%: src/tools/%.c $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRYPTO_OBJECTS) src/shared/hash_util.c src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha1.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< src/shared/hash_util.c $(TARGET_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/git: src/tools/git.c $(GIT_PRIVATE_SOURCES) $(TLS_SOURCES) $(CRYPTO_SOURCES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha1.h src/shared/compression/crc32.h src/shared/compression/zlib.h $(HOST_TLS_PLATFORM_SOURCE) $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(TLS_SOURCES) $(CRYPTO_SOURCES) $(SHARED_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/git: src/tools/git.c $(GIT_PRIVATE_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha1.h src/shared/compression/crc32.h src/shared/compression/zlib.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ssh: src/tools/ssh.c $(SHARED_SOURCES) $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ssh: src/tools/ssh.c $(TARGET_REUSABLE_OBJECTS) $(SSH_CLIENT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SSH_CLIENT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/sshd: src/tools/sshd.c $(SSHD_TOOL_SOURCES) $(SHARED_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sshd/sshd.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_transport.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SSHD_TOOL_SOURCES) $(SHARED_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sshd: src/tools/sshd.c $(SSHD_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(SSH_TRANSPORT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sshd/sshd.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_transport.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SSHD_TOOL_SOURCES) $(SSH_TRANSPORT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

MAKE_TOOL_SOURCES := src/tools/make/make_parse.c src/tools/make/make_exec.c
LINKER_TOOL_SOURCES := src/compiler/linker.c \
    src/compiler/linker_util.c \
    src/compiler/linker_elf.c \
    src/compiler/linker_object.c \
    src/compiler/linker_symbols.c \
    src/compiler/linker_gc.c \
    src/compiler/linker_merge.c \
    src/compiler/linker_icf.c \
    src/compiler/linker_reloc.c \
    src/compiler/linker_layout.c \
    src/compiler/linker_report.c \
	src/compiler/linker_lto.c \
	src/compiler/linker_macho.c
HOST_LINKER_CFLAGS = $(filter-out -Isrc/shared,$(CFLAGS) $(HOST_SIZE_FLAGS) $(PROFILE_CFLAGS)) $(HOST_SHARED_INC_FLAG)
HOST_EXPACK_CFLAGS = $(filter-out -Isrc/shared,$(HOST_CFLAGS)) $(HOST_SHARED_INC_FLAG)
HOST_NCC_CFLAGS = $(filter-out -Isrc/shared,$(HOST_CFLAGS)) -DCOMPILER_LINKER_ENABLE_REPORTING=0 $(HOST_SHARED_INC_FLAG)
HOST_READELF_CFLAGS = $(filter-out -Isrc/shared,$(HOST_CFLAGS)) $(HOST_SHARED_INC_FLAG)
AWK_TOOL_SOURCES  := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c
SERVICE_TOOL_SOURCES := src/tools/service/service_main.c src/tools/service/service_pidfile.c src/tools/service/service_spawn.c src/tools/service/service_signal.c src/tools/service/service_config.c
HTTPD_TOOL_SOURCES := src/tools/httpd/httpd_main.c src/tools/httpd/http_listener.c src/tools/httpd/http_conn.c src/tools/httpd/http_parse.c src/tools/httpd/http_route.c src/tools/httpd/http_static.c src/tools/httpd/http_log.c
EDITOR_TOOL_SOURCES := src/tools/editor/highlight.c
MAIL_TOOL_SOURCES := src/tools/mail/imap.c src/tools/mail/message.c src/tools/mail/mime.c
HOST_TLS_PLATFORM_SOURCE := src/platform/posix/tls.c
TARGET_TLS_PLATFORM_SOURCE := src/platform/linux/tls.c
MACOS_RUNTIME_SHIM_SOURCE := src/platform/macos/newlinker_runtime.c
macos_objects = $(patsubst %.c,$(MACOS_BUILD_DIR)/.obj/%.lto.o,$(filter %.c,$(sort $(1))))
MACOS_PLATFORM_SOURCES := $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_RUNTIME_SHIM_SOURCE) $(MACOS_PROFILE_RUNTIME_SOURCE)
MACOS_COMMON_OBJECTS := $(call macos_objects,$(MACOS_PLATFORM_SOURCES))
MACOS_HASH_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_HASH_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_GIT_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_GIT_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_TLS_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_TLS_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_AWK_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_AWK_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_ARCHIVE_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_ARCHIVE_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_IMAGE_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_IMAGE_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_PGP_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_PGP_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_PGPQUERY_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_PGPQUERY_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_PDF_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_PDF_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_XML_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_XML_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_NCC_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_NCC_SOURCES) src/platform/macos/freestanding.c $(MACOS_RUNTIME_SHIM_SOURCE) $(MACOS_PROFILE_RUNTIME_SOURCE))
MACOS_SSH_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_SSH_SOURCES) src/platform/macos/freestanding.c $(MACOS_RUNTIME_SHIM_SOURCE) $(MACOS_PROFILE_RUNTIME_SOURCE))
MACOS_SSHD_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_SSHD_SOURCES) src/platform/macos/freestanding.c $(MACOS_RUNTIME_SHIM_SOURCE) $(MACOS_PROFILE_RUNTIME_SOURCE))
MACOS_HTTPD_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_HTTPD_SOURCES) src/platform/macos/freestanding.c $(MACOS_RUNTIME_SHIM_SOURCE) $(MACOS_PROFILE_RUNTIME_SOURCE))
MACOS_SHELL_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_SHELL_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_EDITOR_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_EDITOR_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_MAIL_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_MAIL_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_MAKE_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_MAKE_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_SERVICE_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_SERVICE_SOURCES) $(MACOS_PLATFORM_SOURCES))
MACOS_USB_OBJECTS := $(call macos_objects,$(MACOS_FREESTANDING_USB_SOURCES) $(MACOS_PLATFORM_SOURCES))

$(BUILD_DIR)/linker: src/tools/linker.c $(LINKER_TOOL_SOURCES) $(LINKER_SIGNING_SOURCE) src/compiler/linker.h src/compiler/compiler.h src/compiler/source.h $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_LINKER_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=1 $< $(LINKER_TOOL_SOURCES) $(LINKER_SIGNING_SOURCE) $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@

$(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.o: tests/fixtures/macho/newlinker_tiny_start.c | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.o: tests/fixtures/macho/newlinker_tiny_helper.c | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.lto.o: tests/fixtures/macho/newlinker_tiny_start.c | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.lto.o: tests/fixtures/macho/newlinker_tiny_helper.c | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o: src/platform/macos/newlinker_start.S | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/%.lto.o: %.c $$(wildcard $$*/*.c $$*/*.h) src/shared/platform.h src/shared/runtime.h src/shared/tool_util.h src/platform/macos/trace.h | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -Isrc/shared -Isrc/compiler -Isrc/platform/macos -Isrc/arch/aarch64/macos -c $< -o $@

$(MACOS_BUILD_DIR)/.obj/src/platform/macos/profiler_runtime.lto.o: src/platform/macos/profiler_runtime.c | $(MACOS_BUILD_DIR)/.obj
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_CFLAGS) -Isrc/shared -Isrc/compiler -Isrc/platform/macos -Isrc/arch/aarch64/macos -c $< -o $@

$(MACOS_BUILD_DIR)/tiny: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.o $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.o | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) -o $@ $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.o $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.o

$(MACOS_BUILD_DIR)/tiny-lto: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.lto.o $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.lto.o | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_start.lto.o $(MACOS_BUILD_DIR)/.obj/newlinker_tiny_helper.lto.o

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_HASH_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_HASH_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_HASH_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_GIT_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_GIT_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_GIT_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_TLS_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_TLS_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_TLS_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_AWK_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_AWK_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_AWK_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_ARCHIVE_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_ARCHIVE_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_ARCHIVE_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_IMAGE_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_IMAGE_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_IMAGE_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_PGP_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_PGP_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_PGP_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_PGPQUERY_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_PGPQUERY_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_PGPQUERY_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_PDF_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_PDF_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_PDF_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_XML_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_XML_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_XML_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_NCC_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_NCC_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_NCC_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_SSH_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_SSH_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_SSH_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_SSHD_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_SSHD_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_SSHD_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_HTTPD_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_HTTPD_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_HTTPD_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_PING6_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/ping.lto.o $(MACOS_COMMON_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/ping.lto.o $(MACOS_COMMON_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_ALIAS_TOOLS)): $(MACOS_BUILD_DIR)/%: $(MACOS_BUILD_DIR)/ripgrep | $(MACOS_BUILD_DIR)
	rm -f $@ && ln -sfn ripgrep $@

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_SHELL_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_SHELL_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_SHELL_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_TUI_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_EDITOR_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_EDITOR_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_MAIL_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_MAIL_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_MAIL_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_MAKE_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_MAKE_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_MAKE_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_SERVICE_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_SERVICE_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_SERVICE_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_USB_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_USB_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_USB_OBJECTS)

$(addprefix $(MACOS_BUILD_DIR)/,$(MACOS_FREESTANDING_GENERIC_TOOLS)): $(MACOS_BUILD_DIR)/%: $(BUILD_DIR)/linker $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/%.lto.o $(MACOS_COMMON_OBJECTS) | $(MACOS_BUILD_DIR)
	$(BUILD_DIR)/linker --target=mach-o-arm64 $(MACOS_LDFLAGS) $(MACOS_MAP_FLAG) --lto-cc="$(MACOS_FREESTANDING_CC)" -o $@ $(MACOS_BUILD_DIR)/.obj/src/platform/macos/newlinker_start.o $(MACOS_BUILD_DIR)/.obj/src/tools/$*.lto.o $(MACOS_COMMON_OBJECTS)

$(TARGET_BUILD_DIR)/linker: src/tools/linker.c $(LINKER_TOOL_SOURCES) $(LINKER_SIGNING_SOURCE) src/compiler/linker.h src/compiler/compiler.h src/compiler/source.h $(TARGET_REUSABLE_OBJECTS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=1 $< $(LINKER_TOOL_SOURCES) $(LINKER_SIGNING_SOURCE) $(TARGET_REUSABLE_OBJECTS) $(PROFILE_RUNTIME_SOURCE) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/make: src/tools/make.c $(MAKE_TOOL_SOURCES) src/tools/make/make_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) src/tools/make.c $(MAKE_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/make: src/tools/make.c $(MAKE_TOOL_SOURCES) src/tools/make/make_impl.h $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) src/tools/make.c $(MAKE_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/awk: src/tools/awk.c $(AWK_TOOL_SOURCES) src/tools/awk/awk_impl.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) src/tools/awk.c $(AWK_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/awk: src/tools/awk.c $(AWK_TOOL_SOURCES) src/tools/awk/awk_impl.h $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) src/tools/awk.c $(AWK_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/httpd: src/tools/httpd.c $(HTTPD_TOOL_SOURCES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h src/shared/server_log.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(HTTPD_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/httpd: src/tools/httpd.c $(HTTPD_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h src/shared/server_log.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(HTTPD_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/service: src/tools/service.c $(SERVICE_TOOL_SOURCES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SERVICE_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/service: src/tools/service.c $(SERVICE_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SERVICE_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/editor: src/tools/editor.c $(EDITOR_TOOL_SOURCES) $(TUI_SOURCES) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(EDITOR_TOOL_SOURCES) $(TUI_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/editor: src/tools/editor.c $(EDITOR_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(EDITOR_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/mail: src/tools/mail.c $(MAIL_TOOL_SOURCES) $(TUI_SOURCES) $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(MAIL_TOOL_SOURCES) $(TUI_SOURCES) $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/lsusb: src/tools/lsusb.c $(USB_SOURCES) src/shared/usb.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_USB_PLATFORM_SOURCES) $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(USB_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) $(HOST_USB_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/mail: src/tools/mail.c $(MAIL_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_TLS_OBJECTS) $(TARGET_CRYPTO_OBJECTS) $(TARGET_TLS_PLATFORM_OBJECT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(MAIL_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_TLS_OBJECTS) $(TARGET_CRYPTO_OBJECTS) $(TARGET_TLS_PLATFORM_OBJECT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(TARGET_BUILD_DIR)/lsusb: src/tools/lsusb.c $(USB_SOURCES) src/shared/usb.h $(TARGET_USB_PLATFORM_SOURCES) $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(USB_SOURCES) $(TARGET_USB_PLATFORM_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/wtf: src/tools/wtf.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/wget: src/tools/wget.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/portscan: src/tools/portscan.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_EXPACK_CFLAGS) -DPORTSCAN_NO_TLS=1 $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/wtf: src/tools/wtf.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_TLS_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_TLS_OBJECTS) $(INCEPTION_CRYPTO_OBJECTS) $(INCEPTION_TARGET_TLS_PLATFORM_OBJECT) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(TARGET_BUILD_DIR)/wget: src/tools/wget.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_TLS_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_TLS_OBJECTS) $(INCEPTION_CRYPTO_OBJECTS) $(INCEPTION_TARGET_TLS_PLATFORM_OBJECT) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(TARGET_BUILD_DIR)/portscan: src/tools/portscan.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_TLS_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_TLS_OBJECTS) $(INCEPTION_CRYPTO_OBJECTS) $(INCEPTION_TARGET_TLS_PLATFORM_OBJECT) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

ifneq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
$(FREESTANDING_OBJECT_BUILD_DIR)/%.o: %.c src/compiler/source_manifest.h | $(FREESTANDING_OBJECT_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -c $< -o $@
endif

$(INCEPTION_OBJECT_BUILD_DIR)/%.o: %.c src/compiler/source_manifest.h | $(INCEPTION_OBJECT_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -c $< -o $@

$(addprefix $(BUILD_DIR)/,$(IMAGE_TOOLS)): $(BUILD_DIR)/%: src/tools/%.c $(IMAGE_SOURCES) src/shared/image/image.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(IMAGE_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(addprefix $(TARGET_BUILD_DIR)/,$(IMAGE_TOOLS)): $(TARGET_BUILD_DIR)/%: src/tools/%.c $(IMAGE_SOURCES) src/shared/image/image.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_IMAGE_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_IMAGE_OBJECTS) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_IMAGE_OBJECTS) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(addprefix $(BUILD_DIR)/,$(PGP_TOOLS)): $(BUILD_DIR)/%: src/tools/%.c $(PGP_SOURCES) src/shared/pgp.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(filter-out $(SHARED_SOURCES),$(PGP_SOURCES)) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(addprefix $(BUILD_DIR)/,$(PGPQUERY_TOOLS)): $(BUILD_DIR)/%: src/tools/%.c $(PGPQUERY_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) src/shared/pgp.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(filter-out $(SHARED_SOURCES),$(PGPQUERY_SOURCES)) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(addprefix $(TARGET_BUILD_DIR)/,$(PGP_TOOLS)): $(TARGET_BUILD_DIR)/%: src/tools/%.c $(PGP_SOURCES) src/shared/pgp.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_PGP_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_PGP_OBJECTS) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_PGP_OBJECTS) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(addprefix $(TARGET_BUILD_DIR)/,$(PGPQUERY_TOOLS)): $(TARGET_BUILD_DIR)/%: src/tools/%.c $(PGPQUERY_SOURCES) $(TARGET_TLS_PLATFORM_SOURCE) src/shared/pgp.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_PGPQUERY_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_PGPQUERY_OBJECTS) $(INCEPTION_TARGET_TLS_PLATFORM_OBJECT) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_PGPQUERY_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(addprefix $(BUILD_DIR)/,$(PDF_TOOLS)): $(BUILD_DIR)/%: src/tools/%.c $(PDF_SOURCES) src/shared/pdf.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(PDF_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(addprefix $(TARGET_BUILD_DIR)/,$(PDF_TOOLS)): $(TARGET_BUILD_DIR)/%: src/tools/%.c $(PDF_SOURCES) src/shared/pdf.h $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_PDF_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_PDF_OBJECTS) $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_PDF_OBJECTS) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(BUILD_DIR)/bc: src/tools/bc.c src/shared/bignum.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) -Wno-pedantic $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/bc: src/tools/bc.c src/shared/bignum.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_SPECIAL_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) -Wno-pedantic $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) -Wno-pedantic $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_EXPACK_CFLAGS) $< $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/readelf: src/tools/readelf.c $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/archive_util.h src/shared/crypto/sha256.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_READELF_CFLAGS) $< $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/fonttest: $(FONTTEST_SOURCE) $(FONTRENDER_DEPS) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) -DFR_RASTER_DISABLE_SIMD=1 -Isrc/shared/fontrender $< $(FONTRENDER_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/fonttest: $(FONTTEST_SOURCE) $(FONTRENDER_DEPS) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -DFR_RASTER_DISABLE_SIMD=1 -Isrc/shared/fontrender $< $(FONTRENDER_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(TARGET_BUILD_DIR)/readelf: src/tools/readelf.c $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/archive_util.h src/shared/crypto/sha256.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(TARGET_BUILD_DIR)/threadtest: $(THREADTEST_SOURCE) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(INCEPTION_BUILD_DIR)/man: src/tools/man.c $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(PROFILE_RUNTIME_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(addprefix $(INCEPTION_BUILD_DIR)/,$(INCEPTION_UNICODE_TOOLS)): $(INCEPTION_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(PROFILE_RUNTIME_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(PROFILE_RUNTIME_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@

ifneq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
$(TARGET_BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(TARGET_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(PROFILE_RUNTIME_SOURCE) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(INCEPTION_BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(INCEPTION_BUILD_DIR)/readelf: src/tools/readelf.c $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/archive_util.h src/shared/crypto/sha256.h $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(PROFILE_RUNTIME_SOURCE) $(EXPACK_SIGNING_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(INCEPTION_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) $(PROFILE_RUNTIME_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(PROFILE_RUNTIME_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

clean:
	@for path in $(BUILD_ROOT) tests/tmp; do \
		if [ -e "$$path" ]; then \
			chmod -R u+rwX "$$path" 2>/dev/null || true; \
		fi; \
	done
	rm -rf $(BUILD_ROOT) tests/tmp
