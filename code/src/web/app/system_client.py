"""DuckDroid2 System 的外部协议客户端。

协议（见 DuckDroid2 docs/devlogs/2026-10-29-19-M7设计-进程模型与网关.md §4.1）：

    [4B 大端长度][UTF-8 JSON]

每个 app 一个 UDS listener，连接本身决定 app。一次请求的响应：

    {"ok": true, "app": ..., "instance": ..., "result": {...},
     "error": {...}, "meta": {...}}

这个客户端只做三件事：发帧、收帧、把可重试的错误重试掉。它不解析业务结果，
所以换成 libeleven / szstone 也只是换 socket 和 params。
"""

from __future__ import annotations

import json
import socket
import struct
import time
import uuid
from typing import Any

from .config import Settings

MAX_FRAME_BYTES = 16 * 1024 * 1024

# 实例还在预热 / 排队满时的临时错误，值得在 deadline 内重试。
RETRY_CODES = frozenset({"OVERLOADED", "INITIALIZING"})
# 会话绑定的实例已经不存在或已被回收：丢掉 session 重新分配。
SESSION_LOST_CODES = frozenset({"SESSION_NOT_FOUND", "SESSION_GONE"})


class SystemClientError(Exception):
    """System 不可达、帧损坏或超时。"""

    def __init__(self, message: str, *, code: str = "SYSTEM_CLIENT_ERROR") -> None:
        super().__init__(message)
        self.code = code


def _recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = bytearray()
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise SystemClientError(
                "System 关闭了 UDS 连接", code="CONNECTION_CLOSED"
            )
        buf.extend(chunk)
    return bytes(buf)


class SystemClient:
    def __init__(self, settings: Settings) -> None:
        self._settings = settings
        self.app = settings.app
        self.socket_path = settings.socket_path

    def ping(self) -> bool:
        """只检查 listener 是否可连，不发帧。"""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self._settings.connect_timeout_s)
        try:
            sock.connect(self.socket_path)
            return True
        except OSError:
            return False
        finally:
            sock.close()

    def sign(
        self,
        params: dict[str, str] | None = None,
        *,
        session: str | None = None,
        deadline_ms: int | None = None,
    ) -> dict[str, Any]:
        """发一次 op=sign，返回原始响应字典（ok=false 也返回）。"""
        timeout_ms = deadline_ms or self._settings.deadline_ms
        deadline = time.monotonic() + timeout_ms / 1000.0
        request_id = f"web-{uuid.uuid4().hex}"

        attempt = 0
        while True:
            response = self._exchange(
                request_id=f"{request_id}-{attempt}",
                params=params or {},
                session=session,
                deadline=deadline,
            )
            if response.get("ok"):
                return response

            error = response.get("error") or {}
            code = error.get("code", "")
            if code in RETRY_CODES and time.monotonic() < deadline:
                time.sleep(0.05)
                attempt += 1
                continue
            if (
                code in SESSION_LOST_CODES
                and session is not None
                and time.monotonic() < deadline
            ):
                # 实例被回收：下一次请求不带 session，让 System 重新分配。
                session = None
                attempt += 1
                continue
            return response

    def _exchange(
        self,
        *,
        request_id: str,
        params: dict[str, str],
        session: str | None,
        deadline: float,
    ) -> dict[str, Any]:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise SystemClientError("请求超时", code="DEADLINE_EXCEEDED")

        body: dict[str, Any] = {
            "version": 1,
            "id": request_id,
            "app": self.app,
            "op": "sign",
            "params": params,
        }
        if session:
            body["session"] = {"id": session}
        payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
        if len(payload) > MAX_FRAME_BYTES:
            raise SystemClientError("请求帧过大", code="BAD_REQUEST")

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(remaining)
        try:
            sock.connect(self.socket_path)
            sock.sendall(struct.pack(">I", len(payload)) + payload)
            length = struct.unpack(">I", _recv_exact(sock, 4))[0]
            if length > MAX_FRAME_BYTES:
                raise SystemClientError("响应帧过大", code="BAD_FRAME")
            data = _recv_exact(sock, length)
        except OSError as exc:
            raise SystemClientError(
                f"无法连接 System ({self.socket_path}): {exc}",
                code="SYSTEM_UNAVAILABLE",
            ) from exc
        finally:
            sock.close()

        try:
            response = json.loads(data)
        except json.JSONDecodeError as exc:
            raise SystemClientError("System 返回了非法 JSON", code="BAD_JSON") from exc
        if not isinstance(response, dict):
            raise SystemClientError("System 返回的不是 JSON 对象", code="BAD_FRAME")
        return response
