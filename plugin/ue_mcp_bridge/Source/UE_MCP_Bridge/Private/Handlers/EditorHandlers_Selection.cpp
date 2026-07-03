// Live working-context capture.
//
// get_selected_graph_nodes  - the nodes the user has selected in the currently
//   focused asset-editor graph (Blueprint / Widget Blueprint / Animation
//   Blueprint), rendered as readable text plus a structured node list. Powers
//   "explain what I have selected" and "insert logic after this node" flows.
//   Material / Behavior Tree editors live behind private toolkit headers, so
//   those are reported as unsupported here (use asset(describe) instead).
//
// get_content_browser_selection - the Content Browser's current folder and the
//   assets selected in it, so an agent can act on "make one of these" / "use
//   the selected asset" without the user pasting paths.

#include "EditorHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "BlueprintEditor.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString SelNodeTitle(const UEdGraphNode* Node)
	{
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\n"), TEXT(" "));
		Title.ReplaceInline(TEXT("\r"), TEXT(""));
		return Title.TrimStartAndEnd();
	}

	bool SelIsExec(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	FString SelInputs(const UEdGraphNode* Node)
	{
		TArray<FString> Parts;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || SelIsExec(Pin) || Pin->bHidden) continue;
			const FString PinName = Pin->GetDisplayName().ToString().IsEmpty()
				? Pin->PinName.ToString() : Pin->GetDisplayName().ToString();
			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Pin->LinkedTo[0]->GetOwningNode())
			{
				Parts.Add(FString::Printf(TEXT("%s<-%s"), *PinName, *SelNodeTitle(Pin->LinkedTo[0]->GetOwningNode())));
			}
			else
			{
				const FString Val = Pin->GetDefaultAsString();
				if (!Val.IsEmpty() && Val != TEXT("()") && Val != TEXT("None") && Val != TEXT("0") && Val != TEXT("false"))
				{
					Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *Val));
				}
			}
		}
		return Parts.Num() > 0 ? FString::Printf(TEXT(" (%s)"), *FString::Join(Parts, TEXT(", "))) : FString();
	}

	void WalkSelected(UEdGraphNode* Node, const TSet<UEdGraphNode*>& Sel, FString& Out, int32 Level, TSet<UEdGraphNode*>& Visited)
	{
		while (Node && Sel.Contains(Node) && !Visited.Contains(Node))
		{
			Visited.Add(Node);
			Out += FString::ChrN(Level * 2, TEXT(' ')) + SelNodeTitle(Node) + SelInputs(Node) + TEXT("\n");

			TArray<UEdGraphPin*> Outs;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && SelIsExec(Pin) && Pin->LinkedTo.Num() > 0) Outs.Add(Pin);
			}
			if (Outs.Num() == 1 && Outs[0]->LinkedTo[0])
			{
				Node = Outs[0]->LinkedTo[0]->GetOwningNode();
				continue;
			}
			for (UEdGraphPin* Pin : Outs)
			{
				FString PinLabel = Pin->GetDisplayName().ToString();
				if (PinLabel.IsEmpty()) PinLabel = Pin->PinName.ToString();
				if (Pin->LinkedTo[0] && Sel.Contains(Pin->LinkedTo[0]->GetOwningNode()))
				{
					Out += FString::ChrN((Level + 1) * 2, TEXT(' ')) + FString::Printf(TEXT("[%s]\n"), *PinLabel);
					WalkSelected(Pin->LinkedTo[0]->GetOwningNode(), Sel, Out, Level + 2, Visited);
				}
			}
			return;
		}
	}

	// Render a graph-editor selection set as text + a structured node array.
	void DescribeSelection(const TSet<UObject*>& Selected, FString& OutText, TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		TSet<UEdGraphNode*> Nodes;
		for (UObject* Obj : Selected)
		{
			if (UEdGraphNode* N = Cast<UEdGraphNode>(Obj)) Nodes.Add(N);
		}
		for (UEdGraphNode* N : Nodes)
		{
			TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
			NObj->SetStringField(TEXT("title"), SelNodeTitle(N));
			NObj->SetStringField(TEXT("class"), N->GetClass()->GetName());
			OutNodes.Add(MakeShared<FJsonValueObject>(NObj));
		}

		// Entry = selected node whose incoming exec is not from another selected node.
		TArray<UEdGraphNode*> Entries;
		for (UEdGraphNode* N : Nodes)
		{
			bool bEntry = true;
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (SelIsExec(Pin) && Pin->Direction == EGPD_Input)
				{
					for (UEdGraphPin* L : Pin->LinkedTo)
					{
						if (L && Nodes.Contains(L->GetOwningNode())) { bEntry = false; break; }
					}
				}
			}
			if (bEntry) Entries.Add(N);
		}

		TSet<UEdGraphNode*> Visited;
		for (UEdGraphNode* Entry : Entries)
		{
			WalkSelected(Entry, Nodes, OutText, 0, Visited);
		}
		// Any selected node not reached via exec (pure data islands) - list flat.
		for (UEdGraphNode* N : Nodes)
		{
			if (!Visited.Contains(N))
			{
				OutText += SelNodeTitle(N) + SelInputs(N) + TEXT("\n");
			}
		}
	}
}

TSharedPtr<FJsonValue> FEditorHandlers::GetSelectedGraphNodes(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return MCPError(TEXT("GEditor not available"));
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem) return MCPError(TEXT("AssetEditorSubsystem not available"));

	// The active editor is the most-recently-activated open asset editor.
	IAssetEditorInstance* Active = nullptr;
	UObject* ActiveAsset = nullptr;
	double Best = -1.0;
	for (UObject* Asset : Subsystem->GetAllEditedAssets())
	{
		for (IAssetEditorInstance* Editor : Subsystem->FindEditorsForAsset(Asset))
		{
			if (Editor && Editor->GetLastActivationTime() > Best)
			{
				Best = Editor->GetLastActivationTime();
				Active = Editor;
				ActiveAsset = Asset;
			}
		}
	}
	if (!Active) return MCPError(TEXT("No asset editor is currently open"));

	const FName EditorName = Active->GetEditorName();
	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("editor"), EditorName.ToString());
	if (ActiveAsset) Result->SetStringField(TEXT("assetPath"), ActiveAsset->GetPathName());

	// Blueprint / Widget / Animation editors all derive from FBlueprintEditor,
	// so a single cast reaches GetSelectedNodes() for every one of them.
	if (EditorName == TEXT("BlueprintEditor") ||
		EditorName == TEXT("WidgetBlueprintEditor") ||
		EditorName == TEXT("AnimationBlueprintEditor"))
	{
		FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Active);
		const FGraphPanelSelectionSet Selected = BPEditor->GetSelectedNodes();

		FString Text;
		TArray<TSharedPtr<FJsonValue>> NodeArr;
		DescribeSelection(Selected, Text, NodeArr);

		Result->SetNumberField(TEXT("nodeCount"), NodeArr.Num());
		Result->SetArrayField(TEXT("nodes"), NodeArr);
		Result->SetStringField(TEXT("text"),
			NodeArr.Num() == 0 ? TEXT("No nodes are selected in the active graph editor.") : Text);
		return MCPResult(Result);
	}

	Result->SetNumberField(TEXT("nodeCount"), 0);
	Result->SetStringField(TEXT("text"),
		FString::Printf(TEXT("The active editor ('%s') does not support graph-node selection capture. "
			"For Material / Behavior Tree assets use asset(describe)."), *EditorName.ToString()));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FEditorHandlers::GetContentBrowserSelection(const TSharedPtr<FJsonObject>& Params)
{
	FContentBrowserModule& Module = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	IContentBrowserSingleton& CB = Module.Get();

	auto Result = MCPSuccess();
	// UE 5.6's no-arg GetCurrentPath() returns FContentBrowserItemPath, not FString.
	// GetInternalPathString() asserts when the path is None (no content browser
	// focused), so guard on the virtual path before reading it.
	const FContentBrowserItemPath CurrentPath = CB.GetCurrentPath();
	Result->SetStringField(TEXT("currentPath"),
		CurrentPath.GetVirtualPathName().IsNone() ? FString() : CurrentPath.GetInternalPathString());

	TArray<FAssetData> Selected;
	CB.GetSelectedAssets(Selected);

	TArray<TSharedPtr<FJsonValue>> AssetsArr;
	for (const FAssetData& Data : Selected)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Data.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Data.GetObjectPathString());
		Obj->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());
		AssetsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Result->SetArrayField(TEXT("selectedAssets"), AssetsArr);
	Result->SetNumberField(TEXT("count"), AssetsArr.Num());
	return MCPResult(Result);
}
