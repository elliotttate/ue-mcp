/**
 * `graph` — the project knowledge graph. Build it, then ask project-wide
 * structural questions ("what depends on X", "how do A and B connect", "which
 * assets are the hubs") that per-asset reads cannot answer. This is the
 * project-wide understanding layer.
 */
import { z } from "zod";
import { categoryTool, type ToolDef, type ToolContext } from "../types.js";
import {
  GraphBuilder,
  loadGraph,
  neighbors,
  subgraph,
  shortestPath,
  topHubs,
  findNodes,
  type Direction,
  type NodeKind,
  type KnowledgeGraph,
} from "../intelligence/knowledge-graph.js";
import type { IntelligenceConfig } from "../intelligence/config.js";

function cfgOf(ctx: ToolContext): IntelligenceConfig {
  return (ctx.project.config.intelligence ?? {}) as IntelligenceConfig;
}

function requireGraph(ctx: ToolContext): KnowledgeGraph {
  const g = loadGraph(ctx.project.projectDir!);
  if (!g) throw new Error('No knowledge graph found. Run graph(action="build") first.');
  return g;
}

export const graphTool: ToolDef = categoryTool(
  "graph",
  "Project knowledge graph (blueprint dependencies + C++ includes) with hub/centrality ranking and path queries. Build it first; blueprint edges need the editor connected.",
  {
    build: {
      description:
        "Build the knowledge graph. Params: includeAssets? (blueprint deps via editor; default true), includeCode? (C++ includes; default true)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const builder = new GraphBuilder(ctx.project.projectDir!, ctx.project.projectName, ctx.bridge, cfgOf(ctx));
        const { stats } = await builder.build({
          includeAssets: p.includeAssets !== false,
          includeCode: p.includeCode !== false,
          onProgress: (m) => console.error(`[ue-mcp][graph] ${m}`),
        });
        let note: string | undefined;
        if (!ctx.bridge.isConnected) {
          note = stats.cachedDeps > 0
            ? "Editor not connected — blueprint edges use dependencies cached from the last index build. Run index(build) with the editor connected to refresh them."
            : "Editor not connected and no cached blueprint deps were found — only the C++ include graph was built. Run index(build) with the editor connected, then rebuild the graph.";
        }
        return { ok: true, stats, note };
      },
    },
    stats: {
      description: "Graph summary: node/edge counts by kind plus the top hubs.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        return summarize(g);
      },
    },
    project_map: {
      description: "High-level project map: counts by kind, edge totals, and the most central blueprints and files.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        return {
          ...summarize(g),
          topBlueprints: topHubs(g, 15, "blueprint").map(fmtHub),
          topFiles: topHubs(g, 15, "file").map(fmtHub),
        };
      },
    },
    neighbors: {
      description: "Direct neighbors of a node. Params: node (id), direction? (out|in|both)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        const node = String(p.node ?? "");
        if (!g.nodes[node]) return { error: `Node not found: ${node}`, hint: 'Use graph(action="find") to locate it.' };
        const dir = (p.direction as Direction) ?? "both";
        return {
          node,
          direction: dir,
          neighbors: neighbors(g, node, dir).map((n) => ({ id: n.node.id, kind: n.node.kind, edge: n.edge.type })),
        };
      },
    },
    subgraph: {
      description: "k-hop neighborhood around a node. Params: node (id), depth? (default 1)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        const node = String(p.node ?? "");
        if (!g.nodes[node]) return { error: `Node not found: ${node}` };
        const depth = typeof p.depth === "number" ? Math.min(4, Math.max(1, p.depth)) : 1;
        const sg = subgraph(g, node, depth);
        return {
          node,
          depth,
          nodeCount: sg.nodes.length,
          edgeCount: sg.edges.length,
          truncated: sg.truncated,
          nodes: sg.nodes.map((n) => ({ id: n.id, kind: n.kind })),
          edges: sg.edges.slice(0, 1000),
        };
      },
    },
    path: {
      description: "Shortest dependency path between two nodes. Params: from (id), to (id)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        const from = String(p.from ?? "");
        const to = String(p.to ?? "");
        const path = shortestPath(g, from, to);
        return path ? { from, to, length: path.length - 1, path } : { from, to, path: null, note: "No path found." };
      },
    },
    hubs: {
      description: "Most-depended-on nodes (by in-degree, then centrality). Params: limit?, kind? (blueprint|asset|file)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        const limit = typeof p.limit === "number" ? Math.min(100, p.limit) : 20;
        return { hubs: topHubs(g, limit, p.kind as NodeKind | undefined).map(fmtHub) };
      },
    },
    find: {
      description: "Find nodes whose id/label matches a query. Params: query",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const g = requireGraph(ctx);
        const nodes = findNodes(g, String(p.query ?? ""));
        return { query: p.query, count: nodes.length, nodes };
      },
    },
  },
  undefined,
  {
    node: z.string().optional().describe("Node id (asset path or source file path)"),
    from: z.string().optional().describe("Path query: source node id"),
    to: z.string().optional().describe("Path query: target node id"),
    depth: z.number().int().min(1).max(4).optional().describe("Neighborhood hop depth"),
    direction: z.enum(["out", "in", "both"]).optional().describe("Edge direction for neighbors"),
    limit: z.number().int().min(1).max(100).optional().describe("Max results for hubs"),
    kind: z.enum(["blueprint", "asset", "file", "class", "module"]).optional().describe("Restrict to a node kind"),
    query: z.string().optional().describe("Find-by-name query"),
    includeAssets: z.boolean().optional().describe("Include blueprint dependency edges (default true)"),
    includeCode: z.boolean().optional().describe("Include C++ include edges (default true)"),
  },
);

function summarize(g: KnowledgeGraph) {
  const byKind: Record<string, number> = {};
  for (const n of Object.values(g.nodes)) byKind[n.kind] = (byKind[n.kind] ?? 0) + 1;
  const byEdge: Record<string, number> = {};
  for (const e of g.edges) byEdge[e.type] = (byEdge[e.type] ?? 0) + 1;
  return {
    builtAt: g.builtAt,
    nodes: Object.keys(g.nodes).length,
    edges: g.edges.length,
    nodesByKind: byKind,
    edgesByType: byEdge,
    topHubs: topHubs(g, 10).map(fmtHub),
  };
}

function fmtHub(h: { node: { id: string; kind: string }; scores: { inDegree: number; centrality: number } }) {
  return { id: h.node.id, kind: h.node.kind, inDegree: h.scores.inDegree, centrality: h.scores.centrality };
}
