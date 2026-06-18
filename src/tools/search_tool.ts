/**
 * `search` — retrieval over the project's semantic index. Returns ranked
 * snippets with file/line provenance for the driving agent to reason over.
 * This is the "find the relevant code/blueprint/doc" surface; it supplies no
 * intelligence of its own — the attached LLM does the reasoning.
 */
import { z } from "zod";
import { categoryTool, type ToolDef, type ToolContext } from "../types.js";
import { loadIndex } from "../intelligence/indexer.js";
import { searchIndex, type SearchMode } from "../intelligence/search.js";
import type { IntelligenceConfig } from "../intelligence/config.js";
import type { ChunkKind } from "../intelligence/types.js";

function cfgOf(ctx: { project: { config: { intelligence?: unknown } } }): IntelligenceConfig {
  return (ctx.project.config.intelligence ?? {}) as IntelligenceConfig;
}

const NOT_BUILT = {
  error: "No index found. Run index(action=\"build\") first to enable search.",
};

export const searchTool: ToolDef = categoryTool(
  "search",
  "Semantic + lexical retrieval over the indexed project (code, config, docs, blueprint summaries). Build the index first with the `index` tool.",
  {
    semantic: {
      description: "Pure embedding similarity search. Params: query, k?, kind?, sourcePrefix?",
      handler: async (ctx, p) => runSearch(ctx, p, "semantic"),
    },
    hybrid: {
      description:
        "Fused semantic + lexical search (best default for code). Params: query, k?, kind?, sourcePrefix?",
      handler: async (ctx, p) => runSearch(ctx, p, "hybrid"),
    },
    code_examples: {
      description:
        "Retrieve example code chunks relevant to a query (kind=code). Params: query, k?",
      handler: async (ctx, p) => runSearch(ctx, p, "hybrid", "code", 900),
    },
    references: {
      description:
        "Find where a symbol/identifier appears across the codebase (lexical-first). Params: query (the symbol), k?",
      handler: async (ctx, p) => runSearch(ctx, p, "lexical", "code"),
    },
  },
  undefined,
  {
    query: z.string().optional().describe("Search query or symbol"),
    k: z.number().int().min(1).max(50).optional().describe("Max results (default 8)"),
    kind: z
      .enum(["code", "blueprint", "asset", "document", "config", "text", "all"])
      .optional()
      .describe("Restrict to a chunk kind"),
    sourcePrefix: z.string().optional().describe("Restrict to sources starting with this string"),
  },
);

async function runSearch(
  ctx: ToolContext,
  p: Record<string, unknown>,
  mode: SearchMode,
  forceKind?: ChunkKind,
  snippetChars?: number,
): Promise<unknown> {
  ctx.project.ensureLoaded();
  const query = String(p.query ?? "").trim();
  if (!query) throw new Error("search requires a 'query' parameter");
  const idx = loadIndex(ctx.project.projectDir!, cfgOf(ctx));
  if (!idx) return NOT_BUILT;
  const hits = await searchIndex(idx.store, idx.provider, {
    query,
    k: typeof p.k === "number" ? p.k : undefined,
    mode,
    kind: forceKind ?? (p.kind as ChunkKind | "all" | undefined),
    sourcePrefix: typeof p.sourcePrefix === "string" ? p.sourcePrefix : undefined,
    snippetChars,
  });
  return { query, mode, provider: idx.store.provider, count: hits.length, hits };
}
