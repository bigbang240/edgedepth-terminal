// ═══════════════════════════════════════════════════════════════════════════════
// SQLite history of the normalized stream (roadmap Phase 6:
// "ذخیرهٔ تاریخی SQLite از جریان نرمال‌شده").
//
// The recorder already rolls the RAW wire into .edpack replay packs (phase 4);
// this store keeps the NORMALIZED market data queryable: every trade, every
// liquidation, completed 1m candles and a periodic top-of-book poll, in a
// single WAL-mode database queried over REST (/history/...).
//
// Writes are batched (2s / 500 rows) inside transactions - the ingest path
// never blocks on I/O. Rows older than HISTORY_DAYS are pruned hourly and the
// WAL is truncated so the file stays proportional to the retention window.
// bun:sqlite is built into the runtime - zero new dependencies.
// ═══════════════════════════════════════════════════════════════════════════════

import { Database } from "bun:sqlite";
import { mkdirSync } from "node:fs";
import { dirname } from "node:path";

type TradeRow = [number, string, number, number, number, number];
type LiqRow = [number, string, string, number, number, number];
type CandleRow2 = [number, string, number, number, number, number, number];
type PollRow = [number, string, number, number, number, number, number, number];

export interface HistoryQuery {
  symbol?: string;
  from?: number;
  to?: number;
  limit?: number;
}

export class HistoryStore {
  readonly path: string;
  readonly keepDays: number;
  private db: Database;
  private tradesBuf: TradeRow[] = [];
  private liqsBuf: LiqRow[] = [];
  private candlesBuf: CandleRow2[] = [];
  private pollsBuf: PollRow[] = [];
  private lastFlush = 0;

  constructor(path: string, keepDays = 7) {
    this.path = path;
    this.keepDays = keepDays;
    mkdirSync(dirname(path), { recursive: true });
    this.db = new Database(path, { create: true });
    this.db.exec("PRAGMA journal_mode = WAL;");
    this.db.exec("PRAGMA synchronous = NORMAL;");
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS trades (
        ts INTEGER NOT NULL, symbol TEXT NOT NULL,
        price REAL NOT NULL, qty REAL NOT NULL,
        is_buy INTEGER NOT NULL, usd REAL NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_trades_sym_ts ON trades(symbol, ts);
      CREATE TABLE IF NOT EXISTS liquidations (
        ts INTEGER NOT NULL, symbol TEXT NOT NULL, side TEXT NOT NULL,
        price REAL NOT NULL, qty REAL NOT NULL, usd REAL NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_liq_sym_ts ON liquidations(symbol, ts);
      CREATE TABLE IF NOT EXISTS candles_1m (
        ts INTEGER NOT NULL, symbol TEXT NOT NULL,
        o REAL NOT NULL, h REAL NOT NULL, l REAL NOT NULL, c REAL NOT NULL, v REAL NOT NULL,
        PRIMARY KEY (symbol, ts)
      );
      CREATE TABLE IF NOT EXISTS book_polls (
        ts INTEGER NOT NULL, symbol TEXT NOT NULL,
        best_bid REAL, best_ask REAL,
        bid_notional REAL, ask_notional REAL,
        bid_levels INTEGER, ask_levels INTEGER
      );
      CREATE INDEX IF NOT EXISTS idx_polls_sym_ts ON book_polls(symbol, ts);
    `);
    this.prune(); // drop anything older than the retention window on boot
  }

  // ── ingest queue (called from the WS hot path - never blocks) ───────────────

  queueTrade(row: TradeRow) {
    this.tradesBuf.push(row);
    if (this.tradesBuf.length >= 500) this.flush();
  }

  queueLiq(row: LiqRow) {
    this.liqsBuf.push(row);
    if (this.liqsBuf.length >= 500) this.flush();
  }

  queueCandle(row: CandleRow2) {
    this.candlesBuf.push(row);
  }

  queueBookPoll(row: PollRow) {
    this.pollsBuf.push(row);
  }

  /** Flush every buffered batch; safe to call from a timer or on shutdown. */
  flush() {
    const now = Date.now();
    if (
      !this.tradesBuf.length &&
      !this.liqsBuf.length &&
      !this.candlesBuf.length &&
      !this.pollsBuf.length
    ) {
      this.lastFlush = now;
      return;
    }
    try {
      this.db.exec("BEGIN");
      if (this.tradesBuf.length) {
        const st = this.db.query(
          "INSERT INTO trades (ts, symbol, price, qty, is_buy, usd) VALUES (?, ?, ?, ?, ?, ?)",
        );
        for (const r of this.tradesBuf) st.run(...r);
      }
      if (this.liqsBuf.length) {
        const st = this.db.query(
          "INSERT INTO liquidations (ts, symbol, side, price, qty, usd) VALUES (?, ?, ?, ?, ?, ?)",
        );
        for (const r of this.liqsBuf) st.run(...r);
      }
      if (this.candlesBuf.length) {
        const st = this.db.query(
          "INSERT OR REPLACE INTO candles_1m (ts, symbol, o, h, l, c, v) VALUES (?, ?, ?, ?, ?, ?, ?)",
        );
        for (const r of this.candlesBuf) st.run(...r);
      }
      if (this.pollsBuf.length) {
        const st = this.db.query(
          "INSERT INTO book_polls (ts, symbol, best_bid, best_ask, bid_notional, ask_notional, bid_levels, ask_levels) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        );
        for (const r of this.pollsBuf) st.run(...r);
      }
      this.db.exec("COMMIT");
      this.tradesBuf = [];
      this.liqsBuf = [];
      this.candlesBuf = [];
      this.pollsBuf = [];
      this.lastFlush = now;
    } catch (e) {
      try {
        this.db.exec("ROLLBACK");
      } catch {
        /* nothing to roll back */
      }
      // Never let history I/O kill recording; drop the batch (counters stay).
      this.tradesBuf = [];
      this.liqsBuf = [];
      this.candlesBuf = [];
      this.pollsBuf = [];
      this.lastFlush = now;
      console.error(`[history] flush failed: ${e}`);
    }
  }

  /** Retention prune + WAL truncate so the file tracks the window. */
  prune() {
    const cutoff = Date.now() - this.keepDays * 86_400_000;
    try {
      this.db.exec("BEGIN");
      for (const table of ["trades", "liquidations", "book_polls"])
        this.db.query(`DELETE FROM ${table} WHERE ts < ?`).run(cutoff);
      this.db.query("DELETE FROM candles_1m WHERE ts < ?").run(cutoff);
      this.db.exec("COMMIT");
      this.db.exec("PRAGMA wal_checkpoint(TRUNCATE);");
    } catch (e) {
      console.error(`[history] prune failed: ${e}`);
    }
  }

  // ── queries (REST /history/...) ─────────────────────────────────────────────

  trades(q: HistoryQuery) {
    const { where, args, limit } = this.where(q);
    return this.db
      .query(
        `SELECT ts, symbol, price, qty, is_buy, usd FROM trades ${where} ORDER BY ts DESC LIMIT ${limit}`,
      )
      .all(...args);
  }

  liquidations(q: HistoryQuery) {
    const { where, args, limit } = this.where(q);
    return this.db
      .query(
        `SELECT ts, symbol, side, price, qty, usd FROM liquidations ${where} ORDER BY ts DESC LIMIT ${limit}`,
      )
      .all(...args);
  }

  candles(q: HistoryQuery) {
    const { where, args, limit } = this.where(q);
    return this.db
      .query(
        `SELECT ts, symbol, o, h, l, c, v FROM candles_1m ${where} ORDER BY ts ASC LIMIT ${limit}`,
      )
      .all(...args);
  }

  bookPolls(q: HistoryQuery) {
    const { where, args, limit } = this.where(q);
    return this.db
      .query(
        `SELECT ts, symbol, best_bid, best_ask, bid_notional, ask_notional, bid_levels, ask_levels FROM book_polls ${where} ORDER BY ts DESC LIMIT ${limit}`,
      )
      .all(...args);
  }

  private where(q: HistoryQuery) {
    const clauses: string[] = [];
    const args: unknown[] = [];
    if (q.symbol) {
      clauses.push("symbol = ?");
      args.push(q.symbol.toLowerCase());
    }
    if (q.from) {
      clauses.push("ts >= ?");
      args.push(q.from);
    }
    if (q.to) {
      clauses.push("ts <= ?");
      args.push(q.to);
    }
    const where = clauses.length ? `WHERE ${clauses.join(" AND ")}` : "";
    const limit = Math.max(1, Math.min(q.limit || 500, 10_000));
    return { where, args, limit };
  }

  stats() {
    const count = (t: string) =>
      (this.db.query(`SELECT COUNT(*) AS n FROM ${t}`).get() as { n: number }).n;
    const span = (t: string) =>
      this.db.query(`SELECT MIN(ts) AS a, MAX(ts) AS b FROM ${t}`).get() as {
        a: number | null;
        b: number | null;
      };
    let bytes = 0;
    try {
      bytes = (this.db.query("PRAGMA page_count * page_size AS n").get() as { n: number }).n;
    } catch {
      /* page_size pragma shape differs across runtimes */
    }
    return {
      path: this.path,
      keep_days: this.keepDays,
      bytes,
      trades: { rows: count("trades"), span: span("trades") },
      liquidations: { rows: count("liquidations"), span: span("liquidations") },
      candles_1m: { rows: count("candles_1m"), span: span("candles_1m") },
      book_polls: { rows: count("book_polls"), span: span("book_polls") },
    };
  }

  close() {
    this.flush();
    try {
      this.db.exec("PRAGMA wal_checkpoint(TRUNCATE);");
    } catch {
      /* best effort on shutdown */
    }
    this.db.close();
  }
}
