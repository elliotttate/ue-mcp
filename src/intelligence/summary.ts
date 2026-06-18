/**
 * Project summary: a structural digest of the indexed project plus an OPTIONAL
 * natural-language narration. The digest (counts by kind/language, top
 * directories, dependency hubs, README excerpt) is always produced locally with
 * no model. If a summarizer is configured (OpenAI- or Ollama-compatible chat),
 * it narrates the digest into prose; otherwise the digest is returned for the
 * driving agent to narrate — keeping the layer model-agnostic.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import type { IntelligenceConfig, SummarizerConfig } from "./config.js";
import { loadManifest } from "./manifest.js";
import { manifestPath, vectorStorePath } from "./paths.js";
import { VectorStore } from "./vector-store.js";
import { loadGraph, topHubs } from "./knowledge-graph.js";
import { estimateTokens } from "./tokens.js";

export interface ProjectDigest {
  project: string | null;
  indexed: boolean;
  sources: number;
  chunks: number;
  byKind: Record<string, number>;
  byLanguage: Record<string, number>;
  topDirectories: Array<{ dir: string; files: number }>;
  topHubs: Array<{ id: string; kind: string; inDegree: number }>;
  approxIndexTokens?: number;
  readmeExcerpt?: string;
}

function findReadme(projectDir: string): string | undefined {
  try {
    const entry = fs.readdirSync(projectDir).find((f) => /^readme(\.md|\.txt)?$/i.test(f));
    if (!entry) return undefined;
    return fs.readFileSync(path.join(projectDir, entry), "utf-8").slice(0, 800);
  } catch {
    return undefined;
  }
}

export function buildProjectDigest(projectDir: string, projectName: string | null): ProjectDigest {
  const manifest = loadManifest(manifestPath(projectDir));
  const store = VectorStore.load(vectorStorePath(projectDir));

  const byKind: Record<string, number> = {};
  const byLanguage: Record<string, number> = {};
  const dirCounts = new Map<string, number>();

  if (manifest) {
    for (const [source, entry] of Object.entries(manifest.files)) {
      byKind[entry.kind] = (byKind[entry.kind] ?? 0) + 1;
      const ext = path.extname(source).toLowerCase();
      if (ext) byLanguage[ext] = (byLanguage[ext] ?? 0) + 1;
      const top = source.split("/")[0] || "(root)";
      dirCounts.set(top, (dirCounts.get(top) ?? 0) + 1);
    }
  }

  let approxIndexTokens: number | undefined;
  if (store) {
    let sum = 0;
    for (const e of store.values()) sum += estimateTokens(e.meta.text);
    approxIndexTokens = sum;
  }

  const graph = loadGraph(projectDir);
  const hubs = graph
    ? topHubs(graph, 10).map((h) => ({ id: h.node.id, kind: h.node.kind, inDegree: h.scores.inDegree }))
    : [];

  return {
    project: projectName,
    indexed: !!manifest && !!store,
    sources: manifest ? Object.keys(manifest.files).length : 0,
    chunks: store ? store.size : 0,
    byKind,
    byLanguage,
    topDirectories: [...dirCounts.entries()]
      .sort((a, b) => b[1] - a[1])
      .slice(0, 12)
      .map(([dir, files]) => ({ dir, files })),
    topHubs: hubs,
    approxIndexTokens,
    readmeExcerpt: findReadme(projectDir),
  };
}

function digestToText(d: ProjectDigest): string {
  const lines: string[] = [];
  lines.push(`Project: ${d.project ?? "(unknown)"}`);
  lines.push(`Indexed sources: ${d.sources}, chunks: ${d.chunks}`);
  lines.push(`By kind: ${Object.entries(d.byKind).map(([k, v]) => `${k}=${v}`).join(", ")}`);
  lines.push(`Languages: ${Object.entries(d.byLanguage).map(([k, v]) => `${k}=${v}`).join(", ")}`);
  lines.push(`Top directories: ${d.topDirectories.map((t) => `${t.dir}(${t.files})`).join(", ")}`);
  if (d.topHubs.length) {
    lines.push(`Most-depended-on: ${d.topHubs.map((h) => `${h.id} [${h.inDegree}]`).join(", ")}`);
  }
  if (d.readmeExcerpt) lines.push(`\nREADME excerpt:\n${d.readmeExcerpt}`);
  return lines.join("\n");
}

async function summarizeWithProvider(sc: SummarizerConfig, digestText: string, env: NodeJS.ProcessEnv): Promise<string> {
  const prompt =
    "Summarize what this Unreal Engine project is and how it is structured, in 4-8 sentences, " +
    "based on the digest below. Be concrete; do not invent details.\n\n" +
    digestText;

  if (sc.provider === "ollama") {
    const base = (sc.baseUrl ?? "http://localhost:11434").replace(/\/$/, "");
    const res = await fetch(`${base}/api/chat`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model: sc.model ?? "llama3", stream: false, messages: [{ role: "user", content: prompt }] }),
    });
    if (!res.ok) throw new Error(`Ollama HTTP ${res.status}: ${(await res.text()).slice(0, 200)}`);
    const json = (await res.json()) as { message?: { content?: string } };
    return json.message?.content?.trim() ?? "";
  }

  // OpenAI-compatible chat (default).
  const keyEnv = sc.apiKeyEnv ?? "OPENAI_API_KEY";
  const apiKey = env[keyEnv];
  if (!apiKey) throw new Error(`Summarizer provider '${sc.provider}' requires ${keyEnv}.`);
  const base = (sc.baseUrl ?? "https://api.openai.com/v1").replace(/\/$/, "");
  const res = await fetch(`${base}/chat/completions`, {
    method: "POST",
    headers: { "content-type": "application/json", authorization: `Bearer ${apiKey}` },
    body: JSON.stringify({
      model: sc.model ?? "gpt-4o-mini",
      messages: [
        { role: "system", content: "You summarize Unreal Engine projects concisely and accurately." },
        { role: "user", content: prompt },
      ],
    }),
  });
  if (!res.ok) throw new Error(`Summarizer HTTP ${res.status}: ${(await res.text()).slice(0, 200)}`);
  const json = (await res.json()) as { choices?: Array<{ message?: { content?: string } }> };
  return json.choices?.[0]?.message?.content?.trim() ?? "";
}

export async function generateProjectSummary(
  projectDir: string,
  projectName: string | null,
  cfg: IntelligenceConfig,
  env: NodeJS.ProcessEnv = process.env,
): Promise<{ digest: ProjectDigest; summary: string | null; summarizer?: string; note?: string }> {
  const digest = buildProjectDigest(projectDir, projectName);
  const sc = cfg.summarizer;
  if (!sc || !sc.provider || sc.provider === "none") {
    return {
      digest,
      summary: null,
      note: digest.indexed
        ? "No summarizer configured. Narrate this digest yourself, or set ue-mcp.intelligence.summarizer to generate prose."
        : "Project not indexed yet — run index(action=\"build\") first for a fuller digest.",
    };
  }
  const summary = await summarizeWithProvider(sc, digestToText(digest), env);
  return { digest, summary, summarizer: `${sc.provider}:${sc.model ?? "default"}` };
}
