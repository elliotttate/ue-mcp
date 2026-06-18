/**
 * Project memory: durable, human-readable notes the agent accumulates across
 * sessions (decisions, conventions, gotchas, TODOs). Each memory is one
 * markdown file under `.ue-mcp/memory/`. Deliberately plain files so a human
 * can read/edit them and so they survive index rebuilds.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { memoryDir, ensureDir } from "./paths.js";

export interface MemorySummary {
  name: string;
  title: string;
  bytes: number;
  updatedAt: string;
}

function safeName(name: string): string {
  const base = name
    .trim()
    .replace(/\.md$/i, "")
    .replace(/[^A-Za-z0-9._-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 120);
  if (!base) throw new Error(`Invalid memory name: ${JSON.stringify(name)}`);
  return base;
}

function fileFor(projectDir: string, name: string): string {
  return path.join(memoryDir(projectDir), safeName(name) + ".md");
}

export function listMemories(projectDir: string): MemorySummary[] {
  const dir = memoryDir(projectDir);
  if (!fs.existsSync(dir)) return [];
  const out: MemorySummary[] = [];
  for (const f of fs.readdirSync(dir)) {
    if (!f.endsWith(".md")) continue;
    const full = path.join(dir, f);
    let stat: fs.Stats;
    try {
      stat = fs.statSync(full);
    } catch {
      continue;
    }
    const firstLine = fs.readFileSync(full, "utf-8").split(/\r?\n/).find((l) => l.trim()) ?? "";
    out.push({
      name: f.replace(/\.md$/, ""),
      title: firstLine.replace(/^#+\s*/, "").slice(0, 120),
      bytes: stat.size,
      updatedAt: new Date(stat.mtimeMs).toISOString(),
    });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

export function readMemory(projectDir: string, name: string): string | null {
  const file = fileFor(projectDir, name);
  return fs.existsSync(file) ? fs.readFileSync(file, "utf-8") : null;
}

export function writeMemory(projectDir: string, name: string, content: string): string {
  ensureDir(memoryDir(projectDir));
  const file = fileFor(projectDir, name);
  fs.writeFileSync(file, content, "utf-8");
  return file;
}

export function appendMemory(projectDir: string, name: string, content: string): string {
  ensureDir(memoryDir(projectDir));
  const file = fileFor(projectDir, name);
  const prefix = fs.existsSync(file) && fs.readFileSync(file, "utf-8").length > 0 ? "\n" : "";
  fs.appendFileSync(file, prefix + content + "\n", "utf-8");
  return file;
}

export function deleteMemory(projectDir: string, name: string): boolean {
  const file = fileFor(projectDir, name);
  if (!fs.existsSync(file)) return false;
  fs.rmSync(file, { force: true });
  return true;
}
