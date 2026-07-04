#pragma once

// Helpers shared between BlueprintHandlers.cpp and BlueprintHandlers_Graph.cpp
// after the file was split. Kept in Private/ because it is internal to the
// plugin - no downstream code is expected to include this.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UActorComponent;
class UEdGraphNode;

// Resolve the named component template on a blueprint, honouring inheritance.
// See definition in BlueprintHandlers_Graph.cpp for the full contract (bForWrite
// semantics, ICH-override creation on write, CDO fallback on read, etc.).
UActorComponent* ResolveComponentTemplate(
	UBlueprint* Blueprint,
	const FString& ComponentName,
	bool bForWrite,
	bool& bOutIsInherited,
	TArray<FString>& OutAvailable);

// Compact post-mutation connection report for a set of freshly created nodes:
// node labels, exec/data links, dangling exec outputs, and unset data inputs.
// Returned inline by import_nodes_t3d and author_logic so agents can verify
// wiring without a follow-up read_graph round-trip. Defined in
// BlueprintHandlers_Graph.cpp.
TSharedPtr<FJsonObject> BuildCompactConnectionReport(const TArray<UEdGraphNode*>& Nodes);
