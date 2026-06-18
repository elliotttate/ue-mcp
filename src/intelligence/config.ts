/**
 * Configuration shape for the Project Intelligence layer. The same shape is
 * mirrored as a zod schema in schemas.ts (validated when reading ue-mcp.yml).
 * Kept here as the canonical TS type so both project.ts and the engine modules
 * can import it without depending on zod.
 *
 * Design constraint: everything is optional and defaults to a fully local,
 * zero-setup configuration. API providers are opt-in and read their keys from
 * environment variables (never stored in project config).
 */

export type EmbeddingProviderName = "local" | "openai" | "voyage" | "cohere" | "ollama";
export type ImageProviderName = "openai" | "stability" | "replicate";
export type SummarizerProviderName = "none" | "openai" | "ollama";

export interface EmbeddingConfig {
  /** Default "local" — a zero-dependency hashing embedding (no network). */
  provider?: EmbeddingProviderName;
  model?: string;
  /** Local provider dimensionality (ignored by API providers). Default 512. */
  dim?: number;
  /** Base URL override (for Ollama or OpenAI-compatible gateways). */
  baseUrl?: string;
  /** Env var holding the API key. Defaults per provider. */
  apiKeyEnv?: string;
}

export interface ImageConfig {
  provider?: ImageProviderName;
  model?: string;
  baseUrl?: string;
  apiKeyEnv?: string;
}

export interface SummarizerConfig {
  /** Default "none" — summaries are produced heuristically from the index. */
  provider?: SummarizerProviderName;
  model?: string;
  baseUrl?: string;
  apiKeyEnv?: string;
}

export interface IntelligenceConfig {
  embedding?: EmbeddingConfig;
  image?: ImageConfig;
  summarizer?: SummarizerConfig;
  /** Extra ignore globs for indexing, merged with the built-in UE excludes. */
  ignore?: string[];
  /** Also honor the project's .gitignore when indexing (default false). */
  respectGitignore?: boolean;
  /** Max file size (bytes) to index. Default 1 MiB. */
  maxFileSize?: number;
  /** Watch the project and incrementally re-index on change (default false). */
  watch?: boolean;
}

/** Default env var names for each API provider's key. */
export const DEFAULT_KEY_ENV: Record<string, string> = {
  openai: "OPENAI_API_KEY",
  voyage: "VOYAGE_API_KEY",
  cohere: "COHERE_API_KEY",
  stability: "STABILITY_API_KEY",
  replicate: "REPLICATE_API_TOKEN",
};
