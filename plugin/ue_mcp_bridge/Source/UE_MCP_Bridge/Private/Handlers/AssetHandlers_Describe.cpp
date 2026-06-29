// Universal agent-readable asset summary. One entry point (describe_asset) that
// auto-detects the asset class and renders a compact text digest tuned for an
// LLM to read before acting:
//   Blueprint     - parent class, components, variables, per-graph pseudocode,
//                   and simple performance smells (logic on Event Tick).
//   BehaviorTree  - blackboard + indented tree of composites/tasks with their
//                   decorators and services.
//   Material(Inst)- domain/blend/shading + parameter inventory + expression count.
//   (fallback)    - class name + a handful of editable properties.

#include "AssetHandlers.h"
#include "BlueprintHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString BTNodeName(const UBTNode* Node)
	{
		if (!Node) return TEXT("(null)");
		const FString Name = Node->GetNodeName();
		return Name.IsEmpty() ? Node->GetClass()->GetName() : Name;
	}

	void DescribeBTNode(UBTNode* Node, FString& Out, int32 Level, TSet<UBTNode*>& Visited)
	{
		if (!Node || Visited.Contains(Node)) return;
		Visited.Add(Node);

		const FString Pad = FString::ChrN(Level * 2, TEXT(' '));
		Out += FString::Printf(TEXT("%s- %s\n"), *Pad, *BTNodeName(Node));

		UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node);
		if (!Composite) return;

		// Services live on the composite itself.
		for (const UBTService* Service : Composite->Services)
		{
			if (Service) Out += FString::Printf(TEXT("%s  [Service] %s\n"), *Pad, *BTNodeName(Service));
		}

		const int32 NumChildren = Composite->GetChildrenNum();
		for (int32 i = 0; i < NumChildren; ++i)
		{
			const FBTCompositeChild& Child = Composite->Children[i];
			for (const UBTDecorator* Dec : Child.Decorators)
			{
				if (Dec) Out += FString::Printf(TEXT("%s  [Decorator] %s\n"), *Pad, *BTNodeName(Dec));
			}
			UBTNode* ChildNode = Child.ChildComposite
				? static_cast<UBTNode*>(Child.ChildComposite)
				: static_cast<UBTNode*>(Child.ChildTask);
			DescribeBTNode(ChildNode, Out, Level + 1, Visited);
		}
	}

	FString DescribeBehaviorTree(UBehaviorTree* BT)
	{
		FString Out;
		Out += FString::Printf(TEXT("Behavior Tree: %s\n"), *BT->GetName());
		Out += FString::Printf(TEXT("Blackboard: %s\n\n"),
			BT->BlackboardAsset ? *BT->BlackboardAsset->GetName() : TEXT("None"));
		Out += TEXT("Tree:\n");
		TSet<UBTNode*> Visited;
		if (BT->RootNode)
		{
			DescribeBTNode(BT->RootNode, Out, 0, Visited);
		}
		else
		{
			Out += TEXT("  (empty - no root node)\n");
		}
		return Out;
	}

	FString DescribeMaterial(UMaterialInterface* Mat)
	{
		FString Out;
		Out += FString::Printf(TEXT("Material: %s\n"), *Mat->GetName());

		auto AppendParams = [&Out, Mat](const TCHAR* Label, auto Getter)
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Getter(Infos, Ids);
			if (Infos.Num() == 0) return;
			TArray<FString> Names;
			for (const FMaterialParameterInfo& Info : Infos) Names.Add(Info.Name.ToString());
			Out += FString::Printf(TEXT("%s (%d): %s\n"), Label, Names.Num(), *FString::Join(Names, TEXT(", ")));
		};

		AppendParams(TEXT("Scalar params"),  [Mat](TArray<FMaterialParameterInfo>& I, TArray<FGuid>& G){ Mat->GetAllScalarParameterInfo(I, G); });
		AppendParams(TEXT("Vector params"),  [Mat](TArray<FMaterialParameterInfo>& I, TArray<FGuid>& G){ Mat->GetAllVectorParameterInfo(I, G); });
		AppendParams(TEXT("Texture params"), [Mat](TArray<FMaterialParameterInfo>& I, TArray<FGuid>& G){ Mat->GetAllTextureParameterInfo(I, G); });

		Out += FString::Printf(TEXT("Blend mode: %d\n"), (int32)Mat->GetBlendMode());
		if (UMaterial* BaseMat = Cast<UMaterial>(Mat))
		{
			Out += FString::Printf(TEXT("Expressions: %d\n"), BaseMat->GetExpressions().Num());
		}
		return Out;
	}

	FString DescribeBlueprintAsset(UBlueprint* BP)
	{
		FString Out;
		Out += FString::Printf(TEXT("Blueprint: %s\n"), *BP->GetName());
		if (BP->ParentClass)
		{
			Out += FString::Printf(TEXT("Parent: %s\n"), *BP->ParentClass->GetName());
		}

		// Components from the SimpleConstructionScript.
		if (BP->SimpleConstructionScript)
		{
			TArray<FString> Comps;
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass)
				{
					Comps.Add(FString::Printf(TEXT("%s (%s)"),
						*Node->GetVariableName().ToString(), *Node->ComponentClass->GetName()));
				}
			}
			if (Comps.Num() > 0)
			{
				Out += FString::Printf(TEXT("Components: %s\n"), *FString::Join(Comps, TEXT(", ")));
			}
		}

		// Member variables.
		if (BP->NewVariables.Num() > 0)
		{
			TArray<FString> Vars;
			for (const FBPVariableDescription& Var : BP->NewVariables)
			{
				Vars.Add(Var.VarName.ToString());
			}
			Out += FString::Printf(TEXT("Variables (%d): %s\n"), Vars.Num(), *FString::Join(Vars, TEXT(", ")));
		}

		// Performance smell: real logic wired off Event Tick.
		bool bTickUsed = false;
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			for (UEdGraphNode* Node : G->Nodes)
			{
				UK2Node_Event* Evt = Cast<UK2Node_Event>(Node);
				if (!Evt) continue;
				if (Evt->EventReference.GetMemberName() == TEXT("ReceiveTick"))
				{
					for (UEdGraphPin* Pin : Evt->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output &&
							Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
							Pin->LinkedTo.Num() > 0)
						{
							bTickUsed = true;
						}
					}
				}
			}
		}
		if (bTickUsed)
		{
			Out += TEXT("\n--- POTENTIAL PERFORMANCE ISSUES ---\nLogic runs on Event Tick (per-frame). Consider timers or events.\n");
		}

		// Full per-graph pseudocode.
		Out += TEXT("\n");
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			FString Kind = TEXT("Graph");
			if (BP->UbergraphPages.Contains(G)) Kind = TEXT("EventGraph");
			else if (BP->FunctionGraphs.Contains(G)) Kind = TEXT("Function");
			else if (BP->MacroGraphs.Contains(G)) Kind = TEXT("Macro");
			Out += FString::Printf(TEXT("=== %s: %s ===\n%s\n"),
				*Kind, *G->GetName(), *FBlueprintHandlers::DescribeGraphAsText(G));
		}
		return Out;
	}
}

TSharedPtr<FJsonValue> FAssetHandlers::DescribeAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;

	UObject* Asset = LoadAssetByPath<UObject>(AssetPath);
	if (!Asset) return MCPError(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

	FString Text;
	FString Kind;
	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		Kind = TEXT("Blueprint");
		Text = DescribeBlueprintAsset(BP);
	}
	else if (UBehaviorTree* BT = Cast<UBehaviorTree>(Asset))
	{
		Kind = TEXT("BehaviorTree");
		Text = DescribeBehaviorTree(BT);
	}
	else if (UMaterialInterface* Mat = Cast<UMaterialInterface>(Asset))
	{
		Kind = TEXT("Material");
		Text = DescribeMaterial(Mat);
	}
	else
	{
		Kind = Asset->GetClass()->GetName();
		Text = FString::Printf(TEXT("%s: %s\n(no specialized describer; use reflect_class or asset read for details)\n"),
			*Kind, *Asset->GetName());
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("assetType"), Kind);
	Result->SetStringField(TEXT("text"), Text);
	return MCPResult(Result);
}
