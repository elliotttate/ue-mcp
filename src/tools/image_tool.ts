/**
 * `image` — text-to-image generation that lands in the project. Generate a PNG
 * from a prompt (model-agnostic provider), then import it as a UE texture via
 * the existing asset bridge. Keeps the heavy provider config out of the editor
 * and the import path on the tested asset handler.
 */
import { z } from "zod";
import { categoryTool, type ToolDef, type ToolContext } from "../types.js";
import { generateImage, listGeneratedImages } from "../intelligence/image-gen.js";
import type { ImageConfig } from "../intelligence/config.js";

function imgCfg(ctx: ToolContext): ImageConfig {
  const intel = (ctx.project.config.intelligence ?? {}) as { image?: ImageConfig };
  return intel.image ?? {};
}

export const imageTool: ToolDef = categoryTool(
  "image",
  "Generate images from text prompts (OpenAI / Stability / Replicate, configured locally) and import them into the project as textures.",
  {
    generate: {
      description: "Generate a PNG from a prompt and save it under .ue-mcp/generated-images/. Params: prompt, size?",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const prompt = String(p.prompt ?? "").trim();
        if (!prompt) throw new Error("image.generate requires a 'prompt'");
        const result = await generateImage(ctx.project.projectDir!, imgCfg(ctx), prompt, {
          size: typeof p.size === "string" ? p.size : undefined,
        });
        return { ok: true, ...result };
      },
    },
    import: {
      description: "Import an existing image file into the project as a texture. Params: file (absolute path), destPath (/Game/...), name?",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const file = String(p.file ?? "");
        if (!file) throw new Error("image.import requires a 'file' path");
        const destPath = typeof p.destPath === "string" ? p.destPath : "/Game/GeneratedImages";
        const result = await ctx.bridge.call("import_texture", {
          filename: file,
          destinationPath: destPath,
          assetName: typeof p.name === "string" ? p.name : undefined,
        });
        return { ok: true, imported: result };
      },
    },
    generate_and_import: {
      description: "Generate from a prompt and import the result as a texture in one call. Params: prompt, destPath (/Game/...), name?, size?",
      handler: async (ctx, p) => {
        ctx.project.ensureLoaded();
        const prompt = String(p.prompt ?? "").trim();
        if (!prompt) throw new Error("image.generate_and_import requires a 'prompt'");
        const gen = await generateImage(ctx.project.projectDir!, imgCfg(ctx), prompt, {
          size: typeof p.size === "string" ? p.size : undefined,
        });
        if (!ctx.bridge.isConnected) {
          return { ok: true, generated: gen, imported: null, note: "Image generated but editor not connected — run image(import) later." };
        }
        const imported = await ctx.bridge.call("import_texture", {
          filename: gen.file,
          destinationPath: typeof p.destPath === "string" ? p.destPath : "/Game/GeneratedImages",
          assetName: typeof p.name === "string" ? p.name : undefined,
        });
        return { ok: true, generated: gen, imported };
      },
    },
    list: {
      description: "List images generated for this project.",
      handler: async (ctx) => {
        ctx.project.ensureLoaded();
        return { images: listGeneratedImages(ctx.project.projectDir!) };
      },
    },
  },
  undefined,
  {
    prompt: z.string().optional().describe("Text prompt for image generation"),
    size: z.string().optional().describe("Image size, e.g. 1024x1024"),
    file: z.string().optional().describe("Image file to import"),
    destPath: z.string().optional().describe("/Game destination path for import"),
    name: z.string().optional().describe("Asset name for the imported texture"),
  },
);
