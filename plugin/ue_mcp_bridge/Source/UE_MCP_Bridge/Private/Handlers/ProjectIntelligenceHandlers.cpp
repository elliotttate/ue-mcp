#include "ProjectIntelligenceHandlers.h"
#include "HandlerRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "Engine/World.h"
#endif

namespace
{
	TSharedPtr<FJsonValue> MakeErrorResult(const FString& Message)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("error"), Message);
		return MakeShared<FJsonValueObject>(Obj);
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
}

void FProjectIntelligenceHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("extract_index_summary"), &ExtractIndexSummary);
	Registry.RegisterHandler(TEXT("extract_index_summaries"), &ExtractIndexSummaries);
	Registry.RegisterHandler(TEXT("get_editor_context_bundle"), &GetEditorContextBundle);
}

TSharedPtr<FJsonObject> FProjectIntelligenceHandlers::BuildAssetSummary(const FString& Path)
{
	// Normalize "/Game/Foo.Foo" or "/Game/Foo" → package name "/Game/Foo".
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets);
	if (Assets.Num() == 0)
	{
		return nullptr;
	}

	const FAssetData& Asset = Assets[0];

	// Compact, embeddable text summary — name, classes, and selected registry
	// tags. Cheap: no asset is loaded into memory.
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Asset: %s"), *Asset.AssetName.ToString()));
	Lines.Add(FString::Printf(TEXT("Class: %s"), *Asset.AssetClassPath.ToString()));
	Lines.Add(FString::Printf(TEXT("Package: %s"), *PackageName));

	const FName ParentClassTag(TEXT("ParentClass"));
	const FName NativeParentTag(TEXT("NativeParentClass"));
	const FName GeneratedClassTag(TEXT("GeneratedClass"));
	const FName BlueprintTypeTag(TEXT("BlueprintType"));
	for (const FName& Tag : { ParentClassTag, NativeParentTag, GeneratedClassTag, BlueprintTypeTag })
	{
		FString Value;
		if (Asset.GetTagValue(Tag, Value) && !Value.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("%s: %s"), *Tag.ToString(), *Value));
		}
	}

	// Forward package dependencies → fed to the knowledge graph.
	TArray<FName> Deps;
	AssetRegistry.GetDependencies(FName(*PackageName), Deps);

	TArray<TSharedPtr<FJsonValue>> DepValues;
	for (const FName& Dep : Deps)
	{
		const FString DepStr = Dep.ToString();
		if (DepStr.StartsWith(TEXT("/Script/")))
		{
			continue; // native module reference, not an asset edge
		}
		DepValues.Add(MakeShared<FJsonValueString>(DepStr));
		if (DepValues.Num() >= 100)
		{
			break;
		}
	}
	if (DepValues.Num() > 0)
	{
		TArray<FString> DepNames;
		for (const TSharedPtr<FJsonValue>& D : DepValues)
		{
			DepNames.Add(D->AsString());
		}
		Lines.Add(TEXT("Dependencies: ") + FString::Join(DepNames, TEXT(", ")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("summary"), FString::Join(Lines, TEXT("\n")));
	Result->SetStringField(TEXT("name"), Asset.AssetName.ToString());
	Result->SetArrayField(TEXT("dependencies"), DepValues);
	return Result;
}

TSharedPtr<FJsonValue> FProjectIntelligenceHandlers::ExtractIndexSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing 'path'"));
	}
	TSharedPtr<FJsonObject> Summary = BuildAssetSummary(Path);
	if (!Summary.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("No asset found for %s"), *Path));
	}
	return MakeShared<FJsonValueObject>(Summary);
}

TSharedPtr<FJsonValue> FProjectIntelligenceHandlers::ExtractIndexSummaries(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("paths"), Paths) || Paths == nullptr)
	{
		return MakeErrorResult(TEXT("Missing 'paths' array"));
	}

	TArray<TSharedPtr<FJsonValue>> Summaries;
	for (const TSharedPtr<FJsonValue>& PathValue : *Paths)
	{
		FString Path;
		if (!PathValue.IsValid() || !PathValue->TryGetString(Path) || Path.IsEmpty())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Summary = BuildAssetSummary(Path);
		if (Summary.IsValid())
		{
			Summaries.Add(MakeShared<FJsonValueObject>(Summary));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("summaries"), Summaries);
	return MakeShared<FJsonValueObject>(Result);
}

TSharedPtr<FJsonValue> FProjectIntelligenceHandlers::GetEditorContextBundle(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Selection.
	TArray<TSharedPtr<FJsonValue>> Selection;
	if (GEditor)
	{
		if (USelection* Selected = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*Selected); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("label"), Actor->GetActorLabel());
					O->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					O->SetStringField(TEXT("path"), Actor->GetPathName());
					Selection.Add(MakeShared<FJsonValueObject>(O));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("selection"), Selection);
	Result->SetNumberField(TEXT("selectionCount"), Selection.Num());

	// Active editor viewport camera.
	if (GCurrentLevelEditingViewportClient)
	{
		TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
		Viewport->SetObjectField(TEXT("location"), VectorToJson(GCurrentLevelEditingViewportClient->GetViewLocation()));
		const FRotator Rot = GCurrentLevelEditingViewportClient->GetViewRotation();
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
		Viewport->SetObjectField(TEXT("rotation"), RotObj);
		Result->SetObjectField(TEXT("viewport"), Viewport);
	}

	// Current level.
	if (GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			Result->SetStringField(TEXT("level"), World->GetMapName());
		}
	}

	return MakeShared<FJsonValueObject>(Result);
#else
	return MakeErrorResult(TEXT("get_editor_context_bundle requires the editor"));
#endif
}
