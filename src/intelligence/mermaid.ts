/**
 * Mermaid renderers for the structured graph data the bridge and knowledge
 * graph already produce. Emitting mermaid here (rather than asking the LLM to
 * transcribe node ids by hand) guarantees the diagram matches the real graph.
 * Clients that render mermaid show a diagram; others show the code block.
 */
import type { GraphNode, GraphEdge } from "./knowledge-graph.js";

/** Make a label safe inside a mermaid `["..."]` node. */
function escLabel(s: string): string {
  return s
    .replace(/[\r\n]+/g, " ")
    .replace(/"/g, "'")
    .replace(/[[\]{}]/g, "")
    .trim()
    .slice(0, 60) || "(unnamed)";
}

export interface MermaidResult {
  mermaid: string;
  nodeCount: number;
  truncated: boolean;
}

/** Render a set of knowledge-graph nodes/edges as a flowchart. */
export function graphToMermaid(
  nodes: GraphNode[],
  edges: GraphEdge[],
  opts: { maxNodes?: number; direction?: "LR" | "TD" } = {},
): MermaidResult {
  const max = opts.maxNodes ?? 60;
  const dir = opts.direction ?? "LR";
  const truncated = nodes.length > max;
  const shown = nodes.slice(0, max);

  const idMap = new Map<string, string>();
  const lines = [`flowchart ${dir}`];
  shown.forEach((n, i) => {
    const mid = `n${i}`;
    idMap.set(n.id, mid);
    lines.push(`  ${mid}["${escLabel(n.label)}"]`);
  });
  for (const e of edges) {
    const a = idMap.get(e.from);
    const b = idMap.get(e.to);
    if (!a || !b) continue;
    lines.push(e.type === "parent" ? `  ${a} -.->|parent| ${b}` : `  ${a} --> ${b}`);
  }
  return { mermaid: lines.join("\n"), nodeCount: shown.length, truncated };
}

export interface BlueprintGraphSummary {
  graphName?: string;
  graphType?: string;
  nodes?: Array<{ id?: string; title?: string; class?: string }>;
  execEdges?: Array<{ from?: string; to?: string }>;
  dataEdges?: Array<{ from?: string; to?: string }>;
}

/**
 * Render a blueprint graph summary (from read_blueprint_graph_summary) as a
 * flowchart: solid arrows for execution flow, dotted for data flow.
 */
export function blueprintSummaryToMermaid(
  summary: BlueprintGraphSummary,
  opts: { maxNodes?: number } = {},
): MermaidResult {
  const nodes = summary.nodes ?? [];
  const max = opts.maxNodes ?? 80;
  const truncated = nodes.length > max;
  const shown = nodes.slice(0, max);

  const idMap = new Map<string, string>();
  const lines = ["flowchart TD"];
  shown.forEach((n, i) => {
    const mid = `g${i}`;
    if (n.id) idMap.set(n.id, mid);
    lines.push(`  ${mid}["${escLabel(n.title ?? n.id ?? "node")}"]`);
  });
  for (const e of summary.execEdges ?? []) {
    const a = e.from ? idMap.get(e.from) : undefined;
    const b = e.to ? idMap.get(e.to) : undefined;
    if (a && b) lines.push(`  ${a} --> ${b}`);
  }
  for (const e of summary.dataEdges ?? []) {
    const a = e.from ? idMap.get(e.from) : undefined;
    const b = e.to ? idMap.get(e.to) : undefined;
    if (a && b) lines.push(`  ${a} -.-> ${b}`);
  }
  return { mermaid: lines.join("\n"), nodeCount: shown.length, truncated };
}
