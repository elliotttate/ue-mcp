/**
 * A token -> entry postings index built over the vector store. It lets the
 * hybrid reranker gather lexical candidates (entries that actually contain a
 * query token) without rescanning and re-tokenizing every chunk on each query.
 * Built once per loaded store and cached; for large projects this turns the
 * lexical pass from an O(all chunks) scan into O(matching chunks).
 */
import type { VectorStore } from "./vector-store.js";
import type { VectorEntry } from "./types.js";
import { tokenize } from "./text.js";

export class LexicalIndex {
  private postings = new Map<string, Set<string>>();
  private byId = new Map<string, VectorEntry>();

  constructor(store: VectorStore) {
    for (const entry of store.values()) {
      this.byId.set(entry.id, entry);
      this.add(entry.id, entry.meta.text);
      if (entry.meta.symbol) this.add(entry.id, entry.meta.symbol);
    }
  }

  private add(id: string, text: string): void {
    const seen = new Set<string>();
    for (const tok of tokenize(text)) {
      if (seen.has(tok)) continue;
      seen.add(tok);
      let posting = this.postings.get(tok);
      if (!posting) {
        posting = new Set();
        this.postings.set(tok, posting);
      }
      posting.add(id);
    }
  }

  /** Entries containing at least one of the query tokens. */
  candidates(queryTokens: string[]): VectorEntry[] {
    const ids = new Set<string>();
    for (const tok of queryTokens) {
      const posting = this.postings.get(tok);
      if (posting) for (const id of posting) ids.add(id);
    }
    const out: VectorEntry[] = [];
    for (const id of ids) {
      const e = this.byId.get(id);
      if (e) out.push(e);
    }
    return out;
  }
}
