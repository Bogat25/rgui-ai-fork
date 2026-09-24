# Setting up the RGui AI assistant on a pendrive

Written for the person assembling the stick. Do this once, on a machine
where you can download files and run a build; afterwards the stick works
offline on any Windows 10/11 x64 machine that lets you run a program
from removable media.

## 1. What goes where

```
E:\
├── Start-R.cmd                       copied from R\ai\Start-R.cmd
├── work\                             created on first run
└── R\                                R_HOME: the R you build below
    ├── bin\x64\Rgui.exe
    ├── etc\Rai.conf                  settings for the assistant
    └── ai\
        ├── system_prompt.txt         standing instructions
        ├── context\                  your course material
        ├── llama\                    llama-server.exe and its DLLs
        └── models\                   the .gguf file
```

`ai\` is created by the build, with `system_prompt.txt` and
`context\README.txt` in it. You fill `llama\`, `models\` and `context\`.

Budget on a 128 GB stick: R about 300 MB, llama.cpp about 60 MB, the
model 2.7 GB. Everything else is free space.

## 2. Build it: the pipeline

Everything from here to a finished pendrive is automated by `rgui.cmd`
in the repository root (a wrapper around `rgui.ps1` that does not depend
on the PowerShell execution policy). It needs Rtools45 installed and
about 15 GB free on the build drive; nothing else, and no administrator
rights. No `MkRules.local` is needed with Rtools45.

```
rgui doctor           what is installed, downloaded and built
rgui fetch            Tcl/Tk bundle, llama.cpp, and the model (2.7 GB,
                      SHA-256 checked, resumable)
rgui full             build R, base and recommended packages
rgui test -Real -Gui  every test there is, see below
rgui package          assemble the stick layout in D:\rgui-build\dist
rgui deploy -Drive E: copy it onto the stick
```

The build happens in `D:\rgui-build\tree`, not in the repository:
R's makefiles do not cope with spaces in paths, and it keeps the
repository clean. Only files you changed are copied across, so builds
after the first are incremental. Use `-BuildRoot` to build elsewhere.

### The edit-build-run loop

```
rgui dev              sync, rebuild R.dll + the .exe files, start Rgui
rgui test             compile check with -Werror + unit and protocol tests
```

`dev` takes seconds after the first full build. It closes any Rgui or
llama-server started from the build tree first, because a running Rgui
keeps R.dll locked. It starts Rgui with a throwaway home in
`D:\rgui-build\devhome`, so your own R settings are not involved.

`quick` (inside `dev`) rebuilds the core libraries and `R.dll`. After
editing anything outside `src/gnuwin32` and `src/main` (a base package,
say), use `rgui full` instead; it is incremental too.

### Tests

| Command | What it proves | Time |
|---|---|---|
| `rgui test` | `aichat.c` compiles warning-free with the production flags; JSON, SSE, chunked HTTP, the `<think>` filter, Stop and a missing server behave; captured real `llama-server` output parses correctly | ~10 s |
| `rgui test -Real` | `aichat.c` starts the real `llama-server` itself, the model loads, answers with a fenced code block, and the server is stopped again | ~30 s |
| `rgui test -Gui` | drives the built `Rgui.exe` through window messages: menu entry, panel, all buttons, hide/show/close, Copy code, To editor, R running console code **while** the model answers, a missing model failing politely, and `llama-server` dying with Rgui | ~1–2 min |

When something fails, the script prints the first error lines of the
build log (usually the cause) and the log's path.

`test -Gui` opens and closes real RGui windows and sends them menu
commands, clicks and text. Leave them alone while it runs: a click or a
keystroke of your own changes what the test sees. It borrows the
clipboard for the Copy code check and puts your text back afterwards.

### What the build changes

| File | Change |
|---|---|
| `src/gnuwin32/aichat.c`, `aichat.h` | new, the whole feature |
| `src/gnuwin32/rui.c` | one menu entry in the Misc menu |
| `src/gnuwin32/system.c` | one call to `aichat_shutdown()` on exit |
| `src/gnuwin32/Makefile` | `aichat.c` in `CSOURCES`, `-lws2_32` |
| `src/gnuwin32/Rdll.hide` | keeps the four new symbols out of R.dll's exports |
| `src/gnuwin32/fixed/Makefile` | installs `ai\` |
| `src/gnuwin32/fixed/etc/Rai.conf` | new, the settings file |
| `src/gnuwin32/fixed/ai/` | new, templates |

## 3. Doing it by hand

Only needed without the pipeline. With Rtools45's toolchain first on
`PATH`, `TAR=/usr/bin/tar` and `TAR_OPTIONS=--force-local`, unzip the
Tcl/Tk bundle from CRAN's Rtools45 files page into the source root, run
`sh tools/link-recommended`, then in `src/gnuwin32` run `make -j all`
and **afterwards, separately,** `make -j recommended`: started together
under `-j`, the second races the first and fails.

Then put a llama.cpp Windows CPU build (`llama-<build>-bin-win-cpu-x64.zip`
from its GitHub releases; **all** the DLLs next to `llama-server.exe`)
into `R\ai\llama\`, and `Qwen3.5-4B-Q4_K_M.gguf` from
`unsloth/Qwen3.5-4B-GGUF` on Hugging Face into `R\ai\models\`.
Check the server once with `R\ai\llama\llama-server.exe --version`.

## 4. The model

Qwen3.5-4B Q4_K_M is 2.7 GB on disk and needs roughly 3.5–4 GB of RAM at
`ctx_size = 8192`, which is comfortable on a 16 GB machine. If you use a
file with a different name, point `model =` in `R\etc\Rai.conf` at it.

Expect something like 5–12 tokens per second on an i5 with no GPU: a
paragraph of explanation plus a short code block takes well under a
minute. The first load after plugging the stick in is the slow part,
because 2.7 GB has to come off USB.

## 5. Add the course material

Put the assignment brief, the lecturer's notes, the house reporting
style and a few worked examples into `E:\R\ai\context\` as UTF-8 `.md`
or `.txt` files. `context\README.txt` explains how they are selected and
suggests a naming scheme. Edit `E:\R\ai\system_prompt.txt` if the course
has standing rules that apply to every answer.

No indexing step, no restart: edit a file, ask the next question.

## 6. First run

1. Copy `E:\R\ai\Start-R.cmd` to `E:\Start-R.cmd`.
2. Double-click it. RGui opens exactly as it always does.
3. **Misc → AI assistant**, or Ctrl+T.
4. The status line says `Loading the model...` for a minute or two the
   first time, then `Ready.`
5. Ask something. Use **Copy code** or **To editor**; run the code
   yourself.
6. Ctrl+T again hides the panel. Ctrl+T once more brings it back with
   the conversation intact.

## 7. If something goes wrong

The assistant is a separate process, so none of this can take RGui with
it. The console, the editor, graphics and packages keep working.

| Status line says | Do this |
|---|---|
| `The model server was not found` | Check `server_exe` in `etc\Rai.conf` and that `llama-server.exe` really is in `ai\llama\`. |
| `The model file was not found` | Check `model` in `etc\Rai.conf` against the actual filename in `ai\models\`. |
| `The model server stopped while starting: ...` | The rest of the line is the server's own error. An `invalid argument` means an option in `extra_args` that this llama.cpp does not know. The full output is in `%TEMP%\rgui-llama-server.log`. |
| `The model server exited unexpectedly` | It died without printing an error. Run `llama-server.exe` by hand from a command prompt with the same `-m` argument and read its output. Usually a missing DLL or a corrupt download. |
| `The model did not become ready within ...` | A slow stick. Raise `startup_timeout`. With llama.cpp b11153, `extra_args = --load-mode none` reads the model into RAM up front, which can help on slow USB sticks. |
| `Could not connect to the model server` | Something else is on port 8713. Change `port` in `etc\Rai.conf`. |
| Nothing happens at all, no menu entry | `enabled = no` in `etc\Rai.conf`, or the file has a typo. Delete it to fall back to the defaults. |

To turn the whole feature off, set `enabled = no`. The menu entry then
does not appear and no assistant code runs.

## 8. What it does not do

- It does not run code for you. Ever. You copy it and run it yourself.
- It does not reach the network. It talks to `127.0.0.1` only, and the
  model is a file on the stick.
- It does not learn. The course material is re-read from disk on every
  question; there is no training and no memory between sessions beyond
  the conversation in the open panel.
