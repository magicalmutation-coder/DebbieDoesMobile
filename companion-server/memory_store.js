/**
 * memory_store.js — Debbie's persistent memory & RAG backend
 *
 * Uses SQLite (better-sqlite3) with:
 *   - `facts`  table: long-term key-value facts
 *   - `turns`  FTS5  table: conversation history with full-text search
 *
 * REST endpoints mounted by addMemoryRoutes():
 *   GET  /memory/query?q=<text>&limit=<n>   — retrieve relevant memories
 *   POST /memory/turn                        — store a conversation turn
 *   POST /memory/fact                        — store / update a fact
 *   GET  /memory/stats                       — counts + recent summary
 *   DELETE /memory/clear                     — wipe all memory
 */

'use strict';

const path = require('path');
const rateLimit = require('express-rate-limit');

let db = null;

/* ── DB setup ────────────────────────────────────────────────────────────── */

function initDb() {
    if (db) return db;

    let Database;
    try {
        Database = require('better-sqlite3');
    } catch (e) {
        console.warn('[memory] better-sqlite3 not installed — memory store disabled.');
        console.warn('[memory] Run: cd companion-server && npm install');
        return null;
    }

    const dbPath = process.env.MEMORY_DB_PATH ||
                   path.join(__dirname, 'debbie_memory.db');

    db = new Database(dbPath);
    db.pragma('journal_mode = WAL');
    db.pragma('synchronous = NORMAL');

    /* Long-term facts */
    db.exec(`
        CREATE TABLE IF NOT EXISTS facts (
            key         TEXT PRIMARY KEY,
            value       TEXT NOT NULL,
            importance  INTEGER DEFAULT 5,
            created_at  INTEGER NOT NULL,
            updated_at  INTEGER NOT NULL
        );
    `);

    /* Conversation turns — FTS5 for full-text retrieval */
    db.exec(`
        CREATE VIRTUAL TABLE IF NOT EXISTS turns USING fts5(
            role,
            text,
            session_id UNINDEXED,
            ts         UNINDEXED,
            tokenize = 'porter ascii'
        );
    `);

    /* Regular table to track row count efficiently */
    db.exec(`
        CREATE TABLE IF NOT EXISTS turn_meta (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            role       TEXT,
            ts         INTEGER
        );
    `);

    console.log(`[memory] SQLite memory store ready at ${dbPath}`);
    return db;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

function nowMs() {
    return Date.now();
}

function sanitiseText(t) {
    if (typeof t !== 'string') return '';
    return t.slice(0, 2000).replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, '');
}

function sanitiseKey(k) {
    if (typeof k !== 'string') return '';
    return k.slice(0, 64).replace(/[^a-zA-Z0-9_\-\.]/g, '_');
}

/* ── Core operations ─────────────────────────────────────────────────────── */

/**
 * Store / update a long-term fact.
 */
function saveFact(key, value, importance = 5) {
    if (!db) return false;
    const k = sanitiseKey(key);
    const v = sanitiseText(value);
    const imp = Math.min(10, Math.max(0, parseInt(importance, 10) || 5));
    const now = nowMs();

    db.prepare(`
        INSERT INTO facts (key, value, importance, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(key) DO UPDATE SET
            value      = excluded.value,
            importance = excluded.importance,
            updated_at = excluded.updated_at
    `).run(k, v, imp, now, now);
    return true;
}

/**
 * Store a conversation turn.
 * Prunes oldest turns when count exceeds maxTurns.
 */
function saveTurn(role, text, sessionId = 'default', maxTurns = 500) {
    if (!db) return false;
    const r   = role === 'assistant' ? 'assistant' : 'user';
    const t   = sanitiseText(text);
    const now = nowMs();

    db.prepare(`INSERT INTO turns (role, text, session_id, ts) VALUES (?, ?, ?, ?)`)
      .run(r, t, sessionId, now);
    db.prepare(`INSERT INTO turn_meta (role, ts) VALUES (?, ?)`)
      .run(r, now);

    /* Prune if over limit */
    const countRow = db.prepare(`SELECT COUNT(*) AS n FROM turn_meta`).get();
    if (countRow && countRow.n > maxTurns) {
        const overage = countRow.n - maxTurns;
        /* Delete oldest from FTS — need rowids */
        const oldRows = db.prepare(
            `SELECT id FROM turn_meta ORDER BY id ASC LIMIT ?`
        ).all(overage);
        const stmt = db.prepare(`DELETE FROM turns WHERE rowid = ?`);
        const del  = db.prepare(`DELETE FROM turn_meta WHERE id = ?`);
        for (const row of oldRows) {
            stmt.run(row.id);
            del.run(row.id);
        }
    }
    return true;
}

/**
 * Full-text search over turns and facts, returning the top `limit` results.
 * Falls back to recency-based retrieval when the query is empty.
 */
function queryMemory(queryText, limit = 5) {
    if (!db) return [];
    const lim = Math.min(20, Math.max(1, parseInt(limit, 10) || 5));
    const results = [];

    if (queryText && queryText.trim().length > 0) {
        /* FTS5 search over turns — wrap in try/catch for malformed queries */
        try {
            const q = sanitiseText(queryText).trim();
            const rows = db.prepare(`
                SELECT role, text, ts,
                       rank AS score
                FROM   turns
                WHERE  turns MATCH ?
                ORDER  BY rank
                LIMIT  ?
            `).all(q, lim);

            for (const r of rows) {
                results.push({
                    source:    'conversation',
                    role:      r.role,
                    text:      r.text,
                    timestamp: r.ts,
                });
            }
        } catch (_) {
            /* Fall through to recency fallback */
        }

        /* Also search facts */
        const factRows = db.prepare(`
            SELECT key, value, importance, updated_at
            FROM   facts
            WHERE  LOWER(key)   LIKE '%' || LOWER(?) || '%'
               OR  LOWER(value) LIKE '%' || LOWER(?) || '%'
            ORDER  BY importance DESC, updated_at DESC
            LIMIT  ?
        `).all(queryText, queryText, lim);

        for (const r of factRows) {
            results.push({
                source:    'fact',
                key:       r.key,
                text:      `${r.key}: ${r.value}`,
                timestamp: r.updated_at,
            });
        }
    } else {
        /* No query — return most recent turns */
        const rows = db.prepare(`
            SELECT role, text, ts FROM turns
            ORDER  BY ts DESC
            LIMIT  ?
        `).all(lim);
        for (const r of rows) {
            results.push({
                source:    'conversation',
                role:      r.role,
                text:      r.text,
                timestamp: r.ts,
            });
        }
    }

    /* Sort by relevance then recency */
    results.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
    return results.slice(0, lim);
}

/**
 * Return summary statistics.
 */
function getStats() {
    if (!db) return { enabled: false };
    const factCount = db.prepare(`SELECT COUNT(*) AS n FROM facts`).get().n;
    const turnCount = db.prepare(`SELECT COUNT(*) AS n FROM turn_meta`).get().n;
    const recent = db.prepare(`
        SELECT role, text, ts FROM turns ORDER BY ts DESC LIMIT 5
    `).all();
    return { enabled: true, facts: factCount, turns: turnCount, recent };
}

/**
 * Wipe all stored memory.
 */
function clearAll() {
    if (!db) return;
    db.exec(`DELETE FROM facts; DELETE FROM turns; DELETE FROM turn_meta;`);
    console.log('[memory] All memory cleared');
}

/* ── Express route handler ───────────────────────────────────────────────── */

const memLimiter = rateLimit({ windowMs: 60000, max: 120 });

function addMemoryRoutes(app) {
    /* Lazy-initialise the DB */
    const database = initDb();
    if (!database) {
        /* If better-sqlite3 is missing, mount stub routes that return 503 */
        app.use('/memory', (req, res) => {
            res.status(503).json({
                error: 'Memory store not available. Run: npm install',
            });
        });
        return;
    }

    /* GET /memory/query?q=<text>&limit=<n> */
    app.get('/memory/query', memLimiter, (req, res) => {
        const q     = typeof req.query.q === 'string' ? req.query.q : '';
        const limit = parseInt(req.query.limit, 10) || 5;
        const memories = queryMemory(q, limit);
        res.json({ ok: true, query: q, memories });
    });

    /* POST /memory/turn — { role, text, session_id? } */
    app.post('/memory/turn', memLimiter, (req, res) => {
        const { role = 'user', text = '', session_id = 'default' } = req.body || {};
        if (!text) return res.status(400).json({ error: 'text required' });
        saveTurn(role, text, session_id);
        res.json({ ok: true });
    });

    /* POST /memory/fact — { key, value, importance? } */
    app.post('/memory/fact', memLimiter, (req, res) => {
        const { key = '', value = '', importance = 5 } = req.body || {};
        if (!key || !value) return res.status(400).json({ error: 'key and value required' });
        const ok = saveFact(key, value, importance);
        res.json({ ok });
    });

    /* GET /memory/stats */
    app.get('/memory/stats', memLimiter, (req, res) => {
        res.json(getStats());
    });

    /* DELETE /memory/clear */
    app.delete('/memory/clear', memLimiter, (req, res) => {
        clearAll();
        res.json({ ok: true });
    });

    console.log('[memory] Routes mounted: /memory/{query,turn,fact,stats,clear}');
}

module.exports = { addMemoryRoutes, saveFact, saveTurn, queryMemory, getStats, clearAll, initDb };
