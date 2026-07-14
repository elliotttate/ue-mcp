# Native modules

When a plugin needs engine APIs ue-mcp's bridge doesn't already expose, it ships a UE C++ module alongside the npm package. The module compiles into the user's project at install time and registers handlers on the bridge via `UEMCP::RegisterExternalHandler`. This is Shape C from the [overview](plugins.md); most plugins never need it.

`pie-studio` is a real-world example of this shape. Its manifest:

```yaml
actionPrefix: pie                    # used only when injecting into a built-in

nativeModule:
  uePluginName: PIE_Studio           # name of the .uplugin that gets deployed
  minBridgeApi: 1                    # gate against UEMCP_BRIDGE_API_VERSION
  source: ue/Plugins/PIE_Studio      # path inside your npm tarball
  category: pie                      # surface handlers under a pie(...) tool
  categoryDescription: "PIE record, replay, observe, and input injection"
  handlers:
    record_arm:   { description: "Arm the PIE input recorder" }
    replay_arm:   { description: "Arm the PIE input replayer" }
    inject_input:
      description: "Single-frame Enhanced Input inject"
      timeoutSeconds: 5
      schema:
        action_path: { type: string, description: "InputAction asset path (required)" }
        value_x:     { type: number }
    # ... more handlers
```

## How handlers become MCP actions

Set `category` and ue-mcp surfaces every handler as an MCP action that dispatches to the bare bridge method your C++ registered (`record_arm` above). No TypeScript task class is involved - the C++ handler *is* the implementation. The category value picks one of two shapes:

- **A new (non-built-in) category** - as in the `pie` example above - is **provisioned as its own top-level tool** the plugin owns. Actions are **not** prefixed (the category is the namespace): `pie(action="record_arm")`. Set `categoryDescription` for the tool's summary. This is the right choice when the handlers form their own domain. Cross-plugin name collisions resolve first-wins, like `provides:`.
- **A built-in category** (e.g. `gameplay`) **injects** the handlers into that existing tool, prefixed with `actionPrefix`: handler `record_arm` becomes `gameplay(action="pie_record_arm")`. Choose this when the handlers belong inside a category that already exists.

Two rules that bite if missed:

- **Declare params under each handler's `schema:`.** The MCP SDK strips any param not in the action's schema before it reaches the bridge, so an undeclared param silently never arrives. Same field types as `inject:` schemas. Params-free handlers (status polls, list calls) need no schema. Leave params **optional** (ue-mcp forces them optional regardless): one flat schema backs every action in a category, so a required param would be forced onto unrelated actions - let your C++ handler validate and return a clear error, and note "(required)" in the param description.
- **`timeoutSeconds`** sets the bridge-call timeout for that action (default 30s). Raise it for long-running handlers.

Omit `category` entirely and handlers are still registered on the bridge but exposed as no MCP action - useful only if another task calls them internally. For an agent-facing plugin you almost always want `category`.

## Layout inside the npm tarball

```
pie-studio/
  ue-mcp.plugin.yml
  dist/                              # tsc output (TypeScript tasks, if any)
  ue/                                # native source ships here
    Plugins/
      PIE_Studio/
        PIE_Studio.uplugin
        Source/
          PIE_Studio/
            PIE_Studio.Build.cs
            Private/
              Handlers/              # handler .cpp files
              PIE/                   # engine subsystem wrappers
              UI/                    # editor UI panels
```

Update `package.json` `files:` so the `ue/` directory ships with the published tarball:

```json
"files": ["dist", "ue", "ue-mcp.plugin.yml", "knowledge", "README.md"]
```

## The native module

Add `UE_MCP_Bridge` to `PrivateDependencyModuleNames` in your `.Build.cs`:

```csharp
public class PIE_Studio : ModuleRules
{
    public PIE_Studio(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });
        PrivateDependencyModuleNames.AddRange(new string[] { "UE_MCP_Bridge" });
    }
}
```

Register handlers from `StartupModule`:

```cpp
#include "MCPHandlerRegistration.h"

void FPIE_StudioModule::StartupModule()
{
    UEMCP::RegisterExternalHandler(
        TEXT("inject_input"),
        [](const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonValue>
        {
            // ... do the work, return a JSON value
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), true);
            return MakeShared<FJsonValueObject>(Result);
        });
}

void FPIE_StudioModule::ShutdownModule()
{
    UEMCP::UnregisterExternalHandler(TEXT("inject_input"));
}
```

The handler's method name (`inject_input`) is the bare bridge method. It's what an auto-surfaced action (`gameplay(action="pie_inject_input")`, via `nativeModule.category`) dispatches to, what a TypeScript task can address through `this.call(...)`, and what the bridge looks up on any dispatch. Register it bare - ue-mcp adds the `actionPrefix` when it surfaces the action.

## Install flow

```bash
ue-mcp plugin install pie-studio
```

The CLI now also:

1. Reads `MCPHandlerRegistration.h` from the deployed bridge and checks that `UEMCP_BRIDGE_API_VERSION >= manifest.nativeModule.minBridgeApi`. Install fails fast if the bridge is too old, with a pointer to `ue-mcp deploy`.
2. Copies `<pkgDir>/<source>` to `<projectDir>/Plugins/<uePluginName>/`.
3. Records every copied file in `<projectDir>/.ue-mcp/native-modules.json` so `ue-mcp plugin uninstall` can clean up without nuking user edits.
4. Prints `REBUILD REQUIRED` - the user must build the UE project before launching the editor so the new module compiles in.

## Bridge ABI versioning

`UEMCP_BRIDGE_API_VERSION` is the C++ ABI contract every native plugin compiles against. Bumps are reserved for breaking changes to the `FExternalHandlerFn` signature or the registration contract. A plugin declaring `minBridgeApi: N` refuses to load against a bridge whose version is below N. Inspect the deployed bridge's version with:

```text
project(action="get_status")
```

The response includes `bridgeApiVersion` when a bridge is deployed.
