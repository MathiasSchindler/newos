# WINDOWS

## NAME

windows - native no-CRT PE build model, limitations, and design choices

## DESCRIPTION

Windows has two deliberately separate build models in this repository.

The native model produces PE32+ executables from PowerShell with LLVM/Clang,
lld, and llvm-dlltool. These binaries use the project runtime and the native
platform implementation under `src/platform/windows/`; they do not link the
Microsoft C runtime, a MinGW runtime, or the MSYS2 POSIX runtime. Project code
is linked statically, while operating-system services are imported from the
Windows inbox DLLs Kernel32, Ws2_32, and, where cryptographic randomness is
needed, Bcrypt.

The hosted model runs inside MSYS2, uses `src/platform/posix/`, and depends on
the installed MSYS runtime. It remains useful for POSIX-oriented development
and comparison testing, but it does not exercise the native Windows platform
layer described by most of this page.

The native build is usable for broad compilation and selected real workflows,
but its build coverage is intentionally wider than its platform coverage. A
tool producing an `.exe` does not imply that every operation exposed by that
tool has a native Windows implementation yet.

## CURRENT MODEL

The native entry point is:

```
.\tests\windows\build-windows-freestanding.ps1
```

The dual-output script currently requires `aarch64-w64-windows-gnu`, selected
automatically on an ARM64 host. Production lld/LTO binaries are written to
`build/normal/`; the same source plans are also compiled as ordinary COFF and
linked with `--pack` into `build/packed/`. Use `-BuildDir` and `-PackedDir` to
select other locations.

The normal native build has the following properties:

- it requires LLVM/Clang, lld, and llvm-dlltool, but not Visual Studio, the
  Windows SDK, MSYS2, `make`, or a POSIX shell
- it uses GNU-flavored Windows target triples as a compiler and linker ABI
  choice; it does not pull in the MinGW C runtime
- it builds every tool declared in `TOOLS` except `ncc` by default; `ncc` can
  be requested explicitly with `-Tools ncc`
- it compiles each unique source-and-flag profile once and reuses that object
  across all tools needing the same profile
- it caches objects, compiler dependency files, command signatures, generated
  import archives, and unchanged final links under the selected build tree
- it runs independent compilation jobs in parallel using the logical processor
  count by default, while limiting the more memory-intensive LTO links to four
  concurrent jobs by default
- it uses Clang and lld as the reference production path for normal ARM64 PE
  output
- on ARM64 it also builds every selected tool through the in-tree PE linker;
  the packed directory contains a real `.boot`/`.load` image when that is
  smaller and an exact normal in-tree image otherwise

The root `Makefile` remains the registry for tool names and special Windows
source groups. Shared subsystem source lists come from
`src/compiler/source_manifest.h`, so the PowerShell builder does not maintain
independent copies of the compiler, crypto, TLS, image, XML, shell, SSH, and
other grouped manifests.

`make freestanding` is not currently the native Windows entry point. On Linux
it selects the Linux ABI build, and on supported local macOS hosts it selects
the project-linked Mach-O build. Use the PowerShell script explicitly for
native PE output.

Use `-Profile` with the same PowerShell builder to compile function-instrumented
tools into separate `build/profile-windows/normal/` and `build/profile-windows/packed/` trees. Set
`NEWOS_PROFILE` to an output path when running one; the standard native
`profiler` reads that trace, including worker thread ids. Minimal `true` and
`false` builds remain uninstrumented. See `profiler(1)` for the workflow and
its timing limitations.

For scaling measurements, run unprofiled binaries from the ordinary build:

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-scaling.ps1 -Tool zip
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-scaling.ps1 -Tool zip -ZipStoreOnly
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-scaling.ps1 -Tool sort
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-bzip2.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-bzip2.ps1 -Pattern Mixed
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\bench-grep-unzip.ps1
```

The benchmark generates private inputs under `tests/tmp/`, verifies output
hashes across worker counts, and reports median elapsed and process CPU time,
speedup, and efficiency. By default ZIP has twelve independent 2 MiB files;
change `-ZipFiles` and `-Workers 1,2,4,8,12` to test other widths. Short sort
runs make Windows process CPU accounting coarse, so use repeated elapsed-time
trials for conclusions. The ZIP inputs are deterministic high-entropy data,
not representative of every archive. Earlier compressed ZIP runs at eight and
twelve workers exposed a missing thread-safe allocator flag in the Windows
builder. With the atomic lock enabled, the isolated normal and packed builds
passed 51 repeated ZIP runs across those widths; other workloads still need
their own checks. Failed runs retain their input directory for diagnosis.
The bzip2 benchmark needs Python's standard `bz2` module only to generate a
repeatable multi-block fixture; the decoder has no Python dependency. It
verifies the complete decoded output and reports median scan, decode, and
output times. `-Pattern Mixed` alternates 128 zero bytes with 128 seeded
random bytes for a more compressible fixture. Windows phase times use a coarse
system clock; use wall time
for small differences. On a 12 MiB incompressible fixture, the rolling-byte
scanner reduced the serial scan from about 250 ms to 78-94 ms, and the
twelve-worker median wall time from about 501 ms to 326 ms.
The grep/unzip benchmark uses sixteen deterministic 1 MiB text files in a
deflated ZIP. It verifies ordered `grep -c` counts and ZIP CRC checks at each
worker width, including rejection of a corrupted entry. On Windows ARM64,
four workers reduced grep from about 54 ms to 28 ms and `unzip -t` from about
191 ms to 64 ms; these figures are workload-specific. The optional
`-ProfileDir` setting collects function traces from an instrumented build.

## NATIVE STARTUP AND ABI

Normal tools enter through `mainCRTStartup` in `src/platform/windows/core.c`.
The startup copies and parses `GetCommandLineA()`, calls the tool's ordinary
`main(argc, argv)`, and terminates with `ExitProcess`. No CRT initialization or
libc startup runs first.

`true` and `false` are argument-independent and use the smaller startup in
`src/platform/windows/minimal_start.c`. It calls `main(0, 0)` directly and is
used to reach the validated minimum loader-safe image size.

The current ABI adapter has intentionally small fixed resources:

- command-line storage is 4096 bytes
- at most 63 arguments plus the terminating null pointer are retained
- the command-line parser handles whitespace and double-quoted spans, but is
  not a complete implementation of all Windows backslash-and-quote rules
- environment lookup uses a shared 4096-byte buffer and does not enumerate the
  process environment
- the project fd table has 64 entries and maps descriptors 0, 1, and 2 to the
  standard Windows handles; later entries represent file handles or sockets
- the current platform calls use narrow Win32 `A` interfaces, so full Unicode
  path and environment handling is not yet provided

The compiler emits Windows COFF objects. ARM64 and x86-64 stack probes live in
architecture-specific COMDAT sections under `src/arch/*/windows/chkstk.S`, so
the linker retains `__chkstk` only when a function actually needs it. The
normal build currently disables compiler stack-protector instrumentation. The
platform runtime still contains conditional guard initialization and failure
handling for profiles that enable it later.

## SYSTEM IMPORTS

The project owns small `.def` files under `src/platform/windows/imports/`.
llvm-dlltool turns them into target-specific import archives, avoiding a
dependency on SDK or MinGW import libraries.

- `KERNEL32.dll` supplies process exit, standard handles, file and directory
  operations, environment access, console modes, time, memory, disk-space,
  temporary-file, pipe, and sleep primitives
- `WS2_32.dll` supplies Winsock startup, name resolution for outgoing
  connections, sockets, connect/send/receive, select, and poll
- `BCRYPT.dll` supplies `BCryptGenRandom`; it is linked only into source groups
  that require native random bytes, TLS, SSH, Git, or PGP operations

This is a no-CRT model, not a no-operating-system-import model. Inbox Windows
DLLs are the native kernel-facing contract in the same way that raw syscalls
are the Linux contract.

## PE AND LINKER DESIGN

The production builder compiles with size-oriented freestanding flags,
including `-Oz`, `-ffreestanding`, `-fno-builtin`, function/data sections,
disabled unwind tables, and LTO. Final lld links use:

- `mainCRTStartup` as the explicit entry point
- no standard libraries
- section garbage collection and safe identical-code folding
- stripped symbols and no inserted PE timestamp
- an 8 MiB stack reserve
- merged read-only data and import payloads in the executable/read-only text
  section, while writable initialized data and BSS remain writable and
  separate

Normal loader alignment remains 4096 bytes for virtual sections and 512 bytes
for file data. A one-section image therefore has a practical 1024-byte floor:
one 512-byte header region followed by one 512-byte section. Native ARM64 tests
verify that `true.exe`, `false.exe`, and a simple imported executable meet that
size and still execute. Lower-alignment experiments are not used because they
were rejected by the Windows ARM64 loader.

The repository also has an in-tree PE backend selected with
`linker --target=pe-arm64`. This is an ARM64 linker bring-up path rather than
the general Windows production linker. It currently:

- consumes raw ARM64 COFF objects and project-owned `.def` files
- writes PE32+ executables with the normal Windows image base and alignment
- supports the ARM64 relocations needed by the current Clang-generated test
  objects
- resolves weak aliases and follows associative COMDAT relationships
- performs entry-rooted section garbage collection from `mainCRTStartup`
- removes imports referenced only by discarded sections
- places import descriptors and the IAT in the RX text output section
- emits a separate RW data section only when writable data or BSS is present

The in-tree PE backend does not yet consume archives or LTO objects, does not
target x86-64, and does not emit base-relocation tables. Images requiring
absolute-address rebasing are therefore outside its current supported model.
Clang/lld remains the required path for the complete native tool build.

## IMPLEMENTED PLATFORM SURFACE

The native implementation keeps Win32 declarations and behavior in
`src/platform/windows/` and exposes the shared `platform_*` interface to tools.
The currently implemented core includes:

- page allocation and release through `VirtualAlloc` and `VirtualFree`
- standard input/output/error, file open/read/write/append/seek/close,
  exclusive and temporary-file creation, truncation, and flushing
- current-directory lookup and change, file/directory removal, rename,
  directory creation, hard links, basic existence/type/size metadata, and
  filesystem capacity reporting
- environment lookup, set, and unset for individual variables
- process id, hostname lookup, uname-style reporting, epoch and monotonic time,
  sleeping, selected time formatting, physical-memory reporting, and uptime
- Windows console detection, dimensions, selected terminal flags, raw input,
  and virtual-terminal input/output modes
- anonymous pipes
- outgoing TCP connections, socket read/write/close, and Winsock polling
- native random bytes through `BCryptGenRandom`
- project-owned TLS 1.3 clients with a TLS 1.2 fallback, including application
  read/write and peer-certificate description
- project-owned parsing and application logic for the archive, compression,
  image, PGP, PDF, XML, TUI, SSH, Git, shell, build, service, and other source
  groups selected by the builder

Some POSIX-shaped metadata is intentionally synthetic. Identity lookup derives
the user name from `USERNAME` or `USER`, reports uid/gid 1000 and group
`Users`, and does not map Windows SIDs, tokens, or ACLs. File modes are reduced
to generic directory/file defaults. `chmod` and `chown` compatibility calls
currently validate path existence but do not alter Windows security metadata.

## CURRENT LIMITATIONS

Compilation coverage must not be confused with complete command behavior. The
default builder can produce the declared non-`ncc` tool set, including large
tools such as `sh`, `make`, `httpd`, `service`, `ssh`, `sshd`, `git`, `editor`,
and `mail`, but the following platform facilities remain absent, stubbed, or
deliberately conservative:

- directory enumeration is not implemented, so traversal-oriented commands
  cannot yet provide their normal Windows behavior
- process spawn, wait, timeout, process listing, syscall tracing, privilege
  changes, and signal delivery are not implemented; shell pipelines and tools
  that execute child processes are therefore incomplete
- native worker threads support task-pool workloads; mutex and semaphore
  compatibility code remains a spin-based fallback
- TCP listen/accept and the generic netcat adapter are not implemented, so
  `httpd` and `sshd` are build targets rather than complete native servers;
  service supervision is separately blocked by the missing process-spawn path
- direct DNS query, DHCP, ping, traceroute, socket-table, interface/address/
  route listing, and network mutation backends are not implemented
- symbolic links, node creation, mount/unmount, hostname changes, shutdown,
  and real ownership/mode changes are not implemented
- identity, group, session, process-usage, and load-average reporting is
  synthetic, empty, or limited compared with Windows account and system APIs
- symlink reading and open-file/process inspection are not implemented
- the Windows USB backend currently returns no devices and does not open or
  transfer to USB devices; `lsusb` builds against this bring-up stub
- TLS handshakes and certificate parsing work, but the peer chain and hostname
  are not validated against the Windows trust store; TLS-backed clients must
  not be treated as hardened HTTPS, IMAPS, or general secure-client
  implementations solely because a handshake succeeds
- the native command-line and path layer is narrow-character based and does not
  yet provide complete Windows Unicode or quoting semantics

The Windows-built `ncc` executable is an explicit bring-up target. It can use
the compiler's existing Linux and macOS output paths, but native Windows PE
emission from `ncc` is still future work.

## TECHNICAL DECISIONS

The current Windows strategy is intentional.

- keep shared tool and subsystem logic independent of Windows headers and route
  OS interaction through `src/shared/platform.h`
- keep handwritten Win32 declarations and import definitions small so the
  native no-CRT build does not require the Windows SDK
- use Clang/lld first to establish a reliable ARM64 and x86-64 production path,
  then grow the in-tree PE linker against validated COFF fixtures
- prefer normal loader-safe PE alignment and RX/RW separation over malformed or
  broadly RWX low-alignment images, even when a smaller file can be written
- merge read-only and import data into RX text for compact output, but never
  merge writable state into that section
- use section GC, safe ICF, LTO, collectible stack probes, and deterministic
  timestamps as normal size policy rather than per-tool special cases
- reserve the dedicated minimal startup for tools whose behavior is independent
  of arguments; normal tools retain real command-line handling
- isolate Bcrypt randomness from TLS transport so PGP and other crypto tools do
  not pull the complete TLS client into their image
- compile reusable objects once per flag profile and bound LTO link parallelism
  separately, because linking has a materially higher memory cost than ordinary
  object compilation
- let unsupported host-mutating or introspection paths fail or return empty
  results rather than emulate Linux behavior inaccurately
- keep MSYS2 hosted binaries clearly separate from native PE binaries; copying
  `msys-2.0.dll` beside a hosted tool is not a supported deployment model

## OPTIONS AND OVERRIDES

Useful native-builder options include:

```
.\tests\windows\build-windows-freestanding.ps1 -TargetTriple aarch64-w64-windows-gnu -Tools linker,true,false,echo -Jobs 8 -LinkJobs 2
```

- `-Compiler PATH` selects a specific `clang.exe`
- `-TargetTriple` selects the ARM64 Windows GNU ABI output; other targets are
  rejected while the packed linker remains ARM64-only
- `-BuildDir PATH` selects another output and cache tree
- `-PackedDir PATH` selects the packed output and cache tree
- `-Tools NAME[,NAME...]` builds only the selected tools; include `linker` so
  the packed phase can run. Add `ncc` for the compiler bring-up target excluded
  from the default set
- `-Jobs N` controls parallel compilation
- `-LinkJobs N` independently controls concurrent LTO links
- `-Clean` removes both selected output trees, including object and import caches
- `-VerboseCommands` prints full compiler and linker commands

PowerShell execution policy can be bypassed for one invocation without changing
the machine-wide policy:

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\windows\build-windows-freestanding.ps1
```

For the hosted POSIX comparison path, run `make host` inside MSYS2 and keep the
installed MSYS runtime directory on `PATH`. Those binaries use
`src/platform/posix/` and depend on `msys-2.0.dll`; they are not native no-CRT
PE outputs.

## TESTING

Run the native ARM64 PE linker regression with:

```
.\tests\windows\test-pe-arm64-linker.ps1
```

On native ARM64 Windows this verifies execution and layout for lld and the
in-tree linker, including the 1024-byte minimum image, live-import selection,
dead-section removal, weak and associative COMDAT handling, writable-data
separation, and retention or collection of stack probes. The script skips on
non-ARM64 hosts. It also verifies exact fallback plus real packed text and
initialized-data/BSS execution without a temporary file.

The main builder verifies that the complete selected source surface compiles
and links for ARM64 in both forms. Its cache summary makes incremental behavior
visible; an unchanged second invocation should report zero compiled objects and
zero linked tools. There is not yet a native Windows equivalent of the complete
Phase 1 or isolated-userland smoke suite, so broad command behavior still needs
targeted native tests as platform primitives are added.

## SEE ALSO

man, build, platform, runtime, compiler, linker, project-layout, macos, userland
