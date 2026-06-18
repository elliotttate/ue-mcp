/**
 * Optional file watcher that keeps the index fresh by running a debounced
 * incremental build whenever project files change. Recursive fs.watch is used
 * where supported (Windows, macOS); on platforms without it (Linux) watching is
 * reported unavailable rather than failing. The incremental build is serialized
 * by the indexer's per-project lock, so a watch run and a manual build never
 * collide.
 */
import * as fs from "node:fs";
import type { IBridge } from "../bridge.js";
import type { IntelligenceConfig } from "./config.js";
import { Indexer } from "./indexer.js";
import { info, warn } from "../log.js";

const IGNORE_RE = /(^|[/\\])(\.ue-mcp|\.git|Saved|Intermediate|Binaries|DerivedDataCache|Build|node_modules)([/\\]|$)/;

export class ProjectWatcher {
  private watcher: fs.FSWatcher | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private building = false;
  private dirty = false;

  constructor(
    private readonly projectDir: string,
    private readonly projectName: string | null,
    private readonly bridge: IBridge,
    private readonly cfg: IntelligenceConfig,
    private readonly debounceMs = 2500,
  ) {}

  get active(): boolean {
    return this.watcher !== null;
  }

  /** Returns true if watching started, false if unsupported on this platform. */
  start(): boolean {
    if (this.watcher) return true;
    try {
      this.watcher = fs.watch(this.projectDir, { recursive: true }, (_event, filename) => {
        if (!filename) return;
        if (IGNORE_RE.test(filename.toString())) return;
        this.schedule();
      });
    } catch (e) {
      warn("intel", `recursive watch unavailable on this platform: ${(e as Error).message}`);
      return false;
    }
    info("intel", `watching ${this.projectDir} for changes`);
    return true;
  }

  stop(): void {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    this.watcher?.close();
    this.watcher = null;
  }

  private schedule(): void {
    this.dirty = true;
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.run(), this.debounceMs);
  }

  private async run(): Promise<void> {
    if (this.building) return; // re-scheduled via `dirty` when the current run ends
    this.building = true;
    this.dirty = false;
    try {
      const stats = await new Indexer(this.projectDir, this.projectName, this.bridge, this.cfg).build({ rebuild: false });
      if (stats.addedFiles + stats.changedFiles + stats.removedFiles > 0) {
        info("intel", `watch reindex: +${stats.addedFiles} ~${stats.changedFiles} -${stats.removedFiles}`);
      }
    } catch (e) {
      // A concurrent manual build holds the lock and will pick up these changes.
      warn("intel", `watch reindex skipped: ${(e as Error).message}`);
    } finally {
      this.building = false;
      if (this.dirty) this.schedule();
    }
  }
}

const watchers = new Map<string, ProjectWatcher>();

export function startWatching(
  projectDir: string,
  projectName: string | null,
  bridge: IBridge,
  cfg: IntelligenceConfig,
): boolean {
  let w = watchers.get(projectDir);
  if (!w) {
    w = new ProjectWatcher(projectDir, projectName, bridge, cfg);
    watchers.set(projectDir, w);
  }
  return w.start();
}

export function stopWatching(projectDir: string): boolean {
  const w = watchers.get(projectDir);
  if (!w || !w.active) return false;
  w.stop();
  return true;
}

export function isWatching(projectDir: string): boolean {
  return watchers.get(projectDir)?.active ?? false;
}
