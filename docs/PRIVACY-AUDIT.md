# Repository privacy review

Review date: **2026-10-07**. Findings are deliberately redacted; no email address,
credential value, token fragment, private directory, or user content is copied here.

## Scope

The initial tracked inventory contained 5,149 files. The review checked tracked
working-tree bytes, including binary bytes, staged documentation changes, Git
author/committer metadata, commit messages, and 5,178 unique blob objects reachable
from local refs before this documentation commit. Patterns covered common provider
tokens, private-key markers, credential URLs/assignments, JWT candidates,
maintainer-email matches, user-home paths, and the known private workspace path.
Selected UTF-16 representations were checked as well as ordinary byte strings.

## Result and changes

- No live credential or personal mailbox belonging to the local maintainer was
  identified in the scanned content or matched maintainer metadata.
- Email addresses in inherited copyright notices, manuals, contributor records,
  and examples are upstream material and were retained.
- Flagged home-directory strings are inherited documentation, source comments,
  or fixtures. A credential-shaped URL is a documented placeholder example,
  not an identified account credential.
- Added ignore rules for private work/reference folders, R user-state files,
  environment files, model partials, logs, credential containers, and dumps.
- Public guides now explain the actual local inference/download boundary and
  how to share synthetic, redacted reproductions.

Git authorship remains public, including public no-reply identities. Removing an
address from a README would not remove it from commit metadata. No public history
was rewritten, and no GitHub write was performed for this review.

## Limits and maintenance

This is a pattern-based review, not a guarantee that no private material exists.
Image pixels, compressed PDF contents, encrypted files, arbitrary custom-format
secrets, and unrelated personal identities cannot be fully assessed by these
patterns. No OCR/manual inspection of all inherited images was performed.

The review covers locally reachable refs, not unreachable objects, remote-only
branches, cached pages, forks, Actions logs/artifacts, or every published binary.
Ignored local files are outside the public tracked-tree inventory. Review screenshots
and distribution artifacts separately before publishing.

Before publication, inspect staged files and author settings privately. Use the
hosting provider's no-reply email if desired, keep real notes/profiles outside
the checkout, and follow [SECURITY.md](../SECURITY.md) if an actual secret is found.
