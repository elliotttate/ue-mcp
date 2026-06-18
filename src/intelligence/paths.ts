/**
 * Canonical on-disk layout for the Project Intelligence state. Everything
 * lives under `<projectDir>/.ue-mcp/` so it is per-project, git-ignorable,
 * and trivially clearable.
 */
import * as fs from "node:fs";
import * as path from "node:path";

export function intelligenceDir(projectDir: string): string {
  return path.join(projectDir, ".ue-mcp");
}

export function indexDir(projectDir: string): string {
  return path.join(intelligenceDir(projectDir), "index");
}

export function vectorStorePath(projectDir: string): string {
  return path.join(indexDir(projectDir), "vectors.jsonl");
}

export function manifestPath(projectDir: string): string {
  return path.join(indexDir(projectDir), "manifest.json");
}

export function graphPath(projectDir: string): string {
  return path.join(indexDir(projectDir), "graph.json");
}

export function memoryDir(projectDir: string): string {
  return path.join(intelligenceDir(projectDir), "memory");
}

export function generatedImageDir(projectDir: string): string {
  return path.join(intelligenceDir(projectDir), "generated-images");
}

/** Optional user-authored ignore file (one glob-ish pattern per line). */
export function indexIgnorePath(projectDir: string): string {
  return path.join(intelligenceDir(projectDir), "INDEX_IGNORE");
}

export function ensureDir(dir: string): void {
  fs.mkdirSync(dir, { recursive: true });
}
