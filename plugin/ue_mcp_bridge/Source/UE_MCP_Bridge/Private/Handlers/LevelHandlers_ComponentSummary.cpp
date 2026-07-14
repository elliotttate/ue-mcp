#include "LevelHandlers.h"

#include "HandlerUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
	constexpr int32 DefaultComponentSampleLimit = 20;
	constexpr int32 MaxComponentSampleLimit = 100;

	struct FNearestComponentCandidate
	{
		UActorComponent* Component = nullptr;
		USceneComponent* SceneComponent = nullptr;
		UPrimitiveComponent* PrimitiveComponent = nullptr;
		UStaticMeshComponent* StaticMeshComponent = nullptr;
		FVector WorldLocation = FVector::ZeroVector;
		double Distance = 0.0;
		bool bHasWorldLocation = false;
	};

	bool IsCandidateNearer(const FNearestComponentCandidate& A, const FNearestComponentCandidate& B)
	{
		if (!FMath::IsNearlyEqual(A.Distance, B.Distance))
		{
			return A.Distance < B.Distance;
		}
		const FString AName = A.Component ? A.Component->GetName() : FString();
		const FString BName = B.Component ? B.Component->GetName() : FString();
		return AName < BName;
	}

	void AddBoundedCandidate(
		TArray<FNearestComponentCandidate>& Candidates,
		const FNearestComponentCandidate& Candidate,
		const int32 Limit)
	{
		if (Limit <= 0)
		{
			return;
		}
		if (Candidates.Num() < Limit)
		{
			Candidates.Add(Candidate);
			return;
		}

		int32 FarthestIndex = 0;
		for (int32 Index = 1; Index < Candidates.Num(); ++Index)
		{
			if (IsCandidateNearer(Candidates[FarthestIndex], Candidates[Index]))
			{
				FarthestIndex = Index;
			}
		}
		if (IsCandidateNearer(Candidate, Candidates[FarthestIndex]))
		{
			Candidates[FarthestIndex] = Candidate;
		}
	}
}

// Aggregate a large actor's component state without serializing its entire
// component tree. The only per-component output is a capped nearest sample,
// which keeps payload size stable even for generated actors with thousands of
// StaticMeshComponents.
TSharedPtr<FJsonValue> FLevelHandlers::SummarizeComponents(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	FString ActorPath;
	FString InternalName;
	const bool bHasLabel = Params->TryGetStringField(TEXT("actorLabel"), ActorLabel) && !ActorLabel.IsEmpty();
	const bool bHasPath = Params->TryGetStringField(TEXT("actorPath"), ActorPath) && !ActorPath.IsEmpty();
	const bool bHasInternalName = Params->TryGetStringField(TEXT("internalName"), InternalName) && !InternalName.IsEmpty();
	if (!bHasLabel && !bHasPath && !bHasInternalName)
	{
		return MCPError(TEXT("Missing 'actorLabel', 'internalName', or 'actorPath' parameter"));
	}

	const FString WorldScope = OptionalString(Params, TEXT("world"), TEXT("auto"));
	UWorld* World = ResolveWorldScope(WorldScope);
	if (!World)
	{
		return MCPError(FString::Printf(TEXT("World '%s' not available"), *WorldScope));
	}

	AActor* Actor = nullptr;
	if (bHasPath)
	{
		Actor = FindActorByLabelOrPath(World, FString(), ActorPath);
	}
	if (!Actor && bHasLabel)
	{
		// Accept the runtime UObject name through actorLabel as a convenience, but
		// expose internalName explicitly so callers do not have to rely on it.
		Actor = FindActorByLabelOrName(World, ActorLabel);
	}
	if (!Actor && bHasInternalName)
	{
		Actor = FindActorByLabelNameOrPath(World, InternalName);
	}
	if (!Actor)
	{
		const FString Identifier = bHasPath ? ActorPath : (bHasLabel ? ActorLabel : InternalName);
		return MCPError(FString::Printf(TEXT("Actor not found: %s"), *Identifier));
	}

	const FString ComponentClassFilter = OptionalString(Params, TEXT("componentClass"));
	const int32 SampleLimit = FMath::Clamp(
		OptionalInt(Params, TEXT("sampleLimit"), DefaultComponentSampleLimit),
		0,
		MaxComponentSampleLimit);

	double MaxDistance = 0.0;
	const bool bHasMaxDistance = Params->TryGetNumberField(TEXT("maxDistance"), MaxDistance);
	if (bHasMaxDistance && (!FMath::IsFinite(MaxDistance) || MaxDistance < 0.0))
	{
		return MCPError(TEXT("maxDistance must be a finite number greater than or equal to zero"));
	}

	AActor* ReferenceActor = nullptr;
	FString ReferenceSource;
	const FString ReferenceActorToken = OptionalString(Params, TEXT("referenceActor"));
	if (!ReferenceActorToken.IsEmpty())
	{
		ReferenceActor = FindActorByLabelNameOrPath(World, ReferenceActorToken);
		if (!ReferenceActor)
		{
			return MCPError(FString::Printf(TEXT("Reference actor not found: %s"), *ReferenceActorToken));
		}
		ReferenceSource = TEXT("referenceActor");
	}
	else if (OptionalBool(Params, TEXT("usePlayerPawn"), true))
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			ReferenceActor = PlayerController->GetPawn();
		}
		if (ReferenceActor)
		{
			ReferenceSource = TEXT("playerPawn");
		}
	}
	if (!ReferenceActor)
	{
		ReferenceActor = Actor;
		ReferenceSource = TEXT("targetActorFallback");
	}
	const FVector ReferenceLocation = ReferenceActor->GetActorLocation();

	int32 MatchingComponentCount = 0;
	int32 SceneComponentCount = 0;
	int32 PrimitiveComponentCount = 0;
	int32 WithinMaxDistanceCount = 0;
	int32 EligibleSampleCount = 0;

	int32 StaticMeshTotal = 0;
	int32 StaticMeshVisible = 0;
	int32 StaticMeshHiddenInGame = 0;
	int32 StaticMeshRenderInMainPass = 0;
	int32 StaticMeshOwnerNoSee = 0;
	int32 StaticMeshEffectiveVisible = 0;
	int32 StaticMeshWithMesh = 0;
	int32 StaticMeshWithinMaxDistance = 0;
	int32 StaticMeshDesiredDrawDistanceNonZero = 0;
	int32 StaticMeshCachedDrawDistanceNonZero = 0;

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	TArray<FNearestComponentCandidate> NearestCandidates;
	NearestCandidates.Reserve(SampleLimit);

	for (UActorComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}
		const FString ComponentClassName = Component->GetClass()->GetName();
		const FString ComponentClassPath = Component->GetClass()->GetPathName();
		if (!ComponentClassFilter.IsEmpty()
			&& !ComponentClassName.Contains(ComponentClassFilter, ESearchCase::IgnoreCase)
			&& !ComponentClassPath.Contains(ComponentClassFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++MatchingComponentCount;
		USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
		UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component);
		UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
		if (SceneComponent)
		{
			++SceneComponentCount;
		}
		if (PrimitiveComponent)
		{
			++PrimitiveComponentCount;
		}

		const bool bHasWorldLocation = SceneComponent != nullptr;
		const FVector ComponentLocation = SceneComponent ? SceneComponent->GetComponentLocation() : Actor->GetActorLocation();
		const double Distance = FVector::Distance(ComponentLocation, ReferenceLocation);
		const bool bWithinMaxDistance = !bHasMaxDistance || Distance <= MaxDistance;
		if (bWithinMaxDistance)
		{
			++WithinMaxDistanceCount;
			++EligibleSampleCount;
		}

		if (StaticMeshComponent)
		{
			++StaticMeshTotal;
			const bool bVisible = StaticMeshComponent->IsVisible();
			if (bVisible) ++StaticMeshVisible;
			if (StaticMeshComponent->bHiddenInGame) ++StaticMeshHiddenInGame;
			if (StaticMeshComponent->bRenderInMainPass) ++StaticMeshRenderInMainPass;
			if (StaticMeshComponent->bOwnerNoSee) ++StaticMeshOwnerNoSee;
			if (StaticMeshComponent->GetStaticMesh()) ++StaticMeshWithMesh;
			if (bWithinMaxDistance) ++StaticMeshWithinMaxDistance;
			if (!FMath::IsNearlyZero(StaticMeshComponent->LDMaxDrawDistance)) ++StaticMeshDesiredDrawDistanceNonZero;
			if (!FMath::IsNearlyZero(StaticMeshComponent->CachedMaxDrawDistance)) ++StaticMeshCachedDrawDistanceNonZero;
			if (bVisible
				&& !StaticMeshComponent->bHiddenInGame
				&& StaticMeshComponent->bRenderInMainPass
				&& !StaticMeshComponent->bOwnerNoSee
				&& !Actor->IsHidden())
			{
				++StaticMeshEffectiveVisible;
			}
		}

		if (bWithinMaxDistance)
		{
			FNearestComponentCandidate Candidate;
			Candidate.Component = Component;
			Candidate.SceneComponent = SceneComponent;
			Candidate.PrimitiveComponent = PrimitiveComponent;
			Candidate.StaticMeshComponent = StaticMeshComponent;
			Candidate.WorldLocation = ComponentLocation;
			Candidate.Distance = Distance;
			Candidate.bHasWorldLocation = bHasWorldLocation;
			AddBoundedCandidate(NearestCandidates, Candidate, SampleLimit);
		}
	}

	NearestCandidates.Sort([](const FNearestComponentCandidate& A, const FNearestComponentCandidate& B)
	{
		return IsCandidateNearer(A, B);
	});

	TArray<TSharedPtr<FJsonValue>> Sample;
	Sample.Reserve(NearestCandidates.Num());
	for (const FNearestComponentCandidate& Candidate : NearestCandidates)
	{
		UActorComponent* Component = Candidate.Component;
		if (!Component)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Component->GetName());
		Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
		Entry->SetStringField(TEXT("path"), Component->GetPathName());
		Entry->SetNumberField(TEXT("distance"), Candidate.Distance);
		Entry->SetBoolField(TEXT("registered"), Component->IsRegistered());
		Entry->SetBoolField(TEXT("active"), Component->IsActive());
		if (Candidate.bHasWorldLocation)
		{
			Entry->SetObjectField(TEXT("worldLocation"), MCPVec3ToJsonObject(Candidate.WorldLocation));
		}
		if (Candidate.SceneComponent)
		{
			Entry->SetBoolField(TEXT("visible"), Candidate.SceneComponent->IsVisible());
			Entry->SetBoolField(TEXT("hiddenInGame"), Candidate.SceneComponent->bHiddenInGame);
		}
		if (Candidate.PrimitiveComponent)
		{
			Entry->SetBoolField(TEXT("renderInMainPass"), Candidate.PrimitiveComponent->bRenderInMainPass);
			Entry->SetBoolField(TEXT("ownerNoSee"), Candidate.PrimitiveComponent->bOwnerNoSee);
			Entry->SetNumberField(TEXT("desiredMaxDrawDistance"), Candidate.PrimitiveComponent->LDMaxDrawDistance);
			Entry->SetNumberField(TEXT("cachedMaxDrawDistance"), Candidate.PrimitiveComponent->CachedMaxDrawDistance);
		}
		if (Candidate.StaticMeshComponent)
		{
			if (UStaticMesh* Mesh = Candidate.StaticMeshComponent->GetStaticMesh())
			{
				Entry->SetStringField(TEXT("meshName"), Mesh->GetName());
				Entry->SetStringField(TEXT("meshPath"), Mesh->GetPathName());
			}
			else
			{
				Entry->SetStringField(TEXT("meshName"), TEXT(""));
				Entry->SetStringField(TEXT("meshPath"), TEXT(""));
			}
		}
		Sample.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
	ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
	ActorJson->SetStringField(TEXT("name"), Actor->GetName());
	ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	ActorJson->SetStringField(TEXT("path"), Actor->GetPathName());
	ActorJson->SetBoolField(TEXT("hiddenInGame"), Actor->IsHidden());

	TSharedPtr<FJsonObject> ReferenceJson = MakeShared<FJsonObject>();
	ReferenceJson->SetStringField(TEXT("source"), ReferenceSource);
	ReferenceJson->SetStringField(TEXT("label"), ReferenceActor->GetActorLabel());
	ReferenceJson->SetStringField(TEXT("name"), ReferenceActor->GetName());
	ReferenceJson->SetStringField(TEXT("path"), ReferenceActor->GetPathName());
	ReferenceJson->SetObjectField(TEXT("location"), MCPVec3ToJsonObject(ReferenceLocation));

	TSharedPtr<FJsonObject> StaticMeshCounts = MakeShared<FJsonObject>();
	StaticMeshCounts->SetNumberField(TEXT("total"), StaticMeshTotal);
	StaticMeshCounts->SetNumberField(TEXT("visible"), StaticMeshVisible);
	StaticMeshCounts->SetNumberField(TEXT("hiddenInGame"), StaticMeshHiddenInGame);
	StaticMeshCounts->SetNumberField(TEXT("renderInMainPass"), StaticMeshRenderInMainPass);
	StaticMeshCounts->SetNumberField(TEXT("ownerNoSee"), StaticMeshOwnerNoSee);
	StaticMeshCounts->SetNumberField(TEXT("effectiveVisible"), StaticMeshEffectiveVisible);
	StaticMeshCounts->SetNumberField(TEXT("withMesh"), StaticMeshWithMesh);
	StaticMeshCounts->SetNumberField(TEXT("withinMaxDistance"), StaticMeshWithinMaxDistance);
	StaticMeshCounts->SetNumberField(TEXT("desiredMaxDrawDistanceNonZero"), StaticMeshDesiredDrawDistanceNonZero);
	StaticMeshCounts->SetNumberField(TEXT("cachedMaxDrawDistanceNonZero"), StaticMeshCachedDrawDistanceNonZero);

	TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("totalComponents"), Components.Num());
	Counts->SetNumberField(TEXT("matchingComponents"), MatchingComponentCount);
	Counts->SetNumberField(TEXT("sceneComponents"), SceneComponentCount);
	Counts->SetNumberField(TEXT("primitiveComponents"), PrimitiveComponentCount);
	Counts->SetNumberField(TEXT("withinMaxDistance"), WithinMaxDistanceCount);
	Counts->SetObjectField(TEXT("staticMeshComponents"), StaticMeshCounts);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("world"), WorldScope);
	Result->SetObjectField(TEXT("actor"), ActorJson);
	Result->SetObjectField(TEXT("reference"), ReferenceJson);
	if (!ComponentClassFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("componentClass"), ComponentClassFilter);
	}
	if (bHasMaxDistance)
	{
		Result->SetNumberField(TEXT("maxDistance"), MaxDistance);
	}
	Result->SetNumberField(TEXT("sampleLimit"), SampleLimit);
	Result->SetBoolField(TEXT("sampleTruncated"), EligibleSampleCount > Sample.Num());
	Result->SetObjectField(TEXT("counts"), Counts);
	Result->SetArrayField(TEXT("nearestComponents"), Sample);
	return MCPResult(Result);
}
