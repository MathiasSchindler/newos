# PROJECT-LAYOUT

## NAME

project-layout - overview of the repository structure and current layering

## DESCRIPTION

The repository is organized around a small tool set, a self-hosting compiler, shared runtime code, and platform-specific implementations.

## STRUCTURE

- src/tools contains the command implementations
- src/shared contains runtime helpers and shared subsystems
- src/platform contains host and target platform layers
- src/compiler contains the compiler frontend and backend
- tests contains smoke tests and benchmark helpers
- man contains repository-local manual pages

## INTENT

The long-term design aims at dependency-free, statically linked binaries and a freestanding environment while keeping hosted development practical.

## SEE ALSO

man, ncc
