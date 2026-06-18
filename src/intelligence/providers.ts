/**
 * Pluggable text-embedding providers. The default ("local") needs no network,
 * no API key, and no native modules — it is a hashing-trick bag-of-subwords
 * embedding that gives genuinely useful lexical/semantic retrieval out of the
 * box and upgrades to a real model when one is configured.
 *
 * API providers (OpenAI, Voyage, Cohere, Ollama) are reached via the global
 * fetch — no SDKs. Keys come from environment variables, never project config.
 */
import { warn } from "../log.js";
import type { EmbeddingProvider } from "./types.js";
import type { EmbeddingConfig } from "./config.js";
import { DEFAULT_KEY_ENV } from "./config.js";
import { fnv1a, tokenize, trigrams } from "./text.js";
import { addHashed, l2normalize } from "./vec.js";

const LOCAL_DIM_DEFAULT = 512;

/** Zero-dependency local embedding. Deterministic, offline, instant. */
export class LocalHashingEmbedding implements EmbeddingProvider {
  readonly name = "local";
  readonly key: string;
  readonly dim: number;

  constructor(dim = LOCAL_DIM_DEFAULT) {
    this.dim = dim;
    this.key = `local:hash-${dim}`;
  }

  async embed(texts: string[]): Promise<Float32Array[]> {
    return texts.map((t) => this.embedOne(t));
  }

  private embedOne(text: string): Float32Array {
    const v = new Float32Array(this.dim);
    const tokens = tokenize(text);
    for (const tok of tokens) {
      addHashed(v, tok, 1, this.dim, fnv1a);
      for (const g of trigrams(tok)) addHashed(v, "#" + g, 0.4, this.dim, fnv1a);
    }
    l2normalize(v);
    return v;
  }
}

interface HttpEmbeddingOpts {
  name: string;
  model: string;
  dimHint: number;
  endpoint: string;
  apiKey?: string;
  /** Build the request body for a batch. */
  body: (texts: string[], model: string) => unknown;
  /** Extract number[][] from the parsed response. */
  parse: (json: unknown) => number[][];
  /** Extra headers (e.g. Authorization). */
  headers: Record<string, string>;
  /** Max texts per request (provider batch limits). */
  batchSize: number;
}

/** Base for HTTP-backed providers. Batches, normalizes, and adapts to the
 *  model's true dimensionality from the first response. */
class HttpEmbedding implements EmbeddingProvider {
  readonly name: string;
  readonly key: string;
  private _dim: number;
  constructor(private readonly o: HttpEmbeddingOpts) {
    this.name = o.name;
    this.key = `${o.name}:${o.model}`;
    this._dim = o.dimHint;
  }
  get dim(): number {
    return this._dim;
  }

  async embed(texts: string[]): Promise<Float32Array[]> {
    const out: Float32Array[] = [];
    for (let i = 0; i < texts.length; i += this.o.batchSize) {
      const batch = texts.slice(i, i + this.o.batchSize);
      const arrs = await this.embedBatch(batch);
      for (const a of arrs) {
        if (a.length) this._dim = a.length;
        const v = Float32Array.from(a);
        l2normalize(v);
        out.push(v);
      }
    }
    return out;
  }

  private async embedBatch(texts: string[]): Promise<number[][]> {
    const res = await fetch(this.o.endpoint, {
      method: "POST",
      headers: { "content-type": "application/json", ...this.o.headers },
      body: JSON.stringify(this.o.body(texts, this.o.model)),
    });
    if (!res.ok) {
      const detail = await res.text().catch(() => "");
      throw new Error(`${this.name} embeddings HTTP ${res.status}: ${detail.slice(0, 300)}`);
    }
    const json = (await res.json()) as unknown;
    const arrs = this.o.parse(json);
    if (arrs.length !== texts.length) {
      throw new Error(`${this.name} returned ${arrs.length} embeddings for ${texts.length} inputs`);
    }
    return arrs;
  }
}

function openai(model: string, apiKey: string, baseUrl?: string): EmbeddingProvider {
  const base = (baseUrl ?? "https://api.openai.com/v1").replace(/\/$/, "");
  return new HttpEmbedding({
    name: "openai",
    model,
    dimHint: model.includes("large") ? 3072 : 1536,
    endpoint: `${base}/embeddings`,
    headers: { authorization: `Bearer ${apiKey}` },
    batchSize: 128,
    body: (texts, m) => ({ model: m, input: texts }),
    parse: (json) => {
      const j = json as { data?: Array<{ embedding: number[] }> };
      return (j.data ?? []).map((d) => d.embedding);
    },
  });
}

function voyage(model: string, apiKey: string, baseUrl?: string): EmbeddingProvider {
  const base = (baseUrl ?? "https://api.voyageai.com/v1").replace(/\/$/, "");
  return new HttpEmbedding({
    name: "voyage",
    model,
    dimHint: model.includes("lite") ? 512 : 1024,
    endpoint: `${base}/embeddings`,
    headers: { authorization: `Bearer ${apiKey}` },
    batchSize: 128,
    body: (texts, m) => ({ model: m, input: texts }),
    parse: (json) => {
      const j = json as { data?: Array<{ embedding: number[] }> };
      return (j.data ?? []).map((d) => d.embedding);
    },
  });
}

function cohere(model: string, apiKey: string, baseUrl?: string): EmbeddingProvider {
  const base = (baseUrl ?? "https://api.cohere.com/v2").replace(/\/$/, "");
  return new HttpEmbedding({
    name: "cohere",
    model,
    dimHint: 1024,
    endpoint: `${base}/embed`,
    headers: { authorization: `Bearer ${apiKey}` },
    batchSize: 96,
    body: (texts, m) => ({ model: m, texts, input_type: "search_document", embedding_types: ["float"] }),
    parse: (json) => {
      const j = json as { embeddings?: { float?: number[][] } | number[][] };
      const e = j.embeddings;
      if (Array.isArray(e)) return e;
      return e?.float ?? [];
    },
  });
}

function ollama(model: string, baseUrl?: string): EmbeddingProvider {
  const base = (baseUrl ?? "http://localhost:11434").replace(/\/$/, "");
  return new HttpEmbedding({
    name: "ollama",
    model,
    dimHint: 768,
    endpoint: `${base}/api/embed`,
    headers: {},
    batchSize: 64,
    body: (texts, m) => ({ model: m, input: texts }),
    parse: (json) => {
      const j = json as { embeddings?: number[][] };
      return j.embeddings ?? [];
    },
  });
}

const DEFAULT_MODELS: Record<string, string> = {
  openai: "text-embedding-3-small",
  voyage: "voyage-3",
  cohere: "embed-english-v3.0",
  ollama: "nomic-embed-text",
};

/**
 * Build an embedding provider from config + environment. Falls back to the
 * local provider (with a warning) whenever a selected API provider is missing
 * its key, so indexing never hard-fails on a misconfiguration.
 */
export function createEmbeddingProvider(
  cfg: EmbeddingConfig | undefined,
  env: NodeJS.ProcessEnv = process.env,
): EmbeddingProvider {
  const provider = cfg?.provider ?? "local";
  if (provider === "local") return new LocalHashingEmbedding(cfg?.dim);

  const model = cfg?.model ?? DEFAULT_MODELS[provider] ?? "default";

  if (provider === "ollama") {
    return ollama(model, cfg?.baseUrl);
  }

  const keyEnv = cfg?.apiKeyEnv ?? DEFAULT_KEY_ENV[provider];
  const apiKey = keyEnv ? env[keyEnv] : undefined;
  if (!apiKey) {
    warn(
      "intel",
      `embedding provider '${provider}' selected but ${keyEnv} is not set — falling back to local hashing embedding`,
    );
    return new LocalHashingEmbedding(cfg?.dim);
  }

  switch (provider) {
    case "openai":
      return openai(model, apiKey, cfg?.baseUrl);
    case "voyage":
      return voyage(model, apiKey, cfg?.baseUrl);
    case "cohere":
      return cohere(model, apiKey, cfg?.baseUrl);
    default:
      return new LocalHashingEmbedding(cfg?.dim);
  }
}
