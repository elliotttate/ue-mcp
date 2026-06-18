/**
 * The indexing orchestrator. Ties together the walker, chunker, manifest,
 * embedding provider, and vector store into an incremental build:
 *
 *   walk → diff against manifest → evict stale → chunk → embed → upsert → save
 *
 * A provider change or catalogue bump forces a clean rebuild. Asset/blueprint
 * extraction goes through the editor bridge; when the bridge is down those
 * files are skipped (not recorded), so they get picked up automatically on a
 * later build once the editor is connected.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import type { IBridge } from "../bridge.js";
import type { IntelligenceConfig } from "./config.js";
import type { Chunk, VectorEntry, ManifestEntry } from "./types.js";
import { createEmbeddingProvider } from "./providers.js";
import { VectorStore } from "./vector-store.js";
import { walkProject, type FileRecord } from "./walker.js";
import { chunkText, chunkBlob } from "./chunker.js";
import { extractBlueprintSummary, toGamePath } from "./bp-extract.js";
import { parseDocumentToMarkdown, collectDocuments } from "./documents.js";
import {
  loadManifest,
  saveManifest,
  emptyManifest,
  diffManifest,
  manifestEntryFor,
  hashFile,
  hashText,
  CATALOGUE_VERSION,
} from "./manifest.js";
import { manifestPath, vectorStorePath, indexDir, ensureDir } from "./paths.js";
import { withProjectLock } from "./locks.js";

const DEFAULT_MAX_FILE_SIZE = 1024 * 1024; // 1 MiB
const EMBED_GROUP = 256;

export interface IndexBuildOptions {
  rebuild?: boolean;
  includeAssets?: boolean;
  onProgress?: (msg: string) => void;
}

export interface IngestResult {
  ok: boolean;
  ingested: string[];
  chunks: number;
  failures: Array<{ file: string; reason: string }>;
}

export interface IndexStats {
  provider: string;
  dim: number;
  sources: number;
  chunks: number;
  addedFiles: number;
  changedFiles: number;
  removedFiles: number;
  skippedAssets: number;
  assetsIndexed: number;
  rebuilt: boolean;
  durationMs: number;
}

const LANG: Record<string, string> = {
  ".cpp": "cpp", ".h": "cpp", ".hpp": "cpp", ".c": "c", ".cc": "cpp", ".cxx": "cpp",
  ".inl": "cpp", ".cs": "csharp", ".py": "python", ".usf": "hlsl", ".ush": "hlsl",
  ".ini": "ini", ".json": "json", ".md": "markdown", ".txt": "text",
};

function readText(absPath: string): string | null {
  try {
    const buf = fs.readFileSync(absPath);
    // Reject obvious binaries (NUL bytes in the head).
    const head = buf.subarray(0, Math.min(buf.length, 8000));
    if (head.includes(0)) return null;
    return buf.toString("utf-8");
  } catch {
    return null;
  }
}

interface PendingSource {
  rec: FileRecord;
  hash: string;
  chunks: Chunk[];
  /** Blueprint dependency ids, cached into the manifest for the graph. */
  dependencies?: string[];
}

export class Indexer {
  constructor(
    private readonly projectDir: string,
    private readonly projectName: string | null,
    private readonly bridge: IBridge,
    private readonly cfg: IntelligenceConfig,
  ) {}

  /**
   * Parse and index an external document (or every supported document under a
   * folder) into the same vector store, under synthetic `ingested/<name>`
   * sources. Reuses the configured embedding provider; refuses if it would mix
   * vectors from two different providers.
   */
  async ingest(target: string): Promise<IngestResult> {
    return withProjectLock(`index:${this.projectDir}`, () => this.ingestInternal(target));
  }

  private async ingestInternal(target: string): Promise<IngestResult> {
    const provider = createEmbeddingProvider(this.cfg.embedding);
    const providerKey = provider.key;
    const storeFile = vectorStorePath(this.projectDir);
    const manifestFile = manifestPath(this.projectDir);

    let store = VectorStore.load(storeFile);
    if (store && store.provider !== providerKey) {
      throw new Error(
        `Index was built with provider '${store.provider}' but config now uses '${providerKey}'. Run index(build, rebuild=true) first.`,
      );
    }
    const manifest = loadManifest(manifestFile) ?? emptyManifest(providerKey);

    const abs = path.isAbsolute(target) ? target : path.join(this.projectDir, target);
    if (!fs.existsSync(abs)) throw new Error(`Path not found: ${abs}`);
    const docs = fs.statSync(abs).isDirectory() ? collectDocuments(abs) : [abs];

    const ingested: string[] = [];
    const failures: Array<{ file: string; reason: string }> = [];
    const allChunks: Chunk[] = [];
    const recorded: Array<{ sourceKey: string; chunks: Chunk[]; hash: string; size: number }> = [];

    for (const file of docs) {
      try {
        const parsed = await parseDocumentToMarkdown(file);
        const sourceKey = "ingested/" + path.basename(file);
        const chunks = chunkBlob(sourceKey, parsed.text, "document", path.basename(file));
        const prev = manifest.files[sourceKey];
        if (store && prev) store.remove(prev.chunkIds);
        recorded.push({ sourceKey, chunks, hash: hashText(parsed.text), size: parsed.text.length });
        allChunks.push(...chunks);
        ingested.push(sourceKey);
      } catch (e) {
        failures.push({ file, reason: e instanceof Error ? e.message : String(e) });
      }
    }

    if (allChunks.length > 0) {
      const vectors: Float32Array[] = [];
      for (let i = 0; i < allChunks.length; i += EMBED_GROUP) {
        const vs = await provider.embed(allChunks.slice(i, i + EMBED_GROUP).map((c) => c.text));
        vectors.push(...vs);
      }
      if (!store) store = new VectorStore(vectors[0].length, providerKey);
      store.upsert(
        allChunks.map((c, idx) => ({
          id: c.id,
          vector: vectors[idx],
          meta: {
            id: c.id,
            source: c.source,
            kind: c.kind,
            startLine: c.startLine,
            endLine: c.endLine,
            symbol: c.symbol,
            language: c.language,
            text: c.text,
          },
        })),
      );
    }

    for (const r of recorded) {
      const entry: ManifestEntry = {
        source: r.sourceKey,
        hash: r.hash,
        mtime: Date.now(),
        size: r.size,
        kind: "document",
        chunkIds: r.chunks.map((c) => c.id),
      };
      manifest.files[r.sourceKey] = entry;
    }

    ensureDir(indexDir(this.projectDir));
    if (!store) store = new VectorStore(provider.dim, providerKey);
    store.save(storeFile);
    saveManifest(manifestFile, manifest);

    return { ok: true, ingested, chunks: allChunks.length, failures };
  }

  clear(): void {
    const dir = indexDir(this.projectDir);
    for (const f of ["vectors.jsonl", "manifest.json", "graph.json"]) {
      try {
        fs.rmSync(`${dir}/${f}`, { force: true });
      } catch {
        /* ignore */
      }
    }
  }

  /** Build/refresh the index. Serialized per project so two concurrent builds
   *  cannot corrupt the shared store and manifest. */
  async build(opts: IndexBuildOptions = {}): Promise<IndexStats> {
    return withProjectLock(`index:${this.projectDir}`, () => this.buildInternal(opts));
  }

  private async buildInternal(opts: IndexBuildOptions = {}): Promise<IndexStats> {
    const startedAt = Date.now();
    const log = opts.onProgress ?? (() => {});
    const provider = createEmbeddingProvider(this.cfg.embedding);
    const providerKey = provider.key;
    const maxFileSize = this.cfg.maxFileSize ?? DEFAULT_MAX_FILE_SIZE;
    const includeAssets = (opts.includeAssets ?? true) && this.bridge.isConnected;

    const manifestFile = manifestPath(this.projectDir);
    const storeFile = vectorStorePath(this.projectDir);

    let manifest = loadManifest(manifestFile);
    let store = VectorStore.load(storeFile);

    const mismatch =
      !manifest ||
      !store ||
      manifest.provider !== providerKey ||
      manifest.catalogue !== CATALOGUE_VERSION ||
      store.provider !== providerKey;
    const fullRebuild = !!opts.rebuild || mismatch;

    if (fullRebuild) {
      manifest = emptyManifest(providerKey);
      store = null; // recreated after first embeddings so we capture the true dim
      log("full rebuild");
    }
    const activeManifest = manifest!;

    log("walking project");
    const files = walkProject(this.projectDir, {
      includeAssets,
      maxFileSize,
      ignore: this.cfg.ignore ?? [],
    });
    const diff = diffManifest(activeManifest, files, fullRebuild);
    log(`${diff.added.length} added, ${diff.changed.length} changed, ${diff.removed.length} removed`);

    // Evict removed sources. When the editor is offline we did not scan assets,
    // so a previously-indexed blueprint is "missing" only because we skipped it —
    // do NOT treat it as removed, or every offline build would wipe the BP index.
    const removed = includeAssets
      ? diff.removed
      : diff.removed.filter((src) => activeManifest.files[src]?.kind !== "blueprint");
    if (store) {
      for (const src of removed) {
        const e = activeManifest.files[src];
        if (e) store.remove(e.chunkIds);
        delete activeManifest.files[src];
      }
    }
    // NB: changed sources are evicted later, only once their replacement chunks
    // exist, so a transient extraction failure can never drop an asset.

    // Build chunks for everything new or changed.
    const pending: PendingSource[] = [];
    const allChunks: Chunk[] = [];
    let skippedAssets = 0;
    let assetsIndexed = 0;

    for (const rec of [...diff.added, ...diff.changed]) {
      let chunks: Chunk[] = [];
      let dependencies: string[] | undefined;
      if (rec.kind === "blueprint") {
        const gamePath = toGamePath(rec.relPath, this.projectName);
        const summary = await extractBlueprintSummary(this.bridge, gamePath);
        if (!summary) {
          skippedAssets++;
          continue; // not recorded → retried on a later build when bridge is up
        }
        chunks = chunkBlob(gamePath, summary.text, "blueprint", summary.symbol);
        dependencies = summary.dependencies;
        assetsIndexed++;
      } else {
        const text = readText(rec.absPath);
        if (text === null) continue;
        chunks = chunkText(rec.relPath, text, rec.kind, LANG[rec.ext]);
      }
      if (chunks.length === 0) continue;
      pending.push({ rec, hash: diff.hashes.get(rec.relPath) ?? hashFile(rec), chunks, dependencies });
      allChunks.push(...chunks);
    }

    // Now that replacements exist, evict the prior chunks of each changed source.
    // (Chunk ids are reused per source, so evict before the upsert below.)
    if (store) {
      for (const p of pending) {
        const prev = activeManifest.files[p.rec.relPath];
        if (prev) store.remove(prev.chunkIds);
      }
    }

    // Embed in groups for bounded memory + progress.
    if (allChunks.length > 0) {
      log(`embedding ${allChunks.length} chunks via ${providerKey}`);
      const vectors: Float32Array[] = [];
      for (let i = 0; i < allChunks.length; i += EMBED_GROUP) {
        const group = allChunks.slice(i, i + EMBED_GROUP);
        const vs = await provider.embed(group.map((c) => c.text));
        vectors.push(...vs);
        log(`embedded ${Math.min(i + EMBED_GROUP, allChunks.length)}/${allChunks.length}`);
      }
      if (!store) store = new VectorStore(vectors[0].length, providerKey);
      const entries: VectorEntry[] = allChunks.map((c, idx) => ({
        id: c.id,
        vector: vectors[idx],
        meta: {
          id: c.id,
          source: c.source,
          kind: c.kind,
          startLine: c.startLine,
          endLine: c.endLine,
          symbol: c.symbol,
          language: c.language,
          text: c.text,
        },
      }));
      store.upsert(entries);
    }

    // Record manifest entries for everything we indexed this pass.
    for (const p of pending) {
      activeManifest.files[p.rec.relPath] = manifestEntryFor(
        p.rec,
        p.hash,
        p.chunks.map((c) => c.id),
        p.dependencies,
      );
    }

    // Persist (create an empty store for empty projects so status works).
    ensureDir(indexDir(this.projectDir));
    if (!store) store = new VectorStore(provider.dim, providerKey);
    store.save(storeFile);
    saveManifest(manifestFile, activeManifest);

    return {
      provider: providerKey,
      dim: store.dim,
      sources: Object.keys(activeManifest.files).length,
      chunks: store.size,
      addedFiles: diff.added.length,
      changedFiles: diff.changed.length,
      removedFiles: diff.removed.length,
      skippedAssets,
      assetsIndexed,
      rebuilt: fullRebuild,
      durationMs: Date.now() - startedAt,
    };
  }
}

/** Load an existing index for querying. Returns null when nothing is built. */
export function loadIndex(
  projectDir: string,
  cfg: IntelligenceConfig,
): { store: VectorStore; provider: ReturnType<typeof createEmbeddingProvider> } | null {
  const store = VectorStore.load(vectorStorePath(projectDir));
  if (!store) return null;
  const provider = createEmbeddingProvider(cfg.embedding);
  return { store, provider };
}

/** Read-only status without touching the editor or rebuilding. */
export function indexStatus(projectDir: string): {
  built: boolean;
  provider?: string;
  dim?: number;
  sources?: number;
  chunks?: number;
  updatedAt?: string;
} {
  const manifest = loadManifest(manifestPath(projectDir));
  const store = VectorStore.load(vectorStorePath(projectDir));
  if (!manifest || !store) return { built: false };
  return {
    built: true,
    provider: store.provider,
    dim: store.dim,
    sources: Object.keys(manifest.files).length,
    chunks: store.size,
    updatedAt: manifest.updatedAt,
  };
}
