#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

/**
 * Handlers that accelerate the server-side Project Intelligence layer
 * (indexing / context capture). These are OPTIONAL fast paths: when present the
 * TypeScript server prefers them, and when absent it composes existing reads
 * (read_blueprint_graph_summary + get_blueprint_dependencies; get_selected_actors
 * + get_viewport_info), so the feature works with or without this handler.
 *
 * - extract_index_summary: a cheap, asset-registry-only blueprint/asset summary
 *   (name, classes, tags, dependencies) suitable for project-wide indexing
 *   without loading every asset.
 * - get_editor_context_bundle: the live selection + viewport + level in one call.
 */
class FProjectIntelligenceHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	static TSharedPtr<FJsonValue> ExtractIndexSummary(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetEditorContextBundle(const TSharedPtr<FJsonObject>& Params);
};
