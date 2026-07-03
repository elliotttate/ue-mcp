#include "ProjectIntelligenceHandlers.h"
#include "HandlerRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
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

	/**
	 * Crash-recovery guard for index extraction. Before each asset is summarized
	 * its path is written to a sentinel file; the sentinel is deleted when the
	 * batch completes. A sentinel that survives into the next editor session
	 * means that asset took the editor down mid-extraction, so it is appended to
	 * a permanent skip list and never extracted again. Both files live under
	 * Saved/UEMCP/ and are plain text - delete extract_skip_list.txt to retry.
	 */
	namespace ExtractCrashGuard
	{
		FString GuardDir() { return FPaths::ProjectSavedDir() / TEXT("UEMCP"); }
		FString SentinelPath() { return GuardDir() / TEXT("extract_in_progress.txt"); }
		FString SkipListPath() { return GuardDir() / TEXT("extract_skip_list.txt"); }

		TSet<FString>& SkipSet()
		{
			static TSet<FString> Set;
			return Set;
		}

		/** Promote a stale sentinel (left by a crashed session) to the skip list,
		 *  then load the skip list. Runs once per editor session. */
		void Initialize()
		{
			static bool bInitialized = false;
			if (bInitialized)
			{
				return;
			}
			bInitialized = true;

			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			FString StalePath;
			if (FFileHelper::LoadFileToString(StalePath, *SentinelPath()))
			{
				StalePath.TrimStartAndEndInline();
				if (!StalePath.IsEmpty())
				{
					UE_LOG(LogTemp, Warning,
						TEXT("[UE-MCP] Index extraction of '%s' did not survive the previous editor session; adding it to %s"),
						*StalePath, *SkipListPath());
					FFileHelper::SaveStringToFile(StalePath + LINE_TERMINATOR, *SkipListPath(),
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
						&IFileManager::Get(), FILEWRITE_Append);
				}
				PlatformFile.DeleteFile(*SentinelPath());
			}

			TArray<FString> Lines;
			if (FFileHelper::LoadFileToStringArray(Lines, *SkipListPath()))
			{
				for (FString& Line : Lines)
				{
					Line.TrimStartAndEndInline();
					if (!Line.IsEmpty())
					{
						SkipSet().Add(Line);
					}
				}
			}
		}

		bool IsSkipped(const FString& Path)
		{
			return SkipSet().Contains(Path);
		}

		void BeginAsset(const FString& Path)
		{
			FFileHelper::SaveStringToFile(Path, *SentinelPath(),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void EndBatch()
		{
			FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*SentinelPath());
		}
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
	if (Assets.Num() == 0 && AssetRegistry.IsLoadingAssets())
	{
		// Fresh editor boot: the background asset scan may not have reached this
		// path yet. Force a targeted synchronous scan of the package's directory
		// (cheap; siblings in the same folder resolve immediately afterwards) and
		// retry once, so an index kicked off right after launch still resolves.
		AssetRegistry.ScanPathsSynchronous({ FPackageName::GetLongPackagePath(PackageName) }, false);
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets);
	}
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
	ExtractCrashGuard::Initialize();
	if (ExtractCrashGuard::IsSkipped(Path))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Skipped: %s crashed the editor during a previous extraction (delete Saved/UEMCP/extract_skip_list.txt to retry)"), *Path));
	}
	ExtractCrashGuard::BeginAsset(Path);
	TSharedPtr<FJsonObject> Summary = BuildAssetSummary(Path);
	ExtractCrashGuard::EndBatch();
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

	ExtractCrashGuard::Initialize();

	TArray<TSharedPtr<FJsonValue>> Summaries;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	for (const TSharedPtr<FJsonValue>& PathValue : *Paths)
	{
		FString Path;
		if (!PathValue.IsValid() || !PathValue->TryGetString(Path) || Path.IsEmpty())
		{
			continue;
		}
		if (ExtractCrashGuard::IsSkipped(Path))
		{
			Skipped.Add(MakeShared<FJsonValueString>(Path));
			continue;
		}
		ExtractCrashGuard::BeginAsset(Path);
		TSharedPtr<FJsonObject> Summary = BuildAssetSummary(Path);
		if (Summary.IsValid())
		{
			Summaries.Add(MakeShared<FJsonValueObject>(Summary));
		}
	}
	ExtractCrashGuard::EndBatch();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("summaries"), Summaries);
	if (Skipped.Num() > 0)
	{
		Result->SetArrayField(TEXT("skipped"), Skipped);
	}
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
