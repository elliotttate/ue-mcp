/**
 * MCP resources for the Project Intelligence layer. Resources are read-only,
 * browsable surfaces the client can subscribe to or inline without a tool call:
 *   - ue://index/status   index build state
 *   - ue://project-map    high-level knowledge-graph overview
 *   - ue://memory         list of project memories
 *   - ue://memory/{name}  a single memory's markdown
 */
import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { ToolContext } from "./types.js";
import { indexStatus } from "./intelligence/indexer.js";
import { loadGraph, topHubs } from "./intelligence/knowledge-graph.js";
import { listMemories, readMemory } from "./intelligence/memory-store.js";

function jsonContents(uri: URL, data: unknown) {
  return { contents: [{ uri: uri.href, mimeType: "application/json", text: JSON.stringify(data, null, 2) }] };
}

export function registerIntelligenceResources(server: McpServer, ctx: ToolContext): void {
  server.resource("index-status", "ue://index/status", async (uri) => {
    const dir = ctx.project.projectDir;
    return jsonContents(uri, dir ? indexStatus(dir) : { built: false, error: "no project loaded" });
  });

  server.resource("project-map", "ue://project-map", async (uri) => {
    const dir = ctx.project.projectDir;
    const g = dir ? loadGraph(dir) : null;
    if (!g) return jsonContents(uri, { error: 'no knowledge graph; run graph(action="build")' });
    const byKind: Record<string, number> = {};
    for (const n of Object.values(g.nodes)) byKind[n.kind] = (byKind[n.kind] ?? 0) + 1;
    return jsonContents(uri, {
      builtAt: g.builtAt,
      nodes: Object.keys(g.nodes).length,
      edges: g.edges.length,
      nodesByKind: byKind,
      topHubs: topHubs(g, 15).map((h) => ({ id: h.node.id, kind: h.node.kind, inDegree: h.scores.inDegree })),
    });
  });

  server.resource("memory-index", "ue://memory", async (uri) => {
    const dir = ctx.project.projectDir;
    return jsonContents(uri, { memories: dir ? listMemories(dir) : [] });
  });

  server.resource(
    "memory",
    new ResourceTemplate("ue://memory/{name}", {
      list: async () => {
        const dir = ctx.project.projectDir;
        const memos = dir ? listMemories(dir) : [];
        return {
          resources: memos.map((m) => ({ uri: `ue://memory/${m.name}`, name: m.name, description: m.title })),
        };
      },
    }),
    async (uri, variables) => {
      const dir = ctx.project.projectDir;
      const raw = variables.name;
      const name = Array.isArray(raw) ? raw[0] : raw;
      const content = dir ? readMemory(dir, String(name)) : null;
      return {
        contents: [
          { uri: uri.href, mimeType: "text/markdown", text: content ?? `# memory '${name}' not found` },
        ],
      };
    },
  );
}
