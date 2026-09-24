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

Budget on a 128 GB stick: R about 250 MB, llama.cpp about 100 MB, the
model about 2.5 GB. Everything else is free space.

## 2. Build R with the assistant

Follow `src/gnuwin32/INSTALL` as usual — the assistant adds no new build
dependency. In short, with Rtools installed:

```
cd src\gnuwin32
copy MkRules.dist MkRules.local
rem  edit MkRules.local: set EXT_LIBS and the toolchain paths
make all recommended
```

The changes for the assistant are:

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

Then copy the installed tree to `E:\R`.

## 3. Get llama.cpp

Download a prebuilt Windows x64 CPU binary from the llama.cpp releases
page (`llama-<build>-bin-win-cpu-x64.zip`, or the AVX2 variant, which
every i5 from 2013 onwards supports). Unzip it and copy the contents —
`llama-server.exe` **and every DLL next to it** — into

```
E:\R\ai\llama\
```

Do not put only the .exe there: it will fail to start with a missing-DLL
error that Windows shows in a dialog the assistant cannot read.

Check it once from a command prompt:

```
E:\R\ai\llama\llama-server.exe --version
```

## 4. Get the model

Download **Qwen3.5-4B, GGUF, Q4_K_M** and save it as

```
E:\R\ai\models\Qwen3.5-4B-Q4_K_M.gguf
```

If the file you download has a different name, either rename it or point
`model =` in `E:\R\etc\Rai.conf` at the real name. Q4_K_M for a 4B model
is about 2.4–2.6 GB on disk and needs roughly 3.5–4 GB of RAM at
`ctx_size = 8192`, which is comfortable on a 16 GB machine.

Expect something like 5–12 tokens per second on an i5 with no GPU: a
paragraph of explanation plus a short code block takes well under a
minute. The first load after plugging the stick in is the slow part,
because 2.5 GB has to come off USB.

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
| `The model server exited unexpectedly` | Run `llama-server.exe` by hand from a command prompt with the same `-m` argument and read its output. Usually a missing DLL or a corrupt download. |
| `The model did not become ready within ...` | A slow stick. Raise `startup_timeout`, and try `extra_args = --no-mmap`. |
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
