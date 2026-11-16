"""FastAPI relay for the two Morse-code ESP32s.

Run a single uvicorn worker: long-poll waiters live in this process.

    uv run uvicorn app:app --host 0.0.0.0 --port 8180
"""

from __future__ import annotations

import asyncio
import logging
import time
from contextlib import asynccontextmanager
from typing import Annotated

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from auth import current_device
from config import Config, Device, load_config
from db import Database
from models import FAMILY, AckRequest, SendMessageRequest

VERSION = "0.1.0"
ONLINE_WINDOW_S = 45.0
MAX_BODY_BYTES = 8 * 1024
SEND_RATE_LIMIT = 20
SEND_RATE_WINDOW_S = 60.0
PRUNE_INTERVAL_S = 3600.0
DISCONNECT_CHECK_S = 5.0

log = logging.getLogger("morse.relay")


class MessageNotifier:
    """Wakes long-polls without a lost-wakeup window.

    Waiters capture ``version``, query the DB, and only then block. An insert
    that lands between the query and the block bumps the version first, so the
    waiter notices immediately instead of sleeping until the timeout.
    """

    def __init__(self) -> None:
        self._condition = asyncio.Condition()
        self._version = 0

    @property
    def version(self) -> int:
        return self._version

    async def notify(self) -> None:
        async with self._condition:
            self._version += 1
            self._condition.notify_all()

    async def wait_for_change(self, seen: int, timeout: float) -> bool:
        async with self._condition:
            if self._version != seen:
                return True
            try:
                await asyncio.wait_for(
                    self._condition.wait_for(lambda: self._version != seen),
                    timeout=timeout,
                )
            except asyncio.TimeoutError:
                return False
            return True


class RateLimiter:
    """Simple in-memory sliding window, per device."""

    def __init__(self, limit: int = SEND_RATE_LIMIT, window: float = SEND_RATE_WINDOW_S):
        self.limit = limit
        self.window = window
        self._events: dict[str, list[float]] = {}

    def allow(self, key: str, now: float) -> bool:
        events = self._events.setdefault(key, [])
        cutoff = now - self.window
        while events and events[0] <= cutoff:
            events.pop(0)
        if len(events) >= self.limit:
            return False
        events.append(now)
        return True


def _validation_detail(exc: RequestValidationError) -> str:
    parts = []
    for error in exc.errors():
        location = ".".join(str(part) for part in error.get("loc", ()))
        parts.append(f"{location}: {error.get('msg', 'invalid')}")
    return "; ".join(parts) or "invalid request"


async def _prune_loop(db: Database) -> None:
    while True:
        await asyncio.sleep(PRUNE_INTERVAL_S)
        try:
            deleted = await db.prune()
            if deleted:
                log.info("retention: deleted %d message(s)", deleted)
        except Exception:  # keep the relay alive even if one prune fails
            log.exception("retention job failed")


def _message_view(row: dict, names: dict[str, str], now: float) -> dict:
    return {
        "seq": int(row["seq"]),
        "from": row["from_device"],
        "from_name": names.get(row["from_device"], row["from_device"].upper()),
        "to": row["to_device"],
        "text": row["text"],
        "ts": int(row["created_at"]),
        "age_s": max(0, int(now - float(row["created_at"]))),
    }


def create_app(config: Config | None = None) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        cfg = config or load_config()
        app.state.config = cfg
        app.state.names = {device.id: device.display_name for device in cfg.devices}
        app.state.notifier = MessageNotifier()
        app.state.limiter = RateLimiter()

        db = Database(cfg.db_path(), cfg.server.retention)
        await db.connect()
        await db.sync_devices([(d.id, d.display_name) for d in cfg.devices])
        app.state.db = db
        log.info(
            "relay ready: %d device(s), db=%s, retention=%d",
            len(cfg.devices),
            cfg.db_path(),
            cfg.server.retention,
        )

        prune_task = asyncio.create_task(_prune_loop(db))
        try:
            yield
        finally:
            prune_task.cancel()
            try:
                await prune_task
            except asyncio.CancelledError:
                pass
            await db.close()

    app = FastAPI(title="morse-relay", version=VERSION, lifespan=lifespan)

    @app.middleware("http")
    async def limit_body_size(request: Request, call_next):
        if request.method in ("POST", "PUT", "PATCH"):
            raw_length = request.headers.get("content-length")
            if raw_length is not None and raw_length.isdigit():
                if int(raw_length) > MAX_BODY_BYTES:
                    return JSONResponse(
                        status_code=413,
                        content={
                            "error": "payload_too_large",
                            "detail": f"request body must be at most {MAX_BODY_BYTES} bytes",
                        },
                    )
        return await call_next(request)

    @app.exception_handler(StarletteHTTPException)
    async def http_error(_: Request, exc: StarletteHTTPException) -> JSONResponse:
        if isinstance(exc.detail, dict) and "error" in exc.detail:
            content = exc.detail
        else:
            content = {"error": "http_error", "detail": str(exc.detail)}
        return JSONResponse(
            status_code=exc.status_code, content=content, headers=exc.headers
        )

    @app.exception_handler(RequestValidationError)
    async def validation_error(
        _: Request, exc: RequestValidationError
    ) -> JSONResponse:
        return JSONResponse(
            status_code=400,
            content={"error": "validation_error", "detail": _validation_detail(exc)},
        )

    @app.get("/health")
    async def health(request: Request) -> dict:
        db: Database = request.app.state.db
        return {
            "status": "ok",
            "version": VERSION,
            "latest_seq": await db.latest_seq(),
            "server_time": int(time.time()),
        }

    @app.post("/api/v1/messages")
    async def send_message(
        payload: SendMessageRequest,
        request: Request,
        device: Device = Depends(current_device),
    ):
        db: Database = request.app.state.db
        cfg: Config = request.app.state.config
        limiter: RateLimiter = request.app.state.limiter
        notifier: MessageNotifier = request.app.state.notifier

        now = time.time()
        if not limiter.allow(device.id, now):
            raise HTTPException(
                status_code=429,
                detail={
                    "error": "rate_limited",
                    "detail": f"at most {limiter.limit} messages per minute",
                },
            )

        known = cfg.device_ids()
        if payload.to != FAMILY and payload.to not in known:
            raise HTTPException(
                status_code=400,
                detail={
                    "error": "unknown_recipient",
                    "detail": f"to must be 'family' or one of {sorted(known)}",
                },
            )
        if payload.to == device.id:
            raise HTTPException(
                status_code=400,
                detail={"error": "self_recipient", "detail": "cannot send to yourself"},
            )

        row, duplicate = await db.insert_message(
            from_device=device.id,
            client_msg_id=payload.client_msg_id,
            to_device=payload.to,
            text=payload.text,
            created_at=now,
        )
        if not duplicate:
            await notifier.notify()

        body = {
            "seq": int(row["seq"]),
            "from": row["from_device"],
            "from_name": request.app.state.names.get(device.id, device.display_name),
            "to": row["to_device"],
            "text": row["text"],
            "ts": int(row["created_at"]),
            "duplicate": duplicate,
        }
        return JSONResponse(status_code=200 if duplicate else 201, content=body)

    @app.get("/api/v1/messages")
    async def poll_messages(
        request: Request,
        since: Annotated[int, Query(ge=0)] = 0,
        wait: Annotated[float, Query(ge=0, le=25)] = 0,
        limit: Annotated[int, Query(ge=1, le=50)] = 20,
        device: Device = Depends(current_device),
    ) -> dict:
        db: Database = request.app.state.db
        notifier: MessageNotifier = request.app.state.notifier

        peer_window = float(ONLINE_WINDOW_S)
        deadline = time.monotonic() + wait
        messages: list[dict] = []
        truncated = False

        while True:
            await db.touch(device.id, time.time())
            seen = notifier.version
            rows, truncated = await db.fetch_after(device.id, since, limit)
            if rows or wait <= 0:
                messages = rows
                break

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            if await request.is_disconnected():
                # The device went away; drop the waiter without work.
                return JSONResponse(status_code=499, content={"error": "client_closed"})

            await notifier.wait_for_change(seen, min(remaining, DISCONNECT_CHECK_S))

        await db.touch(device.id, time.time())
        states = await db.device_states()
        now = time.time()
        latest_seq = await db.latest_seq()
        unread = await db.unread(device.id)
        peer_ids = request.app.state.config.peer_ids(exclude=device.id)
        peer_online = any(
            state.id in peer_ids and now - state.last_seen < peer_window
            for state in states
        )

        return {
            "messages": [
                _message_view(row, request.app.state.names, now) for row in messages
            ],
            "latest_seq": latest_seq,
            "truncated": truncated,
            "unread": unread,
            "peer_online": peer_online,
            "server_time": int(now),
        }

    @app.post("/api/v1/ack")
    async def ack(
        payload: AckRequest,
        request: Request,
        device: Device = Depends(current_device),
    ) -> dict:
        db: Database = request.app.state.db
        unread = await db.ack(device.id, payload.upto_seq)
        return {"ok": True, "unread": unread, "upto_seq": payload.upto_seq}

    @app.get("/api/v1/state")
    async def state(request: Request, device: Device = Depends(current_device)) -> dict:
        db: Database = request.app.state.db
        now = time.time()
        rows = await db.device_states()
        return {
            "devices": [
                {
                    "id": row.id,
                    "name": row.name,
                    "online": now - row.last_seen < ONLINE_WINDOW_S,
                    "last_seen_s": max(0, int(now - row.last_seen))
                    if row.last_seen
                    else None,
                }
                for row in rows
            ],
            "unread": await db.unread(device.id),
            "latest_seq": await db.latest_seq(),
            "server_time": int(now),
        }

    return app


app = create_app()
