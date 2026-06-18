/**
 * `memory` — durable project notes the agent maintains across sessions. Stored
 * as plain markdown under `.ue-mcp/memory/`. Use it to record decisions,
 * conventions, naming schemes, and gotchas so they persist beyond a single
 * conversation's context window.
 */
import { z } from "zod";
import { categoryTool, type ToolDef } from "../types.js";
import {
  listMemories,
  readMemory,
  writeMemory,
  appendMemory,
  deleteMemory,
} from "../intelligence/memory-store.js";

export const memoryTool: ToolDef = categoryTool(
  "memory",
  "Durable project memory: markdown notes the agent keeps across sessions (decisions, conventions, gotchas, TODOs). Stored under .ue-mcp/memory/.",
  {
    list: {
      description: "List all memories with their titles.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        return { memories: listMemories(ctx.project.projectDir!) };
      },
    },
    read: {
      description: "Read a memory by name. Params: name",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const name = String(p.name ?? "");
        if (!name) throw new Error("memory.read requires a 'name'");
        const content = readMemory(ctx.project.projectDir!, name);
        return content === null ? { name, exists: false } : { name, exists: true, content };
      },
    },
    write: {
      description: "Create or overwrite a memory. Params: name, content",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const name = String(p.name ?? "");
        if (!name) throw new Error("memory.write requires a 'name'");
        const file = writeMemory(ctx.project.projectDir!, name, String(p.content ?? ""));
        return { ok: true, name, file };
      },
    },
    append: {
      description: "Append to a memory (creates it if missing). Params: name, content",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const name = String(p.name ?? "");
        if (!name) throw new Error("memory.append requires a 'name'");
        const file = appendMemory(ctx.project.projectDir!, name, String(p.content ?? ""));
        return { ok: true, name, file };
      },
    },
    delete: {
      description: "Delete a memory. Params: name",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const name = String(p.name ?? "");
        if (!name) throw new Error("memory.delete requires a 'name'");
        return { ok: true, name, deleted: deleteMemory(ctx.project.projectDir!, name) };
      },
    },
  },
  undefined,
  {
    name: z.string().optional().describe("Memory name (slug)"),
    content: z.string().optional().describe("Memory body (markdown)"),
  },
);
