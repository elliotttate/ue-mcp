// Pseudocode -> Blueprint graph compiler.
//
// author_blueprint_logic parses a constrained C++-style block and emits a wired
// EventGraph. The grammar is deliberately small but covers the bread-and-butter
// of event-graph authoring, which otherwise costs many node-by-node calls:
//
//   event OnBeginPlay() { ... }           // engine event (ReceiveBeginPlay)
//   event OnTick() { ... }                // engine event; DeltaSeconds bound
//   event MyEvent(float Amount) { ... }   // custom event; params -> bindings
//
//   Inside a body:
//     UClass::Func(args);                  // static / library call (exec-chained)
//     Target->Func(args);                  // member call (Target wired to self)
//     Func(args);                          // call resolved on self / libraries
//     Type Name = Call(args);              // bind Name to the call's return pin
//     ExistingVar = Expr;                  // Set on a Blueprint member variable
//     if (BoolExpr) { ... } else { ... }   // Branch (K2Node_IfThenElse)
//
//   Args resolve to: number / "string" / true|false / nullptr literals,
//   SelfActor, DeltaSeconds, a prior binding name, or a nested call.
//
// Not yet supported (use node-by-node + describe_graph, or wrap as events):
//   loops, switch, casts, timelines, sequence, struct make/break, return values.
// Unsupported constructs are reported in `warnings` rather than failing the run.

#include "BlueprintHandlers.h"
#include "BlueprintHandlers_Internal.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetTextLibrary.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Remembers the node GUIDs created by the last author/insert run per Blueprint,
// so undo_last_authored can wipe exactly what was generated.
static TMap<FString, TArray<FGuid>> GLastAuthoredNodes;

namespace
{
	// ── Tokenizing helpers (brace/paren/string aware) ──────────────────────
	bool IsIdentChar(TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); }

	// Index of the first top-level occurrence of Ch (depth 0, outside strings).
	int32 FindTopLevelChar(const FString& S, TCHAR Ch, int32 Start = 0)
	{
		int32 Depth = 0; bool bInStr = false; TCHAR StrCh = 0;
		for (int32 i = Start; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			if (bInStr) { if (C == StrCh && S[i - 1] != TEXT('\\')) bInStr = false; continue; }
			if (C == TEXT('"') || C == TEXT('\'')) { bInStr = true; StrCh = C; continue; }
			// Match BEFORE the depth bump so an opening bracket ('{' / '(') we are
			// searching for at depth 0 is returned rather than swallowed as an opener.
			if (Depth == 0 && C == Ch) return i;
			if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{')) Depth++;
			else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}')) Depth--;
		}
		return INDEX_NONE;
	}

	// Given an opening bracket at OpenIdx, return the index of its match.
	int32 FindMatching(const FString& S, int32 OpenIdx)
	{
		const TCHAR Open = S[OpenIdx];
		const TCHAR Close = Open == TEXT('(') ? TEXT(')') : Open == TEXT('{') ? TEXT('}') : TEXT(']');
		int32 Depth = 0; bool bInStr = false; TCHAR StrCh = 0;
		for (int32 i = OpenIdx; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			if (bInStr) { if (C == StrCh && S[i - 1] != TEXT('\\')) bInStr = false; continue; }
			if (C == TEXT('"') || C == TEXT('\'')) { bInStr = true; StrCh = C; continue; }
			if (C == Open) Depth++;
			else if (C == Close) { if (--Depth == 0) return i; }
		}
		return INDEX_NONE;
	}

	// Split on top-level commas (function argument lists).
	TArray<FString> SplitArgs(const FString& S)
	{
		TArray<FString> Out;
		int32 Depth = 0; bool bInStr = false; TCHAR StrCh = 0; int32 Last = 0;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			if (bInStr) { if (C == StrCh && S[i - 1] != TEXT('\\')) bInStr = false; continue; }
			if (C == TEXT('"') || C == TEXT('\'')) { bInStr = true; StrCh = C; continue; }
			if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{')) Depth++;
			else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}')) Depth--;
			else if (Depth == 0 && C == TEXT(',')) { Out.Add(S.Mid(Last, i - Last).TrimStartAndEnd()); Last = i + 1; }
		}
		const FString Tail = S.Mid(Last).TrimStartAndEnd();
		if (!Tail.IsEmpty()) Out.Add(Tail);
		return Out;
	}

	// Top-level assignment '=' (skips ==, !=, >=, <=).
	int32 FindAssignment(const FString& S)
	{
		int32 Depth = 0; bool bInStr = false; TCHAR StrCh = 0;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			if (bInStr) { if (C == StrCh && S[i - 1] != TEXT('\\')) bInStr = false; continue; }
			if (C == TEXT('"') || C == TEXT('\'')) { bInStr = true; StrCh = C; continue; }
			if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{')) Depth++;
			else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}')) Depth--;
			else if (Depth == 0 && C == TEXT('='))
			{
				const TCHAR Prev = i > 0 ? S[i - 1] : 0;
				const TCHAR Next = i + 1 < S.Len() ? S[i + 1] : 0;
				if (Next != TEXT('=') && Prev != TEXT('=') && Prev != TEXT('!') && Prev != TEXT('<') && Prev != TEXT('>'))
					return i;
			}
		}
		return INDEX_NONE;
	}

	bool KeywordAt(const FString& S, int32 Pos, const TCHAR* Word)
	{
		const FString W = Word;
		if (Pos + W.Len() > S.Len()) return false;
		if (S.Mid(Pos, W.Len()) != W) return false;
		const TCHAR After = Pos + W.Len() < S.Len() ? S[Pos + W.Len()] : 0;
		return !IsIdentChar(After);
	}

	// A resolved value: an output pin to wire, or a literal default string.
	struct FAuthoredValue
	{
		UEdGraphPin* Pin = nullptr;
		FString Literal;
		bool bLiteral = false;
		static FAuthoredValue MakeLit(const FString& L) { FAuthoredValue V; V.bLiteral = true; V.Literal = L; return V; }
		static FAuthoredValue MakePin(UEdGraphPin* P) { FAuthoredValue V; V.Pin = P; return V; }
		bool IsValid() const { return Pin != nullptr || bLiteral; }
	};

	// Per-graph compile session: holds the exec cursor, name bindings, layout
	// cursor, created-node list, and warnings.
	struct FAuthorSession
	{
		UBlueprint* BP = nullptr;
		UEdGraph* Graph = nullptr;
		const UEdGraphSchema_K2* Schema = nullptr;
		UEdGraphPin* CurrentExec = nullptr;        // exec-out pin to chain the next statement onto
		TMap<FString, FAuthoredValue> Bindings;    // local name -> value
		UK2Node_Self* SelfNode = nullptr;
		TArray<FGuid>* Created = nullptr;
		TArray<FString>* Warnings = nullptr;
		int32 LayoutX = 0;
		int32 LayoutY = 0;

		void Warn(const FString& W) { if (Warnings) Warnings->Add(W); }

		// Finalize a freshly NewObject'd node: place, guid, allocate pins.
		void Finalize(UEdGraphNode* Node, bool bReconstruct = true)
		{
			Graph->Modify();
			Graph->AddNode(Node, false, false);
			Node->CreateNewGuid();
			Node->NodePosX = LayoutX;
			Node->NodePosY = LayoutY;
			LayoutX += 320;
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			if (bReconstruct)
			{
				if (UK2Node* K2 = Cast<UK2Node>(Node)) K2->ReconstructNode();
			}
			if (Created) Created->Add(Node->NodeGuid);
		}

		UK2Node_Self* GetSelfNode()
		{
			if (!SelfNode)
			{
				SelfNode = NewObject<UK2Node_Self>(Graph);
				Finalize(SelfNode, false);
			}
			return SelfNode;
		}

		// Wire CurrentExec -> InExecPin and advance CurrentExec to OutExecPin.
		void ChainExec(UEdGraphPin* InExec, UEdGraphPin* OutExec)
		{
			if (CurrentExec && InExec) Schema->TryCreateConnection(CurrentExec, InExec);
			CurrentExec = OutExec;
		}

		// Resolve a callee function by class + name, mirroring add_node's search order.
		UFunction* ResolveFunction(const FString& ClassName, const FString& FuncName)
		{
			if (FuncName.IsEmpty()) return nullptr;
			const FName FN(*FuncName);
			if (!ClassName.IsEmpty())
			{
				UClass* C = FindClassByShortName(ClassName);
				if (!C) C = LoadObject<UClass>(nullptr, *ClassName);
				if (C) { if (UFunction* F = C->FindFunctionByName(FN)) return F; }
			}
			if (BP->ParentClass) { if (UFunction* F = BP->ParentClass->FindFunctionByName(FN)) return F; }
			if (BP->GeneratedClass) { if (UFunction* F = BP->GeneratedClass->FindFunctionByName(FN)) return F; }
			static UClass* Libs[] = {
				UGameplayStatics::StaticClass(), UKismetSystemLibrary::StaticClass(),
				UKismetMathLibrary::StaticClass(), UKismetStringLibrary::StaticClass(),
				UKismetArrayLibrary::StaticClass(), UKismetTextLibrary::StaticClass(),
			};
			for (UClass* L : Libs) { if (UFunction* F = L->FindFunctionByName(FN)) return F; }
			// Unique BlueprintCallable match across loaded classes.
			UFunction* Unique = nullptr; int32 N = 0;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UFunction* Cand = It->FindFunctionByName(FN, EIncludeSuperFlag::ExcludeSuper);
				if (Cand && Cand->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
				{
					if (Cand != Unique) { Unique = Cand; if (++N > 1) break; }
				}
			}
			return N == 1 ? Unique : nullptr;
		}

		void SetPinLiteral(UEdGraphPin* Pin, const FString& Raw)
		{
			FString V = Raw.TrimStartAndEnd();
			if (V.StartsWith(TEXT("\"")) && V.EndsWith(TEXT("\"")) && V.Len() >= 2) V = V.Mid(1, V.Len() - 2);
			else if (V == TEXT("nullptr")) V = TEXT("None");
			Schema->TrySetDefaultValue(*Pin, V);
		}

		void ApplyValueToPin(const FAuthoredValue& Val, UEdGraphPin* Pin)
		{
			if (!Pin) return;
			if (Val.Pin) Schema->TryCreateConnection(Val.Pin, Pin);
			else if (Val.bLiteral) SetPinLiteral(Pin, Val.Literal);
		}

		// Evaluate an expression to a value (pin or literal).
		FAuthoredValue EvalExpr(const FString& InExpr)
		{
			FString E = InExpr.TrimStartAndEnd();
			if (E.IsEmpty()) return FAuthoredValue();

			// Bindings / specials first.
			if (FAuthoredValue* Found = Bindings.Find(E)) return *Found;
			if (E == TEXT("SelfActor") || E == TEXT("self") || E == TEXT("this"))
			{
				UK2Node_Self* S = GetSelfNode();
				UEdGraphPin* SelfOut = nullptr;
				for (UEdGraphPin* P : S->Pins) { if (P && P->Direction == EGPD_Output) { SelfOut = P; break; } }
				return FAuthoredValue::MakePin(SelfOut);
			}

			// Literals.
			if (E == TEXT("true") || E == TEXT("false") || E == TEXT("nullptr")) return FAuthoredValue::MakeLit(E);
			if (E.StartsWith(TEXT("\""))) return FAuthoredValue::MakeLit(E);
			{
				bool bNumeric = true;
				for (int32 i = 0; i < E.Len(); ++i)
				{
					const TCHAR C = E[i];
					if (!(FChar::IsDigit(C) || C == TEXT('.') || C == TEXT('-') || C == TEXT('+') || C == TEXT('f'))) { bNumeric = false; break; }
				}
				if (bNumeric) { FString Num = E; Num.RemoveFromEnd(TEXT("f")); return FAuthoredValue::MakeLit(Num); }
			}

			// Call expression.
			if (FindTopLevelChar(E, TEXT('(')) != INDEX_NONE) return EvalCall(E);

			// Bare token: treat as a literal (enum value / unknown name).
			return FAuthoredValue::MakeLit(E);
		}

		// Build a CallFunction node from "Class::Func(args)" / "Target->Func(args)" / "Func(args)".
		FAuthoredValue EvalCall(const FString& Expr)
		{
			const int32 Open = FindTopLevelChar(Expr, TEXT('('));
			const int32 Close = Open != INDEX_NONE ? FindMatching(Expr, Open) : INDEX_NONE;
			if (Open == INDEX_NONE || Close == INDEX_NONE) { Warn(FString::Printf(TEXT("unparseable call: %s"), *Expr)); return FAuthoredValue(); }

			FString Callee = Expr.Left(Open).TrimStartAndEnd();
			const FString ArgsStr = Expr.Mid(Open + 1, Close - Open - 1);

			FString ClassName, TargetExpr, FuncName;
			int32 Sep;
			if ((Sep = Callee.Find(TEXT("::"))) != INDEX_NONE)
			{
				ClassName = Callee.Left(Sep).TrimStartAndEnd();
				FuncName = Callee.Mid(Sep + 2).TrimStartAndEnd();
			}
			else if ((Sep = Callee.Find(TEXT("->"))) != INDEX_NONE)
			{
				TargetExpr = Callee.Left(Sep).TrimStartAndEnd();
				FuncName = Callee.Mid(Sep + 2).TrimStartAndEnd();
			}
			else
			{
				FuncName = Callee;
			}

			// Resolve the function. For member calls try the target's pin class.
			UClass* TargetClass = nullptr;
			FAuthoredValue TargetVal;
			if (!TargetExpr.IsEmpty())
			{
				TargetVal = EvalExpr(TargetExpr);
				if (TargetVal.Pin && TargetVal.Pin->PinType.PinSubCategoryObject.IsValid())
				{
					TargetClass = Cast<UClass>(TargetVal.Pin->PinType.PinSubCategoryObject.Get());
				}
				if ((TargetExpr == TEXT("SelfActor") || TargetExpr == TEXT("self")) && BP->ParentClass)
				{
					TargetClass = BP->ParentClass;
				}
			}
			UFunction* Func = ResolveFunction(TargetClass ? TargetClass->GetPathName() : ClassName, FuncName);
			if (!Func)
			{
				Warn(FString::Printf(TEXT("function not found: %s"), *Callee));
				return FAuthoredValue();
			}

			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
			Node->SetFromFunction(Func);
			Finalize(Node);

			// Exec chaining (impure functions only — pure calls have no exec pins).
			UEdGraphPin* ExecIn = Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* ExecOut = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			if (ExecIn && ExecOut) ChainExec(ExecIn, ExecOut);

			// Self / target wiring.
			if (UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input))
			{
				if (!TargetExpr.IsEmpty()) ApplyValueToPin(TargetVal, SelfPin);
				// bare/static call: leave self pin to its implicit-self default.
			}

			// Argument wiring: positional over visible input data pins.
			TArray<UEdGraphPin*> InputPins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
				InputPins.Add(Pin);
			}
			const TArray<FString> Args = ArgsStr.TrimStartAndEnd().IsEmpty() ? TArray<FString>() : SplitArgs(ArgsStr);
			for (int32 i = 0; i < Args.Num(); ++i)
			{
				if (i >= InputPins.Num()) { Warn(FString::Printf(TEXT("%s: more args than input pins"), *FuncName)); break; }
				ApplyValueToPin(EvalExpr(Args[i]), InputPins[i]);
			}

			return FAuthoredValue::MakePin(Node->GetReturnValuePin());
		}

		// One simple (non-control-flow) statement.
		void BuildSimple(const FString& Stmt)
		{
			const FString S = Stmt.TrimStartAndEnd();
			if (S.IsEmpty()) return;

			const int32 Eq = FindAssignment(S);
			if (Eq != INDEX_NONE)
			{
				const FString Lhs = S.Left(Eq).TrimStartAndEnd();
				const FString Rhs = S.Mid(Eq + 1).TrimStartAndEnd();

				TArray<FString> LhsTokens;
				Lhs.ParseIntoArrayWS(LhsTokens);
				const FString Name = LhsTokens.Num() > 0 ? LhsTokens.Last() : Lhs;

				// Assignment to an existing member variable -> VariableSet node.
				const bool bDeclared = LhsTokens.Num() >= 2; // had a type -> new local binding
				const bool bIsMemberVar = !bDeclared && BP->GeneratedClass &&
					BP->GeneratedClass->FindPropertyByName(FName(*Name)) != nullptr;

				if (bIsMemberVar)
				{
					UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
					SetNode->VariableReference.SetSelfMember(FName(*Name));
					Finalize(SetNode);
					UEdGraphPin* ExecIn = SetNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
					UEdGraphPin* ExecOut = SetNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
					if (ExecIn && ExecOut) ChainExec(ExecIn, ExecOut);
					// The value input pin is named after the variable.
					if (UEdGraphPin* ValPin = SetNode->FindPin(FName(*Name), EGPD_Input))
					{
						ApplyValueToPin(EvalExpr(Rhs), ValPin);
					}
				}
				else
				{
					// Local SSA binding: Name now refers to the RHS value.
					Bindings.Add(Name, EvalExpr(Rhs));
				}
				return;
			}

			// Bare call statement.
			EvalCall(S);
		}

		// if (cond) { then } [else { else }]  ->  Branch. Returns body index past it.
		int32 BuildIf(const FString& Body, int32 Pos)
		{
			const int32 POpen = FindTopLevelChar(Body, TEXT('('), Pos);
			const int32 PClose = POpen != INDEX_NONE ? FindMatching(Body, POpen) : INDEX_NONE;
			if (POpen == INDEX_NONE || PClose == INDEX_NONE) { Warn(TEXT("malformed if(...)")); return Body.Len(); }
			const FString Cond = Body.Mid(POpen + 1, PClose - POpen - 1);

			// Start past the condition's ')' — scanning from the ')' itself would
			// drive the depth counter negative and hide the then-block '{'.
			const int32 BOpen = FindTopLevelChar(Body, TEXT('{'), PClose + 1);
			const int32 BClose = BOpen != INDEX_NONE ? FindMatching(Body, BOpen) : INDEX_NONE;
			if (BOpen == INDEX_NONE || BClose == INDEX_NONE) { Warn(TEXT("malformed if-block")); return Body.Len(); }
			const FString ThenBlock = Body.Mid(BOpen + 1, BClose - BOpen - 1);

			// Optional else.
			FString ElseBlock;
			int32 After = BClose + 1;
			int32 Scan = After;
			while (Scan < Body.Len() && FChar::IsWhitespace(Body[Scan])) Scan++;
			if (KeywordAt(Body, Scan, TEXT("else")))
			{
				const int32 EOpen = FindTopLevelChar(Body, TEXT('{'), Scan);
				const int32 EClose = EOpen != INDEX_NONE ? FindMatching(Body, EOpen) : INDEX_NONE;
				if (EOpen != INDEX_NONE && EClose != INDEX_NONE)
				{
					ElseBlock = Body.Mid(EOpen + 1, EClose - EOpen - 1);
					After = EClose + 1;
				}
			}

			UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
			Finalize(Branch);
			UEdGraphPin* ExecIn = Branch->GetExecPin();
			if (ExecIn && CurrentExec) Schema->TryCreateConnection(CurrentExec, ExecIn);
			if (UEdGraphPin* CondPin = Branch->GetConditionPin())
			{
				ApplyValueToPin(EvalExpr(Cond), CondPin);
			}

			UEdGraphPin* ThenPin = Branch->GetThenPin();
			UEdGraphPin* ElsePin = Branch->GetElsePin();
			const int32 SavedX = LayoutX;

			CurrentExec = ThenPin;
			LayoutY += 220;
			BuildBlock(ThenBlock);

			if (!ElseBlock.IsEmpty())
			{
				CurrentExec = ElsePin;
				LayoutX = SavedX;
				LayoutY += 220;
				BuildBlock(ElseBlock);
			}

			// Both arms terminate independently; no merge pin exists on Branch.
			CurrentExec = nullptr;
			LayoutY += 220;
			return After;
		}

		// Process a body, statement by statement.
		void BuildBlock(const FString& Body)
		{
			int32 i = 0; const int32 N = Body.Len();
			while (i < N)
			{
				while (i < N && (FChar::IsWhitespace(Body[i]) || Body[i] == TEXT(';'))) i++;
				if (i >= N) break;

				if (KeywordAt(Body, i, TEXT("if"))) { i = BuildIf(Body, i); continue; }

				if (KeywordAt(Body, i, TEXT("return")))
				{
					const int32 Semi = FindTopLevelChar(Body, TEXT(';'), i);
					// Return values aren't wired in v1; evaluate for side effects only.
					const FString Expr = Body.Mid(i + 6, (Semi == INDEX_NONE ? N : Semi) - (i + 6)).TrimStartAndEnd();
					if (!Expr.IsEmpty()) EvalExpr(Expr);
					Warn(TEXT("'return' value is not wired to a result node in v1"));
					i = Semi == INDEX_NONE ? N : Semi + 1;
					continue;
				}

				const int32 Semi = FindTopLevelChar(Body, TEXT(';'), i);
				const FString Stmt = Body.Mid(i, (Semi == INDEX_NONE ? N : Semi) - i);
				BuildSimple(Stmt);
				i = Semi == INDEX_NONE ? N : Semi + 1;
			}
		}
	};

	// Find an existing engine-event node (e.g. ReceiveBeginPlay) in the ubergraph.
	UK2Node_Event* FindEventNode(UEdGraph* Graph, const FName MemberName)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
			{
				if (Evt->EventReference.GetMemberName() == MemberName) return Evt;
			}
		}
		return nullptr;
	}

	// One top-level definition parsed from the code block.
	struct FDefinition
	{
		FString Header;   // text before the body's '{'
		FString Body;     // inside the braces
	};

	TArray<FDefinition> ParseDefinitions(const FString& Code)
	{
		TArray<FDefinition> Defs;
		int32 i = 0; const int32 N = Code.Len();
		while (i < N)
		{
			const int32 Brace = FindTopLevelChar(Code, TEXT('{'), i);
			if (Brace == INDEX_NONE) break;
			const int32 Close = FindMatching(Code, Brace);
			if (Close == INDEX_NONE) break;
			FDefinition Def;
			Def.Header = Code.Mid(i, Brace - i).TrimStartAndEnd();
			Def.Body = Code.Mid(Brace + 1, Close - Brace - 1);
			Defs.Add(Def);
			i = Close + 1;
		}
		return Defs;
	}
}

// ── author_blueprint_logic ─────────────────────────────────────────────
TSharedPtr<FJsonValue> FBlueprintHandlers::AuthorLogic(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString Code;
	if (auto Err = RequireString(Params, TEXT("code"), Code)) return Err;

	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP) return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	if (BP->UbergraphPages.Num() == 0) return MCPError(TEXT("Blueprint has no EventGraph"));
	UEdGraph* EventGraph = BP->UbergraphPages[0];
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	TArray<FGuid> Created;
	TArray<FString> Warnings;
	int32 LayoutY = 0;
	int32 EventsBuilt = 0;

	for (const FDefinition& Def : ParseDefinitions(Code))
	{
		// Split the header into the part before '(' (keywords + name) and params.
		const int32 POpen = FindTopLevelChar(Def.Header, TEXT('('));
		const int32 PClose = POpen != INDEX_NONE ? FindMatching(Def.Header, POpen) : INDEX_NONE;
		FString Decl = POpen != INDEX_NONE ? Def.Header.Left(POpen) : Def.Header;
		const FString ParamStr = (POpen != INDEX_NONE && PClose != INDEX_NONE) ? Def.Header.Mid(POpen + 1, PClose - POpen - 1) : FString();

		TArray<FString> DeclTokens;
		Decl.TrimStartAndEnd().ParseIntoArrayWS(DeclTokens);
		if (DeclTokens.Num() == 0) { Warnings.Add(TEXT("empty definition header")); continue; }
		const bool bCustomKeyword = DeclTokens[0] == TEXT("event");
		const FString Name = DeclTokens.Last();

		FAuthorSession Session;
		Session.BP = BP;
		Session.Graph = EventGraph;
		Session.Schema = Schema;
		Session.Created = &Created;
		Session.Warnings = &Warnings;
		Session.LayoutY = LayoutY;

		// Map common headers to engine events; everything else becomes a custom event.
		FName EngineEvent = NAME_None;
		if (Name == TEXT("OnBeginPlay") || Name == TEXT("BeginPlay")) EngineEvent = TEXT("ReceiveBeginPlay");
		else if (Name == TEXT("OnTick") || Name == TEXT("Tick")) EngineEvent = TEXT("ReceiveTick");

		if (EngineEvent != NAME_None)
		{
			UK2Node_Event* EventNode = FindEventNode(EventGraph, EngineEvent);
			if (!EventNode)
			{
				EventNode = NewObject<UK2Node_Event>(EventGraph);
				UClass* EvtClass = BP->ParentClass.Get();
				if (!EvtClass) EvtClass = AActor::StaticClass();
				if (UFunction* EvtFunc = EvtClass->FindFunctionByName(EngineEvent))
				{
					EventNode->EventReference.SetFromField<UFunction>(EvtFunc, true);
				}
				EventNode->bOverrideFunction = true;
				Session.Finalize(EventNode);
			}
			Session.CurrentExec = EventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			if (EngineEvent == TEXT("ReceiveTick"))
			{
				if (UEdGraphPin* DeltaPin = EventNode->FindPin(TEXT("DeltaSeconds"), EGPD_Output))
				{
					Session.Bindings.Add(TEXT("DeltaSeconds"), FAuthoredValue::MakePin(DeltaPin));
				}
			}
			Session.LayoutX = EventNode->NodePosX + 320;
			Session.LayoutY = EventNode->NodePosY;
		}
		else
		{
			// Custom event (covers `event Name(...)` and any `void Name(...)`).
			if (!bCustomKeyword) Warnings.Add(FString::Printf(TEXT("'%s' authored as a Custom Event (function-graph definitions are not supported in v1)"), *Name));
			UK2Node_CustomEvent* Custom = NewObject<UK2Node_CustomEvent>(EventGraph);
			Custom->CustomFunctionName = FName(*Name);
			Session.LayoutY = LayoutY;
			Session.Finalize(Custom);
			// Declared params become typed output pins, bound by name.
			if (!ParamStr.TrimStartAndEnd().IsEmpty())
			{
				for (const FString& Param : SplitArgs(ParamStr))
				{
					TArray<FString> PT; Param.TrimStartAndEnd().ParseIntoArrayWS(PT);
					if (PT.Num() < 2) continue;
					const FString PName = PT.Last();
					FString PType; for (int32 k = 0; k < PT.Num() - 1; ++k) { PType += (k ? TEXT(" ") : TEXT("")) + PT[k]; }
					FEdGraphPinType PinType = FBlueprintHandlers::MakePinType(PType);
					if (PinType.PinCategory.IsNone()) { Warnings.Add(FString::Printf(TEXT("param '%s': unresolved type '%s'"), *PName, *PType)); continue; }
					Custom->CreateUserDefinedPin(FName(*PName), PinType, EGPD_Output, false);
				}
				Custom->ReconstructNode();
				for (UEdGraphPin* Pin : Custom->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						Session.Bindings.Add(Pin->PinName.ToString(), FAuthoredValue::MakePin(Pin));
					}
				}
			}
			Session.CurrentExec = Custom->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			Session.LayoutX = Custom->NodePosX + 320;
		}

		Session.BuildBlock(Def.Body);
		EventsBuilt++;
		LayoutY = Session.LayoutY + 320;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	SaveAssetPackage(BP);

	GLastAuthoredNodes.Add(BP->GetPathName(), Created);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
	Result->SetNumberField(TEXT("definitions"), EventsBuilt);
	Result->SetNumberField(TEXT("nodesCreated"), Created.Num());
	TArray<TSharedPtr<FJsonValue>> Guids;
	for (const FGuid& G : Created) Guids.Add(MakeShared<FJsonValueString>(G.ToString()));
	Result->SetArrayField(TEXT("nodeGuids"), Guids);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> W;
		for (const FString& S : Warnings) W.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("warnings"), W);
	}
	// Inline wiring verification: resolve the created guids back to nodes and
	// report their connections, so callers skip the read_graph round-trip.
	{
		TSet<FGuid> CreatedSet(Created);
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		TArray<UEdGraphNode*> CreatedNodes;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && CreatedSet.Contains(Node->NodeGuid)) CreatedNodes.Add(Node);
			}
		}
		if (CreatedNodes.Num() > 0)
		{
			Result->SetObjectField(TEXT("report"), BuildCompactConnectionReport(CreatedNodes));
		}
	}
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("assetPath"), BP->GetPathName());
	MCPSetRollback(Result, TEXT("undo_last_authored"), Payload);
	return MCPResult(Result);
}

// ── insert_blueprint_logic ─────────────────────────────────────────────
// Append a body of statements after an existing node, splicing into its exec
// chain (afterNode.Then -> new chain -> old downstream).
TSharedPtr<FJsonValue> FBlueprintHandlers::InsertLogic(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	FString Code;
	if (auto Err = RequireString(Params, TEXT("code"), Code)) return Err;
	FString AfterNodeId;
	if (auto Err = RequireString(Params, TEXT("afterNodeId"), AfterNodeId)) return Err;

	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP) return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	const FString GraphName = OptionalString(Params, TEXT("graphName"), TEXT("EventGraph"));
	UEdGraph* Graph = FindGraph(BP, GraphName);
	if (!Graph) return MCPError(FString::Printf(TEXT("Graph not found: %s"), *GraphName));

	UEdGraphNode* AfterNode = FindNodeByGuidOrName(Graph, AfterNodeId);
	if (!AfterNode) return MCPError(FString::Printf(TEXT("Node not found: %s"), *AfterNodeId));

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* ThenPin = AfterNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	if (!ThenPin) return MCPError(TEXT("Anchor node has no Then exec pin to insert after"));

	// Detach the existing downstream so we can splice the new chain in front of it.
	UEdGraphPin* DownstreamExec = ThenPin->LinkedTo.Num() > 0 ? ThenPin->LinkedTo[0] : nullptr;
	ThenPin->BreakAllPinLinks();

	TArray<FGuid> Created;
	TArray<FString> Warnings;
	FAuthorSession Session;
	Session.BP = BP;
	Session.Graph = Graph;
	Session.Schema = Schema;
	Session.Created = &Created;
	Session.Warnings = &Warnings;
	Session.CurrentExec = ThenPin;
	Session.LayoutX = AfterNode->NodePosX + 320;
	Session.LayoutY = AfterNode->NodePosY + 200;
	Session.BuildBlock(Code);

	// Reconnect the old downstream onto the tail of the inserted chain.
	if (DownstreamExec && Session.CurrentExec)
	{
		Schema->TryCreateConnection(Session.CurrentExec, DownstreamExec);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	SaveAssetPackage(BP);

	GLastAuthoredNodes.Add(BP->GetPathName(), Created);

	auto Result = MCPSuccess();
	MCPSetCreated(Result);
	Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
	Result->SetNumberField(TEXT("nodesCreated"), Created.Num());
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> W;
		for (const FString& S : Warnings) W.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("warnings"), W);
	}
	return MCPResult(Result);
}

// ── undo_last_authored ─────────────────────────────────────────────────
TSharedPtr<FJsonValue> FBlueprintHandlers::UndoLastAuthored(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("path"), TEXT("assetPath"), AssetPath)) return Err;
	UBlueprint* BP = LoadBlueprint(AssetPath);
	if (!BP) return MCPError(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TArray<FGuid>* Guids = GLastAuthoredNodes.Find(BP->GetPathName());
	if (!Guids || Guids->Num() == 0) return MCPError(TEXT("No authored nodes recorded for this Blueprint this session"));

	int32 Deleted = 0;
	for (const FGuid& G : *Guids)
	{
		if (UEdGraphNode* Node = FBlueprintEditorUtils::GetNodeByGUID(BP, G))
		{
			Node->Modify();
			Node->DestroyNode();
			Deleted++;
		}
	}
	GLastAuthoredNodes.Remove(BP->GetPathName());

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	SaveAssetPackage(BP);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
	Result->SetNumberField(TEXT("nodesDeleted"), Deleted);
	return MCPResult(Result);
}
