# MAIL

## NAME

mail - interactive text-based IMAP and SMTP mail client

## SYNOPSIS

```
mail [-v] [--list|--fetch|--check-smtp] [--folder FOLDER] [--ask-password] [CONFIG]
```

## DESCRIPTION

`mail` opens a full-screen terminal mail client. It follows the shared interactive-tool architecture from `user-interface`: the tool owns mailbox state, focus, panes, and command handling while `tui.c` owns raw terminal setup, key decoding, cursor movement, and styles.

The current screen is split into mailbox-oriented panes. Wide terminals show a folder pane, a message list, and a message preview. Narrow terminals keep the message list and preview visible and omit the folder pane. The layout is deliberately simple so it can evolve toward IMAP folders, mailbox summaries, and message bodies without turning `tui.c` into a mail-specific widget layer.

`mail` is designed around secure IMAP and SMTP submission. It refuses plaintext authentication and sending. Hosted builds use the native TLS backend, with TLS 1.3 first and TLS 1.2 fallback for servers such as SMTP submission endpoints that have not enabled TLS 1.3. The native backend validates TLS CertificateVerify, RSA/SHA-256 or RSA/SHA-384 certificate signatures, DNS subject alternative names, certificate validity times, and a PEM trust-anchor bundle. Freestanding Linux builds use the same native TLS path when the needed network and trust-store files are available.

Non-interactive command modes are available for testing and scripting. `--list` opens a verified TLS IMAP session, authenticates with `LOGIN` or `AUTHENTICATE PLAIN`, and lists available folders. `--fetch` opens the same secure session, selects the configured folder, and prints summaries for the newest messages. `--check-smtp` opens the configured SMTP TLS endpoint and verifies the transport without requiring a password.

Interactive mode can refresh the mailbox, browse folders and message previews, and compose a simple text/plain message. Compose prompts for To and Subject, opens a small body editor, and sends with SMTP `AUTH PLAIN`, `MAIL FROM`, `RCPT TO`, and `DATA` over verified TLS. Non-interactive mode does not yet have an equivalent send command.

Verbose mode prints diagnostics about configuration, selected endpoints, password source, and the TLS transport phases. It is intended for debugging IMAP, SMTP, and TLS negotiation without printing password contents.

## CONFIGURATION

When `CONFIG` is provided, it is parsed as a simple `key=value` file. Blank lines and lines beginning with `#` are ignored.

Unsupported keys are ignored with a warning that includes the line number. This keeps older or experimental config files usable while still making spelling mistakes such as `smtp.post` visible.

Supported keys are:

- `imap.host` IMAP server name
- `imap.port` IMAP TLS port, usually `993`
- `smtp.host` SMTP submission server name
- `smtp.port` SMTP TLS port, usually `465`
- `user` login user name
- `from` sender address for composed mail
- `password` login password for local testing; omit it and use `--ask-password` when possible
- `folder` initial IMAP folder, defaulting to `INBOX`
- `tls` set to `required`, `yes`, or `true` to require TLS

Example:

```
imap.host=imap.example.com
imap.port=993
smtp.host=smtp.example.com
smtp.port=465
user=person@example.com
from=person@example.com
password=temporary-test-secret
folder=INBOX
tls=required
```

Do not commit real credentials. A local `mail.config` file is ignored by the repository.

## CURRENT CAPABILITIES

- opens an interactive terminal session using the shared TUI layer
- renders a folder/message/body pane layout for mailbox-style navigation
- loads account settings from a small key/value config file
- accepts a password from config or from a hidden prompt with `--ask-password`
- provides `--list`, `--fetch`, and `--check-smtp` non-interactive command modes over the platform TLS boundary
- loads the newest messages into the interactive message list when refreshing the inbox
- loads available folder names into the interactive folder pane when refreshing the inbox
- shows From, To, Cc, Date, Subject, and an initial body text slice in the message pane
- extracts a readable body preview from simple MIME messages, preferring text/plain and falling back to text/html converted to plain text
- decodes RFC 2047 Q-encoded UTF-8 header words, quoted-printable body text, base64 body text, and a small set of HTML entities
- composes and sends simple text/plain mail from interactive mode over SMTP TLS
- authenticates to SMTP with `AUTH PLAIN` and submits a single recipient with `MAIL FROM`, `RCPT TO`, and `DATA`
- starts the interactive mailbox view empty instead of showing placeholder messages
- provides `-v` and `--verbose` diagnostics for connection setup, TLS verification, IMAP/SMTP command phases, and sanitized authentication failure responses
- keeps IMAP refresh and SMTP send paths behind the shared TLS transport boundary
- refuses plaintext IMAP and SMTP operations

## CONTROLS

- `Enter` refresh the inbox when no messages are loaded, or move between the message list and preview
- `r` refresh the inbox through the secure IMAP path
- `c` compose and send a simple text/plain message over SMTP TLS
- compose mode prompts for To and Subject, accepts body text with `Enter` for new lines, sends with `Ctrl-D`, and cancels with `Esc`
- `Tab` cycle pane focus
- `Up`, `Down` move in the message list
- `PageUp`, `PageDown` move by several messages
- `q`, `Ctrl-Q` quit

## OPTIONS

- `--list` list available folders over verified TLS
- `--fetch` fetch the newest message headers and initial text previews over verified TLS
- `--check-smtp` verify the configured SMTP TLS endpoint without logging in or sending mail
- `-v`, `--verbose` print diagnostic information about config, endpoints, password source, and TLS transport phases
- `--folder FOLDER` override the configured IMAP folder
- `--ask-password` read the password from the terminal without echoing it
- `CONFIG` read account settings from the named config file

## LIMITATIONS

- hosted builds default to the native TLS 1.3 backend with TLS 1.2 fallback and RSA certificate validation against `SSL_CERT_FILE`, `/etc/ssl/cert.pem`, Homebrew OpenSSL's certificate bundle, or `/etc/ssl/certs/ca-certificates.crt`
- native TLS certificate validation currently supports RSA server certificates and RSA-signed chains with SHA-256 or SHA-384; ECDSA certificates, OCSP, CRL checks, AIA fetching, and name constraints are not implemented yet
- hosted POSIX builds always use the in-tree native TLS backend
- non-interactive mode can list folders, fetch message summaries, and check SMTP TLS, but cannot send an email yet; sending currently requires interactive compose mode
- SMTP sending supports one plain text message to one recipient; Cc, Bcc, Reply-To, attachments, rich MIME composition, saved drafts, and queued sending are not implemented yet
- password handling exists only as config storage or a session prompt; token and keychain support are not implemented yet
- IMAP parsing is intentionally minimal; body display uses the first fetched text slice with lightweight MIME text extraction, while full MIME trees, attachments, charsets beyond the current UTF-8-oriented handling, and full mailbox synchronization are not implemented yet
- compose editing is intentionally basic: it is an inline text/plain editor with no draft recovery, cursor navigation inside prior lines, spell checking, or attachment picker
- the interactive folder pane is populated from the server during refresh, but folder selection and switching are not implemented yet

## SEE ALSO

editor, user-interface, platform, runtime
