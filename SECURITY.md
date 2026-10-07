# Security and private data

This fork's maintained focus is the Windows local assistant. No separate support
promise is made for old tags or inherited upstream platform configurations.

## Report a vulnerability

Use this repository's **Security > Report a vulnerability** if private reporting
is enabled. If it is unavailable, open a minimal issue asking for a private
contact route, without exploit details, credentials, or personal data. No public
security contact email is configured here. Fork-specific reports belong here.

If a credential has already been exposed, revoke or rotate it with its provider.
Removing a file does not remove earlier commits, forks, caches, or downloaded
artifacts. History cleanup requires a separate coordinated action.

## Local AI data boundary

The shipped assistant sends inference requests to loopback, and downloads
runtime/model assets from their configured public hosts. Keep its host on
`127.0.0.1`. A local model endpoint is not an authenticated network service.
R, packages, plugins, and user code have independent network behavior.

Prompts, reference excerpts, images, generated code, R history, and diagnostics
can contain personal data. Local files are not encrypted by this application.
Portable mode is not an anti-forensics feature. Review attachments and generated
code; untrusted reference text can influence model responses.

Never upload real user profiles, `.Renviron`, `.Rhistory`, `.RData`, model-server
logs, memory dumps, or clipboard contents in a public report. Use synthetic
fixtures and redact both text and visible screenshot content.

See the dated [repository privacy review](docs/PRIVACY-AUDIT.md) for the scan's
scope and limitations.
