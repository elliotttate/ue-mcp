/**
 * Shared types for the Project Intelligence layer (indexing, retrieval,
 * knowledge graph, memory). Kept dependency-free so the engine modules are
 * unit-testable without the MCP server or a live editor.
 */

/** A pluggable text-embedding backend. dim is a hint; the store records the
 *  actual vector length produced at index time. */
export interface EmbeddingProvider {
  /** Stable key including model, e.g. "openai:text-embedding-3-small". Used to
   *  detect provider changes and force a rebuild. */
  readonly key: string;
  /** Human-facing short name, e.g. "local", "openai". */
  readonly name: string;
  /** Best-effort output dimensionality hint. */
  readonly dim: number;
  /** Embed a batch of texts into L2-normalized vectors. */
  embed(texts: string[]): Promise<Float32Array[]>;
}

export type ChunkKind = "code" | "blueprint" | "asset" | "document" | "config" | "text";

export interface ChunkMeta {
  /** Stable id: `${source}#${ordinal}`. */
  id: string;
  /** Project-relative file path, or a /Game/... asset path for blueprints. */
  source: string;
  kind: ChunkKind;
  startLine?: number;
  endLine?: number;
  /** Symbol / heading / blueprint name, when known. */
  symbol?: string;
  language?: string;
}

export interface Chunk extends ChunkMeta {
  text: string;
}

/** A stored vector plus the chunk metadata and snippet needed to render a hit
 *  without re-reading the source file. */
export interface VectorEntry {
  id: string;
  vector: Float32Array;
  meta: ChunkMeta & { text: string };
}

export interface SearchHit {
  id: string;
  score: number;
  source: string;
  kind: ChunkKind;
  startLine?: number;
  endLine?: number;
  symbol?: string;
  snippet: string;
}

/** Per-file record in the index manifest, for incremental change detection. */
export interface ManifestEntry {
  /** Project-relative path or asset path. */
  source: string;
  /** Content hash (and, for assets, a fingerprint of the extracted summary). */
  hash: string;
  /** mtimeMs at last index. */
  mtime: number;
  size: number;
  kind: ChunkKind;
  /** Chunk ids produced from this source (so we can evict on change/delete). */
  chunkIds: string[];
  /** For blueprint/asset sources: cached dependency ids from extraction, so the
   *  knowledge graph can be (re)built without re-querying the editor. */
  dependencies?: string[];
}

export interface IndexManifest {
  /** Schema version; a bump forces a full rebuild. */
  catalogue: number;
  /** Provider key the vectors were built with. A change forces a rebuild. */
  provider: string;
  updatedAt: string;
  files: Record<string, ManifestEntry>;
}
