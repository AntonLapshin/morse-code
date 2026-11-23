// SQLite persistence for the MorseRelay Durable Object, via
// ctx.storage.sql (exec with parameter binding — exec serves SELECTs too;
// there is no .query() in workerd SqlStorage).
//
// Schema: same table/column names as the deleted FastAPI relay's
// server/db.py, so the 0.2.0 port stayed reviewable. One addition:
// a `meta` table holding the schema version (the FastAPI schema had no
// version row; the DO needs one to tell a fresh DB from a migrated one).
//
// All functions take the `sql` object (ctx.storage.sql) so unit tests can
// substitute a fake implementing the same exec interface.
export const SCHEMA_VERSION = "1";

export const SCHEMA = `
CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

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
CREATE INDEX IF NOT EXISTS idx_messages_to ON messages(to_device, seq);
`;

const MESSAGE_COLUMNS = "seq, client_msg_id, from_device, to_device, text, created_at";

function rows(cursor) {
  if (!cursor) return [];
  if (typeof cursor.toArray === "function") return cursor.toArray();
  if (Array.isArray(cursor)) return cursor;
  return [...cursor];
}

export function ensureSchema(sql) {
  sql.exec(SCHEMA);
  sql.exec("INSERT OR IGNORE INTO meta (key, value) VALUES ('schema_version', ?)", SCHEMA_VERSION);
}

/** Insert configured devices, refreshing names but keeping read state. */
export function syncDevices(sql, devices) {
  for (const [id, name] of devices) {
    sql.exec(
      "INSERT INTO devices (id, name) VALUES (?, ?) ON CONFLICT(id) DO UPDATE SET name = excluded.name",
      id,
      name,
    );
  }
}

export function touch(sql, deviceId, now) {
  sql.exec("UPDATE devices SET last_seen = ? WHERE id = ?", now, deviceId);
}

export function deviceStates(sql) {
  return rows(
    sql.exec("SELECT id, name, last_seen, last_read_seq FROM devices ORDER BY id"),
  ).map((row) => ({
    id: row.id,
    name: row.name,
    last_seen: Number(row.last_seen),
    last_read_seq: Number(row.last_read_seq),
  }));
}

export function unread(sql, deviceId) {
  const found = rows(
    sql.exec(
      "SELECT COUNT(*) AS n FROM messages m JOIN devices d ON d.id = ? WHERE m.seq > d.last_read_seq AND m.from_device != d.id AND (m.to_device = 'family' OR m.to_device = d.id)",
      deviceId,
    ),
  );
  return found.length > 0 ? Number(found[0].n) : 0;
}

/** last_read_seq only ever moves forward; returns the new unread count. */
export function ack(sql, deviceId, uptoSeq) {
  sql.exec(
    "UPDATE devices SET last_read_seq = MAX(last_read_seq, ?) WHERE id = ?",
    uptoSeq,
    deviceId,
  );
  return unread(sql, deviceId);
}

export function latestSeq(sql) {
  const found = rows(sql.exec("SELECT COALESCE(MAX(seq), 0) AS seq FROM messages"));
  return found.length > 0 ? Number(found[0].seq) : 0;
}

/** Find a message by the sender's client id, or null. Used by the
 *  duplicate check in handleSend so a re-send can be answered without
 *  consuming rate-limit budget (see insertMessage). */
export function findMessage(sql, { fromDevice, clientMsgId }) {
  const found = rows(
    sql.exec(
      `SELECT ${MESSAGE_COLUMNS} FROM messages WHERE from_device = ? AND client_msg_id = ?`,
      fromDevice,
      clientMsgId,
    ),
  );
  return found.length > 0 ? found[0] : null;
}

/** Insert a message, or return the stored one on a duplicate.
 *  Returns { row, duplicate }. */
export function insertMessage(sql, { fromDevice, clientMsgId, toDevice, text, createdAt }) {
  const existing = findMessage(sql, { fromDevice, clientMsgId });
  if (existing) return { row: existing, duplicate: true };
  sql.exec(
    "INSERT INTO messages (client_msg_id, from_device, to_device, text, created_at) VALUES (?, ?, ?, ?, ?)",
    clientMsgId,
    fromDevice,
    toDevice,
    text,
    createdAt,
  );
  const inserted = findMessage(sql, { fromDevice, clientMsgId });
  if (!inserted) throw new Error("inserted message disappeared");
  return { row: inserted, duplicate: false };
}

/** Up to `limit` messages addressed to `deviceId`, oldest first.
 *  When more than `limit` are waiting, the newest are kept and
 *  `truncated` is true; the caller advances past the gap. */
export function fetchAfter(sql, deviceId, since, limit) {
  const found = rows(
    sql.exec(
      `SELECT ${MESSAGE_COLUMNS} FROM messages WHERE seq > ? AND from_device != ? AND (to_device = 'family' OR to_device = ?) ORDER BY seq DESC LIMIT ?`,
      since,
      deviceId,
      deviceId,
      limit + 1,
    ),
  );
  const truncated = found.length > limit;
  const kept = truncated ? found.slice(0, limit) : found;
  return { rows: [...kept].reverse(), truncated };
}

/** Keep the newest `retention` messages. */
export function prune(sql, retention) {
  sql.exec(
    "DELETE FROM messages WHERE seq NOT IN (SELECT seq FROM messages ORDER BY seq DESC LIMIT ?)",
    retention,
  );
}
