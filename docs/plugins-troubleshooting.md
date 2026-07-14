# Validation and troubleshooting

The rules the loader enforces, and how to read the common failures when a plugin doesn't behave.

## Validation rules

These are enforced both at install (`ue-mcp plugin install`) and at server load:

- `actionPrefix` is mandatory and must match `/^[a-z][a-z0-9_]*$/`.
- Every `inject:` target must be a real registered category. A nonexistent target fails install with the list of valid categories.
- A plugin action may never overwrite a built-in. Collisions are hard-skipped with a warning.
- Every `provides:` category name must match `/^[a-z][a-z0-9_]*$/` and must not collide with a built-in category.
- Inter-plugin collisions resolve by `plugins:` order - first wins. Applies to both injected actions and provided category names.
- Every `inject:` and `provides:` entry must point to a task declared under `tasks:`, and every task's `class_path` must resolve under `dist/`.
- `minServerVersion` is checked at install and re-checked at load.
- `nativeModule.minBridgeApi` is checked at install (against the deployed bridge's `UEMCP_BRIDGE_API_VERSION`) and re-checked at load.
- A plugin that fails any of these is skipped entirely (never partially injected) with a loud warning. Other plugins keep loading.

## `plugins(action="list")` returns `pluginCount: 0`

The server didn't find any `plugins:` entries, or every entry failed validation. Check:

1. `ue-mcp.yml` exists in your project root next to the `.uproject` and has a top-level `plugins:` array.
2. Each `name:` is installed under `node_modules/`. Run `npm install` if the lockfile says it should be there.
3. The server's stderr log - every validation failure prints a `[ue-mcp] warn plugin: <package>: <reason>` line at boot.

## `uePluginPresent: false`

The npm-side plugin loaded fine, but the host Unreal plugin it declares as a dependency is missing from your `.uproject`. See [Host UE plugin dependencies](plugins-using.md#host-ue-plugin-dependencies) for the enable steps. The injected actions are still visible in the host category tools - they just won't run end-to-end until the UE plugin is enabled and built.

## `class_path '<path>' could not be resolved`

The plugin's `ue-mcp.plugin.yml` declared a task whose compiled JS file is missing from `dist/`. If you're authoring: run `npm run build` and confirm `dist/<path>.js` exists. If you're consuming: the package was published without its `dist/` directory - open an issue on the plugin's repo.

## `requires server >= <version>`

The plugin's `minServerVersion` is newer than the ue-mcp you're running. Update:

```bash
npm install ue-mcp@latest
```

Then restart your MCP client.

## Injected action appears in `plugins.describe` but not in the host category tool's action list

You restarted the editor but not the MCP server. They're separate processes - the editor restart doesn't respawn the npx-launched ue-mcp server. Reconnect MCP in your client (in Claude Code, `/mcp`).

## `nativeModule requires bridge ABI >= N`

The plugin needs a newer bridge than the one deployed in this project. Run `ue-mcp deploy` to refresh the bridge source, then `ue-mcp build` (or rebuild from the editor) before retrying. The deployed ABI is also visible in `project(action="get_status")` as `bridgeApiVersion`.

## Provided category does not show up as its own MCP tool

The plugin loaded but a name collision skipped its `provides:` entry. Run `plugins(action="describe", name="<package>")` and check the `provided` field. If it's empty, look at the server boot log for a `provides target '<category>' already claimed by '<other plugin>'` warning - earlier-listed plugins win, so reorder your `plugins:` array or drop one of the conflicting packages.

## Native module deployed but handlers come back `Unknown method`

The C++ side didn't compile in. Two common causes:

1. The user never rebuilt after install. Run `npm run build` from the project (or rebuild from the editor IDE) and confirm the new `.dll` lands under `Binaries/Win64/`.
2. The build failed silently because the deployed bridge is older than the plugin expects. Run `ue-mcp deploy` to refresh `MCPHandlerRegistration.h`, then `ue-mcp build`.

If the rebuild succeeds but `Unknown method` persists, you've hit a stale Live Coding patch: delete `<projectDir>/Binaries/Win64/*.patch_*` and rebuild clean. UBT's incremental build can otherwise shadow a freshly built DLL with a leftover patch.
