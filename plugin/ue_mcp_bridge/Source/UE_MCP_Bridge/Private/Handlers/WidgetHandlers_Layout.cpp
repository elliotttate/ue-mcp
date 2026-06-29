// Batch UMG layout authoring.
//
// apply_widget_layout takes an ordered hierarchy spec and builds/configures a
// whole widget tree in one pass, then compiles + saves once. Each entry:
//   { type, name?, parent?, properties?, slotProperties? }
//   - type:           widget class short name (TextBlock, VerticalBox, Button...)
//   - name:           widget name (reused if it already exists — idempotent)
//   - parent:         name of an existing/earlier panel widget; omitted = root
//                     (or auto-added to the root panel)
//   - properties:     {propName: jsonValue} applied to the widget
//   - slotProperties: {propName: jsonValue} applied to the widget's panel slot
//
// Property values go through MCPJsonProperty::SetDottedPropertyFromJson, so
// nested structs, enums, dotted paths, object refs, and arrays all work the
// same way they do for set_widget_property / set_component_property.

#include "WidgetHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	UClass* ResolveWidgetClassForLayout(const FString& ClassName)
	{
		UClass* Cls = FindClassByShortName(ClassName);
		if (!Cls)
		{
			// Try a full-path load as a last resort.
			Cls = LoadObject<UClass>(nullptr, *ClassName);
		}
		if (Cls && Cls->IsChildOf(UWidget::StaticClass())) return Cls;
		return nullptr;
	}

	UWidget* FindInTree(UWidgetTree* Tree, const FString& Name)
	{
		if (!Tree || Name.IsEmpty()) return nullptr;
		UWidget* Found = nullptr;
		Tree->ForEachWidget([&](UWidget* W){ if (W && W->GetName() == Name) Found = W; });
		return Found;
	}

	// Apply a {propName: jsonValue} object onto Target via the shared JSON setter.
	// Collects per-property failures into OutErrors rather than aborting the run.
	void ApplyProps(UObject* Target, const TSharedPtr<FJsonObject>& Props, const FString& Who, TArray<FString>& OutErrors)
	{
		if (!Target || !Props.IsValid()) return;
		for (const auto& Pair : Props->Values)
		{
			FString Err;
			if (!MCPJsonProperty::SetDottedPropertyFromJson(Target, Pair.Key, Pair.Value, Err))
			{
				OutErrors.Add(FString::Printf(TEXT("%s.%s: %s"), *Who, *Pair.Key, *Err));
			}
		}
	}
}

TSharedPtr<FJsonValue> FWidgetHandlers::ApplyLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("assetPath"), TEXT("path"), AssetPath)) return Err;

	const TArray<TSharedPtr<FJsonValue>>* Layout = nullptr;
	if (!Params->TryGetArrayField(TEXT("layout"), Layout) || !Layout)
	{
		return MCPError(TEXT("Missing 'layout' (array of {type, name?, parent?, properties?, slotProperties?})"));
	}

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!WidgetBP) return MCPError(FString::Printf(TEXT("Failed to load WidgetBlueprint at '%s'"), *AssetPath));
	if (!WidgetBP->WidgetTree) return MCPError(TEXT("WidgetTree is null"));

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	WidgetBP->Modify();
	Tree->Modify();

	TArray<TSharedPtr<FJsonValue>> CreatedArr;
	TArray<FString> Warnings;
	int32 CreatedCount = 0;

	for (int32 i = 0; i < Layout->Num(); ++i)
	{
		const TSharedPtr<FJsonObject> Entry = (*Layout)[i].IsValid() ? (*Layout)[i]->AsObject() : nullptr;
		if (!Entry.IsValid()) { Warnings.Add(FString::Printf(TEXT("entry[%d]: not an object"), i)); continue; }

		FString TypeName;
		if (!Entry->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
		{
			Warnings.Add(FString::Printf(TEXT("entry[%d]: missing 'type'"), i));
			continue;
		}
		const FString Name = Entry->HasField(TEXT("name")) ? Entry->GetStringField(TEXT("name")) : FString();
		const FString ParentName = Entry->HasField(TEXT("parent")) ? Entry->GetStringField(TEXT("parent")) : FString();

		// Reuse an existing widget with this name (idempotent re-runs), else build.
		UWidget* Widget = !Name.IsEmpty() ? FindInTree(Tree, Name) : nullptr;
		bool bCreated = false;
		if (!Widget)
		{
			UClass* WClass = ResolveWidgetClassForLayout(TypeName);
			if (!WClass)
			{
				Warnings.Add(FString::Printf(TEXT("entry[%d]: unknown widget class '%s'"), i, *TypeName));
				continue;
			}
			Widget = Tree->ConstructWidget<UWidget>(WClass, Name.IsEmpty() ? NAME_None : FName(*Name));
			if (!Widget)
			{
				Warnings.Add(FString::Printf(TEXT("entry[%d]: failed to construct '%s'"), i, *TypeName));
				continue;
			}
			bCreated = true;

			// Place in the hierarchy.
			if (!ParentName.IsEmpty())
			{
				UPanelWidget* Parent = Cast<UPanelWidget>(FindInTree(Tree, ParentName));
				if (!Parent)
				{
					Warnings.Add(FString::Printf(TEXT("entry[%d]: parent '%s' not found or not a panel"), i, *ParentName));
					continue;
				}
				Parent->AddChild(Widget);
			}
			else if (Tree->RootWidget == nullptr)
			{
				Tree->RootWidget = Widget;
			}
			else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(Tree->RootWidget))
			{
				RootPanel->AddChild(Widget);
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("entry[%d]: no parent given and root is not a panel"), i));
				continue;
			}
			CreatedCount++;
		}

		// Apply widget + slot properties through the shared JSON setter.
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Entry->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			ApplyProps(Widget, *PropsObj, Widget->GetName(), Warnings);
		}
		const TSharedPtr<FJsonObject>* SlotObj = nullptr;
		if (Entry->TryGetObjectField(TEXT("slotProperties"), SlotObj) && SlotObj && Widget->Slot)
		{
			Widget->Slot->Modify();
			ApplyProps(Widget->Slot, *SlotObj, Widget->GetName() + TEXT(".Slot"), Warnings);
		}

		TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("name"), Widget->GetName());
		CObj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		CObj->SetBoolField(TEXT("created"), bCreated);
		CreatedArr.Add(MakeShared<FJsonValueObject>(CObj));
	}

	WidgetBP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	UEditorAssetLibrary::SaveAsset(AssetPath);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetNumberField(TEXT("widgetsCreated"), CreatedCount);
	Result->SetNumberField(TEXT("entriesProcessed"), CreatedArr.Num());
	Result->SetArrayField(TEXT("widgets"), CreatedArr);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> W;
		for (const FString& S : Warnings) W.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("warnings"), W);
	}
	return MCPResult(Result);
}
