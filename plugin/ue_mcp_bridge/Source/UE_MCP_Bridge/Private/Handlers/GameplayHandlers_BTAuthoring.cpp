// Behavior tree node authoring - split from GameplayHandlers.cpp.
// All functions below are still members of FGameplayHandlers; this file is a
// translation-unit partition. Registration stays in GameplayHandlers.cpp.
//
// Authoring strategy: mutate the RUNTIME tree (UBehaviorTree::RootNode /
// FBTCompositeChild) - the exact structure UBehaviorTreeManager::LoadTree
// executes - and null the editor-only BTGraph afterwards. When a human next
// opens the asset, FBehaviorTreeEditor::RestoreBehaviorTree sees the missing
// graph and rebuilds it from the runtime tree via SpawnMissingNodes(), so
// authored trees render correctly in the editor. The trade-off is that any
// hand-made graph layout/comments are regenerated on next open.

#include "GameplayHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "EditorAssetLibrary.h"
#include "UObject/UObjectIterator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	UBehaviorTree* LoadBT(const FString& AssetPath, FString& OutError)
	{
		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
		if (!BT)
		{
			OutError = FString::Printf(TEXT("BehaviorTree not found: %s"), *AssetPath);
		}
		return BT;
	}

	/** Resolve a BT node class by short name ("BTTask_Wait", "Wait"),
	 *  full path, or blueprint class path (auto-appends _C). */
	UClass* ResolveBTClass(const FString& Token, UClass* BaseClass, FString& OutError)
	{
		UClass* Found = nullptr;
		if (Token.Contains(TEXT("/")))
		{
			Found = LoadClass<UObject>(nullptr, *Token);
			if (!Found && !Token.EndsWith(TEXT("_C")))
			{
				Found = LoadClass<UObject>(nullptr, *(Token + TEXT("_C")));
			}
		}
		if (!Found)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(BaseClass) || It->HasAnyClassFlags(CLASS_Abstract)) continue;
				if (It->GetName() == Token)
				{
					Found = *It;
					break;
				}
			}
		}
		// Prefixed conveniences: "Wait" -> BTTask_Wait, "Blackboard" -> BTDecorator_Blackboard, ...
		if (!Found)
		{
			const TCHAR* Prefixes[] = { TEXT("BTTask_"), TEXT("BTDecorator_"), TEXT("BTService_"), TEXT("BTComposite_") };
			for (const TCHAR* Prefix : Prefixes)
			{
				const FString Prefixed = Prefix + Token;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->GetName() == Prefixed && It->IsChildOf(BaseClass) && !It->HasAnyClassFlags(CLASS_Abstract))
					{
						Found = *It;
						break;
					}
				}
				if (Found) break;
			}
		}
		if (!Found)
		{
			OutError = FString::Printf(TEXT("Class '%s' not found or not a %s subclass"), *Token, *BaseClass->GetName());
			return nullptr;
		}
		if (!Found->IsChildOf(BaseClass))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a %s subclass"), *Token, *BaseClass->GetName());
			return nullptr;
		}
		return Found;
	}

	/** Depth-first walk over every composite in the tree. */
	void WalkComposites(UBTCompositeNode* Node, TFunctionRef<void(UBTCompositeNode*)> Visit)
	{
		if (!Node) return;
		Visit(Node);
		for (FBTCompositeChild& Child : Node->Children)
		{
			if (Child.ChildComposite)
			{
				WalkComposites(Child.ChildComposite, Visit);
			}
		}
	}

	/** Match a node by object name (exact) or NodeName (case-insensitive).
	 *  "root" (or empty) resolves to the root composite. */
	UBTNode* FindBTNodeByToken(UBehaviorTree* BT, const FString& Token, FString& OutError)
	{
		if (Token.IsEmpty() || Token.Equals(TEXT("root"), ESearchCase::IgnoreCase))
		{
			if (!BT->RootNode)
			{
				OutError = TEXT("Tree has no root composite yet - add_bt_composite with parentName 'root' first");
			}
			return BT->RootNode;
		}
		TArray<UBTNode*> Matches;
		auto Consider = [&](UBTNode* Node)
		{
			if (!Node) return;
			if (Node->GetName() == Token || Node->NodeName.Equals(Token, ESearchCase::IgnoreCase))
			{
				Matches.AddUnique(Node);
			}
		};
		WalkComposites(BT->RootNode, [&](UBTCompositeNode* Comp)
		{
			Consider(Comp);
			for (FBTCompositeChild& Child : Comp->Children)
			{
				Consider(Child.ChildTask);
				for (UBTDecorator* D : Child.Decorators) Consider(D);
			}
			for (UBTService* S : Comp->Services) Consider(S);
		});
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No node named '%s' in the tree (match by object name or NodeName; use gameplay read_behavior_tree_graph to list)"), *Token);
			return nullptr;
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Names;
			for (UBTNode* M : Matches) Names.Add(M->GetName());
			OutError = FString::Printf(TEXT("Node name '%s' is ambiguous: %s. Use the unique object name."), *Token, *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}
		return Matches[0];
	}

	/** Locate the parent composite + child index owning a given node. */
	bool FindParentSlot(UBehaviorTree* BT, UBTNode* Node, UBTCompositeNode*& OutParent, int32& OutChildIndex)
	{
		OutParent = nullptr;
		OutChildIndex = INDEX_NONE;
		WalkComposites(BT->RootNode, [&](UBTCompositeNode* Comp)
		{
			for (int32 i = 0; i < Comp->Children.Num(); ++i)
			{
				if (Comp->Children[i].ChildComposite == Node || Comp->Children[i].ChildTask == Node)
				{
					OutParent = Comp;
					OutChildIndex = i;
				}
			}
		});
		return OutParent != nullptr;
	}

	/** Apply an optional 'properties' object to a node (dotted paths OK, so
	 *  BlackboardKey.SelectedKeyName works). Returns per-key errors. */
	TArray<TSharedPtr<FJsonValue>> ApplyNodeProperties(UBTNode* Node, const TSharedPtr<FJsonObject>& Params)
	{
		TArray<TSharedPtr<FJsonValue>> Errors;
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Params->TryGetObjectField(TEXT("properties"), Props) || !Props->IsValid())
		{
			return Errors;
		}
		for (const auto& Pair : (*Props)->Values)
		{
			FString Error;
			if (!MCPJsonProperty::SetDottedPropertyFromJson(Node, Pair.Key, Pair.Value, Error))
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Error)));
			}
		}
		return Errors;
	}

	/** Mark the asset dirty, drop the stale editor graph (regenerated from the
	 *  runtime tree on next editor open), and save. */
	void FinalizeBTEdit(UBehaviorTree* BT)
	{
#if WITH_EDITORONLY_DATA
		BT->BTGraph = nullptr;
#endif
		BT->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(BT->GetPathName());
	}

	TSharedPtr<FJsonObject> DescribeNode(const UBTNode* Node)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Node->GetName());
		Obj->SetStringField(TEXT("nodeName"), Node->NodeName);
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		return Obj;
	}
}

// gameplay(add_bt_composite): add a Selector/Sequence/SimpleParallel under a
// parent composite (or as the tree root).
TSharedPtr<FJsonValue> FGameplayHandlers::AddBTComposite(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	const FString CompositeType = OptionalString(Params, TEXT("compositeType"), TEXT("Selector"));
	const FString ParentToken = OptionalString(Params, TEXT("parentName"), TEXT("root"));
	const FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UClass* CompClass = ResolveBTClass(CompositeType, UBTCompositeNode::StaticClass(), Error);
	if (!CompClass) return MCPError(Error);

	UBTCompositeNode* NewComp = NewObject<UBTCompositeNode>(BT, CompClass, NAME_None, RF_Transactional);
	if (!NodeName.IsEmpty()) NewComp->NodeName = NodeName;

	if (!BT->RootNode)
	{
		// First composite becomes the root regardless of parentName.
		BT->RootNode = NewComp;
	}
	else
	{
		UBTNode* Parent = FindBTNodeByToken(BT, ParentToken, Error);
		if (!Parent) return MCPError(Error);
		UBTCompositeNode* ParentComp = Cast<UBTCompositeNode>(Parent);
		if (!ParentComp)
		{
			return MCPError(FString::Printf(TEXT("Parent '%s' is a %s, not a composite"), *ParentToken, *Parent->GetClass()->GetName()));
		}
		FBTCompositeChild& Child = ParentComp->Children.AddDefaulted_GetRef();
		Child.ChildComposite = NewComp;
	}

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyNodeProperties(NewComp, Params);
	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetObjectField(TEXT("node"), DescribeNode(NewComp));
	Result->SetBoolField(TEXT("isRoot"), BT->RootNode == NewComp);
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(add_bt_task): add a task leaf (BTTask_Wait, BTTask_MoveTo, BP task)
// under a composite. properties are applied via the dotted-path JSON setter,
// e.g. {"BlackboardKey.SelectedKeyName": "TargetActor", "WaitTime": 2.5}.
TSharedPtr<FJsonValue> FGameplayHandlers::AddBTTask(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString TaskClass;
	if (auto Err = RequireString(Params, TEXT("taskClass"), TaskClass)) return Err;
	const FString ParentToken = OptionalString(Params, TEXT("parentName"), TEXT("root"));
	const FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UClass* Class = ResolveBTClass(TaskClass, UBTTaskNode::StaticClass(), Error);
	if (!Class) return MCPError(Error);

	UBTNode* Parent = FindBTNodeByToken(BT, ParentToken, Error);
	if (!Parent) return MCPError(Error);
	UBTCompositeNode* ParentComp = Cast<UBTCompositeNode>(Parent);
	if (!ParentComp)
	{
		return MCPError(FString::Printf(TEXT("Parent '%s' is not a composite"), *ParentToken));
	}

	UBTTaskNode* NewTask = NewObject<UBTTaskNode>(BT, Class, NAME_None, RF_Transactional);
	if (!NodeName.IsEmpty()) NewTask->NodeName = NodeName;

	FBTCompositeChild& Child = ParentComp->Children.AddDefaulted_GetRef();
	Child.ChildTask = NewTask;

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyNodeProperties(NewTask, Params);
	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetObjectField(TEXT("node"), DescribeNode(NewTask));
	Result->SetStringField(TEXT("parent"), ParentComp->GetName());
	Result->SetNumberField(TEXT("childIndex"), ParentComp->Children.Num() - 1);
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(add_bt_decorator): attach a decorator to an existing child node
// (the decorator lives on the parent's child slot, matching the BT editor).
TSharedPtr<FJsonValue> FGameplayHandlers::AddBTDecorator(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString DecoratorClass;
	if (auto Err = RequireString(Params, TEXT("decoratorClass"), DecoratorClass)) return Err;
	FString TargetToken;
	if (auto Err = RequireString(Params, TEXT("targetName"), TargetToken)) return Err;
	const FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UClass* Class = ResolveBTClass(DecoratorClass, UBTDecorator::StaticClass(), Error);
	if (!Class) return MCPError(Error);

	UBTNode* Target = FindBTNodeByToken(BT, TargetToken, Error);
	if (!Target) return MCPError(Error);

	UBTDecorator* NewDecorator = NewObject<UBTDecorator>(BT, Class, NAME_None, RF_Transactional);
	if (!NodeName.IsEmpty()) NewDecorator->NodeName = NodeName;

	if (Target == BT->RootNode)
	{
		BT->RootDecorators.Add(NewDecorator);
	}
	else
	{
		UBTCompositeNode* Parent = nullptr;
		int32 ChildIndex = INDEX_NONE;
		if (!FindParentSlot(BT, Target, Parent, ChildIndex))
		{
			return MCPError(FString::Printf(TEXT("Could not locate '%s' in any composite child slot"), *TargetToken));
		}
		Parent->Children[ChildIndex].Decorators.Add(NewDecorator);
	}

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyNodeProperties(NewDecorator, Params);
	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetObjectField(TEXT("node"), DescribeNode(NewDecorator));
	Result->SetStringField(TEXT("attachedTo"), Target->GetName());
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(add_bt_service): attach a service to a composite or task node.
TSharedPtr<FJsonValue> FGameplayHandlers::AddBTService(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString ServiceClass;
	if (auto Err = RequireString(Params, TEXT("serviceClass"), ServiceClass)) return Err;
	FString TargetToken;
	if (auto Err = RequireString(Params, TEXT("targetName"), TargetToken)) return Err;
	const FString NodeName = OptionalString(Params, TEXT("nodeName"), TEXT(""));

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UClass* Class = ResolveBTClass(ServiceClass, UBTService::StaticClass(), Error);
	if (!Class) return MCPError(Error);

	UBTNode* Target = FindBTNodeByToken(BT, TargetToken, Error);
	if (!Target) return MCPError(Error);

	UBTService* NewService = NewObject<UBTService>(BT, Class, NAME_None, RF_Transactional);
	if (!NodeName.IsEmpty()) NewService->NodeName = NodeName;

	if (UBTCompositeNode* Comp = Cast<UBTCompositeNode>(Target))
	{
		Comp->Services.Add(NewService);
	}
	else if (UBTTaskNode* Task = Cast<UBTTaskNode>(Target))
	{
		Task->Services.Add(NewService);
	}
	else
	{
		return MCPError(FString::Printf(TEXT("'%s' is a %s - services attach to composites or tasks"), *TargetToken, *Target->GetClass()->GetName()));
	}

	const TArray<TSharedPtr<FJsonValue>> PropErrors = ApplyNodeProperties(NewService, Params);
	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetObjectField(TEXT("node"), DescribeNode(NewService));
	Result->SetStringField(TEXT("attachedTo"), Target->GetName());
	if (PropErrors.Num() > 0) Result->SetArrayField(TEXT("propertyErrors"), PropErrors);
	return MCPResult(Result);
}

// gameplay(remove_bt_node): remove a composite/task (with its subtree),
// decorator, or service by name.
TSharedPtr<FJsonValue> FGameplayHandlers::RemoveBTNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString Token;
	if (auto Err = RequireString(Params, TEXT("nodeName"), Token)) return Err;

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UBTNode* Node = FindBTNodeByToken(BT, Token, Error);
	if (!Node) return MCPError(Error);

	bool bRemoved = false;
	if (Node == BT->RootNode)
	{
		BT->RootNode = nullptr;
		bRemoved = true;
	}
	else
	{
		// Child slot (composite or task)?
		UBTCompositeNode* Parent = nullptr;
		int32 ChildIndex = INDEX_NONE;
		if (FindParentSlot(BT, Node, Parent, ChildIndex))
		{
			Parent->Children.RemoveAt(ChildIndex);
			bRemoved = true;
		}
		else
		{
			// Decorator or service?
			if (UBTDecorator* Dec = Cast<UBTDecorator>(Node))
			{
				if (BT->RootDecorators.Remove(Dec) > 0) bRemoved = true;
			}
			WalkComposites(BT->RootNode, [&](UBTCompositeNode* Comp)
			{
				if (bRemoved) return;
				for (FBTCompositeChild& Child : Comp->Children)
				{
					if (UBTDecorator* Dec = Cast<UBTDecorator>(Node))
					{
						if (Child.Decorators.Remove(Dec) > 0) { bRemoved = true; return; }
					}
					if (Child.ChildTask)
					{
						if (UBTService* Svc = Cast<UBTService>(Node))
						{
							if (Child.ChildTask->Services.Remove(Svc) > 0) { bRemoved = true; return; }
						}
					}
				}
				if (UBTService* Svc = Cast<UBTService>(Node))
				{
					if (Comp->Services.Remove(Svc) > 0) { bRemoved = true; }
				}
			});
		}
	}

	if (!bRemoved)
	{
		return MCPError(FString::Printf(TEXT("Found '%s' but could not detach it from the tree"), *Token));
	}

	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("removed"), Node->GetName());
	return MCPResult(Result);
}

// gameplay(set_bt_node_property): dotted-path property write on any BT node.
TSharedPtr<FJsonValue> FGameplayHandlers::SetBTNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString Token;
	if (auto Err = RequireString(Params, TEXT("nodeName"), Token)) return Err;
	FString PropertyName;
	if (auto Err = RequireString(Params, TEXT("propertyName"), PropertyName)) return Err;
	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid()) return MCPError(TEXT("Missing 'value'"));

	FString Error;
	UBehaviorTree* BT = LoadBT(AssetPath, Error);
	if (!BT) return MCPError(Error);

	UBTNode* Node = FindBTNodeByToken(BT, Token, Error);
	if (!Node) return MCPError(Error);

	if (!MCPJsonProperty::SetDottedPropertyFromJson(Node, PropertyName, Value, Error))
	{
		return MCPError(FString::Printf(TEXT("Set '%s' failed: %s"), *PropertyName, *Error));
	}

	FinalizeBTEdit(BT);

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("node"), Node->GetName());
	Result->SetStringField(TEXT("propertyName"), PropertyName);
	return MCPResult(Result);
}
