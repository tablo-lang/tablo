# Security Policy

## Reporting a Vulnerability

If you discover a security issue in TabloLang, please report it privately before public disclosure.

Include:
- Affected version/commit
- Reproduction steps or proof of concept
- Impact assessment
- Suggested mitigation (if available)

If private contact channels are not yet configured for this repository, open a minimal GitHub issue marked as security-related without publishing exploit details, and request a private follow-up channel.

## Scope

Security reports are especially relevant for:
- VM/runtime memory safety and sandbox escapes
- File/process/network API boundary issues
- Cryptography/encoding misuse in stdlib modules
- SQLite/process/HTTP handling vulnerabilities

## Notes on Cryptography Modules

Some cryptographic helpers in stdlib are intended for educational/prototyping workflows unless otherwise specified.
Use audited, production-grade designs and padding/key-management policies before relying on them in high-risk deployments.
