# MD5 Files Experiment

This directory contains a small, reproducible MD5 collision demonstration for
plain files. It is intentionally simple: two generated files differ in a visible
128-byte metadata block and then share the same suffix. Because the block pair is
a classic public MD5 collision, both complete files have the same MD5 digest.

This does not make MD5 useful or trustworthy. The point is the opposite: MD5 is
structurally broken, and equal MD5 hashes are not meaningful evidence that two
files are identical.

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

## ELF Scaffold

The generator also has an ELF-aware scaffold mode:

```sh
make -C experimental/md5files build/generate
experimental/md5files/build/generate --in1 path/to/tool-a --in2 path/to/tool-b --out-dir experimental/md5files/out
```

With that command this writes:

- `out/elf-md5file-a.bin`
- `out/elf-md5file-b.bin`

You can override those with `--out-dir`, `--out1`, and `--out2`.

This mode parses both inputs as little-endian ELF64 executables, checks that the
program header table is inside the file, checks that every `PT_LOAD` segment is
inside the original file, and appends a labeled
`newos-md5-collision-metadata` trailer after the original bytes. The collision
block is placed at the same 64-byte-aligned file offset in both outputs, and the
original file modes are preserved so executable inputs remain executable.

The scaffold computes the MD5 of both candidate outputs before returning. With
arbitrary existing ELF files it is expected to exit with status 2 and explain
that the candidate outputs do not collide. That is intentional: the fixed public
collision blocks only work for their controlled MD5 prefix state. This gives us
the binary-safe trailer layout and validation path without pretending to solve
the chosen-prefix collision problem yet.

## Shape

Each generated file is:

```text
128-byte public MD5 collision block || identical explanatory suffix
```

The collision block is treated as openly visible metadata. The suffix is common,
so the Merkle-Damgard suffix property preserves the collision.

## Next Step

The remaining hard part is a real chosen-prefix MD5 backend. The current ELF
mode has the loader-safe append point, equalized collision-block offset, and
honest verification behavior needed to plug such a backend in later.