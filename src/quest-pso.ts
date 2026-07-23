import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as net from "node:net";
import * as path from "node:path";

export interface CommandResult {
  stdout: string;
  stderr: string;
}

export interface CommandOptions {
  cwd?: string;
  timeoutMs?: number;
  maxBufferBytes?: number;
}

export type CommandRunner = (
  executable: string,
  args: string[],
  options?: CommandOptions,
) => Promise<CommandResult>;

export type LineSender = (
  host: string,
  port: number,
  line: string,
  timeoutMs: number,
) => Promise<string>;

export interface AdbDevice {
  serial: string;
  state: string;
  details: Record<string, string>;
}

export interface QuestPsoSetup {
  projectPath: string;
  enginePath: string;
  packageName: string;
  adbPath?: string;
}

export interface QuestPsoPreflight {
  device: AdbDevice;
  adbPath: string;
  adbVersion: string;
  packagePath: string;
  projectPath: string;
  projectDir: string;
  projectName: string;
  packageName: string;
  enginePath: string;
  editorCommandPath: string;
  cookedPipelineDirectory: string;
  stableShaderKeys: string[];
  bundledPipelineCaches: string[];
  warnings: string[];
  remote: QuestPsoRemotePaths;
}

export interface QuestPsoRemotePaths {
  projectDir: string;
  gameDir: string;
  savedDir: string;
  collectedPsos: string;
  writablePipelineCache: string;
  vulkanProgramBinaryCache: string;
}

export interface RqDevServerResponse {
  ok: boolean;
  output: string;
}

export interface RqCommandResult extends RqDevServerResponse {
  command: string;
  commandError: string | null;
  deviceSerial: string;
  devicePort: number;
  hostPort: number;
}

export interface HarvestValidation {
  valid: boolean;
  markerCount: number;
  reasons: string[];
  expectedLevels: number | null;
  completedLevels: number | null;
  levelDoneCount: number;
  levelDoneIndices: number[];
  psosLogged: number | null;
  tourErrors: string[];
  startMarker: string | null;
  doneMarker: string | null;
}

export interface CollectExpandOptions extends QuestPsoSetup {
  devicePort?: number;
  commandTimeoutMs?: number;
  requireCompleteTour?: boolean;
  saveBeforePull?: boolean;
  copyToBuild?: boolean;
  replaceExistingBuildCaches?: boolean;
  outputRoot?: string;
}

export interface QuestPsoDependencies {
  runCommand?: CommandRunner;
  sendLine?: LineSender;
  now?: () => Date;
  platform?: NodeJS.Platform;
}

const MAX_DEV_COMMAND_BYTES = 16 * 1024;
const DEFAULT_DEVICE_PORT = 6788;

function checkedExecFile(
  executable: string,
  args: string[],
  options: CommandOptions = {},
): Promise<CommandResult> {
  return new Promise((resolve, reject) => {
    execFile(
      executable,
      args,
      {
        cwd: options.cwd,
        encoding: "utf8",
        timeout: options.timeoutMs ?? 120_000,
        maxBuffer: options.maxBufferBytes ?? 64 * 1024 * 1024,
        windowsHide: true,
      },
      (error, stdout, stderr) => {
        const result = { stdout: stdout ?? "", stderr: stderr ?? "" };
        if (!error) {
          resolve(result);
          return;
        }
        const detail = result.stderr.trim() || result.stdout.trim() || error.message;
        reject(new Error(`${path.basename(executable)} ${args.join(" ")} failed: ${detail}`));
      },
    );
  });
}

function sendTcpLine(
  host: string,
  port: number,
  line: string,
  timeoutMs: number,
): Promise<string> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port });
    let response = "";
    let settled = false;

    const finish = (error?: Error, value?: string) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      if (error) reject(error);
      else resolve(value ?? "");
    };

    socket.setTimeout(timeoutMs);
    socket.setEncoding("utf8");
    socket.on("connect", () => socket.write(`${line}\n`, "utf8"));
    socket.on("data", (chunk: string) => {
      response += chunk;
      if (Buffer.byteLength(response, "utf8") > 4 * 1024 * 1024) {
        finish(new Error("RQDevServer response exceeded 4 MiB."));
        return;
      }
      const newline = response.indexOf("\n");
      if (newline >= 0) finish(undefined, response.slice(0, newline));
    });
    socket.on("timeout", () => finish(new Error(`RQDevServer timed out after ${timeoutMs} ms.`)));
    socket.on("error", (error) => finish(error));
    socket.on("end", () => {
      if (!settled) finish(new Error("RQDevServer closed the connection before returning a JSON line."));
    });
  });
}

function assertAbsoluteExistingFile(value: string, label: string): string {
  if (!path.isAbsolute(value)) throw new Error(`${label} must be an explicit absolute path.`);
  const resolved = path.resolve(value);
  if (!fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) {
    throw new Error(`${label} does not exist or is not a file: ${resolved}`);
  }
  return resolved;
}

function assertAbsoluteExistingDirectory(value: string, label: string): string {
  if (!path.isAbsolute(value)) throw new Error(`${label} must be an explicit absolute path.`);
  const resolved = path.resolve(value);
  if (!fs.existsSync(resolved) || !fs.statSync(resolved).isDirectory()) {
    throw new Error(`${label} does not exist or is not a directory: ${resolved}`);
  }
  return resolved;
}

function assertAbsoluteDirectoryTarget(value: string, label: string): string {
  if (!path.isAbsolute(value)) throw new Error(`${label} must be an explicit absolute path.`);
  const resolved = path.resolve(value);
  if (fs.existsSync(resolved) && !fs.statSync(resolved).isDirectory()) {
    throw new Error(`${label} exists but is not a directory: ${resolved}`);
  }
  return resolved;
}

function assertSafeIdentifier(value: string, label: string, pattern: RegExp): string {
  const trimmed = value.trim();
  if (!pattern.test(trimmed)) throw new Error(`${label} contains unsupported characters: ${value}`);
  return trimmed;
}

function assertPort(port: number, label: string): number {
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error(`${label} must be an integer from 1 to 65535.`);
  }
  return port;
}

export function quoteRemoteShellArg(value: string): string {
  if (/[\r\n\0]/.test(value)) throw new Error("Remote shell values cannot contain newlines or NUL bytes.");
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

function findFiles(root: string, extension: string, recursive: boolean): string[] {
  if (!fs.existsSync(root)) return [];
  const out: string[] = [];
  const visit = (directory: string) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const fullPath = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        if (recursive) visit(fullPath);
      } else if (entry.isFile() && entry.name.toLowerCase().endsWith(extension.toLowerCase())) {
        out.push(fullPath);
      }
    }
  };
  visit(root);
  return out.sort((left, right) => left.localeCompare(right));
}

function buildCacheDestinations(projectDir: string, spcName: string): string[] {
  return [
    path.join(projectDir, "Build", "Android", "PipelineCaches", spcName),
    path.join(projectDir, "Build", "Android_ASTC", "PipelineCaches", spcName),
  ];
}

function assertBuildPublicationAllowed(destinations: string[], replaceExisting: boolean): void {
  for (const destination of destinations) {
    if (!fs.existsSync(destination)) continue;
    if (!fs.statSync(destination).isFile()) {
      throw new Error(`Build cache destination exists but is not a file: ${destination}`);
    }
    if (!replaceExisting) {
      throw new Error(
        `Refusing to overwrite an existing Build .spc: ${destination}. `
        + "Set replaceExistingBuildCaches=true to back it up and replace it transactionally.",
      );
    }
  }
}

function publishSpcTransaction(
  outputSpc: string,
  runDirectory: string,
  destinations: string[],
  replaceExisting: boolean,
): Array<{ path: string; bytes: number; replaced: boolean; backupPath: string | null }> {
  assertBuildPublicationAllowed(destinations, replaceExisting);
  const outputBytes = fs.statSync(outputSpc).size;
  const token = path.basename(runDirectory).replace(/[^A-Za-z0-9_-]/g, "_");
  const pending: Array<{
    destination: string;
    tempPath: string;
    rollbackPath: string;
    existed: boolean;
    backupPath: string | null;
    movedOld: boolean;
    installed: boolean;
  }> = [];
  try {
    for (const destination of destinations) {
      const existed = fs.existsSync(destination);
      const directory = path.dirname(destination);
      fs.mkdirSync(directory, { recursive: true });
      const tempPath = path.join(directory, `.${path.basename(destination)}.${token}.tmp`);
      const rollbackPath = path.join(directory, `.${path.basename(destination)}.${token}.rollback`);
      if (fs.existsSync(tempPath) || fs.existsSync(rollbackPath)) {
        throw new Error(`Refusing to reuse stale PSO publication scratch files beside ${destination}.`);
      }
      fs.copyFileSync(outputSpc, tempPath);
      if (fs.statSync(tempPath).size !== outputBytes) {
        fs.rmSync(tempPath, { force: true });
        throw new Error(`Prepared .spc size mismatch at ${tempPath}.`);
      }

      const target = {
        destination,
        tempPath,
        rollbackPath,
        existed,
        backupPath: null as string | null,
        movedOld: false,
        installed: false,
      };
      pending.push(target);
      if (existed) {
        const platformName = path.basename(path.dirname(path.dirname(destination)));
        const backupDirectory = path.join(runDirectory, "PreviousBuildCaches", platformName);
        fs.mkdirSync(backupDirectory, { recursive: true });
        target.backupPath = path.join(backupDirectory, path.basename(destination));
        fs.copyFileSync(destination, target.backupPath);
        if (fs.statSync(target.backupPath).size !== fs.statSync(destination).size) {
          fs.rmSync(tempPath, { force: true });
          throw new Error(`Backup size mismatch for existing Build cache: ${destination}`);
        }
      }
    }
  } catch (error) {
    for (const target of pending) {
      if (fs.existsSync(target.tempPath)) fs.rmSync(target.tempPath, { force: true });
    }
    throw error;
  }

  try {
    for (const target of pending) {
      if (target.existed) {
        fs.renameSync(target.destination, target.rollbackPath);
        target.movedOld = true;
      }
      fs.renameSync(target.tempPath, target.destination);
      target.installed = true;
    }
  } catch (error) {
    for (const target of [...pending].reverse()) {
      if (target.installed && fs.existsSync(target.destination)) {
        fs.rmSync(target.destination, { force: true });
      }
      if (target.movedOld && fs.existsSync(target.rollbackPath)) {
        fs.renameSync(target.rollbackPath, target.destination);
      }
      if (fs.existsSync(target.tempPath)) fs.rmSync(target.tempPath, { force: true });
    }
    throw error;
  }

  for (const target of pending) {
    if (fs.existsSync(target.rollbackPath)) fs.rmSync(target.rollbackPath, { force: true });
  }
  return pending.map((target) => ({
    path: target.destination,
    bytes: fs.statSync(target.destination).size,
    replaced: target.existed,
    backupPath: target.backupPath,
  }));
}

function editorCommandForPlatform(enginePath: string, platform: NodeJS.Platform): string {
  if (platform === "win32") {
    return path.join(enginePath, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe");
  }
  if (platform === "darwin") {
    return path.join(enginePath, "Engine", "Binaries", "Mac", "UnrealEditor-Cmd");
  }
  return path.join(enginePath, "Engine", "Binaries", "Linux", "UnrealEditor-Cmd");
}

export function parseAdbDevices(output: string): AdbDevice[] {
  const devices: AdbDevice[] = [];
  for (const rawLine of output.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("List of devices") || line.startsWith("*")) continue;
    const tokens = line.split(/\s+/);
    if (tokens.length < 2) continue;
    const details: Record<string, string> = {};
    for (const token of tokens.slice(2)) {
      const separator = token.indexOf(":");
      if (separator > 0) details[token.slice(0, separator)] = token.slice(separator + 1);
    }
    devices.push({ serial: tokens[0], state: tokens[1], details });
  }
  return devices;
}

export function selectExactlyOneDevice(output: string): AdbDevice {
  const devices = parseAdbDevices(output);
  if (devices.length !== 1) {
    const summary = devices.length === 0
      ? "none"
      : devices.map((device) => `${device.serial} (${device.state})`).join(", ");
    throw new Error(`Quest PSO operations require exactly one adb device; found ${devices.length}: ${summary}`);
  }
  if (devices[0].state !== "device") {
    throw new Error(`The only adb device is not ready: ${devices[0].serial} (${devices[0].state}).`);
  }
  return devices[0];
}

export function questPsoRemotePaths(packageName: string, projectName: string): QuestPsoRemotePaths {
  const safePackage = assertSafeIdentifier(packageName, "packageName", /^[A-Za-z0-9_.]+$/);
  const safeProject = assertSafeIdentifier(projectName, "projectName", /^[A-Za-z0-9_]+$/);
  const projectDir = `/sdcard/Android/data/${safePackage}/files/UnrealGame/${safeProject}`;
  const gameDir = `${projectDir}/${safeProject}`;
  const savedDir = `${gameDir}/Saved`;
  return {
    projectDir,
    gameDir,
    savedDir,
    collectedPsos: `${savedDir}/CollectedPSOs`,
    writablePipelineCache: `${savedDir}/${safeProject}_SF_VULKAN_ES31_ANDROID.upipelinecache`,
    vulkanProgramBinaryCache: `/sdcard/Android/data/${safePackage}/files/VulkanProgramBinaryCache`,
  };
}

function markerFields(line: string): Record<string, string> {
  const fields: Record<string, string> = {};
  for (const match of line.matchAll(/\b([A-Za-z][A-Za-z0-9_]*)=([^\s]+)/g)) {
    fields[match[1]] = match[2];
  }
  return fields;
}

function integerField(fields: Record<string, string>, name: string): number | null {
  if (!(name in fields) || !/^-?\d+$/.test(fields[name])) return null;
  const value = Number(fields[name]);
  return Number.isSafeInteger(value) ? value : null;
}

export function parseHarvestLog(logText: string): HarvestValidation {
  const markerLines = logText
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.includes("RQPSO MARKER"));

  let startIndex = -1;
  for (let index = markerLines.length - 1; index >= 0; --index) {
    if (/\bRQPSO MARKER TourStart\b/.test(markerLines[index])) {
      startIndex = index;
      break;
    }
  }

  const runLines = startIndex >= 0 ? markerLines.slice(startIndex) : [];
  const startMarker = startIndex >= 0 ? markerLines[startIndex] : null;
  const doneIndex = runLines.findIndex((line) => /\bRQPSO MARKER TourDone\b/.test(line));
  const doneMarker = doneIndex >= 0 ? runLines[doneIndex] : null;
  const boundedLines = doneIndex >= 0 ? runLines.slice(0, doneIndex + 1) : runLines;
  const levelDoneLines = boundedLines.filter((line) => /\bRQPSO MARKER LevelDone\b/.test(line));
  const tourErrors = boundedLines.filter((line) => /\bRQPSO MARKER TourError\b/.test(line));
  const startFields = startMarker ? markerFields(startMarker) : {};
  const doneFields = doneMarker ? markerFields(doneMarker) : {};
  const expectedLevels = integerField(startFields, "levels");
  const completedLevels = integerField(doneFields, "levels");
  const psosLogged = integerField(doneFields, "psosLogged");
  const levelDoneIndices = [...new Set(levelDoneLines
    .map((line) => integerField(markerFields(line), "index"))
    .filter((value): value is number => value !== null))].sort((left, right) => left - right);

  const reasons: string[] = [];
  if (!startMarker) reasons.push("missing TourStart marker");
  if (!doneMarker) reasons.push("missing TourDone marker after the latest TourStart");
  if (expectedLevels === null || expectedLevels < 1) reasons.push("TourStart levels must be at least 1");
  if (doneMarker && doneFields.ok !== "1") reasons.push("TourDone did not report ok=1");
  if (completedLevels === null) reasons.push("TourDone is missing a valid levels count");
  if (expectedLevels !== null && completedLevels !== null && completedLevels !== expectedLevels) {
    reasons.push(`TourDone levels=${completedLevels} does not match TourStart levels=${expectedLevels}`);
  }
  if (expectedLevels !== null && levelDoneLines.length !== expectedLevels) {
    reasons.push(`LevelDone count=${levelDoneLines.length} does not match expected levels=${expectedLevels}`);
  }
  if (expectedLevels !== null && levelDoneIndices.length !== expectedLevels) {
    reasons.push(`LevelDone unique index count=${levelDoneIndices.length} does not match expected levels=${expectedLevels}`);
  }
  if (
    expectedLevels !== null
    && levelDoneIndices.length === expectedLevels
    && !levelDoneIndices.every((value, index) => value === index)
  ) {
    reasons.push(`LevelDone indices must be exactly 0..${expectedLevels - 1}; got ${levelDoneIndices.join(",")}`);
  }
  const startPass = startFields.pass ?? null;
  if (startMarker && !startPass) reasons.push("TourStart is missing a pass identifier");
  if (startPass) {
    const mismatchedPassMarkers = boundedLines.filter((line) => {
      if (!/\bRQPSO MARKER (TourStart|LevelDone|TourError|TourDone)\b/.test(line)) return false;
      return markerFields(line).pass !== startPass;
    });
    if (mismatchedPassMarkers.length > 0) {
      reasons.push(`${mismatchedPassMarkers.length} marker(s) do not match TourStart pass=${startPass}`);
    }
  }
  if (psosLogged === null || psosLogged < 1) reasons.push("TourDone psosLogged must be at least 1");
  if (tourErrors.length > 0) reasons.push(`harvest emitted ${tourErrors.length} TourError marker(s)`);

  return {
    valid: reasons.length === 0,
    markerCount: markerLines.length,
    reasons,
    expectedLevels,
    completedLevels,
    levelDoneCount: levelDoneLines.length,
    levelDoneIndices,
    psosLogged,
    tourErrors,
    startMarker,
    doneMarker,
  };
}

export class QuestPsoHost {
  private readonly runCommand: CommandRunner;
  private readonly sendLine: LineSender;
  private readonly now: () => Date;
  private readonly platform: NodeJS.Platform;

  constructor(dependencies: QuestPsoDependencies = {}) {
    this.runCommand = dependencies.runCommand ?? checkedExecFile;
    this.sendLine = dependencies.sendLine ?? sendTcpLine;
    this.now = dependencies.now ?? (() => new Date());
    this.platform = dependencies.platform ?? process.platform;
  }

  private async oneDevice(adbPath: string): Promise<AdbDevice> {
    const result = await this.runCommand(adbPath, ["devices", "-l"]);
    return selectExactlyOneDevice(result.stdout);
  }

  private async adb(
    adbPath: string,
    serial: string,
    args: string[],
    options?: CommandOptions,
  ): Promise<CommandResult> {
    return this.runCommand(adbPath, ["-s", serial, ...args], options);
  }

  private async setLaunchCommandLine(adbPath: string, serial: string, commandLine: string): Promise<void> {
    const remoteCommand = `setprop debug.ue.commandline ${quoteRemoteShellArg(commandLine)}`;
    await this.adb(adbPath, serial, ["shell", remoteCommand]);
  }

  private async stopAndClearLaunch(
    adbPath: string,
    serial: string,
    packageName: string,
    bestEffort = false,
  ): Promise<void> {
    if (bestEffort) {
      await this.adb(adbPath, serial, ["shell", "am", "force-stop", packageName]).catch(() => undefined);
      await this.setLaunchCommandLine(adbPath, serial, "").catch(() => undefined);
      return;
    }
    await this.adb(adbPath, serial, ["shell", "am", "force-stop", packageName]);
    await this.setLaunchCommandLine(adbPath, serial, "");
  }

  async preflight(setup: QuestPsoSetup): Promise<QuestPsoPreflight> {
    const projectPath = assertAbsoluteExistingFile(setup.projectPath, "projectPath");
    if (path.extname(projectPath).toLowerCase() !== ".uproject") {
      throw new Error(`projectPath must point to a .uproject file: ${projectPath}`);
    }
    const enginePath = assertAbsoluteExistingDirectory(setup.enginePath, "enginePath");
    const packageName = assertSafeIdentifier(setup.packageName, "packageName", /^[A-Za-z0-9_.]+$/);
    const adbPath = setup.adbPath?.trim() || "adb";
    const adbVersionResult = await this.runCommand(adbPath, ["version"]);
    const device = await this.oneDevice(adbPath);
    const packageResult = await this.adb(adbPath, device.serial, ["shell", "pm", "path", packageName]);
    const packagePath = packageResult.stdout.trim();
    if (!packagePath.startsWith("package:")) {
      throw new Error(`Android package ${packageName} is not installed on ${device.serial}.`);
    }

    const projectDir = path.dirname(projectPath);
    const projectName = assertSafeIdentifier(
      path.basename(projectPath, path.extname(projectPath)),
      "projectName",
      /^[A-Za-z0-9_]+$/,
    );
    const editorCommandPath = editorCommandForPlatform(enginePath, this.platform);
    if (!fs.existsSync(editorCommandPath) || !fs.statSync(editorCommandPath).isFile()) {
      throw new Error(`UnrealEditor-Cmd was not found under the explicit enginePath: ${editorCommandPath}`);
    }

    const cookedPipelineDirectory = path.join(
      projectDir,
      "Saved",
      "Cooked",
      "Android_ASTC",
      projectName,
      "Metadata",
      "PipelineCaches",
    );
    const stableShaderKeys = findFiles(cookedPipelineDirectory, ".shk", false);
    if (stableShaderKeys.length === 0) {
      throw new Error(`No stable shader key (.shk) files were found in ${cookedPipelineDirectory}. Run an Android_ASTC cook first.`);
    }

    const bundledDirectories = [
      path.join(projectDir, "Build", "Android", "PipelineCaches"),
      path.join(projectDir, "Build", "Android_ASTC", "PipelineCaches"),
    ];
    const bundledPipelineCaches = bundledDirectories.flatMap((directory) => findFiles(directory, ".spc", false));
    const warnings = bundledPipelineCaches.length > 0
      ? ["Bundled .spc files exist. If the installed APK was cooked with them, -logPSO records only cache misses rather than a full regeneration."]
      : [];

    return {
      device,
      adbPath,
      adbVersion: adbVersionResult.stdout.trim().split(/\r?\n/)[0] ?? "",
      packagePath,
      projectPath,
      projectDir,
      projectName,
      packageName,
      enginePath,
      editorCommandPath,
      cookedPipelineDirectory,
      stableShaderKeys,
      bundledPipelineCaches,
      warnings,
      remote: questPsoRemotePaths(packageName, projectName),
    };
  }

  async clearAndLaunch(
    setup: QuestPsoSetup & { launchArgs?: string[]; devicePort?: number; allowBundledCacheRisk?: boolean },
  ): Promise<Record<string, unknown>> {
    const preflight = await this.preflight(setup);
    if (preflight.bundledPipelineCaches.length > 0 && setup.allowBundledCacheRisk !== true) {
      throw new Error(
        `Refusing a potentially partial recording because bundled .spc files exist: ${preflight.bundledPipelineCaches.join(", ")}. `
        + "Remove them and recook/reinstall, or set allowBundledCacheRisk=true for an intentional miss-only capture.",
      );
    }
    const devicePort = assertPort(setup.devicePort ?? DEFAULT_DEVICE_PORT, "devicePort");
    const launchArgs = setup.launchArgs ?? [];
    for (const arg of launchArgs) {
      if (!arg || /[\r\n\0]/.test(arg)) throw new Error("launchArgs must be non-empty single-line arguments.");
      if (arg.toLowerCase() === "-nologpso") throw new Error("launchArgs cannot disable PSO logging with -NoLogPSO.");
    }
    const commandLineParts = ["-logPSO", `-rqdevport=${devicePort}`];
    for (const arg of launchArgs) {
      if (!commandLineParts.some((existing) => existing.toLowerCase() === arg.toLowerCase())) {
        commandLineParts.push(arg);
      }
    }
    const commandLine = commandLineParts.join(" ");
    const { adbPath, device, remote } = preflight;

    await this.adb(adbPath, device.serial, ["shell", "am", "force-stop", preflight.packageName]);
    await this.adb(adbPath, device.serial, ["shell", "rm", "-rf", remote.collectedPsos]);
    await this.adb(adbPath, device.serial, ["shell", "rm", "-f", remote.writablePipelineCache]);
    await this.adb(adbPath, device.serial, ["shell", "rm", "-rf", remote.vulkanProgramBinaryCache]);
    const warnings = [...preflight.warnings];
    try {
      await this.adb(adbPath, device.serial, ["logcat", "-G", "16M"]);
    } catch (error) {
      warnings.push(`Could not enlarge logcat to 16 MiB: ${error instanceof Error ? error.message : String(error)}`);
    }
    await this.adb(adbPath, device.serial, ["logcat", "-c"]);
    await this.setLaunchCommandLine(adbPath, device.serial, commandLine);
    try {
      await this.adb(adbPath, device.serial, [
        "shell",
        "monkey",
        "-p",
        preflight.packageName,
        "-c",
        "android.intent.category.LAUNCHER",
        "1",
      ]);
    } catch (error) {
      await this.setLaunchCommandLine(adbPath, device.serial, "").catch(() => undefined);
      throw error;
    }

    return {
      launched: true,
      device: preflight.device,
      packageName: preflight.packageName,
      commandLine,
      cleared: [remote.collectedPsos, remote.writablePipelineCache, remote.vulkanProgramBinaryCache],
      warnings,
      allowBundledCacheRisk: setup.allowBundledCacheRisk === true,
    };
  }

  async sendCommand(options: {
    adbPath?: string;
    command: string;
    devicePort?: number;
    timeoutMs?: number;
  }): Promise<RqCommandResult> {
    const adbPath = options.adbPath?.trim() || "adb";
    const command = options.command.trim();
    if (!command || /[\r\n\0]/.test(command)) {
      throw new Error("command must be one non-empty line.");
    }
    if (Buffer.byteLength(command, "utf8") > MAX_DEV_COMMAND_BYTES) {
      throw new Error(`command exceeds ${MAX_DEV_COMMAND_BYTES} UTF-8 bytes.`);
    }
    const devicePort = assertPort(options.devicePort ?? DEFAULT_DEVICE_PORT, "devicePort");
    const timeoutMs = Math.max(100, Math.min(options.timeoutMs ?? 20_000, 120_000));
    const device = await this.oneDevice(adbPath);
    const forward = await this.adb(adbPath, device.serial, ["forward", "tcp:0", `tcp:${devicePort}`]);
    const hostPort = Number(forward.stdout.trim());
    if (!Number.isInteger(hostPort) || hostPort < 1 || hostPort > 65535) {
      throw new Error(`adb did not return a valid ephemeral forwarded port: ${forward.stdout.trim()}`);
    }

    try {
      const rawResponse = await this.sendLine("127.0.0.1", hostPort, command, timeoutMs);
      let parsed: unknown;
      try {
        parsed = JSON.parse(rawResponse);
      } catch {
        throw new Error(`RQDevServer returned invalid JSON: ${rawResponse.slice(0, 500)}`);
      }
      if (
        !parsed
        || typeof parsed !== "object"
        || typeof (parsed as { ok?: unknown }).ok !== "boolean"
        || typeof (parsed as { output?: unknown }).output !== "string"
      ) {
        throw new Error("RQDevServer response must be a JSON object with boolean ok and string output fields.");
      }
      const response = parsed as RqDevServerResponse;
      const commandError = /^error:\s*/i.test(response.output.trim()) ? response.output.trim() : null;
      return { ...response, command, commandError, deviceSerial: device.serial, devicePort, hostPort };
    } finally {
      await this.adb(adbPath, device.serial, ["forward", "--remove", `tcp:${hostPort}`]).catch(() => undefined);
    }
  }

  async saveRecordedPso(options: {
    adbPath?: string;
    devicePort?: number;
    timeoutMs?: number;
  } = {}): Promise<RqCommandResult> {
    const result = await this.sendCommand({ ...options, command: "r.ShaderPipelineCache.Save" });
    if (!result.ok || result.commandError) {
      throw new Error(`RQDevServer rejected r.ShaderPipelineCache.Save: ${result.commandError ?? result.output}`);
    }
    return result;
  }

  private async readHarvestValidation(adbPath: string, serial: string): Promise<HarvestValidation & { deviceSerial: string }> {
    const logcat = await this.adb(
      adbPath,
      serial,
      ["logcat", "-d", "-s", "UE"],
      { maxBufferBytes: 64 * 1024 * 1024 },
    );
    return { ...parseHarvestLog(logcat.stdout), deviceSerial: serial };
  }

  async validateHarvest(options: { adbPath?: string } = {}): Promise<HarvestValidation & { deviceSerial: string }> {
    const adbPath = options.adbPath?.trim() || "adb";
    const device = await this.oneDevice(adbPath);
    return this.readHarvestValidation(adbPath, device.serial);
  }

  async collectExpand(options: CollectExpandOptions): Promise<Record<string, unknown>> {
    const preflight = await this.preflight(options);
    const spcName = `${preflight.projectName}_SF_VULKAN_ES31_ANDROID.spc`;
    const copyToBuild = options.copyToBuild !== false;
    const replaceExistingBuildCaches = options.replaceExistingBuildCaches === true;
    const buildDestinations = copyToBuild
      ? buildCacheDestinations(preflight.projectDir, spcName)
      : [];
    // Refuse before saving/stopping/pulling so an unacknowledged overwrite
    // cannot mutate either the device session or the Build tree.
    assertBuildPublicationAllowed(buildDestinations, replaceExistingBuildCaches);

    // Publishing fails closed by default: callers must either provide a
    // complete RQPSO tour or explicitly opt out for legacy/no-tour builds.
    const requireCompleteTour = options.requireCompleteTour !== false;
    let tourValidationMode = requireCompleteTour ? "required" : "disabled";
    let harvestValidation: (HarvestValidation & { deviceSerial: string }) | null = null;
    let saveResult: RqCommandResult | null = null;
    try {
      if (requireCompleteTour) {
        const validation = await this.readHarvestValidation(preflight.adbPath, preflight.device.serial);
        harvestValidation = validation;
        if (!validation.valid) {
          throw new Error(`Refusing to publish a partial PSO harvest: ${validation.reasons.join("; ")}`);
        }
      }

      if (options.saveBeforePull !== false) {
        saveResult = await this.saveRecordedPso({
          adbPath: preflight.adbPath,
          devicePort: options.devicePort,
          timeoutMs: options.commandTimeoutMs,
        });
      }
    } catch (error) {
      await this.stopAndClearLaunch(preflight.adbPath, preflight.device.serial, preflight.packageName, true);
      throw error;
    }

    await this.stopAndClearLaunch(preflight.adbPath, preflight.device.serial, preflight.packageName);

    const outputRoot = options.outputRoot
      ? assertAbsoluteDirectoryTarget(options.outputRoot, "outputRoot")
      : path.join(preflight.projectDir, "PSOCache");
    fs.mkdirSync(outputRoot, { recursive: true });
    const runName = `harvest-${this.now().toISOString().replace(/[:.]/g, "-")}`;
    const runDirectory = path.join(outputRoot, runName);
    const collectedDirectory = path.join(runDirectory, "CollectedPSOs");
    const stableDirectory = path.join(runDirectory, "StableKeys");
    fs.mkdirSync(runDirectory, { recursive: false });
    fs.mkdirSync(stableDirectory, { recursive: false });

    await this.adb(
      preflight.adbPath,
      preflight.device.serial,
      ["pull", preflight.remote.collectedPsos, collectedDirectory],
      { timeoutMs: 10 * 60_000, maxBufferBytes: 64 * 1024 * 1024 },
    );
    const recordingFiles = findFiles(collectedDirectory, ".upipelinecache", true);
    if (recordingFiles.length === 0) {
      throw new Error(`No .upipelinecache recordings were pulled from ${preflight.remote.collectedPsos}.`);
    }

    const stableShaderKeys = preflight.stableShaderKeys.map((source) => {
      const destination = path.join(stableDirectory, path.basename(source));
      fs.copyFileSync(source, destination);
      return destination;
    });
    const outputSpc = path.join(runDirectory, spcName);
    const expandArgs = [
      preflight.projectPath,
      "-run=ShaderPipelineCacheTools",
      "Expand",
      ...recordingFiles,
      ...stableShaderKeys,
      outputSpc,
    ];
    const expandResult = await this.runCommand(
      preflight.editorCommandPath,
      expandArgs,
      { cwd: preflight.projectDir, timeoutMs: 20 * 60_000, maxBufferBytes: 64 * 1024 * 1024 },
    );
    if (!fs.existsSync(outputSpc) || !fs.statSync(outputSpc).isFile() || fs.statSync(outputSpc).size < 1) {
      throw new Error(`ShaderPipelineCacheTools reported success but did not create a non-empty .spc: ${outputSpc}`);
    }

    const outputBytes = fs.statSync(outputSpc).size;
    const copiedTo = copyToBuild
      ? publishSpcTransaction(
        outputSpc,
        runDirectory,
        buildDestinations,
        replaceExistingBuildCaches,
      )
      : [];

    return {
      success: true,
      device: preflight.device,
      harvestValidation,
      tourValidationMode,
      saveCommand: saveResult,
      runDirectory,
      pulledFrom: preflight.remote.collectedPsos,
      recordingFiles,
      recordingCount: recordingFiles.length,
      stableShaderKeys,
      stableShaderKeyCount: stableShaderKeys.length,
      editorCommandPath: preflight.editorCommandPath,
      projectPath: preflight.projectPath,
      outputSpc,
      outputBytes,
      copiedTo,
      replaceExistingBuildCaches,
      expandOutputTail: expandResult.stdout.trim().slice(-4000),
      warnings: preflight.warnings,
    };
  }
}

export const questPsoHost = new QuestPsoHost();
