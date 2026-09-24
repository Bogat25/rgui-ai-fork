# Tests for the AI assistant's non-GUI half

These are not part of the R build. They exist so that the risky parts of
`aichat.c` — HTTP chunked decoding, SSE framing, JSON escaping and
unescaping, the `<think>` filter, cancellation — can be exercised without
building all of R and without a real model.

`test_aichat.c` `#include`s `aichat.c` so that its static functions are
reachable, and `stubs.h` supplies the couple of dozen GraphApp and R
symbols it references. Nothing is drawn, so the stubs can be empty.

`fakeserver.py` stands in for `llama-server`. It answers `/health`, and
for `/v1/chat/completions` it emits a chunked SSE stream cut into small,
deliberately uneven pieces so that SSE records, JSON tokens and even the
`<think>` tags straddle chunk boundaries. It `json.loads()` the request
body, so a malformed request fails the test rather than passing quietly.

## Running them

Any reasonably recent mingw-w64 gcc will do; Rtools' compiler works.
`config.h`, `Rconfig.h`, `psignal.h`, `iconv.h` and `Rversion.h` are not
in `../../include` until R has been configured, so point `-I` at a
directory holding copies:

```sh
mkdir -p /tmp/inc
cp ../fixed/h/*.h /tmp/inc/
sh ../../../tools/GETVERSION > /tmp/inc/Rversion.h

gcc -O1 -g -std=gnu2x \
    -I. -I/tmp/inc -I../../include -I.. -I../../extra \
    -DHAVE_CONFIG_H -DR_DLL_BUILD -DWin32 \
    test_aichat.c -o test_aichat.exe -lws2_32
```

If `libintl.h` is missing (it comes from the external toolchain tree, not
from this source tree) either add `-I` for it or drop a one-line stub
into `/tmp/inc`.

Then, in two shells:

```sh
python fakeserver.py 45712 --expected     # writes expected.txt
python fakeserver.py 45712                # leave running
./test_aichat.exe 45712 expected.txt
```

Pick a port that Windows has not reserved: `netsh interface ipv4 show
excludedportrange protocol=tcp` lists the ones that will fail to bind
with WinError 10013.

## What is covered

| Test | What would break without it |
|---|---|
| `json_find_string` plain / nested / escapes | Tokens silently dropped |
| `json_find_string` key name inside a value | Wrong field read from a chunk |
| `\u` and surrogate pairs | Mangled accented characters and emoji |
| `content: null` | The role-only first chunk emitting garbage |
| `json_escape` | Invalid request body for any multi-line question |
| `<think>` filter, fed one byte at a time | Reasoning text leaking into the transcript |
| `ai_extract_code` | Copy code producing the wrong text |
| `ai_relevance` | Course files selected at random |
| `ai_build_request` shape | Server rejecting the conversation |
| end-to-end stream | Chunked decoding and SSE framing |
| awkward question | The `json_escape` regression above, over the wire |
| cancel mid-stream | Stop not working, or hanging |
| no server | A hang or crash when the model is missing |

## What is not covered

Everything above the `user interface` marker in `aichat.c`: window
creation, layout, the buttons, the EDIT-control helpers and the
hide/show behaviour. Those need a real GraphApp and a real RGui, so they
have to be checked by running the built Rgui.exe.
