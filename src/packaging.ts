/**
 * package_project - full UAT BuildCookRun packaging (build + cook + stage +
 * pak + archive) driven from the TS server, since UAT is an external process
 * the editor bridge cannot host. Packaging takes tens of minutes, so this is
 * a background job: start returns immediately and get_package_status polls
 * progress parsed from the UAT log stream.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { spawn, type ChildProcess } from "child_process";
import type { ProjectContext } from "./project.js";
import { findEngineInstall } from "./deployer.js";

const IS_WINDOWS = process.platform === "win32";
const TAIL_LIMIT = 300;

interface PackageJob {
  running: boolean;
  startedAt: number;
  finishedAt: number | null;
  command: string;
  platform: string;
  configuration: string;
  archiveDirectory: string;
  stage: string;
  exitCode: number | null;
  success: boolean | null;
  error?: string;
  outputTail: string[];
}

let currentJob: PackageJob | null = null;
let currentProc: ChildProcess | null = null;

function findRunUAT(project: ProjectContext): string | null {
  const envEngine = process.env.UE_ENGINE_PATH;
  const scriptName = IS_WINDOWS ? "RunUAT.bat" : "RunUAT.sh";
  const candidates: string[] = [];
  if (envEngine) candidates.push(path.join(envEngine, "Engine", "Build", "BatchFiles", scriptName));
  const associatedRoot = findEngineInstall(project.engineAssociation ?? null);
  if (associatedRoot) candidates.push(path.join(associatedRoot, "Engine", "Build", "BatchFiles", scriptName));
  if (process.env.UE_BUILD_TOOL_PATH) {
    // Build.bat lives in the same BatchFiles directory as RunUAT.
    candidates.push(path.join(path.dirname(process.env.UE_BUILD_TOOL_PATH), scriptName));
  }
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

// UAT announces each phase with "********** <NAME> COMMAND STARTED **********".
function updateStageFromLine(job: PackageJob, line: string): void {
  const match = line.match(/\*{4,}\s+([A-Z ]+?) COMMAND STARTED\s+\*{4,}/);
  if (match) {
    job.stage = match[1].trim().toLowerCase();
    return;
  }
  if (line.includes("BUILD SUCCESSFUL")) job.stage = "done";
}

export function startPackageProject(
  project: ProjectContext,
  p: Record<string, unknown>,
): Record<string, unknown> {
  if (currentJob?.running) {
    return {
      success: false,
      error: `A packaging job is already running (stage: ${currentJob.stage}). Poll get_package_status or wait for it to finish.`,
    };
  }

  project.ensureLoaded();
  const projectPath = path.resolve(project.projectPath!);
  const runUAT = findRunUAT(project);
  if (!runUAT) {
    return {
      success: false,
      error: "RunUAT not found. Set UE_ENGINE_PATH to the engine root, or ensure the project's EngineAssociation resolves to an installed engine.",
    };
  }

  const platform = typeof p.platform === "string" && p.platform ? p.platform : "Win64";
  const configuration = typeof p.configuration === "string" && p.configuration ? p.configuration : "Development";
  const projectName = path.basename(projectPath, ".uproject");
  const archiveDirectory = typeof p.archiveDirectory === "string" && p.archiveDirectory
    ? path.resolve(p.archiveDirectory)
    : path.join(path.dirname(projectPath), "Saved", "MCP", "Packages", platform);
  fs.mkdirSync(archiveDirectory, { recursive: true });

  const args = [
    "BuildCookRun",
    `-project="${projectPath}"`,
    `-platform=${platform}`,
    `-clientconfig=${configuration}`,
    "-build",
    "-cook",
    "-stage",
    "-archive",
    `-archivedirectory="${archiveDirectory}"`,
    "-noP4",
    "-utf8output",
    "-unattended",
  ];
  if (p.pak !== false) args.push("-pak");
  if (p.clean === true) args.push("-clean");
  if (Array.isArray(p.maps) && p.maps.length > 0) {
    args.push(`-map=${(p.maps as string[]).join("+")}`);
  }

  const job: PackageJob = {
    running: true,
    startedAt: Date.now(),
    finishedAt: null,
    command: `RunUAT ${args.join(" ")}`,
    platform,
    configuration,
    archiveDirectory,
    stage: "starting",
    exitCode: null,
    success: null,
    outputTail: [],
  };
  currentJob = job;

  let proc: ChildProcess;
  if (IS_WINDOWS) {
    // Same cmd /c quoting dance as buildProject: the batch path may contain
    // spaces and the args already carry their own quotes.
    const fullCommand = `cmd /c ""${runUAT}" ${args.join(" ")}"`;
    proc = spawn(fullCommand, [], { shell: true, stdio: "pipe" });
  } else {
    proc = spawn(runUAT, args.map((a) => a.replace(/"/g, "")), { stdio: "pipe" });
  }
  currentProc = proc;

  const onData = (data: Buffer) => {
    const lines = data.toString().split(/\r?\n/);
    for (const line of lines) {
      if (!line.trim()) continue;
      job.outputTail.push(line);
      if (job.outputTail.length > TAIL_LIMIT) job.outputTail.shift();
      updateStageFromLine(job, line);
    }
  };
  proc.stdout?.on("data", onData);
  proc.stderr?.on("data", onData);

  proc.on("close", (code) => {
    job.running = false;
    job.finishedAt = Date.now();
    job.exitCode = code;
    job.success = code === 0;
    if (code !== 0) {
      job.error = `UAT exited with code ${code}. Check the outputTail for the failing step.`;
      if (job.stage !== "done") job.stage = `failed (during ${job.stage})`;
    } else {
      job.stage = "done";
    }
    currentProc = null;
  });
  proc.on("error", (err) => {
    job.running = false;
    job.finishedAt = Date.now();
    job.success = false;
    job.error = `Failed to launch UAT: ${err.message}`;
    job.stage = "failed";
    currentProc = null;
  });

  return {
    success: true,
    started: true,
    platform,
    configuration,
    archiveDirectory,
    hint: "Packaging runs in the background (typically 10-60 minutes). Poll editor(get_package_status) for progress; the archive lands in archiveDirectory when stage=done.",
  };
}

export function getPackageStatus(p: Record<string, unknown>): Record<string, unknown> {
  if (!currentJob) {
    return { success: true, active: false, message: "No packaging job has been started in this session." };
  }
  const tailLines = typeof p.tailLines === "number" ? Math.max(0, Math.min(p.tailLines, TAIL_LIMIT)) : 25;
  const job = currentJob;
  return {
    success: job.success ?? true,
    active: job.running,
    stage: job.stage,
    platform: job.platform,
    configuration: job.configuration,
    archiveDirectory: job.archiveDirectory,
    elapsedSeconds: Math.round(((job.finishedAt ?? Date.now()) - job.startedAt) / 1000),
    exitCode: job.exitCode,
    ...(job.error ? { error: job.error } : {}),
    outputTail: tailLines > 0 ? job.outputTail.slice(-tailLines) : [],
  };
}

export function cancelPackageJob(): Record<string, unknown> {
  if (!currentJob?.running || !currentProc) {
    return { success: false, error: "No packaging job is running." };
  }
  try {
    if (IS_WINDOWS && currentProc.pid) {
      // UAT spawns child processes (UBT, cooker); kill the whole tree.
      spawn("taskkill", ["/PID", String(currentProc.pid), "/T", "/F"], { stdio: "ignore" });
    } else {
      currentProc.kill("SIGTERM");
    }
    currentJob.stage = "cancelled";
    return { success: true, cancelled: true };
  } catch (e) {
    return { success: false, error: e instanceof Error ? e.message : String(e) };
  }
}
