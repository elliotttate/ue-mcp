/**
 * Project file walker for indexing. Honors a built-in set of Unreal/SCM
 * excludes plus user-supplied ignore globs (from config and an optional
 * .ue-mcp/INDEX_IGNORE file). Returns lightweight file records — content is
 * read later, only for files that actually changed.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import type { ChunkKind } from "./types.js";
import { indexIgnorePath } from "./paths.js";

export interface FileRecord {
  absPath: string;
  /** Project-relative POSIX path (the stable index key). */
  relPath: string;
  ext: string;
  mtime: number;
  size: number;
  kind: ChunkKind;
}

/** Directories never worth indexing. Matched by exact segment name. */
const EXCLUDED_DIRS = new Set([
  "Saved",
  "Intermediate",
  "Binaries",
  "DerivedDataCache",
  "Build",
  ".git",
  ".svn",
  ".plastic",
  ".p4",
  ".vs",
  ".idea",
  "node_modules",
  ".ue-mcp",
]);

/** Extension → chunk kind. Files with no entry here are skipped (binaries,
 *  images, etc.), except .uasset/.umap which are handled out-of-band. */
const EXT_KIND: Record<string, ChunkKind> = {
  ".cpp": "code", ".h": "code", ".hpp": "code", ".c": "code", ".cc": "code",
  ".cxx": "code", ".inl": "code", ".cs": "code", ".py": "code",
  ".usf": "code", ".ush": "code", ".verse": "code",
  ".ini": "config", ".uproject": "config", ".uplugin": "config", ".json": "config",
  ".md": "document", ".txt": "document", ".rst": "document",
};

const ASSET_EXTS = new Set([".uasset", ".umap"]);

export function classifyExt(ext: string): ChunkKind | "asset" | null {
  const lower = ext.toLowerCase();
  if (ASSET_EXTS.has(lower)) return "asset";
  return EXT_KIND[lower] ?? null;
}

/** A compiled ignore pattern (gitignore-lite). */
interface IgnoreRule {
  re: RegExp;
  dirOnly: boolean;
}

/** Translate a gitignore-lite glob into a RegExp anchored at the project root.
 *  Supports `*` (non-slash), `**` (any), leading `/` (anchored), trailing `/`
 *  (directory only). Unanchored patterns match at any depth. */
function compileIgnore(pattern: string): IgnoreRule | null {
  let p = pattern.trim();
  if (!p || p.startsWith("#")) return null;
  const dirOnly = p.endsWith("/");
  if (dirOnly) p = p.slice(0, -1);
  const anchored = p.startsWith("/");
  if (anchored) p = p.slice(1);

  let re = "";
  for (let i = 0; i < p.length; i++) {
    const c = p[i];
    if (c === "*") {
      if (p[i + 1] === "*") {
        re += ".*";
        i++;
        if (p[i + 1] === "/") i++;
      } else {
        re += "[^/]*";
      }
    } else if (c === "?") {
      re += "[^/]";
    } else if (".+^${}()|[]\\".includes(c)) {
      re += "\\" + c;
    } else {
      re += c;
    }
  }
  const prefix = anchored ? "^" : "(^|.*/)";
  return { re: new RegExp(`${prefix}${re}(/.*)?$`), dirOnly };
}

export function loadIgnoreRules(projectDir: string, extra: string[] = [], respectGitignore = false): IgnoreRule[] {
  const patterns = [...extra];
  // Project .gitignore (opt-in). Negation (`!`) lines are unsupported and skipped.
  if (respectGitignore) {
    const gitignore = path.join(projectDir, ".gitignore");
    if (fs.existsSync(gitignore)) {
      try {
        for (const line of fs.readFileSync(gitignore, "utf-8").split(/\r?\n/)) {
          if (!line.trim().startsWith("!")) patterns.push(line);
        }
      } catch {
        /* ignore unreadable .gitignore */
      }
    }
  }
  const ignoreFile = indexIgnorePath(projectDir);
  if (fs.existsSync(ignoreFile)) {
    try {
      patterns.push(...fs.readFileSync(ignoreFile, "utf-8").split(/\r?\n/));
    } catch {
      /* ignore unreadable INDEX_IGNORE */
    }
  }
  const rules: IgnoreRule[] = [];
  for (const p of patterns) {
    const r = compileIgnore(p);
    if (r) rules.push(r);
  }
  return rules;
}

function isIgnored(relPosix: string, isDir: boolean, rules: IgnoreRule[]): boolean {
  for (const r of rules) {
    if (r.dirOnly && !isDir) continue;
    if (r.re.test(relPosix)) return true;
  }
  return false;
}

export interface WalkOptions {
  /** Include .uasset/.umap records (only meaningful when the bridge is up). */
  includeAssets: boolean;
  maxFileSize: number;
  ignore: string[];
  /** Also honor the project's .gitignore (opt-in). */
  respectGitignore?: boolean;
  /** Optional extra root directories (e.g. plugin Content dirs). */
  extraRoots?: string[];
}

/**
 * Walk the project (and any extra roots), yielding indexable file records.
 * The project's own Source/Config/Content/Plugins live under projectDir, so a
 * single recursive walk from projectDir covers the common case.
 */
export function walkProject(projectDir: string, opts: WalkOptions): FileRecord[] {
  const rules = loadIgnoreRules(projectDir, opts.ignore, opts.respectGitignore);
  const out: FileRecord[] = [];
  const roots = [projectDir, ...(opts.extraRoots ?? [])];
  const seen = new Set<string>();

  const visit = (absDir: string): void => {
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(absDir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const abs = path.join(absDir, entry.name);
      const rel = path.relative(projectDir, abs).split(path.sep).join("/");
      if (entry.isDirectory()) {
        if (EXCLUDED_DIRS.has(entry.name)) continue;
        if (isIgnored(rel, true, rules)) continue;
        visit(abs);
      } else if (entry.isFile()) {
        const ext = path.extname(entry.name);
        const kind = classifyExt(ext);
        if (!kind) continue;
        if (kind === "asset" && !opts.includeAssets) continue;
        if (isIgnored(rel, false, rules)) continue;
        if (seen.has(abs)) continue;
        let stat: fs.Stats;
        try {
          stat = fs.statSync(abs);
        } catch {
          continue;
        }
        // Asset binaries are not read as text, so skip the size cap for them.
        if (kind !== "asset" && stat.size > opts.maxFileSize) continue;
        seen.add(abs);
        out.push({
          absPath: abs,
          relPath: rel,
          ext: ext.toLowerCase(),
          mtime: stat.mtimeMs,
          size: stat.size,
          kind: kind === "asset" ? "blueprint" : kind,
        });
      }
    }
  };

  for (const root of roots) {
    if (fs.existsSync(root)) visit(root);
  }
  return out;
}
