/**
 * Hybrid retrieval over the project index. Fuses dense semantic similarity
 * (embedding cosine) with a cheap lexical signal (query-token / phrase / symbol
 * overlap) so exact identifier matches are never lost — important for code,
 * where the right answer often hinges on a precise symbol name the embedding
 * alone may rank below a paraphrase.
 */
import type { EmbeddingProvider, SearchHit, VectorEntry, ChunkKind } from "./types.js";
import type { VectorStore } from "./vector-store.js";
import { tokenize } from "./text.js";

export type SearchMode = "semantic" | "hybrid" | "lexical";

export interface SearchOptions {
  query: string;
  k?: number;
  /** Restrict to a chunk kind, or "all". */
  kind?: ChunkKind | "all";
  mode?: SearchMode;
  /** Restrict to sources beginning with this string (e.g. "Source/", "/Game/"). */
  sourcePrefix?: string;
  /** Max chars of chunk text returned per hit. */
  snippetChars?: number;
}

const SEM_WEIGHT = 0.65;
const LEX_WEIGHT = 0.35;

function makeFilter(
  kind: ChunkKind | "all" | undefined,
  sourcePrefix: string | undefined,
): ((e: VectorEntry) => boolean) | undefined {
  if ((!kind || kind === "all") && !sourcePrefix) return undefined;
  return (e: VectorEntry) => {
    if (kind && kind !== "all" && e.meta.kind !== kind) return false;
    if (sourcePrefix && !e.meta.source.startsWith(sourcePrefix)) return false;
    return true;
  };
}

function lexicalScore(qTokens: string[], lowerText: string, symbol: string | undefined, phrase: string): number {
  if (qTokens.length === 0) return 0;
  let hits = 0;
  for (const q of qTokens) if (lowerText.includes(q)) hits++;
  let s = hits / qTokens.length;
  if (phrase.length > 2 && lowerText.includes(phrase)) s += 0.6;
  if (symbol && qTokens.includes(symbol.toLowerCase())) s += 0.4;
  return s;
}

function toHit(entry: VectorEntry, score: number, snippetChars: number): SearchHit {
  const text = entry.meta.text;
  return {
    id: entry.id,
    score: Number(score.toFixed(4)),
    source: entry.meta.source,
    kind: entry.meta.kind,
    startLine: entry.meta.startLine,
    endLine: entry.meta.endLine,
    symbol: entry.meta.symbol,
    snippet: text.length > snippetChars ? text.slice(0, snippetChars) + "…" : text,
  };
}

export async function searchIndex(
  store: VectorStore,
  provider: EmbeddingProvider,
  opts: SearchOptions,
): Promise<SearchHit[]> {
  const k = Math.max(1, opts.k ?? 8);
  const mode: SearchMode = opts.mode ?? "hybrid";
  const snippetChars = opts.snippetChars ?? 600;
  const filter = makeFilter(opts.kind, opts.sourcePrefix);
  const qTokens = [...new Set(tokenize(opts.query))];
  const phrase = opts.query.toLowerCase().trim();

  const merged = new Map<string, { entry: VectorEntry; sem: number; lex: number }>();

  if (mode !== "lexical") {
    const [qvec] = await provider.embed([opts.query]);
    for (const { entry, score } of store.search(qvec, Math.max(k * 8, 64), filter)) {
      merged.set(entry.id, { entry, sem: score, lex: 0 });
    }
  }

  if (mode !== "semantic") {
    for (const entry of store.values()) {
      if (filter && !filter(entry)) continue;
      const lex = lexicalScore(qTokens, entry.meta.text.toLowerCase(), entry.meta.symbol, phrase);
      if (lex <= 0) continue;
      const existing = merged.get(entry.id);
      if (existing) existing.lex = lex;
      else merged.set(entry.id, { entry, sem: 0, lex });
    }
  }

  const scored = [...merged.values()].map(({ entry, sem, lex }) => {
    // Local cosine lives in [-1,1]; map to [0,1]. API providers are already ~[0,1].
    const sem01 = Math.max(0, Math.min(1, (sem + 1) / 2));
    const lex01 = Math.min(1, lex);
    let score: number;
    if (mode === "semantic") score = sem01;
    else if (mode === "lexical") score = lex01;
    else score = SEM_WEIGHT * sem01 + LEX_WEIGHT * lex01;
    return { entry, score };
  });

  scored.sort((a, b) => b.score - a.score);
  return scored.slice(0, k).map(({ entry, score }) => toHit(entry, score, snippetChars));
}
