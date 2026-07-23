import { z } from "zod";
import { ErrorCode, McpError } from "../errors.js";
import { questPsoHost, type QuestPsoSetup } from "../quest-pso.js";
import { categoryTool, type ToolDef } from "../types.js";

function requiredString(params: Record<string, unknown>, name: string): string {
  const value = typeof params[name] === "string" ? params[name].trim() : "";
  if (!value) throw new McpError(ErrorCode.INVALID_PARAMS, `${name} is required.`);
  return value;
}

function setup(params: Record<string, unknown>): QuestPsoSetup {
  return {
    projectPath: requiredString(params, "projectPath"),
    enginePath: requiredString(params, "enginePath"),
    packageName: requiredString(params, "packageName"),
    adbPath: typeof params.adbPath === "string" ? params.adbPath : undefined,
  };
}

export const questPsoTool: ToolDef = categoryTool(
  "quest_pso",
  "Host-side Quest/Android PSO recording orchestration. Uses exact-one-device adb preflight, the development-only RQDevServer TCP console bridge, explicit project/engine paths, and fail-closed recording/output validation. No Unreal editor bridge connection is required.",
  {
    preflight: {
      description: "Verify adb, exactly one ready device, installed package, explicit .uproject/engine paths, UnrealEditor-Cmd, and cooked Android_ASTC .shk files. Warns if Build contains bundled .spc files that can suppress full recording. Params: projectPath, enginePath, packageName, adbPath?",
      handler: async (_ctx, params) => questPsoHost.preflight(setup(params)),
    },
    clear_launch: {
      description: "Force-stop the package, clear CollectedPSOs plus writable Unreal/Vulkan caches, clear logcat, set an authoritative -logPSO command line, and launch. Preflight requires exactly one device and refuses local bundled .spc risk unless explicitly acknowledged. Params: projectPath, enginePath, packageName, adbPath?, devicePort? (default 6788), launchArgs?, allowBundledCacheRisk? (default false)",
      handler: async (_ctx, params) => questPsoHost.clearAndLaunch({
        ...setup(params),
        devicePort: params.devicePort as number | undefined,
        launchArgs: params.launchArgs as string[] | undefined,
        allowBundledCacheRisk: params.allowBundledCacheRisk as boolean | undefined,
      }),
    },
    command: {
      description: "Forward an ephemeral localhost port to the device RQDevServer, send one newline-terminated console command, validate its {ok,output} JSON line, flag output beginning with error:, then remove the forward. Requires exactly one adb device. Params: command, adbPath?, devicePort? (default 6788), timeoutMs?",
      handler: async (_ctx, params) => questPsoHost.sendCommand({
        command: requiredString(params, "command"),
        adbPath: typeof params.adbPath === "string" ? params.adbPath : undefined,
        devicePort: params.devicePort as number | undefined,
        timeoutMs: params.timeoutMs as number | undefined,
      }),
    },
    save: {
      description: "Explicitly send r.ShaderPipelineCache.Save through RQDevServer over an ephemeral adb forward. The response proves game-thread dispatch, not file creation; collect_expand verifies that recordings and the expanded output actually exist. Params: adbPath?, devicePort? (default 6788), timeoutMs?",
      handler: async (_ctx, params) => questPsoHost.saveRecordedPso({
        adbPath: typeof params.adbPath === "string" ? params.adbPath : undefined,
        devicePort: params.devicePort as number | undefined,
        timeoutMs: params.timeoutMs as number | undefined,
      }),
    },
    validate: {
      description: "Read UE logcat and validate the latest RQPSO run: TourStart levels>=1, TourDone ok=1 with matching levels and psosLogged>=1, exactly one unique LevelDone index per expected level, and no TourError markers. Params: adbPath?",
      handler: async (_ctx, params) => questPsoHost.validateHarvest({
        adbPath: typeof params.adbPath === "string" ? params.adbPath : undefined,
      }),
    },
    collect_expand: {
      description: "Explicitly save through RQDevServer, stop the app, pull every .upipelinecache recording into a unique run directory, expand with cooked .shk files via the explicit engine/project paths, validate non-empty output, and transactionally copy the .spc to Android and Android_ASTC Build caches. Existing Build caches are refused unless replaceExistingBuildCaches=true, in which case they are backed up under the run directory first. Tour validation defaults to auto: any RQPSO marker makes a complete valid tour mandatory, while marker-free latest-main RQAction/RQQuery captures use artifact validation. Set requireCompleteTour=true to require markers or false to disable marker checks. Params: projectPath, enginePath, packageName, adbPath?, devicePort?, commandTimeoutMs?, requireCompleteTour? (default auto), saveBeforePull? (default true), copyToBuild? (default true), replaceExistingBuildCaches? (default false), outputRoot?",
      handler: async (_ctx, params) => questPsoHost.collectExpand({
        ...setup(params),
        devicePort: params.devicePort as number | undefined,
        commandTimeoutMs: params.commandTimeoutMs as number | undefined,
        requireCompleteTour: params.requireCompleteTour as boolean | undefined,
        saveBeforePull: params.saveBeforePull as boolean | undefined,
        copyToBuild: params.copyToBuild as boolean | undefined,
        replaceExistingBuildCaches: params.replaceExistingBuildCaches as boolean | undefined,
        outputRoot: typeof params.outputRoot === "string" ? params.outputRoot : undefined,
      }),
    },
  },
  undefined,
  {
    projectPath: z.string().optional().describe("Explicit absolute path to the target .uproject; required by preflight, clear_launch, and collect_expand."),
    enginePath: z.string().optional().describe("Explicit absolute Unreal Engine root containing Engine/Binaries; required by preflight, clear_launch, and collect_expand."),
    packageName: z.string().optional().describe("Installed Android application id; required by preflight, clear_launch, and collect_expand."),
    adbPath: z.string().optional().describe("adb executable path or command name (default adb)."),
    devicePort: z.number().int().min(1).max(65535).optional().describe("RQDevServer TCP port on the device (default 6788)."),
    timeoutMs: z.number().int().min(100).max(120000).optional().describe("command/save RQDevServer response timeout in milliseconds (default 20000)."),
    commandTimeoutMs: z.number().int().min(100).max(120000).optional().describe("collect_expand RQDevServer save timeout in milliseconds."),
    command: z.string().optional().describe("One newline-free RQDevServer console command; required by command."),
    launchArgs: z.array(z.string()).optional().describe("Extra launch arguments appended after mandatory -logPSO and -rqdevport; -NoLogPSO is rejected."),
    allowBundledCacheRisk: z.boolean().optional().describe("clear_launch: acknowledge local Build .spc files and allow an intentional miss-only capture (default false)."),
    requireCompleteTour: z.boolean().optional().describe("collect_expand tour-marker policy: omitted=auto (enforce if any RQPSO marker exists), true=require a complete marker tour, false=disable marker checks. Recording/output validation always runs."),
    saveBeforePull: z.boolean().optional().describe("collect_expand: send r.ShaderPipelineCache.Save before stopping/pulling (default true)."),
    copyToBuild: z.boolean().optional().describe("collect_expand: copy validated .spc to Build/Android*/PipelineCaches (default true)."),
    replaceExistingBuildCaches: z.boolean().optional().describe("collect_expand: back up and transactionally replace existing Build .spc files (default false; otherwise existing files are refused)."),
    outputRoot: z.string().optional().describe("Absolute directory for unique harvest run output; created if absent (default <project>/PSOCache)."),
  },
);
