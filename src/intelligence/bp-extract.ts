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
export function toGamePath(relPosix: string, projectName: string | null): string {
  // relPosix like "Content/Blueprints/BP_Player.uasset" → "/Game/Blueprints/BP_Player"
  const m = /(^|.*?\/)Content\/(.+)\.(uasset|umap)$/i.exec(relPosix);
  if (m) return "/Game/" + m[2];
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
): Promise<unknown | null> {
  try {
    return await bridge.call(method, params, 20_000);
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
