/**
 * Local Unreal code/identifier validation against the live editor's reflection
 * database. Catches hallucinated class names and blueprint node types before
 * the agent acts on them — entirely on the local machine (no cloud round-trip).
 * Results are advisory: an identifier we cannot reflect (e.g. an F-struct, which
 * is not a UClass) is reported "unverified", never a hard "invalid".
 */
import type { IBridge } from "../bridge.js";

export type VerdictStatus = "verified" | "unverified" | "not_found";

export interface IdentifierVerdict {
  name: string;
  status: VerdictStatus;
  kind?: string;
  detail?: string;
}

async function tryCall(bridge: IBridge, method: string, params: Record<string, unknown>): Promise<unknown | null> {
  try {
    return await bridge.call(method, params, 15_000);
  } catch {
    return null;
  }
}

function looksFound(result: unknown): boolean {
  if (result === null || result === undefined) return false;
  if (typeof result !== "object") return true;
  const o = result as Record<string, unknown>;
  if (o.error || o.found === false || o.exists === false) return false;
  if (o.found === true || o.exists === true || o.className || o.class || o.properties || o.functions) return true;
  return Object.keys(o).length > 0;
}

function hasResults(result: unknown): boolean {
  if (!result || typeof result !== "object") return false;
  const o = result as Record<string, unknown>;
  for (const v of Object.values(o)) {
    if (Array.isArray(v) && v.length > 0) return true;
  }
  return false;
}

/** Extract UE-style identifiers from a code snippet: prefixed PascalCase types
 *  (U/A/F/E/I/S...) and PascalCase call targets. */
export function extractIdentifiers(code: string): string[] {
  const set = new Set<string>();
  const typeRe = /\b([AUFEIS][A-Z][A-Za-z0-9_]{2,})\b/g;
  const callRe = /\b([A-Z][A-Za-z0-9_]{2,})\s*\(/g;
  let m: RegExpExecArray | null;
  while ((m = typeRe.exec(code)) !== null) set.add(m[1]);
  while ((m = callRe.exec(code)) !== null) set.add(m[1]);
  return [...set].slice(0, 120);
}

export async function validateClassNames(bridge: IBridge, names: string[]): Promise<IdentifierVerdict[]> {
  const out: IdentifierVerdict[] = [];
  for (const name of names) {
    if (!bridge.isConnected) {
      out.push({ name, status: "unverified", detail: "editor not connected" });
      continue;
    }
    const r = await tryCall(bridge, "reflect_class", { className: name });
    if (r === null) {
      out.push({ name, status: "unverified", detail: "not a reflectable UClass (may still be a valid struct/enum/function)" });
    } else if (looksFound(r)) {
      out.push({ name, status: "verified", kind: "class" });
    } else {
      out.push({ name, status: "not_found" });
    }
  }
  return out;
}

/** Recursively collect candidate node/type/function names from a blueprint plan. */
export function collectPlanNames(plan: unknown, out: Set<string> = new Set(), depth = 0): Set<string> {
  if (depth > 6 || out.size > 200) return out;
  if (Array.isArray(plan)) {
    for (const v of plan) collectPlanNames(v, out, depth + 1);
  } else if (plan && typeof plan === "object") {
    for (const [k, v] of Object.entries(plan)) {
      if (typeof v === "string" && /type|node|function|class|target/i.test(k)) out.add(v);
      else collectPlanNames(v, out, depth + 1);
    }
  }
  return out;
}

export async function validateNodeTypes(bridge: IBridge, names: string[]): Promise<IdentifierVerdict[]> {
  const out: IdentifierVerdict[] = [];
  for (const name of names) {
    if (!bridge.isConnected) {
      out.push({ name, status: "unverified", detail: "editor not connected" });
      continue;
    }
    const r = await tryCall(bridge, "search_node_types", { query: name });
    if (r === null) out.push({ name, status: "unverified", detail: "node search unavailable" });
    else if (hasResults(r)) out.push({ name, status: "verified", kind: "node" });
    else out.push({ name, status: "not_found" });
  }
  return out;
}

export function summarizeVerdicts(verdicts: IdentifierVerdict[]): {
  verified: string[];
  notFound: string[];
  unverified: string[];
  allClear: boolean;
} {
  const verified = verdicts.filter((v) => v.status === "verified").map((v) => v.name);
  const notFound = verdicts.filter((v) => v.status === "not_found").map((v) => v.name);
  const unverified = verdicts.filter((v) => v.status === "unverified").map((v) => v.name);
  return { verified, notFound, unverified, allClear: notFound.length === 0 };
}
