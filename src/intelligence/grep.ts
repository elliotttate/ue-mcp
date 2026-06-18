/**
 * Filesystem literal/regex search over project text files. Unlike `search`,
 * this needs no index and no embedding — it is the precise "find this exact
 * string/pattern" complement to semantic retrieval, with the same UE-aware
 * ignore handling as the indexer.
 */
import * as fs from "node:fs";
import { walkProject } from "./walker.js";

export interface GrepOptions {
  query: string;
  /** Treat query as a regular expression (default: literal substring). */
  regex?: boolean;
  ignoreCase?: boolean;
  /** Restrict to these file extensions (e.g. [".cpp", ".h"]). */
  ext?: string[];
  /** Restrict to sources starting with this project-relative prefix. */
  sourcePrefix?: string;
  maxResults?: number;
  maxFileSize?: number;
  ignore?: string[];
  respectGitignore?: boolean;
}

export interface GrepMatch {
  source: string;
  line: number;
  text: string;
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export function grepProject(
  projectDir: string,
  opts: GrepOptions,
): { count: number; truncated: boolean; matches: GrepMatch[] } {
  const max = opts.maxResults ?? 200;
  const flags = opts.ignoreCase ? "i" : "";
  let re: RegExp;
  try {
    re = new RegExp(opts.regex ? opts.query : escapeRegExp(opts.query), flags);
  } catch (e) {
    throw new Error(`Invalid regex: ${e instanceof Error ? e.message : String(e)}`);
  }

  const files = walkProject(projectDir, {
    includeAssets: false,
    maxFileSize: opts.maxFileSize ?? 1024 * 1024,
    ignore: opts.ignore ?? [],
    respectGitignore: opts.respectGitignore,
  });
  const extSet = opts.ext && opts.ext.length ? new Set(opts.ext.map((e) => e.toLowerCase())) : null;

  const matches: GrepMatch[] = [];
  let truncated = false;
  for (const f of files) {
    if (extSet && !extSet.has(f.ext)) continue;
    if (opts.sourcePrefix && !f.relPath.startsWith(opts.sourcePrefix)) continue;
    let text: string;
    try {
      const buf = fs.readFileSync(f.absPath);
      if (buf.subarray(0, Math.min(buf.length, 8000)).includes(0)) continue; // binary
      text = buf.toString("utf-8");
    } catch {
      continue;
    }
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      if (re.test(lines[i])) {
        matches.push({ source: f.relPath, line: i + 1, text: lines[i].slice(0, 400) });
        if (matches.length >= max) {
          truncated = true;
          break;
        }
      }
    }
    if (truncated) break;
  }
  return { count: matches.length, truncated, matches };
}
