# Legacy Hash Collision Files Experiment

This directory contains small, reproducible collision demonstrations for legacy
hashes. It is intentionally simple: two generated plain files differ in a visible
128-byte collision block and then share the same suffix. The default profile uses
a classic public MD5 collision; an optional SHA-1 profile uses the public
SHAttered collision structure.

This does not make MD5 or SHA-1 useful or trustworthy. The point is the opposite:
both are structurally broken, and equal legacy hashes are not meaningful evidence
that two files are identical.

For the current SHA-1 research state, interim measurements, and full-SHA-1
requirements, see [`research-sha1.md`](research-sha1.md).

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

This builds a tiny generator and writes the default MD5 pair:

- `out/md5file-a.bin`
- `out/md5file-b.bin`

The verifier checks two properties:

- both files have the same MD5 digest
- the files are byte-for-byte different

It prefers the project `build/md5sum` tool when present, then falls back to the
host `md5sum` or macOS `md5` command.

To generate and verify the SHA-1 plain-file pair:

```sh
make -C experimental/md5files verify-sha1
```

This writes:

- `out/sha1file-a.bin`
- `out/sha1file-b.bin`

The generator also accepts the profile explicitly:

```sh
experimental/md5files/build/generate --hash md5 experimental/md5files/out
experimental/md5files/build/generate --hash sha1 experimental/md5files/out
```

The generator is internally organized around a small collision-profile interface
so legacy hash demonstrations can share the hosted experiment scaffolding without
moving experiment-specific code into the main project.

There are also deliberately reduced `sha1-24`, `sha1-32`, `sha1-40`,
`sha1-48`, `sha1-56`, and `sha1-64` research profiles for exercising the ELF
chosen-prefix pipeline. They compare only the selected prefix bits of SHA-1 and
are not SHA-1 collisions.

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

The backend-less ELF wrapper mode is currently MD5-only. Without `--backend`,
this mode parses both inputs as little-endian ELF64 executables, then emits two
Linux/x86-64 ELF wrapper executables. The wrappers share the controlled
MD5-collision prefix and append both original input binaries as identical suffix
data. At runtime, a bit that differs inside the collision payload selects which
embedded ELF is copied to a memfd and executed with the original argument vector
and environment.

Because both outputs have the same prefix collision state and the same suffix,
their MD5 hashes match even when the input ELF files have very different sizes.
The wrappers are intentionally Linux/x86-64 specific and rely on `/proc/self/exe`,
`memfd_create`, and `execveat`.

The public SHAttered SHA-1 blocks do not provide the same built-in wrapper path:
their collision depends on the fixed `%PDF...` prefix, while a Linux ELF binary
must start with the ELF magic bytes. Producing two working ELF binaries with the
same SHA-1 therefore needs a SHA-1 chosen-prefix backend for the exact ELF
prefixes.

As stepping stones, the toy reduced profiles can prove the ELF pipeline with
truncated SHA-1 collisions:

```sh
make -C experimental/md5files toy-sha1-elf-demo
make -C experimental/md5files toy-sha1-32-elf-demo
make -C experimental/md5files toy-sha1-40-elf-demo
make -C experimental/md5files toy-sha1-scale
make -C experimental/md5files toy-sha1-large-scale
make -C experimental/md5files toy-sha1-cuda-large-scale
```

The success targets write files such as `out/elf-sha1-48file-*.bin`. The first
output preserves the `true` fixture behavior and the second preserves the `false`
fixture behavior, while the generator verifies that the requested SHA-1 prefix
bits match after appending the backend payload and common suffix. This is useful
for plumbing and scaling experiments only; it does not demonstrate a full SHA-1
collision.

On the current Linux test machine, the simple CPU backend measured:

| Profile | Search budget | Result | Wall time | User time | Max RSS |
| --- | ---: | --- | ---: | ---: | ---: |
| `sha1-24` | 65K x 16M | found | 0.08 s | 0.08 s | 5.3 MB |
| `sha1-32` | 1M x 16M | found | 1.35 s | 1.31 s | 66.7 MB |
| `sha1-40` | 4M x 16M | found | 5.62 s | 5.48 s | 263.2 MB |
| `sha1-48` | 33M x 33M | found | 56.64 s | 56.14 s | 1.57 GB |
| `sha1-56` | 33M x 33M | missed | 86.52 s | 85.98 s | 1.57 GB |
| `sha1-64` | 33M x 33M | missed | 86.81 s | 86.28 s | 1.57 GB |

This backend is a birthday-search plumbing test, not a SHA-1 cryptanalytic
attack. It is single-threaded. The rapid memory growth is expected because it
stores one side of the search in a hash table; the current implementation uses
split key/nonce arrays to avoid padding overhead.

An optional CUDA backend is available when `nvcc` and a CUDA-capable GPU are
present. It is still the same reduced birthday-search plumbing test, but it
builds and probes the table on the GPU:

```sh
make -C experimental/md5files toy-sha1-48-cuda-demo
make -C experimental/md5files toy-sha1-56-cuda-attempt
make -C experimental/md5files toy-sha1-64-cuda-attempt
make -C experimental/md5files toy-sha1-64-cuda-batched
make -C experimental/md5files toy-sha1-cuda-large-scale
```

On an NVIDIA GeForce RTX 4070 Ti SUPER with 16 GB, the CUDA backend measured:

| Profile | Search budget | Result | Wall time |
| --- | ---: | --- | ---: |
| `sha1-48` | 33M x 33M | found | 0.46 s |
| `sha1-56` | 33M x 33M | missed | 0.46 s |
| `sha1-64` | 33M x 33M | missed | 0.46 s |
| `sha1-56` | 268M x 268M | found | 2.18 s |
| `sha1-64` | 34 x (268M x 268M) | found | 64.53 s |

The larger `sha1-56` run used:

```sh
NEWOS_TOY_A_CANDIDATES=268435456 \
NEWOS_TOY_B_LIMIT=268435456 \
NEWOS_TOY_TABLE_CAPACITY=1073741824 \
make -C experimental/md5files toy-sha1-56-cuda-attempt
```

The successful `sha1-64` batched run used the same 268M x 268M per-batch table
size and allowed up to 64 batches:

```sh
NEWOS_TOY_BATCHES=64 \
make -C experimental/md5files toy-sha1-64-cuda-batched
```

It found a match at cumulative probability about 12.4%. A full 256-batch run
would cover the same 2^32-by-2^32 nonce grid used by the payload format and would
have about 63% success probability, but this remains a truncated-hash experiment,
not a full SHA-1 collision.

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
MD5 collision search engine and does not currently support SHA-1.

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
the exact selected-hash state at the trailer position. For SHA-1, this backend
path is required for ELF output. For MD5, use that path when you specifically
want an appended trailer layout instead of the controlled wrapper layout.

## Backend Contract

The ELF scaffold can now call an external backend:

```sh
experimental/md5files/build/generate \
	--hash md5 \
	--in1 experimental/md5files/examples/true \
	--in2 experimental/md5files/examples/false \
	--out-dir experimental/md5files/out \
	--backend 'path/to/backend-command'
```

The Makefile target forwards `BACKEND` in the same way:

```sh
make -C experimental/md5files scaffold-examples BACKEND='path/to/backend-command'
```

For a SHA-1 backend, select the profile explicitly:

```sh
experimental/md5files/build/generate \
	--hash sha1 \
	--in1 experimental/md5files/examples/true \
	--in2 experimental/md5files/examples/false \
	--out-dir experimental/md5files/out \
	--backend 'path/to/sha1-chosen-prefix-backend'
```

For the reduced toy backend used by `toy-sha1-elf-demo`, use `--hash sha1-24`,
`--hash sha1-32`, `--hash sha1-40`, `--hash sha1-48`, `--hash sha1-56`, or
`--hash sha1-64`. That backend performs a birthday search over two payload
domains. `NEWOS_TOY_A_CANDIDATES`, `NEWOS_TOY_B_LIMIT`, and
`NEWOS_TOY_TABLE_CAPACITY` can override its default bounded search sizes.

The generator starts the backend directly through the project platform process
API. It does not evaluate the string with a shell, so use a backend executable or
script path with simple whitespace-separated arguments rather than shell
redirection, variable assignment, or pipelines.

The backend command receives paths through hash-neutral environment variables:

- `NEWOS_HASH_NAME`: selected profile name, for example `md5` or `sha1`
- `NEWOS_HASH_UPPERCASE_NAME`: display name, for example `MD5` or `SHA-1`
- `NEWOS_HASH_PREFIX_A`: binary prefix for the first output
- `NEWOS_HASH_PREFIX_B`: binary prefix for the second output
- `NEWOS_HASH_BLOCK_A`: path where the backend must write the first binary payload
- `NEWOS_HASH_BLOCK_B`: path where the backend must write the second binary payload
- `NEWOS_HASH_SUFFIX`: common suffix that the generator appends after the payload
- `NEWOS_HASH_COLLISION_OFFSET`: shared file offset for the collision payload
- `NEWOS_HASH_BLOCK_SIZE`: built-in public collision block size for the profile
- `NEWOS_HASH_MAX_PAYLOAD_SIZE`: largest accepted backend payload size

For compatibility, MD5 backends also receive the older MD5-specific names:

- `NEWOS_MD5_PREFIX_A`: binary prefix for the first output
- `NEWOS_MD5_PREFIX_B`: binary prefix for the second output
- `NEWOS_MD5_BLOCK_A`: path where the backend must write the first binary payload
- `NEWOS_MD5_BLOCK_B`: path where the backend must write the second binary payload
- `NEWOS_MD5_COLLISION_OFFSET`: shared file offset for the collision payload
- `NEWOS_MD5_BLOCK_SIZE`: `128`, the built-in public collision block size
- `NEWOS_MD5_MAX_PAYLOAD_SIZE`: largest accepted backend payload size

The prefix files already include the original ELF bytes, the labeled metadata
prelude, and zero padding up to the selected hash's block-aligned collision
offset. The backend should return two non-empty binary payloads of the same size
for those exact prefixes and exit with status 0. The generator then appends the
common suffix, writes the final ELF files, computes both selected-hash digests,
and still exits with status 2 if the backend payloads do not actually collide.

## Shape

Each generated file is:

```text
[optional profile prefix] || 128-byte public collision block || identical explanatory suffix
```

The MD5 plain-file profile has no prefix. The SHA-1 plain-file profile uses the
common SHAttered prefix before the differing 128-byte block. The collision block
is treated as openly visible metadata. The suffix is common, so the
Merkle-Damgard suffix property preserves the collision.

For the controlled ELF demo, the shape is:

```text
ELF prefix || controlled collision payload || identical executable suffix
```

The executable suffix is selected from the known differing payload bits, but it
is identical in both outputs, so the MD5 collision is preserved.

## Optional Backend Path

The MD5 wrapper mode above handles arbitrary Linux/x86-64 ELF pairs by embedding
both inputs behind a controlled collision prefix. A separate chosen-prefix
backend is only needed for MD5 if you specifically want the appended trailer
layout described in the backend contract, where each output starts with a
different original ELF prefix. SHA-1 ELF output always needs such a backend with
the current built-in collision material. Such a backend should be treated as
security research material and kept behind the same explicit safety framing as
this experiment.
