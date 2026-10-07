# RGui AI

**R for Windows with a local AI assistant for statistics, code, plots, and course notes.**

This is an independent **hard fork** of R's source tree. Local AI is the main
development goal. There is no plan to keep this repository synchronized with
upstream R long term; fixes and dependency updates are maintained here. This is
not an official R Project distribution. Original authorship and licenses remain
in place.

## What you can do

- Ask questions while R and the script editor remain usable; answers stream in
  a separate native assistant window.
- Search your own reference notes through bounded, keyword-ranked context
  selection. This is local document lookup, not an Internet search service.
- Attach the current script, recent console output, the last error, the current
  plot, picture files, or a screenshot pasted with Ctrl+V.
- Read formatted answers in Unicode, preview attached pictures, and ask follow-up
  questions about them. Up to four recent pictures are sent per request.
- Copy generated code or insert it into the script editor. You decide what to run.
- Use a per-user installation or a portable USB layout without administrator
  permissions.

The default assistant uses **Qwen3.5-4B Q4_K_M** and its multimodal projector
through the **llama.cpp CPU runtime**. No cloud AI account, API key, GPU, Python,
or Node installation is required to use the packaged assistant. Model downloads
need a network connection; inference works offline after setup. Other R features,
packages, and user code can still access the network.

## Install and start

1. Open this repository's **Releases** page and choose an installer from a
   successful version-tag build, if available. Check its adjacent SHA-256 file.
2. Run `RGui-AI-<version>-setup.exe` and choose a writable installation folder.
   The installer targets Windows 10 1903+ x64; Windows 11 x64 is the development
   target. Packages are currently unsigned.
3. Start **RGui AI** from its shortcut. For a portable copy, use `Start-R.cmd`
   beside the `R` directory.
4. Open **Misc > AI assistant**, or press **Ctrl+T**. Accept the initial model
   and picture-reader download, approximately **3.4 GB** in total.
5. Type a question and select **Send** or press **Ctrl+Enter**. **Stop** cancels
   an answer or pauses a download; **New chat** clears the conversation.

Keep several GB of free disk space for the models and additional RAM for R,
the model, and picture processing. Performance depends on the CPU, context size,
and storage speed; see the [setup guide](src/gnuwin32/fixed/ai/SETUP.md).

## Add your reference material

Put UTF-8 `.txt`, `.md`, `.R`, `.Rmd`, or `.csv` files in `R/ai/context` in a
packaged installation. Edit `R/ai/system_prompt.txt` for standing instructions.
Files are read for each question; there is no training or indexing step.
Model settings live in `R/etc/Rai.conf`. Keep private notes and real conversations
outside the source repository.

## Build from source

Install Rtools45 and Python 3 for the test harness. From this checkout:

```powershell
.\rgui.cmd doctor -BuildRoot C:\rgui-build
.\rgui.cmd fetch -NoModel -BuildRoot C:\rgui-build
.\rgui.cmd full -BuildRoot C:\rgui-build
.\rgui.cmd test -BuildRoot C:\rgui-build
.\rgui.cmd installer -Version 0.1.0 -BuildRoot C:\rgui-build
```

`0.1.0` is an example fork version. Use your intended version consistently.
BuildRoot must be outside the checkout and contain no spaces. Omit `-NoModel`
when fetching to enable real-model tests or prepare an offline portable copy.
The installer excludes model weights and offers their download on first use.

## Documentation

| Guide | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Start here for users and maintainers |
| [Assistant guide](docs/USER-GUIDE.md) | Attachments, note lookup, settings, privacy, troubleshooting |
| [Build and release guide](docs/BUILDING.md) | Toolchain, edit/build cycle, tests, packaging, tags |
| [Architecture](docs/ARCHITECTURE.md) | Source map, request flow, ownership, integration boundaries |
| [Validation status](docs/VALIDATION.md) | Available checks and limits of the published evidence |
| [Privacy review](docs/PRIVACY-AUDIT.md) | Repository audit scope, findings, and history limitations |
| [Contributing](CONTRIBUTING.md) / [Security](SECURITY.md) | Reports, development, and sensitive data |

The original [R README](README), [installation instructions](INSTALL), and
[R manuals](doc/manual) remain available. They describe upstream R and its build
system; the guides above describe this fork's Windows assistant and packaging.

## License and origins

R and this fork's native assistant use **GPL-2.0-or-later**; see [COPYING](COPYING)
and individual file notices. Bundled components retain their own terms. llama.cpp
and model assets have separate notices and licenses: downloading a model does not
change its license. Original R contributors are listed in [doc/AUTHORS](doc/AUTHORS).
