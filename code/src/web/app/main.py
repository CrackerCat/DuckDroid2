"""DuckDroid2 Web API —— 对 System（UDS 网关）的最小 FastAPI 封装。

分层：

    HTTP 客户端 -> FastAPI (本文件) -> UDS 帧协议 -> System -> Worker -> Instance

这是一个占位/案例：只演示如何把 Java/HTTP 世界的 JSON 转成 System 的
op=sign 请求，以及如何把结果读回来。生产所需的鉴权、限流、连接池、
任务队列、面板都不在这里。
"""

from __future__ import annotations

import json
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from .config import settings
from .system_client import SystemClient, SystemClientError

app = FastAPI(
    title="DuckDroid2 Web API",
    description="把 DuckDroid2 System 的 UDS 帧协议包成 HTTP JSON 的最小案例。",
    version="0.1.0",
)

client = SystemClient(settings)

# 到达 guest 之前就失败的请求 -> 400；资源/排队问题 -> 503；
# 会话丢失需要重新分配 -> 409；guest 自身失败 -> 502。
_CLIENT_ERRORS = {
    "BAD_FRAME",
    "BAD_JSON",
    "BAD_VERSION",
    "BAD_REQUEST",
    "UNKNOWN_OP",
    "APP_MISMATCH",
}
_UNAVAILABLE_ERRORS = {
    "OVERLOADED",
    "INITIALIZING",
    "QUEUE_TIMEOUT",
    "DEADLINE_EXCEEDED",
    "CALL_TIMEOUT",
}
_SESSION_ERRORS = {"SESSION_NOT_FOUND", "SESSION_GONE"}


class GenericSignRequest(BaseModel):
    """直接转给 System 的通用请求；params 由目标自己解释。"""

    params: dict[str, str] = Field(default_factory=dict)
    session: str | None = None
    deadline_ms: int | None = Field(default=None, ge=1)


class JdSignRequest(BaseModel):
    """京东 16.1.50 `Bridge.main(101)` 的参数。

    全部可省略：省略的键不会发给 System，插件会使用内置的 coral 样本。
    """

    request: str | None = None
    device_payload: str | None = None
    dfp_eid: str | None = None
    pay_version: str | None = None
    openudid: str | None = None
    sdk_config: str | None = None
    session: str | None = None
    deadline_ms: int | None = Field(default=None, ge=1)

    def to_params(self) -> dict[str, str]:
        names = (
            "request",
            "device_payload",
            "dfp_eid",
            "pay_version",
            "openudid",
            "sdk_config",
        )
        return {
            name: value
            for name in names
            if (value := getattr(self, name)) is not None
        }


def _status_for(response: dict[str, Any]) -> int:
    if response.get("ok"):
        return 200
    code = (response.get("error") or {}).get("code", "")
    if code in _CLIENT_ERRORS:
        return 400
    if code in _UNAVAILABLE_ERRORS:
        return 503
    if code in _SESSION_ERRORS:
        return 409
    return 502


def _call(
    params: dict[str, str],
    *,
    session: str | None,
    deadline_ms: int | None,
) -> dict[str, Any]:
    try:
        return client.sign(params, session=session, deadline_ms=deadline_ms)
    except SystemClientError as exc:
        status = 503 if exc.code == "SYSTEM_UNAVAILABLE" else 502
        raise HTTPException(
            status_code=status,
            detail={"ok": False, "error": {"code": exc.code, "message": str(exc)}},
        ) from exc


def _json(status: int, payload: dict[str, Any]) -> JSONResponse:
    return JSONResponse(payload, status_code=status)


@app.get("/", include_in_schema=False)
def index() -> dict[str, Any]:
    return {
        "service": "duckdroid2-webapi",
        "app": settings.app,
        "socket": settings.socket_path,
        "docs": "/docs",
        "endpoints": ["/health", "/sign", "/jd/sign"],
    }


@app.get("/health")
def health() -> JSONResponse:
    reachable = client.ping()
    payload = {
        "status": "ok" if reachable else "unavailable",
        "app": settings.app,
        "socket": settings.socket_path,
    }
    return _json(200 if reachable else 503, payload)


@app.post("/sign")
def sign(request: GenericSignRequest) -> JSONResponse:
    """通用转发：响应原样返回 System 的结果。"""
    response = _call(
        request.params,
        session=request.session,
        deadline_ms=request.deadline_ms,
    )
    return _json(_status_for(response), response)


@app.post("/jd/sign")
def jd_sign(request: JdSignRequest) -> JSONResponse:
    """调用京东签名器（app id: jdgs16，`Bridge.main(101)`）。

    成功时 `result.value` 是 b1–b7 的 JSON 字符串；这里额外解析成 `headers`，
    同时保留原始 `signature`。
    """
    response = _call(
        request.to_params(),
        session=request.session,
        deadline_ms=request.deadline_ms,
    )
    if not response.get("ok"):
        return _json(_status_for(response), response)

    signature = (response.get("result") or {}).get("value", "")
    headers: dict[str, str] | None = None
    if isinstance(signature, str):
        try:
            parsed = json.loads(signature)
        except json.JSONDecodeError:
            parsed = None
        if isinstance(parsed, dict):
            headers = parsed

    return _json(
        200,
        {
            "ok": True,
            "app": response.get("app", settings.app),
            "instance": response.get("instance", ""),
            # System 的 session id 就是 instance uuid；带回去即可固定到同一实例。
            "session": response.get("instance", ""),
            "signature": signature,
            "headers": headers,
            "meta": response.get("meta", {}),
        },
    )


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("app.main:app", host="127.0.0.1", port=8000)
