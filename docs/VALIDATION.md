# Validation status

Documentation review date: **2026-10-07**. This page describes the available
verification and its limits; it does not certify every R feature or machine.

The repository includes meaningful native, streaming-protocol, actual-control,
real-model, GUI, and installer tests. Their coverage and invocation are described
in [BUILDING.md](BUILDING.md), the [harness README](../src/gnuwin32/aitests/README.md),
and the [portable setup guide](../src/gnuwin32/fixed/ai/SETUP.md).

The reviewed source includes picture inference, file/clipboard/plot attachments,
thumbnail viewers, Unicode formatting, code copying/insertion, bounded history,
resumable downloads, and selection/scroll preservation during streaming.
No new full native build, real-model run, GUI run, or installer execution was
performed for this documentation/privacy change. Check a particular tag's
workflow and release evidence before treating its package as verified.

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
