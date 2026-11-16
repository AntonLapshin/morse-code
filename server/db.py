"""SQLite storage for the relay.

One connection is enough: the relay is a single uvicorn worker serving two
ESP32s. WAL keeps reads from blocking the long-poll insert path, and writes are
serialized by an ``asyncio.Lock`` so duplicate detection and insertion stay
atomic within the process.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
from pathlib import Path

import aiosqlite

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  last_seen     REAL NOT NULL DEFAULT 0,
  last_read_seq INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS messages (
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,
  client_msg_id TEXT NOT NULL,
  from_device   TEXT NOT NULL REFERENCES devices(id),
  to_device     TEXT NOT NULL DEFAULT 'family',
  text          TEXT NOT NULL,
  created_at    REAL NOT NULL,
  UNIQUE(from_device, client_msg_id)
);

CREATE INDEX IF NOT EXISTS idx_messages_seq ON messages(seq);
CREATE INDEX IF NOT EXISTS idx_messages_to  ON messages(to_device, seq);
"""

MESSAGE_COLUMNS = "seq, client_msg_id, from_device, to_device, text, created_at"


class DatabaseError(RuntimeError):
    pass


@dataclass(frozen=True)
class DeviceRow:
    id: str
    name: str
    last_seen: float
    last_read_seq: int


class Database:
    def __init__(self, path: Path, retention: int = 1000) -> None:
        self.path = Path(path)
        self.retention = retention
        self._conn: aiosqlite.Connection | None = None
        self._write_lock = asyncio.Lock()

    @property
    def conn(self) -> aiosqlite.Connection:
        if self._conn is None:
            raise DatabaseError("database is not connected")
        return self._conn

    async def connect(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = await aiosqlite.connect(self.path)
        self._conn.row_factory = aiosqlite.Row
        await self._conn.execute("PRAGMA journal_mode = WAL")
        await self._conn.execute("PRAGMA busy_timeout = 5000")
        await self._conn.execute("PRAGMA foreign_keys = ON")
        await self._conn.executescript(SCHEMA)
        await self._conn.commit()

    async def close(self) -> None:
        if self._conn is not None:
            await self._conn.close()
            self._conn = None

    # -- devices ---------------------------------------------------------

    async def sync_devices(self, devices: list[tuple[str, str]]) -> None:
        """Insert configured devices, refreshing names but keeping read state."""
        async with self._write_lock:
            await self.conn.executemany(
                """
                INSERT INTO devices (id, name) VALUES (?, ?)
                ON CONFLICT(id) DO UPDATE SET name = excluded.name
                """,
                devices,
            )
            await self.conn.commit()

    async def touch(self, device_id: str, now: float) -> None:
        await self.conn.execute(
            "UPDATE devices SET last_seen = ? WHERE id = ?", (now, device_id)
        )
        await self.conn.commit()

    async def device_states(self) -> list[DeviceRow]:
        cursor = await self.conn.execute(
            "SELECT id, name, last_seen, last_read_seq FROM devices ORDER BY id"
        )
        rows = await cursor.fetchall()
        return [
            DeviceRow(
                id=row["id"],
                name=row["name"],
                last_seen=float(row["last_seen"]),
                last_read_seq=int(row["last_read_seq"]),
            )
            for row in rows
        ]

    async def unread(self, device_id: str) -> int:
        cursor = await self.conn.execute(
            """
            SELECT COUNT(*) AS n
            FROM messages m
            JOIN devices d ON d.id = ?
            WHERE m.seq > d.last_read_seq
              AND m.from_device != d.id
              AND (m.to_device = 'family' OR m.to_device = d.id)
            """,
            (device_id,),
        )
        row = await cursor.fetchone()
        return int(row["n"]) if row else 0

    async def ack(self, device_id: str, upto_seq: int) -> int:
        async with self._write_lock:
            await self.conn.execute(
                """
                UPDATE devices
                SET last_read_seq = MAX(last_read_seq, ?)
                WHERE id = ?
                """,
                (upto_seq, device_id),
            )
            await self.conn.commit()
        return await self.unread(device_id)

    # -- messages --------------------------------------------------------

    async def latest_seq(self) -> int:
        cursor = await self.conn.execute("SELECT COALESCE(MAX(seq), 0) AS seq FROM messages")
        row = await cursor.fetchone()
        return int(row["seq"]) if row else 0

    async def insert_message(
        self,
        *,
        from_device: str,
        client_msg_id: str,
        to_device: str,
        text: str,
        created_at: float,
    ) -> tuple[dict, bool]:
        """Insert a message, or return the stored one on a duplicate.

        Returns ``(row, duplicate)``.
        """
        async with self._write_lock:
            cursor = await self.conn.execute(
                f"SELECT {MESSAGE_COLUMNS} FROM messages "
                "WHERE from_device = ? AND client_msg_id = ?",
                (from_device, client_msg_id),
            )
            existing = await cursor.fetchone()
            if existing is not None:
                return dict(existing), True

            cursor = await self.conn.execute(
                """
                INSERT INTO messages (client_msg_id, from_device, to_device, text, created_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (client_msg_id, from_device, to_device, text, created_at),
            )
            seq = int(cursor.lastrowid or 0)
            await self.conn.commit()

            cursor = await self.conn.execute(
                f"SELECT {MESSAGE_COLUMNS} FROM messages WHERE seq = ?", (seq,)
            )
            row = await cursor.fetchone()
            if row is None:
                raise DatabaseError("inserted message disappeared")
            return dict(row), False

    async def fetch_after(
        self, device_id: str, since: int, limit: int
    ) -> tuple[list[dict], bool]:
        """Return up to ``limit`` messages addressed to ``device_id``, oldest first.

        When more than ``limit`` messages are waiting, the newest are kept and
        ``truncated`` is true; the caller advances past the gap.
        """
        cursor = await self.conn.execute(
            f"""
            SELECT {MESSAGE_COLUMNS}
            FROM messages
            WHERE seq > ?
              AND from_device != ?
              AND (to_device = 'family' OR to_device = ?)
            ORDER BY seq DESC
            LIMIT ?
            """,
            (since, device_id, device_id, limit + 1),
        )
        rows = await cursor.fetchall()
        truncated = len(rows) > limit
        if truncated:
            rows = rows[:limit]
        return [dict(row) for row in reversed(rows)], truncated

    async def prune(self) -> int:
        """Keep the newest ``retention`` messages. Returns rows deleted."""
        async with self._write_lock:
            cursor = await self.conn.execute(
                """
                DELETE FROM messages
                WHERE seq NOT IN (
                  SELECT seq FROM messages ORDER BY seq DESC LIMIT ?
                )
                """,
                (self.retention,),
            )
            await self.conn.commit()
            return int(cursor.rowcount or 0)
