// Graph -> readable pseudocode describer.
//
// describe_blueprint_graph walks each graph's execution chains and emits a
// compact, indented text rendering: event/function entry points, the call
// sequence, branch/sequence sub-blocks, and inline data-pin inputs (literals
// or "<- SourceNode.Pin"). This is dramatically cheaper for an agent to reason
// over than the JSON node/edge dump, and it is the natural "read" half of the
// read -> edit -> recompile loop that pairs with author_logic.

#include "BlueprintHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString Indent(int32 Level)
	{
		return FString::ChrN(Level * 2, TEXT(' '));
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\n"), TEXT(" "));
		Title.ReplaceInline(TEXT("\r"), TEXT(""));
		return Title.TrimStartAndEnd();
	}

	bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	// Render the data inputs of a node inline: " (Damage=10, Target<-Get Player)".
	FString DescribeInputs(const UEdGraphNode* Node)
	{
		TArray<FString> Parts;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsExecPin(Pin) || Pin->bHidden) continue;
			const FString PinName = Pin->GetDisplayName().ToString().IsEmpty()
				? Pin->PinName.ToString()
				: Pin->GetDisplayName().ToString();
			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				const UEdGraphNode* Src = Pin->LinkedTo[0]->GetOwningNode();
				if (Src) Parts.Add(FString::Printf(TEXT("%s<-%s"), *PinName, *NodeTitle(Src)));
			}
			else
			{
				const FString Val = Pin->GetDefaultAsString();
				// Skip empty / structurally-default values to keep lines short.
				if (!Val.IsEmpty() && Val != TEXT("()") && Val != TEXT("None") && Val != TEXT("0") && Val != TEXT("false"))
				{
					Parts.Add(FString::Printf(TEXT("%s=%s"), *PinName, *Val));
				}
			}
		}
		return Parts.Num() > 0 ? FString::Printf(TEXT(" (%s)"), *FString::Join(Parts, TEXT(", "))) : FString();
	}

	// Linked exec-output pins, in declaration order.
	TArray<UEdGraphPin*> LinkedExecOuts(const UEdGraphNode* Node)
	{
		TArray<UEdGraphPin*> Outs;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && IsExecPin(Pin) && Pin->LinkedTo.Num() > 0)
			{
				Outs.Add(Pin);
			}
		}
		return Outs;
	}

	void DescribeExec(UEdGraphNode* Node, FString& Out, int32 Level, TSet<UEdGraphNode*>& Visited)
	{
		// Linear chains stay at one indent level (reads like statements); only
		// multi-output nodes (Branch, Sequence, Cast, ForEach) open a sub-block.
		while (Node && !Visited.Contains(Node))
		{
			Visited.Add(Node);
			Out += Indent(Level) + NodeTitle(Node) + DescribeInputs(Node) + TEXT("\n");

			TArray<UEdGraphPin*> Outs = LinkedExecOuts(Node);
			if (Outs.Num() == 0) return;
			if (Outs.Num() == 1)
			{
				Node = Outs[0]->LinkedTo[0] ? Outs[0]->LinkedTo[0]->GetOwningNode() : nullptr;
				continue;
			}
			for (UEdGraphPin* Pin : Outs)
			{
				FString PinLabel = Pin->GetDisplayName().ToString();
				if (PinLabel.IsEmpty()) PinLabel = Pin->PinName.ToString();
				Out += Indent(Level + 1) + FString::Printf(TEXT("[%s]\n"), *PinLabel);
				if (Pin->LinkedTo[0])
				{
					DescribeExec(Pin->LinkedTo[0]->GetOwningNode(), Out, Level + 2, Visited);
				}
			}
			return;
		}
	}

	// True for nodes that begin an execution chain: they have at least one exec
	// pin and no incoming exec connection.
	bool IsExecEntry(const UEdGraphNode* Node)
	{
		bool bHasExecPin = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsExecPin(Pin)) continue;
			bHasExecPin = true;
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0) return false;
		}
		return bHasExecPin;
	}
}

FString FBlueprintHandlers::DescribeGraphAsText(UEdGraph* Graph)
{
	if (!Graph) return TEXT("(null graph)");

	// Entry points first (events / function entries), then any remaining
	// exec-rooted nodes. Pure data nodes are reached inline via DescribeInputs.
	TArray<UEdGraphNode*> Entries;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && IsExecEntry(Node)) Entries.Add(Node);
	}

	FString Out;
	TSet<UEdGraphNode*> Visited;
	for (UEdGraphNode* Entry : Entries)
	{
		DescribeExec(Entry, Out, 0, Visited);
		Out += TEXT("\n");
	}

	if (Out.TrimStartAndEnd().IsEmpty())
	{
		return FString::Printf(TEXT("(no execution chains; %d node(s) total)"), Graph->Nodes.Num());
	}
	return Out;
}

TSharedPtr<FJsonValue> FBlueprintHandlers::DescribeBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint) return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	const FString GraphName = OptionalString(Params, TEXT("graphName"));

	// Resolve the target set: one named graph, or all graphs on the blueprint.
	TArray<UEdGraph*> Targets;
	if (!GraphName.IsEmpty())
	{
		UEdGraph* G = FindGraph(Blueprint, GraphName);
		if (!G) return MCPError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		Targets.Add(G);
	}
	else
	{
		Blueprint->GetAllGraphs(Targets);
	}

	FString Combined;
	TArray<TSharedPtr<FJsonValue>> GraphArr;
	for (UEdGraph* G : Targets)
	{
		if (!G) continue;
		// Classify the graph for the header line.
		FString Kind = TEXT("Graph");
		if (Blueprint->UbergraphPages.Contains(G)) Kind = TEXT("EventGraph");
		else if (Blueprint->FunctionGraphs.Contains(G)) Kind = TEXT("Function");
		else if (Blueprint->MacroGraphs.Contains(G)) Kind = TEXT("Macro");

		const FString Text = DescribeGraphAsText(G);
		Combined += FString::Printf(TEXT("=== %s: %s ===\n%s\n"), *Kind, *G->GetName(), *Text);

		TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
		GObj->SetStringField(TEXT("name"), G->GetName());
		GObj->SetStringField(TEXT("kind"), Kind);
		GObj->SetStringField(TEXT("text"), Text);
		GObj->SetNumberField(TEXT("nodeCount"), G->Nodes.Num());
		GraphArr.Add(MakeShared<FJsonValueObject>(GObj));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("text"), Combined);
	Result->SetArrayField(TEXT("graphs"), GraphArr);
	Result->SetNumberField(TEXT("graphCount"), GraphArr.Num());
	return MCPResult(Result);
}
