# Local assistant architecture

## Boundaries

RGui hosts a native GraphApp assistant window with Unicode RichEdit controls.
The model runs in a separate llama.cpp process. Workers perform network,
download, and generation work while the R console and editor remain available.
The UI owns conversation state; closing the assistant hides it, and application
shutdown stops owned work and processes.

```text
RGui window -> editable question + explicit attachments
            -> prompt + ranked note excerpts + bounded chat/images
            -> worker -> 127.0.0.1:8713 -> llama-server -> local GGUF/projector
            <- UI events <- streamed HTTP/SSE response
            -> Copy code / To editor -> user reviews and runs R
```

Note lookup is bounded lexical selection, not an embedding service. Each request
reads the editable prompt and reference files. Text and pictures are retained
for bounded follow-ups. There is no training pass or automatic code execution.

## Source map

| Location | Responsibility |
| --- | --- |
| [aichat.c](../src/gnuwin32/aichat.c), [aichat.h](../src/gnuwin32/aichat.h) | Configuration, window, context, workers, HTTP/SSE, downloads, cancellation, history, editor actions |
| [aiimage.c](../src/gnuwin32/aiimage.c), [aiimage.h](../src/gnuwin32/aiimage.h) | GDI+ decoding, scaling, encoding, clipboard, thumbnails, picture viewer |
| [aiplot.c](../src/gnuwin32/aiplot.c) | Export from the R Windows graphics device |
| [rui.c](../src/gnuwin32/rui.c) | Main menu integration |
| [system.c](../src/gnuwin32/system.c) | Assistant shutdown during RGui exit |
| [Makefile](../src/gnuwin32/Makefile), [Rdll.hide](../src/gnuwin32/Rdll.hide) | Compilation, Windows libraries, exported-symbol control |
| [fixed/ai](../src/gnuwin32/fixed/ai), [Rai.conf](../src/gnuwin32/fixed/etc/Rai.conf) | Installed defaults, prompt, notes scaffold, portable launcher |
| [aitests](../src/gnuwin32/aitests) | Native/control regressions, fake server, real/GUI fixtures |
| [rgui.ps1](../rgui.ps1), [packaging](../packaging) | Build isolation, verified assets, distribution, installer and workflow rehearsal |

The runtime uses loopback by default. A healthy externally started local server
may be reused; process cleanup must remain limited to children this application
owns. Keep worker state out of R's evaluation thread and do not turn editor
insertion into implicit evaluation.

## Relationship to the other forks

RGui AI, RStudio AI, and GUSEK AI share the local-model approach, pinned assets,
explicit attachments, note lookup, and user-reviewed code actions. They are
separate applications, not a shared live service. Defaults use ports 8713,
18713, and 28713 respectively, with separate profiles and process ownership.
RStudio integrates a session/GWT/Node Chat provider; GUSEK uses a docked C++ pane
and MathProg/solver context. The implementations are maintained independently.

## Maintenance contract

This hard fork does not automatically receive upstream fixes. Review dependency
updates deliberately, retain original notices, validate new server flags against
the pinned runtime, and exercise real image requests whenever image handling
changes. Fake-server success proves framing and UI integration, not model quality.
