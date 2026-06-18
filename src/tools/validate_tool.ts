/**
 * `validate` — check AI-written Unreal identifiers, code, and blueprint plans
 * against the live reflection database before acting on them. Fully local
 * (uses the editor bridge, no cloud). Reports unknown identifiers so the agent
 * can correct hallucinations early. Advisory by design: "unverified" means
 * "could not confirm", not "wrong".
 */
import { z } from "zod";
import { categoryTool, type ToolDef } from "../types.js";
import {
  extractIdentifiers,
  validateClassNames,
  validateNodeTypes,
  collectPlanNames,
  summarizeVerdicts,
} from "../intelligence/validation.js";

function toList(v: unknown): string[] {
  if (Array.isArray(v)) return v.map(String);
  if (typeof v === "string") return v.split(/[\s,]+/).filter(Boolean);
  return [];
}

export const validateTool: ToolDef = categoryTool(
  "validate",
  "Validate Unreal identifiers, code, and blueprint plans against the live reflection database (local, no cloud). Requires the editor connected to verify; otherwise reports 'unverified'.",
  {
    unreal_code: {
      description: "Extract UE identifiers from a code snippet and check each against reflection. Params: code",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const code = String(p.code ?? "");
        if (!code) throw new Error("validate.unreal_code requires 'code'");
        const ids = extractIdentifiers(code);
        const verdicts = await validateClassNames(ctx.bridge, ids);
        return { checked: ids.length, summary: summarizeVerdicts(verdicts), verdicts };
      },
    },
    identifiers: {
      description: "Validate a list of class/type identifiers. Params: identifiers (array or comma/space-separated string)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const ids = toList(p.identifiers);
        if (ids.length === 0) throw new Error("validate.identifiers requires 'identifiers'");
        const verdicts = await validateClassNames(ctx.bridge, ids);
        return { checked: ids.length, summary: summarizeVerdicts(verdicts), verdicts };
      },
    },
    blueprint_plan: {
      description: "Validate node/function/class names referenced by a blueprint plan. Params: plan (object or JSON string)",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        let plan: unknown = p.plan;
        if (typeof plan === "string") {
          try {
            plan = JSON.parse(plan);
          } catch {
            throw new Error("validate.blueprint_plan: 'plan' was a string but not valid JSON");
          }
        }
        if (!plan || typeof plan !== "object") throw new Error("validate.blueprint_plan requires a 'plan' object");
        const names = [...collectPlanNames(plan)];
        const verdicts = await validateNodeTypes(ctx.bridge, names);
        return { checked: names.length, names, summary: summarizeVerdicts(verdicts), verdicts };
      },
    },
  },
  undefined,
  {
    code: z.string().optional().describe("Code snippet to validate"),
    identifiers: z.union([z.array(z.string()), z.string()]).optional().describe("Identifiers to validate"),
    plan: z.union([z.record(z.unknown()), z.string()]).optional().describe("Blueprint plan object or JSON string"),
  },
);
