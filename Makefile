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
WINDOWS_TARGET_ARCH ?= x86_64
WINDOWS_TARGET_TRIPLE ?= x86_64-w64-windows-gnu
WINDOWS_TARGET_CC ?= $(TARGET_CC)
WINDOWS_TARGET_CC_TARGET_FLAG ?= --target=$(WINDOWS_TARGET_TRIPLE)
MACOS_FREESTANDING_ARCH ?= aarch64
MACOS_FREESTANDING_TRIPLE ?= arm64-apple-macos11
MACOS_FREESTANDING_CC ?= $(shell xcrun --find clang 2>/dev/null || echo $(CC))
MACOS_FREESTANDING_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
MACOS_FREESTANDING_LTO ?= 1
MACOS_FREESTANDING_LTO_FLAGS ?= $(if $(filter 1 yes true,$(MACOS_FREESTANDING_LTO)),-flto)
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
FREESTANDING_LTO ?= 0
FREESTANDING_LTO_FLAGS ?= $(if $(filter 1 yes true,$(FREESTANDING_LTO)),$(shell "$(TARGET_CC)" --version 2>/dev/null | grep -qi clang && echo -flto))
FREESTANDING_CFLAGS ?= -ffreestanding -fno-builtin $(FREESTANDING_STACK_CFLAGS) -fno-unwind-tables -fno-asynchronous-unwind-tables $(FREESTANDING_SECTION_CFLAGS) $(FREESTANDING_PIE_CFLAGS) $(FREESTANDING_OPT_CFLAGS) $(FREESTANDING_LTO_FLAGS)
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
SELFHOST_GC_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-dead_strip,-Wl$(COMMA)--gc-sections)
SELFHOST_STRIP_LDFLAGS ?= $(if $(filter Darwin,$(HOST_OS)),-Wl$(COMMA)-x,-Wl$(COMMA)-s)
SELFHOST_SIZE_FLAGS ?= $(SELFHOST_SECTION_CFLAGS) $(SELFHOST_GC_LDFLAGS) $(SELFHOST_STRIP_LDFLAGS)
TARGET_LDFLAGS ?= -nostdlib $(FREESTANDING_PIE_LDFLAGS) $(TARGET_LINKER_FLAG) $(FREESTANDING_COMPACT_LDFLAGS) $(FREESTANDING_BUILD_ID_LDFLAGS) $(FREESTANDING_GC_LDFLAGS) $(FREESTANDING_STRIP_LDFLAGS) $(TARGET_BUILTINS_LIB) $(FREESTANDING_LTO_FLAGS)
BUILD_ROOT ?= build
HOST_OS_NAME := $(if $(filter Darwin,$(HOST_OS)),macos,$(shell printf '%s' "$(HOST_OS)" | tr A-Z a-z))
HOST_ARCH_NAME := $(if $(filter arm64 aarch64,$(HOST_ARCH)),aarch64,$(HOST_ARCH))
LOCAL_MACOS_FREESTANDING := $(if $(filter Darwin,$(HOST_OS)),$(if $(filter aarch64,$(HOST_ARCH_NAME)),1,0),0)
DEFAULT_HOST_BUILD_DIR := $(BUILD_ROOT)/host-$(HOST_OS_NAME)-$(HOST_ARCH_NAME)
BUILD_DIR ?= $(DEFAULT_HOST_BUILD_DIR)
TARGET_BUILD_DIR ?= $(BUILD_ROOT)/freestanding-linux-$(TARGET_ARCH)
WINDOWS_TARGET_BUILD_DIR ?= $(BUILD_ROOT)/freestanding-windows-$(WINDOWS_TARGET_ARCH)
MACOS_FREESTANDING_BUILD_DIR ?= $(BUILD_ROOT)/freestanding-macos-$(MACOS_FREESTANDING_ARCH)
SELFHOST_BUILD_DIR ?= $(BUILD_ROOT)/selfhost-$(HOST_OS_NAME)-$(HOST_ARCH_NAME)
HOST_SIZE_FLAGS ?= $(if $(filter $(BUILD_DIR),$(DEFAULT_HOST_BUILD_DIR)),$(HOST_SECTION_CFLAGS) $(HOST_GC_LDFLAGS))
HOST_CFLAGS ?= $(CFLAGS) $(HOST_SIZE_FLAGS)
EXPACK_HOST_PTHREAD_ENABLED := $(if $(filter $(BUILD_DIR),$(DEFAULT_HOST_BUILD_DIR)),$(if $(filter MSYS_NT% MINGW% CYGWIN%,$(HOST_OS)),,1))
EXPACK_HOST_THREAD_FLAGS ?= $(if $(EXPACK_HOST_PTHREAD_ENABLED),$(if $(filter Darwin,$(HOST_OS)),,-pthread))
EXPACK_HOST_THREAD_DEFS ?= $(if $(EXPACK_HOST_PTHREAD_ENABLED),-DNEWOS_RUNTIME_THREAD_SAFE_ALLOC=1 -DNEWOS_HAVE_PTHREAD=1,-DEXPACK_DISABLE_PTHREAD=1)
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
LOCAL_PLATFORM_ONLY ?= $(if $(filter Darwin,$(HOST_OS)),1,0)
DEFAULT_ALL_TARGETS := host
ifeq ($(LOCAL_PLATFORM_ONLY),0)
DEFAULT_ALL_TARGETS += freestanding
endif
TOOLS := sh ls cat clear echo pwd mkdir mount umount rm rmdir cp mv ln chmod chown chgrp mknod uname hostname init getty login dmesg logger stty touch gzip gunzip bzip2 bunzip2 xz unxz tar md5sum sha256sum sha512sum sleep env kill pgrep pkill shutdown wc head tail ps top sort cut tr grep ripgrep rg ping ping6 ip id whoami find sed awk date tee xargs dd od hexdump basename dirname realpath cmp diff file strings ar readelf objdump strip linker expack printf which readlink stat du df netcat portscan dhcp nslookup dig ssh scp sshd traceroute whois lsof sql ncc man test [ true false expr uniq seq mktemp yes less more watch wget wtf mail editor patch make tac nl paste join comm split csplit shuf fold fmt tsort sync truncate timeout time expand unexpand printenv ed bc pstree free uptime who users groups column rev httpd service imginfo imgcheck imgmeta c2pa xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo
INCEPTION_TOOLS ?= $(TOOLS)
WINDOWS_FREESTANDING_TOOLS ?= $(TOOLS)
WINDOWS_FREESTANDING_BIGNUM_TOOLS := bc expr seq
WINDOWS_FREESTANDING_HASH_TOOLS := md5sum sha256sum sha512sum
WINDOWS_FREESTANDING_IMAGE_TOOLS := imgmeta imginfo imgcheck c2pa
WINDOWS_FREESTANDING_REGEX_TOOLS := grep ripgrep rg sed csplit ed
WINDOWS_FREESTANDING_ARCHIVE_TOOLS := ar readelf objdump strip expack gzip gunzip bzip2 bunzip2 xz unxz tar
WINDOWS_FREESTANDING_AWK_TOOLS := awk
WINDOWS_FREESTANDING_XML_TOOLS := xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo
WINDOWS_FREESTANDING_TUI_TOOLS := editor
WINDOWS_FREESTANDING_MAIL_TOOLS := mail
WINDOWS_FREESTANDING_WGET_TOOLS := wget
WINDOWS_FREESTANDING_NCC_TOOLS := ncc
WINDOWS_FREESTANDING_SHELL_TOOLS := sh
WINDOWS_FREESTANDING_MAKE_TOOLS := make
WINDOWS_FREESTANDING_HTTPD_TOOLS := httpd
WINDOWS_FREESTANDING_SERVICE_TOOLS := service
WINDOWS_FREESTANDING_SSH_TOOLS := ssh
WINDOWS_FREESTANDING_SSHD_TOOLS := sshd
WINDOWS_FREESTANDING_ALIAS_TOOLS := ping6
MACOS_FREESTANDING_TOOLS ?= true false echo printf basename dirname yes rev seq expr test [ nl tac expand unexpand fold wc head tail cat cut tr uniq cmp comm join paste printenv pwd mkdir rmdir tee which readlink realpath sleep file strings hexdump od md5sum sha256sum sha512sum dd touch truncate sync bc split shuf fmt column tsort mktemp clear date uname hostname whoami id groups ls du stat df rm cp mv ln chmod chown chgrp free kill csplit sort env time timeout watch find ps pgrep pkill stty more less xargs grep sed ed patch diff logger wtf awk gzip gunzip bzip2 bunzip2 xz unxz tar ar readelf objdump strip expack imgmeta imginfo imgcheck c2pa xmltokens xmlcheck xmlfmt xmlmin xmlget xmlcut xmlgrep xmlcount xmlsafe xmlstrip xml2lines xmlcanon xmlnscheck xmlvalidate xmlrename xmldel xmlset xml2json xml2yaml xml2csv xmldiff xmlstats xmluniq xmlsort xmljoin xmlsplit xmltail xmlhead xmlquery xmlrecode xmldtdapply xmldtdinfo wget sql man pstree ncc netcat portscan nslookup dig ssh scp sshd traceroute whois lsof httpd ip ping ping6 sh mail editor make dhcp dmesg getty init login mknod mount rg ripgrep service shutdown top umount uptime users who
MACOS_FREESTANDING_HASH_TOOLS := md5sum sha256sum sha512sum
MACOS_FREESTANDING_TLS_TOOLS := wtf wget
MACOS_FREESTANDING_AWK_TOOLS := awk
MACOS_FREESTANDING_IMAGE_TOOLS := imgmeta imginfo imgcheck c2pa
MACOS_FREESTANDING_ARCHIVE_TOOLS := gzip gunzip bzip2 bunzip2 xz unxz tar ar readelf objdump strip expack
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
MACOS_FREESTANDING_GENERIC_TOOLS := $(filter-out $(MACOS_FREESTANDING_HASH_TOOLS) $(MACOS_FREESTANDING_TLS_TOOLS) $(MACOS_FREESTANDING_AWK_TOOLS) $(MACOS_FREESTANDING_IMAGE_TOOLS) $(MACOS_FREESTANDING_ARCHIVE_TOOLS) $(MACOS_FREESTANDING_XML_TOOLS) $(MACOS_FREESTANDING_NCC_TOOLS) $(MACOS_FREESTANDING_SSH_TOOLS) $(MACOS_FREESTANDING_SSHD_TOOLS) $(MACOS_FREESTANDING_HTTPD_TOOLS) $(MACOS_FREESTANDING_PING6_TOOLS) $(MACOS_FREESTANDING_SHELL_TOOLS) $(MACOS_FREESTANDING_MAIL_TOOLS) $(MACOS_FREESTANDING_TUI_TOOLS) $(MACOS_FREESTANDING_MAKE_TOOLS) $(MACOS_FREESTANDING_SERVICE_TOOLS),$(MACOS_FREESTANDING_TOOLS))
INCEPTION_BUILD_DIR ?= $(BUILD_ROOT)/inception-freestanding-$(TARGET_ARCH)
INCEPTION_OBJECT_BUILD_DIR ?= $(INCEPTION_BUILD_DIR)/.obj
FREESTANDING_OBJECT_BUILD_DIR ?= $(TARGET_BUILD_DIR)/.obj
INCEPTION_JOBS ?= $(PARALLEL_JOBS)
INCEPTION_TARGETS := $(addprefix $(INCEPTION_BUILD_DIR)/,$(INCEPTION_TOOLS))
TOOL_SOURCES := $(addprefix src/tools/,$(addsuffix .c,$(TOOLS)))
COMPILER_SOURCES := $(shell grep -oE '"src/compiler/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
COMPILER_IMPL_INCLUDES := \
	src/compiler/backend_internal.h \
	src/compiler/parser_internal.h \
	src/compiler/targets/target_info.h
SHARED_SOURCES := $(shell grep -oE '"src/shared/(runtime/[^"]+|compression/[^"]+|tool_[^"]+|archive_util|bignum|simple_config|server_log|xml|xml_stream|xml_dtd)\.c"' src/compiler/source_manifest.h | tr -d '"')
SHARED_DEPS := $(SHARED_SOURCES) src/compiler/source_manifest.h
IMAGE_SOURCES := $(shell grep -oE '"src/shared/(image/[^"]+|crypto/(sha256|p256))\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
IMAGE_TOOLS := imginfo imgcheck imgmeta c2pa
CRYPTO_SOURCES := $(shell grep -oE '"src/shared/crypto/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
TLS_SOURCES := $(shell grep -oE '"src/shared/tls/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"' | sort -u)
TUI_SOURCES := $(shell grep -oE '"src/shared/tui\.c"' src/compiler/source_manifest.h | tr -d '"')
HASH_SOURCES := \
	$(shell grep -oE '"src/shared/hash_util\.c"' src/compiler/source_manifest.h | tr -d '"') \
	$(CRYPTO_SOURCES)
SSH_TRANSPORT_SOURCES := $(shell grep -oE '"src/tools/ssh/ssh_(core|client_io)\.c"' src/compiler/source_manifest.h | tr -d '"')
SSH_CLIENT_SOURCES := $(shell grep -oE '"src/tools/ssh/ssh_(core|known_hosts|client[^"]*)\.c"' src/compiler/source_manifest.h | tr -d '"')
SSHD_TOOL_SOURCES := $(shell grep -oE '"src/tools/sshd/sshd_[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
EXPACK_SIGNING_SOURCE := src/shared/crypto/sha256.c
EXPACK_PRIVATE_SOURCES := src/tools/expack/internal.h src/tools/expack/codecs.c src/tools/expack/formats.c src/tools/expack/outputs.c src/tools/expack/outputs_elf.c src/tools/expack/outputs_pe.c src/tools/expack/macho_arm64_runner_template.c src/tools/expack/macho_arm64_lzrep_runner.inc src/tools/expack/macho_arm64_lzss_runner.inc src/tools/expack/macho_arm64_lz4_runner.inc
SSH_CRYPTO_SOURCES := \
	$(CRYPTO_SOURCES) \
	src/shared/crypto/curve25519.c \
	src/shared/crypto/ed25519.c \
	src/shared/crypto/chacha20_poly1305.c \
	src/shared/crypto/ssh_kdf.c
SSH_CRYPTO_SOURCES := $(sort $(SSH_CRYPTO_SOURCES))
SHELL_SOURCES := $(shell grep -oE '"src/tools/sh/shell_[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
HOST_PLATFORM_SOURCES := $(shell grep -oE '"src/platform/posix/[^"]+\.c"' src/compiler/source_manifest.h | tr -d '"')
WINDOWS_FREESTANDING_RUNTIME_SOURCES := src/shared/runtime/memory.c src/shared/runtime/string.c src/shared/runtime/parse.c src/shared/runtime/io.c src/shared/runtime/unicode_utf8.c src/shared/runtime/unicode.c src/shared/tool_json.c src/shared/tool_cli.c src/shared/tool_io.c src/shared/tool_file.c src/shared/tool_path.c src/shared/tool_fs.c src/shared/tool_regex.c src/shared/tool_process.c src/platform/windows/core.c
WINDOWS_FREESTANDING_TLS_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) src/platform/windows/tls.c
WINDOWS_FREESTANDING_IMAGE_SOURCES := $(IMAGE_SOURCES) src/shared/compression/crc32.c src/shared/compression/zlib.c
WINDOWS_FREESTANDING_HASH_SOURCES := src/shared/hash_util.c src/shared/crypto/md5.c src/shared/crypto/sha256.c src/shared/crypto/sha512.c
WINDOWS_FREESTANDING_REGEX_SOURCES :=
WINDOWS_FREESTANDING_ARCHIVE_SOURCES := src/shared/archive_util.c src/shared/compression/crc32.c src/shared/compression/lzss.c $(EXPACK_SIGNING_SOURCE)
WINDOWS_FREESTANDING_AWK_SOURCES := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c $(WINDOWS_FREESTANDING_REGEX_SOURCES)
WINDOWS_FREESTANDING_XML_SOURCES := src/shared/xml.c src/shared/xml_stream.c src/shared/xml_dtd.c src/shared/tool_xml.c $(WINDOWS_FREESTANDING_REGEX_SOURCES)
WINDOWS_FREESTANDING_TUI_SOURCES := src/shared/tui.c
WINDOWS_FREESTANDING_EDITOR_SOURCES := src/tools/editor/highlight.c $(WINDOWS_FREESTANDING_TUI_SOURCES)
WINDOWS_FREESTANDING_MAIL_SOURCES := src/tools/mail/imap.c src/tools/mail/message.c src/tools/mail/mime.c $(WINDOWS_FREESTANDING_TUI_SOURCES) $(WINDOWS_FREESTANDING_TLS_SOURCES)
WINDOWS_FREESTANDING_NCC_SOURCES := $(COMPILER_SOURCES) $(SHARED_SOURCES)
WINDOWS_FREESTANDING_SHELL_SOURCES := $(SHELL_SOURCES) $(SHARED_SOURCES)
WINDOWS_FREESTANDING_MAKE_SOURCES = $(MAKE_TOOL_SOURCES) $(SHARED_SOURCES)
WINDOWS_FREESTANDING_HTTPD_SOURCES = $(HTTPD_TOOL_SOURCES) $(SHARED_SOURCES)
WINDOWS_FREESTANDING_SERVICE_SOURCES = $(SERVICE_TOOL_SOURCES) $(SHARED_SOURCES)
WINDOWS_FREESTANDING_SSH_SOURCES := $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) $(TLS_SOURCES) src/platform/windows/tls.c $(SHARED_SOURCES)
WINDOWS_FREESTANDING_SSHD_SOURCES := $(SSHD_TOOL_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) $(TLS_SOURCES) src/platform/windows/tls.c $(SHARED_SOURCES)
WINDOWS_FREESTANDING_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Isrc/shared -Isrc/platform/windows
WINDOWS_FREESTANDING_LDFLAGS ?= -nostdlib -fuse-ld=lld -Wl$(COMMA)-e$(COMMA)mainCRTStartup -Wl$(COMMA)-s -Wl$(COMMA)--gc-sections -Wl$(COMMA)--stack$(COMMA)8388608 -lkernel32 -lws2_32
WINDOWS_FREESTANDING_TLS_LDFLAGS ?= $(WINDOWS_FREESTANDING_LDFLAGS) -lbcrypt
MACOS_FREESTANDING_RUNTIME_SOURCES := src/shared/runtime/memory.c src/shared/runtime/string.c src/shared/runtime/parse.c src/shared/runtime/io.c src/shared/runtime/unicode_utf8.c src/shared/runtime/unicode.c src/shared/tool_json.c src/shared/tool_cli.c src/shared/tool_file.c src/shared/tool_io.c src/shared/tool_path.c src/shared/tool_fs.c src/shared/tool_regex.c src/shared/tool_process.c src/shared/bignum.c src/platform/macos/freestanding.c
MACOS_FREESTANDING_HASH_SOURCES := src/shared/hash_util.c src/shared/crypto/md5.c src/shared/crypto/sha256.c src/shared/crypto/sha512.c
MACOS_FREESTANDING_TLS_SOURCES := $(TLS_SOURCES) $(CRYPTO_SOURCES) src/platform/macos/tls.c
MACOS_FREESTANDING_AWK_SOURCES := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c
MACOS_FREESTANDING_ARCHIVE_SOURCES := src/shared/archive_util.c src/shared/compression/crc32.c src/shared/compression/lzss.c src/shared/compression/zlib.c $(EXPACK_SIGNING_SOURCE)
MACOS_FREESTANDING_IMAGE_SOURCES := $(IMAGE_SOURCES) src/shared/compression/crc32.c src/shared/compression/zlib.c
MACOS_FREESTANDING_XML_SOURCES := src/shared/xml.c src/shared/xml_stream.c src/shared/xml_dtd.c src/shared/tool_xml.c
MACOS_FREESTANDING_NCC_SOURCES := $(COMPILER_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_SSH_SOURCES := $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_SSHD_SOURCES := $(SSHD_TOOL_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_HTTPD_SOURCES = $(HTTPD_TOOL_SOURCES) $(SHARED_SOURCES)
MACOS_FREESTANDING_SHELL_SOURCES := $(SHELL_SOURCES)
MACOS_FREESTANDING_EDITOR_SOURCES = $(EDITOR_TOOL_SOURCES) $(TUI_SOURCES)
MACOS_FREESTANDING_MAIL_SOURCES = $(MAIL_TOOL_SOURCES) $(TUI_SOURCES) $(MACOS_FREESTANDING_TLS_SOURCES)
MACOS_FREESTANDING_MAKE_SOURCES = $(MAKE_TOOL_SOURCES)
MACOS_FREESTANDING_SERVICE_SOURCES = $(SERVICE_TOOL_SOURCES) src/shared/simple_config.c
MACOS_FREESTANDING_CFLAGS ?= -target $(MACOS_FREESTANDING_TRIPLE) $(if $(MACOS_FREESTANDING_SDKROOT),-isysroot $(MACOS_FREESTANDING_SDKROOT)) -std=c11 -Wall -Wextra -Wpedantic -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections $(MACOS_FREESTANDING_LTO_FLAGS) -Isrc/shared -Isrc/platform/macos -Isrc/arch/aarch64/macos
MACOS_FREESTANDING_LDFLAGS ?= -nodefaultlibs -lSystem -Wl$(COMMA)-dead_strip -Wl$(COMMA)-x -Wl$(COMMA)-no_function_starts -Wl$(COMMA)-adhoc_codesign $(MACOS_FREESTANDING_LTO_FLAGS)
MACOS_FREESTANDING_NO_LTO_CFLAGS := $(filter-out $(MACOS_FREESTANDING_LTO_FLAGS),$(MACOS_FREESTANDING_CFLAGS))
MACOS_FREESTANDING_NO_LTO_LDFLAGS := $(filter-out $(MACOS_FREESTANDING_LTO_FLAGS),$(MACOS_FREESTANDING_LDFLAGS))
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
FREESTANDING_CRYPTO_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(CRYPTO_SOURCES))
FREESTANDING_SSH_CRYPTO_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(SSH_CRYPTO_SOURCES))
FREESTANDING_TLS_OBJECTS := $(patsubst %.c,$(FREESTANDING_OBJECT_BUILD_DIR)/%.o,$(TLS_SOURCES))
FREESTANDING_TUI_OBJECT = $(FREESTANDING_OBJECT_BUILD_DIR)/$(TUI_SOURCES:.c=.o)
FREESTANDING_TARGET_TLS_PLATFORM_OBJECT = $(FREESTANDING_OBJECT_BUILD_DIR)/$(TARGET_TLS_PLATFORM_SOURCE:.c=.o)
INCEPTION_REUSABLE_SOURCES := $(filter-out src/shared/runtime/unicode.c,$(filter %.c,$(SHARED_SOURCES) $(TARGET_PLATFORM_SOURCES)))
INCEPTION_REUSABLE_OBJECTS := $(patsubst %.c,$(INCEPTION_OBJECT_BUILD_DIR)/%.o,$(INCEPTION_REUSABLE_SOURCES))
INCEPTION_UNICODE_OBJECT := $(INCEPTION_OBJECT_BUILD_DIR)/src/shared/runtime/unicode.o
INCEPTION_IMAGE_OBJECTS := $(patsubst %.c,$(INCEPTION_OBJECT_BUILD_DIR)/%.o,$(IMAGE_SOURCES))
INCEPTION_CRYPTO_OBJECTS := $(patsubst %.c,$(INCEPTION_OBJECT_BUILD_DIR)/%.o,$(CRYPTO_SOURCES))
INCEPTION_SSH_CRYPTO_OBJECTS := $(patsubst %.c,$(INCEPTION_OBJECT_BUILD_DIR)/%.o,$(SSH_CRYPTO_SOURCES))
INCEPTION_TLS_OBJECTS := $(patsubst %.c,$(INCEPTION_OBJECT_BUILD_DIR)/%.o,$(TLS_SOURCES))
INCEPTION_TARGET_TLS_PLATFORM_OBJECT = $(INCEPTION_OBJECT_BUILD_DIR)/$(TARGET_TLS_PLATFORM_SOURCE:.c=.o)
TARGET_REUSABLE_OBJECTS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_REUSABLE_OBJECTS) $(TARGET_ARCH_DIR)/syscall_stubs.S,$(FREESTANDING_REUSABLE_INPUTS))
TARGET_IMAGE_OBJECTS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_IMAGE_OBJECTS),$(FREESTANDING_IMAGE_OBJECTS))
TARGET_CRYPTO_OBJECTS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_CRYPTO_OBJECTS),$(FREESTANDING_CRYPTO_OBJECTS))
TARGET_SSH_CRYPTO_OBJECTS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_SSH_CRYPTO_OBJECTS),$(FREESTANDING_SSH_CRYPTO_OBJECTS))
TARGET_TLS_OBJECTS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_TLS_OBJECTS),$(FREESTANDING_TLS_OBJECTS))
TARGET_TLS_PLATFORM_OBJECT = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_TARGET_TLS_PLATFORM_OBJECT),$(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT))
TARGET_TUI_INPUT = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(TUI_SOURCES),$(FREESTANDING_TUI_OBJECT))
TARGET_UNICODE_OBJECT = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_UNICODE_OBJECT))
TARGET_SPECIAL_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_SPECIAL_PREREQS),$(FREESTANDING_REUSABLE_INPUTS))
TARGET_IMAGE_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_IMAGE_PREREQS),$(FREESTANDING_IMAGE_OBJECTS) $(FREESTANDING_REUSABLE_INPUTS))
TARGET_TLS_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_TLS_PREREQS),$(FREESTANDING_TLS_OBJECTS) $(FREESTANDING_CRYPTO_OBJECTS) $(FREESTANDING_TARGET_TLS_PLATFORM_OBJECT) $(FREESTANDING_REUSABLE_INPUTS))
INCEPTION_UNICODE_TOOLS := wc fold fmt expand unexpand column
INCEPTION_SPECIAL_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT))
INCEPTION_IMAGE_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_IMAGE_OBJECTS) $(INCEPTION_SPECIAL_PREREQS))
INCEPTION_TLS_PREREQS = $(if $(filter $(INCEPTION_BUILD_DIR),$(TARGET_BUILD_DIR)),$(INCEPTION_TLS_OBJECTS) $(INCEPTION_CRYPTO_OBJECTS) $(INCEPTION_TARGET_TLS_PLATFORM_OBJECT) $(INCEPTION_SPECIAL_PREREQS))
HOST_OUTPUTS := $(BUILD_DIR)/.ssh_core_check $(addprefix $(BUILD_DIR)/,$(TOOLS))
HOST_COMPAT_TARGETS := $(if $(filter $(BUILD_DIR),$(DEFAULT_HOST_BUILD_DIR)),$(BUILD_ROOT)/.ssh_core_check $(addprefix $(BUILD_ROOT)/,$(TOOLS)))

.DEFAULT_GOAL := all
.SECONDEXPANSION:

.PHONY: all host freestanding freestanding-newlinker freestanding-macos selfhost inception test test-phase1 test-smoke test-freestanding test-inception test-newlinker-expack test-newlinker-optimizations newlinker-size-report benchmark clean

test: test-freestanding test-phase1 test-smoke

test-phase1: host
	PHASE1_JOBS=$(PHASE1_JOBS) sh ./tests/phase1/run_phase1_tests.sh

test-smoke: host
	SKIP_PHASE1=1 sh ./tests/run_smoke_tests.sh

test-inception: inception
	NEWOS_INCEPTION_BUILD_DIR="$(abspath $(INCEPTION_BUILD_DIR))" sh ./tests/suites/inception.sh

freestanding-newlinker: $(BUILD_DIR)/linker
	LINKER="$(abspath $(BUILD_DIR)/linker)" bash build-freestanding-newlinker.sh

test-newlinker-expack: $(BUILD_DIR)/expack
	NEWOS_EXPACK="$(abspath $(BUILD_DIR)/expack)" bash ./tests/suites/newlinker_expack.sh

test-newlinker-optimizations: $(BUILD_DIR)/linker
	NEWOS_LINKER="$(abspath $(BUILD_DIR)/linker)" bash ./tests/suites/newlinker_optimizations.sh

newlinker-size-report: $(BUILD_DIR)/linker
	LINKER="$(abspath $(BUILD_DIR)/linker)" bash report-newlinker-size.sh

ifeq ($(LOCAL_PLATFORM_ONLY),1)
test-freestanding:
	@echo "Skipping freestanding tests on this host"
else
test-freestanding: freestanding
	NEWOS_FREESTANDING_BUILD_DIR="$(abspath $(TARGET_BUILD_DIR))" sh ./tests/suites/freestanding.sh
endif

benchmark: host
	./tests/benchmarks/run_benchmarks.sh

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
freestanding: freestanding-macos
else ifeq ($(LOCAL_PLATFORM_ONLY),1)
freestanding: host
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
host: $(HOST_OUTPUTS) $(HOST_COMPAT_TARGETS)
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
freestanding: freestanding-macos
else ifeq ($(LOCAL_PLATFORM_ONLY),1)
freestanding: host
else
freestanding: $(TARGET_BUILD_DIR)/.ssh_core_check $(addprefix $(TARGET_BUILD_DIR)/,$(TOOLS))
endif
else
host:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host
freestanding:
ifeq ($(LOCAL_MACOS_FREESTANDING),1)
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding-macos
else ifeq ($(LOCAL_PLATFORM_ONLY),1)
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) host
else
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding
endif
endif

selfhost: $(DEFAULT_HOST_BUILD_DIR)/ncc
	+@$(MAKE) --no-print-directory BUILD_DIR="$(SELFHOST_BUILD_DIR)" CC="$(abspath $(DEFAULT_HOST_BUILD_DIR)/ncc)" CFLAGS="$(CFLAGS) $(SELFHOST_SIZE_FLAGS)" HOST_PLATFORM_SOURCES="$(SELFHOST_PLATFORM_SOURCES)" NEWOS_NCC_LINKER="$${NEWOS_NCC_LINKER:-cc}" host

inception: $(TARGET_BUILD_DIR)/ncc
	+@$(MAKE) --no-print-directory -j$(INCEPTION_JOBS) TARGET_CC="$(abspath $(TARGET_BUILD_DIR)/ncc)" TARGET_CC_TARGET_FLAG=--target=linux-x86_64 TARGET_BUILD_DIR="$(INCEPTION_BUILD_DIR)" FREESTANDING_OPT_CFLAGS=-Os FREESTANDING_SECTION_CFLAGS= FREESTANDING_PIE_CFLAGS= TARGET_LINKER_FLAG= FREESTANDING_COMPACT_LDFLAGS= FREESTANDING_BUILD_ID_LDFLAGS= TARGET_BUILTINS_LIB= TARGET_LDFLAGS="-nostdlib -static -Wl,--gc-sections -Wl,-s" $(INCEPTION_TARGETS)

ifeq ($(AUTO_PARALLEL),1)
freestanding-macos: $(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_TOOLS))
else ifneq ($(strip $(PARALLEL_MAKEFLAGS)),)
freestanding-macos: $(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_TOOLS))
else
freestanding-macos:
	+@$(MAKE) --no-print-directory AUTO_PARALLEL=1 -j$(PARALLEL_JOBS) freestanding-macos
endif

$(sort $(BUILD_ROOT) $(BUILD_DIR) $(TARGET_BUILD_DIR) $(MACOS_FREESTANDING_BUILD_DIR) $(SELFHOST_BUILD_DIR) $(FREESTANDING_OBJECT_BUILD_DIR) $(INCEPTION_OBJECT_BUILD_DIR)):
	mkdir -p $@

$(BUILD_ROOT)/.ssh_core_check: $(BUILD_DIR)/.ssh_core_check | $(BUILD_ROOT)
	rm -f $@ && ln -sfn $(patsubst $(BUILD_ROOT)/%,%,$<) $@

$(BUILD_ROOT)/%: $(BUILD_DIR)/% | $(BUILD_ROOT)
	rm -f $@ && ln -sfn $(patsubst $(BUILD_ROOT)/%,%,$<) $@

$(BUILD_DIR)/ping6: $(BUILD_DIR)/ping | $(BUILD_DIR)
	rm -f $@ && ln -sfn ping $@

$(TARGET_BUILD_DIR)/ping6: $(TARGET_BUILD_DIR)/ping | $(TARGET_BUILD_DIR)
	rm -f $@ && ln -sfn ping $@

$(BUILD_DIR)/.ssh_core_check: $(SSH_CLIENT_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/tools/ssh/ssh_transport.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $(HOST_SECTION_CFLAGS) -fsyntax-only $(SSH_CLIENT_SOURCES) && : > $@

$(TARGET_BUILD_DIR)/.ssh_core_check: $(SSH_CLIENT_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/tools/ssh/ssh_transport.h src/shared/platform.h src/shared/runtime.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -fsyntax-only $(SSH_CLIENT_SOURCES) && : > $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_HASH_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_HASH_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_HASH_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_TLS_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_TLS_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_TLS_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_AWK_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_AWK_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_AWK_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_ARCHIVE_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(EXPACK_PRIVATE_SOURCES) $(MACOS_FREESTANDING_ARCHIVE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_ARCHIVE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_IMAGE_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_IMAGE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_IMAGE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_XML_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_XML_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_NO_LTO_CFLAGS) $< $(MACOS_FREESTANDING_XML_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_NO_LTO_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_NCC_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_NCC_SOURCES) src/compiler/source_manifest.h src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_NO_LTO_CFLAGS) -Isrc/compiler $< $(MACOS_FREESTANDING_NCC_SOURCES) src/platform/macos/freestanding.c $(MACOS_FREESTANDING_NO_LTO_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_SSH_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_SSH_SOURCES) src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_SSH_SOURCES) src/platform/macos/freestanding.c $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_SSHD_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_SSHD_SOURCES) src/tools/sshd/sshd.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_transport.h src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_SSHD_SOURCES) src/platform/macos/freestanding.c $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_HTTPD_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_HTTPD_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h src/shared/server_log.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_HTTPD_SOURCES) src/platform/macos/freestanding.c $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_PING6_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/ping.c $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_SHELL_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_SHELL_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_SHELL_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_TUI_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_EDITOR_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_EDITOR_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_MAIL_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_MAIL_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_MAIL_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_MAKE_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_MAKE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/tools/make/make_impl.h src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_MAKE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_SERVICE_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_SERVICE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/tools/service/service_impl.h src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/simple_config.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_SERVICE_SOURCES) $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(addprefix $(MACOS_FREESTANDING_BUILD_DIR)/,$(MACOS_FREESTANDING_GENERIC_TOOLS)): $(MACOS_FREESTANDING_BUILD_DIR)/%: src/tools/%.c $(MACOS_FREESTANDING_RUNTIME_SOURCES) src/shared/bignum.h src/shared/runtime.h src/shared/platform.h src/arch/aarch64/macos/syscall.h | $(MACOS_FREESTANDING_BUILD_DIR)
	mkdir -p $(dir $@) && $(MACOS_FREESTANDING_CC) $(MACOS_FREESTANDING_CFLAGS) $< $(MACOS_FREESTANDING_RUNTIME_SOURCES) $(MACOS_FREESTANDING_LDFLAGS) -o $@

$(BUILD_DIR)/sh: src/tools/sh.c $(SHARED_SOURCES) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(SHELL_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sh: src/tools/sh.c $(TARGET_REUSABLE_OBJECTS) $(SHELL_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sh/shell_shared.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SHELL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_NCC_CFLAGS) $< $(COMPILER_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ncc: src/tools/ncc.c $(COMPILER_SOURCES) $(COMPILER_IMPL_INCLUDES) src/compiler/source_manifest.h $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/compiler/backend.h src/compiler/backend_internal.h src/compiler/compiler.h src/compiler/object_writer.h src/compiler/source.h src/compiler/lexer.h src/compiler/ir.h src/compiler/parser.h src/compiler/preprocessor.h src/compiler/semantic.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=0 $< $(COMPILER_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/md5sum $(BUILD_DIR)/sha256sum $(BUILD_DIR)/sha512sum: $(BUILD_DIR)/%: src/tools/%.c $(SHARED_SOURCES) $(HASH_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(HASH_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/md5sum $(TARGET_BUILD_DIR)/sha256sum $(TARGET_BUILD_DIR)/sha512sum: $(TARGET_BUILD_DIR)/%: src/tools/%.c $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRYPTO_OBJECTS) src/shared/hash_util.c src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/hash_util.h src/shared/crypto/crypto_util.h src/shared/crypto/md5.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< src/shared/hash_util.c $(TARGET_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/ssh: src/tools/ssh.c $(SHARED_SOURCES) $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(SSH_CLIENT_SOURCES) $(SSH_CRYPTO_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/ssh: src/tools/ssh.c $(TARGET_REUSABLE_OBJECTS) $(SSH_CLIENT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_known_hosts.h src/tools/ssh/ssh_client.h src/tools/ssh/ssh_client_internal.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/sha512.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SSH_CLIENT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/sshd: src/tools/sshd.c $(SSHD_TOOL_SOURCES) $(SHARED_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sshd/sshd.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_transport.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SSHD_TOOL_SOURCES) $(SHARED_SOURCES) $(SSH_TRANSPORT_SOURCES) $(SSH_CRYPTO_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/sshd: src/tools/sshd.c $(SSHD_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(SSH_TRANSPORT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/tools/sshd/sshd.h src/tools/ssh/ssh_core.h src/tools/ssh/ssh_transport.h src/shared/crypto/crypto_util.h src/shared/crypto/sha256.h src/shared/crypto/curve25519.h src/shared/crypto/ed25519.h src/shared/crypto/chacha20_poly1305.h src/shared/crypto/ssh_kdf.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(SSHD_TOOL_SOURCES) $(SSH_TRANSPORT_SOURCES) $(TARGET_SSH_CRYPTO_OBJECTS) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

MAKE_TOOL_SOURCES := src/tools/make/make_parse.c src/tools/make/make_exec.c
LINKER_TOOL_SOURCES := src/compiler/linker.c
HOST_LINKER_CFLAGS = $(filter-out -Isrc/shared,$(CFLAGS) $(HOST_SIZE_FLAGS)) -idirafter src/shared
HOST_EXPACK_CFLAGS = $(filter-out -Isrc/shared,$(HOST_CFLAGS)) -idirafter src/shared
HOST_NCC_CFLAGS = $(filter-out -Isrc/shared,$(HOST_CFLAGS)) -DCOMPILER_LINKER_ENABLE_REPORTING=0 -idirafter src/shared
AWK_TOOL_SOURCES  := src/tools/awk/awk_parse.c src/tools/awk/awk_exec.c
SERVICE_TOOL_SOURCES := src/tools/service/service_main.c src/tools/service/service_pidfile.c src/tools/service/service_spawn.c src/tools/service/service_signal.c src/tools/service/service_config.c
HTTPD_TOOL_SOURCES := src/tools/httpd/httpd_main.c src/tools/httpd/http_listener.c src/tools/httpd/http_conn.c src/tools/httpd/http_parse.c src/tools/httpd/http_route.c src/tools/httpd/http_static.c src/tools/httpd/http_log.c
EDITOR_TOOL_SOURCES := src/tools/editor/highlight.c
MAIL_TOOL_SOURCES := src/tools/mail/imap.c src/tools/mail/message.c src/tools/mail/mime.c
HOST_TLS_PLATFORM_SOURCE := src/platform/posix/tls.c
TARGET_TLS_PLATFORM_SOURCE := src/platform/linux/tls.c

$(BUILD_DIR)/linker: src/tools/linker.c $(LINKER_TOOL_SOURCES) src/compiler/linker.h src/compiler/compiler.h src/compiler/source.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_LINKER_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=1 $< $(LINKER_TOOL_SOURCES) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/linker: src/tools/linker.c $(LINKER_TOOL_SOURCES) src/compiler/linker.h src/compiler/compiler.h src/compiler/source.h $(TARGET_REUSABLE_OBJECTS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) -DCOMPILER_LINKER_ENABLE_REPORTING=1 $< $(LINKER_TOOL_SOURCES) $(TARGET_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

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

$(TARGET_BUILD_DIR)/mail: src/tools/mail.c $(MAIL_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_TLS_OBJECTS) $(TARGET_CRYPTO_OBJECTS) $(TARGET_TLS_PLATFORM_OBJECT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/tui.h $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(MAIL_TOOL_SOURCES) $(TARGET_TUI_INPUT) $(TARGET_TLS_OBJECTS) $(TARGET_CRYPTO_OBJECTS) $(TARGET_TLS_PLATFORM_OBJECT) $(TARGET_REUSABLE_OBJECTS) $(TARGET_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/wtf: src/tools/wtf.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/wget: src/tools/wget.c $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(TLS_SOURCES) $(CRYPTO_SOURCES) $(HOST_TLS_PLATFORM_SOURCE) $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

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

$(BUILD_DIR)/bc: src/tools/bc.c src/shared/bignum.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) -Wno-pedantic $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(TARGET_BUILD_DIR)/bc: src/tools/bc.c src/shared/bignum.h $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_SPECIAL_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) -Wno-pedantic $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) -Wno-pedantic $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(BUILD_DIR)/rg: src/tools/rg.c src/tools/ripgrep.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

$(BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_SOURCES) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_EXPACK_CFLAGS) $(EXPACK_HOST_THREAD_FLAGS) $(EXPACK_HOST_THREAD_DEFS) $< $(SHARED_SOURCES) $(EXPACK_SIGNING_SOURCE) $(HOST_PLATFORM_SOURCES) -o $@ $(EXPACK_HOST_THREAD_FLAGS)

$(TARGET_BUILD_DIR)/rg: src/tools/rg.c src/tools/ripgrep.c $(SHARED_SOURCES) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(TARGET_SPECIAL_PREREQS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
ifeq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
else
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(INCEPTION_BUILD_DIR)/man: src/tools/man.c $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(addprefix $(INCEPTION_BUILD_DIR)/,$(INCEPTION_UNICODE_TOOLS)): $(INCEPTION_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(INCEPTION_UNICODE_OBJECT) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(HOST_PLATFORM_SOURCES) $(SELFHOST_CC_DEP) | $(BUILD_DIR)
	mkdir -p $(dir $@) && $(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@

ifneq ($(TARGET_BUILD_DIR),$(INCEPTION_BUILD_DIR))
$(TARGET_BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_DEPS) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(EXPACK_SIGNING_SOURCE) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(TARGET_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(TARGET_PLATFORM_SOURCES) $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(TARGET_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(FREESTANDING_REUSABLE_INPUTS) $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@
endif

$(INCEPTION_BUILD_DIR)/expack: src/tools/expack.c $(EXPACK_PRIVATE_SOURCES) $(SHARED_DEPS) $(EXPACK_SIGNING_SOURCE) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h src/shared/crypto/sha256.h $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(EXPACK_SIGNING_SOURCE) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

$(INCEPTION_BUILD_DIR)/%: src/tools/%.c $$(wildcard src/tools/$$*/*.c src/tools/$$*/*.h) $(SHARED_DEPS) src/shared/runtime.h src/shared/platform.h src/shared/tool_util.h $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_CRT) $(TARGET_ARCH_DIR)/syscall.h src/platform/linux/common.h | $(INCEPTION_BUILD_DIR)
	mkdir -p $(dir $@) && $(TARGET_CC) $(TARGET_CC_TARGET_FLAG) $(CFLAGS) $(FREESTANDING_CFLAGS) $< $(INCEPTION_REUSABLE_OBJECTS) $(TARGET_ARCH_DIR)/syscall_stubs.S $(TARGET_CRT) $(TARGET_LDFLAGS) -o $@

clean:
	@for path in $(BUILD_ROOT) tests/tmp; do \
		if [ -e "$$path" ]; then \
			chmod -R u+rwX "$$path" 2>/dev/null || true; \
		fi; \
	done
	rm -rf $(BUILD_ROOT) tests/tmp
