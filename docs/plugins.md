# Plugins

ue-mcp's plugin system lets npm packages extend the server in three ways:

- **Inject** new actions into ue-mcp's built-in categories so agents discover them where they're already working.
- **Provide** entirely new top-level categories that the plugin owns end-to-end.
- **Ship native C++** that registers handlers directly with the editor bridge, opening up engine APIs that have no built-in coverage.

Most plugins use only the first shape; the other two are available when injection is the wrong fit. This section splits into [using a plugin somebody else wrote](plugins-using.md) and [writing your own](plugins-authoring.md).

!!! info "Live reference"
    [`pie-studio`](https://github.com/db-lyon/pie-studio) ([npm](https://www.npmjs.com/package/pie-studio)) is the canonical native-module reference. It ships C++ handlers for PIE recording, replay, observation, and input injection, surfaced as a `pie` category it provisions via `nativeModule.category`. See [Native C++ modules](plugins-native-modules.md).

## Quick start

In your Unreal project directory:

```bash
ue-mcp plugin install pie-studio
```

That runs `npm install --save`, validates the plugin's manifest, deploys the native C++ module, and adds an entry to your `ue-mcp.yml`. Rebuild the UE project so the native module compiles in, then restart your MCP client (in Claude Code, `/mcp` reconnects).

Verify with the introspection tool:

```text
plugins(action="list")
```

```json
{
  "pluginCount": 1,
  "active": 1,
  "plugins": [
    {
      "name": "pie-studio",
      "version": "0.0.2",
      "actionPrefix": "",
      "status": "active",
      "categories": ["gameplay"],
      "injectedActions": 33,
      "flows": 0,
      "nativeModule": "PIE_Studio"
    }
  ]
}
```

Once `status: "active"`, the injected actions (e.g. `gameplay(action="pie_record_arm", ...)`) are callable end-to-end. Full consumer details are in [Using plugins](plugins-using.md).

## How plugins work

A plugin is a normal npm package that ships:

- A `ue-mcp.plugin.yml` manifest declaring an `actionPrefix`, the actions it injects into which host categories, and the task classes that back them.
- Compiled task classes (one per injected action) under `dist/`, each extending `UeMcpTask` from [`ue-mcp/task`](plugins-authoring.md#writing-tasks).
- Optional `knowledge/<category>.md` markdown that the server attaches to the host category's AI-facing docs at boot.
- Optional `flows:` entries that compose injected actions with built-ins.

At server start, ue-mcp:

1. Reads `plugins:` from your project's `ue-mcp.yml`.
2. Resolves each entry against `<project>/node_modules/`.
3. Loads and validates each plugin's `ue-mcp.plugin.yml`.
4. Imports its task classes and registers them with the flow runtime.
5. Merges the injected actions into the host category tools - the action shows up as `<category>(action="<prefix>_<bare>", ...)`.
6. Concatenates the plugin's knowledge files into the host categories' AI-facing docs.

The injection happens before any tool is registered with the MCP client, so by the time the agent sees the `gameplay` tool's action list, the plugin's actions are already there alongside the built-ins.

## Three shapes a plugin can take

| Shape | Manifest blocks | When to reach for it |
|-------|-----------------|----------------------|
| **A. Inject only** | `inject:` | The action belongs inside an existing category. Default choice. |
| **B. Provide a new category** | `provides:` (with or without `inject:`) | The plugin opens a whole new domain - audio middleware, build pipelines, networking layers - that doesn't fit inside any built-in category. |
| **C. Ship native C++** | `nativeModule:` (with `category:` to surface its handlers) | The plugin needs engine APIs ue-mcp's built-in handlers don't expose. The plugin ships a UE C++ module that registers handlers on the editor bridge; `nativeModule.category` surfaces them as actions with no TypeScript. |

Shape A is overwhelmingly the right answer. An action that belongs inside an existing category is best discovered where agents are already working.

Shape B is for genuinely new domains. If your plugin's actions don't fit anywhere in the built-in category list, owning a new top-level category is cleaner than forcing a misfit injection.

Shape C is for capability that can't be expressed through orchestration of existing actions. The plugin ships C++ source that compiles into the user's project alongside the bridge, and registers handlers via `UEMCP::RegisterExternalHandler` from its `StartupModule`. Native handlers participate in the same dispatch path as built-in ones. `pie-studio` is a Shape C plugin. See [Native C++ modules](plugins-native-modules.md).

## Official plugins

| Plugin | What it does |
|--------|-------------|
| [`pie-studio`](https://github.com/db-lyon/pie-studio) | PIE recording, replay, observation, and input injection. 33 native C++ handlers injected into `gameplay`. |

## Where to next

- [Using plugins](plugins-using.md) - install, configure, introspect, and manage plugins in a project.
- [Authoring a plugin](plugins-authoring.md) - scaffold, manifest, tasks, provided categories, and publishing.
- [Native C++ modules](plugins-native-modules.md) - ship engine APIs the bridge doesn't expose.
- [Guards](plugins-guards.md) - gate every bridge call: source control, access policy, audit.
- [Validation and troubleshooting](plugins-troubleshooting.md) - the rules the loader enforces and common failures.
- [PIE Studio](pie-record-replay.md) - the canonical native-module plugin, documented end to end.
