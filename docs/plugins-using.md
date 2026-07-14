# Using plugins

Installing, configuring, and managing plugins in a project. If you're writing a plugin, see [Authoring a plugin](plugins-authoring.md).

## Installing

The supported install path is:

```bash
ue-mcp plugin install <package-name>
```

It's a thin wrapper that:

1. Runs `npm install --save <package-name>` so the package lands in `node_modules/` and is recorded in your `package.json`.
2. Validates the plugin's `ue-mcp.plugin.yml` - checks that `actionPrefix` is a legal identifier, every `inject:` target is a real registered category, every `class_path` resolves, and `minServerVersion` is satisfied.
3. Appends a `- name: <package-name>` entry to your `ue-mcp.yml`'s `plugins:` array (creating the array if needed).
4. Prints the restart instruction.

You can also install manually - `npm install --save <package-name>` and edit `ue-mcp.yml` yourself. The end state is identical.

## The `plugins:` array

The consumer surface is a single block in `ue-mcp.yml`:

```yaml
plugins:
  - name: pie-studio
  - name: some-other-plugin
    version: "^0.2.0"     # optional; npm semver range against package.json
```

Each entry resolves against the project's `node_modules/`. If `version` is omitted, whatever is currently installed loads. Order matters - see [Ordering and collisions](#ordering-and-collisions).

## Introspection

Two read-only actions on the `plugins` category:

| Action | What it returns |
|--------|-----------------|
| `plugins(action="list")` | Every plugin: name, version, prefix, status, count of injected actions and flows, host UE plugin dependency check. |
| `plugins(action="describe", name="<package>")` | Full detail for one plugin: the same fields as `list`, plus the actual injected action names, knowledge file paths, flows, and the resolved package + manifest paths on disk. |

Both reflect the live state of the server, so they're the right tool when something looks wrong - see [Troubleshooting](plugins-troubleshooting.md).

## Host UE plugin dependencies

A plugin can declare a single Unreal-side dependency in its manifest:

```yaml
uePluginDependency: SomePlugin
```

This is the **`.uplugin` filename** - the same string that appears as `Plugins[].Name` in your `.uproject`. ue-mcp checks for it at server start and reports the result as `uePluginPresent` in `plugins(action="list")`.

The check is informational, not gating: the npm-side plugin loads regardless, and its injected actions appear in the host category tools. But until the UE plugin is enabled in `.uproject` and its C++ modules are built, the actions will fail at execute time with a clear error.

To enable a host UE plugin:

1. Add `{ "Name": "<DepName>", "Enabled": true }` to your `.uproject`'s `Plugins` array.
2. Build the project (e.g. `npm run build` or `editor(action="build_all")`).
3. Restart the editor.
4. Run `plugins(action="list")` to confirm `uePluginPresent: true`.

For source-distributed UE plugins, drop the source under `Plugins/<DepName>/` - either as a git submodule (recommended for size) or as a vendored copy. The `.uplugin` file inside that directory is what UE's plugin discovery walks.

## Ordering and collisions

- **Plugin vs built-in:** A plugin action can never override a built-in. Collisions are hard-skipped at load time with a warning in the server log; the built-in stays.
- **Plugin vs plugin:** First entry in `plugins:` wins. If two plugins both inject `gameplay.foo_bar`, only the earlier-listed one's version is registered. The order is intentionally stable - your `ue-mcp.yml` is the source of truth for resolution.
- **Failed plugins are skipped, not partially loaded.** If a plugin fails validation (bad manifest, missing class_path, server-version mismatch, etc.), it is dropped entirely with a loud warning. Other plugins keep loading. The host tools are never partially mutated.

## Removing a plugin

There is no separate uninstall command - `npm uninstall <package-name>` and delete the entry from `ue-mcp.yml`. On next restart, the actions are gone.
