# Quest PSO harvests

The `quest_pso` tool runs the host side of an Android/Quest shader pipeline cache harvest. It does not use the editor bridge and does not compile Unreal. It shells out to `adb`, talks to a development-only in-game console server, and invokes `UnrealEditor-Cmd -run=ShaderPipelineCacheTools` only when expanding recordings.

## Safety model

- Every device action runs `adb devices -l` and requires exactly one listed device in the `device` state. A second, offline, or unauthorized device fails closed.
- `projectPath` and `enginePath` must be explicit absolute paths. The tool verifies the `.uproject`, `UnrealEditor-Cmd`, Android package, and cooked Android ASTC `.shk` files before clearing or publishing anything.
- `clear_launch` always supplies `-logPSO`, rejects `-NoLogPSO`, and clears `CollectedPSOs`, the writable Unreal pipeline cache, and the Vulkan program-binary cache before launching. If the local Build folders contain `.spc` files, it refuses to start unless `allowBundledCacheRisk=true` explicitly acknowledges a miss-only capture.
- RQDevServer commands use `adb forward tcp:0 ...`, so adb allocates a collision-free local port. The forward is removed after the single request.
- `collect_expand` requires a complete valid RQPSO tour by default, writes each pull to a unique timestamped directory, and verifies that expansion produced a non-empty `.spc` before copying it into either Build cache. Existing Build caches are refused by default; `replaceExistingBuildCaches=true` first backs up both old caches under the run directory and then replaces both through same-directory atomic renames with rollback.

## Device protocol

The expected development build exposes a line-oriented TCP server on device port 6788 (override with `devicePort`). Send one UTF-8 console command plus a newline; the server replies with one JSON line:

```json
{"ok":true,"output":""}
```

The `save` action sends the exact command `r.ShaderPipelineCache.Save`. RQDevServer's `ok` field proves that the game-thread dispatch completed, not that a file was written; `collect_expand` is the authoritative path because it verifies pulled recordings and the expanded output. The generic `command` action can forward other development console commands without opening a persistent port. Latest Roboquest main can be host-driven with commands such as `RQAction startsolo`, `RQAction level <row>`, `RQAction ready`, and `RQAction continue`, with state polled through `RQQuery loaded_level`, `RQQuery ready_gate`, `RQQuery level_gen_finished`, and related queries.

## Typical automated workflow

1. Cook and install an Android ASTC Development build. For a full regeneration, ensure the installed APK was cooked without a bundled `.spc`; otherwise Unreal records only misses from that cache.
2. Call `quest_pso(action="preflight", projectPath="D:/Projects/Game/Game.uproject", enginePath="D:/Engines/UE5", packageName="com.example.game")`.
3. Call `clear_launch`, then use `command` with the available `RQAction` and `RQQuery` verbs to drive the desired mainline coverage matrix.
4. For a build that includes the optional experimental RQPSO tour, call `validate` after its terminal marker. A valid marker run requires:
   - `TourStart levels=N`, with `N >= 1`;
   - exactly `N` `LevelDone` markers with unique indices;
   - no `TourError` marker;
   - `TourDone ok=1 levels=N psosLogged=P`, with `P >= 1`.
5. Call `collect_expand` with the same explicit paths and package. It sends `r.ShaderPipelineCache.Save`, stops the app, clears the debug command line, pulls every recursive `.upipelinecache`, expands all recordings with all cooked `.shk` files, rejects missing/empty output, and copies the validated `.spc` to:
   - `Build/Android/PipelineCaches`
   - `Build/Android_ASTC/PipelineCaches`

`requireCompleteTour` defaults to true and fails closed when the RQPSO marker tour is missing, partial, or invalid. Set it to false explicitly only for a legacy/no-tour build such as latest main without the experimental RQPSO tour. Recording and `.spc` artifact validation always runs even when marker validation is disabled. `saveBeforePull` and `copyToBuild` default to true and can be disabled for diagnostic-only collection. `replaceExistingBuildCaches` defaults to false.

## Actions

| Action | Purpose |
|--------|---------|
| `preflight` | Verify the one-device, package, engine, project, and stable-key prerequisites. |
| `clear_launch` | Clear old device caches and launch with mandatory `-logPSO`. |
| `command` | Send one RQDevServer console command through an ephemeral adb forward. |
| `save` | Send the exact `r.ShaderPipelineCache.Save` command. |
| `validate` | Parse the latest RQPSO marker sequence from UE logcat. |
| `collect_expand` | Validate, save, pull all recordings, expand, validate output, and copy the `.spc`. |
