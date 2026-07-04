#include "SourceControlHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "SourceControlHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	// Accept either a content path (/Game/Foo, /Game/Foo.Foo) or a filesystem
	// path and resolve to the absolute file the provider tracks.
	FString ResolveSourceControlFile(const FString& In)
	{
		if (In.StartsWith(TEXT("/")))
		{
			const FString PackageName = In.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(In)
				: In;
			if (FPackageName::IsValidLongPackageName(PackageName))
			{
				return USourceControlHelpers::PackageFilename(PackageName);
			}
		}
		return FPaths::ConvertRelativePathToFull(In);
	}

	// Collect 'path' / 'paths' / 'assetPath' into resolved absolute filenames.
	TArray<FString> CollectFiles(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Raw;
		FString Single;
		if (Params->TryGetStringField(TEXT("path"), Single) && !Single.IsEmpty()) Raw.Add(Single);
		Single.Reset();
		if (Params->TryGetStringField(TEXT("assetPath"), Single) && !Single.IsEmpty()) Raw.Add(Single);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("paths"), Arr))
		{
			Raw.Append(JsonArrayToStringList(Arr));
		}

		TArray<FString> Files;
		for (const FString& Item : Raw)
		{
			const FString Resolved = ResolveSourceControlFile(Item);
			if (!Resolved.IsEmpty()) Files.AddUnique(Resolved);
		}
		return Files;
	}

	// Shared preamble: enabled provider or a structured error.
	TSharedPtr<FJsonValue> RequireProvider(FString& OutProviderName)
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			return MCPError(TEXT("No revision control provider is active. Enable one in the editor (Revision Control settings) or via ISourceControlModule."));
		}
		ISourceControlProvider& Provider = Module.GetProvider();
		if (!Provider.IsAvailable())
		{
			return MCPError(FString::Printf(
				TEXT("Revision control provider '%s' is enabled but not available (not connected?)"),
				*Provider.GetName().ToString()));
		}
		OutProviderName = Provider.GetName().ToString();
		return nullptr;
	}

	TSharedPtr<FJsonObject> StateToJson(const FSourceControlState& State)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("file"), State.Filename);
		Obj->SetBoolField(TEXT("isSourceControlled"), State.bIsSourceControlled);
		Obj->SetBoolField(TEXT("isCheckedOut"), State.bIsCheckedOut);
		Obj->SetBoolField(TEXT("isAdded"), State.bIsAdded);
		Obj->SetBoolField(TEXT("isModified"), State.bIsModified);
		Obj->SetBoolField(TEXT("isCurrent"), State.bIsCurrent);
		Obj->SetBoolField(TEXT("canCheckOut"), State.bCanCheckOut);
		Obj->SetBoolField(TEXT("canAdd"), State.bCanAdd);
		Obj->SetBoolField(TEXT("canRevert"), State.bCanRevert);
		Obj->SetBoolField(TEXT("isConflicted"), State.bIsConflicted);
		if (State.bIsCheckedOutOther)
		{
			Obj->SetBoolField(TEXT("isCheckedOutOther"), true);
			if (!State.CheckedOutOther.IsEmpty())
			{
				Obj->SetStringField(TEXT("checkedOutBy"), State.CheckedOutOther);
			}
		}
		return Obj;
	}

	// checkout/add/revert share the same per-file loop shape.
	TSharedPtr<FJsonValue> RunPerFile(
		const TSharedPtr<FJsonObject>& Params,
		TFunctionRef<bool(const FString&)> Operation,
		const TCHAR* OperationName)
	{
		FString ProviderName;
		if (auto Err = RequireProvider(ProviderName)) return Err;

		const TArray<FString> Files = CollectFiles(Params);
		if (Files.Num() == 0) return MCPError(TEXT("Supply 'path', 'paths', or 'assetPath'"));

		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 Succeeded = 0;
		for (const FString& File : Files)
		{
			const bool bOk = Operation(File);
			if (bOk) Succeeded++;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("file"), File);
			Row->SetBoolField(TEXT("succeeded"), bOk);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		auto Result = MCPSuccess();
		Result->SetBoolField(TEXT("success"), Succeeded == Files.Num());
		Result->SetStringField(TEXT("provider"), ProviderName);
		Result->SetStringField(TEXT("operation"), OperationName);
		Result->SetNumberField(TEXT("succeededCount"), Succeeded);
		Result->SetNumberField(TEXT("fileCount"), Files.Num());
		Result->SetArrayField(TEXT("files"), Rows);
		return MCPResult(Result);
	}
}

void FSourceControlHandlers::RegisterHandlers(FMCPHandlerRegistry& Registry)
{
	Registry.RegisterHandler(TEXT("source_control_status"), &Status);
	Registry.RegisterHandler(TEXT("source_control_checkout"), &CheckOut);
	Registry.RegisterHandler(TEXT("source_control_add"), &MarkForAdd);
	Registry.RegisterHandler(TEXT("source_control_revert"), &Revert);
}

TSharedPtr<FJsonValue> FSourceControlHandlers::Status(const TSharedPtr<FJsonObject>& Params)
{
	// Status without paths is still useful: reports provider + enabled state.
	ISourceControlModule& Module = ISourceControlModule::Get();
	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("enabled"), Module.IsEnabled());
	if (!Module.IsEnabled())
	{
		Result->SetStringField(TEXT("provider"), TEXT("None"));
		return MCPResult(Result);
	}
	ISourceControlProvider& Provider = Module.GetProvider();
	Result->SetStringField(TEXT("provider"), Provider.GetName().ToString());
	Result->SetBoolField(TEXT("available"), Provider.IsAvailable());

	const TArray<FString> Files = CollectFiles(Params);
	if (Files.Num() > 0)
	{
		if (!Provider.IsAvailable())
		{
			return MCPError(FString::Printf(
				TEXT("Revision control provider '%s' is enabled but not available (not connected?)"),
				*Provider.GetName().ToString()));
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<FSourceControlState> States = USourceControlHelpers::QueryFileStates(Files, /*bSilent*/ true);
		for (const FSourceControlState& State : States)
		{
			Rows.Add(MakeShared<FJsonValueObject>(StateToJson(State)));
		}
		Result->SetArrayField(TEXT("states"), Rows);
	}
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FSourceControlHandlers::CheckOut(const TSharedPtr<FJsonObject>& Params)
{
	return RunPerFile(Params, [](const FString& File)
	{
		return USourceControlHelpers::CheckOutFile(File, /*bSilent*/ true);
	}, TEXT("checkout"));
}

TSharedPtr<FJsonValue> FSourceControlHandlers::MarkForAdd(const TSharedPtr<FJsonObject>& Params)
{
	return RunPerFile(Params, [](const FString& File)
	{
		return USourceControlHelpers::MarkFileForAdd(File, /*bSilent*/ true);
	}, TEXT("add"));
}

TSharedPtr<FJsonValue> FSourceControlHandlers::Revert(const TSharedPtr<FJsonObject>& Params)
{
	return RunPerFile(Params, [](const FString& File)
	{
		return USourceControlHelpers::RevertFile(File, /*bSilent*/ true);
	}, TEXT("revert"));
}
