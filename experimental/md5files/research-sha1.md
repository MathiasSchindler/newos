# SHA-1 Collision Research Notes

This note documents the current state of the `experimental/md5files` SHA-1 work,
the interim results, and the path toward a full SHA-1 ELF collision experiment.
It is research and educational material only. None of the reduced-hash work below
is a full SHA-1 collision.

## Starting point

The experiment began with MD5 collision support and a Linux/x86-64 ELF wrapper
that can produce two working ELF files with the same MD5. The wrapper embeds both
input ELFs behind a controlled MD5 collision prefix, then uses a differing bit in
the collision payload to select which embedded ELF to execute.

SHA-1 support was first added for plain files using the public SHAttered
collision structure. The generated files:

- `out/sha1file-a.bin`
- `out/sha1file-b.bin`

have the same full SHA-1 digest and different bytes. They are plain collision
files, not executable ELF files.

## Why the public SHAttered blocks do not directly make ELFs

The public SHAttered files are an identical-prefix SHA-1 collision:

```text
SHA1(P || M1  || M2  || S)
=
SHA1(P || M1' || M2' || S)
```

The prefix `P` is a fixed PDF/JPEG prefix beginning with `%PDF-1.3`. The two
files then differ in two successive SHA-1 message blocks, 128 bytes total. The
first block pair creates a controlled internal SHA-1 state difference; the second
block pair cancels it so both computations return to the same chaining value.
After that, any identical suffix preserves the collision.

That exact collision is tied to the SHA-1 internal state after the PDF prefix. A
Linux ELF file must instead begin with the ELF magic bytes `0x7f 'E' 'L' 'F'`.
Therefore the public SHAttered blocks cannot simply be moved behind an ELF
header.

Sources:

- SHAttered project site: <https://shattered.io/>
- SHAttered paper: Marc Stevens, Pierre Karpman, Thomas Peyrin, "The first
  collision for full SHA-1", CRYPTO 2017, available from
  <https://shattered.io/static/shattered.pdf>

Relevant observations from the paper:

- The published result is an identical-prefix collision.
- The collision uses two near-collision block pairs after a carefully selected
  PDF prefix.
- The attack builds on SHA-1 differential cryptanalysis, disturbance vectors,
  nonlinear paths, message modification rules, and GPU-assisted search.
- The paper estimates the practical attack at roughly `2^63.1`
  SHA-1-equivalent work, not generic birthday search over full SHA-1.

## Backend contract for executable outputs

The generator now has a hash-neutral backend contract for ELF scaffold mode. The
generator provides exact prefix files, a common suffix file, and output paths for
collision payloads through environment variables:

- `NEWOS_HASH_NAME`
- `NEWOS_HASH_UPPERCASE_NAME`
- `NEWOS_HASH_PREFIX_A`
- `NEWOS_HASH_PREFIX_B`
- `NEWOS_HASH_BLOCK_A`
- `NEWOS_HASH_BLOCK_B`
- `NEWOS_HASH_SUFFIX`
- `NEWOS_HASH_COLLISION_OFFSET`
- `NEWOS_HASH_BLOCK_SIZE`
- `NEWOS_HASH_MAX_PAYLOAD_SIZE`

The backend writes two equal-sized payloads. The generator appends the common
suffix, computes the selected hash profile, and rejects the outputs if they do
not collide. For MD5, the old `NEWOS_MD5_*` names are still provided for
compatibility.

For full SHA-1 ELF output, this backend contract is the right integration point:
the missing part is a real SHA-1 collision generator for the exact ELF-suitable
prefixes.

## Reduced SHA-1 profiles

To prove the executable pipeline and measure resource scaling, the experiment now
includes reduced profiles:

- `sha1-24`
- `sha1-32`
- `sha1-40`
- `sha1-48`
- `sha1-56`
- `sha1-64`

These compare only the first selected number of SHA-1 bits. They are useful for
plumbing and scaling tests. They are not SHA-1 collisions.

The successful outputs preserve ELF behavior:

- `elf-sha1-XXfile-a.bin` preserves the `true` fixture behavior.
- `elf-sha1-XXfile-b.bin` preserves the `false` fixture behavior.

## CPU backend

`toy_sha1_backend.c` is a simple CPU birthday-search backend. It stores one side
of the search in a hash table and probes the other side.

Useful targets:

```sh
make -C experimental/md5files toy-sha1-elf-demo
make -C experimental/md5files toy-sha1-32-elf-demo
make -C experimental/md5files toy-sha1-40-elf-demo
make -C experimental/md5files toy-sha1-48-elf-demo
make -C experimental/md5files toy-sha1-scale
make -C experimental/md5files toy-sha1-large-scale
```

Measured on the current Linux machine:

| Profile | Search budget | Result | Wall time | User time | Max RSS |
| --- | ---: | --- | ---: | ---: | ---: |
| `sha1-24` | 65K x 16M | found | 0.08 s | 0.08 s | 5.3 MB |
| `sha1-32` | 1M x 16M | found | 1.35 s | 1.31 s | 66.7 MB |
| `sha1-40` | 4M x 16M | found | 5.62 s | 5.48 s | 263.2 MB |
| `sha1-48` | 33M x 33M | found | 56.64 s | 56.14 s | 1.57 GB |
| `sha1-56` | 33M x 33M | missed | 86.52 s | 85.98 s | 1.57 GB |
| `sha1-64` | 33M x 33M | missed | 86.81 s | 86.28 s | 1.57 GB |

The CPU backend is single-threaded. Its memory use was reduced by storing keys
and nonces in split arrays instead of a padded struct array.

## CUDA backend

`toy_sha1_cuda_backend.cu` is an optional CUDA backend confined to this
experimental directory. It requires `nvcc` and a CUDA-capable GPU. It is not part
of the freestanding/no-dependency project path.

Useful targets:

```sh
make -C experimental/md5files toy-sha1-48-cuda-demo
make -C experimental/md5files toy-sha1-56-cuda-attempt
make -C experimental/md5files toy-sha1-64-cuda-attempt
make -C experimental/md5files toy-sha1-64-cuda-batched
make -C experimental/md5files toy-sha1-cuda-large-scale
```

Measured on an NVIDIA GeForce RTX 4070 Ti SUPER with 16 GB:

| Profile | Search budget | Result | Wall time |
| --- | ---: | --- | ---: |
| `sha1-48` | 33M x 33M | found | 0.46 s |
| `sha1-56` | 33M x 33M | missed | 0.46 s |
| `sha1-64` | 33M x 33M | missed | 0.46 s |
| `sha1-56` | 268M x 268M | found | 2.18 s |
| `sha1-64` | 34 x (268M x 268M) | found | 64.53 s |

The successful larger `sha1-56` run used:

```sh
NEWOS_TOY_A_CANDIDATES=268435456 \
NEWOS_TOY_B_LIMIT=268435456 \
NEWOS_TOY_TABLE_CAPACITY=1073741824 \
make -C experimental/md5files toy-sha1-56-cuda-attempt
```

The successful `sha1-64` batched run used:

```sh
NEWOS_TOY_BATCHES=64 \
make -C experimental/md5files toy-sha1-64-cuda-batched
```

It found a match after 34 batches with matching 64-bit SHA-1 prefix:

```text
07ca1c566160afd5
```

The two generated files were valid ELF files preserving the expected true/false
fixture behavior.

## Probability model for the reduced birthday search

For an `n`-bit truncated match, one independent table/probe batch has approximate
success probability:

```text
p ~= 1 - exp(-(A * B) / 2^n)
```

where `A` is the number of stored A-side candidates and `B` is the number of
B-side candidates probed.

For the large CUDA batch size:

```text
A = 2^28
B = 2^28
```

For `sha1-64`, one batch has probability about `2^-8`, or roughly 0.39%. A
64-batch run has about 22% cumulative probability. In the observed run, a match
appeared after 34 batches, at about 12.4% cumulative probability. A 256-batch run
would cover the current 32-bit nonce spaces and has about 63% cumulative
probability.

## Where this does not lead

The reduced birthday-search path does not scale to full SHA-1. Full SHA-1 has a
160-bit digest, and a generic birthday collision needs about `2^80` work. A
single 16 GB GPU is valuable for reduced experiments but does not make generic
full SHA-1 collision search realistic.

Scaling the reduced profiles upward remains useful for measuring machinery, but
it is not a trajectory from 64 bits to full SHA-1. It is a different problem.

## What is needed for full SHA-1

To produce two working ELF files with the same full SHA-1 digest, the experiment
needs a real SHA-1 collision backend, not a larger generic search. There are two
plausible goals:

1. **Identical-prefix ELF wrapper collision**
   - Choose a common ELF wrapper prefix.
   - Run a SHAttered-style identical-prefix SHA-1 collision attack from the
     SHA-1 internal state after that exact ELF prefix.
   - Generate two near-collision block pairs.
   - Append identical executable suffix/wrapper data.

2. **Chosen-prefix ELF collision**
   - Allow two genuinely different prefixes.
   - Generate a SHA-1 chosen-prefix collision for those prefixes.
   - This is more general and harder than the original SHAttered identical-prefix
     demonstration.

For either path, the backend needs real SHA-1 cryptanalytic components:

- disturbance vector selection
- nonlinear differential paths
- attack conditions over SHA-1 steps
- message modification rules
- near-collision block search
- GPU kernels that accelerate that specific differential attack

The current generator and backend contract can validate and package results once
such a backend exists. The missing work is the cryptanalytic backend itself.

## Recommended next steps

1. Keep the reduced CUDA backend as a benchmark and plumbing tool.
2. Add better progress reporting and optional longer CUDA runs if more reduced
   measurements are useful.
3. Study existing public SHA-1 collision research code, especially Marc Stevens'
   HashClash/SHA-1 work and the SHAttered paper's references.
4. Decide whether the next full-SHA-1 attempt should target:
   - identical-prefix ELF wrapper collision first, or
   - chosen-prefix collision support.
5. If using existing research code, keep it confined to `experimental/md5files`
   or another experimental-only directory so the normal no-dependency project
   path remains unchanged.

## Current conclusion

The project now has:

- full SHA-1 plain-file collision generation using public SHAttered blocks
- MD5 working ELF collision wrappers
- SHA-1 ELF scaffold/backend integration
- reduced SHA-1 ELF collisions through 64 bits
- optional CUDA acceleration for reduced SHA-1 experiments

The next leap to full SHA-1 requires a cryptanalytic SHA-1 collision backend.
CUDA helps, but only when used to accelerate that attack, not as generic brute
force.
