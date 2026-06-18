/**
 * Rough token estimation for context budgeting. Mirrors the common ~4-chars-
 * per-token heuristic, floored by a word count so very short strings are not
 * underestimated. Good enough for "will this fit"; not a billing-grade count.
 */
export function estimateTokens(text: string): number {
  if (!text) return 0;
  const chars = text.length;
  const words = (text.match(/\S+/g) ?? []).length;
  return Math.max(Math.ceil(chars / 4), Math.ceil(words * 0.75));
}
