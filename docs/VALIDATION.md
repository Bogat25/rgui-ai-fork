# Validation status

Latest implementation validation: **2026-10-23**. Documentation/privacy review:
**2026-10-07**. This page describes the available
verification and its limits; it does not certify every R feature or machine.

The repository includes meaningful native, streaming-protocol, actual-control,
real-model, GUI, and installer tests. Their coverage and invocation are described
in [BUILDING.md](BUILDING.md), the [harness README](../src/gnuwin32/aitests/README.md),
and the [portable setup guide](../src/gnuwin32/fixed/ai/SETUP.md).

The reviewed source includes picture inference, file/clipboard/plot attachments,
thumbnail viewers, Unicode formatting, code copying/insertion, bounded history,
resumable downloads, and selection/scroll preservation during streaming.
No new full native build, real-model run, GUI run, or installer execution was
performed for the 2026-10-07 documentation/privacy change. Check a particular tag's
workflow and release evidence before treating its package as verified.

## Complete offline installers and SourceForge, 2026-10-23

Local Windows checks passed for the shared release infrastructure and RGui's
offline installer. The package includes the pinned Qwen text model, picture
reader, CPU runtime and model license in one EXE.

- Production compiler checks, native/control, streaming and downloader suites
  passed with `rgui.cmd test -Installer -Offline`.
- A complete installer compiled to **3,498,498,814 bytes**, within the
  4,100,000,000-byte release budget.
- The installer suite used a unique application/registry/shortcut identity. It
  verified both installed model hashes and real CPU text/picture inference,
  launched RGui from a path containing spaces, preserved edited configuration,
  prompt and notes on upgrade, and preserved work and custom models on uninstall.
- Shared publisher regressions cover verification before promotion, independent
  prereleases, downgrade rejection, credential-safe errors and retryable cleanup.

The package used the existing built runtime; this pass did not repeat a full R
build or the entire assistant GUI matrix. The hosted workflow builds its tagged
source before packaging. SourceForge's public SSH host fingerprint was checked;
authenticated upload, default-download selection and remote pruning remain
untested until the account/projects and GitHub environment are configured. No
hosted workflow was triggered and no remote release was published.

## Shortcut-only assistant access, 2026-10-09

The assistant starts closed and opens/closes through the configured Ctrl+letter
(default Ctrl+T). Console and assistant menu entries were removed. A GraphApp
keyboard filter handles the shortcut independently of menus; hiding preserves
the transcript and holding the key does not repeatedly toggle the window.

Validated locally on Windows 11:

- `rgui.cmd quick`: rebuilt GraphApp, core libraries and R.dll successfully.
- `rgui.cmd test -Gui -ShortcutOnly`: production compiler checks, native/control
  and streaming tests against synthetic and captured streams, and 24 actual
  GUI assertions passed. The shortcut works from the console, question box,
  transcript and script editor. Startup and both menu bars expose no assistant
  opening control; the transcript survives hiding/reopening.

The shortcut suite uses build-tree configuration overrides restored afterward.
It does not access the clipboard or download models. Full clipboard/real-model
GUI and installer suites were not rerun for this change. No hosted workflow was
triggered and no release was published.

## Checks to run before a release

```powershell
.\rgui.cmd test -BuildRoot C:\rgui-build
.\rgui.cmd test -Real -Gui -BuildRoot C:\rgui-build
.\rgui.cmd installer -Offline -Version 0.1.0 -BuildRoot C:\rgui-build
.\rgui.cmd test -Installer -Offline -BuildRoot C:\rgui-build
```

The installer suite compiles a separate test identity. The GUI suite manipulates
real windows and the clipboard. Distinguish a missing prerequisite or skipped
suite from a passing test.

## Remaining limits

- Hosted default checks do not exercise the whole real-model/desktop matrix.
- Windows 10, other CPUs, accessibility, GPU execution, and diverse clipboard
  sources require separate validation.
- A model reading a test image does not establish accuracy on every plot or page.
- Portable mode still uses host temporary storage when R requires it.
- Packages are unsigned; signing and distribution reputation are separate work.
- Local source and privacy checks do not inspect remote Actions logs or every
  historical release binary.
