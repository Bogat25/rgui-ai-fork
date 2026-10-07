# Contributing to RGui AI

This is an independently maintained hard fork focused on local AI for Windows
RGui. Long-term upstream synchronization is not planned. Report fork-specific
bugs and proposals in this repository's Issues tab rather than asking upstream
R maintainers to support this distribution.

## Report a problem

Include the fork tag/commit, Windows version, installation type, reproduction
steps, expected/actual behavior, and a small synthetic example. For AI issues,
include the model/runtime versions, relevant non-secret settings, and whether
text, file attachments, pasted screenshots, or plots fail. Remove personal
paths, usernames, private notes, data, and credentials from logs and screenshots.
See [SECURITY.md](SECURITY.md) for sensitive reports.

## Develop a change

Read the [architecture](docs/ARCHITECTURE.md) and [build guide](docs/BUILDING.md).
Keep inference local by default, downloads verifiable, the R UI responsive,
process ownership explicit, and code execution under the user's control.
Preserve UTF-8, picture-history bounds, cancellation, and upgrade data handling.
Add regressions for behavioral fixes and run the suites the change affects.
Record skipped or blocked checks honestly.

Retain original copyright and license notices. Do not commit model weights,
runtime downloads, personal prompts/notes, real chat logs, user profiles, or
credentials. Review both staged content and commit metadata before publishing.
Use clear conventional commit messages describing the behavior changed.

External contributors can use GitHub's normal fork and pull-request process.
Repository maintainers decide what to accept and publish; this documentation
does not authorize automated GitHub writes.
