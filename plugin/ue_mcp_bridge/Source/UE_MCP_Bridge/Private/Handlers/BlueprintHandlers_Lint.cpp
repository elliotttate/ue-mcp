// Blueprint anti-pattern report.
//
// lint_blueprints scans one Blueprint (assetPath) or a folder of them
// (pathFilter, recursive) and flags common performance / maintainability
// smells an agent should know about before extending a project:
//   - EventTick        : logic wired off Event Tick (per-frame cost)
//   - GetAllActorsTick : an expensive GetAllActorsOfClass alongside Tick logic
//   - LargeGraph       : a single graph with an unusually high node count
//
// It complements the project-intelligence index: a quick, on-demand quality
// pass rather than a persisted artifact.

#include "BlueprintHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Kind, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), Kind);
		Obj->SetStringField(TEXT("detail"), Detail);
		Issues.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// Scan one blueprint's graphs and return its issue list.
	TArray<TSharedPtr<FJsonValue>> LintOne(UBlueprint* BP, int32 LargeGraphThreshold)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!BP) return Issues;

		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);

		bool bTickConnected = false;
		bool bHasGetAllActors = false;
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			if (G->Nodes.Num() > LargeGraphThreshold)
			{
				AddIssue(Issues, TEXT("LargeGraph"),
					FString::Printf(TEXT("Graph '%s' has %d nodes (> %d)"), *G->GetName(), G->Nodes.Num(), LargeGraphThreshold));
			}
			for (UEdGraphNode* Node : G->Nodes)
			{
				if (!Node) continue;
				const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				if (Title.Contains(TEXT("Get All Actors Of Class"))) bHasGetAllActors = true;

				if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
				{
					if (Evt->EventReference.GetMemberName() == TEXT("ReceiveTick"))
					{
						for (UEdGraphPin* Pin : Evt->Pins)
						{
							if (Pin && Pin->Direction == EGPD_Output &&
								Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
								Pin->LinkedTo.Num() > 0)
							{
								bTickConnected = true;
							}
						}
					}
				}
			}
		}

		if (bTickConnected)
		{
			AddIssue(Issues, TEXT("EventTick"), TEXT("Logic runs on Event Tick (per-frame). Prefer timers/events where possible."));
		}
		if (bTickConnected && bHasGetAllActors)
		{
			AddIssue(Issues, TEXT("GetAllActorsTick"), TEXT("GetAllActorsOfClass present in a Blueprint that ticks - cache results instead of querying per frame."));
		}
		return Issues;
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::LintBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	const int32 LargeGraphThreshold = OptionalInt(Params, TEXT("largeGraphThreshold"), 300);
	const int32 Limit = FMath::Clamp(OptionalInt(Params, TEXT("limit"), 200), 1, 2000);

	// Resolve the target blueprint set: a single asset, or a recursive folder.
	TArray<FString> TargetPaths;
	const FString AssetPath = OptionalString(Params, TEXT("assetPath"));
	if (!AssetPath.IsEmpty())
	{
		TargetPaths.Add(AssetPath);
	}
	else
	{
		const FString PathFilter = OptionalString(Params, TEXT("pathFilter"), TEXT("/Game"));
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*PathFilter));
		TArray<FAssetData> Found;
		AR.GetAssets(Filter, Found);
		for (const FAssetData& Data : Found)
		{
			TargetPaths.Add(Data.GetObjectPathString());
			if (TargetPaths.Num() >= Limit) break;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Reports;
	int32 Scanned = 0;
	int32 Flagged = 0;
	for (const FString& Path : TargetPaths)
	{
		UBlueprint* BP = LoadBlueprint(Path);
		if (!BP) continue;
		Scanned++;
		TArray<TSharedPtr<FJsonValue>> Issues = LintOne(BP, LargeGraphThreshold);
		if (Issues.Num() == 0) continue;
		Flagged++;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Obj->SetArrayField(TEXT("issues"), Issues);
		Reports.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MCPSuccess();
	Result->SetNumberField(TEXT("scanned"), Scanned);
	Result->SetNumberField(TEXT("flagged"), Flagged);
	Result->SetArrayField(TEXT("blueprints"), Reports);
	if (TargetPaths.Num() >= Limit && AssetPath.IsEmpty())
	{
		Result->SetStringField(TEXT("note"), FString::Printf(TEXT("Capped at limit=%d; narrow pathFilter or raise limit for full coverage."), Limit));
	}
	return MCPResult(Result);
}
