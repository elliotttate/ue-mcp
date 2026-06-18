/**
 * Pluggable text-to-image generation. Like the embedding layer, this is
 * model-agnostic: OpenAI Images, Stability, and Replicate are all reachable via
 * the global fetch with keys read from the environment. Generated PNGs are
 * written under `.ue-mcp/generated-images/`; importing them into the project as
 * a texture is a separate step that goes through the existing asset bridge.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import type { ImageConfig } from "./config.js";
import { DEFAULT_KEY_ENV } from "./config.js";
import { httpFetch } from "./http.js";
import { fnv1a } from "./text.js";

const GEN_TIMEOUT_MS = 120_000;
import { generatedImageDir, ensureDir } from "./paths.js";

export interface ImageGenResult {
  file: string;
  provider: string;
  model: string;
  bytes: number;
}

function outFile(projectDir: string, prompt: string): string {
  const slug = prompt.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40) || "image";
  const dir = generatedImageDir(projectDir);
  ensureDir(dir);
  return path.join(dir, `${slug}-${fnv1a(prompt + String(fs.readdirSync(dir).length)).toString(16)}.png`);
}

function keyFor(cfg: ImageConfig, provider: string, env: NodeJS.ProcessEnv): string | undefined {
  const keyEnv = cfg.apiKeyEnv ?? DEFAULT_KEY_ENV[provider] ?? (provider === "openai" ? "OPENAI_API_KEY" : "");
  return keyEnv ? env[keyEnv] : undefined;
}

export async function generateImage(
  projectDir: string,
  cfg: ImageConfig,
  prompt: string,
  opts: { size?: string } = {},
  env: NodeJS.ProcessEnv = process.env,
): Promise<ImageGenResult> {
  const provider = cfg.provider ?? "openai";
  const size = opts.size ?? "1024x1024";
  const file = outFile(projectDir, prompt);

  if (provider === "openai") {
    const model = cfg.model ?? "gpt-image-1";
    const apiKey = keyFor(cfg, "openai", env);
    if (!apiKey) throw new Error("OpenAI image generation requires OPENAI_API_KEY (or config apiKeyEnv).");
    const base = (cfg.baseUrl ?? "https://api.openai.com/v1").replace(/\/$/, "");
    const body: Record<string, unknown> = { model, prompt, size, n: 1 };
    if (model.startsWith("dall-e")) body.response_format = "b64_json";
    const res = await httpFetch(
      `${base}/images/generations`,
      {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${apiKey}` },
        body: JSON.stringify(body),
      },
      { timeoutMs: GEN_TIMEOUT_MS },
    );
    if (!res.ok) throw new Error(`OpenAI images HTTP ${res.status}: ${(await res.text()).slice(0, 300)}`);
    const json = (await res.json()) as { data?: Array<{ b64_json?: string; url?: string }> };
    const item = json.data?.[0];
    const buf = item?.b64_json
      ? Buffer.from(item.b64_json, "base64")
      : item?.url
        ? Buffer.from(await (await httpFetch(item.url, {}, { retries: 2 })).arrayBuffer())
        : null;
    if (!buf) throw new Error("OpenAI returned no image data.");
    fs.writeFileSync(file, buf);
    return { file, provider, model, bytes: buf.length };
  }

  if (provider === "stability") {
    const model = cfg.model ?? "core";
    const apiKey = keyFor(cfg, "stability", env);
    if (!apiKey) throw new Error("Stability image generation requires STABILITY_API_KEY (or config apiKeyEnv).");
    const base = (cfg.baseUrl ?? "https://api.stability.ai/v2beta/stable-image/generate").replace(/\/$/, "");
    const form = new FormData();
    form.append("prompt", prompt);
    form.append("output_format", "png");
    const res = await httpFetch(
      `${base}/${model}`,
      {
        method: "POST",
        headers: { authorization: `Bearer ${apiKey}`, accept: "image/*" },
        body: form,
      },
      { timeoutMs: GEN_TIMEOUT_MS },
    );
    if (!res.ok) throw new Error(`Stability HTTP ${res.status}: ${(await res.text()).slice(0, 300)}`);
    const buf = Buffer.from(await res.arrayBuffer());
    fs.writeFileSync(file, buf);
    return { file, provider, model, bytes: buf.length };
  }

  if (provider === "replicate") {
    const model = cfg.model ?? "black-forest-labs/flux-schnell";
    const apiKey = keyFor(cfg, "replicate", env);
    if (!apiKey) throw new Error("Replicate image generation requires REPLICATE_API_TOKEN (or config apiKeyEnv).");
    const buf = await replicateGenerate(model, prompt, apiKey);
    fs.writeFileSync(file, buf);
    return { file, provider, model, bytes: buf.length };
  }

  throw new Error(`Unknown image provider: ${provider}`);
}

async function replicateGenerate(model: string, prompt: string, apiKey: string): Promise<Buffer> {
  const start = await httpFetch(
    `https://api.replicate.com/v1/models/${model}/predictions`,
    {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${apiKey}`, Prefer: "wait" },
      body: JSON.stringify({ input: { prompt } }),
    },
    { timeoutMs: GEN_TIMEOUT_MS },
  );
  if (!start.ok) throw new Error(`Replicate HTTP ${start.status}: ${(await start.text()).slice(0, 300)}`);
  let pred = (await start.json()) as { status?: string; output?: unknown; urls?: { get?: string } };

  // Poll if not already finished (Prefer: wait usually resolves synchronously).
  let tries = 0;
  while (pred.status && pred.status !== "succeeded" && pred.status !== "failed" && tries < 60) {
    await new Promise((r) => setTimeout(r, 1500));
    const getUrl = pred.urls?.get;
    if (!getUrl) break;
    const poll = await httpFetch(getUrl, { headers: { authorization: `Bearer ${apiKey}` } }, { timeoutMs: 30_000 });
    pred = (await poll.json()) as typeof pred;
    tries++;
  }
  if (pred.status === "failed") throw new Error("Replicate prediction failed.");
  const out = pred.output;
  const url = Array.isArray(out) ? (out[0] as string) : typeof out === "string" ? out : null;
  if (!url) throw new Error("Replicate returned no output URL.");
  return Buffer.from(await (await httpFetch(url, {}, { retries: 2 })).arrayBuffer());
}

export function listGeneratedImages(projectDir: string): Array<{ file: string; bytes: number }> {
  const dir = generatedImageDir(projectDir);
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((f) => f.endsWith(".png"))
    .map((f) => {
      const full = path.join(dir, f);
      return { file: full, bytes: fs.statSync(full).size };
    });
}
