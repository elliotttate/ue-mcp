import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";

// Fab asset importer. Fab is Epic's unified content marketplace; UE ships an
// editor plugin whose window is a web frontend (catalog browse, library,
// purchase, and signed-URL resolution all live on Epic's servers) sitting on a
// thin native download+import layer. This category drives the native pieces
// that don't need the web frontend: login lifecycle, syncing the user's owned
// library into the Content Browser, inspecting/clearing the download cache, and
// importing owned/local source files through the Fab import pipeline.
//
// Store catalog browsing and buying are intentionally out of scope: they need
// the authenticated Fab backend (no official consumer REST API exists) and stay
// in the web window. Log in there once, add items to your library, then use
// sync_library + import_file here.
export const fabTool: ToolDef = categoryTool(
  "fab",
  "Import Fab (Epic marketplace) content: check plugin/login status, trigger login/logout, sync your owned library into the Content Browser, inspect/clear the download cache, and import owned or local source files into the project.",
  {
    status:       bp("Report Fab plugin state: whether the module is loaded, whether the native import/cache API is linked in this build, whether the Fab window has been opened this session, and the download cache location/size. Call this first.", "fab_status", () => ({})),
    login:        bp("Trigger the Fab login flow (EOS account portal). Asynchronous - returns once the flow is opened, not once authenticated. Complete any prompt, then call status.", "fab_login", () => ({})),
    logout:       bp("Clear the persistent Fab authentication for this device.", "fab_logout", () => ({})),
    sync_library: bp("Load the user's owned Fab library (\"My Folder\") into the Content Browser via TEDS. Requires an active login; items appear asynchronously. Params: batchSize? (items per sync request).", "fab_sync_library", (p) => ({ batchSize: p.batchSize })),
    list_cached:  bp("List the entries currently in the local Fab download cache (already-downloaded owned assets). Params: none.", "fab_list_cached", () => ({})),
    cache_info:   bp("Report the Fab download cache location, total size, and entry count.", "fab_cache_info", () => ({})),
    clear_cache:  bp("Delete the local Fab download cache to reclaim disk. Does not affect assets already imported into the project.", "fab_clear_cache", () => ({})),
    import_file:  bp("Import a source file into the project through the Fab Interchange import pipeline. Use for owned assets that are downloaded/cached locally, or any local source file (fbx, textures). Single files import synchronously and report the created asset paths; pack/quixel workflows may run asynchronously. Params: source (absolute path to the source file on disk), destination (content path like /Game/Fab/Imported).", "fab_import_file", (p) => ({ source: p.source, destination: p.destination })),
  },
  undefined,
  {
    batchSize: z.number().optional().describe("sync_library: number of library items to pull per sync request"),
    source: z.string().optional().describe("import_file: absolute path to the source file on disk"),
    destination: z.string().optional().describe("import_file: destination content path, e.g. /Game/Fab/Imported"),
  },
);
