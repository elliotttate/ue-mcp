/**
 * Document ingestion. Converts external documents to plain markdown/text so
 * they can be embedded into the project index alongside code. Text-like formats
 * (md/txt/json/csv) are handled natively with no dependencies. Office and PDF
 * formats are parsed via OPTIONAL libraries loaded with a guarded dynamic
 * import — if they are not installed, ingestion reports a clear, actionable
 * message instead of failing. This keeps the server's dependency footprint at
 * zero while still supporting rich documents when the user opts in.
 */
import * as fs from "node:fs";
import * as path from "node:path";

export const NATIVE_DOC_EXTS = new Set([".md", ".markdown", ".txt", ".rst", ".json", ".csv", ".tsv", ".log"]);
export const RICH_DOC_EXTS = new Set([".pdf", ".docx", ".pptx", ".xlsx"]);

export function isSupportedDoc(ext: string): boolean {
  const e = ext.toLowerCase();
  return NATIVE_DOC_EXTS.has(e) || RICH_DOC_EXTS.has(e);
}

/** Dynamic import that never throws on a missing optional package. The
 *  non-literal specifier keeps the type checker from resolving the module. */
async function optionalImport(name: string): Promise<Record<string, unknown> | null> {
  try {
    const spec = name;
    return (await import(spec)) as Record<string, unknown>;
  } catch {
    return null;
  }
}

export interface ParsedDocument {
  text: string;
  /** Which path produced this: native, or the optional lib name used. */
  via: string;
}

export class MissingParserError extends Error {
  constructor(
    public readonly ext: string,
    public readonly suggestion: string,
  ) {
    super(`No parser available for ${ext}. ${suggestion}`);
    this.name = "MissingParserError";
  }
}

export async function parseDocumentToMarkdown(absPath: string): Promise<ParsedDocument> {
  const ext = path.extname(absPath).toLowerCase();

  if (NATIVE_DOC_EXTS.has(ext)) {
    return { text: fs.readFileSync(absPath, "utf-8"), via: "native" };
  }

  if (ext === ".pdf") {
    const lib = (await optionalImport("pdf-parse")) as { default?: (b: Buffer) => Promise<{ text: string }> } | null;
    const fn = lib?.default;
    if (fn) {
      const out = await fn(fs.readFileSync(absPath));
      return { text: out.text, via: "pdf-parse" };
    }
    throw new MissingParserError(ext, "Install it to enable PDF ingestion: npm i -D pdf-parse");
  }

  if (ext === ".docx") {
    const lib = (await optionalImport("mammoth")) as
      | { extractRawText?: (o: { path: string }) => Promise<{ value: string }> }
      | null;
    if (lib?.extractRawText) {
      const out = await lib.extractRawText({ path: absPath });
      return { text: out.value, via: "mammoth" };
    }
    throw new MissingParserError(ext, "Install it to enable DOCX ingestion: npm i -D mammoth");
  }

  if (ext === ".xlsx") {
    const lib = (await optionalImport("xlsx")) as
      | {
          readFile?: (p: string) => { SheetNames: string[]; Sheets: Record<string, unknown> };
          utils?: { sheet_to_csv?: (s: unknown) => string };
        }
      | null;
    if (lib?.readFile && lib.utils?.sheet_to_csv) {
      const wb = lib.readFile(absPath);
      const parts: string[] = [];
      for (const name of wb.SheetNames) {
        parts.push(`# ${name}\n` + lib.utils.sheet_to_csv(wb.Sheets[name]));
      }
      return { text: parts.join("\n\n"), via: "xlsx" };
    }
    throw new MissingParserError(ext, "Install it to enable XLSX ingestion: npm i -D xlsx");
  }

  if (ext === ".pptx") {
    const lib = (await optionalImport("officeparser")) as
      | { parseOfficeAsync?: (p: string) => Promise<string> }
      | null;
    if (lib?.parseOfficeAsync) {
      return { text: await lib.parseOfficeAsync(absPath), via: "officeparser" };
    }
    throw new MissingParserError(ext, "Install it to enable PPTX ingestion: npm i -D officeparser");
  }

  throw new MissingParserError(ext, "Unsupported document type.");
}

/** Collect supported document files under a directory (non-recursive limit of
 *  a few levels to avoid runaway walks). */
export function collectDocuments(absDir: string, maxDepth = 4): string[] {
  const out: string[] = [];
  const walk = (dir: string, depth: number): void => {
    if (depth > maxDepth) return;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const abs = path.join(dir, e.name);
      if (e.isDirectory()) {
        if (e.name === "node_modules" || e.name.startsWith(".")) continue;
        walk(abs, depth + 1);
      } else if (e.isFile() && isSupportedDoc(path.extname(e.name))) {
        out.push(abs);
      }
    }
  };
  walk(absDir, 0);
  return out;
}
