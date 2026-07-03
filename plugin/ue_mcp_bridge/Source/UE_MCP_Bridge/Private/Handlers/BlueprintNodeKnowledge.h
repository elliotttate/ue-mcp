#pragma once

// Blueprint node/pin knowledge layer: curated corrections for the names agents
// most often get wrong, plus confidence-scored fuzzy resolution. The goal is
// that an approximately-right add_node / connect_pins call succeeds (with the
// correction reported back) instead of failing and burning a retry round-trip.
//
// Knowledge encoded here comes from observed agent failures:
// - Blueprint display names differ from UFUNCTION names (Destroy vs
//   K2_DestroyActor, LineTraceByChannel vs LineTraceSingle).
// - Pin display names differ from internal FName ("Item Index", "Dimension 1").
// - Well-known pin aliases (Item -> NewItem on Array_Add, Map -> TargetMap).

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;
class UClass;
class UFunction;

namespace MCPNodeKnowledge
{
	struct FPinResolution
	{
		UEdGraphPin* Pin = nullptr;
		/** Set when a non-exact match was used - the name the caller sent. */
		FString CorrectedFrom;
		/** Ranked "did you mean" candidates for the error message when Pin is null. */
		FString Suggestions;
	};

	/** Resolve a pin by name: exact -> curated alias -> case-insensitive ->
	 *  display-name -> normalized -> fuzzy word scoring. Only auto-corrects on
	 *  a confident, unambiguous match; otherwise Pin is null and Suggestions
	 *  carries the ranked candidates. */
	FPinResolution ResolvePin(UEdGraphNode* Node, const FString& RequestedName);

	/** Find a function on a class, tolerating blueprint display names:
	 *  exact -> curated redirect (Destroy -> K2_DestroyActor, ...) -> K2_ prefix
	 *  -> case-insensitive -> DisplayName metadata. OutCorrectedName is set to
	 *  the real UFUNCTION name when a correction was applied. */
	UFunction* FindFunctionSmart(const UClass* Class, const FString& FunctionName, FString* OutCorrectedName = nullptr);

	/** Ranked near-miss function names across the given classes, for the error
	 *  message when no function resolves anywhere. */
	FString SuggestFunctions(const TArray<const UClass*>& Classes, const FString& FunctionName, int32 MaxSuggestions = 5);

	/** Compact "Name (direction, category)" pin list for error messages. */
	FString DescribeAvailablePins(const UEdGraphNode* Node);
}
