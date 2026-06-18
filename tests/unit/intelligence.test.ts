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
import { computeScores, shortestPath, neighbors, topHubs, subgraph, type KnowledgeGraph } from "../../src/intelligence/knowledge-graph.js";
import { Indexer, loadIndex, indexStatus } from "../../src/intelligence/indexer.js";
import { toGamePath } from "../../src/intelligence/bp-extract.js";
import { grepProject } from "../../src/intelligence/grep.js";
import { buildProjectDigest } from "../../src/intelligence/summary.js";
import type { IBridge } from "../../src/bridge.js";
import type { VectorEntry } from "../../src/intelligence/types.js";

/** A bridge whose connection can be toggled, answering extract_index_summary. */
class MockBridge implements IBridge {
  connected = true;
  get isConnected() {
    return this.connected;
  }
  async connect() {
    /* no-op */
  }
  async call(method: string) {
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
