# Security Policy

## Project Status and Scope

This repository is a research and educational operating-system/userland project.
It includes freestanding tools, low-level runtime code, compiler and linker code,
networking code, parsers, cryptographic primitives, TLS-related code, and
security-relevant experiments.

The vast majority of the code in this repository has been produced with help from
large language models, including GPT-5.4, a fine-tuned version of GPT-5.4, and
GPT-5.5. There is no explicit or implicit statement that the code is valid,
functional, robust, secure, production-ready, or suitable for any particular
purpose.

At this point, this software should not be used in or anywhere near production.
That warning applies to the project as a whole and especially to security-sensitive
areas such as cryptography, TLS, parsers, archive handling, networking, process
execution, executable loading, and compiler/linker behavior.

## Cryptography and Security Components

This project implements a number of security components and cryptographic
primitives. That is a clear violation of the standard "don't roll your own crypto"
best practice. These implementations exist for research, education, portability
experiments, and freestanding-system exploration, not as production security
components.

Do not rely on this repository for confidentiality, integrity, authentication,
authorization, signing, transport security, secure parsing, sandboxing, malware
detection, or any other security boundary.

MD5 and SHA-1 are considered broken for collision resistance. Any system that uses
MD5 or SHA-1 for identity, integrity, signing, allowlisting, cache trust, or other
security decisions should be migrated to collision-resistant hashes and
authenticated signatures.

## Security Research and Dual-Use Content

Some code in this repository demonstrates broken legacy mechanisms, unusual
executable formats, malformed inputs, collision behavior, or other
security-relevant behavior. This content is included for defensive research,
education, compatibility testing, and to improve the safety and robustness of the
project.

Do not use this repository to attack systems, bypass trust controls, distribute
disguised executables, mislead users, or exploit systems that still depend on
broken or obsolete mechanisms. Harmful use is not condoned.

## Heuristic Scanner Warnings

This project intentionally produces statically linked, dependency-free binaries
that do not rely on a standard C library. Some outputs also use compact or unusual
ELF, Mach-O, or PE layouts. As a result, generated binaries may differ from
standard toolchain outputs in ways that trigger heuristic security scanners.

Binaries packed with `expack` can have higher entropy and may use Linux syscalls
such as `memfd_create` and `execveat`; both characteristics are commonly watched
by malware heuristics. Windows PE files produced by this project have also raised
scanner warnings at a higher rate in past testing. These false positives are
expected and should be evaluated in that context, but a false-positive expectation
is not a guarantee that any particular binary is safe.

## Reporting Vulnerabilities or Abuse Concerns

Security reports and abuse-risk concerns are welcome via mathiasschindler@github.com

Useful reports include:

- memory-safety bugs in tools or shared runtime code
- parser bugs that can be triggered by untrusted input
- unsafe behavior in networking, archive, crypto, TLS, image, XML, compiler, or
  linker code
- build or release issues that could misrepresent generated artifacts
- misuse concerns related to security research or dual-use experiments

## Disclosure Expectations

Because this repository is a research project and is not production software, the
maintainer does not require embargoed or confidential handling of vulnerability
reports. Reporters are encouraged to notify the maintainer so issues can be
understood, documented, fixed, or mitigated where appropriate, but they are not
asked to restrict discussion or disclosure of their findings.

Please do not use vulnerability information to attack systems, mislead users, or
cause harm to third parties.

## Response Expectations

This is an independent research project, so response times may vary. The
maintainer will make a best-effort attempt to review actionable reports, reproduce
issues where possible, and publish fixes or mitigations when appropriate.