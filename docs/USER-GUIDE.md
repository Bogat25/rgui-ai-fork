# Using RGui AI

## Installation and portable use

Use the installer and checksum from this fork's Releases page, when a release is
available. It installs for the current user, with `PrivilegesRequired=lowest`.
Select a writable folder. The installer contains R, the assistant, and the CPU
runtime; the model and picture reader download separately.

For a portable setup, assemble the distribution with `rgui.cmd package` and copy
the contents of BuildRoot's `dist` directory to a writable folder or USB drive:

```text
Start-R.cmd
R/
  bin/x64/Rgui.exe
  etc/Rai.conf
  ai/system_prompt.txt
  ai/context/
  ai/llama/
  ai/models/
work/
```

Launch through `Start-R.cmd` or the installer shortcut. The launcher keeps R's
home, history, user library, and user startup files under `work`. It uses portable
temporary storage when a usable path without spaces is available, otherwise the
Windows temp directory. Portable use is not a promise that Windows or R packages
leave no traces on the host.

Upgrades preserve the assistant configuration and prompt. Uninstall removes
application files, model weights under `R/ai/models`, and `work/tmp`, while keeping
the remaining `work` data. Back up custom notes, prompts, settings, and coursework
before upgrades or removal; do not depend on uninstall as a data-erasure tool.

## Ask and review

Press **Ctrl+T** to open or close the assistant. It starts closed; there are no
assistant menu entries or toolbar buttons. Enter adds a newline and Ctrl+Enter
sends. The model warms when the window opens. **Stop** cancels generation or
pauses downloading. Hiding the window preserves the chat; **New chat** clears it.
Conversation state is in memory and is not a persistent chat archive.

Answers stream with Markdown-style code blocks, lists, and emphasis. You can
scroll or select earlier text while an answer streams. **Copy code** extracts
fenced code from the latest answer. **To editor** inserts code in an open script
or opens a new script. An answer without a code block is reported as such.
Neither action executes the code. Check generated statistics and run R yourself.

## Attach text and pictures

The **Attach** menu can add the current script, recent console output, or the
last console error to the editable question. Review these excerpts before sending.
The assistant receives selected context, not unrestricted access to your R session.

Use **Current plot** for the R graphics device's image, **Picture file...**, or
**Picture from the clipboard**. Ctrl+V in the question box can attach a screenshot
or image files copied in Explorer. Supported picture formats include PNG, JPEG,
BMP, GIF, and TIFF. Pictures are scaled and encoded before inference.

Attached thumbnails open a viewer; their **x** removes an attachment. **Remove
pictures** clears pending pictures. Sent thumbnails can be opened from the
transcript. Pictures remain available for follow-ups, with at most four recent
images per request. The multimodal projector must be installed and `vision=yes`.
Text chat remains available when vision is disabled.

## Reference-note search

Put UTF-8 notes in the configured `context_dir`, normally `R/ai/context`. Supported
text files are `.txt`, `.md`, `.R`, `.Rmd`, and `.csv`. README scaffolding is
excluded. Files sharing words with your question are prioritized and excerpts
are bounded by `context_max_chars` (default 12000).

This is lightweight keyword selection of local reference material. It does not
browse the web, train the model, parse every PDF, or build an embedding database.
Convert a PDF to text yourself or attach a relevant page as a picture. Use one
topic per file, descriptive filenames, and a direct question. Changes to notes
and the system prompt apply to subsequent questions without an indexing step.

## Configuration

Edit `R/etc/Rai.conf`; restart RGui after changing launch or model settings.
Paths are relative to R_HOME. The file documents every setting.

| Setting | Purpose / shipped default |
| --- | --- |
| `enabled`, `hotkey` | Enable assistant; Ctrl+T |
| `server_exe`, `model`, `vision_model` | CPU server, GGUF model, projector paths |
| `host`, `port` | Local endpoint: `127.0.0.1:8713` |
| `autostart`, `startup_timeout`, `request_timeout` | Server startup and timeout control |
| `threads`, `gpu_layers` | Automatic thread choice; CPU inference (`0` GPU layers) |
| `ctx_size`, `n_predict`, `keep_history` | 8192-token context, 1024-token answer, 12 earlier messages |
| `vision`, `image_max_tokens` | Enable pictures; 256-token image budget |
| `thinking`, `strip_think` | Thinking disabled; hidden reasoning stripped |
| `context_dir`, `system_prompt_file` | Course references and standing instructions |
| `model_url`, `vision_url` and hashes | Download source, integrity pins, expected sizes |

Use the shipped model/projector pair. A different model may need a matching
projector, compatible server options, and updated integrity settings. Keep the
host on loopback; the model server is not a public authenticated service.

## Privacy and troubleshooting

Inference goes to the local CPU server. Downloading dependencies or model files
contacts their hosts; R packages and code have their own network behavior. Notes,
attachments, generated text, clipboard contents, R history, and server logs may
contain sensitive material. They remain ordinary local data, without application
encryption. Share a synthetic reproduction rather than a real course or work file.

| Symptom | Check |
| --- | --- |
| Assistant command missing | `enabled` and `hotkey` in Rai.conf |
| Model or server missing | Paths and all runtime DLLs; finish the verified download |
| Download paused or checksum failed | Resume; check free space and matching URL/hash pair |
| Picture is not understood | Confirm thumbnail, `vision=yes`, matching projector; ask about visible details |
| Slow first answer | Model load from storage; reduce image budget/context or allow more startup time |
| Startup or connection error | Status text and `%TEMP%\rgui-llama-server.log`; check port conflicts |
| Generated code is wrong | Reproduce in R and attach the error and relevant notes |

See the [detailed setup guide](../src/gnuwin32/fixed/ai/SETUP.md) for additional
diagnostics, and [SECURITY.md](../SECURITY.md) before sharing logs.
