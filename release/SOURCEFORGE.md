# SourceForge offline releases

This guide applies to all three forks. Each repository contains the same
`release/` helpers and `.github/actions/sourceforge` action, with an application-
specific Windows build workflow. No SourceForge account or project has been
created by these files.

## Copy-and-paste project details

Create these three projects at <https://sourceforge.net/create/>. The suggested
URL names below have not been reserved or checked for availability. If one is
taken, append `-b25` and use that actual name in `SOURCEFORGE_PROJECT`.
SourceForge documents URL names as 3-30 letters, numbers or dashes; changing one
later requires support, so choose it before configuring the release environment.

| Field | RGui | RStudio | GUSEK |
| --- | --- | --- | --- |
| Project name | RGui AI | RStudio (Local Assistant Fork) | GUSEK AI |
| URL name | `rgui-local-ai` | `rstudio-local-assistant` | `gusek-local-ai` |
| Homepage / source code | <https://github.com/Bogat25/RGui-fork> | <https://github.com/Bogat25/rstudio> | <https://github.com/Bogat25/Gusek> |
| Support URL | <https://github.com/Bogat25/RGui-fork/issues> | <https://github.com/Bogat25/rstudio/issues> | <https://github.com/Bogat25/Gusek/issues> |
| Primary application license | GNU GPL v2 or later | GNU AGPL v3 | GNU GPL v3 or later |
| Suggested tags | R, statistics, local AI, offline | R, IDE, statistics, local AI, offline | MathProg, GLPK, optimization, local AI, offline |

The license entries describe the applications. Bundled R, model files,
llama.cpp and other components keep their own licenses and notices. The
SourceForge display name for the RStudio fork does not rename the installed
application, which remains **RStudio**.

Use the following text for the short summary and full description fields.

### RGui AI

Short summary:

```text
R for Windows with an offline assistant for code and statistics.
```

Full description:

```text
RGui AI is an independent fork of R for Windows with a local AI assistant for statistics, R code, plots, pictures, and course notes. The complete offline installer includes R, the llama.cpp CPU runtime, Qwen3.5-4B, and its picture reader. The local assistant runs without a cloud AI account or GPU. Users review and run suggested code themselves. This is not an official R Project distribution. Source code and issue tracking are maintained on GitHub.
```

### RStudio (Local Assistant Fork)

Short summary:

```text
RStudio Desktop fork with an offline assistant for R and statistics.
```

Full description:

```text
This independent RStudio Desktop fork adds a local AI assistant for R code, statistics, plots, pictures, and reference notes. The complete offline installer includes the IDE, R, the llama.cpp CPU runtime, Qwen3.5-4B, and its picture reader. The local assistant runs without a cloud AI account or GPU. Users review and run suggested code themselves. This is not an official Posit product or distribution. Source code and issue tracking are maintained on GitHub.
```

### GUSEK AI

Short summary:

```text
MathProg and GLPK editor with an offline optimization assistant.
```

Full description:

```text
GUSEK AI is an independent Windows MathProg and GLPK editor fork with a local AI assistant for optimization models, solver output, pictures, and reference notes. The complete offline installer includes the editor, GLPK solver, llama.cpp CPU runtime, Qwen3.5-4B, and its picture reader. The local assistant runs without a cloud AI account or GPU. Users review and run generated models themselves. This is not an official GUSEK, GLPK, SciTE, or Scintilla release. Source code and issue tracking are maintained on GitHub.
```

For all three projects, enable **Files / Downloads**, select **Windows** as the
supported operating system and **English** as the interface language. Suggested
categories are Science/Engineering and Mathematics, with Software Development
for RStudio if offered. **Beta** is a suitable initial development status for
these unsigned fork releases. Use GitHub for source and issues; a second Git
repository, tracker or wiki on SourceForge is optional.

You can use the same SourceForge publishing account and dedicated CI key for all
three projects. Your SourceForge login need not match your GitHub username.
The first project may require phone verification; complete that in the browser.
No OAuth application or MCP token is needed for this manual setup.

## One-time SourceForge setup

1. Create and verify a [SourceForge account](https://sourceforge.net/user/registration).
2. [Create one project](https://sourceforge.net/create/) for each independent
   fork. Enable the Files tool and use a description that identifies the project
   as an independent fork. Use the copy-and-paste project details above;
   availability of the proposed URL names is not assumed.
   Enter the corresponding GitHub repository as the source-code/homepage link.
   Code development can remain on GitHub.
3. The publishing account must be a project administrator or have permission to
   manage release files in all three projects. A dedicated publishing account
   may be added as a project member with the required release permissions.
4. Generate a dedicated, unattended **ED25519 SSH key** for GitHub Actions.
   Use a new key without a passphrase, stored outside the repositories. Do not
   reuse personal login keys or Codex/Claude/Gemini credentials. For example,
   run `ssh-keygen -t ed25519 -C sourceforge-releases` and choose a new filename
   when prompted. This command prompts for a passphrase; leave it empty for
   this dedicated CI key.

   To choose the filename explicitly in PowerShell:

   ```powershell
   New-Item -ItemType Directory -Force "$env:USERPROFILE\.ssh" | Out-Null
   ssh-keygen -t ed25519 -C "sourceforge-releases" -f "$env:USERPROFILE\.ssh\sourceforge-releases"
   ```

   Choose a new filename if that key already exists. The `.pub` file is the
   public key; the matching file without `.pub` is the private key.
5. In [SourceForge account services](https://sourceforge.net/auth/shell_services),
   add **only the public `.pub` key** to SSH keys. SourceForge accepts public
   keys; the private key belongs in the GitHub environment secret below.
6. In account settings/services, generate a **Releases API Key** for the same
   publishing account. The publisher uses it to select the default Windows
   download after verifying the uploaded installer.

Do not create release folders or upload installers manually. The publisher
creates and manages its own `windows/stable` and `windows/prerelease` folders.
SSH verifies the FRS server against SourceForge's documented ED25519 fingerprint;
no manually maintained known-hosts variable is required.

## One-time GitHub setup in each repository

Under **Settings > Environments**, create an environment named **`sourceforge`**.
Leave required reviewers disabled if releases should publish without manual
approval. Restrict the environment to your release tags if desired.

Add these **environment variables**:

| Name | Value |
| --- | --- |
| `SOURCEFORGE_PROJECT` | This application's actual SourceForge project slug, not its display name or URL |
| `SOURCEFORGE_USERNAME` | SourceForge login username of the publishing account |

With the proposed names, `SOURCEFORGE_PROJECT` is `rgui-local-ai` in
`Bogat25/RGui-fork`, `rstudio-local-assistant` in `Bogat25/rstudio`, and
`gusek-local-ai` in `Bogat25/Gusek`. Substitute any actual names that differ.
`SOURCEFORGE_USERNAME` is the same publishing login in all three environments.

Add these **environment secrets**:

| Name | Value |
| --- | --- |
| `SOURCEFORGE_SSH_KEY` | The complete private OpenSSH key corresponding to the public key registered on SourceForge |
| `SOURCEFORGE_API_KEY` | The publishing account's Releases API Key |

Use GitHub's secret editor or a secure file-based secret upload. Do not paste
credentials into chat, repository files, workflow YAML, or terminal output.
The same dedicated publishing account can serve the three projects; each
repository must use its own correct `SOURCEFORGE_PROJECT` value.

The release job refers to environment secrets, and build jobs have read-only
repository permissions. The release credentials are supplied only to the Linux
publisher job after the Windows build and checks succeed.

## Release behavior

The owner pushes the local commits and then a new unique version tag, such as
`v0.2.0`. This starts the existing release workflow. Creating a local tag alone
does not start a hosted workflow. RStudio also accepts four numeric components,
such as `v0.2.0.1`; use the version format supported by that fork.

The workflow:

1. Fetches the pinned CPU runtime and both pinned models.
2. Builds the application and its **`-offline-setup.exe`**, including the model
   license. Models are verified before compilation and again by Inno's `Hash`
   checks. All forks share `release/assets.json` and a single-EXE size budget of
   4,100,000,000 bytes. RStudio uses tighter application compression for this
   variant.
3. Runs the repository's checks, installs under a separate test identity,
   verifies installed model hashes, exercises local text/picture inference,
   and checks upgrade/uninstall preservation. The source build and installed
   product keep their own existing runtime data paths.
4. Transfers the tested package through a temporary Actions artifact with
   **one-day retention**, then uploads it and its checksum/release notes to
   SourceForge using rsync over pinned-host SSH. GitHub Actions/cache quotas
   still apply; SourceForge's free file hosting does not change GitHub's quotas.
5. Downloads the public mirror copies and verifies their full SHA-256 hashes.
   Mirror availability can lag behind upload, so verification retries before
   publication. Large downloads make this phase take longer than uploading a
   small online installer.
6. Selects the new stable EXE as the default Windows download, then removes
   superseded files in its own channel. Older queued tags cannot replace a newer
   version. Publishing jobs are serialized per repository.
7. Creates the GitHub release with SourceForge links and the small checksum
   asset. GUSEK retains its optional slim portable ZIP on GitHub. The multi-GB
   installer is hosted on SourceForge.

Stable and prerelease channels each retain their newest installer. An RC tag
such as `v0.3.0-rc.1` never replaces the main stable Windows download. If only
stable versions are wanted, publish only stable tags. Unrelated SourceForge
files are not deleted. Interrupted unpromoted candidates can need manual cleanup.

If upload or mirror verification fails, the previous default stays available.
If promotion fails, old files stay available. Retry the failed publisher job,
which uses the same tested artifact. A completed version cannot be overwritten
with different bytes; use a new tag for a changed build. A failed cleanup keeps
its pending cleanup records so the next retry can finish safely. Existing GitHub
releases are not overwritten; if that final step already succeeded, do not
re-run the entire publication merely to create it again.

## Website download buttons

Use one stable URL per project:

```text
https://sourceforge.net/projects/YOUR-PROJECT-SLUG/files/latest/download
```

Put the appropriate URL on each application's Download button. It follows
SourceForge's explicit Windows default, so the website does not need to change
for every version. Until the first successful stable publication there may be
no stable default download; publish a stable tag before adding this button.
Older GitHub release pages retain source history, while
their installer links may be removed by the latest-only retention policy; the
release notes also link to the latest stable installer.

SourceForge also serves `windows/stable/latest.json` and
`windows/prerelease/latest.json` inside the project's Files area. Those manifests
include version, source commit, filenames, exact sizes, hashes, and publication
state. The website can use them for version labels later, if needed.

## Local commands

Run the matching application wrapper from its repository:

```powershell
.\rgui.cmd fetch -BuildRoot D:\rgui-build
.\rgui.cmd installer -Offline -Version 0.2.0 -BuildRoot D:\rgui-build
.\rgui.cmd test -Installer -Offline -BuildRoot D:\rgui-build
```

Replace `rgui` and its build directory with `rstudio` or `gusek` as appropriate.
Build the native application first. RStudio accepts `-ToolsRoot` and `-RHome`
when those dependencies are not registered on the machine. Omitting `-Offline`
still builds the smaller first-use-download installer.

Run publisher and packaging regressions without an account or credentials:

```powershell
python -m unittest discover -s release -p 'test_*.py' -v
powershell -NoProfile -ExecutionPolicy Bypass -File release\test-offline.ps1
```

Actual SourceForge upload, default selection, and pruning need the one-time
account/project configuration and a hosted run. Local checks do not publish.
Keep the common `release/` files and composite action synchronized across forks;
each checkout builds independently without needing sibling repositories.

## References

- [SourceForge file uploads and limits](https://sourceforge.net/p/forge/documentation/Release%20Files%20for%20Download/)
- [Default-download release API](https://sourceforge.net/p/forge/documentation/Using%20the%20Release%20API/)
- [SSH key setup](https://sourceforge.net/p/forge/documentation/SSH%20Keys/)
- [SSH host fingerprints](https://sourceforge.net/p/forge/documentation/SSH%20Key%20Fingerprints/)
