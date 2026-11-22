// Fake ctx.storage.sql for Durable Object unit tests: implements the
// exact statements store.js issues, over in-memory JS tables. NOT a SQL
// engine — it pattern-matches the canonical statements only. The
// integration test (tests/integration.test.mjs, workers pool) runs the
// same code against real workerd SQLite.
export class FakeSql {
  constructor() {
    this.devices = new Map(); // id -> { id, name, last_seen, last_read_seq }
    this.messages = []; // { seq, client_msg_id, from_device, to_device, text, created_at }
    this.nextSeq = 1;
    // Counts fetchAfter queries (the SELECT ... WHERE seq > ? below), so
    // tests can assert a long-poll that times out does not re-query.
    this.fetchAfterCalls = 0;
  }

  exec(sql, ...params) {
    const s = sql.replace(/\s+/g, " ").trim();
    if (s.startsWith("CREATE TABLE") || s.startsWith("INSERT OR IGNORE INTO meta")) {
      return { toArray: () => [] };
    }
    if (s.startsWith("INSERT INTO devices")) {
      const [id, name] = params;
      const prev = this.devices.get(id);
      this.devices.set(id, {
        id,
        name,
        last_seen: prev ? prev.last_seen : 0,
        last_read_seq: prev ? prev.last_read_seq : 0,
      });
      return;
    }
    if (s.startsWith("UPDATE devices SET last_seen")) {
      const [now, id] = params;
      const d = this.devices.get(id);
      if (d) d.last_seen = now;
      return;
    }
    if (s === "SELECT id, name, last_seen, last_read_seq FROM devices ORDER BY id") {
      const out = [...this.devices.values()]
        .sort((a, b) => (a.id < b.id ? -1 : 1))
        .map((d) => ({ ...d }));
      return { toArray: () => out };
    }
    if (s.startsWith("SELECT COUNT(*) AS n FROM messages")) {
      const [deviceId] = params;
      const d = this.devices.get(deviceId);
      const lastRead = d ? d.last_read_seq : 0;
      const n = this.messages.filter(
        (m) =>
          m.seq > lastRead &&
          m.from_device !== deviceId &&
          (m.to_device === "family" || m.to_device === deviceId),
      ).length;
      return { toArray: () => [{ n }] };
    }
    if (s.startsWith("UPDATE devices SET last_read_seq")) {
      const [upto, id] = params;
      const d = this.devices.get(id);
      if (d) d.last_read_seq = Math.max(d.last_read_seq, upto);
      return;
    }
    if (s === "SELECT COALESCE(MAX(seq), 0) AS seq FROM messages") {
      const seq = this.messages.length > 0 ? Math.max(...this.messages.map((m) => m.seq)) : 0;
      return { toArray: () => [{ seq }] };
    }
    if (
      s.startsWith("SELECT seq, client_msg_id, from_device, to_device, text, created_at FROM messages WHERE from_device = ? AND client_msg_id = ?")
    ) {
      const [from, cid] = params;
      const out = this.messages
        .filter((m) => m.from_device === from && m.client_msg_id === cid)
        .map((m) => ({ ...m }));
      return { toArray: () => out };
    }
    if (s.startsWith("INSERT INTO messages")) {
      const [clientMsgId, fromDevice, toDevice, text, createdAt] = params;
      this.messages.push({
        seq: this.nextSeq++,
        client_msg_id: clientMsgId,
        from_device: fromDevice,
        to_device: toDevice,
        text,
        created_at: createdAt,
      });
      return;
    }
    if (
      s.startsWith("SELECT seq, client_msg_id, from_device, to_device, text, created_at FROM messages WHERE seq > ?")
    ) {
      this.fetchAfterCalls++;
      const [since, deviceId, , limitPlus1] = params;
      const out = this.messages
        .filter(
          (m) =>
            m.seq > since &&
            m.from_device !== deviceId &&
            (m.to_device === "family" || m.to_device === deviceId),
        )
        .sort((a, b) => b.seq - a.seq)
        .slice(0, limitPlus1)
        .map((m) => ({ ...m }));
      return { toArray: () => out };
    }
    if (s.startsWith("DELETE FROM messages WHERE seq NOT IN")) {
      const [retention] = params;
      const keep = new Set(
        [...this.messages]
          .sort((a, b) => b.seq - a.seq)
          .slice(0, retention)
          .map((m) => m.seq),
      );
      this.messages = this.messages.filter((m) => keep.has(m.seq));
      return;
    }
    throw new Error(`FakeSql.exec: unsupported statement: ${s.slice(0, 80)}`);
  }
}

/** Fake DurableObject ctx: { storage: { sql, setAlarm } }. */
export function fakeCtx() {
  const sql = new FakeSql();
  const alarms = [];
  return {
    sql,
    alarms,
    ctx: {
      storage: {
        sql,
        setAlarm: (ms) => {
          alarms.push(ms);
        },
      },
    },
  };
}
