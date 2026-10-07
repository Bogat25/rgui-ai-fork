# RGui AI documentation

This repository is an independent hard fork focused on local AI in RGui. Routine
upstream synchronization is not planned. Windows x64 is the assistant target;
the presence of other R platform sources does not imply assistant support there.

## For users

- [Assistant guide](USER-GUIDE.md): install, chat, attach pictures, add notes,
  change settings, and diagnose failures.
- [Portable setup and detailed configuration](../src/gnuwin32/fixed/ai/SETUP.md).
- [Original R manuals](../doc/manual) and [original README](../README).
- [Security and private data](../SECURITY.md).

## For developers and release maintainers

- [Build, tests, and releases](BUILDING.md).
- [Architecture and source map](ARCHITECTURE.md).
- [Validation evidence and limitations](VALIDATION.md).
- [Repository privacy audit](PRIVACY-AUDIT.md).
- [Contribution guide](../CONTRIBUTING.md).

Examples use `C:\rgui-build` as a disposable build directory. It is an example,
not a required drive or a reference to a maintainer's computer.
