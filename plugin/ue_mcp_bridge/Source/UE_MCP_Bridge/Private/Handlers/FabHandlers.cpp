#include "FabHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "Modules/ModuleManager.h"
#include "UObject/UObjectHash.h"
#include "Misc/PackageName.h"
#include "Engine/Engine.h"

#ifndef WITH_FAB_PLUGIN
#define WITH_FAB_PLUGIN 0
#endif

#if WITH_FAB_PLUGIN
#include "Importers/GenericAssetImporter.h"
#include "Utilities/FabAssetsCache.h"
#endif

// ─── Helpers ──────────────────────────────────────────────────────────────

static const TCHAR* FabModuleName = TEXT("Fab");

// The Fab plugin is an engine editor plugin, enabled by default on 5.8 but not
// guaranteed to be present or loaded (disabled, or an older engine). Every
// action funnels through this so callers get a clear message instead of a
// crash or an "Unknown method".
static bool IsFabModuleLoaded()
{
	return FModuleManager::Get().IsModuleLoaded(FabModuleName);
}

// Run one of the Fab plugin's registered console commands (Fab.Login,
// Fab.Logout, Fab.ClearCache, Fab.TEDS.MyFolderIntegration, ...). These are
// FAutoConsoleCommands, so they route through GEngine->Exec without any
// compile-time dependency on the Fab module - the login/sync/clear paths work
// even when WITH_FAB_PLUGIN is off (as long as the module is loaded).
static bool RunFabConsoleCommand(const FString& Command)
{
	if (!GEngine) return false;
	return GEngine->Exec(nullptr, *Command);
}

// Best-effort login/window hint: whether a UFabBrowserApi (the JS<->native
// bridge object created with the Fab window) currently exists. Reached by
// reflection so we never link the plugin's Private headers. Not a definitive
// auth check - the real login state lives in EOS behind private symbols - but
// a useful "has the Fab window been opened this session" signal.
static bool HasFabBrowserApiInstance()
{
	UClass* ApiClass = FindObject<UClass>(nullptr, TEXT("/Script/Fab.FabBrowserApi"));
	if (!ApiClass) return false;
	TArray<UObject*> Instances;
	GetObjectsOfClass(ApiClass, Instances, /*bIncludeDerivedClasses=*/true, RF_ClassDefaultObject);
	return Instances.Num() > 0;
}

// ─── Actions ──────────────────────────────────────────────────────────────

TSharedPtr<FJsonValue> FFabHandlers::Status(const TSharedPtr<FJsonObject>& Params)
{
	auto Res = MCPSuccess();
	const bool bLoaded = IsFabModuleLoaded();
	Res->SetBoolField(TEXT("pluginLoaded"), bLoaded);
	// Compile-time link state: whether cache/import actions are backed by the
	// native Fab API in this build, vs. console-command-only.
	Res->SetBoolField(TEXT("nativeApiLinked"), WITH_FAB_PLUGIN ? true : false);
	Res->SetBoolField(TEXT("fabWindowOpened"), HasFabBrowserApiInstance());

	if (!bLoaded)
	{
		Res->SetStringField(TEXT("note"), TEXT("Fab plugin module not loaded. It ships enabled by default on UE 5.8; enable it in the editor's Plugins panel if missing."));
		return MCPResult(Res);
	}

#if WITH_FAB_PLUGIN
	Res->SetStringField(TEXT("cacheLocation"), FFabAssetsCache::GetCacheLocation());
	Res->SetNumberField(TEXT("cacheSizeBytes"), (double)FFabAssetsCache::GetCacheSize());
	Res->SetStringField(TEXT("cacheSize"), FFabAssetsCache::GetCacheSizeString().ToString());
#endif
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FFabHandlers::Login(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
	// Fab.Login opens the EOS account-portal login flow. Asynchronous: this
	// returns once the flow is triggered, not once the user has authenticated.
	if (!RunFabConsoleCommand(TEXT("Fab.Login")))
		return MCPError(TEXT("Failed to invoke Fab.Login console command"));
	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("note"), TEXT("Login flow triggered. Complete authentication in the account-portal window if prompted; call fab(status) afterward."));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FFabHandlers::Logout(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
	if (!RunFabConsoleCommand(TEXT("Fab.Logout")))
		return MCPError(TEXT("Failed to invoke Fab.Logout console command"));
	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("note"), TEXT("Persistent Fab auth cleared."));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FFabHandlers::SyncLibrary(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
	// Loads the user's owned Fab library ("My Folder") into TEDS so it surfaces
	// in the Content Browser. Optional batch size controls how many items are
	// pulled per sync request.
	const int32 BatchSize = OptionalInt(Params, TEXT("batchSize"), 0);
	const FString Command = BatchSize > 0
		? FString::Printf(TEXT("Fab.TEDS.MyFolderIntegration %d"), BatchSize)
		: FString(TEXT("Fab.TEDS.MyFolderIntegration"));
	if (!RunFabConsoleCommand(Command))
		return MCPError(TEXT("Failed to invoke Fab.TEDS.MyFolderIntegration console command"));
	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("note"), TEXT("Library sync queued. Owned Fab items load into the Content Browser asynchronously; requires an active Fab login."));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FFabHandlers::ListCached(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
#if WITH_FAB_PLUGIN
	auto Res = MCPSuccess();
	const TArray<FString> Cached = FFabAssetsCache::GetCachedAssets();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& Entry : Cached)
	{
		Arr.Add(MakeShared<FJsonValueString>(Entry));
	}
	Res->SetArrayField(TEXT("cachedAssets"), Arr);
	Res->SetNumberField(TEXT("count"), Cached.Num());
	Res->SetStringField(TEXT("cacheLocation"), FFabAssetsCache::GetCacheLocation());
	return MCPResult(Res);
#else
	return MCPError(TEXT("Fab native API not linked in this build (Fab plugin absent at build time). Cache listing unavailable."));
#endif
}

TSharedPtr<FJsonValue> FFabHandlers::CacheInfo(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
#if WITH_FAB_PLUGIN
	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("cacheLocation"), FFabAssetsCache::GetCacheLocation());
	Res->SetNumberField(TEXT("cacheSizeBytes"), (double)FFabAssetsCache::GetCacheSize());
	Res->SetStringField(TEXT("cacheSize"), FFabAssetsCache::GetCacheSizeString().ToString());
	Res->SetNumberField(TEXT("count"), FFabAssetsCache::GetCachedAssets().Num());
	return MCPResult(Res);
#else
	return MCPError(TEXT("Fab native API not linked in this build (Fab plugin absent at build time). Cache info unavailable."));
#endif
}

TSharedPtr<FJsonValue> FFabHandlers::ClearCache(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
	// Routed through the console command so this works without WITH_FAB_PLUGIN.
	if (!RunFabConsoleCommand(TEXT("Fab.ClearCache")))
		return MCPError(TEXT("Failed to invoke Fab.ClearCache console command"));
	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("note"), TEXT("Fab download cache cleared."));
	return MCPResult(Res);
}

TSharedPtr<FJsonValue> FFabHandlers::ImportFile(const TSharedPtr<FJsonObject>& Params)
{
	if (!IsFabModuleLoaded()) return MCPError(TEXT("Fab plugin not loaded"));
#if WITH_FAB_PLUGIN
	FString Source;
	if (auto Err = RequireStringAlt(Params, TEXT("source"), TEXT("sourceFile"), Source)) return Err;
	FString Destination;
	if (auto Err = RequireStringAlt(Params, TEXT("destination"), TEXT("destPath"), Destination)) return Err;

	if (!FPaths::FileExists(Source))
		return MCPError(FString::Printf(TEXT("Source file not found on disk: %s"), *Source));
	if (!Destination.StartsWith(TEXT("/")))
		return MCPError(FString::Printf(TEXT("Destination must be a content path like /Game/Fab/Imported (got '%s')"), *Destination));
	if (!FPackageName::IsValidLongPackageName(Destination, /*bIncludeReadOnlyRoots=*/true))
		return MCPError(FString::Printf(TEXT("Destination is not a valid content path: %s"), *Destination));

	// FFabGenericImporter::ImportAsset runs the Fab Interchange pipeline and
	// delivers the created objects via callback. Single source files (fbx,
	// textures) complete on the game thread synchronously, so the callback has
	// fired by the time ImportAsset returns; pack/quixel workflows can defer.
	// We capture whatever the callback produced and report accordingly rather
	// than blocking the game thread.
	TSharedRef<bool> bCompleted = MakeShared<bool>(false);
	TSharedRef<TArray<FString>> ImportedPaths = MakeShared<TArray<FString>>();

	FFabGenericImporter::ImportAsset(
		{ Source },
		Destination,
		[bCompleted, ImportedPaths](const TArray<UObject*>& Objects)
		{
			*bCompleted = true;
			for (const UObject* Obj : Objects)
			{
				if (Obj) ImportedPaths->Add(Obj->GetPathName());
			}
		});

	auto Res = MCPSuccess();
	Res->SetStringField(TEXT("source"), Source);
	Res->SetStringField(TEXT("destination"), Destination);
	if (*bCompleted)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& P : *ImportedPaths)
		{
			Arr.Add(MakeShared<FJsonValueString>(P));
		}
		Res->SetArrayField(TEXT("importedAssets"), Arr);
		Res->SetNumberField(TEXT("count"), ImportedPaths->Num());
		MCPSetCreated(Res);
	}
	else
	{
		// Async workflow (pack extraction, quixel gltf, plugin install): the
		// import is running and will land in Destination shortly.
		Res->SetBoolField(TEXT("async"), true);
		Res->SetStringField(TEXT("note"), FString::Printf(TEXT("Import running asynchronously into %s; poll asset(list) on that path to confirm."), *Destination));
	}
	return MCPResult(Res);
#else
	return MCPError(TEXT("Fab native API not linked in this build (Fab plugin absent at build time). Import unavailable."));
#endif
}

// ─── Registration ──────────────────────────────────────────────────────────

void FFabHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("fab_status"), &Status);
	Registry.RegisterHandler(TEXT("fab_login"), &Login);
	Registry.RegisterHandler(TEXT("fab_logout"), &Logout);
	Registry.RegisterHandler(TEXT("fab_sync_library"), &SyncLibrary);
	Registry.RegisterHandler(TEXT("fab_list_cached"), &ListCached);
	Registry.RegisterHandler(TEXT("fab_cache_info"), &CacheInfo);
	Registry.RegisterHandler(TEXT("fab_clear_cache"), &ClearCache);
	Registry.RegisterHandler(TEXT("fab_import_file"), &ImportFile);
}
