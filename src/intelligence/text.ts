/**
 * Text tokenization shared by the local embedding and the lexical reranker.
 * Tuned for code/Unreal identifiers: it splits on non-alphanumerics AND on
 * camelCase / PascalCase / acronym boundaries, so "GetPlayerHealth_BP" yields
 * ["get", "player", "health", "bp"]. This is what lets identifier search work
 * without a real model.
 */

/** FNV-1a 32-bit hash (unsigned). Deterministic, fast, no deps. */
export function fnv1a(str: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

/** Split a string into normalized lowercase word tokens (identifier-aware). */
export function tokenize(text: string): string[] {
  const out: string[] = [];
  for (const raw of text.split(/[^A-Za-z0-9]+/)) {
    if (!raw) continue;
    const split = raw
      .replace(/([a-z0-9])([A-Z])/g, "$1 $2") // camelCase boundary
      .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2") // ACRONYMWord boundary
      .split(/\s+/);
    for (const p of split) {
      const t = p.toLowerCase();
      if (t.length >= 1) out.push(t);
    }
  }
  return out;
}

/** Character trigrams for subword robustness (typos, partial identifiers). */
export function trigrams(token: string): string[] {
  if (token.length < 3) return [token];
  const out: string[] = [];
  for (let i = 0; i <= token.length - 3; i++) out.push(token.slice(i, i + 3));
  return out;
}

/** Bag-of-tokens with term frequencies. */
export function termFreqs(text: string): Map<string, number> {
  const m = new Map<string, number>();
  for (const t of tokenize(text)) m.set(t, (m.get(t) ?? 0) + 1);
  return m;
}
