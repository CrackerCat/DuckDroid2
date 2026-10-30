"""Web API 配置。

全部来自环境变量，保持这一层足够薄：它只决定连哪个 System socket、
默认 app 是谁、单个请求最多等多久。
"""

from __future__ import annotations

import os
from dataclasses import dataclass


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be an integer, got {raw!r}") from exc


@dataclass(frozen=True)
class Settings:
    app: str
    socket_path: str
    # 一次 HTTP 请求内部允许重试 OVERLOADED / INITIALIZING 的总时长。
    deadline_ms: int = 120_000
    connect_timeout_s: float = 5.0

    @classmethod
    def from_env(cls) -> "Settings":
        app = os.getenv("DUCKEMU_APP", "jdgs16")
        socket_dir = os.getenv("DUCKEMU_SOCKET_DIR", "/tmp/duckemu-jdgs")
        # 显式 DUCKEMU_SOCKET 优先，便于把 Web API 指到任意 app 的 listener。
        socket_path = os.getenv("DUCKEMU_SOCKET") or os.path.join(
            socket_dir, f"{app}.sock"
        )
        return cls(
            app=app,
            socket_path=socket_path,
            deadline_ms=_env_int("DUCKEMU_DEADLINE_MS", 120_000),
            connect_timeout_s=float(os.getenv("DUCKEMU_CONNECT_TIMEOUT_S", "5")),
        )


settings = Settings.from_env()
