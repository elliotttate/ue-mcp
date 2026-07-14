/**
 * The registry of all category tools the MCP server exposes.
 *
 * Kept in its own module (instead of living inside `index.ts`) so tests can
 * import the list without triggering the MCP server's top-level `main()`
 * side effect.
 */
import type { ToolDef } from "./types.js";

import { projectTool } from "./tools/project.js";
import { assetTool } from "./tools/asset.js";
import { blueprintTool } from "./tools/blueprint.js";
import { levelTool } from "./tools/level.js";
import { materialTool } from "./tools/material.js";
import { animationTool } from "./tools/animation.js";
import { landscapeTool } from "./tools/landscape.js";
import { pcgTool } from "./tools/pcg.js";
import { foliageTool } from "./tools/foliage.js";
import { niagaraTool } from "./tools/niagara.js";
import { audioTool } from "./tools/audio.js";
import { widgetTool } from "./tools/widget.js";
import { editorTool } from "./tools/editor.js";
import { reflectionTool } from "./tools/reflection.js";
import { gameplayTool } from "./tools/gameplay.js";
import { gasTool } from "./tools/gas.js";
import { networkingTool } from "./tools/networking.js";
import { demoTool } from "./tools/demo.js";
import { feedbackTool } from "./tools/feedback.js";
import { statetreeTool } from "./tools/statetree.js";
import { chooserTool } from "./tools/chooser.js";
import { pluginsTool } from "./tools/plugins.js";
// Project Intelligence layer (indexing / retrieval / graph / memory / context /
// image / validation). These are local-handler tools: they run in the server
// process and use the editor bridge only to extract data.
import { indexTool } from "./tools/index_tool.js";
import { searchTool } from "./tools/search_tool.js";
import { graphTool } from "./tools/graph_tool.js";
import { memoryTool } from "./tools/memory_tool.js";
import { contextTool } from "./tools/context_tool.js";
import { imageTool } from "./tools/image_tool.js";
import { validateTool } from "./tools/validate_tool.js";
import { epicTool } from "./tools/epic.js";
import { fabTool } from "./tools/fab.js";

export const ALL_TOOLS: ToolDef[] = [
  projectTool,
  assetTool,
  blueprintTool,
  levelTool,
  materialTool,
  animationTool,
  landscapeTool,
  pcgTool,
  foliageTool,
  niagaraTool,
  audioTool,
  widgetTool,
  editorTool,
  reflectionTool,
  gameplayTool,
  gasTool,
  networkingTool,
  demoTool,
  feedbackTool,
  statetreeTool,
  chooserTool,
  pluginsTool,
  indexTool,
  searchTool,
  graphTool,
  memoryTool,
  contextTool,
  imageTool,
  validateTool,
  epicTool,
  fabTool,
];

/** Flatten to (toolName, actionName, bridgeMethod) triples for every action
 *  that dispatches to a C++ bridge method (i.e. has `bridge` set). Local-only
 *  actions (those with a custom `handler`) are excluded. */
export function enumerateBridgeActions(): Array<{
  tool: string;
  action: string;
  bridge: string;
}> {
  const out: Array<{ tool: string; action: string; bridge: string }> = [];
  for (const t of ALL_TOOLS) {
    for (const [action, spec] of Object.entries(t.actions)) {
      if (spec.bridge) out.push({ tool: t.name, action, bridge: spec.bridge });
    }
  }
  return out;
}
