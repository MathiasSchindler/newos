# MD5 Files Experiment

This directory contains a small, reproducible MD5 collision demonstration for
plain files. It is intentionally simple: two generated files differ in a visible
128-byte metadata block and then share the same suffix. Because the block pair is
a classic public MD5 collision, both complete files have the same MD5 digest.

This does not make MD5 useful or trustworthy. The point is the opposite: MD5 is
structurally broken, and equal MD5 hashes are not meaningful evidence that two
files are identical.

## Security Research and Safety Notice

This directory is security research and educational material. It demonstrates why
MD5 must not be used for identity, integrity, signing, allowlisting, cache trust,
or any other security decision. The generated files are intentionally labeled and
the source is kept in `experimental/` so that the security nature of the work is
visible.

Do not use this code to mislead users, bypass trust checks, distribute disguised
executables, or attack systems that still depend on MD5 or SHA-1. Harmful use is
not condoned. If a system still treats MD5 or SHA-1 equality as proof that two
files are the same or trustworthy, the appropriate response is to migrate that
system to collision-resistant hashing and authenticated signatures, not to exploit
the weakness.

This experiment is intended to support defensive review, compatibility testing,
and clear demonstrations that legacy hashes are broken. Questions or concerns
about misuse can be raised through the repository's normal issue/contact channel.

## Usage

From the repository root:

```sh
make -C experimental/md5files
```

This builds a tiny generator and writes:

- `out/md5file-a.bin`
- `out/md5file-b.bin`

The verifier checks two properties:

- both files have the same MD5 digest
- the files are byte-for-byte different

It prefers the project `build/md5sum` tool when present, then falls back to the
host `md5sum` or macOS `md5` command.

The generator is internally organized around a small collision-profile interface
so other legacy hash demonstrations can be added later without moving
experiment-specific code into the main project. The only active profile today is
MD5.

## ELF Wrapper

The generator also has an ELF-aware wrapper mode:

```sh
make -C experimental/md5files build/generate
experimental/md5files/build/generate --in1 path/to/tool-a --in2 path/to/tool-b --out-dir experimental/md5files/out
```

With that command this writes:

- `out/elf-md5file-a.bin`
- `out/elf-md5file-b.bin`

You can override those with `--out-dir`, `--out1`, and `--out2`.

Without `--backend`, this mode parses both inputs as little-endian ELF64
executables, then emits two Linux/x86-64 ELF wrapper executables. The wrappers
share the controlled MD5-collision prefix and append both original input binaries
as identical suffix data. At runtime, a bit that differs inside the collision
payload selects which embedded ELF is copied to a memfd and executed with the
original argument vector and environment.

Because both outputs have the same prefix collision state and the same suffix,
their MD5 hashes match even when the input ELF files have very different sizes.
The wrappers are intentionally Linux/x86-64 specific and rely on `/proc/self/exe`,
`memfd_create`, and `execveat`.

## Controlled ELF Demo

The experiment can produce a controlled Linux/x86-64 ELF pair with matching MD5
hashes without external collision-generation dependencies:

```sh
make -C experimental/md5files elf-demo
```

This writes `out/elf-true` and `out/elf-false`. They share an identical ELF
prefix, the generator emits the controlled colliding middle, and then appends
common code that exits differently based on a bit that differs in the generated
collision payload. On Linux/x86-64, `elf-true` exits with status 0 and
`elf-false` exits with status 1. On macOS the files can be generated and hashed,
but not executed directly.

The same binary also keeps a tiny compatibility mode for the old fast-collision
surface used during bring-up:

```sh
experimental/md5files/build/generate --controlled-fastcoll -q -p prefix.bin -o collision1.bin collision2.bin
```

That mode only accepts the controlled 128-byte ELF prefix. It is not a general
MD5 collision search engine.

The `examples/` fixture below is harder: it starts from preexisting binaries and
therefore still needs a chosen-prefix backend.

## Arbitrary ELF Fixture

The `examples/` directory contains tiny static ELF `true` and `false` binaries.
The harmless proof of concept is for those two binaries to remain behaviorally
different while having the same MD5 hash.

The wrapper can be run against them with:

```sh
make -C experimental/md5files scaffold-examples
```

This now produces `out/elf-md5file-a.bin` and `out/elf-md5file-b.bin` with the
same MD5 hash while preserving the selected embedded program behavior.

The generator also keeps a backend interface for a command that accepts the two
validated ELF prefixes and returns a pair of chosen-prefix collision payloads for
the exact MD5 state at the trailer position. Use that path when you specifically
want an appended trailer layout instead of the controlled wrapper layout.

## Backend Contract

The ELF scaffold can now call an external backend:

```sh
experimental/md5files/build/generate \
	--in1 experimental/md5files/examples/true \
	--in2 experimental/md5files/examples/false \
	--out-dir experimental/md5files/out \
	--backend 'path/to/backend-command'
```

The Makefile target forwards `BACKEND` in the same way:

```sh
make -C experimental/md5files scaffold-examples BACKEND='path/to/backend-command'
```

The generator starts the backend directly through the project platform process
API. It does not evaluate the string with a shell, so use a backend executable or
script path with simple whitespace-separated arguments rather than shell
redirection, variable assignment, or pipelines.

The backend command receives paths through environment variables:

- `NEWOS_MD5_PREFIX_A`: binary prefix for the first output
- `NEWOS_MD5_PREFIX_B`: binary prefix for the second output
- `NEWOS_MD5_BLOCK_A`: path where the backend must write the first binary payload
- `NEWOS_MD5_BLOCK_B`: path where the backend must write the second binary payload
- `NEWOS_MD5_COLLISION_OFFSET`: shared file offset for the collision payload
- `NEWOS_MD5_BLOCK_SIZE`: `128`, the built-in public collision block size
- `NEWOS_MD5_MAX_PAYLOAD_SIZE`: largest accepted backend payload size

The prefix files already include the original ELF bytes, the labeled metadata
prelude, and zero padding up to the shared 64-byte-aligned collision offset. The
backend should return two non-empty binary payloads of the same size for those
exact prefixes and exit with status 0. The generator then appends the common
suffix, writes the final ELF files, computes both MD5 digests, and still exits
with status 2 if the backend payloads do not actually collide.

## Shape

Each generated file is:

```text
128-byte public MD5 collision block || identical explanatory suffix
```

The collision block is treated as openly visible metadata. The suffix is common,
so the Merkle-Damgard suffix property preserves the collision.

For the controlled ELF demo, the shape is:

```text
ELF prefix || controlled collision payload || identical executable suffix
```

The executable suffix is selected from the known differing payload bits, but it
is identical in both outputs, so the MD5 collision is preserved.

## Optional Backend Path

The wrapper mode above handles arbitrary Linux/x86-64 ELF pairs by embedding both
inputs behind a controlled collision prefix. A separate chosen-prefix backend is
only needed if you specifically want the appended trailer layout described in the
backend contract, where each output starts with a different original ELF prefix.
Such a backend should be treated as security research material and kept behind the
same explicit safety framing as this experiment.
