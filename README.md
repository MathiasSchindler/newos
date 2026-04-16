# newos

newos is an experimental userland project for a Linux-ABI-compatible operating system.

In broad terms, this repository is a growing collection of command-line tools, shared runtime code, and platform backends that are designed to build on host systems such as macOS while also targeting a freestanding Linux environment.

The project has been written with the help of a finetuned version of GPT 5.4, with an emphasis on portability, small utilities, and clear separation between tool logic and platform-specific code.

## Scope

The repository currently focuses on:

- small Unix-style command-line programs
- shared support code for strings, I/O, archives, and hashing
- platform layers for hosted POSIX builds and freestanding Linux/AArch64 builds

## License

This project is released under CC-0.
