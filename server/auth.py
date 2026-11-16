"""Bearer-token authentication.

The token decides who the sender is; the request body never supplies ``from``.
"""

from __future__ import annotations

import time

from fastapi import Depends, HTTPException, Request

from config import Config, Device


def _bearer_token(request: Request) -> str:
    header = request.headers.get("authorization", "")
    scheme, _, value = header.partition(" ")
    if scheme.lower() != "bearer" or not value.strip():
        raise HTTPException(
            status_code=401,
            detail={
                "error": "missing_token",
                "detail": "Authorization: Bearer <token> required",
            },
            headers={"WWW-Authenticate": "Bearer"},
        )
    return value.strip()


async def current_device(request: Request) -> Device:
    config: Config = request.app.state.config
    token = _bearer_token(request)
    device = config.device_by_token(token)
    if device is None:
        raise HTTPException(
            status_code=401,
            detail={"error": "invalid_token", "detail": "unknown bearer token"},
            headers={"WWW-Authenticate": "Bearer"},
        )

    # Every authenticated request proves the device is alive. Long-polls also
    # touch while waiting, so presence does not expire mid-request.
    await request.app.state.db.touch(device.id, time.time())
    return device


CurrentDevice = Depends(current_device)
