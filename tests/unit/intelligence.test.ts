import { describe, expect, it, beforeEach, afterEach } from "vitest";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import { tokenize } from "../../src/intelligence/text.js";
import { LocalHashingEmbedding } from "../../src/intelligence/providers.js";
import { dot } from "../../src/intelligence/vec.js";
import { VectorStore } from "../../src/intelligence/vector-store.js";
import { chunkText } from "../../src/intelligence/chunker.js";
import { diffManifest, emptyManifest, manifestEntryFor, hashFile } from "../../src/intelligence/manifest.js";
import { walkProject } from "../../src/intelligence/walker.js";
import { searchIndex } from "../../src/intelligence/search.js";
import {
  computeScores,
  shortestPath,
  neighbors,
  topHubs,
  subgraph,
  GraphBuilder,
  loadGraph,
  type KnowledgeGraph,
} from "../../src/intelligence/knowledge-graph.js";
import { Indexer, loadIndex, indexStatus } from "../../src/intelligence/indexer.js";
import { toGamePath } from "../../src/intelligence/bp-extract.js";
import { grepProject } from "../../src/intelligence/grep.js";
import { buildProjectDigest } from "../../src/intelligence/summary.js";
import type { IBridge } from "../../src/bridge.js";
import type { VectorEntry } from "../../src/intelligence/types.js";

/** A bridge whose connection can be toggled, answering blueprint extraction
 *  (batch preferred) and counting calls per method. */
class MockBridge implements IBridge {
  connected = true;
  calls: Record<string, number> = {};
  get isConnected() {
    return this.connected;
  }
  async connect() {
    /* no-op */
  }
  async call(method: string, params?: Record<string, unknown>) {
    this.calls[method] = (this.calls[method] ?? 0) + 1;
    if (method === "extract_index_summaries") {
      const paths = (params?.paths as string[]) ?? [];
      return {
        summaries: paths.map((p) => ({
          path: p,
          summary: `Blueprint ${p} parent ACharacter`,
          dependencies: ["/Game/Core/BP_GameMode"],
          name: p.split("/").pop(),
        })),
      };
    }
    if (method === "extract_index_summary") {
      return {
        summary: "Blueprint BP_Player parent ACharacter variables Health Stamina",
        dependencies: ["/Game/Core/BP_GameMode"],
        name: "BP_Player",
      };
    }
    throw new Error(`unsupported ${method}`);
  }
}

const offlineBridge: IBridge = {
  isConnected: false,
  async call() {
    throw new Error("offline");
  },
  async connect() {
    /* no-op */
  },
};

function tmpProject(): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-intel-"));
  fs.writeFileSync(path.join(dir, "Game.uproject"), JSON.stringify({ EngineAssociation: "5.6" }));
  fs.mkdirSync(path.join(dir, "Source", "Game"), { recursive: true });
  return dir;
}

describe("text.tokenize", () => {
  it("splits identifiers on camelCase and separators", () => {
    expect(tokenize("GetPlayerHealth_BP")).toEqual(["get", "player", "health", "bp"]);
    expect(tokenize("UCharacterMovementComponent")).toContain("character");
    expect(tokenize("a.b-c d")).toEqual(["a", "b", "c", "d"]);
  });
});

describe("LocalHashingEmbedding", () => {
  const emb = new LocalHashingEmbedding(256);

  it("is deterministic and unit-normalized", async () => {
    const [a, b] = await emb.embed(["the quick brown fox", "the quick brown fox"]);
    expect(a.length).toBe(256);
    expect(dot(a, b)).toBeCloseTo(1, 5); // identical text → cosine 1
    expect(Math.sqrt(dot(a, a))).toBeCloseTo(1, 5);
  });

  it("ranks related text above unrelated", async () => {
    const [q] = await emb.embed(["player health damage"]);
    const [related] = await emb.embed(["apply damage to the player health component"]);
    const [unrelated] = await emb.embed(["sort an array of integers quickly"]);
    expect(dot(q, related)).toBeGreaterThan(dot(q, unrelated));
  });
});

describe("VectorStore", () => {
  it("round-trips through save/load and searches by cosine", async () => {
    const dir = tmpProject();
    try {
      const emb = new LocalHashingEmbedding(128);
      const texts = ["spawn an actor in the level", "compile the blueprint graph", "import a texture asset"];
      const vecs = await emb.embed(texts);
      const store = new VectorStore(128, emb.key);
      store.upsert(
        texts.map((t, i): VectorEntry => ({
          id: `s#${i}`,
          vector: vecs[i],
          meta: { id: `s#${i}`, source: `f${i}.cpp`, kind: "code", text: t },
        })),
      );
      const file = path.join(dir, "vectors.jsonl");
      store.save(file);
      const loaded = VectorStore.load(file)!;
      expect(loaded.size).toBe(3);

      const [q] = await emb.embed(["how do I spawn an actor"]);
      const hits = loaded.search(q, 1);
      expect(hits[0].entry.meta.source).toBe("f0.cpp");
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("chunkText", () => {
  it("covers the file with line spans", () => {
    const lines = Array.from({ length: 50 }, (_, i) => `line ${i} ${"x".repeat(40)}`);
    const chunks = chunkText("a.cpp", lines.join("\n"), "code", "cpp");
    expect(chunks.length).toBeGreaterThan(1);
    expect(chunks[0].startLine).toBe(1);
    // spans are monotonic and cover the end
    expect(chunks[chunks.length - 1].endLine).toBeGreaterThanOrEqual(45);
    for (const c of chunks) expect(c.id.startsWith("a.cpp#")).toBe(true);
  });
});

describe("manifest diff", () => {
  it("detects added, changed, and removed files", () => {
    const dir = tmpProject();
    try {
      const f = path.join(dir, "Source", "Game", "A.cpp");
      fs.writeFileSync(f, "int a = 1;");
      let files = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [] });
      const manifest = emptyManifest("local:hash-256");

      let d = diffManifest(manifest, files, false);
      expect(d.added.some((r) => r.relPath.endsWith("A.cpp"))).toBe(true);

      // record every walked file into the manifest (the .uproject is indexed
      // as config too, so recording only A.cpp would leave it "added").
      for (const rec of files) {
        manifest.files[rec.relPath] = manifestEntryFor(rec, hashFile(rec), [`${rec.relPath}#0`]);
      }

      // unchanged → no diff
      files = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [] });
      d = diffManifest(manifest, files, false);
      expect(d.added.length + d.changed.length).toBe(0);

      // change content → changed
      fs.writeFileSync(f, "int a = 2; // changed");
      // bump mtime explicitly (some filesystems have coarse resolution)
      const future = new Date(Date.now() + 5000);
      fs.utimesSync(f, future, future);
      files = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [] });
      d = diffManifest(manifest, files, false);
      expect(d.changed.some((r) => r.relPath.endsWith("A.cpp"))).toBe(true);

      // remove file → removed
      fs.rmSync(f);
      files = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [] });
      d = diffManifest(manifest, files, false);
      expect(d.removed.some((s) => s.endsWith("A.cpp"))).toBe(true);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("knowledge graph algorithms", () => {
  const graph: KnowledgeGraph = {
    version: 1,
    builtAt: "now",
    nodes: {
      A: { id: "A", label: "A", kind: "blueprint" },
      B: { id: "B", label: "B", kind: "asset" },
      C: { id: "C", label: "C", kind: "asset" },
      D: { id: "D", label: "D", kind: "blueprint" },
    },
    edges: [
      { from: "A", to: "B", type: "depends_on" },
      { from: "A", to: "C", type: "depends_on" },
      { from: "D", to: "B", type: "depends_on" },
    ],
    scores: {},
  };
  graph.scores = computeScores(graph.nodes, graph.edges);

  it("computes in/out degree and centrality", () => {
    expect(graph.scores.B.inDegree).toBe(2); // B is depended on by A and D
    expect(graph.scores.A.outDegree).toBe(2);
    expect(graph.scores.B.centrality).toBeGreaterThan(graph.scores.A.centrality);
  });

  it("ranks B as the top hub", () => {
    expect(topHubs(graph, 1)[0].node.id).toBe("B");
  });

  it("finds neighbors and shortest paths", () => {
    expect(neighbors(graph, "B", "in").map((n) => n.node.id).sort()).toEqual(["A", "D"]);
    expect(shortestPath(graph, "A", "D")).toEqual(["A", "B", "D"]);
    expect(shortestPath(graph, "A", "A")).toEqual(["A"]);
    const sg = subgraph(graph, "A", 1);
    expect(sg.nodes.map((n) => n.id).sort()).toEqual(["A", "B", "C"]);
  });
});

describe("Indexer end-to-end (offline)", () => {
  let dir: string;
  beforeEach(() => {
    dir = tmpProject();
  });
  afterEach(() => {
    fs.rmSync(dir, { recursive: true, force: true });
  });

  it("builds, searches, incrementally updates, and clears", async () => {
    fs.writeFileSync(
      path.join(dir, "Source", "Game", "Health.cpp"),
      "void ApplyDamage(float Amount) { Health -= Amount; if (Health <= 0) Die(); }",
    );
    fs.writeFileSync(path.join(dir, "README.md"), "# Game\nThis project is about spaceship combat and inventory.");

    const indexer = new Indexer(dir, "Game", offlineBridge, {});
    const stats = await indexer.build({ includeAssets: false });
    expect(stats.chunks).toBeGreaterThan(0);
    expect(stats.sources).toBeGreaterThanOrEqual(2);
    expect(stats.rebuilt).toBe(true);

    const status = indexStatus(dir);
    expect(status.built).toBe(true);
    expect(status.chunks).toBe(stats.chunks);

    const idx = loadIndex(dir, {})!;
    const hits = await searchIndex(idx.store, idx.provider, { query: "apply damage to health", k: 3 });
    expect(hits[0].source).toContain("Health.cpp");

    // Incremental: add a file, rebuild → exactly one added.
    fs.writeFileSync(path.join(dir, "Source", "Game", "Inventory.cpp"), "void AddItem(int Id) {}");
    const stats2 = await indexer.build({ includeAssets: false });
    expect(stats2.rebuilt).toBe(false);
    expect(stats2.addedFiles).toBe(1);

    indexer.clear();
    expect(indexStatus(dir).built).toBe(false);
  });

  it("preserves the blueprint index across an offline rebuild", async () => {
    fs.mkdirSync(path.join(dir, "Content"), { recursive: true });
    fs.writeFileSync(path.join(dir, "Content", "BP_Player.uasset"), Buffer.from([0, 1, 2, 3, 4]));
    fs.writeFileSync(path.join(dir, "Source", "Game", "A.cpp"), "int a = 1;");

    const bridge = new MockBridge();
    const indexer = new Indexer(dir, "Game", bridge, {});

    const s1 = await indexer.build({ includeAssets: true });
    expect(s1.assetsIndexed).toBe(1);
    const idx1 = loadIndex(dir, {})!;
    expect([...idx1.store.sources()].some((s) => s.includes("BP_Player"))).toBe(true);

    // Editor goes away; an incremental rebuild must NOT evict the blueprint index.
    bridge.connected = false;
    await indexer.build({ includeAssets: true });
    const idx2 = loadIndex(dir, {})!;
    expect([...idx2.store.sources()].some((s) => s.includes("BP_Player"))).toBe(true);
  });
});

describe("knowledge graph from cached manifest deps", () => {
  it("builds blueprint dependency edges offline after an indexed build", async () => {
    const dir = tmpProject();
    try {
      fs.mkdirSync(path.join(dir, "Content"), { recursive: true });
      fs.writeFileSync(path.join(dir, "Content", "BP_Player.uasset"), Buffer.from([0, 1, 2]));
      const bridge = new MockBridge();
      // Index with the editor "connected" so deps get cached into the manifest.
      await new Indexer(dir, "Game", bridge, {}).build({ includeAssets: true });

      // Editor goes away; the graph must still get blueprint edges from cache.
      bridge.connected = false;
      const { stats } = await new GraphBuilder(dir, "Game", bridge, {}).build();
      expect(stats.cachedDeps).toBeGreaterThanOrEqual(1);
      expect(stats.liveDeps).toBe(0);

      const g = loadGraph(dir)!;
      const edge = g.edges.find((e) => e.from === "/Game/BP_Player" && e.to.includes("BP_GameMode"));
      expect(edge).toBeTruthy();
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("subgraph bounds", () => {
  it("caps node count and flags truncation", () => {
    const g: KnowledgeGraph = { version: 1, builtAt: "now", nodes: {}, edges: [], scores: {} };
    g.nodes["H"] = { id: "H", label: "H", kind: "blueprint" };
    for (let i = 0; i < 8; i++) {
      const id = `N${i}`;
      g.nodes[id] = { id, label: id, kind: "asset" };
      g.edges.push({ from: "H", to: id, type: "depends_on" });
    }
    g.scores = computeScores(g.nodes, g.edges);
    const sg = subgraph(g, "H", 1, 3);
    expect(sg.truncated).toBe(true);
    expect(sg.nodes.length).toBeLessThanOrEqual(3);
  });
});

describe("batch blueprint extraction", () => {
  it("uses the batch handler and avoids per-asset calls", async () => {
    const dir = tmpProject();
    try {
      fs.mkdirSync(path.join(dir, "Content"), { recursive: true });
      for (const n of ["BP_A", "BP_B", "BP_C"]) {
        fs.writeFileSync(path.join(dir, "Content", `${n}.uasset`), Buffer.from([0, 1]));
      }
      const bridge = new MockBridge();
      const stats = await new Indexer(dir, "Game", bridge, {}).build({ includeAssets: true });
      expect(stats.assetsIndexed).toBe(3);
      expect(bridge.calls["extract_index_summaries"]).toBeGreaterThanOrEqual(1);
      expect(bridge.calls["extract_index_summary"] ?? 0).toBe(0);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("C++ class hierarchy", () => {
  it("adds parent edges from class declarations", async () => {
    const dir = tmpProject();
    try {
      fs.writeFileSync(path.join(dir, "Source", "Game", "MyActor.h"), "class MYGAME_API AMyActor : public AActor {};");
      const { stats } = await new GraphBuilder(dir, "Game", offlineBridge, {}).build({ includeAssets: false });
      expect(stats.classes).toBeGreaterThanOrEqual(1);
      const g = loadGraph(dir)!;
      expect(g.edges.some((e) => e.from === "class:AMyActor" && e.to === "class:AActor" && e.type === "parent")).toBe(true);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe(".gitignore awareness", () => {
  it("excludes ignored files only when respectGitignore is set", () => {
    const dir = tmpProject();
    try {
      fs.writeFileSync(path.join(dir, ".gitignore"), "Source/Game/Ignored.cpp\n");
      fs.writeFileSync(path.join(dir, "Source", "Game", "Ignored.cpp"), "int ignored;");
      fs.writeFileSync(path.join(dir, "Source", "Game", "Kept.cpp"), "int kept;");

      const withGit = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [], respectGitignore: true });
      expect(withGit.some((f) => f.relPath.endsWith("Ignored.cpp"))).toBe(false);
      expect(withGit.some((f) => f.relPath.endsWith("Kept.cpp"))).toBe(true);

      const withoutGit = walkProject(dir, { includeAssets: false, maxFileSize: 1e6, ignore: [] });
      expect(withoutGit.some((f) => f.relPath.endsWith("Ignored.cpp"))).toBe(true);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("lexical index + token budget", () => {
  it("caches the loaded index, gathers candidates, and budgets results", async () => {
    const dir = tmpProject();
    try {
      for (let i = 0; i < 5; i++) {
        fs.writeFileSync(
          path.join(dir, "Source", "Game", `F${i}.cpp`),
          `void Spawn${i}() { /* spawn actor ${i} with health and damage handling */ }`,
        );
      }
      await new Indexer(dir, "Game", offlineBridge, {}).build({ includeAssets: false });

      const a = loadIndex(dir, {})!;
      const b = loadIndex(dir, {})!;
      expect(a.store).toBe(b.store); // cached by mtime

      const lex = a.getLexical();
      expect(lex.candidates(["spawn"]).length).toBeGreaterThan(0);

      const full = await searchIndex(a.store, a.provider, { query: "spawn actor", k: 5, lexical: lex });
      const budgeted = await searchIndex(a.store, a.provider, { query: "spawn actor", k: 5, lexical: lex, maxTokens: 5 });
      expect(budgeted.length).toBeGreaterThanOrEqual(1);
      expect(budgeted.length).toBeLessThanOrEqual(full.length);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("context compression", () => {
  it("aliases repeated paths/identifiers losslessly with real savings", async () => {
    const { compressText, alphabetId } = await import("../../src/intelligence/compress.js");
    expect(alphabetId(0)).toBe("a");
    expect(alphabetId(26)).toBe("aa");

    const repeated = "/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter";
    const text = Array.from({ length: 6 }, (_, i) => `ref ${i}: ${repeated} uses AFWFPSCharacterComponent`).join("\n");
    const r = compressText(text);

    // It found aliases and shortened the text.
    expect(Object.keys(r.legend).length).toBeGreaterThan(0);
    expect(r.compressedChars).toBeLessThan(r.originalChars);
    expect(r.savedPct).toBeGreaterThan(0);

    // Lossless: substituting each alias back reproduces the original.
    let restored = r.text;
    for (const [alias, value] of Object.entries(r.legend)) {
      restored = restored.split(alias).join(value);
    }
    expect(restored).toBe(text);
  });

  it("returns text unchanged when nothing recurs enough", async () => {
    const { compressText } = await import("../../src/intelligence/compress.js");
    const r = compressText("a short unique line with no repeats");
    expect(Object.keys(r.legend).length).toBe(0);
    expect(r.text).toBe("a short unique line with no repeats");
  });
});

describe("mermaid rendering", () => {
  it("renders a knowledge-graph subset as a flowchart", async () => {
    const { graphToMermaid } = await import("../../src/intelligence/mermaid.js");
    const nodes = [
      { id: "A", label: "BP_Player", kind: "blueprint" as const },
      { id: "B", label: "BP_GameMode", kind: "asset" as const },
    ];
    const edges = [{ from: "A", to: "B", type: "depends_on" as const }];
    const { mermaid, truncated } = graphToMermaid(nodes, edges, { direction: "LR" });
    expect(mermaid.startsWith("flowchart LR")).toBe(true);
    expect(mermaid).toContain('["BP_Player"]');
    expect(mermaid).toMatch(/n0 --> n1/);
    expect(truncated).toBe(false);
  });

  it("renders a blueprint summary with exec (solid) and data (dotted) edges", async () => {
    const { blueprintSummaryToMermaid } = await import("../../src/intelligence/mermaid.js");
    const summary = {
      graphName: "EventGraph",
      nodes: [
        { id: "x1", title: "Event BeginPlay" },
        { id: "x2", title: 'Print "Hi"' },
        { id: "x3", title: "Get Health" },
      ],
      execEdges: [{ from: "x1", to: "x2" }],
      dataEdges: [{ from: "x3", to: "x2" }],
    };
    const { mermaid } = blueprintSummaryToMermaid(summary);
    expect(mermaid.startsWith("flowchart TD")).toBe(true);
    expect(mermaid).toContain("Event BeginPlay");
    expect(mermaid).toContain("Print 'Hi'"); // double quotes sanitized to single
    expect(mermaid).toMatch(/g0 --> g1/); // exec solid
    expect(mermaid).toMatch(/g2 -\.-> g1/); // data dotted
  });
});

describe("toGamePath", () => {
  it("maps project and plugin content to the correct mount", () => {
    expect(toGamePath("Content/Blueprints/BP_Player.uasset", "Game")).toBe("/Game/Blueprints/BP_Player");
    expect(toGamePath("Plugins/MyPlugin/Content/Widgets/WBP_HUD.uasset", "Game")).toBe("/MyPlugin/Widgets/WBP_HUD");
    expect(toGamePath("Content/Maps/Main.umap", "Game")).toBe("/Game/Maps/Main");
  });
});

describe("grepProject", () => {
  it("finds literal and regex matches with provenance", () => {
    const dir = tmpProject();
    try {
      fs.writeFileSync(path.join(dir, "Source", "Game", "Combat.cpp"), "void Fire() {}\nvoid Reload() {}\n");
      const literal = grepProject(dir, { query: "Reload" });
      expect(literal.matches.some((m) => m.source.endsWith("Combat.cpp") && m.line === 2)).toBe(true);
      const rx = grepProject(dir, { query: "void \\w+\\(", regex: true });
      expect(rx.count).toBe(2);
      const filtered = grepProject(dir, { query: "Fire", ext: [".md"] });
      expect(filtered.count).toBe(0); // no .md files match
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("buildProjectDigest", () => {
  it("summarizes index composition after a build", async () => {
    const dir = tmpProject();
    try {
      fs.writeFileSync(path.join(dir, "Source", "Game", "Player.cpp"), "class APlayer {};");
      fs.writeFileSync(path.join(dir, "README.md"), "# MyGame\nA tactics game.");
      await new Indexer(dir, "Game", offlineBridge, {}).build({ includeAssets: false });
      const digest = buildProjectDigest(dir, "Game");
      expect(digest.indexed).toBe(true);
      expect(digest.sources).toBeGreaterThanOrEqual(2);
      expect(digest.byKind.code).toBeGreaterThanOrEqual(1);
      expect(digest.readmeExcerpt).toContain("MyGame");
      expect(digest.approxIndexTokens).toBeGreaterThan(0);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});
