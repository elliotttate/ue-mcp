import { z } from "zod";
import { categoryTool, bp, directive, type ToolDef, type ToolContext } from "../types.js";
import { startEditor, stopEditor, restartEditor, buildProject } from "../editor-control.js";
import { startPackageProject, getPackageStatus, cancelPackageJob } from "../packaging.js";
import { pushWorkaround, workaroundCount } from "../workaround-tracker.js";
import { searchTools } from "../tool-search.js";
import { Vec3, Rotator } from "../schemas.js";
import { McpError, ErrorCode } from "../errors.js";

export const editorTool: ToolDef = categoryTool(
  "editor",
  "Editor commands, Python execution, PIE, undo/redo, hot reload, viewport, performance, sequencer, build pipeline, logs, editor control.",
  {
    start_editor: {
      description: "Launch Unreal Editor with the current project and reconnect bridge",
      handler: async (ctx: ToolContext) => {
        const result = await startEditor(ctx.project);
        if (result.success) {
          try { await ctx.bridge.connect(5000); } catch { /* reconnect timer handles it */ }
        }
        return result;
      },
    },
    stop_editor: {
      description: "Close Unreal Editor gracefully",
      handler: async () => {
        return stopEditor();
      },
    },
    restart_editor: {
      description: "Stop then start the editor",
      handler: async (ctx: ToolContext) => {
        return restartEditor(ctx.project, ctx.bridge);
      },
    },
    build_project: {
      description: "Build the project's C++ code using Unreal Build Tool. Editor should be stopped first.",
      handler: async (ctx: ToolContext) => {
        ctx.project.ensureLoaded();
        const lines: string[] = [];
        const result = await buildProject(ctx.project.projectPath!, {
          enginePath: ctx.project.config.enginePath,
          onOutput: (text) => lines.push(text),
        });
        return { ...result, output: lines.join("") };
      },
    },
    execute_command: bp("Run a console command in the editor or PIE world. Params: command, world? (auto|editor|pie), captureLog? (return fresh log lines observed while the synchronous command executes), logFilter?, maxLines? (default 100; retains the last matches). Fresh log capture avoids stale search_log matches for machine-readable diagnostics.", "execute_command", (p) => ({ command: p.command, world: p.world, captureLog: p.captureLog, logFilter: p.logFilter, maxLines: p.maxLines })),
    execute_python: {
      description: "GATED LAST RESORT. execute_python is unreachable until a semantic tool search over your taskSummary has been run AND every candidate it returns is EXPLICITLY ruled out with a stated reason. Flow: (1) call with taskSummary (+code) - it returns the candidate actions; (2) re-call with the same taskSummary/code PLUS ruledOut=[{action, reason}] giving a specific reason each candidate does not fit. Python runs only once every candidate is ruled out. Params: code, taskSummary (required), ruledOut? (#704)",
      handler: async (ctx: ToolContext, params: Record<string, unknown>) => {
        const code = (params.code as string) ?? "";
        const taskSummary = ((params.taskSummary as string) ?? "").trim();

        // #704: hard gate. Require an intent statement, run the semantic search,
        // and refuse to run Python until EVERY candidate action is explicitly
        // ruled out with a stated reason.
        if (!taskSummary) {
          return {
            blocked: true,
            reason: "missing_task_summary",
            message: "execute_python requires a 'taskSummary' (plain-words intent). It is searched against the tool registry and gated behind ruling out every candidate. Re-call with taskSummary.",
          };
        }

        // Candidates = meaningful matches (a name/phrase hit), capped at 5.
        const candidates = (await searchTools(taskSummary, 5)).filter((h) => h.score >= 4);
        if (candidates.length > 0) {
          const ruledRaw = Array.isArray(params.ruledOut) ? (params.ruledOut as Array<Record<string, unknown>>) : [];
          const ruled = new Map<string, string>();
          for (const r of ruledRaw) {
            const action = String(r?.action ?? "").trim();
            const reason = String(r?.reason ?? "").trim();
            if (action && reason.length >= 12) ruled.set(action, reason); // non-trivial reason required
          }
          const unresolved = candidates.filter((c) => !ruled.has(c.action));
          if (unresolved.length > 0) {
            pushWorkaround({ code, timestamp: new Date().toISOString(), taskSummary, suggestedTool: candidates.map((c) => `${c.tool}(${c.action})`).join(", ") });
            return {
              blocked: true,
              reason: "candidates_not_ruled_out",
              taskSummary,
              candidates,
              needReasonFor: unresolved.map((c) => `${c.tool}(${c.action})`),
              message: `execute_python is GATED. A tool search for "${taskSummary}" returned ${candidates.length} candidate action(s). Rule out EACH with a specific reason (>=12 chars) via ruledOut:[{action, reason}], then re-call. Still need a reason for: ${unresolved.map((c) => c.action).join(", ")}. If one of these actually does the task, call it instead of Python.`,
            };
          }
        }

        // Gate passed (no candidates, or every candidate ruled out) - run Python.
        const result = await ctx.bridge.call("execute_python", { code });

        // Track this workaround in memory, and side-channel to a tmp log so
        // the record survives even if the agent ignores the directive.
        const snippet = typeof result === "object" && result !== null
          ? JSON.stringify(result).slice(0, 200)
          : String(result).slice(0, 200);
        const entry = { code, timestamp: new Date().toISOString(), resultSnippet: snippet, taskSummary };
        pushWorkaround(entry);
        try {
          const os = await import("node:os");
          const fs = await import("node:fs");
          const path = await import("node:path");
          fs.appendFileSync(
            path.join(os.tmpdir(), "ue-mcp-workarounds.log"),
            JSON.stringify(entry) + "\n",
          );
        } catch {
          // side-channel is best-effort; primary tracking is the in-memory stack
        }

        const n = workaroundCount();
        return directive(
          [
            `[AGENT DIRECTIVE — MANDATORY]`,
            `execute_python was used as a workaround (${n} time(s) this session).`,
            `This means a native ue-mcp tool could not handle the task.`,
            ``,
            `YOUR NEXT MESSAGE TO THE USER must include:`,
            `"I had to use execute_python to <describe what you did and why>.`,
            ` Would you like to submit feedback so this can become a native tool?"`,
            ``,
            `If the user agrees, call feedback(action="submit") with:`,
            `  title  — short description of the gap`,
            `  summary — what was attempted and why the native tool fell short`,
            `  pythonWorkaround — the Python code above`,
            `  idealTool — what tool/action should handle this natively`,
            ``,
            `Do NOT skip this step. Do NOT defer it to "later."`,
          ].join("\n"),
          result,
          {
            kind: "workaround.feedback",
            requiredActions: [
              "surface_workaround_to_user",
              "ask_if_user_wants_to_submit_feedback",
              "on_agreement_call_feedback_submit",
            ],
            context: {
              workaroundCount: n,
              feedbackTool: "feedback",
              feedbackAction: "submit",
              expectedFields: ["title", "summary", "pythonWorkaround", "idealTool"],
            },
          },
        );
      },
    },
    run_python_file: bp("Run a Python file from disk with __file__/__name__ populated (#142). Params: filePath, args?", "run_python_file", (p) => ({ filePath: p.filePath, args: p.args })),
    set_property: bp("Set UObject property. Saves the package to disk by default; pass save=false to leave it dirty in-memory (batch many writes, then editor(save_dirty)/asset(save)) (#674). Params: objectPath, propertyName, value, save? (default true)", "set_property"),
    get_property: bp("Read UObject property. Params: objectPath, propertyName", "get_property"),
    describe_object: bp("Describe a UObject and optionally list/read properties. Params: objectPath, includeProperties?, includeValues?, propertyNames?", "describe_object"),
    play_in_editor: bp("PIE control. Params: pieAction (start|stop|status), preview? (start only: 'vr' launches in the connected HMD - the editor's VR Preview mode - and errors with a diagnostic if no XR runtime/HMD unless force=true; 'mobile' for mobile preview), force?, waitForAssetRegistry? (start only; default true - block until the AssetRegistry initial scan completes before requesting PIE, otherwise PIE silently no-ops on cold editor starts), assetRegistryTimeoutSeconds? (default 180) (#406)", "pie_control", (p) => ({ action: p.pieAction ?? "status", preview: p.preview, force: p.force, waitForAssetRegistry: p.waitForAssetRegistry, assetRegistryTimeoutSeconds: p.assetRegistryTimeoutSeconds })),
    get_xr_status: bp("XR/VR snapshot: runtime present + name, HMD connected/enabled, stereo, head tracking, tracking origin, world-to-meters, tracked device count, spectator screen availability, XR plugin enabled states, and key VR CVars (vr.PixelDensity, r.InstancedStereo, ...). Check before play_in_editor preview='vr'", "get_xr_status"),
    get_xr_poses: bp("Live HMD pose + every motion controller in the active (PIE-preferred) world: motion source, tracked state, world position/rotation, owning actor", "get_xr_poses"),
    xr_enable_hmd: bp("Toggle the HMD + stereo rendering (e.g. drop out of / back into VR mid-PIE). Params: enabled? (default true)", "xr_enable_hmd", (p) => ({ enabled: p.enabled })),
    xr_set_tracking_origin: bp("Set the XR tracking origin. Params: origin (Local/seated | LocalFloor/standing | Stage/room-scale | View)", "xr_set_tracking_origin", (p) => ({ origin: p.origin })),
    xr_set_world_to_meters: bp("Set world-to-meters scale on the active world's WorldSettings (UE default 100; smaller = player feels bigger). Params: worldToMeters", "xr_set_world_to_meters", (p) => ({ worldToMeters: p.worldToMeters })),
    xr_set_spectator_mode: bp("Set the HMD spectator (mirror) screen mode. Params: spectatorMode (Disabled | SingleEyeLetterboxed | Undistorted | Distorted | SingleEye | SingleEyeCroppedToFill | Texture | TexturePlusEye)", "xr_set_spectator_mode", (p) => ({ mode: p.spectatorMode })),
    get_runtime_value: bp("Read PIE actor property. Params: actorLabel, propertyName (supports dotted paths: component.field or component.struct.field for nested reads on component subobjects, #344/#381)", "get_runtime_value"),
    get_pie_pawn: bp("Resolve the controlled pawn in the active PIE world. Params: playerIndex? (default 0). Returns actorLabel/class/location/rotation (#228/#229)", "get_pie_pawn", (p) => ({ playerIndex: p.playerIndex })),
    invoke_function: bp("Call a BlueprintCallable / Exec UFUNCTION on a target actor or one of its components. Params: actorLabel, functionName, component? (component subobject name; redirects target from the actor to that component, #382), args? (object), actorArgs? (object mapping UObject* parameter name to actor label, resolved against live actors in the active world; #383), world? (editor|pie). Returns out/return params (#228/#229)", "invoke_function", (p) => ({ actorLabel: p.actorLabel, functionName: p.functionName, component: p.component, args: p.args, actorArgs: p.actorArgs, world: p.world })),
    invoke_static_function: bp("Call a static UFUNCTION on a UBlueprintFunctionLibrary (no actor instance). invoke_function needs an actor/component target; this targets the library class CDO instead, so it reaches static *_BlueprintOnly libraries (Voxel sculpt/query/stamp), GeometryScript, Kismet math, any function library. Params: className (short name or /Script/Module.Class path), functionName, args? (name -> JSON value, same marshalling as invoke_function), actorArgs? (name -> actor label for UObject* params that are actors, e.g. the sculpt actor), worldContextParam? (name of a UObject* param to fill with the editor/PIE world; auto-detected for params named WorldContextObject), world? (editor|pie). Returns return/out params under returnValues. Discover libraries + functions with list_function_libraries.", "invoke_static_function", (p) => ({ className: p.className, functionName: p.functionName, args: p.args, actorArgs: p.actorArgs, worldContextParam: p.worldContextParam, world: p.world })),
    list_function_libraries: bp("Enumerate UBlueprintFunctionLibrary subclasses on this build. Filter by name (case-insensitive substring, e.g. 'GeometryScript' / 'Kismet' / 'Animation'). Returns name, module, and (by default) every static BlueprintCallable function on the library with its tooltip. Use to discover what's available for editor.invoke_function (#455). Params: pattern?, includeFunctions?", "list_function_libraries", (p) => ({ pattern: p.pattern, includeFunctions: p.includeFunctions })),
    set_pie_time_scale: bp("Fast-forward PIE game time. Params: factor (>0). Raises WorldSettings caps and calls SetGlobalTimeDilation.", "set_pie_time_scale"),
    simulate_input: bp("Inject a key/axis event into the running PIE session through the game viewport, flowing through the full input stack (PlayerInput, Enhanced Input, UMG focus) like hardware input. Params: key (FKey name: W, SpaceBar, LeftMouseButton, Gamepad_LeftX, ...), inputEvent? (tap|press|release, default tap), holdSeconds? (tap only: schedule the release N seconds later without blocking), amount? (analog depression 0-1, default 1), axisValue? (inject an IE_Axis sample with this value instead of a key event)", "simulate_pie_input", (p) => ({ key: p.key, event: p.inputEvent, holdSeconds: p.holdSeconds, amount: p.amount, axisValue: p.axisValue })),
    check_pie_condition: bp("Evaluate one runtime condition against the PIE world once (no waiting). Conditions: pie_running | pie_stopped | actor_exists | actor_gone | actor_count (className?, expected, op?) | property (actorLabel, propertyName incl. dotted paths, expected, op? ==|!=|>|<|>=|<=|contains). Returns {met, value?, worldTimeSeconds}", "check_pie_condition", (p) => ({ condition: p.condition, actorLabel: p.actorLabel, actorPath: p.actorPath, className: p.className, propertyName: p.propertyName, expected: p.expected, op: p.op })),
    pie_line_trace: bp("Line trace in the live PIE world. Params: start? [x,y,z], end? [x,y,z] - omit both to trace from the player camera forward (distance?, default 10000). channel? (Visibility|Camera|Pawn|WorldStatic|WorldDynamic|PhysicsBody). Returns hit actor/component/location/normal/distance", "pie_line_trace", (p) => ({ start: p.start, end: p.end, distance: p.distance, channel: p.traceChannel })),
    wait_for_pie_event: {
      description: "Poll a PIE condition until it holds or the timeout expires. Same condition params as check_pie_condition, plus timeoutMs? (default 30000) and intervalMs? (default 250). Waiting happens server-side; the editor never blocks. Returns {met, waitedMs, polls, value?}",
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return waitForPieCondition(ctx, p);
      },
    },
    run_pie_test_sequence: {
      description:
        "Run a scripted PIE test: steps execute in order, waiting server-side between editor calls. Params: steps (array), stopOnFailure? (default true). Step forms: " +
        "{do:'start_pie'} | {do:'stop_pie'} | {do:'input', key, inputEvent?, holdSeconds?, axisValue?} | {do:'wait', ms} | " +
        "{do:'wait_for', condition..., timeoutMs?, intervalMs?} | {do:'assert', condition...} | {do:'console', command} | " +
        "{do:'screenshot', filename?} | {do:'trace', start?, end?, channel?}. Returns {passed, steps[]} with per-step results",
      timeoutMs: 600_000,
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return runPieTestSequence(ctx, p);
      },
    },
    run_automation_tests: {
      description: "Run UE Automation Framework tests (functional/smoke/project suites) matching a name filter and wait for structured results: per-test state, duration, errors/warnings with file:line events, plus a pass/fail summary. Params: filter (substring) OR filters[] OR runAll=true, timeoutSeconds? (default 600). Long suites: pass a larger timeoutSeconds and poll get_automation_status instead of waiting",
      timeoutMs: 660_000,
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return runAutomationTests(ctx, p);
      },
    },
    list_automation_tests: {
      description: "Enumerate available UE Automation Framework test names, optionally filtered by substring. Params: filter?",
      timeoutMs: 120_000,
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return runAutomationTests(ctx, { ...p, listOnly: true, timeoutSeconds: 90 });
      },
    },
    get_automation_status: bp("Poll the in-flight automation run: phase (finding_workers|running|complete|failed), elapsed, counts, and results when finished", "get_automation_test_status"),
    hot_reload: bp("Hot reload C++", "hot_reload"),
    undo: bp("Undo last transaction", "undo"),
    redo: bp("Redo last transaction", "redo"),
    get_perf_stats: bp("Editor performance stats", "get_editor_performance_stats"),
    run_stat: bp("Run stat command. Params: command", "run_stat_command"),
    start_trace: bp("Start writing an Unreal Insights .utrace to disk. Params: channels? (default 'default,frame,bookmark'), outputPath? (default Saved/Profiling/MCP_<timestamp>.utrace). Returns tracePath", "start_trace", (p) => ({ channels: p.channels, outputPath: p.outputPath })),
    stop_trace: bp("Stop the active Insights trace. Returns the finished tracePath, ready for analyze_trace", "stop_trace"),
    get_trace_status: bp("Whether an Insights trace is running and where it writes", "get_trace_status"),
    analyze_trace: { ...bp("Analyze a .utrace via TraceServices: duration, game/render frame stats (avg/min/max/p50/p90/p99 ms, avg fps, hitches over 33ms), bookmarks. Params: tracePath", "analyze_trace", (p) => ({ tracePath: p.tracePath })), timeoutMs: 180_000 },
    profile_pie: {
      description: "One-call profiler: start an Insights trace, ensure PIE is running, capture for durationSeconds (default 10), stop the trace, and return the analyzed frame stats. Params: durationSeconds?, channels?, stopPieAfter? (default false)",
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return profilePie(ctx, p);
      },
    },
    set_scalability: bp("Set rendering quality via the Scalability system (actually applies + persists, not just sg.* cvars). Params: level (Low|Medium|High|Epic|Cinematic). Returns appliedLevels (#591)", "set_scalability"),
    set_cvars: bp("Bulk-set console variables. Params: cvars ({name: value} object OR [{name, value}] array). Returns per-cvar old/new values and any notFound names (#591)", "set_cvars", (p) => ({ cvars: p.cvars })),
    get_cvar: bp("Read console variable(s): value, type, setBy origin, readOnly, help. Use after set_cvars to confirm a write took effect. Params: name OR names[]", "get_cvar", (p) => ({ name: p.name, names: p.names })),
    discover_cvars: bp("Enumerate console variables by name prefix (e.g. r.Lumen) or substring. Returns name/value/type/setBy/help per match, name-sorted. Params: prefix? | contains?, includeCommands? (default false), includeHelp? (default true), limit? (default 200)", "discover_cvars", (p) => ({ prefix: p.prefix, contains: p.contains, includeCommands: p.includeCommands, includeHelp: p.includeHelp, limit: p.limit })),
    capture_screenshot: bp("Screenshot. Params: filename?, resolution?, target? (auto|pie|editor; auto routes to PIE viewport when PIE is running) (#226)", "capture_screenshot"),
    capture_scene_png: bp("Headless PNG screenshot via SceneCapture2D (works unfocused, guaranteed RGBA8 LDR). focusActorLabel auto-frames the camera on an actor's bounds; world:pie captures the running game world (#599). Params: outputPath, location?, rotation?, focusActorLabel?, focusDirection?, focusMargin?, world? (editor|pie), width? (default 1280), height? (default 720), fov? (default 90) (#148/#599)", "capture_scene_png", (p) => ({ outputPath: p.outputPath, location: p.location, rotation: p.rotation, focusActorLabel: p.focusActorLabel, focusDirection: p.focusDirection, focusMargin: p.focusMargin, world: p.world, width: p.width, height: p.height, fov: p.fov, fullyLoadTextures: p.fullyLoadTextures })),
    set_realtime: bp("Toggle realtime update on the level editor viewports so the editor-world sim (Niagara, anims) ticks - otherwise capture_scene_png renders an unticked, empty sim. Params: enabled (default true) (#537)", "set_realtime", (p) => ({ enabled: p.enabled })),
    get_viewport: bp("Get viewport camera", "get_viewport_info"),
    hit_test_viewport_pixel: bp("Ray-cast from a screen pixel through the active editor viewport and return the first hit. Builds the ray from the live viewport's projection matrix (no FOV/aspect guessing). Returns hit + actorLabel/actorClass/componentName/componentClass/materialPath/location/impactPoint/normal/distance/faceIndex/boneName/physicalMaterial. Params: x, y (pixel coords), width? height? (override viewport size when picking from a different-resolution screenshot), maxDistance? (default 200000), ignoreActors? (array of actor labels) (#418)", "hit_test_viewport_pixel", (p) => ({ x: p.x, y: p.y, width: p.width, height: p.height, maxDistance: p.maxDistance, ignoreActors: p.ignoreActors })),
    get_runtime_values: bp("Bulk runtime read across the active world. For each actor/component matching classFilter, resolves every path against the (actor|component) root and returns rows of {actorLabel, actorClass, componentName?, componentClass?, values, errors?}. Paths support property hops, sub-object hops, and zero-arg BlueprintCallable getter calls at any segment (e.g. 'PowerConnector.GetRequired' reaches a UFUNCTION on a UObject sub-object). classFilter matches actor class OR component class - omit to match everything. World defaults to PIE if running, else editor. Params: classFilter?, paths[], world? (editor|pie) (#414)", "get_runtime_values", (p) => ({ classFilter: p.classFilter, paths: p.paths, world: p.world })),
    set_viewport: bp("Set viewport camera. Params: location?, rotation?", "set_viewport_camera"),
    focus_on_actor: bp("Focus on actor. Params: actorLabel", "focus_viewport_on_actor"),
    create_sequence: bp("Create Level Sequence. Params: name, packagePath?", "create_level_sequence"),
    get_sequence_info: bp("Read sequence: bindings (possessable/spawnable) with their Sequencer tags (#556), tracks, and optional section detail. Params: assetPath, includeSectionDetails? (attach sockets, first transform key values per track)", "get_sequence_info"),
    add_sequence_track: bp("Add an empty track. Params: assetPath, trackType, actorLabel?", "add_sequence_track"),
    add_sequence_section: bp("Add a section to a track (creating the track if needed), set its start/end in seconds, and for a CameraCut track bind it to a camera. Returns the section index + channel names to key. Params: sequencePath, trackType (Transform|Float|Fade|CameraCut|Audio|Event|SkeletalAnimation|Visibility|Bool|Integer|Color|Sub), actorLabel? (binding scope), startSeconds?, endSeconds?, cameraActorLabel?, propertyName? (property tracks: which property to animate; Visibility defaults to bHidden), subSequencePath? (Sub track: child sequence) (#548)", "add_sequence_section", (p) => ({ sequencePath: p.sequencePath ?? p.assetPath, trackType: p.trackType, actorLabel: p.actorLabel, startSeconds: p.startSeconds, endSeconds: p.endSeconds, cameraActorLabel: p.cameraActorLabel, propertyName: p.propertyName, subSequencePath: p.subSequencePath })),
    set_sequence_keyframes: bp("Add keyframes to a section channel. Transform channels: Location.X/Y/Z, Rotation.X/Y/Z (or friendly x/y/z, yaw/pitch/roll); Fade/Float: the float channel; Color: R/G/B/A; Visibility/Bool (value true/false) and Integer use their single channel. Params: sequencePath, trackType, actorLabel?, sectionIndex? (default 0), channel, keyframes ([{seconds, value}]), interpolation? (cubic|linear) (#548)", "set_sequence_keyframes", (p) => ({ sequencePath: p.sequencePath ?? p.assetPath, trackType: p.trackType, actorLabel: p.actorLabel, sectionIndex: p.sectionIndex, channel: p.channel, keyframes: p.keyframes, interpolation: p.interpolation })),
    mrq_create_job: bp("Movie Render Queue: allocate a render job for a Level Sequence with output settings, image format, and the deferred pass in one call. Params: sequencePath, jobName?, mapPath? (default: current level), outputDirectory?, resolutionX?/resolutionY?, fileNameFormat?, imageFormat? (png|jpg|bmp|exr), addDeferredPass? (default true), clearQueue? (default false). Requires the Movie Render Queue plugin", "mrq_create_job", (p) => ({ sequencePath: p.sequencePath ?? p.assetPath, jobName: p.jobName, mapPath: p.mapPath, outputDirectory: p.outputDirectory, resolutionX: p.resolutionX, resolutionY: p.resolutionY, fileNameFormat: p.fileNameFormat, imageFormat: p.imageFormat, addDeferredPass: p.addDeferredPass, clearQueue: p.clearQueue })),
    mrq_render: bp("Movie Render Queue: start rendering the queue in-process via the PIE executor. Fire-and-forget - poll mrq_status until rendering=false, then collect frames from the job's output directory", "mrq_render"),
    mrq_status: bp("Movie Render Queue: rendering flag + per-job name/progress/status message", "mrq_status"),
    mrq_clear: bp("Movie Render Queue: delete every job in the queue", "mrq_clear"),
    set_sequence_playback_range: bp("Set a Level Sequence's playback range in seconds. Params: sequencePath, startSeconds, endSeconds (#548)", "set_sequence_playback_range", (p) => ({ sequencePath: p.sequencePath ?? p.assetPath, startSeconds: p.startSeconds, endSeconds: p.endSeconds })),
    play_sequence: bp("Play/stop/pause sequence. Params: assetPath, sequenceAction", "play_sequence", (p) => ({ assetPath: p.assetPath, action: p.sequenceAction ?? "play" })),
    build_all: bp("Build all (geometry, lighting, paths, HLOD)", "build_all"),
    build_geometry: bp("Rebuild BSP geometry", "build_geometry"),
    build_hlod: bp("Build HLODs", "build_hlod"),
    validate_assets: bp("Run data validation. Params: directory?", "validate_assets"),
    get_build_status: bp("Get build/map status", "get_build_status"),
    cook_content: bp("Cook content. Params: platform?", "cook_content"),
    package_project: {
      description: "Package the project via UAT BuildCookRun (build + cook + stage + pak + archive) as a background job - typically 10-60 minutes. Returns immediately; poll get_package_status. Params: platform? (default Win64), configuration? (default Development), archiveDirectory? (default Saved/MCP/Packages/<platform>), maps? (limit cooked maps), pak? (default true), clean?",
      handler: async (ctx: ToolContext, p: Record<string, unknown>) => {
        return startPackageProject(ctx.project, p);
      },
    },
    get_package_status: {
      description: "Poll the background packaging job: stage (build/cook/stage/pak/archive/done/failed), elapsed, exit code, and the UAT log tail. Params: tailLines? (default 25)",
      handler: async (_ctx: ToolContext, p: Record<string, unknown>) => {
        return getPackageStatus(p);
      },
    },
    cancel_package: {
      description: "Cancel the running packaging job (kills the UAT process tree)",
      handler: async () => {
        return cancelPackageJob();
      },
    },
    get_log: bp("Read output log. Params: maxLines?, filter?, category?", "get_output_log"),
    search_log: bp("Search log. Params: query", "search_log"),
    get_message_log: bp("Read message log. Params: logName?", "get_message_log"),
    list_crashes: bp("List crash reports", "list_crashes"),
    get_crash_info: bp("Get crash details. Params: crashFolder", "get_crash_info"),
    check_for_crashes: bp("Check for recent crashes", "check_for_crashes"),
    set_dialog_policy: bp("Auto-respond to dialogs matching a pattern. Params: pattern, response", "set_dialog_policy"),
    clear_dialog_policy: bp("Clear dialog policies. Params: pattern?", "clear_dialog_policy"),
    get_dialog_policy: bp("Get current dialog policies", "get_dialog_policy"),
    list_dialogs: bp("List active modal dialogs", "list_dialogs"),
    respond_to_dialog: bp("Click a button on the active modal dialog. Params: buttonIndex?, buttonLabel?", "respond_to_dialog"),
    open_asset: bp("Open asset in its editor. Params: assetPath", "open_asset"),
    reload_bridge: bp("Hot-reload Python bridge handlers from disk", "reload_handlers"),
    save_dirty: bp("Flush every dirty package and return a per-package saved/failed map. Use after multi-step CDO/component edits when set_class_default leaves the asset dirty without persisting (#378). Params: includeMaps? (default true), includeContent? (default true)", "save_dirty", (p) => ({ includeMaps: p.includeMaps, includeContent: p.includeContent })),
    configure_pie: bp("Set ULevelEditorPlaySettings - multi-client PIE, net mode, single-process flag, Play-in-New-Window resolution. Params: numClients?, netMode? (standalone|listen|client), runUnderOneProcess?, launchSeparateServer?, newWindowWidth?, newWindowHeight? (#384/#671)", "configure_pie", (p) => ({ numClients: p.numClients, netMode: p.netMode, runUnderOneProcess: p.runUnderOneProcess, launchSeparateServer: p.launchSeparateServer, newWindowWidth: p.newWindowWidth, newWindowHeight: p.newWindowHeight })),
    get_pie_config: bp("Read current ULevelEditorPlaySettings (numClients, netMode, single-process, separate-server) (#384)", "get_pie_config"),
    pie_set_player_view: bp("Point the running PIE player's view (control rotation) at a pitch/yaw/roll so a capture frames the intended direction. Requires PIE. Params: pitch?, yaw?, roll? (#671)", "pie_set_player_view", (p) => ({ pitch: p.pitch, yaw: p.yaw, roll: p.roll })),
    stage_game_input: bp("Stage input for the running game: set input mode (gameOnly|gameAndUI|uiOnly) and mouse cursor so injected/simulated input reaches the pawn. Requires PIE. Params: inputMode? (default gameOnly), showMouseCursor? (#671)", "stage_game_input", (p) => ({ inputMode: p.inputMode, showMouseCursor: p.showMouseCursor })),
    invoke_function_repeating: bp("Fire a parameterless UFUNCTION on an actor repeatedly at an interval for sustained on-demand triggering (human visual verification). Returns immediately; the remaining calls run in the background. Params: actorLabel, functionName, count? (default 5), intervalSeconds? (default 1.0), world? (editor|pie|auto) (#583)", "invoke_function_repeating", (p) => ({ actorLabel: p.actorLabel, functionName: p.functionName, count: p.count, intervalSeconds: p.intervalSeconds, world: p.world })),
    list_dirty_packages: bp("Enumerate currently-dirty content + map packages (#340)", "list_dirty_packages"),
  },
  undefined,
  {
    command: z.string().optional(),
    captureLog: z.boolean().optional().describe("execute_command: capture fresh log lines observed while the synchronous command executes"),
    logFilter: z.string().optional().describe("execute_command: case-insensitive filter applied only to freshly captured lines"),
    code: z.string().optional(),
    taskSummary: z.string().optional().describe("execute_python: plain-words intent, searched against the tool registry to gate the call (#704)"),
    ruledOut: z.array(z.object({ action: z.string(), reason: z.string() })).optional().describe("execute_python: reason each searched candidate action does not fit; every candidate must be ruled out before Python runs (#704)"),
    filePath: z.string().optional().describe("Absolute path to a .py file for run_python_file"),
    args: z.union([
      z.array(z.string()),
      z.record(z.unknown()),
    ]).optional().describe("run_python_file: array of positional args. invoke_function: object mapping parameter name to value"),
    objectPath: z.string().optional(),
    target: z.string().optional().describe("capture_screenshot target: auto (default) | pie | editor"),
    playerIndex: z.number().optional().describe("get_pie_pawn: 0-based player index (default 0)"),
    functionName: z.string().optional(),
    count: z.number().optional().describe("invoke_function_repeating: total number of calls (default 5) (#583)"),
    intervalSeconds: z.number().optional().describe("invoke_function_repeating: seconds between calls (default 1.0) (#583)"),
    component: z.string().optional().describe("invoke_function: optional component subobject name to call the function on instead of the actor (#382)"),
    actorArgs: z.record(z.string()).optional().describe("invoke_function: map of UObject* parameter name to actor label, resolved against live actors in the active world (#383)"),
    className: z.string().optional().describe("invoke_static_function: UBlueprintFunctionLibrary class - short name or /Script/Module.Class path"),
    worldContextParam: z.string().optional().describe("invoke_static_function: name of a UObject* param to fill with the editor/PIE world (auto-detected for params named WorldContextObject)"),
    world: z.string().optional().describe("World scope for supported actions: auto | editor | pie"),
    propertyName: z.string().optional(),
    propertyNames: z.array(z.string()).optional().describe("describe_object: optional dotted/indexed property paths to describe"),
    includeProperties: z.boolean().optional().describe("describe_object: include reflected property metadata (default true)"),
    includeValues: z.boolean().optional().describe("describe_object: include current property values (default false)"),
    value: z.unknown().optional(),
    save: z.boolean().optional().describe("set_property: save package to disk after the write (default true; false leaves it dirty) (#674)"),
    pieAction: z.enum(["start", "stop", "status"]).optional(),
    preview: z.string().optional().describe("play_in_editor start: vr (VR Preview in the HMD) | mobile | default"),
    force: z.boolean().optional().describe("play_in_editor start preview=vr: launch even when no HMD is detected"),
    origin: z.string().optional().describe("xr_set_tracking_origin: Local (seated) | LocalFloor (standing) | Stage (room-scale) | View"),
    worldToMeters: z.number().optional().describe("xr_set_world_to_meters: scale (UE default 100)"),
    spectatorMode: z.string().optional().describe("xr_set_spectator_mode: Disabled | SingleEyeLetterboxed | Undistorted | Distorted | SingleEye | SingleEyeCroppedToFill | Texture | TexturePlusEye"),
    waitForAssetRegistry: z.boolean().optional().describe("play_in_editor start: block until AssetRegistry finishes the initial scan (default true)"),
    assetRegistryTimeoutSeconds: z.number().optional().describe("play_in_editor start: wait budget for the AssetRegistry scan (default 180s)"),
    actorLabel: z.string().optional(),
    level: z.string().optional(),
    filename: z.string().optional(),
    resolution: z.number().optional(),
    location: Vec3.optional(),
    rotation: Rotator.optional(),
    name: z.string().optional(),
    packagePath: z.string().optional(),
    assetPath: z.string().optional(),
    trackType: z.string().optional(),
    sequencePath: z.string().optional().describe("Level Sequence asset path for sequencer authoring (#548)"),
    startSeconds: z.number().optional().describe("Section/playback range start in seconds (#548)"),
    endSeconds: z.number().optional().describe("Section/playback range end in seconds (#548)"),
    cameraActorLabel: z.string().optional().describe("add_sequence_section CameraCut: camera actor to bind (#548)"),
    sectionIndex: z.number().optional().describe("set_sequence_keyframes: target section index (default 0) (#548)"),
    channel: z.string().optional().describe("set_sequence_keyframes: channel name (Location.X, Rotation.Z, yaw, fade...) (#548)"),
    keyframes: z.array(z.object({ seconds: z.number(), value: z.number() })).optional().describe("set_sequence_keyframes: [{seconds, value}] (#548)"),
    interpolation: z.string().optional().describe("set_sequence_keyframes: cubic (default) or linear (#548)"),
    sequenceAction: z.enum(["play", "stop", "pause"]).optional(),
    directory: z.string().optional(),
    platform: z.string().optional(),
    configuration: z.string().optional().describe("package_project: Debug | Development | Test | Shipping (default Development)"),
    archiveDirectory: z.string().optional().describe("package_project: where the packaged build is archived (default Saved/MCP/Packages/<platform>)"),
    maps: z.array(z.string()).optional().describe("package_project: restrict cooking to these maps"),
    pak: z.boolean().optional().describe("package_project: produce .pak files (default true)"),
    clean: z.boolean().optional().describe("package_project: clean before building"),
    tailLines: z.number().optional().describe("get_package_status: UAT log tail lines to return (default 25)"),
    maxLines: z.number().optional(),
    filter: z.string().optional(),
    filters: z.array(z.string()).optional().describe("run_automation_tests: multiple name substrings (a test matching any runs)"),
    runAll: z.boolean().optional().describe("run_automation_tests: run every discovered test instead of filtering (may take a long time)"),
    timeoutSeconds: z.number().optional().describe("run_automation_tests: run deadline enforced editor-side (default 600)"),
    maxTests: z.number().optional().describe("run_automation_tests: cap on tests to run (default 50) (#693)"),
    category: z.string().optional(),
    query: z.string().optional(),
    logName: z.string().optional(),
    crashFolder: z.string().optional(),
    pattern: z.string().optional().describe("Substring filter — dialog title/message, or library name for list_function_libraries (#455)"),
    includeFunctions: z.boolean().optional().describe("list_function_libraries: include each library's function listing (default true) (#455)"),
    response: z.enum(["yes", "no", "ok", "cancel", "retry", "continue", "yesall", "noall"]).optional().describe("Auto-response for matched dialogs"),
    buttonIndex: z.number().optional().describe("Index of button to click in active dialog"),
    buttonLabel: z.string().optional().describe("Label of button to click in active dialog"),
    factor: z.number().optional().describe("Time-scale factor for set_pie_time_scale (e.g. 500)"),
    includeSectionDetails: z.boolean().optional().describe("Include attach sockets + first-key transform values in get_sequence_info"),
    outputPath: z.string().optional().describe("Absolute or project-relative output path for capture_scene_png (e.g. \"Saved/Screenshots/cap.png\")"),
    enabled: z.boolean().optional().describe("set_realtime: enable/disable viewport realtime update (#537)"),
    width: z.number().optional().describe("Capture width in pixels"),
    height: z.number().optional().describe("Capture height in pixels"),
    fov: z.number().optional().describe("Capture FOV in degrees"),
    focusActorLabel: z.string().optional().describe("capture_scene_png: auto-frame the camera on this actor's bounds (#599)"),
    focusDirection: Vec3.optional().describe("capture_scene_png: framing direction from the actor (default front/above) (#599)"),
    focusMargin: z.number().optional().describe("capture_scene_png: bounds fill margin, higher pulls back (default 1.5) (#599)"),
    fullyLoadTextures: z.boolean().optional().describe("capture_scene_png: force-stream textures + flush render thread before capture to avoid the checker/stale frame (default true) (#662)"),
    x: z.number().optional().describe("hit_test_viewport_pixel: viewport pixel X"),
    y: z.number().optional().describe("hit_test_viewport_pixel: viewport pixel Y"),
    maxDistance: z.number().optional().describe("hit_test_viewport_pixel: max ray length in cm (default 200000)"),
    ignoreActors: z.array(z.string()).optional().describe("hit_test_viewport_pixel: actor labels to skip"),
    classFilter: z.string().optional().describe("get_runtime_values: actor or component class name (omit for all)"),
    paths: z.array(z.string()).optional().describe("get_runtime_values: dotted property/function paths to evaluate per match"),
    includeMaps: z.boolean().optional().describe("save_dirty: include map packages (default true)"),
    includeContent: z.boolean().optional().describe("save_dirty: include content packages (default true)"),
    cvars: z.union([z.record(z.unknown()), z.array(z.object({ name: z.string(), value: z.unknown() }))]).optional().describe("set_cvars: {name: value} object or [{name, value}] array of console variables (#591)"),
    names: z.array(z.string()).optional().describe("get_cvar: console variable names to read in one call"),
    prefix: z.string().optional().describe("discover_cvars: name prefix to enumerate (e.g. r.Lumen)"),
    contains: z.string().optional().describe("discover_cvars: name substring to enumerate (alternative to prefix)"),
    includeCommands: z.boolean().optional().describe("discover_cvars: also list console commands, not just variables (default false)"),
    includeHelp: z.boolean().optional().describe("discover_cvars: include help text per match (default true)"),
    limit: z.number().optional().describe("discover_cvars: max results returned (default 200)"),
    numClients: z.number().optional().describe("configure_pie: number of PIE clients"),
    netMode: z.string().optional().describe("configure_pie: standalone | listen | client"),
    runUnderOneProcess: z.boolean().optional().describe("configure_pie: single-process flag"),
    launchSeparateServer: z.boolean().optional().describe("configure_pie: separate dedicated server"),
    key: z.string().optional().describe("simulate_input: FKey name (W, SpaceBar, LeftMouseButton, Gamepad_LeftX, ...)"),
    inputEvent: z.enum(["tap", "press", "release"]).optional().describe("simulate_input: key event kind (default tap)"),
    holdSeconds: z.number().optional().describe("simulate_input tap: schedule the release N seconds later (non-blocking)"),
    amount: z.number().optional().describe("simulate_input: analog depression 0-1 (default 1)"),
    axisValue: z.number().optional().describe("simulate_input: inject an IE_Axis sample with this value instead of a key event"),
    condition: z.string().optional().describe("PIE condition: pie_running | pie_stopped | actor_exists | actor_gone | actor_count | property"),
    actorPath: z.string().optional().describe("PIE condition: actor path alternative to actorLabel"),
    expected: z.union([z.number(), z.boolean(), z.string()]).optional().describe("PIE condition: expected value for property/actor_count"),
    op: z.string().optional().describe("PIE condition comparison: == != > < >= <= contains (default ==)"),
    timeoutMs: z.number().optional().describe("wait_for_pie_event: total wait budget (default 30000)"),
    intervalMs: z.number().optional().describe("wait_for_pie_event: poll interval (default 250, min 100)"),
    start: z.array(z.number()).optional().describe("pie_line_trace: trace start [x,y,z] (omit for player camera)"),
    end: z.array(z.number()).optional().describe("pie_line_trace: trace end [x,y,z] (omit for camera-forward)"),
    distance: z.number().optional().describe("pie_line_trace: camera-forward trace length (default 10000)"),
    traceChannel: z.string().optional().describe("pie_line_trace: Visibility (default) | Camera | Pawn | WorldStatic | WorldDynamic | PhysicsBody"),
    steps: z.array(z.record(z.unknown())).optional().describe("run_pie_test_sequence: ordered step list (see action description)"),
    stopOnFailure: z.boolean().optional().describe("run_pie_test_sequence: stop at the first failed step (default true)"),
    channels: z.string().optional().describe("start_trace/profile_pie: Insights channel list (default 'default,frame,bookmark')"),
    tracePath: z.string().optional().describe("analyze_trace: path to a .utrace file"),
    durationSeconds: z.number().optional().describe("profile_pie: capture length (default 10)"),
    stopPieAfter: z.boolean().optional().describe("profile_pie: stop PIE when the capture ends (default false)"),
    subSequencePath: z.string().optional().describe("add_sequence_section Sub track: child LevelSequence asset path"),
    jobName: z.string().optional().describe("mrq_create_job: display name for the render job"),
    mapPath: z.string().optional().describe("mrq_create_job: map to render (default: currently loaded level)"),
    outputDirectory: z.string().optional().describe("mrq_create_job: frame output directory"),
    resolutionX: z.number().optional().describe("mrq_create_job: output width"),
    resolutionY: z.number().optional().describe("mrq_create_job: output height"),
    fileNameFormat: z.string().optional().describe("mrq_create_job: MRQ file name format string"),
    imageFormat: z.string().optional().describe("mrq_create_job: png (default) | jpg | bmp | exr"),
    addDeferredPass: z.boolean().optional().describe("mrq_create_job: add the deferred render pass (default true)"),
    clearQueue: z.boolean().optional().describe("mrq_create_job: empty the queue first (default false)"),
    newWindowWidth: z.number().optional().describe("configure_pie: Play-in-New-Window width (#671)"),
    newWindowHeight: z.number().optional().describe("configure_pie: Play-in-New-Window height (#671)"),
    pitch: z.number().optional().describe("pie_set_player_view: control-rotation pitch (#671)"),
    yaw: z.number().optional().describe("pie_set_player_view: control-rotation yaw (#671)"),
    roll: z.number().optional().describe("pie_set_player_view: control-rotation roll (#671)"),
    inputMode: z.string().optional().describe("stage_game_input: gameOnly|gameAndUI|uiOnly (#671)"),
    showMouseCursor: z.boolean().optional().describe("stage_game_input: show mouse cursor (#671)"),
  },
);

/* ── PIE test-loop orchestration ─────────────────────────────────────
 * Waiting lives here in the server: bridge handlers run on the editor's
 * game thread and must never sleep, so wait_for_pie_event polls the
 * single-tick check_pie_condition handler between engine ticks. */

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function pieConditionParams(p: Record<string, unknown>): Record<string, unknown> {
  return {
    condition: p.condition,
    actorLabel: p.actorLabel,
    actorPath: p.actorPath,
    className: p.className,
    propertyName: p.propertyName,
    expected: p.expected,
    op: p.op,
  };
}

async function waitForPieCondition(ctx: ToolContext, p: Record<string, unknown>): Promise<Record<string, unknown>> {
  const timeoutMs = typeof p.timeoutMs === "number" ? p.timeoutMs : 30_000;
  const intervalMs = Math.max(100, typeof p.intervalMs === "number" ? p.intervalMs : 250);
  const startedAt = Date.now();
  let polls = 0;
  let last: unknown = null;
  let lastError: string | null = null;

  for (;;) {
    polls++;
    try {
      last = await ctx.bridge.call("check_pie_condition", pieConditionParams(p));
      lastError = null;
      const obj = last as Record<string, unknown> | null;
      if (obj && obj.met === true) {
        return { met: true, waitedMs: Date.now() - startedAt, polls, value: obj.value, worldTimeSeconds: obj.worldTimeSeconds };
      }
    } catch (e) {
      // Transient errors (actor not spawned yet -> property read fails) count
      // as "not met yet"; the timeout bounds how long we keep trying.
      lastError = e instanceof Error ? e.message : String(e);
    }
    if (Date.now() - startedAt + intervalMs > timeoutMs) {
      return {
        met: false,
        timedOut: true,
        waitedMs: Date.now() - startedAt,
        polls,
        last,
        ...(lastError ? { lastError } : {}),
      };
    }
    await sleep(intervalMs);
  }
}

async function runAutomationTests(ctx: ToolContext, p: Record<string, unknown>): Promise<Record<string, unknown>> {
  const timeoutSeconds = typeof p.timeoutSeconds === "number" ? p.timeoutSeconds : 600;
  const start = (await ctx.bridge.call("start_automation_tests", {
    filter: p.filter,
    filters: p.filters,
    listOnly: p.listOnly,
    runAll: p.runAll,
    timeoutSeconds,
  })) as Record<string, unknown>;
  if (start && start.success === false) return start;

  // The bridge enforces its own timeout and salvages partial results; the
  // extra 30s here covers worker discovery + result collection overhead.
  const deadline = Date.now() + (timeoutSeconds + 30) * 1000;
  for (;;) {
    await sleep(1_500);
    let status: Record<string, unknown> | null = null;
    try {
      status = (await ctx.bridge.call("get_automation_test_status", {})) as Record<string, unknown>;
    } catch {
      // Transient bridge hiccup mid-run; keep polling until the deadline.
    }
    if (status && (status.phase === "complete" || status.phase === "failed")) {
      return status;
    }
    if (Date.now() > deadline) {
      return { success: false, error: `Automation run did not finish within ${timeoutSeconds + 30}s`, lastStatus: status };
    }
  }
}

async function profilePie(ctx: ToolContext, p: Record<string, unknown>): Promise<Record<string, unknown>> {
  const durationSeconds = Math.min(typeof p.durationSeconds === "number" ? p.durationSeconds : 10, 300);

  const started = (await ctx.bridge.call("start_trace", { channels: p.channels })) as Record<string, unknown>;
  const tracePath = started?.tracePath;

  let pieStartedHere = false;
  try {
    const status = (await ctx.bridge.call("check_pie_condition", { condition: "pie_running" })) as Record<string, unknown>;
    if (status?.met !== true) {
      await ctx.bridge.call("pie_control", { action: "start" }, 200_000);
      const wait = await waitForPieCondition(ctx, { condition: "pie_running", timeoutMs: 60_000 });
      if (wait.met !== true) {
        throw new McpError(ErrorCode.NO_HANDLER, "PIE did not start within 60s - trace aborted");
      }
      pieStartedHere = true;
      // Give the world a moment to settle so startup hitches don't dominate.
      await sleep(2_000);
    }
    await sleep(durationSeconds * 1000);
  } finally {
    try { await ctx.bridge.call("stop_trace", {}); } catch { /* trace may have stopped with an editor error */ }
    if (pieStartedHere && p.stopPieAfter === true) {
      try { await ctx.bridge.call("pie_control", { action: "stop" }); } catch { /* leave PIE state as-is */ }
    }
  }

  const analysis = await ctx.bridge.call("analyze_trace", { tracePath }, 180_000);
  return { tracePath, durationSeconds, pieStartedHere, analysis };
}

async function runPieTestSequence(ctx: ToolContext, p: Record<string, unknown>): Promise<Record<string, unknown>> {
  const steps = Array.isArray(p.steps) ? (p.steps as Record<string, unknown>[]) : null;
  if (!steps || steps.length === 0) {
    throw new McpError(ErrorCode.INVALID_PARAMS, "run_pie_test_sequence requires a non-empty 'steps' array");
  }
  const stopOnFailure = p.stopOnFailure !== false;

  const results: Record<string, unknown>[] = [];
  let passed = true;

  for (let i = 0; i < steps.length; i++) {
    const step = steps[i];
    const kind = String(step.do ?? step.action ?? "");
    const record: Record<string, unknown> = { index: i, do: kind };
    try {
      switch (kind) {
        case "start_pie": {
          await ctx.bridge.call("pie_control", { action: "start" }, 200_000);
          const wait = await waitForPieCondition(ctx, {
            condition: "pie_running",
            timeoutMs: typeof step.timeoutMs === "number" ? step.timeoutMs : 60_000,
          });
          record.ok = wait.met === true;
          record.detail = wait;
          break;
        }
        case "stop_pie": {
          await ctx.bridge.call("pie_control", { action: "stop" });
          const wait = await waitForPieCondition(ctx, { condition: "pie_stopped", timeoutMs: 15_000 });
          record.ok = wait.met === true;
          record.detail = wait;
          break;
        }
        case "input": {
          record.detail = await ctx.bridge.call("simulate_pie_input", {
            key: step.key,
            event: step.inputEvent ?? step.event,
            holdSeconds: step.holdSeconds,
            amount: step.amount,
            axisValue: step.axisValue,
          });
          record.ok = true;
          break;
        }
        case "wait": {
          const ms = typeof step.ms === "number" ? step.ms : (typeof step.seconds === "number" ? step.seconds * 1000 : 1000);
          await sleep(Math.min(ms, 120_000));
          record.ok = true;
          break;
        }
        case "wait_for": {
          const wait = await waitForPieCondition(ctx, step);
          record.ok = wait.met === true;
          record.detail = wait;
          break;
        }
        case "assert": {
          const check = (await ctx.bridge.call("check_pie_condition", pieConditionParams(step))) as Record<string, unknown>;
          record.ok = check?.met === true;
          record.detail = check;
          break;
        }
        case "console": {
          record.detail = await ctx.bridge.call("execute_command", { command: step.command });
          record.ok = true;
          break;
        }
        case "screenshot": {
          record.detail = await ctx.bridge.call("capture_screenshot", { filename: step.filename, target: "pie" });
          record.ok = true;
          break;
        }
        case "trace": {
          const trace = (await ctx.bridge.call("pie_line_trace", {
            start: step.start,
            end: step.end,
            distance: step.distance,
            channel: step.channel ?? step.traceChannel,
          })) as Record<string, unknown>;
          record.detail = trace;
          // A trace step with expectHit set becomes an assertion.
          record.ok = step.expectHit === undefined ? true : trace?.hit === step.expectHit;
          break;
        }
        default:
          record.ok = false;
          record.detail = `Unknown step kind '${kind}'. Expected start_pie, stop_pie, input, wait, wait_for, assert, console, screenshot, or trace.`;
      }
    } catch (e) {
      record.ok = false;
      record.detail = e instanceof Error ? e.message : String(e);
    }

    results.push(record);
    if (record.ok !== true) {
      passed = false;
      if (stopOnFailure) break;
    }
  }

  return { passed, completedSteps: results.length, totalSteps: steps.length, steps: results };
}
