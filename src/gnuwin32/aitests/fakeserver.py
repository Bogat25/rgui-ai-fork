"""A stand-in for llama-server that speaks the same wire protocol.

Exercises the parts of aichat.c that are easy to get wrong:
  - chunked transfer encoding, with SSE records split across chunks
  - a <think> block whose opening and closing tags straddle chunks
  - \\u escapes, including a surrogate pair
  - a role-only first delta whose "content" is null
"""
import base64, hashlib, io, json, socket, sys, threading, time

PORT = int(sys.argv[1])

# --replay FILE: send a captured llama-server SSE stream instead of the
# synthetic one.  With --expected, write the text that stream should
# produce, taken from its "content" deltas.
REPLAY = None
if "--replay" in sys.argv:
    REPLAY = sys.argv[sys.argv.index("--replay") + 1]


def replay_expected(path):
    text = []
    for line in io.open(path, encoding="utf-8").read().split("\n"):
        if not line.startswith("data: ") or line[6:].strip() == "[DONE]":
            continue
        for ch in json.loads(line[6:]).get("choices", []):
            c = ch.get("delta", {}).get("content")
            if isinstance(c, str):
                text.append(c)
    return "".join(text)

PIECES = [
    None,                      # role-only chunk: "content": null
    "<thi", "nk>", "I should use t.test here.", "</thi", "nk>",
    "Use ", "`t.test()`", ":\n\n```r\n",
    "t.test(len ~ supp, data = ToothGrowth)\n",
    "```\n\n",
    "Quotes: \"x\", backslash: \\, ",
    "unicode: éá and \U0001F600 done.",
]

# Asked something containing NOCODE, the server answers in prose only,
# so the GUI test can check what Copy code does with an answer that has
# no code block.
PIECES_NOCODE = ["A p-value is the probability of data at least this ",
                 "extreme if the null hypothesis were true."]

EXPECTED = ("Use `t.test()`:\n\n```r\n"
            "t.test(len ~ supp, data = ToothGrowth)\n"
            "```\n\n"
            "Quotes: \"x\", backslash: \\, "
            "unicode: éá and \U0001F600 done.")


# A stand-in for the model file: 8 MB of deterministic bytes, served at
# /file/<anything> with Range support.  "?slow" in the URL spreads it
# over a few seconds, so Stop and "R stays usable" can be tested.
DOWNLOAD = bytes((i * 31 + 7) & 0xFF for i in range(8_000_000))


def serve_file(conn, head, target):
    rng = None
    for h in head.decode(errors="replace").split("\r\n")[1:]:
        if h.lower().startswith("range:"):
            rng = h.split(":", 1)[1].strip()
    io.open("last_range.txt", "w").write(rng or "none")
    start = 0
    if rng and rng.startswith("bytes=") and rng.endswith("-"):
        start = int(rng[6:-1])
    if start >= len(DOWNLOAD):
        conn.sendall(b"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n"
                     b"Connection: close\r\n\r\n")
        return
    body = DOWNLOAD[start:]
    status = b"206 Partial Content" if start else b"200 OK"
    conn.sendall(b"HTTP/1.1 " + status + b"\r\nContent-Type: application/octet-stream"
                 b"\r\nContent-Length: " + str(len(body)).encode() +
                 b"\r\nConnection: close\r\n\r\n")
    slow = "slow" in target
    for i in range(0, len(body), 65536):
        try:
            conn.sendall(body[i:i + 65536])
        except OSError:
            return                      # the client went away (Stop)
        if slow:
            time.sleep(0.04)


def message_parts(msg):
    """Text and pictures of one message: content is a string, or a list
    of parts in the OpenAI format when pictures are attached."""
    c = msg.get("content") or ""
    if isinstance(c, str):
        return c, []
    text = "".join(p.get("text", "") for p in c if p.get("type") == "text")
    pics = [p["image_url"]["url"] for p in c if p.get("type") == "image_url"]
    return text, pics


def save_pictures(urls):
    """Write the pictures of the last question to received-N.png|jpg and
    a summary to received.txt, for the GUI test to look at."""
    names = []
    for i, url in enumerate(urls, 1):
        head, _, data = url.partition(",")
        ext = "jpg" if "jpeg" in head else "png"
        name = "received-%d.%s" % (i, ext)
        io.open(name, "wb").write(base64.b64decode(data))
        names.append(name)
    return names


def sse_record(piece):
    if piece is None:
        delta = {"role": "assistant"}
    else:
        delta = {"content": piece}
    obj = {"id": "x", "object": "chat.completion.chunk",
           "choices": [{"index": 0, "delta": delta, "finish_reason": None}]}
    return "data: " + json.dumps(obj, ensure_ascii=False) + "\n\n"


def chunk(data_bytes):
    return ("%x\r\n" % len(data_bytes)).encode() + data_bytes + b"\r\n"


def handle(conn):
    conn.settimeout(10)
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = conn.recv(65536)
        if not d:
            conn.close()
            return
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    line0 = head.split(b"\r\n")[0].decode()

    if line0.startswith("GET /health"):
        body = b'{"status":"ok"}'
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     b"Content-Length: " + str(len(body)).encode() +
                     b"\r\nConnection: close\r\n\r\n" + body)
        conn.close()
        return

    if line0.startswith("GET /file/"):
        serve_file(conn, head, line0.split(" ")[1])
        conn.close()
        return

    if not line0.startswith("POST /v1/chat/completions"):
        conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                     b"Connection: close\r\n\r\n")
        conn.close()
        return

    # Read the request body so we can echo its length back for checking.
    clen = 0
    for h in head.decode(errors="replace").split("\r\n")[1:]:
        if h.lower().startswith("content-length:"):
            clen = int(h.split(":", 1)[1])
    body = rest
    while len(body) < clen:
        d = conn.recv(65536)
        if not d:
            break
        body += d
    req = json.loads(body.decode("utf-8"))
    sys.stderr.write("server: %d messages, system prompt %d chars\n"
                     % (len(req["messages"]), len(message_parts(req["messages"][0])[0])))

    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                 b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n")

    # Deliberately awkward framing: one byte stream cut at arbitrary
    # points, so SSE records and even JSON tokens straddle chunks.
    if REPLAY:
        raw = io.open(REPLAY, "rb").read()
    else:
        msgs = req.get("messages") or []
        last, pics = message_parts(msgs[-1]) if msgs else ("", [])
        total = sum(len(message_parts(m)[1]) for m in msgs)
        if pics or total:
            names = save_pictures(pics)
            io.open("received.txt", "w").write("last=%d total=%d %s\n" % (len(pics), total, " ".join(names)))
            # A canned answer about the pictures, with the counts in it.
            pieces = [None, "I can see ", "PICTURES last=%d total=%d" % (len(pics), total),
                      ". It is **a plot** of six boxes."]
        else:
            pieces = PIECES_NOCODE if "NOCODE" in last else PIECES
        stream = "".join(sse_record(p) for p in pieces) + "data: [DONE]\n\n"
        raw = stream.encode("utf-8")
    i, size = 0, 7
    while i < len(raw):
        conn.sendall(chunk(raw[i:i + size]))
        i += size
        size = 7 if size > 40 else size + 11
        time.sleep(0.001)
    conn.sendall(b"0\r\n\r\n")
    conn.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(8)
    sys.stderr.write("server: listening on %d\n" % PORT)
    sys.stderr.flush()
    deadline = time.time() + 300
    while time.time() < deadline:
        srv.settimeout(2)
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    if "--expected" in sys.argv:
        text = replay_expected(REPLAY) if REPLAY else EXPECTED
        io.open("expected.txt","w",encoding="utf-8",newline="").write(text)
        io.open("download.sha256", "w").write(hashlib.sha256(DOWNLOAD).hexdigest())
        io.open("download.size", "w").write(str(len(DOWNLOAD)))
    else:
        main()
