// Token-efficient, loss-aware Blueprint graph export for native-code analysis. This is deliberately read-only: T3D remains the exact
// round-trip format, while this representation is optimized for analysis and
// Blueprint-to-C++ migration planning.

#include "BlueprintHandlers.h"
#include "HandlerUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	struct FCompactGraphWork
	{
		UEdGraph* Graph = nullptr;
		int32 Depth = 0;
		FString ReferencedByGraphId;
		FString ReferencedByNodeId;
		FString ReferenceKind;
	};

	struct FDerivedBlueprintCacheEntry
	{
		int32 TotalDescendantCount = 0;
		bool bTruncated = false;
		TArray<UBlueprint*> LoadedBlueprints;
	};

	FString GuidString(const FGuid& Guid)
	{
		return Guid.IsValid() ? Guid.ToString(EGuidFormats::Digits) : FString();
	}

	FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	FString BlueprintAssetPath(const UBlueprint* Blueprint)
	{
		return Blueprint && Blueprint->GetOutermost()
			? Blueprint->GetOutermost()->GetName()
			: FString();
	}

	UBlueprint* GetOwningBlueprint(const UEdGraph* Graph)
	{
		return Graph ? Graph->GetTypedOuter<UBlueprint>() : nullptr;
	}

	UEdGraph* FindLocalGraph(UBlueprint* Blueprint, const FName GraphName)
	{
		if (!Blueprint) return nullptr;
		auto FindIn = [GraphName](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph && Graph->GetFName() == GraphName) return Graph;
			}
			return nullptr;
		};
		if (UEdGraph* Graph = FindIn(Blueprint->UbergraphPages)) return Graph;
		if (UEdGraph* Graph = FindIn(Blueprint->FunctionGraphs)) return Graph;
		if (UEdGraph* Graph = FindIn(Blueprint->MacroGraphs)) return Graph;
		if (UEdGraph* Graph = FindIn(Blueprint->DelegateSignatureGraphs)) return Graph;
		return nullptr;
	}

	FString GraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (!Blueprint || !Graph) return TEXT("Unknown");
		if (Blueprint->UbergraphPages.Contains(Graph)) return TEXT("EventGraph");
		if (Blueprint->FunctionGraphs.Contains(Graph))
		{
			return Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript
				? TEXT("Construction") : TEXT("Function");
		}
		if (Blueprint->MacroGraphs.Contains(Graph)) return TEXT("Macro");
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) return TEXT("DelegateSignature");
		return TEXT("Graph");
	}

	FString GraphId(const UEdGraph* Graph)
	{
		if (!Graph) return FString();
		const FString StableGuid = GuidString(Graph->GraphGuid);
		return StableGuid.IsEmpty() ? Graph->GetPathName() : StableGuid;
	}

	FString ContainerName(const EPinContainerType ContainerType)
	{
		switch (ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set: return TEXT("set");
		case EPinContainerType::Map: return TEXT("map");
		default: return TEXT("none");
		}
	}

	TSharedPtr<FJsonObject> SerializeTerminalType(const FEdGraphTerminalType& Type)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("category"), Type.TerminalCategory.ToString());
		Json->SetStringField(TEXT("subcategory"), Type.TerminalSubCategory.ToString());
		Json->SetStringField(TEXT("subcategoryObject"), ObjectPath(Type.TerminalSubCategoryObject.Get()));
		Json->SetBoolField(TEXT("const"), Type.bTerminalIsConst);
		Json->SetBoolField(TEXT("weakPointer"), Type.bTerminalIsWeakPointer);
		return Json;
	}

	TSharedPtr<FJsonObject> SerializePinType(const FEdGraphPinType& Type)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("category"), Type.PinCategory.ToString());
		Json->SetStringField(TEXT("subcategory"), Type.PinSubCategory.ToString());
		Json->SetStringField(TEXT("subcategoryObject"), ObjectPath(Type.PinSubCategoryObject.Get()));
		Json->SetStringField(TEXT("container"), ContainerName(Type.ContainerType));
		Json->SetBoolField(TEXT("reference"), Type.bIsReference);
		Json->SetBoolField(TEXT("const"), Type.bIsConst);
		Json->SetBoolField(TEXT("weakPointer"), Type.bIsWeakPointer);
		Json->SetBoolField(TEXT("uobjectWrapper"), Type.bIsUObjectWrapper);
		if (Type.ContainerType == EPinContainerType::Map)
		{
			Json->SetObjectField(TEXT("mapValueType"), SerializeTerminalType(Type.PinValueType));
		}
		return Json;
	}

	TSharedPtr<FJsonObject> SerializeVariable(const FBPVariableDescription& Variable)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Variable.VarName.ToString());
		Json->SetStringField(TEXT("guid"), GuidString(Variable.VarGuid));
		Json->SetObjectField(TEXT("type"), SerializePinType(Variable.VarType));
		Json->SetStringField(TEXT("defaultValue"), Variable.DefaultValue);
		Json->SetStringField(TEXT("category"), Variable.Category.ToString());
		Json->SetStringField(TEXT("propertyFlags"), FString::Printf(TEXT("%llu"), static_cast<uint64>(Variable.PropertyFlags)));
		Json->SetStringField(TEXT("repNotifyFunction"), Variable.RepNotifyFunc.ToString());
		Json->SetStringField(TEXT("friendlyName"), Variable.FriendlyName);
		return Json;
	}

	void AddNamedFunctionFlag(TArray<TSharedPtr<FJsonValue>>& Out, const UFunction* Function, EFunctionFlags Flag, const TCHAR* Name)
	{
		if (Function && Function->HasAnyFunctionFlags(Flag))
		{
			Out.Add(MakeShared<FJsonValueString>(Name));
		}
	}

	TArray<TSharedPtr<FJsonValue>> FunctionFlagNames(const UFunction* Function)
	{
		TArray<TSharedPtr<FJsonValue>> Flags;
		AddNamedFunctionFlag(Flags, Function, FUNC_BlueprintCallable, TEXT("BlueprintCallable"));
		AddNamedFunctionFlag(Flags, Function, FUNC_BlueprintPure, TEXT("BlueprintPure"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Event, TEXT("Event"));
		AddNamedFunctionFlag(Flags, Function, FUNC_BlueprintEvent, TEXT("BlueprintEvent"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Native, TEXT("Native"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Net, TEXT("Net"));
		AddNamedFunctionFlag(Flags, Function, FUNC_NetServer, TEXT("NetServer"));
		AddNamedFunctionFlag(Flags, Function, FUNC_NetClient, TEXT("NetClient"));
		AddNamedFunctionFlag(Flags, Function, FUNC_NetMulticast, TEXT("NetMulticast"));
		AddNamedFunctionFlag(Flags, Function, FUNC_NetReliable, TEXT("NetReliable"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Static, TEXT("Static"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Const, TEXT("Const"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Public, TEXT("Public"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Protected, TEXT("Protected"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Private, TEXT("Private"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Exec, TEXT("Exec"));
		AddNamedFunctionFlag(Flags, Function, FUNC_Final, TEXT("Final"));
		return Flags;
	}

	TSharedPtr<FJsonObject> SerializeFunctionParameter(const FProperty* Property)
	{
		auto Json = MakeShared<FJsonObject>();
		FString ExtendedType;
		const FString CppType = Property->GetCPPType(&ExtendedType, CPPF_None);
		Json->SetStringField(TEXT("name"), Property->GetName());
		Json->SetStringField(TEXT("propertyClass"), Property->GetClass()->GetName());
		Json->SetStringField(TEXT("cppType"), CppType + ExtendedType);
		Json->SetStringField(TEXT("direction"), Property->HasAnyPropertyFlags(CPF_ReturnParm)
			? TEXT("return") : Property->HasAnyPropertyFlags(CPF_OutParm) ? TEXT("output") : TEXT("input"));
		Json->SetBoolField(TEXT("reference"), Property->HasAnyPropertyFlags(CPF_ReferenceParm));
		Json->SetBoolField(TEXT("const"), Property->HasAnyPropertyFlags(CPF_ConstParm));
		Json->SetStringField(TEXT("propertyFlags"), FString::Printf(TEXT("%llu"), static_cast<uint64>(Property->PropertyFlags)));
		return Json;
	}

	TSharedPtr<FJsonObject> SerializeFunctionMetadata(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		auto Json = MakeShared<FJsonObject>();
		const FName FunctionName = Graph->GetFName();
		UFunction* Function = Blueprint && Blueprint->GeneratedClass
			? Blueprint->GeneratedClass->FindFunctionByName(FunctionName) : nullptr;
		UFunction* ParentFunction = Blueprint && Blueprint->ParentClass
			? Blueprint->ParentClass->FindFunctionByName(FunctionName) : nullptr;

		Json->SetStringField(TEXT("name"), FunctionName.ToString());
		Json->SetStringField(TEXT("objectPath"), ObjectPath(Function));
		Json->SetStringField(TEXT("ownerClass"), Function ? ObjectPath(Function->GetOwnerClass()) : FString());
		Json->SetStringField(TEXT("flagsValue"), Function
			? FString::Printf(TEXT("%llu"), static_cast<uint64>(Function->FunctionFlags)) : TEXT("0"));
		Json->SetArrayField(TEXT("flags"), FunctionFlagNames(Function));
		Json->SetBoolField(TEXT("latent"), Function && Function->HasMetaData(TEXT("Latent")));
		Json->SetBoolField(TEXT("override"), ParentFunction != nullptr);
		Json->SetStringField(TEXT("parentFunction"), ObjectPath(ParentFunction));

		FGuid FunctionGuid;
		if (Blueprint && Blueprint->GeneratedClass)
		{
			FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(Blueprint->GeneratedClass, FunctionName, FunctionGuid);
		}
		Json->SetStringField(TEXT("guid"), GuidString(FunctionGuid));

		TArray<TSharedPtr<FJsonValue>> Parameters;
		if (Function)
		{
			for (TFieldIterator<FProperty> It(Function); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm))
				{
					Parameters.Add(MakeShared<FJsonValueObject>(SerializeFunctionParameter(*It)));
				}
			}
		}
		Json->SetArrayField(TEXT("parameters"), Parameters);

		TArray<UK2Node_FunctionEntry*> Entries;
		Graph->GetNodesOfClass(Entries);
		if (Entries.Num() > 0 && Entries[0])
		{
			UK2Node_FunctionEntry* Entry = Entries[0];
			auto Binding = MakeShared<FJsonObject>();
			Binding->SetStringField(TEXT("memberName"), Entry->FunctionReference.GetMemberName().ToString());
			Binding->SetStringField(TEXT("memberParent"), ObjectPath(Entry->FunctionReference.GetMemberParentClass()));
			Binding->SetStringField(TEXT("memberGuid"), GuidString(Entry->FunctionReference.GetMemberGuid()));
			Binding->SetBoolField(TEXT("selfContext"), Entry->FunctionReference.IsSelfContext());
			Binding->SetStringField(TEXT("customGeneratedFunctionName"), Entry->CustomGeneratedFunctionName.ToString());
			Binding->SetNumberField(TEXT("extraFlags"), Entry->GetExtraFlags());
			Binding->SetBoolField(TEXT("editable"), Entry->bIsEditable);
			Json->SetObjectField(TEXT("entryBinding"), Binding);
		}
		return Json;
	}

	TSharedPtr<FJsonObject> SerializeBlueprintMetadata(UBlueprint* Blueprint)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("assetPath"), BlueprintAssetPath(Blueprint));
		Json->SetStringField(TEXT("objectPath"), ObjectPath(Blueprint));
		Json->SetStringField(TEXT("name"), Blueprint ? Blueprint->GetName() : FString());
		Json->SetStringField(TEXT("blueprintType"), Blueprint
			? StaticEnum<EBlueprintType>()->GetNameStringByValue(Blueprint->BlueprintType) : FString());
		Json->SetStringField(TEXT("parentClass"), Blueprint ? ObjectPath(Blueprint->ParentClass) : FString());
		Json->SetStringField(TEXT("generatedClass"), Blueprint ? ObjectPath(Blueprint->GeneratedClass) : FString());
		Json->SetStringField(TEXT("skeletonClass"), Blueprint ? ObjectPath(Blueprint->SkeletonGeneratedClass) : FString());
		Json->SetStringField(TEXT("classDefaultObject"), Blueprint && Blueprint->GeneratedClass
			? ObjectPath(Blueprint->GeneratedClass->GetDefaultObject(false)) : FString());

		TArray<TSharedPtr<FJsonValue>> Variables;
		if (Blueprint)
		{
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				Variables.Add(MakeShared<FJsonValueObject>(SerializeVariable(Variable)));
			}
		}
		Json->SetArrayField(TEXT("variables"), Variables);

		TArray<TSharedPtr<FJsonValue>> Interfaces;
		if (Blueprint)
		{
			for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
			{
				auto InterfaceJson = MakeShared<FJsonObject>();
				InterfaceJson->SetStringField(TEXT("interfaceClass"), ObjectPath(Interface.Interface));
				InterfaceJson->SetNumberField(TEXT("graphCount"), Interface.Graphs.Num());
				Interfaces.Add(MakeShared<FJsonValueObject>(InterfaceJson));
			}
		}
		Json->SetArrayField(TEXT("interfaces"), Interfaces);
		return Json;
	}

	FString SemanticTypeForNode(const UEdGraphNode* Node, bool& bOutSupported, FString& OutReason)
	{
		bOutSupported = true;
		OutReason.Reset();
		if (!Node)
		{
			bOutSupported = false;
			OutReason = TEXT("Null node");
			return TEXT("unsupported");
		}

		static const TMap<FName, FString> ExactTypes = {
			{TEXT("K2Node_FunctionEntry"), TEXT("function_entry")},
			{TEXT("K2Node_FunctionResult"), TEXT("function_result")},
			{TEXT("K2Node_Event"), TEXT("event")},
			{TEXT("K2Node_CustomEvent"), TEXT("custom_event")},
			{TEXT("K2Node_ActorBoundEvent"), TEXT("actor_bound_event")},
			{TEXT("K2Node_ComponentBoundEvent"), TEXT("component_bound_event")},
			{TEXT("K2Node_CallFunction"), TEXT("call_function")},
			{TEXT("K2Node_CallParentFunction"), TEXT("call_parent_function")},
			{TEXT("K2Node_VariableGet"), TEXT("variable_get")},
			{TEXT("K2Node_VariableSet"), TEXT("variable_set")},
			{TEXT("K2Node_IfThenElse"), TEXT("branch")},
			{TEXT("K2Node_ExecutionSequence"), TEXT("sequence")},
			{TEXT("K2Node_Select"), TEXT("select")},
			{TEXT("K2Node_Knot"), TEXT("knot")},
			{TEXT("K2Node_Tunnel"), TEXT("tunnel")},
			{TEXT("K2Node_MacroInstance"), TEXT("macro_instance")},
			{TEXT("K2Node_Composite"), TEXT("composite")},
			{TEXT("K2Node_DynamicCast"), TEXT("dynamic_cast")},
			{TEXT("K2Node_ClassDynamicCast"), TEXT("class_dynamic_cast")},
			{TEXT("K2Node_SwitchInteger"), TEXT("switch_int")},
			{TEXT("K2Node_SwitchString"), TEXT("switch_string")},
			{TEXT("K2Node_SwitchName"), TEXT("switch_name")},
			{TEXT("K2Node_SwitchEnum"), TEXT("switch_enum")},
			{TEXT("K2Node_MakeStruct"), TEXT("make_struct")},
			{TEXT("K2Node_BreakStruct"), TEXT("break_struct")},
			{TEXT("K2Node_SetFieldsInStruct"), TEXT("set_fields_in_struct")},
			{TEXT("K2Node_MakeArray"), TEXT("make_array")},
			{TEXT("K2Node_MakeMap"), TEXT("make_map")},
			{TEXT("K2Node_MakeSet"), TEXT("make_set")},
			{TEXT("K2Node_GetArrayItem"), TEXT("get_array_item")},
			{TEXT("K2Node_AddDelegate"), TEXT("add_delegate")},
			{TEXT("K2Node_RemoveDelegate"), TEXT("remove_delegate")},
			{TEXT("K2Node_ClearDelegate"), TEXT("clear_delegate")},
			{TEXT("K2Node_CreateDelegate"), TEXT("create_delegate")},
			{TEXT("K2Node_AssignDelegate"), TEXT("assign_delegate")},
			{TEXT("K2Node_CallDelegate"), TEXT("call_delegate")},
			{TEXT("K2Node_AsyncAction"), TEXT("async_action")},
			{TEXT("K2Node_AddComponent"), TEXT("add_component")},
			{TEXT("K2Node_AddComponentByClass"), TEXT("add_component_by_class")},
			{TEXT("K2Node_ConstructObjectFromClass"), TEXT("construct_object")},
			{TEXT("K2Node_Timeline"), TEXT("timeline")},
			{TEXT("K2Node_SpawnActorFromClass"), TEXT("spawn_actor")},
			{TEXT("K2Node_FormatText"), TEXT("format_text")},
			{TEXT("K2Node_GetClassDefaults"), TEXT("get_class_defaults")},
			{TEXT("K2Node_GetSubsystem"), TEXT("get_subsystem")},
			{TEXT("K2Node_Literal"), TEXT("literal")},
			{TEXT("K2Node_Self"), TEXT("self")},
			{TEXT("K2Node_Message"), TEXT("interface_message")},
			{TEXT("K2Node_PromotableOperator"), TEXT("promotable_operator")},
			{TEXT("K2Node_CommutativeAssociativeBinaryOperator"), TEXT("commutative_operator")},
			{TEXT("EdGraphNode_Comment"), TEXT("comment")},
		};

		const FName ClassName = Node->GetClass()->GetFName();
		if (const FString* Semantic = ExactTypes.Find(ClassName)) return *Semantic;
		if (Node->IsA<UK2Node_CallFunction>()) return TEXT("call_function");
		if (Node->IsA<UK2Node_Variable>()) return TEXT("variable");
		if (Node->IsA<UK2Node_Event>()) return TEXT("event");

		bOutSupported = false;
		OutReason = FString::Printf(
			TEXT("No compact semantic mapping for %s; raw node properties, pins, defaults, and links are still preserved"),
			*ClassName.ToString());
		return TEXT("unsupported");
	}

	TSharedPtr<FJsonObject> SerializeMemberReference(const FMemberReference& Reference)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("memberName"), Reference.GetMemberName().ToString());
		Json->SetStringField(TEXT("memberParent"), ObjectPath(Reference.GetMemberParentClass()));
		Json->SetStringField(TEXT("memberGuid"), GuidString(Reference.GetMemberGuid()));
		Json->SetBoolField(TEXT("selfContext"), Reference.IsSelfContext());
		return Json;
	}

	bool HasUnsafePinOwnership(const UEdGraphNode* Node)
	{
		if (!Node) return true;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->GetOwningNodeUnchecked() != Node) return true;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNodeUnchecked()) return true;
			}
		}
		return false;
	}

	FString SafeNodeTitle(const UEdGraphNode* Node)
	{
		return Node && !HasUnsafePinOwnership(Node)
			? Node->GetNodeTitle(ENodeTitleType::ListView).ToString()
			: Node ? Node->GetName() : FString();
	}

	TSharedPtr<FJsonObject> SerializePin(const UEdGraphPin* Pin, const UEdGraphNode* ExpectedOwner)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Pin ? GuidString(Pin->PinId) : FString());
		Json->SetStringField(TEXT("persistentGuid"), Pin ? GuidString(Pin->PersistentGuid) : FString());
		Json->SetStringField(TEXT("rawName"), Pin ? Pin->PinName.ToString() : FString());
		Json->SetStringField(TEXT("displayName"), Pin && !Pin->PinFriendlyName.IsEmpty()
			? Pin->PinFriendlyName.ToString() : Pin ? Pin->PinName.ToString() : FString());
		Json->SetStringField(TEXT("direction"), !Pin ? TEXT("Invalid")
			: Pin->Direction == EGPD_Input ? TEXT("Input")
			: Pin->Direction == EGPD_Output ? TEXT("Output") : TEXT("Invalid"));
		if (!Pin) return Json;

		Json->SetObjectField(TEXT("type"), SerializePinType(Pin->PinType));
		auto Defaults = MakeShared<FJsonObject>();
		Defaults->SetStringField(TEXT("value"), Pin->DefaultValue);
		Defaults->SetStringField(TEXT("autogeneratedValue"), Pin->AutogeneratedDefaultValue);
		Defaults->SetStringField(TEXT("objectPath"), ObjectPath(Pin->DefaultObject));
		Defaults->SetStringField(TEXT("text"), Pin->DefaultTextValue.ToString());
		Defaults->SetBoolField(TEXT("ignored"), Pin->bDefaultValueIsIgnored);
		Json->SetObjectField(TEXT("default"), Defaults);

		Json->SetBoolField(TEXT("hidden"), Pin->bHidden);
		Json->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
		Json->SetBoolField(TEXT("notConnectable"), Pin->bNotConnectable);
		Json->SetBoolField(TEXT("advancedView"), Pin->bAdvancedView);
		Json->SetBoolField(TEXT("saveIfOrphaned"), Pin->ShouldSavePinIfOrphaned());
		const UEdGraphNode* ActualOwner = Pin->GetOwningNodeUnchecked();
		Json->SetBoolField(TEXT("ownerValid"), ActualOwner != nullptr);
		Json->SetBoolField(TEXT("ownerMatchesNode"), ActualOwner == ExpectedOwner);
		Json->SetStringField(TEXT("ownerNodeId"), ActualOwner ? GuidString(ActualOwner->NodeGuid) : FString());

		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			auto Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("pinId"), LinkedPin ? GuidString(LinkedPin->PinId) : FString());
			Link->SetStringField(TEXT("rawPinName"), LinkedPin ? LinkedPin->PinName.ToString() : FString());
			const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNodeUnchecked() : nullptr;
			Link->SetStringField(TEXT("nodeId"), LinkedNode ? GuidString(LinkedNode->NodeGuid) : FString());
			Link->SetBoolField(TEXT("dangling"), !LinkedPin || !LinkedNode);
			Links.Add(MakeShared<FJsonValueObject>(Link));
		}
		Json->SetArrayField(TEXT("links"), Links);
		return Json;
	}

	UEdGraph* ReferencedGraph(const UEdGraphNode* Node, FString& OutKind)
	{
		OutKind.Reset();
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			OutKind = TEXT("macro");
			return Macro->GetMacroGraph();
		}
		if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			OutKind = TEXT("composite");
			return Composite->BoundGraph;
		}
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			if (UFunction* Function = Call->GetTargetFunction())
			{
				if (const UBlueprintGeneratedClass* OwnerClass = Cast<UBlueprintGeneratedClass>(Function->GetOwnerClass()))
				{
					if (UBlueprint* OwnerBlueprint = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy))
					{
						if (UEdGraph* FunctionGraph = FindLocalGraph(OwnerBlueprint, Function->GetFName()))
						{
							OutKind = TEXT("blueprint_function");
							return FunctionGraph;
						}
					}
				}
			}
		}
		return nullptr;
	}

	FDerivedBlueprintCacheEntry& GetDerivedBlueprints(
		UBlueprint* Blueprint,
		const int32 LoadLimit,
		TMap<UBlueprint*, FDerivedBlueprintCacheEntry>& Cache)
	{
		FDerivedBlueprintCacheEntry* Existing = Cache.Find(Blueprint);
		if (Existing) return *Existing;

		FDerivedBlueprintCacheEntry& Entry = Cache.Add(Blueprint);
		if (!Blueprint || !Blueprint->GeneratedClass) return Entry;

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FTopLevelAssetPath> RootClasses;
		RootClasses.Add(FTopLevelAssetPath(Blueprint->GeneratedClass));
		TSet<FTopLevelAssetPath> DerivedClassPaths;
		Registry.GetDerivedClassNames(RootClasses, TSet<FTopLevelAssetPath>(), DerivedClassPaths);

		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> BlueprintAssets;
		Registry.GetAssets(Filter, BlueprintAssets);

		TArray<FAssetData> DerivedAssets;
		for (const FAssetData& Asset : BlueprintAssets)
		{
			const FString GeneratedClassTag = Asset.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
			if (GeneratedClassTag.IsEmpty()) continue;
			const FTopLevelAssetPath GeneratedClassPath(FPackageName::ExportTextPathToObjectPath(GeneratedClassTag));
			if (DerivedClassPaths.Contains(GeneratedClassPath)) DerivedAssets.Add(Asset);
		}
		DerivedAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});
		Entry.TotalDescendantCount = DerivedAssets.Num();
		Entry.bTruncated = DerivedAssets.Num() > LoadLimit;
		const int32 InspectCount = FMath::Min(DerivedAssets.Num(), LoadLimit);
		Entry.LoadedBlueprints.Reserve(InspectCount);
		for (int32 Index = 0; Index < InspectCount; ++Index)
		{
			if (UBlueprint* ChildBlueprint = Cast<UBlueprint>(DerivedAssets[Index].GetAsset()))
			{
				Entry.LoadedBlueprints.Add(ChildBlueprint);
			}
		}
		return Entry;
	}

	TSharedPtr<FJsonObject> SerializeDerivedOverrides(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const int32 LoadLimit,
		TMap<UBlueprint*, FDerivedBlueprintCacheEntry>& Cache)
	{
		auto Json = MakeShared<FJsonObject>();
		FDerivedBlueprintCacheEntry& Derived = GetDerivedBlueprints(Blueprint, LoadLimit, Cache);
		Json->SetStringField(TEXT("discovery"), TEXT("AssetRegistry.GetDerivedClassNames"));
		Json->SetNumberField(TEXT("descendantCount"), Derived.TotalDescendantCount);
		Json->SetNumberField(TEXT("inspectedCount"), Derived.LoadedBlueprints.Num());
		Json->SetBoolField(TEXT("truncated"), Derived.bTruncated);
		Json->SetNumberField(TEXT("loadLimit"), LoadLimit);

		TArray<TSharedPtr<FJsonValue>> Overrides;
		for (UBlueprint* ChildBlueprint : Derived.LoadedBlueprints)
		{
			if (!ChildBlueprint) continue;
			UEdGraph* OverrideGraph = FindLocalGraph(ChildBlueprint, Graph->GetFName());
			if (!OverrideGraph) continue;

			auto Override = MakeShared<FJsonObject>();
			Override->SetStringField(TEXT("assetPath"), BlueprintAssetPath(ChildBlueprint));
			Override->SetStringField(TEXT("parentClass"), ObjectPath(ChildBlueprint->ParentClass));
			Override->SetStringField(TEXT("generatedClass"), ObjectPath(ChildBlueprint->GeneratedClass));
			Override->SetStringField(TEXT("graphGuid"), GuidString(OverrideGraph->GraphGuid));
			Override->SetObjectField(TEXT("function"), SerializeFunctionMetadata(ChildBlueprint, OverrideGraph));
			Overrides.Add(MakeShared<FJsonValueObject>(Override));
		}
		Json->SetNumberField(TEXT("localOverrideCount"), Overrides.Num());
		Json->SetArrayField(TEXT("overrides"), Overrides);
		return Json;
	}

	FString MakeDumpPath(const FString& AssetPath, const FString& GraphName)
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		const FString FileName = FPaths::MakeValidFileName(AssetName + TEXT("_") + GraphName + TEXT("_compact.json"));
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_MCP"), TEXT("CompactGraphs"), FileName);
	}

	bool WriteJson(const TSharedPtr<FJsonObject>& Json, const FString& Path, FString& OutError)
	{
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
		{
			OutError = FString::Printf(TEXT("Failed to create directory for %s"), *Path);
			return false;
		}
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Json.ToSharedRef(), Writer))
		{
			OutError = TEXT("Failed to serialize compact graph JSON");
			return false;
		}
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write %s"), *Path);
			return false;
		}
		return true;
	}
}

TSharedPtr<FJsonValue> FBlueprintHandlers::ExportCompactGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Error = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Error;
	const FString RequestedGraphName = OptionalString(Params, TEXT("graphName"), TEXT("EventGraph"));
	const int32 MaxDepth = FMath::Clamp(static_cast<int32>(OptionalNumber(Params, TEXT("maxDepth"), 0.0)), 0, 5);
	const bool bIncludeHiddenPins = OptionalBool(Params, TEXT("includeHiddenPins"), true);
	const bool bIncludeDerivedOverrides = OptionalBool(Params, TEXT("includeDerivedOverrides"), false);
	const int32 DerivedOverrideLoadLimit = FMath::Clamp(
		static_cast<int32>(OptionalNumber(Params, TEXT("derivedOverrideLoadLimit"), 128.0)), 1, 5000);
	const bool bDumpToFile = OptionalBool(Params, TEXT("dumpToFile"), false);
	const bool bInlineResult = OptionalBool(Params, TEXT("inlineResult"), true);
	if (!bInlineResult && !bDumpToFile)
	{
		return MCPError(TEXT("inlineResult=false requires dumpToFile=true"));
	}

	UBlueprint* RootBlueprint = LoadBlueprint(AssetPath);
	if (!RootBlueprint) return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	UEdGraph* RootGraph = FindGraph(RootBlueprint, RequestedGraphName);
	if (!RootGraph) return MCPError(FString::Printf(TEXT("Graph not found: %s"), *RequestedGraphName));

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("schema"), TEXT("ue-mcp.compact-blueprint-graph"));
	Result->SetStringField(TEXT("schemaVersion"), TEXT("1.0.0"));
	Result->SetStringField(TEXT("fidelity"), TEXT("analysis-loss-aware; use export_nodes_t3d for exact round-trip mutation"));
	auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("assetPath"), BlueprintAssetPath(RootBlueprint));
	Root->SetStringField(TEXT("graphName"), RootGraph->GetName());
	Root->SetStringField(TEXT("graphId"), GraphId(RootGraph));
	Root->SetNumberField(TEXT("maxDepth"), MaxDepth);
	Result->SetObjectField(TEXT("root"), Root);

	TArray<FCompactGraphWork> Queue;
	Queue.Add({RootGraph, 0, FString(), FString(), TEXT("root")});
	TSet<const UEdGraph*> Queued;
	Queued.Add(RootGraph);
	TArray<TSharedPtr<FJsonValue>> Graphs;
	TArray<TSharedPtr<FJsonValue>> Blueprints;
	TSet<const UBlueprint*> SerializedBlueprints;
	TArray<TSharedPtr<FJsonValue>> UnsupportedNodes;
	TArray<TSharedPtr<FJsonValue>> ExternalDependencies;
	TMap<UBlueprint*, FDerivedBlueprintCacheEntry> DerivedBlueprintCache;
	int32 TotalNodes = 0;
	int32 TotalPins = 0;
	int32 TotalEdges = 0;
	int32 OrphanedPins = 0;
	int32 OwnerlessOrMismatchedPins = 0;
	int32 DanglingLinks = 0;

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		const FCompactGraphWork Work = Queue[QueueIndex];
		UEdGraph* Graph = Work.Graph;
		if (!Graph) continue;
		UBlueprint* Blueprint = GetOwningBlueprint(Graph);
		if (!Blueprint) continue;
		if (!SerializedBlueprints.Contains(Blueprint))
		{
			SerializedBlueprints.Add(Blueprint);
			Blueprints.Add(MakeShared<FJsonValueObject>(SerializeBlueprintMetadata(Blueprint)));
		}

		auto GraphJson = MakeShared<FJsonObject>();
		const FString CurrentGraphId = GraphId(Graph);
		const FString Kind = GraphKind(Blueprint, Graph);
		GraphJson->SetStringField(TEXT("id"), CurrentGraphId);
		GraphJson->SetStringField(TEXT("guid"), GuidString(Graph->GraphGuid));
		GraphJson->SetStringField(TEXT("name"), Graph->GetName());
		GraphJson->SetStringField(TEXT("kind"), Kind);
		GraphJson->SetNumberField(TEXT("depth"), Work.Depth);
		GraphJson->SetStringField(TEXT("objectPath"), Graph->GetPathName());
		GraphJson->SetStringField(TEXT("schemaClass"), ObjectPath(Graph->GetSchema()));
		GraphJson->SetStringField(TEXT("referencedByGraphId"), Work.ReferencedByGraphId);
		GraphJson->SetStringField(TEXT("referencedByNodeId"), Work.ReferencedByNodeId);
		GraphJson->SetStringField(TEXT("referenceKind"), Work.ReferenceKind);
		GraphJson->SetStringField(TEXT("ownerBlueprintAssetPath"), BlueprintAssetPath(Blueprint));

		if (Kind == TEXT("Function") || Kind == TEXT("Construction") || Kind == TEXT("DelegateSignature"))
		{
			GraphJson->SetObjectField(TEXT("function"), SerializeFunctionMetadata(Blueprint, Graph));
			if (bIncludeDerivedOverrides && Kind == TEXT("Function"))
			{
				GraphJson->SetObjectField(TEXT("derivedOverrides"), SerializeDerivedOverrides(
					Blueprint, Graph, DerivedOverrideLoadLimit, DerivedBlueprintCache));
			}
		}

		TArray<FBPVariableDescription> LocalVariables;
		if (const UEdGraphSchema* Schema = Graph->GetSchema())
		{
			Schema->GetLocalVariables(Graph, LocalVariables);
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
			if (!Entry)
			{
				continue;
			}
			for (const FBPVariableDescription& EntryLocal : Entry->LocalVariables)
			{
				const bool bAlreadyPresent = LocalVariables.ContainsByPredicate(
					[&EntryLocal](const FBPVariableDescription& Existing)
					{
						return Existing.VarGuid == EntryLocal.VarGuid ||
							Existing.VarName == EntryLocal.VarName;
					});
				if (!bAlreadyPresent)
				{
					LocalVariables.Add(EntryLocal);
				}
			}
			break;
		}
		TArray<TSharedPtr<FJsonValue>> Locals;
		for (const FBPVariableDescription& Local : LocalVariables)
		{
			Locals.Add(MakeShared<FJsonValueObject>(SerializeVariable(Local)));
		}
		GraphJson->SetArrayField(TEXT("localVariables"), Locals);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Edges;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			++TotalNodes;
			bool bSupported = false;
			FString UnsupportedReason;
			const FString SemanticType = SemanticTypeForNode(Node, bSupported, UnsupportedReason);
			auto NodeJson = MakeShared<FJsonObject>();
			const FString NodeId = GuidString(Node->NodeGuid);
			NodeJson->SetStringField(TEXT("id"), NodeId);
			NodeJson->SetStringField(TEXT("rawName"), Node->GetName());
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			NodeJson->SetStringField(TEXT("title"), SafeNodeTitle(Node));
			NodeJson->SetBoolField(TEXT("titleFallbackForUnsafePins"), HasUnsafePinOwnership(Node));
			NodeJson->SetStringField(TEXT("semanticType"), SemanticType);
			NodeJson->SetBoolField(TEXT("semanticSupported"), bSupported);
			if (!bSupported) NodeJson->SetStringField(TEXT("unsupportedReason"), UnsupportedReason);
			NodeJson->SetNumberField(TEXT("posX"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("posY"), Node->NodePosY);
			NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
			NodeJson->SetBoolField(TEXT("enabled"), Node->GetDesiredEnabledState() != ENodeEnabledState::Disabled);
			NodeJson->SetBoolField(TEXT("developmentOnly"), Node->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly);

			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				NodeJson->SetObjectField(TEXT("memberReference"), SerializeMemberReference(Call->FunctionReference));
				UFunction* Function = Call->GetTargetFunction();
				NodeJson->SetStringField(TEXT("targetFunction"), ObjectPath(Function));
				NodeJson->SetArrayField(TEXT("targetFunctionFlags"), FunctionFlagNames(Function));
				NodeJson->SetBoolField(TEXT("latent"), Function && Function->HasMetaData(TEXT("Latent")));
			}
			else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
			{
				NodeJson->SetObjectField(TEXT("memberReference"), SerializeMemberReference(Variable->VariableReference));
			}
			else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
			{
				NodeJson->SetObjectField(TEXT("memberReference"), SerializeMemberReference(Event->EventReference));
			}
			else if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				NodeJson->SetObjectField(TEXT("memberReference"), SerializeMemberReference(Entry->FunctionReference));
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || (!bIncludeHiddenPins && Pin->bHidden)) continue;
				++TotalPins;
				if (Pin->bOrphanedPin) ++OrphanedPins;
				if (Pin->GetOwningNodeUnchecked() != Node) ++OwnerlessOrMismatchedPins;
				Pins.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, Node)));

				if (Pin->Direction != EGPD_Output) continue;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					auto Edge = MakeShared<FJsonObject>();
					const UEdGraphNode* TargetNode = LinkedPin ? LinkedPin->GetOwningNodeUnchecked() : nullptr;
					Edge->SetStringField(TEXT("kind"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ? TEXT("exec") : TEXT("data"));
					Edge->SetStringField(TEXT("sourceNodeId"), NodeId);
					Edge->SetStringField(TEXT("sourcePinId"), GuidString(Pin->PinId));
					Edge->SetStringField(TEXT("sourcePinName"), Pin->PinName.ToString());
					Edge->SetStringField(TEXT("targetNodeId"), TargetNode ? GuidString(TargetNode->NodeGuid) : FString());
					Edge->SetStringField(TEXT("targetPinId"), LinkedPin ? GuidString(LinkedPin->PinId) : FString());
					Edge->SetStringField(TEXT("targetPinName"), LinkedPin ? LinkedPin->PinName.ToString() : FString());
					const bool bDangling = !LinkedPin || !TargetNode;
					Edge->SetBoolField(TEXT("dangling"), bDangling);
					if (bDangling) ++DanglingLinks;
					++TotalEdges;
					Edges.Add(MakeShared<FJsonValueObject>(Edge));
				}
			}
			NodeJson->SetArrayField(TEXT("pins"), Pins);

			FString ReferenceKind;
			if (UEdGraph* DependencyGraph = ReferencedGraph(Node, ReferenceKind))
			{
				NodeJson->SetStringField(TEXT("referencedGraphId"), GraphId(DependencyGraph));
				NodeJson->SetStringField(TEXT("referencedGraphPath"), DependencyGraph->GetPathName());
				NodeJson->SetStringField(TEXT("referenceKind"), ReferenceKind);
				UBlueprint* DependencyBlueprint = GetOwningBlueprint(DependencyGraph);
				const FString DependencyAssetPath = BlueprintAssetPath(DependencyBlueprint);
				const bool bUserGraph = DependencyAssetPath.StartsWith(TEXT("/Game/"));
				if (bUserGraph && Work.Depth < MaxDepth && !Queued.Contains(DependencyGraph))
				{
					Queued.Add(DependencyGraph);
					Queue.Add({DependencyGraph, Work.Depth + 1, CurrentGraphId, NodeId, ReferenceKind});
				}
				else if (!bUserGraph)
				{
					auto External = MakeShared<FJsonObject>();
					External->SetStringField(TEXT("sourceGraphId"), CurrentGraphId);
					External->SetStringField(TEXT("sourceNodeId"), NodeId);
					External->SetStringField(TEXT("targetGraphPath"), DependencyGraph->GetPathName());
					External->SetStringField(TEXT("reason"), TEXT("Engine/plugin graph omitted from recursive user-content capture"));
					ExternalDependencies.Add(MakeShared<FJsonValueObject>(External));
				}
			}

			if (!bSupported)
			{
				auto Unsupported = MakeShared<FJsonObject>();
				Unsupported->SetStringField(TEXT("graphId"), CurrentGraphId);
				Unsupported->SetStringField(TEXT("graphName"), Graph->GetName());
				Unsupported->SetStringField(TEXT("nodeId"), NodeId);
				Unsupported->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
				Unsupported->SetStringField(TEXT("title"), SafeNodeTitle(Node));
				Unsupported->SetStringField(TEXT("reason"), UnsupportedReason);
				UnsupportedNodes.Add(MakeShared<FJsonValueObject>(Unsupported));
			}
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}

		GraphJson->SetNumberField(TEXT("nodeCount"), Nodes.Num());
		GraphJson->SetNumberField(TEXT("edgeCount"), Edges.Num());
		GraphJson->SetArrayField(TEXT("nodes"), Nodes);
		GraphJson->SetArrayField(TEXT("edges"), Edges);
		Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
	}

	Result->SetArrayField(TEXT("blueprints"), Blueprints);
	Result->SetArrayField(TEXT("graphs"), Graphs);
	Result->SetArrayField(TEXT("unsupportedNodes"), UnsupportedNodes);
	Result->SetArrayField(TEXT("externalGraphDependencies"), ExternalDependencies);
	auto Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("graphCount"), Graphs.Num());
	Stats->SetNumberField(TEXT("blueprintCount"), Blueprints.Num());
	Stats->SetNumberField(TEXT("nodeCount"), TotalNodes);
	Stats->SetNumberField(TEXT("pinCount"), TotalPins);
	Stats->SetNumberField(TEXT("edgeCount"), TotalEdges);
	Stats->SetNumberField(TEXT("unsupportedNodeCount"), UnsupportedNodes.Num());
	Stats->SetNumberField(TEXT("orphanedPinCount"), OrphanedPins);
	Stats->SetNumberField(TEXT("ownerlessOrMismatchedPinCount"), OwnerlessOrMismatchedPins);
	Stats->SetNumberField(TEXT("danglingLinkCount"), DanglingLinks);
	Stats->SetNumberField(TEXT("externalDependencyCount"), ExternalDependencies.Num());
	Result->SetObjectField(TEXT("stats"), Stats);

	if (bDumpToFile)
	{
		FString OutputPath = OptionalString(Params, TEXT("outputPath"));
		if (OutputPath.IsEmpty()) OutputPath = MakeDumpPath(AssetPath, RootGraph->GetName());
		else if (FPaths::IsRelative(OutputPath)) OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), OutputPath);
		FString WriteError;
		if (!WriteJson(Result, OutputPath, WriteError)) return MCPError(WriteError);
		Result->SetStringField(TEXT("dumpPath"), OutputPath);
	}
	if (!bInlineResult)
	{
		Result->RemoveField(TEXT("blueprints"));
		Result->RemoveField(TEXT("graphs"));
		Result->RemoveField(TEXT("unsupportedNodes"));
		Result->RemoveField(TEXT("externalGraphDependencies"));
		Result->SetBoolField(TEXT("inlineOmitted"), true);
	}
	return MCPResult(Result);
}
