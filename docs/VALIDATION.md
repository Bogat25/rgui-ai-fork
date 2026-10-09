# Validation status

Latest implementation validation: **2026-10-09**. Documentation/privacy review:
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
.\rgui.cmd installer -Version 0.1.0 -BuildRoot C:\rgui-build
.\rgui.cmd test -Installer -BuildRoot C:\rgui-build
```

Use a disposable profile for the installer suite: its production application
identity can conflict with a real installation. The GUI suite manipulates real
windows and the clipboard. Distinguish a missing prerequisite or skipped suite
from a passing test.

## Remaining limits

- Hosted default checks do not exercise the whole real-model/desktop matrix.
- Windows 10, other CPUs, accessibility, GPU execution, and diverse clipboard
  sources require separate validation.
- A model reading a test image does not establish accuracy on every plot or page.
- Portable mode still uses host temporary storage when R requires it.
- Packages are unsigned; signing and distribution reputation are separate work.
- Local source and privacy checks do not inspect remote Actions logs or every
  historical release binary.
