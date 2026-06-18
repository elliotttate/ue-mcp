/**
 * Incremental index manifest. Records, per source, a content hash and the
 * chunk ids it produced, so re-indexing only touches files that actually
 * changed and can evict stale chunks from the vector store. A catalogue-version
 * or provider change invalidates the whole manifest (forcing a clean rebuild).
 */
import * as fs from "node:fs";
import * as path from "node:path";
import * as crypto from "node:crypto";
import type { IndexManifest, ManifestEntry } from "./types.js";
import type { FileRecord } from "./walker.js";

/** Bump to force every project to fully re-index on next build. */
export const CATALOGUE_VERSION = 1;

export function emptyManifest(provider: string): IndexManifest {
  return { catalogue: CATALOGUE_VERSION, provider, updatedAt: new Date().toISOString(), files: {} };
}

export function loadManifest(filePath: string): IndexManifest | null {
  if (!fs.existsSync(filePath)) return null;
  try {
    const m = JSON.parse(fs.readFileSync(filePath, "utf-8")) as IndexManifest;
    if (typeof m.catalogue !== "number" || typeof m.files !== "object") return null;
    return m;
  } catch {
    return null;
  }
}

export function saveManifest(filePath: string, manifest: IndexManifest): void {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  manifest.updatedAt = new Date().toISOString();
  const tmp = filePath + ".tmp";
  fs.writeFileSync(tmp, JSON.stringify(manifest), "utf-8");
  fs.renameSync(tmp, filePath);
}

/** Content hash for change detection. Text files hash their bytes; asset
 *  binaries use a cheap mtime+size fingerprint (extracting them every walk
 *  would be far too slow). */
export function hashFile(rec: FileRecord): string {
  if (rec.kind === "blueprint" || rec.kind === "asset") {
    return `m${Math.round(rec.mtime)}-${rec.size}`;
  }
  try {
    const buf = fs.readFileSync(rec.absPath);
    return crypto.createHash("sha1").update(buf).digest("hex");
  } catch {
    return `m${Math.round(rec.mtime)}-${rec.size}`;
  }
}

export function hashText(text: string): string {
  return crypto.createHash("sha1").update(text).digest("hex");
}

export interface ManifestDiff {
  added: FileRecord[];
  changed: FileRecord[];
  /** Sources present in the manifest but no longer on disk. */
  removed: string[];
  /** Pre-computed hashes keyed by relPath, for the added/changed set. */
  hashes: Map<string, string>;
}

/**
 * Compare the manifest against the current file set. A file is "changed" when
 * its mtime/size differ AND its content hash differs (the hash is the source of
 * truth; mtime is only used to skip hashing unchanged files).
 */
export function diffManifest(
  manifest: IndexManifest,
  files: FileRecord[],
  fullRebuild: boolean,
): ManifestDiff {
  const added: FileRecord[] = [];
  const changed: FileRecord[] = [];
  const hashes = new Map<string, string>();
  const present = new Set<string>();

  for (const rec of files) {
    present.add(rec.relPath);
    const prev = manifest.files[rec.relPath];
    if (fullRebuild || !prev) {
      hashes.set(rec.relPath, hashFile(rec));
      added.push(rec);
      continue;
    }
    // Fast path: identical mtime+size → assume unchanged, skip hashing.
    if (!fullRebuild && prev.mtime === rec.mtime && prev.size === rec.size) {
      continue;
    }
    const h = hashFile(rec);
    hashes.set(rec.relPath, h);
    if (h !== prev.hash) changed.push(rec);
  }

  const removed: string[] = [];
  if (!fullRebuild) {
    for (const source of Object.keys(manifest.files)) {
      if (!present.has(source)) removed.push(source);
    }
  }

  return { added, changed, removed, hashes };
}

export function manifestEntryFor(
  rec: FileRecord,
  hash: string,
  chunkIds: string[],
  dependencies?: string[],
): ManifestEntry {
  return {
    source: rec.relPath,
    hash,
    mtime: rec.mtime,
    size: rec.size,
    kind: rec.kind,
    chunkIds,
    dependencies,
  };
}
