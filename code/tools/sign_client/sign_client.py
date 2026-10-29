#!/usr/bin/env python3
"""Minimal UDS client for DuckEmu's signal path.

This is the shape the Web API will use: a 4-byte big-endian length followed by
UTF-8 JSON, one frame per request/response. Retries OVERLOADED while a worker's
instances are still initializing.

    python3 sign_client.py /tmp/duckemu/libeleven.sock 7
    python3 sign_client.py --raw /tmp/duckemu/libeleven.sock 7

By default the response's `value` is decoded with libeleven's code table
(data(n) = custom_b64(f"{n}:{now_ms}")); pass --raw to print it untouched.
"""

import json
import socket
import struct
import sys
import time

LIBELEVEN_ALPHABET = "45AogfZw/9+FcYrSVH1TKM7k3BebEtqCIGPJyQD60lUXpNmhWuRLds8jainzv2Ox"


def decode_libeleven(text):
    bits = 0
    count = 0
    out = bytearray()
    for ch in text:
        if ch == "=":
            break
        bits = (bits << 6) | LIBELEVEN_ALPHABET.index(ch)
        count += 6
        if count >= 8:
            count -= 8
            out.append((bits >> count) & 0xFF)
    return out.decode()


def exchange(path, request):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    try:
        body = json.dumps(request, separators=(",", ":")).encode()
        sock.sendall(struct.pack(">I", len(body)) + body)

        def exact(size):
            buf = b""
            while len(buf) < size:
                chunk = sock.recv(size - len(buf))
                if not chunk:
                    raise SystemExit("connection closed")
                buf += chunk
            return buf

        length = struct.unpack(">I", exact(4))[0]
        return json.loads(exact(length))
    finally:
        sock.close()


def sign(path, value, session, raw):
    for attempt in range(600):
        request = {
            "version": 1,
            "id": "cli-%d" % attempt,
            "app": "libeleven",
            "op": "sign",
            "params": {"value": str(value)},
        }
        if session:
            request["session"] = {"id": session}
        response = exchange(path, request)
        if response.get("ok"):
            encoded = response["result"]["value"]
            shown = encoded if raw else decode_libeleven(encoded)
            print("instance=%s  %s  meta=%s" % (
                response["instance"], shown, json.dumps(response["meta"])))
            return response
        if response.get("error", {}).get("code") == "OVERLOADED":
            time.sleep(0.1)
            continue
        print("error:", json.dumps(response.get("error"), ensure_ascii=False))
        return None
    raise SystemExit("timed out waiting for a ready instance")


def main():
    args = sys.argv[1:]
    raw = False
    if args and args[0] == "--raw":
        raw = True
        args = args[1:]
    if len(args) < 1:
        print(__doc__)
        return 2
    path = args[0]
    value = args[1] if len(args) > 1 else "7"

    # First request opens a session, the second pins to the same instance.
    first = sign(path, value, None, raw)
    if first is not None:
        sign(path, value, first["instance"], raw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
