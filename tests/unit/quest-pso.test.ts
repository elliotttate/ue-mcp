import { afterEach, describe, expect, it } from "vitest";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import {
  parseHarvestLog,
  quoteRemoteShellArg,
  QuestPsoHost,
  selectExactlyOneDevice,
  type CommandRunner,
  type LineSender,
} from "../../src/quest-pso.js";
import { questPsoTool } from "../../src/tools/quest_pso.js";

const tempDirectories: string[] = [];

afterEach(() => {
  for (const directory of tempDirectories.splice(0)) {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

function makeFixture(): {
  root: string;
  projectPath: string;
  enginePath: string;
  editorPath: string;
} {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-quest-pso-"));
  tempDirectories.push(root);
  const projectPath = path.join(root, "RoboquestVR.uproject");
  fs.writeFileSync(projectPath, JSON.stringify({ FileVersion: 3 }));
  const enginePath = path.join(root, "tapir-unreal-engine");
  const editorPath = path.join(enginePath, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe");
  fs.mkdirSync(path.dirname(editorPath), { recursive: true });
  fs.writeFileSync(editorPath, "test editor placeholder");
  const stableDirectory = path.join(
    root,
    "Saved",
    "Cooked",
    "Android_ASTC",
    "RoboquestVR",
    "Metadata",
    "PipelineCaches",
  );
  fs.mkdirSync(stableDirectory, { recursive: true });
  fs.writeFileSync(path.join(stableDirectory, "ShaderStableInfo-Global.shk"), "global keys");
  fs.writeFileSync(path.join(stableDirectory, "ShaderStableInfo-RoboquestVR.shk"), "project keys");
  return { root, projectPath, enginePath, editorPath };
}

function readyDeviceOutput(): string {
  return [
    "List of devices attached",
    "1WMHH123456789 device product:hollywood model:Quest_3 device:hollywood transport_id:1",
    "",
  ].join("\n");
}

function baseAdbResult(args: string[]): { stdout: string; stderr: string } | null {
  if (args[0] === "version") return { stdout: "Android Debug Bridge version 1.0.41\n", stderr: "" };
  if (args[0] === "devices") return { stdout: readyDeviceOutput(), stderr: "" };
  if (args.includes("pm") && args.includes("path")) {
    return { stdout: "package:/data/app/com.Flat2VRStudios.RoboquestVR/base.apk\n", stderr: "" };
  }
  return null;
}

describe("Quest PSO adb preflight", () => {
  it("quotes multiword and apostrophe-containing values for adb remote shell", () => {
    expect(quoteRemoteShellArg("-logPSO -rqdevport=6788")).toBe("'-logPSO -rqdevport=6788'");
    expect(quoteRemoteShellArg("alpha'beta")).toBe("'alpha'\\''beta'");
  });

  it("requires exactly one ready device, including unauthorized/offline rows", () => {
    expect(selectExactlyOneDevice(readyDeviceOutput()).serial).toBe("1WMHH123456789");
    expect(() => selectExactlyOneDevice([
      "List of devices attached",
      "quest-a device",
      "quest-b unauthorized",
    ].join("\n"))).toThrow(/exactly one adb device/);
    expect(() => selectExactlyOneDevice("List of devices attached\nquest unauthorized\n"))
      .toThrow(/not ready/);
  });

  it("checks explicit project/engine paths, installed package, cooked keys, and bundled-cache risk", async () => {
    const fixture = makeFixture();
    const bundled = path.join(fixture.root, "Build", "Android", "PipelineCaches", "old.spc");
    fs.mkdirSync(path.dirname(bundled), { recursive: true });
    fs.writeFileSync(bundled, "old cache");
    const runner: CommandRunner = async (_file, args) => baseAdbResult(args) ?? { stdout: "", stderr: "" };
    const host = new QuestPsoHost({ runCommand: runner, platform: "win32" });

    const result = await host.preflight({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
    });

    expect(result.device.serial).toBe("1WMHH123456789");
    expect(result.editorCommandPath).toBe(fixture.editorPath);
    expect(result.stableShaderKeys).toHaveLength(2);
    expect(result.bundledPipelineCaches).toEqual([bundled]);
    expect(result.warnings[0]).toMatch(/records only cache misses/);
    await expect(host.clearAndLaunch({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
    })).rejects.toThrow(/allowBundledCacheRisk=true/);
  });
});

describe("Quest PSO RQDevServer protocol", () => {
  it("uses an ephemeral adb forward, sends the explicit save command, and removes the forward", async () => {
    const calls: string[][] = [];
    const runner: CommandRunner = async (_file, args) => {
      calls.push(args);
      if (args[0] === "devices") return { stdout: readyDeviceOutput(), stderr: "" };
      if (args.includes("forward") && args.includes("tcp:0")) return { stdout: "49152\n", stderr: "" };
      return { stdout: "", stderr: "" };
    };
    const sent: Array<{ host: string; port: number; line: string }> = [];
    const sender: LineSender = async (host, port, line) => {
      sent.push({ host, port, line });
      return JSON.stringify({ ok: true, output: "" });
    };
    const host = new QuestPsoHost({ runCommand: runner, sendLine: sender });

    const result = await host.saveRecordedPso();

    expect(result.ok).toBe(true);
    expect(result.commandError).toBeNull();
    expect(sent).toEqual([{ host: "127.0.0.1", port: 49152, line: "r.ShaderPipelineCache.Save" }]);
    expect(calls).toContainEqual(["-s", "1WMHH123456789", "forward", "tcp:0", "tcp:6788"]);
    expect(calls).toContainEqual(["-s", "1WMHH123456789", "forward", "--remove", "tcp:49152"]);
  });
});

describe("Quest PSO harvest validation", () => {
  it("accepts only a complete latest run with unique LevelDone indices and nonzero PSOs", () => {
    const valid = parseHarvestLog([
      "old RQPSO MARKER TourStart pass=old levels=1",
      "old RQPSO MARKER TourError pass=old reason=aborted",
      "UE RQPSO MARKER TourStart pass=main levels=2 seed=123",
      "UE RQPSO MARKER LevelDone pass=main level=A index=0 psosLogged=17",
      "UE RQPSO MARKER LevelDone pass=main level=B index=1 psosLogged=24",
      "UE RQPSO MARKER TourDone pass=main ok=1 levels=2 psosLogged=24",
    ].join("\n"));
    expect(valid.valid).toBe(true);
    expect(valid.markerCount).toBe(6);
    expect(valid.levelDoneIndices).toEqual([0, 1]);
    expect(valid.psosLogged).toBe(24);

    const partial = parseHarvestLog([
      "UE RQPSO MARKER TourStart pass=main levels=2 seed=123",
      "UE RQPSO MARKER LevelDone pass=main level=A index=0 psosLogged=17",
      "UE RQPSO MARKER TourDone pass=main ok=1 levels=2 psosLogged=17",
    ].join("\n"));
    expect(partial.valid).toBe(false);
    expect(partial.reasons.join(" ")).toMatch(/LevelDone count=1/);

    const wrongIndices = parseHarvestLog([
      "UE RQPSO MARKER TourStart pass=main levels=2 seed=123",
      "UE RQPSO MARKER LevelDone pass=main level=A index=5 psosLogged=17",
      "UE RQPSO MARKER LevelDone pass=main level=B index=6 psosLogged=24",
      "UE RQPSO MARKER TourDone pass=main ok=1 levels=2 psosLogged=24",
    ].join("\n"));
    expect(wrongIndices.valid).toBe(false);
    expect(wrongIndices.reasons.join(" ")).toMatch(/exactly 0\.\.1/);
  });
});

describe("Quest PSO collection and expansion", () => {
  it("auto-detects any marker-capable run and refuses partial coverage before save/pull", async () => {
    const fixture = makeFixture();
    const calls: string[][] = [];
    const runner: CommandRunner = async (_file, args) => {
      calls.push(args);
      const base = baseAdbResult(args);
      if (base) return base;
      if (args.includes("logcat") && args.includes("-d")) {
        return {
          stdout: [
            "UE RQPSO MARKER TourStart pass=main levels=2 seed=123",
            "UE RQPSO MARKER LevelDone pass=main level=A index=0 psosLogged=17",
          ].join("\n"),
          stderr: "",
        };
      }
      return { stdout: "", stderr: "" };
    };
    const sender: LineSender = async () => {
      throw new Error("save must not run after failed marker validation");
    };
    const host = new QuestPsoHost({ runCommand: runner, sendLine: sender, platform: "win32" });

    await expect(host.collectExpand({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
    })).rejects.toThrow(/Refusing to publish a partial PSO harvest/);
    expect(calls.some((args) => args.includes("pull"))).toBe(false);
    expect(calls.some((args) => args.at(-1) === "setprop debug.ue.commandline ''")).toBe(true);
  });

  it("launches with -logPSO after clearing all device-side caches", async () => {
    const fixture = makeFixture();
    const calls: string[][] = [];
    const runner: CommandRunner = async (_file, args) => {
      calls.push(args);
      return baseAdbResult(args) ?? { stdout: "", stderr: "" };
    };
    const host = new QuestPsoHost({ runCommand: runner, platform: "win32" });

    const result = await host.clearAndLaunch({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
      launchArgs: ["-gauntlet=VRAutomationGauntletController", "-scenario=PSOHarvest", "-unattended"],
    });

    expect(result.launched).toBe(true);
    const setprop = calls.find((args) => args.some((arg) => arg.startsWith("setprop debug.ue.commandline")));
    expect(setprop?.at(-1)).toContain("'-logPSO -rqdevport=6788");
    expect(setprop?.at(-1)).toContain("-scenario=PSOHarvest");
    expect(calls.some((args) => args.some((arg) => arg.endsWith("/CollectedPSOs")) && args.includes("-rf"))).toBe(true);
    expect(calls.some((args) => args.some((arg) => arg.includes("VulkanProgramBinaryCache")))).toBe(true);
    expect(calls).toContainEqual(["-s", "1WMHH123456789", "logcat", "-G", "16M"]);
    expect(calls.some((args) => args.includes("monkey"))).toBe(true);
  });

  it("pulls every recording, expands using explicit paths, validates output, and copies both Build caches", async () => {
    const fixture = makeFixture();
    const calls: Array<{ file: string; args: string[] }> = [];
    const runner: CommandRunner = async (file, args) => {
      calls.push({ file, args });
      const base = baseAdbResult(args);
      if (base) return base;
      if (args.includes("forward") && args.includes("tcp:0")) return { stdout: "49153\n", stderr: "" };
      const pullIndex = args.indexOf("pull");
      if (pullIndex >= 0) {
        const destination = args[pullIndex + 2];
        fs.mkdirSync(path.join(destination, "nested"), { recursive: true });
        fs.writeFileSync(path.join(destination, "first.upipelinecache"), "first recording");
        fs.writeFileSync(path.join(destination, "nested", "second.upipelinecache"), "second recording");
        return { stdout: "2 files pulled", stderr: "" };
      }
      if (file === fixture.editorPath) {
        fs.writeFileSync(args.at(-1)!, "expanded stable cache");
        return { stdout: "Expanded 2 pipeline caches", stderr: "" };
      }
      return { stdout: "", stderr: "" };
    };
    const sender: LineSender = async (_host, _port, line) => {
      expect(line).toBe("r.ShaderPipelineCache.Save");
      return JSON.stringify({ ok: true, output: "saved" });
    };
    const host = new QuestPsoHost({
      runCommand: runner,
      sendLine: sender,
      platform: "win32",
      now: () => new Date("2026-07-23T12:34:56.000Z"),
    });

    const result = await host.collectExpand({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
    });

    expect(result.recordingCount).toBe(2);
    expect(result.stableShaderKeyCount).toBe(2);
    expect(result.outputBytes).toBeGreaterThan(0);
    expect(result.copiedTo).toHaveLength(2);
    const expand = calls.find((call) => call.file === fixture.editorPath)!;
    expect(expand.args[0]).toBe(fixture.projectPath);
    expect(expand.args).toContain("-run=ShaderPipelineCacheTools");
    expect(expand.args.filter((arg) => arg.endsWith(".upipelinecache"))).toHaveLength(2);
    expect(expand.args.filter((arg) => arg.endsWith(".shk"))).toHaveLength(2);

    await expect(host.collectExpand({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
    })).rejects.toThrow(/replaceExistingBuildCaches=true/);

    const replacementHost = new QuestPsoHost({
      runCommand: runner,
      sendLine: sender,
      platform: "win32",
      now: () => new Date("2026-07-23T12:35:56.000Z"),
    });
    const replacement = await replacementHost.collectExpand({
      projectPath: fixture.projectPath,
      enginePath: fixture.enginePath,
      packageName: "com.Flat2VRStudios.RoboquestVR",
      replaceExistingBuildCaches: true,
    });
    const replacementCopies = replacement.copiedTo as Array<{ replaced: boolean; backupPath: string | null }>;
    expect(replacementCopies.every((copy) => copy.replaced && copy.backupPath && fs.existsSync(copy.backupPath))).toBe(true);
  });
});

describe("quest_pso MCP surface", () => {
  it("registers the bounded host-side actions and validates port/path shapes", () => {
    for (const action of ["preflight", "clear_launch", "command", "save", "validate", "collect_expand"]) {
      expect(questPsoTool.schema.action.safeParse(action).success).toBe(true);
    }
    expect(questPsoTool.schema.devicePort.safeParse(6788).success).toBe(true);
    expect(questPsoTool.schema.devicePort.safeParse(70000).success).toBe(false);
  });
});
