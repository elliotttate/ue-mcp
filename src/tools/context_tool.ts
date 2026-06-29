/**
 * `context` — capture the live editor's working context (current selection,
 * viewport, level) as a structured bundle the agent can reason over. Mirrors
 * the "what is the user looking at / working on" capture that a co-pilot needs,
 * built on existing editor reads. Prefers a dedicated bridge handler when the
 * deployed plugin provides one, and composes existing reads otherwise.
 */
import { z } from "zod";
import { categoryTool, type ToolDef, type ToolContext } from "../types.js";
import type { IBridge } from "../bridge.js";

async function tryCall(bridge: IBridge, method: string, params: Record<string, unknown> = {}): Promise<unknown | null> {
  try {
    return await bridge.call(method, params, 15_000);
  } catch {
    return null;
  }
}

function requireEditor(ctx: ToolContext): void {
  ctx.project.ensureLoaded();
  if (!ctx.bridge.isConnected) {
    throw new Error("Editor not connected. The context tool reads the live editor; start Unreal with the MCP bridge plugin.");
  }
}

/** Pull a list of selected-actor labels out of whatever shape get_selected
 *  returns, so we can fan out to get_actor_details. */
function selectionLabels(selected: unknown): string[] {
  const labels: string[] = [];
  const push = (v: unknown) => {
    if (typeof v === "string") labels.push(v);
    else if (v && typeof v === "object") {
      const o = v as Record<string, unknown>;
      const l = o.label ?? o.actorLabel ?? o.name ?? o.actorName;
      if (typeof l === "string") labels.push(l);
    }
  };
  if (Array.isArray(selected)) selected.forEach(push);
  else if (selected && typeof selected === "object") {
    const o = selected as Record<string, unknown>;
    const arr = o.actors ?? o.selected ?? o.selection;
    if (Array.isArray(arr)) arr.forEach(push);
  }
  return labels.slice(0, 50);
}

export const contextTool: ToolDef = categoryTool(
  "context",
  "Capture the live editor working context (selection, viewport, level) as a structured bundle for the agent. Requires the editor connected.",
  {
    get: {
      description: "Assemble a context bundle: current selection + viewport camera + level.",
      handler: async (ctx) => {
        requireEditor(ctx);
        const dedicated = await tryCall(ctx.bridge, "get_editor_context_bundle");
        if (dedicated) return { source: "bridge", bundle: dedicated };
        const [selected, viewport] = await Promise.all([
          tryCall(ctx.bridge, "get_selected_actors"),
          tryCall(ctx.bridge, "get_viewport_info"),
        ]);
        return {
          source: "composed",
          selection: selected ?? [],
          selectionCount: selectionLabels(selected).length,
          viewport: viewport ?? null,
        };
      },
    },
    capture_selection: {
      description: "Detailed dump of every selected actor (label, class, properties). Params: includeProperties?",
      handler: async (ctx, p) => {
        requireEditor(ctx);
        const selected = await tryCall(ctx.bridge, "get_selected_actors");
        const labels = selectionLabels(selected);
        const includeProperties = p.includeProperties === true;
        const details: unknown[] = [];
        for (const label of labels) {
          const d = await tryCall(ctx.bridge, "get_actor_details", { actorLabel: label, includeProperties });
          if (d) details.push(d);
        }
        return { count: labels.length, labels, details };
      },
    },
    capture_viewport: {
      description: "Screenshot the active viewport and return the camera info. Params: filename? (default Saved screenshot)",
      handler: async (ctx, p) => {
        requireEditor(ctx);
        const shot = await tryCall(ctx.bridge, "capture_screenshot", {
          filename: typeof p.filename === "string" ? p.filename : undefined,
          target: "auto",
        });
        const viewport = await tryCall(ctx.bridge, "get_viewport_info");
        return { screenshot: shot ?? null, viewport: viewport ?? null };
      },
    },
    capture_graph_selection: {
      description: "Describe the nodes selected in the active asset-editor graph (Blueprint/Widget/Animation) as readable pseudocode + a node list. Use for 'explain this selection' or to anchor blueprint(insert_logic). Params: none",
      handler: async (ctx) => {
        requireEditor(ctx);
        return ctx.bridge.call("get_selected_graph_nodes", {}, 15_000);
      },
    },
    content_browser: {
      description: "The Content Browser's current folder + the assets selected in it. Use for 'make one of these' / 'use the selected asset'. Params: none",
      handler: async (ctx) => {
        requireEditor(ctx);
        return ctx.bridge.call("get_content_browser_selection", {}, 15_000);
      },
    },
  },
  undefined,
  {
    includeProperties: z.boolean().optional().describe("Include reflected actor properties in capture_selection"),
    filename: z.string().optional().describe("Screenshot filename for capture_viewport"),
  },
);
