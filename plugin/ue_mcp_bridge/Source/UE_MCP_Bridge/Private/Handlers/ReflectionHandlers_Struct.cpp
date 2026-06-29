// User Defined Struct authoring. Sibling of create_enum/set_enum_entries in
// ReflectionHandlers.cpp — split into its own translation unit because it pulls
// in the BlueprintGraph pin-type resolver and the struct editor utilities.
//
// create_struct  — make a UUserDefinedStruct seeded with {name,type} fields.
// set_struct_fields — replace the field list on an existing struct.
//
// Field type strings are resolved through FBlueprintHandlers::MakePinType, the
// same resolver add_variable uses, so "float", "FVector", "Actor", "E_MyEnum",
// "/Game/Path/S_Other", soft-refs etc. all work identically here.

#include "ReflectionHandlers.h"
#include "BlueprintHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerAssetCreate.h"
#include "Engine/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
// Full definition of FStructVariableDescription (GetVarDesc element type).
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Factories/Factory.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// One {name, type, isArray?} field spec parsed from the wire.
	struct FStructFieldSpec
	{
		FString Name;
		FString Type;
		bool bIsArray = false;
	};

	// Parse the variables[] / fields[] array. Each entry must be an object with
	// "name" and "type"; an optional "isArray" wraps the pin in a TArray.
	TArray<FStructFieldSpec> ParseFieldSpecs(const TArray<TSharedPtr<FJsonValue>>* Arr)
	{
		TArray<FStructFieldSpec> Out;
		if (!Arr) return Out;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			if (!Obj.IsValid()) continue;
			FStructFieldSpec Spec;
			Obj->TryGetStringField(TEXT("name"), Spec.Name);
			Obj->TryGetStringField(TEXT("type"), Spec.Type);
			Obj->TryGetBoolField(TEXT("isArray"), Spec.bIsArray);
			if (!Spec.Name.IsEmpty() && !Spec.Type.IsEmpty())
			{
				Out.Add(MoveTemp(Spec));
			}
		}
		return Out;
	}

	// Append the requested fields to Struct. Returns count added; pushes any
	// unresolved type strings onto OutUnresolved so the caller can report them.
	int32 AddFieldsToStruct(
		UUserDefinedStruct* Struct,
		const TArray<FStructFieldSpec>& Fields,
		TArray<FString>& OutUnresolved)
	{
		int32 Added = 0;
		for (const FStructFieldSpec& Field : Fields)
		{
			FEdGraphPinType PinType = FBlueprintHandlers::MakePinType(Field.Type);
			if (PinType.PinCategory.IsNone())
			{
				OutUnresolved.Add(FString::Printf(TEXT("%s (%s)"), *Field.Name, *Field.Type));
				continue;
			}
			PinType.ContainerType = Field.bIsArray ? EPinContainerType::Array : EPinContainerType::None;

			if (!FStructureEditorUtils::AddVariable(Struct, PinType))
			{
				OutUnresolved.Add(FString::Printf(TEXT("%s (add failed)"), *Field.Name));
				continue;
			}
			// The freshly added variable is the last description entry; rename it
			// to the caller's friendly name.
			auto& Descs = FStructureEditorUtils::GetVarDesc(Struct);
			if (Descs.Num() > 0)
			{
				FStructureEditorUtils::RenameVariable(Struct, Descs.Last().VarGuid, Field.Name);
			}
			Added++;
		}
		return Added;
	}
}

// ─── create_struct ─────────────────────────────────────────────────────
TSharedPtr<FJsonValue> FReflectionHandlers::CreateStruct(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	const FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game"));
	const FString OnConflict = OptionalString(Params, TEXT("onConflict"), TEXT("skip"));

	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.StructureFactory"));
	if (!FactoryClass)
	{
		return MCPError(TEXT("StructureFactory not found in /Script/UnrealEd"));
	}
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);

	auto Created = MCPCreateAssetIdempotent<UUserDefinedStruct>(Name, PackagePath, OnConflict, TEXT("UserDefinedStruct"), Factory);
	if (Created.EarlyReturn) return Created.EarlyReturn;
	UUserDefinedStruct* Struct = Created.Asset;

	// A freshly created struct ships with one auto member ("MemberVar_0"). Snapshot
	// those GUIDs so we can drop them once the requested fields are in place.
	TArray<FGuid> DefaultGuids;
	for (const auto& Desc : FStructureEditorUtils::GetVarDesc(Struct))
	{
		DefaultGuids.Add(Desc.VarGuid);
	}

	const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("variables"), FieldsArr))
	{
		Params->TryGetArrayField(TEXT("fields"), FieldsArr);
	}
	const TArray<FStructFieldSpec> Fields = ParseFieldSpecs(FieldsArr);

	TArray<FString> Unresolved;
	const int32 Added = AddFieldsToStruct(Struct, Fields, Unresolved);

	// Only remove the auto member once we have at least one real field — a struct
	// must always keep one member.
	if (Added > 0)
	{
		for (const FGuid& Guid : DefaultGuids)
		{
			FStructureEditorUtils::RemoveVariable(Struct, Guid);
		}
	}

	Struct->MarkPackageDirty();
	UEditorAssetLibrary::SaveAsset(Struct->GetPathName());

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Struct->GetPathName());
	Result->SetStringField(TEXT("name"), Name);
	Result->SetNumberField(TEXT("fieldsAdded"), Added);
	if (Unresolved.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> U;
		for (const FString& S : Unresolved) U.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("unresolved"), U);
	}
	MCPSetDeleteAssetRollback(Result, Struct->GetPathName());
	return MCPResult(Result);
}

// ─── set_struct_fields ─────────────────────────────────────────────────
// Replace the entire field list on an existing UUserDefinedStruct.
TSharedPtr<FJsonValue> FReflectionHandlers::SetStructFields(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireString(Params, TEXT("assetPath"), AssetPath)) return Err;

	UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(LoadObject<UObject>(nullptr, *AssetPath));
	if (!Struct)
	{
		// Tolerate "/Game/Foo/S_Bar" without the ".S_Bar" object suffix.
		Struct = Cast<UUserDefinedStruct>(LoadAssetByPath<UObject>(AssetPath));
	}
	if (!Struct) return MCPError(FString::Printf(TEXT("UserDefinedStruct not found: %s"), *AssetPath));

	const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("variables"), FieldsArr))
	{
		Params->TryGetArrayField(TEXT("fields"), FieldsArr);
	}
	if (!FieldsArr)
	{
		return MCPError(TEXT("Missing 'variables' (array of {name, type, isArray?})"));
	}
	const TArray<FStructFieldSpec> Fields = ParseFieldSpecs(FieldsArr);
	if (Fields.Num() == 0)
	{
		return MCPError(TEXT("'variables' resolved to zero valid fields; a struct must keep at least one member"));
	}

	// Snapshot existing fields, add the new set, then drop the old ones — this
	// keeps the struct valid (>=1 member) throughout.
	TArray<FGuid> OldGuids;
	for (const auto& Desc : FStructureEditorUtils::GetVarDesc(Struct))
	{
		OldGuids.Add(Desc.VarGuid);
	}

	TArray<FString> Unresolved;
	const int32 Added = AddFieldsToStruct(Struct, Fields, Unresolved);
	if (Added > 0)
	{
		for (const FGuid& Guid : OldGuids)
		{
			FStructureEditorUtils::RemoveVariable(Struct, Guid);
		}
	}

	Struct->MarkPackageDirty();
	UEditorAssetLibrary::SaveAsset(Struct->GetPathName());

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("assetPath"), Struct->GetPathName());
	Result->SetNumberField(TEXT("fields"), Added);
	if (Unresolved.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> U;
		for (const FString& S : Unresolved) U.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("unresolved"), U);
	}
	return MCPResult(Result);
}
