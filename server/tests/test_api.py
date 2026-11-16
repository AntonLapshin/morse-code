"""API tests: send, long-poll, duplicate handling, ack, and auth."""

from __future__ import annotations

import asyncio

import pytest
from httpx import ASGITransport, AsyncClient

from app import create_app
from config import Config, Device, NtfySettings, ServerSettings

DAD_TOKEN = "d" * 24
EMMA_TOKEN = "e" * 24
DAD = {"Authorization": f"Bearer {DAD_TOKEN}"}
EMMA = {"Authorization": f"Bearer {EMMA_TOKEN}"}


@pytest.fixture
def config(tmp_path) -> Config:
    return Config(
        server=ServerSettings(db=str(tmp_path / "morse.db"), retention=1000),
        devices=(
            Device(id="dad", name="DAD", token=DAD_TOKEN),
            Device(id="emma", name="EMMA", token=EMMA_TOKEN),
        ),
        ntfy=NtfySettings(),
        path=tmp_path / "config.toml",
    )


@pytest.fixture
async def client(config: Config):
    app = create_app(config)
    async with app.router.lifespan_context(app):
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://relay") as http:
            yield http


async def send(client: AsyncClient, text: str, *, msg_id: str = "dad-1-1", to: str = "family"):
    return await client.post(
        "/api/v1/messages",
        json={"client_msg_id": msg_id, "to": to, "text": text},
        headers=DAD,
    )


async def test_health_needs_no_auth(client: AsyncClient) -> None:
    response = await client.get("/health")
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["latest_seq"] == 0


async def test_requires_bearer_token(client: AsyncClient) -> None:
    response = await client.post(
        "/api/v1/messages", json={"client_msg_id": "x-1", "text": "HI"}
    )
    assert response.status_code == 401
    assert response.json()["error"] == "missing_token"

    bad = await client.get(
        "/api/v1/state", headers={"Authorization": "Bearer nope"}
    )
    assert bad.status_code == 401
    assert bad.json()["error"] == "invalid_token"


async def test_send_and_poll(client: AsyncClient) -> None:
    sent = await send(client, "HELLO")
    assert sent.status_code == 201
    body = sent.json()
    assert body["from"] == "dad"
    assert body["from_name"] == "DAD"
    assert body["duplicate"] is False
    seq = body["seq"]

    polled = await client.get(
        "/api/v1/messages", params={"since": 0, "wait": 0}, headers=EMMA
    )
    assert polled.status_code == 200
    data = polled.json()
    assert [m["seq"] for m in data["messages"]] == [seq]
    assert data["messages"][0]["text"] == "HELLO"
    assert data["unread"] == 1
    assert data["latest_seq"] == seq
    assert data["peer_online"] is True


async def test_sender_does_not_receive_own_message(client: AsyncClient) -> None:
    await send(client, "MINE")
    polled = await client.get(
        "/api/v1/messages", params={"since": 0}, headers=DAD
    )
    assert polled.json()["messages"] == []


async def test_duplicate_client_msg_id_is_idempotent(client: AsyncClient) -> None:
    first = await send(client, "ONCE", msg_id="dad-1-7")
    second = await send(client, "ONCE", msg_id="dad-1-7")

    assert first.status_code == 201
    assert second.status_code == 200
    assert second.json()["duplicate"] is True
    assert second.json()["seq"] == first.json()["seq"]


async def test_long_poll_wakes_on_insert(client: AsyncClient) -> None:
    poll = asyncio.create_task(
        client.get("/api/v1/messages", params={"since": 0, "wait": 5}, headers=EMMA)
    )
    await asyncio.sleep(0.2)
    assert not poll.done()

    await send(client, "PING")

    response = await asyncio.wait_for(poll, timeout=3)
    assert response.status_code == 200
    assert response.json()["messages"][0]["text"] == "PING"


async def test_long_poll_times_out_empty(client: AsyncClient) -> None:
    response = await client.get(
        "/api/v1/messages", params={"since": 0, "wait": 0.2}, headers=EMMA
    )
    assert response.status_code == 200
    body = response.json()
    assert body["messages"] == []
    assert body["truncated"] is False


async def test_since_skips_old_messages(client: AsyncClient) -> None:
    first = (await send(client, "ONE", msg_id="dad-1-1")).json()
    second = (await send(client, "TWO", msg_id="dad-1-2")).json()

    response = await client.get(
        "/api/v1/messages",
        params={"since": first["seq"]},
        headers=EMMA,
    )
    assert [m["seq"] for m in response.json()["messages"]] == [second["seq"]]


async def test_truncation_returns_newest(client: AsyncClient) -> None:
    seqs = []
    for index in range(5):
        seqs.append(
            (await send(client, f"M{index}", msg_id=f"dad-1-{index}")).json()["seq"]
        )

    response = await client.get(
        "/api/v1/messages", params={"since": 0, "limit": 3}, headers=EMMA
    )
    body = response.json()
    assert body["truncated"] is True
    assert [m["seq"] for m in body["messages"]] == seqs[-3:]
    assert body["latest_seq"] == seqs[-1]


async def test_ack_clears_unread(client: AsyncClient) -> None:
    seq = (await send(client, "READ ME")).json()["seq"]

    ack = await client.post("/api/v1/ack", json={"upto_seq": seq}, headers=EMMA)
    assert ack.status_code == 200
    assert ack.json()["unread"] == 0

    # Older acks must not move last_read_seq backwards.
    older = await client.post("/api/v1/ack", json={"upto_seq": 0}, headers=EMMA)
    assert older.json()["unread"] == 0


async def test_state_lists_devices_and_presence(client: AsyncClient) -> None:
    await client.get("/api/v1/messages", params={"since": 0}, headers=EMMA)
    await send(client, "HI")

    response = await client.get("/api/v1/state", headers=DAD)
    assert response.status_code == 200
    body = response.json()
    by_id = {device["id"]: device for device in body["devices"]}
    assert set(by_id) == {"dad", "emma"}
    assert by_id["dad"]["online"] is True
    assert by_id["emma"]["online"] is True
    assert body["latest_seq"] == 1


async def test_validation_and_recipients(client: AsyncClient) -> None:
    empty = await client.post(
        "/api/v1/messages",
        json={"client_msg_id": "dad-1-9", "text": "   "},
        headers=DAD,
    )
    assert empty.status_code == 400
    assert empty.json()["error"] == "validation_error"

    unknown = await send(client, "HI", to="nobody")
    assert unknown.status_code == 400
    assert unknown.json()["error"] == "unknown_recipient"

    to_self = await send(client, "HI", to="dad")
    assert to_self.status_code == 400
    assert to_self.json()["error"] == "self_recipient"

    oversized = await send(client, "X" * 513)
    assert oversized.status_code == 400


async def test_direct_message_only_reaches_recipient(client: AsyncClient) -> None:
    await send(client, "PSST", msg_id="dad-1-dm", to="emma")

    emma = await client.get("/api/v1/messages", params={"since": 0}, headers=EMMA)
    assert emma.json()["messages"][0]["to"] == "emma"

    await client.post(
        "/api/v1/messages",
        json={"client_msg_id": "e-1", "to": "family", "text": "HI"},
        headers=EMMA,
    )
    dad = await client.get("/api/v1/messages", params={"since": 0}, headers=DAD)
    assert [m["text"] for m in dad.json()["messages"]] == ["HI"]
