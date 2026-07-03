#include "BlueprintNodeKnowledge.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	FString Normalize(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (TCHAR C : In)
		{
			if (C != TEXT(' ') && C != TEXT('_') && C != TEXT('-'))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	/** Confidence that Requested refers to Actual. Cascade mirrors what a human
	 *  reader would accept; thresholds tuned so only unambiguous matches
	 *  auto-correct. */
	float ScoreName(const FString& Requested, const FString& Actual)
	{
		if (Requested == Actual) return 1.0f;
		if (Requested.Equals(Actual, ESearchCase::IgnoreCase)) return 0.95f;
		const FString NormReq = Normalize(Requested);
		const FString NormAct = Normalize(Actual);
		if (NormReq == NormAct) return 0.9f;
		if (NormReq.Len() >= 3 && (NormAct.Contains(NormReq) || NormReq.Contains(NormAct))) return 0.72f;

		// Word coverage: how many words of the shorter name appear in the longer.
		TArray<FString> ReqWords, ActWords;
		Requested.ParseIntoArray(ReqWords, TEXT(" "), true);
		if (ReqWords.Num() <= 1)
		{
			return 0.0f;
		}
		int32 Hits = 0;
		for (const FString& W : ReqWords)
		{
			if (Actual.Contains(W, ESearchCase::IgnoreCase)) Hits++;
		}
		return (Hits == ReqWords.Num()) ? 0.7f : 0.0f;
	}

	/** Curated pin-name aliases: what agents type -> what the pin is called.
	 *  Applied only when the requested name has no exact/near match, and only
	 *  accepted if the alias target actually exists on the node. */
	const TCHAR* const* PinAliasCandidates(const FString& NormRequested, int32& OutCount)
	{
		static const TCHAR* Item[] = { TEXT("NewItem"), TEXT("Item"), TEXT("Value") };
		static const TCHAR* Map[] = { TEXT("TargetMap") };
		static const TCHAR* Array[] = { TEXT("TargetArray") };
		static const TCHAR* Set[] = { TEXT("TargetSet") };
		static const TCHAR* Index[] = { TEXT("Dimension 1"), TEXT("Index") };
		static const TCHAR* Class[] = { TEXT("ActorClass"), TEXT("ObjectClass"), TEXT("InClass"), TEXT("Class") };
		static const TCHAR* Exec[] = { TEXT("execute") };
		static const TCHAR* Then[] = { TEXT("then") };
		static const TCHAR* Delta[] = { TEXT("DeltaSeconds") };

		OutCount = 0;
		if (NormRequested == TEXT("item") || NormRequested == TEXT("itemtoadd") || NormRequested == TEXT("newelement")) { OutCount = 3; return Item; }
		if (NormRequested == TEXT("map")) { OutCount = 1; return Map; }
		if (NormRequested == TEXT("array")) { OutCount = 1; return Array; }
		if (NormRequested == TEXT("set")) { OutCount = 1; return Set; }
		if (NormRequested == TEXT("index") || NormRequested == TEXT("arrayindex")) { OutCount = 2; return Index; }
		if (NormRequested == TEXT("class")) { OutCount = 4; return Class; }
		if (NormRequested == TEXT("exec") || NormRequested == TEXT("in") || NormRequested == TEXT("execin") || NormRequested == TEXT("input")) { OutCount = 1; return Exec; }
		if (NormRequested == TEXT("out") || NormRequested == TEXT("execout") || NormRequested == TEXT("next")) { OutCount = 1; return Then; }
		if (NormRequested == TEXT("deltatime")) { OutCount = 1; return Delta; }
		return nullptr;
	}

	UEdGraphPin* FindPinExact(const UEdGraphNode* Node, const FString& Name)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->PinName.ToString() == Name) return Pin;
		}
		return nullptr;
	}

	/** The single data-output shortcut: requests like "ReturnValue"/"Output"/
	 *  "Result"/"Value" on a node with exactly one visible non-exec output pin
	 *  (var-get nodes name that pin after the variable, which agents rarely
	 *  guess) resolve to that pin. */
	UEdGraphPin* ResolveSoleDataOutput(const UEdGraphNode* Node, const FString& NormRequested)
	{
		if (NormRequested != TEXT("returnvalue") && NormRequested != TEXT("output") &&
			NormRequested != TEXT("result") && NormRequested != TEXT("value") && NormRequested != TEXT("out"))
		{
			return nullptr;
		}
		UEdGraphPin* Sole = nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Sole) return nullptr; // more than one data output - ambiguous
			Sole = Pin;
		}
		return Sole;
	}
}

namespace MCPNodeKnowledge
{

FPinResolution ResolvePin(UEdGraphNode* Node, const FString& RequestedName)
{
	FPinResolution Out;
	if (!Node) return Out;

	// 1. Exact.
	if (UEdGraphPin* Exact = FindPinExact(Node, RequestedName))
	{
		Out.Pin = Exact;
		return Out;
	}

	const FString NormRequested = Normalize(RequestedName);

	// 2. Curated aliases (only if the alias target exists on this node).
	int32 AliasCount = 0;
	if (const TCHAR* const* Aliases = PinAliasCandidates(NormRequested, AliasCount))
	{
		for (int32 i = 0; i < AliasCount; ++i)
		{
			if (UEdGraphPin* Aliased = FindPinExact(Node, Aliases[i]))
			{
				Out.Pin = Aliased;
				Out.CorrectedFrom = RequestedName;
				return Out;
			}
		}
	}

	// 3. Single-data-output shortcut for ReturnValue/Output/Result/Value.
	if (UEdGraphPin* Sole = ResolveSoleDataOutput(Node, NormRequested))
	{
		Out.Pin = Sole;
		Out.CorrectedFrom = RequestedName;
		return Out;
	}

	// 4. Scored resolution over internal names and display names.
	UEdGraphPin* Best = nullptr;
	float BestScore = 0.0f;
	float SecondScore = 0.0f;
	struct FScored { UEdGraphPin* Pin; float Score; };
	TArray<FScored> Scored;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		float Score = ScoreName(RequestedName, Pin->PinName.ToString());
		const FString Display = Pin->GetDisplayName().ToString();
		if (!Display.IsEmpty())
		{
			Score = FMath::Max(Score, ScoreName(RequestedName, Display) - 0.02f);
		}
		if (Score > 0.0f)
		{
			Scored.Add({ Pin, Score });
		}
		if (Score > BestScore)
		{
			SecondScore = BestScore;
			BestScore = Score;
			Best = Pin;
		}
		else if (Score > SecondScore)
		{
			SecondScore = Score;
		}
	}

	// Auto-accept only a confident, clearly-separated winner.
	if (Best && BestScore >= 0.85f && BestScore > SecondScore + 0.05f)
	{
		Out.Pin = Best;
		Out.CorrectedFrom = RequestedName;
		return Out;
	}

	// No confident match: rank suggestions for the error message.
	Scored.Sort([](const FScored& A, const FScored& B) { return A.Score > B.Score; });
	TArray<FString> Names;
	for (int32 i = 0; i < Scored.Num() && i < 3; ++i)
	{
		Names.Add(Scored[i].Pin->PinName.ToString());
	}
	Out.Suggestions = FString::Join(Names, TEXT(", "));
	return Out;
}

/** Blueprint display name -> UFUNCTION name for the engine functions agents
 *  call most. Kept name-only: the K2_* names are unique enough that a wrong
 *  class hit is impossible in practice, and the resolved function is always
 *  re-validated against the class it was found on. */
static const TMap<FString, FString>& FunctionRedirects()
{
	static const TMap<FString, FString> Redirects = {
		{ TEXT("Destroy"), TEXT("K2_DestroyActor") },
		{ TEXT("DestroyActor"), TEXT("K2_DestroyActor") },
		{ TEXT("SetActorLocation"), TEXT("K2_SetActorLocation") },
		{ TEXT("SetActorRotation"), TEXT("K2_SetActorRotation") },
		{ TEXT("SetActorLocationAndRotation"), TEXT("K2_SetActorLocationAndRotation") },
		{ TEXT("SetActorTransform"), TEXT("K2_SetActorTransform") },
		{ TEXT("GetActorLocation"), TEXT("K2_GetActorLocation") },
		{ TEXT("GetActorRotation"), TEXT("K2_GetActorRotation") },
		{ TEXT("AddActorWorldOffset"), TEXT("K2_AddActorWorldOffset") },
		{ TEXT("AddActorWorldRotation"), TEXT("K2_AddActorWorldRotation") },
		{ TEXT("AddActorLocalOffset"), TEXT("K2_AddActorLocalOffset") },
		{ TEXT("AddActorLocalRotation"), TEXT("K2_AddActorLocalRotation") },
		{ TEXT("SetActorRelativeLocation"), TEXT("K2_SetActorRelativeLocation") },
		{ TEXT("SetActorRelativeRotation"), TEXT("K2_SetActorRelativeRotation") },
		{ TEXT("AttachToComponent"), TEXT("K2_AttachToComponent") },
		{ TEXT("AttachToActor"), TEXT("K2_AttachToActor") },
		{ TEXT("DetachFromActor"), TEXT("K2_DetachFromActor") },
		{ TEXT("GetComponentLocation"), TEXT("K2_GetComponentLocation") },
		{ TEXT("GetComponentRotation"), TEXT("K2_GetComponentRotation") },
		{ TEXT("GetComponentScale"), TEXT("K2_GetComponentScale") },
		{ TEXT("SetWorldLocation"), TEXT("K2_SetWorldLocation") },
		{ TEXT("SetWorldRotation"), TEXT("K2_SetWorldRotation") },
		{ TEXT("SetWorldTransform"), TEXT("K2_SetWorldTransform") },
		{ TEXT("SetRelativeLocation"), TEXT("K2_SetRelativeLocation") },
		{ TEXT("SetRelativeRotation"), TEXT("K2_SetRelativeRotation") },
		{ TEXT("SetRelativeTransform"), TEXT("K2_SetRelativeTransform") },
		{ TEXT("AddWorldOffset"), TEXT("K2_AddWorldOffset") },
		{ TEXT("AddWorldRotation"), TEXT("K2_AddWorldRotation") },
		{ TEXT("AddRelativeLocation"), TEXT("K2_AddRelativeLocation") },
		{ TEXT("AddRelativeRotation"), TEXT("K2_AddRelativeRotation") },
		{ TEXT("AddLocalOffset"), TEXT("K2_AddLocalOffset") },
		{ TEXT("AddLocalRotation"), TEXT("K2_AddLocalRotation") },
		{ TEXT("SetTimer"), TEXT("K2_SetTimer") },
		{ TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimer") },
		{ TEXT("SetTimerDelegate"), TEXT("K2_SetTimerDelegate") },
		{ TEXT("ClearTimer"), TEXT("K2_ClearTimer") },
		{ TEXT("ClearTimerHandle"), TEXT("K2_ClearTimerHandle") },
		{ TEXT("ClearAndInvalidateTimerHandle"), TEXT("K2_ClearAndInvalidateTimerHandle") },
		{ TEXT("PauseTimer"), TEXT("K2_PauseTimer") },
		{ TEXT("UnPauseTimer"), TEXT("K2_UnPauseTimer") },
		{ TEXT("LineTraceByChannel"), TEXT("LineTraceSingle") },
		{ TEXT("LineTraceMultiByChannel"), TEXT("LineTraceMulti") },
		{ TEXT("SphereTraceByChannel"), TEXT("SphereTraceSingle") },
		{ TEXT("SphereTraceMultiByChannel"), TEXT("SphereTraceMulti") },
		{ TEXT("BoxTraceByChannel"), TEXT("BoxTraceSingle") },
		{ TEXT("BoxTraceMultiByChannel"), TEXT("BoxTraceMulti") },
		{ TEXT("CapsuleTraceByChannel"), TEXT("CapsuleTraceSingle") },
		{ TEXT("CapsuleTraceMultiByChannel"), TEXT("CapsuleTraceMulti") },
		{ TEXT("LineTraceByProfile"), TEXT("LineTraceSingleByProfile") },
		{ TEXT("GetController"), TEXT("GetController") },
		{ TEXT("OnReset"), TEXT("K2_OnReset") },
	};
	return Redirects;
}

UFunction* FindFunctionSmart(const UClass* Class, const FString& FunctionName, FString* OutCorrectedName)
{
	if (!Class || FunctionName.IsEmpty()) return nullptr;
	if (OutCorrectedName) OutCorrectedName->Reset();

	// 1. Exact.
	if (UFunction* Exact = Class->FindFunctionByName(FName(*FunctionName)))
	{
		return Exact;
	}

	auto Correct = [&](UFunction* Func) -> UFunction*
	{
		if (Func && OutCorrectedName) *OutCorrectedName = Func->GetName();
		return Func;
	};

	// 2. Curated redirect table.
	if (const FString* Redirect = FunctionRedirects().Find(FunctionName))
	{
		if (UFunction* Redirected = Class->FindFunctionByName(FName(**Redirect)))
		{
			return Correct(Redirected);
		}
	}

	// 3. K2_ / BlueprintCallable wrapper prefix.
	if (UFunction* Prefixed = Class->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName))))
	{
		return Correct(Prefixed);
	}

	// 4. Case-insensitive / normalized walk, and DisplayName metadata.
	const FString NormRequested = Normalize(FunctionName);
	UFunction* NormMatch = nullptr;
	for (TFieldIterator<UFunction> It(Class); It; ++It)
	{
		UFunction* Func = *It;
		if (Normalize(Func->GetName()) == NormRequested)
		{
			NormMatch = Func;
			break;
		}
#if WITH_EDITOR
		const FString Display = Func->GetMetaData(TEXT("DisplayName"));
		if (!Display.IsEmpty() && Normalize(Display) == NormRequested)
		{
			NormMatch = Func;
			break;
		}
#endif
	}
	return Correct(NormMatch);
}

FString SuggestFunctions(const TArray<const UClass*>& Classes, const FString& FunctionName, int32 MaxSuggestions)
{
	struct FScoredName { FString Name; FString ClassName; float Score; };
	TArray<FScoredName> Scored;
	for (const UClass* Class : Classes)
	{
		if (!Class) continue;
		for (TFieldIterator<UFunction> It(Class); It; ++It)
		{
			if (!It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
			const float Score = ScoreName(FunctionName, It->GetName());
			if (Score >= 0.6f)
			{
				Scored.Add({ It->GetName(), Class->GetName(), Score });
			}
		}
	}
	Scored.Sort([](const FScoredName& A, const FScoredName& B) { return A.Score > B.Score; });
	TArray<FString> Parts;
	for (int32 i = 0; i < Scored.Num() && i < MaxSuggestions; ++i)
	{
		Parts.Add(FString::Printf(TEXT("%s.%s"), *Scored[i].ClassName, *Scored[i].Name));
	}
	return FString::Join(Parts, TEXT(", "));
}

FString DescribeAvailablePins(const UEdGraphNode* Node)
{
	TArray<FString> Parts;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		Parts.Add(FString::Printf(TEXT("%s (%s%s)"),
			*Pin->PinName.ToString(),
			Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"),
			bExec ? TEXT(", exec") : TEXT("")));
	}
	return FString::Join(Parts, TEXT(", "));
}

} // namespace MCPNodeKnowledge
