// Control Rig authoring - split from AnimationHandlers.cpp.
// All functions below are still members of FAnimationHandlers; this file is a
// translation-unit partition. Registration stays in AnimationHandlers.cpp.
//
// Uses the same programmatic path as the Control Rig editor itself:
// URigHierarchyController for bones/controls and URigVMController for the
// RigVM node graph, then RecompileVM. Asset creation goes through the
// ControlRigEditor factory's BlueprintCallable statics via reflection so the
// bridge does not link against the editor-only module.

#include "AnimationHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "ControlRigBlueprint.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "EditorAssetLibrary.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	UControlRigBlueprint* LoadControlRig(const TSharedPtr<FJsonObject>& Params, FString& OutPath, FString& OutError)
	{
		if (!Params->TryGetStringField(TEXT("assetPath"), OutPath) && !Params->TryGetStringField(TEXT("path"), OutPath))
		{
			OutError = TEXT("Missing 'assetPath'");
			return nullptr;
		}
		UControlRigBlueprint* Rig = LoadObject<UControlRigBlueprint>(nullptr, *OutPath);
		if (!Rig)
		{
			OutError = FString::Printf(TEXT("ControlRigBlueprint not found: %s"), *OutPath);
		}
		return Rig;
	}

	void FinalizeRigEdit(UControlRigBlueprint* Rig)
	{
		Rig->RecompileVM();
		Rig->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(Rig->GetPathName());
	}

	/** Resolve an existing hierarchy element (bone or control) by name for
	 *  parenting; empty name = top level. */
	FRigElementKey ResolveRigParent(UControlRigBlueprint* Rig, const FString& ParentName)
	{
		if (ParentName.IsEmpty() || !Rig->Hierarchy)
		{
			return FRigElementKey();
		}
		const FName Name(*ParentName);
		for (ERigElementType Type : { ERigElementType::Bone, ERigElementType::Control, ERigElementType::Null })
		{
			const FRigElementKey Key(Name, Type);
			if (Rig->Hierarchy->GetIndex(Key) != INDEX_NONE)
			{
				return Key;
			}
		}
		return FRigElementKey();
	}

	UScriptStruct* ResolveRigUnitStruct(const FString& Token, FString& OutError)
	{
		UScriptStruct* Found = nullptr;
		if (Token.Contains(TEXT("/")))
		{
			Found = FindObject<UScriptStruct>(nullptr, *Token);
			if (!Found)
			{
				Found = LoadObject<UScriptStruct>(nullptr, *Token);
			}
		}
		else
		{
			const FString Prefixed = TEXT("RigUnit_") + Token;
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (It->GetName() == Token || It->GetName() == Prefixed)
				{
					Found = *It;
					break;
				}
			}
		}
		if (!Found)
		{
			OutError = FString::Printf(TEXT("Rig unit struct '%s' not found. Use a short name (TwoBoneIKSimplePerItem -> RigUnit_TwoBoneIKSimplePerItem) or a /Script/ControlRig.RigUnit_* path."), *Token);
		}
		return Found;
	}
}

// animation(create_control_rig): create a Control Rig blueprint, optionally
// seeded from a skeletal mesh / skeleton (imports its bone hierarchy).
TSharedPtr<FJsonValue> FAnimationHandlers::CreateControlRig(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (auto Err = RequireString(Params, TEXT("name"), Name)) return Err;
	const FString PackagePath = OptionalString(Params, TEXT("packagePath"), TEXT("/Game/Animation"));
	const FString SkeletalMeshPath = OptionalString(Params, TEXT("skeletalMeshPath"), TEXT(""));
	const FString DesiredPackagePath = PackagePath / Name;

	// Idempotency probe.
	if (auto Existing = MCPCheckAssetExists(PackagePath, Name, OptionalString(Params, TEXT("onConflict"), TEXT("skip")), TEXT("ControlRigBlueprint")))
	{
		return Existing;
	}

	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/ControlRigEditor.ControlRigBlueprintFactory"));
	if (!FactoryClass)
	{
		return MCPError(TEXT("ControlRigEditor module unavailable (Control Rig plugin not enabled?)"));
	}
	UObject* FactoryCDO = FactoryClass->GetDefaultObject();

	UObject* NewRig = nullptr;
	if (!SkeletalMeshPath.IsEmpty())
	{
		UObject* MeshOrSkeleton = LoadObject<UObject>(nullptr, *SkeletalMeshPath);
		if (!MeshOrSkeleton)
		{
			return MCPError(FString::Printf(TEXT("SkeletalMesh/Skeleton not found: %s"), *SkeletalMeshPath));
		}
		UFunction* Func = FactoryClass->FindFunctionByName(TEXT("CreateControlRigFromSkeletalMeshOrSkeleton"));
		if (!Func) return MCPError(TEXT("CreateControlRigFromSkeletalMeshOrSkeleton not found"));
		FStructOnScope FuncParams(Func);
		uint8* Mem = FuncParams.GetStructMemory();
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It))
			{
				ObjProp->SetObjectPropertyValue_InContainer(Mem, MeshOrSkeleton);
			}
		}
		FactoryCDO->ProcessEvent(Func, Mem);
		if (FObjectPropertyBase* ReturnProp = CastField<FObjectPropertyBase>(Func->GetReturnProperty()))
		{
			NewRig = ReturnProp->GetObjectPropertyValue_InContainer(Mem);
		}
		// The factory names the asset itself (CR_<mesh>); the caller gets the
		// real path back in the result.
	}
	else
	{
		UFunction* Func = FactoryClass->FindFunctionByName(TEXT("CreateNewControlRigAsset"));
		if (!Func) return MCPError(TEXT("CreateNewControlRigAsset not found"));
		FStructOnScope FuncParams(Func);
		uint8* Mem = FuncParams.GetStructMemory();
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
			if (FStrProperty* StrProp = CastField<FStrProperty>(*It))
			{
				StrProp->SetPropertyValue_InContainer(Mem, DesiredPackagePath);
			}
		}
		FactoryCDO->ProcessEvent(Func, Mem);
		if (FObjectPropertyBase* ReturnProp = CastField<FObjectPropertyBase>(Func->GetReturnProperty()))
		{
			NewRig = ReturnProp->GetObjectPropertyValue_InContainer(Mem);
		}
	}

	if (!NewRig)
	{
		return MCPError(TEXT("Control rig factory returned null"));
	}
	UEditorAssetLibrary::SaveAsset(NewRig->GetPathName());

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), NewRig->GetPathName());
	MCPSetDeleteAssetRollback(Result, NewRig->GetPathName());
	return MCPResult(Result);
}

// animation(add_rig_bone): append a bone to the rig hierarchy.
TSharedPtr<FJsonValue> FAnimationHandlers::AddRigBone(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);
	FString BoneName;
	if (auto Err = RequireString(Params, TEXT("boneName"), BoneName)) return Err;

	URigHierarchyController* Controller = Rig->GetHierarchyController();
	if (!Controller) return MCPError(TEXT("Rig has no hierarchy controller"));

	FTransform Transform = FTransform::Identity;
	const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Params->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3)
	{
		Transform.SetLocation(FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber()));
	}
	else if (Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj->IsValid())
	{
		Transform.SetLocation(FVector(
			(*LocObj)->GetNumberField(TEXT("x")),
			(*LocObj)->GetNumberField(TEXT("y")),
			(*LocObj)->GetNumberField(TEXT("z"))));
	}

	const FRigElementKey Parent = ResolveRigParent(Rig, OptionalString(Params, TEXT("parentName"), TEXT("")));
	const FRigElementKey NewKey = Controller->AddBone(FName(*BoneName), Parent, Transform, /*bTransformInGlobal*/ true, ERigBoneType::User);
	if (!NewKey.IsValid())
	{
		return MCPError(FString::Printf(TEXT("AddBone '%s' failed (name taken?)"), *BoneName));
	}

	FinalizeRigEdit(Rig);
	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("bone"), NewKey.Name.ToString());
	return MCPResult(Result);
}

// animation(add_rig_control): append an animation control. controlType:
// transform (default) | float | bool | vector | rotator.
TSharedPtr<FJsonValue> FAnimationHandlers::AddRigControl(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);
	FString ControlName;
	if (auto Err = RequireString(Params, TEXT("controlName"), ControlName)) return Err;

	URigHierarchyController* Controller = Rig->GetHierarchyController();
	if (!Controller) return MCPError(TEXT("Rig has no hierarchy controller"));

	const FString ControlType = OptionalString(Params, TEXT("controlType"), TEXT("transform")).ToLower();
	FRigControlSettings Settings;
	FRigControlValue Value;
	if (ControlType == TEXT("float"))
	{
		Settings.ControlType = ERigControlType::Float;
		Value.Set<float>(0.0f);
	}
	else if (ControlType == TEXT("bool"))
	{
		Settings.ControlType = ERigControlType::Bool;
		Value.Set<bool>(false);
	}
	else if (ControlType == TEXT("vector") || ControlType == TEXT("position"))
	{
		Settings.ControlType = ERigControlType::Position;
		Value.SetFromTransform(FTransform::Identity, Settings.ControlType, Settings.PrimaryAxis);
	}
	else if (ControlType == TEXT("rotator"))
	{
		Settings.ControlType = ERigControlType::Rotator;
		Value.SetFromTransform(FTransform::Identity, Settings.ControlType, Settings.PrimaryAxis);
	}
	else
	{
		Settings.ControlType = ERigControlType::EulerTransform;
		Value.SetFromTransform(FTransform::Identity, Settings.ControlType, Settings.PrimaryAxis);
	}
	Settings.DisplayName = FName(*ControlName);

	const FRigElementKey Parent = ResolveRigParent(Rig, OptionalString(Params, TEXT("parentName"), TEXT("")));
	const FRigElementKey NewKey = Controller->AddControl_ForBlueprint(FName(*ControlName), Parent, Settings, Value);
	if (!NewKey.IsValid())
	{
		return MCPError(FString::Printf(TEXT("AddControl '%s' failed (name taken?)"), *ControlName));
	}

	FinalizeRigEdit(Rig);
	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("control"), NewKey.Name.ToString());
	Result->SetStringField(TEXT("controlType"), ControlType);
	return MCPResult(Result);
}

// animation(add_rig_node): place a rig unit node (RigUnit_*) on the VM graph.
TSharedPtr<FJsonValue> FAnimationHandlers::AddRigNode(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);
	FString UnitStruct;
	if (auto Err = RequireString(Params, TEXT("unitStruct"), UnitStruct)) return Err;

	UScriptStruct* Struct = ResolveRigUnitStruct(UnitStruct, Error);
	if (!Struct) return MCPError(Error);

	URigVMController* Controller = Rig->GetOrCreateController(Rig->GetDefaultModel());
	if (!Controller) return MCPError(TEXT("Rig has no VM controller"));

	const double X = OptionalNumber(Params, TEXT("x"), 0.0);
	const double Y = OptionalNumber(Params, TEXT("y"), 0.0);
	const FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));

	URigVMUnitNode* Node = Controller->AddUnitNode(Struct, TEXT("Execute"), FVector2D(X, Y), NodeName);
	if (!Node)
	{
		return MCPError(FString::Printf(TEXT("AddUnitNode(%s) failed - see Output Log"), *Struct->GetName()));
	}

	FinalizeRigEdit(Rig);
	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("nodeName"), Node->GetName());
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (URigVMPin* Pin : Node->GetPins())
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->GetName());
		PinObj->SetStringField(TEXT("direction"),
			Pin->GetDirection() == ERigVMPinDirection::Input ? TEXT("in") :
			Pin->GetDirection() == ERigVMPinDirection::Output ? TEXT("out") : TEXT("io"));
		PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
		Pins.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Result->SetArrayField(TEXT("pins"), Pins);
	return MCPResult(Result);
}

// animation(connect_rig_pins): link "NodeA.Pin" -> "NodeB.Pin".
TSharedPtr<FJsonValue> FAnimationHandlers::ConnectRigPins(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);
	FString FromPin, ToPin;
	if (auto Err = RequireString(Params, TEXT("fromPin"), FromPin)) return Err;
	if (auto Err = RequireString(Params, TEXT("toPin"), ToPin)) return Err;

	URigVMController* Controller = Rig->GetOrCreateController(Rig->GetDefaultModel());
	if (!Controller) return MCPError(TEXT("Rig has no VM controller"));

	if (!Controller->AddLink(FromPin, ToPin))
	{
		return MCPError(FString::Printf(TEXT("AddLink %s -> %s failed. Pin paths are 'NodeName.PinName' (nested: 'Node.Pin.SubPin'); read_rig_graph lists them."), *FromPin, *ToPin));
	}

	FinalizeRigEdit(Rig);
	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("fromPin"), FromPin);
	Result->SetStringField(TEXT("toPin"), ToPin);
	return MCPResult(Result);
}

// animation(set_rig_pin_default): set a pin's default value from a string
// (numbers, names, or full struct text like (X=0,Y=0,Z=10)).
TSharedPtr<FJsonValue> FAnimationHandlers::SetRigPinDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);
	FString PinPath, Value;
	if (auto Err = RequireString(Params, TEXT("pinPath"), PinPath)) return Err;
	if (auto Err = RequireString(Params, TEXT("value"), Value)) return Err;

	URigVMController* Controller = Rig->GetOrCreateController(Rig->GetDefaultModel());
	if (!Controller) return MCPError(TEXT("Rig has no VM controller"));

	if (!Controller->SetPinDefaultValue(PinPath, Value))
	{
		return MCPError(FString::Printf(TEXT("SetPinDefaultValue(%s) failed"), *PinPath));
	}

	FinalizeRigEdit(Rig);
	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("assetPath"), Path);
	Result->SetStringField(TEXT("pinPath"), PinPath);
	return MCPResult(Result);
}

// animation(read_rig_graph): nodes with pins + links + hierarchy elements.
TSharedPtr<FJsonValue> FAnimationHandlers::ReadRigGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	UControlRigBlueprint* Rig = LoadControlRig(Params, Path, Error);
	if (!Rig) return MCPError(Error);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), Path);

	if (URigVMGraph* Model = Rig->GetDefaultModel())
	{
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (URigVMNode* Node : Model->GetNodes())
		{
			if (!Node) continue;
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), Node->GetName());
			NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle());
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (URigVMPin* Pin : Node->GetPins())
			{
				if (!Pin) continue;
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Pin->GetName());
				PinObj->SetStringField(TEXT("direction"),
					Pin->GetDirection() == ERigVMPinDirection::Input ? TEXT("in") :
					Pin->GetDirection() == ERigVMPinDirection::Output ? TEXT("out") : TEXT("io"));
				PinObj->SetStringField(TEXT("type"), Pin->GetCPPType());
				const FString Default = Pin->GetDefaultValue();
				if (!Default.IsEmpty() && Default.Len() < 200)
				{
					PinObj->SetStringField(TEXT("default"), Default);
				}
				Pins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("pins"), Pins);
			Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		Result->SetArrayField(TEXT("nodes"), Nodes);

		TArray<TSharedPtr<FJsonValue>> Links;
		for (URigVMLink* Link : Model->GetLinks())
		{
			if (!Link || !Link->GetSourcePin() || !Link->GetTargetPin()) continue;
			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			LinkObj->SetStringField(TEXT("from"), Link->GetSourcePin()->GetPinPath());
			LinkObj->SetStringField(TEXT("to"), Link->GetTargetPin()->GetPinPath());
			Links.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		Result->SetArrayField(TEXT("links"), Links);
	}

	if (Rig->Hierarchy)
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		Rig->Hierarchy->ForEach([&Elements](const FRigBaseElement* Element) -> bool
		{
			if (Element && Elements.Num() < 500)
			{
				TSharedPtr<FJsonObject> ElemObj = MakeShared<FJsonObject>();
				ElemObj->SetStringField(TEXT("name"), Element->GetKey().Name.ToString());
				ElemObj->SetStringField(TEXT("type"), StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Element->GetKey().Type)));
				Elements.Add(MakeShared<FJsonValueObject>(ElemObj));
			}
			return true;
		});
		Result->SetArrayField(TEXT("hierarchy"), Elements);
	}

	return MCPResult(Result);
}
