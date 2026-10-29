#!/usr/bin/env python3
"""Throughput/latency benchmark for the DuckEmu signal path.

Each client keeps one connection, establishes a session on the first request
and pins to that instance afterwards. Warmup requests are excluded from the
statistics; only the final summary is printed.

    python3 bench.py /tmp/duckemu/libeleven.sock --clients 8 --requests 10000
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time
from collections import Counter


def recv_exact(sock, size):
    buf = b""
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("peer closed the connection")
        buf += chunk
    return buf


def rpc(sock, request):
    body = json.dumps(request, separators=(",", ":")).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    length = struct.unpack(">I", recv_exact(sock, 4))[0]
    return json.loads(recv_exact(sock, length))


def connect(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    # The System may already be listening while its first worker is still
    # starting; retry the connect itself for a short while.
    deadline = time.time() + 60
    while True:
        try:
            sock.connect(path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() > deadline:
                raise
            time.sleep(0.05)


def wait_for_instances(path, target, timeout_s=120):
    """Wait until `target` distinct instances answer (all warm)."""
    seen = set()
    deadline = time.time() + timeout_s
    sock = connect(path)
    try:
        while len(seen) < target and time.time() < deadline:
            response = rpc(sock, make_request(0, 0, None))
            if response.get("ok"):
                seen.add(response["instance"])
            else:
                time.sleep(0.05)
    finally:
        sock.close()
    return seen


def make_request(client, value, session):
    request = {
        "version": 1,
        "id": "bench-%d-%d" % (client, value),
        "app": "libeleven",
        "op": "sign",
        "params": {"value": str(value)},
    }
    if session:
        request["session"] = {"id": session}
    return request


def run_client(index, path, count, barrier, results, lock):
    try:
        sock = connect(path)
        # Warmup / session establishment (excluded from the statistics).
        session = None
        while True:
            response = rpc(sock, make_request(index, 0, None))
            if response.get("ok"):
                session = response["instance"]
                break
            if response.get("error", {}).get("code") != "OVERLOADED":
                raise RuntimeError(json.dumps(response.get("error")))
            time.sleep(0.05)

        barrier.wait()
        latencies = []
        instances = Counter()
        retries = 0
        errors = 0
        for value in range(1, count + 1):
            start = time.perf_counter()
            response = None
            while True:
                try:
                    response = rpc(sock, make_request(index, value, session))
                except (ConnectionError, OSError):
                    # Worker restarted: reconnect and re-establish the session.
                    sock.close()
                    sock = connect(path)
                    response = rpc(sock, make_request(index, value, None))
                if response.get("ok"):
                    break
                code = response.get("error", {}).get("code")
                if code == "OVERLOADED":
                    retries += 1
                    time.sleep(0.002)
                    continue
                if code in ("SESSION_NOT_FOUND", "SESSION_GONE"):
                    response = rpc(sock, make_request(index, value, None))
                    if response.get("ok"):
                        session = response["instance"]
                        break
                errors += 1
                break
            elapsed = time.perf_counter() - start
            if response is not None and response.get("ok"):
                latencies.append(elapsed)
                instances[response["instance"]] += 1
        with lock:
            results.append((latencies, retries, errors, instances))
    finally:
        barrier.abort()


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    position = min(len(sorted_values) - 1, int(len(sorted_values) * fraction))
    return sorted_values[position]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket", help="System UDS path")
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--requests", type=int, default=10000,
                        help="total requests across all clients")
    parser.add_argument("--instances", type=int, default=4,
                        help="wait for this many warm instances before timing")
    args = parser.parse_args()

    ready = wait_for_instances(args.socket, args.instances)
    print("warm       : %d/%d instances" % (len(ready), args.instances))

    per_client = args.requests // args.clients
    total = per_client * args.clients
    if per_client == 0:
        print("--requests must be at least --clients", file=sys.stderr)
        return 2

    barrier = threading.Barrier(args.clients + 1)
    results = []
    lock = threading.Lock()
    threads = [
        threading.Thread(target=run_client,
                         args=(index, args.socket, per_client, barrier, results, lock))
        for index in range(args.clients)
    ]
    for thread in threads:
        thread.start()

    barrier.wait()
    start = time.perf_counter()
    for thread in threads:
        thread.join()
    wall = time.perf_counter() - start

    latencies = []
    retries = 0
    errors = 0
    instances = Counter()
    for client_latencies, client_retries, client_errors, client_instances in results:
        latencies.extend(client_latencies)
        retries += client_retries
        errors += client_errors
        instances.update(client_instances)

    latencies.sort()
    ok = len(latencies)
    mean = sum(latencies) / ok if ok else 0.0
    print("requests   : %d ok, %d error (of %d)" % (ok, errors, total))
    print("clients    : %d persistent connections" % args.clients)
    print("wall       : %.3f s" % wall)
    print("throughput : %.0f req/s" % (ok / wall if wall > 0 else 0.0))
    print("latency    : mean %.3f ms  p50 %.3f ms  p90 %.3f ms  p95 %.3f ms  "
          "p99 %.3f ms  max %.3f ms" % (
              mean * 1e3,
              percentile(latencies, 0.50) * 1e3,
              percentile(latencies, 0.90) * 1e3,
              percentile(latencies, 0.95) * 1e3,
              percentile(latencies, 0.99) * 1e3,
              (latencies[-1] if latencies else 0.0) * 1e3))
    print("retries    : %d OVERLOADED" % retries)
    if instances:
        print("instances  : " + "  ".join(
            "%s=%d" % (name, count) for name, count in sorted(instances.items())))
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
