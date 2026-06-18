/**
 * Line-window chunker. Splits a text file into overlapping windows sized by a
 * character budget, preferring to break on blank lines near the boundary so a
 * chunk rarely cuts a function in half. Each chunk records its line span and a
 * best-effort enclosing symbol, which the retrieval layer surfaces as
 * provenance.
 */
import type { Chunk, ChunkKind } from "./types.js";

const TARGET_CHARS = 1400; // ~350 tokens
const MAX_LINES = 160;
const OVERLAP_LINES = 1;

/** Cheap "what declaration encloses this line" guess for code chunks. */
const DECL_RE =
  /^\s*(?:UCLASS|USTRUCT|UENUM|UFUNCTION|UPROPERTY)?\s*(?:public|private|protected|static|virtual|inline|export|async|def|class|struct|void|FORCEINLINE)?[\s\S]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/;
const CLASS_RE = /^\s*(?:class|struct|UCLASS\b.*?class|def|interface)\s+([A-Za-z_][A-Za-z0-9_]*)/;

function guessSymbol(lines: string[], start: number): string | undefined {
  for (let i = start; i >= 0 && i > start - 40; i--) {
    const line = lines[i];
    const cls = CLASS_RE.exec(line);
    if (cls) return cls[1];
    const fn = DECL_RE.exec(line);
    if (fn && !line.trim().startsWith("//") && !line.trim().startsWith("*")) return fn[1];
  }
  return undefined;
}

export function chunkText(
  source: string,
  text: string,
  kind: ChunkKind,
  language?: string,
): Chunk[] {
  const lines = text.split(/\r?\n/);
  const chunks: Chunk[] = [];
  let ordinal = 0;
  let i = 0;

  while (i < lines.length) {
    let chars = 0;
    let end = i;
    while (end < lines.length && chars < TARGET_CHARS && end - i < MAX_LINES) {
      chars += lines[end].length + 1;
      end++;
    }
    // Prefer to end on a blank line within the last few lines of the window.
    if (end < lines.length) {
      for (let b = end; b > i + 1 && b > end - 8; b--) {
        if (lines[b - 1].trim() === "") {
          end = b;
          break;
        }
      }
    }
    const slice = lines.slice(i, end);
    const body = slice.join("\n").trim();
    if (body.length > 0) {
      chunks.push({
        id: `${source}#${ordinal}`,
        source,
        kind,
        startLine: i + 1,
        endLine: end,
        symbol: kind === "code" ? guessSymbol(lines, i) : undefined,
        language,
        text: body,
      });
      ordinal++;
    }
    if (end <= i) break;
    i = end > i + OVERLAP_LINES ? end - OVERLAP_LINES : end;
  }

  return chunks;
}

/** Chunk a pre-extracted, already-compact text blob (blueprint summary,
 *  parsed document) as a single unit unless it is large. */
export function chunkBlob(
  source: string,
  text: string,
  kind: ChunkKind,
  symbol?: string,
): Chunk[] {
  if (text.length <= TARGET_CHARS * 2) {
    return [{ id: `${source}#0`, source, kind, symbol, text: text.trim() }];
  }
  const chunks = chunkText(source, text, kind);
  if (symbol) for (const c of chunks) c.symbol = c.symbol ?? symbol;
  return chunks;
}
