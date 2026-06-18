/**
 * A tiny embedded vector store. Vectors are kept in memory and persisted to a
 * JSONL file (header line + one base64-Float32 row per entry). Search is a
 * brute-force top-k cosine scan — at project scale (thousands to low tens of
 * thousands of chunks) this is well under a frame and needs no native module,
 * no server, and no extra dependency.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import type { VectorEntry } from "./types.js";
import { dot } from "./vec.js";

const STORE_VERSION = 1;

interface StoreHeader {
  __store: number;
  dim: number;
  provider: string;
}

interface StoreRow {
  id: string;
  v: string; // base64 little-endian Float32
  meta: VectorEntry["meta"];
}

export interface ScoredEntry {
  entry: VectorEntry;
  score: number;
}

export class VectorStore {
  private entries = new Map<string, VectorEntry>();

  constructor(
    public dim: number,
    /** Provider key the vectors were produced with (name:model). */
    public provider: string,
  ) {}

  get size(): number {
    return this.entries.size;
  }

  upsert(items: VectorEntry[]): void {
    for (const e of items) this.entries.set(e.id, e);
  }

  remove(ids: Iterable<string>): void {
    for (const id of ids) this.entries.delete(id);
  }

  /** Drop every entry whose source matches the predicate (used on re-index). */
  removeBySource(match: (source: string) => boolean): void {
    for (const [id, e] of this.entries) {
      if (match(e.meta.source)) this.entries.delete(id);
    }
  }

  clear(): void {
    this.entries.clear();
  }

  /** Top-k by cosine (vectors are stored normalized, so cosine == dot). */
  search(query: Float32Array, k: number, filter?: (e: VectorEntry) => boolean): ScoredEntry[] {
    const scored: ScoredEntry[] = [];
    for (const entry of this.entries.values()) {
      if (filter && !filter(entry)) continue;
      scored.push({ entry, score: dot(query, entry.vector) });
    }
    scored.sort((a, b) => b.score - a.score);
    return scored.slice(0, Math.max(0, k));
  }

  /** Iterate every entry (used by the lexical reranker). */
  values(): IterableIterator<VectorEntry> {
    return this.entries.values();
  }

  /** Distinct sources currently represented in the store. */
  sources(): Set<string> {
    const s = new Set<string>();
    for (const e of this.entries.values()) s.add(e.meta.source);
    return s;
  }

  save(filePath: string): void {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    const header: StoreHeader = { __store: STORE_VERSION, dim: this.dim, provider: this.provider };
    const lines: string[] = [JSON.stringify(header)];
    for (const e of this.entries.values()) {
      const row: StoreRow = { id: e.id, v: encodeVec(e.vector), meta: e.meta };
      lines.push(JSON.stringify(row));
    }
    // Atomic-ish write: temp then rename, so a crash mid-write can't truncate
    // an existing index.
    const tmp = filePath + ".tmp";
    fs.writeFileSync(tmp, lines.join("\n"), "utf-8");
    fs.renameSync(tmp, filePath);
  }

  static load(filePath: string): VectorStore | null {
    if (!fs.existsSync(filePath)) return null;
    const text = fs.readFileSync(filePath, "utf-8");
    const lines = text.split(/\r?\n/).filter((l) => l.length > 0);
    if (lines.length === 0) return null;
    let header: StoreHeader;
    try {
      header = JSON.parse(lines[0]) as StoreHeader;
    } catch {
      return null;
    }
    if (header.__store !== STORE_VERSION) return null;
    const store = new VectorStore(header.dim, header.provider);
    for (let i = 1; i < lines.length; i++) {
      try {
        const row = JSON.parse(lines[i]) as StoreRow;
        store.entries.set(row.id, { id: row.id, vector: decodeVec(row.v), meta: row.meta });
      } catch {
        // Skip a corrupt row rather than discarding the whole index.
      }
    }
    return store;
  }
}

function encodeVec(v: Float32Array): string {
  return Buffer.from(v.buffer, v.byteOffset, v.byteLength).toString("base64");
}

function decodeVec(s: string): Float32Array {
  const buf = Buffer.from(s, "base64");
  // Copy into a fresh, 4-byte-aligned ArrayBuffer (base64 Buffers are not
  // guaranteed aligned for a Float32Array view).
  const ab = new ArrayBuffer(buf.byteLength);
  new Uint8Array(ab).set(buf);
  return new Float32Array(ab);
}
