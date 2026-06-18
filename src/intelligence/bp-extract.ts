/**
 * Blueprint / asset index-summary extraction. Turns a binary .uasset/.umap into
 * a compact text blob the embedder can index. Prefers a dedicated bridge
 * handler (`extract_index_summary`) when the deployed plugin provides one, and
 * falls back to composing existing reads (`read_graph_summary` +
 * `get_dependencies`). Returns null when the editor bridge is unavailable, so
 * indexing degrades gracefully to code/docs only.
 */
import type { IBridge } from "../bridge.js";

export interface BlueprintSummary {
  text: string;
  /** Asset names this blueprint references — fed to the knowledge graph. */
  dependencies: string[];
  symbol?: string;
}

/** Convert an absolute asset path into a /Game-style path the bridge accepts.
 *  Best-effort; the bridge also resolves project-relative content paths. */
export function toGamePath(relPosix: string, _projectName: string | null): string {
  // Plugin content mounts on its own root: Plugins/Foo/Content/BP.uasset → /Foo/BP
  const plugin = /(?:^|.*?\/)Plugins\/([^/]+)\/Content\/(.+)\.(?:uasset|umap)$/i.exec(relPosix);
  if (plugin) return `/${plugin[1]}/${plugin[2]}`;
  // Project content mounts on /Game: Content/Blueprints/BP.uasset → /Game/Blueprints/BP
  const game = /(?:^|.*?\/)Content\/(.+)\.(?:uasset|umap)$/i.exec(relPosix);
  if (game) return "/Game/" + game[1];
  return relPosix.replace(/\.(uasset|umap)$/i, "");
}

function collectStrings(value: unknown, out: string[], depth = 0): void {
  if (depth > 4 || out.length > 400) return;
  if (typeof value === "string") {
    out.push(value);
  } else if (Array.isArray(value)) {
    for (const v of value) collectStrings(v, out, depth + 1);
  } else if (value && typeof value === "object") {
    for (const v of Object.values(value)) collectStrings(v, out, depth + 1);
  }
}

async function tryCall(
  bridge: IBridge,
  method: string,
  params: Record<string, unknown>,
  timeoutMs = 20_000,
): Promise<unknown | null> {
  try {
    return await bridge.call(method, params, timeoutMs);
  } catch {
    return null;
  }
}

export async function extractBlueprintSummary(
  bridge: IBridge,
  gamePath: string,
): Promise<BlueprintSummary | null> {
  if (!bridge.isConnected) return null;

  // Preferred: a single dedicated handler that returns a compact summary.
  const dedicated = await tryCall(bridge, "extract_index_summary", { path: gamePath });
  if (dedicated && typeof dedicated === "object") {
    const d = dedicated as { summary?: unknown; text?: unknown; dependencies?: unknown; name?: unknown };
    const text = typeof d.summary === "string" ? d.summary : typeof d.text === "string" ? d.text : null;
    if (text) {
      const deps: string[] = [];
      collectStrings(d.dependencies ?? [], deps);
      return { text, dependencies: deps, symbol: typeof d.name === "string" ? d.name : undefined };
    }
  }

  // Fallback: compose existing reads. NOTE: these are the *bridge* method names
  // (registered C++ handlers), which differ from the TS action names.
  const summary = await tryCall(bridge, "read_blueprint_graph_summary", { path: gamePath });
  const deps = await tryCall(bridge, "get_blueprint_dependencies", { path: gamePath });
  if (!summary && !deps) return null;

  const parts: string[] = [`Blueprint asset: ${gamePath}`];
  if (summary) parts.push(compact(summary));
  const depNames: string[] = [];
  if (deps) {
    collectStrings(deps, depNames);
    if (depNames.length) parts.push("Dependencies: " + depNames.slice(0, 60).join(", "));
  }
  const text = parts.join("\n").slice(0, 6000);
  const name = gamePath.split("/").pop();
  return { text, dependencies: depNames, symbol: name };
}

/**
 * Extract many blueprint summaries with as few editor round-trips as possible.
 * Prefers a dedicated batch handler (extract_index_summaries) in chunks; for
 * any path the batch does not cover (or if the handler is absent) it falls back
 * to per-asset extraction. Returns a map keyed by game path.
 */
export async function extractBlueprintSummaries(
  bridge: IBridge,
  gamePaths: string[],
  batchSize = 200,
): Promise<Map<string, BlueprintSummary>> {
  const out = new Map<string, BlueprintSummary>();
  if (!bridge.isConnected || gamePaths.length === 0) return out;

  let batchSupported = true;
  for (let i = 0; i < gamePaths.length && batchSupported; i += batchSize) {
    const slice = gamePaths.slice(i, i + batchSize);
    // Generous timeout: the first batch after a fresh editor boot may trigger a
    // synchronous asset-registry scan on the C++ side.
    const res = await tryCall(bridge, "extract_index_summaries", { paths: slice }, 120_000);
    if (!res || typeof res !== "object") {
      batchSupported = false;
      break;
    }
    const items = Array.isArray(res)
      ? res
      : (res as { summaries?: unknown }).summaries;
    if (!Array.isArray(items)) {
      batchSupported = false;
      break;
    }
    for (const it of items) {
      if (!it || typeof it !== "object") continue;
      const o = it as { path?: string; summary?: string; text?: string; dependencies?: unknown; name?: string };
      const text = typeof o.summary === "string" ? o.summary : typeof o.text === "string" ? o.text : null;
      if (o.path && text) {
        const deps: string[] = [];
        collectStrings(o.dependencies ?? [], deps);
        out.set(o.path, { text, dependencies: deps, symbol: typeof o.name === "string" ? o.name : undefined });
      }
    }
  }

  // Per-asset fallback for anything not covered above.
  for (const gp of gamePaths) {
    if (out.has(gp)) continue;
    const s = await extractBlueprintSummary(bridge, gp);
    if (s) out.set(gp, s);
  }
  return out;
}

/** Render an arbitrary bridge result as compact, embeddable text. */
function compact(value: unknown): string {
  if (typeof value === "string") return value;
  try {
    const json = JSON.stringify(value);
    return json.length > 5000 ? json.slice(0, 5000) : json;
  } catch {
    return String(value);
  }
}
