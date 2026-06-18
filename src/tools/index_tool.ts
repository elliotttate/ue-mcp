/**
 * `index` — build and maintain the project's semantic index (code, config,
 * docs, and blueprint summaries). Local-handler tool: all work runs in the MCP
 * server process; the editor bridge is only used to extract blueprint/asset
 * summaries when connected.
 */
import { z } from "zod";
import { categoryTool, type ToolDef } from "../types.js";
import { Indexer, indexStatus } from "../intelligence/indexer.js";
import { generateProjectSummary } from "../intelligence/summary.js";
import { indexIgnorePath } from "../intelligence/paths.js";
import * as fs from "node:fs";

function cfgOf(ctx: { project: { config: { intelligence?: unknown } } }) {
  return (ctx.project.config.intelligence ?? {}) as import("../intelligence/config.js").IntelligenceConfig;
}

export const indexTool: ToolDef = categoryTool(
  "index",
  "Build and maintain the project's semantic index (code, config, docs, blueprint summaries) used by search and graph. Runs locally; blueprint summaries need the editor connected.",
  {
    build: {
      description:
        "Full or incremental index build. Params: rebuild? (force clean rebuild), includeAssets? (index blueprint summaries via the editor; default true)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const log: string[] = [];
        const indexer = new Indexer(ctx.project.projectDir!, ctx.project.projectName, ctx.bridge, cfgOf(ctx));
        const stats = await indexer.build({
          rebuild: p.rebuild === true,
          includeAssets: p.includeAssets !== false,
          onProgress: (m) => {
            log.push(m);
            console.error(`[ue-mcp][index] ${m}`);
          },
        });
        const note = !ctx.bridge.isConnected
          ? "Editor not connected — blueprint/asset summaries were skipped and will be added on a later build."
          : stats.skippedAssets > 0
            ? `${stats.skippedAssets} asset(s) could not be summarized this pass and will retry.`
            : undefined;
        return { ok: true, stats, note };
      },
    },
    update: {
      description: "Incremental index update (alias for build with rebuild=false).",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        const indexer = new Indexer(ctx.project.projectDir!, ctx.project.projectName, ctx.bridge, cfgOf(ctx));
        const stats = await indexer.build({ rebuild: false, includeAssets: true });
        return { ok: true, stats };
      },
    },
    status: {
      description: "Report index state (provider, dimensions, sources, chunk count) without rebuilding.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        return indexStatus(ctx.project.projectDir!);
      },
    },
    summary: {
      description:
        "Project overview: a structural digest (counts by kind/language, top directories, dependency hubs, README excerpt). Narrates to prose if a summarizer is configured, otherwise returns the digest for the agent to narrate.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        return generateProjectSummary(
          ctx.project.projectDir!,
          ctx.project.projectName,
          (ctx.project.config.intelligence ?? {}) as import("../intelligence/config.js").IntelligenceConfig,
        );
      },
    },
    clear: {
      description: "Delete the index (vectors, manifest, graph). The next build is a full rebuild.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        new Indexer(ctx.project.projectDir!, ctx.project.projectName, ctx.bridge, cfgOf(ctx)).clear();
        return { ok: true, cleared: true };
      },
    },
    ingest: {
      description:
        "Parse and index an external document or folder (PDF/DOCX/PPTX/XLSX/MD/TXT) into the project index. Params: path (absolute or project-relative)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const target = String(p.path ?? "");
        if (!target) throw new Error("ingest requires a 'path' parameter");
        const indexer = new Indexer(ctx.project.projectDir!, ctx.project.projectName, ctx.bridge, cfgOf(ctx));
        return indexer.ingest(target);
      },
    },
    ignore_patterns: {
      description:
        "Read or set the .ue-mcp/INDEX_IGNORE patterns (gitignore-lite). Params: set? (string[] to write); omit to read.",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const file = indexIgnorePath(ctx.project.projectDir!);
        if (Array.isArray(p.set)) {
          fs.mkdirSync(file.replace(/[/\\]INDEX_IGNORE$/, ""), { recursive: true });
          fs.writeFileSync(file, (p.set as string[]).join("\n") + "\n", "utf-8");
          return { ok: true, written: file, patterns: p.set };
        }
        const patterns = fs.existsSync(file)
          ? fs.readFileSync(file, "utf-8").split(/\r?\n/).filter((l) => l.trim() && !l.startsWith("#"))
          : [];
        return { path: file, exists: fs.existsSync(file), patterns };
      },
    },
  },
  undefined,
  {
    rebuild: z.boolean().optional().describe("Force a full clean rebuild"),
    includeAssets: z.boolean().optional().describe("Index blueprint/asset summaries via the editor (default true)"),
    path: z.string().optional().describe("File/folder path for ingest"),
    set: z.array(z.string()).optional().describe("Patterns to write for ignore_patterns"),
  },
);
