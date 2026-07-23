#include "AssetHandlers.h"
#include "HandlerUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace
{
	struct FQuantizedProxyPoint
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;

		bool operator==(const FQuantizedProxyPoint& Other) const
		{
			return X == Other.X && Y == Other.Y && Z == Other.Z;
		}

		bool operator<(const FQuantizedProxyPoint& Other) const
		{
			if (X != Other.X) return X < Other.X;
			if (Y != Other.Y) return Y < Other.Y;
			return Z < Other.Z;
		}

		friend uint32 GetTypeHash(const FQuantizedProxyPoint& Point)
		{
			return HashCombineFast(HashCombineFast(::GetTypeHash(Point.X), ::GetTypeHash(Point.Y)), ::GetTypeHash(Point.Z));
		}
	};

	struct FQuantizedProxyTriangle
	{
		FQuantizedProxyPoint Points[3];

		bool operator==(const FQuantizedProxyTriangle& Other) const
		{
			return Points[0] == Other.Points[0]
				&& Points[1] == Other.Points[1]
				&& Points[2] == Other.Points[2];
		}

		friend uint32 GetTypeHash(const FQuantizedProxyTriangle& Triangle)
		{
			return HashCombineFast(
				HashCombineFast(GetTypeHash(Triangle.Points[0]), GetTypeHash(Triangle.Points[1])),
				GetTypeHash(Triangle.Points[2]));
		}
	};

	struct FDuplicateProxyTriangleGroup
	{
		int32 Count = 0;
		TArray<int32> TriangleIds;
		FVector3d ReferenceNormal = FVector3d::ZeroVector;
		int32 ForwardFacingTriangleCount = 0;
		int32 ReverseFacingTriangleCount = 0;
		int32 UnclassifiedFacingTriangleCount = 0;

		void AddTriangle(int32 TriangleId, const FVector3f& A, const FVector3f& B, const FVector3f& C)
		{
			++Count;
			if (TriangleIds.Num() < 16) TriangleIds.Add(TriangleId);

			const FVector3d Cross = (FVector3d(B) - FVector3d(A)).Cross(FVector3d(C) - FVector3d(A));
			const double CrossSquared = Cross.SquaredLength();
			if (CrossSquared <= 1.0e-24)
			{
				++UnclassifiedFacingTriangleCount;
				return;
			}

			const FVector3d Normal = Cross / FMath::Sqrt(CrossSquared);
			if (ForwardFacingTriangleCount == 0 && ReverseFacingTriangleCount == 0)
			{
				ReferenceNormal = Normal;
				++ForwardFacingTriangleCount;
			}
			else if (FVector3d::DotProduct(ReferenceNormal, Normal) >= 0.0)
			{
				++ForwardFacingTriangleCount;
			}
			else
			{
				++ReverseFacingTriangleCount;
			}
		}

		int32 GetSameFacingDuplicateTriangleCount() const
		{
			return FMath::Max(ForwardFacingTriangleCount - 1, 0)
				+ FMath::Max(ReverseFacingTriangleCount - 1, 0);
		}

		int32 GetOppositeFacingPairCount() const
		{
			return ForwardFacingTriangleCount * ReverseFacingTriangleCount;
		}
	};

	struct FProxyAuditTotals
	{
		int64 TriangleCount = 0;
		int64 BuiltRenderTriangleCount = 0;
		int64 BuiltRenderVertexCount = 0;
		int64 BuiltRenderSectionCount = 0;
		int64 DegenerateTriangleCount = 0;
		int64 DuplicateTriangleCount = 0;
		int64 SameFacingDuplicateTriangleCount = 0;
		int64 OppositeFacingPairCount = 0;
		int64 DuplicateTriangleGroupCount = 0;
		int64 MaterialSlotCount = 0;
		int64 PixelDepthOffsetSlotCount = 0;
		int32 HighRiskMeshCount = 0;
		int32 WarningMeshCount = 0;
		int32 BuiltRenderAvailableMeshCount = 0;
		int32 BuiltRenderMissingMeshCount = 0;
	};

	FQuantizedProxyPoint QuantizeProxyPoint(const FVector3f& Position, double ToleranceCm)
	{
		const double InverseTolerance = 1.0 / ToleranceCm;
		return {
			FMath::RoundToInt64(static_cast<double>(Position.X) * InverseTolerance),
			FMath::RoundToInt64(static_cast<double>(Position.Y) * InverseTolerance),
			FMath::RoundToInt64(static_cast<double>(Position.Z) * InverseTolerance)
		};
	}

	FQuantizedProxyTriangle MakeProxyTriangleKey(
		const FVector3f& A,
		const FVector3f& B,
		const FVector3f& C,
		double ToleranceCm)
	{
		FQuantizedProxyTriangle Key = {
			QuantizeProxyPoint(A, ToleranceCm),
			QuantizeProxyPoint(B, ToleranceCm),
			QuantizeProxyPoint(C, ToleranceCm)
		};
		if (Key.Points[1] < Key.Points[0]) Swap(Key.Points[0], Key.Points[1]);
		if (Key.Points[2] < Key.Points[1]) Swap(Key.Points[1], Key.Points[2]);
		if (Key.Points[1] < Key.Points[0]) Swap(Key.Points[0], Key.Points[1]);
		return Key;
	}

	bool IsProxyTriangleDegenerate(
		const FVector3f& A,
		const FVector3f& B,
		const FVector3f& C,
		double ToleranceCm)
	{
		const FVector3d EdgeAB = FVector3d(B) - FVector3d(A);
		const FVector3d EdgeAC = FVector3d(C) - FVector3d(A);
		const FVector3d EdgeBC = FVector3d(C) - FVector3d(B);
		const double MaxEdgeSquared = FMath::Max3(EdgeAB.SquaredLength(), EdgeAC.SquaredLength(), EdgeBC.SquaredLength());
		if (MaxEdgeSquared <= FMath::Square(ToleranceCm)) return true;

		// |AB x AC| / longest edge is the corresponding triangle altitude.
		// Treat triangles thinner than the positional tolerance as degenerate.
		const double CrossSquared = EdgeAB.Cross(EdgeAC).SquaredLength();
		return CrossSquared <= FMath::Square(ToleranceCm) * MaxEdgeSquared;
	}

	void AddBuiltRenderLodStats(
		UStaticMesh* Mesh,
		int32 LodIndex,
		const TSharedPtr<FJsonObject>& Result,
		FProxyAuditTotals& Totals)
	{
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		const int32 RenderLodCount = RenderData ? RenderData->LODResources.Num() : 0;
		const bool bLodAvailable = RenderData && RenderData->LODResources.IsValidIndex(LodIndex);

		Result->SetNumberField(TEXT("builtRenderLodIndex"), LodIndex);
		Result->SetNumberField(TEXT("builtRenderLodCount"), RenderLodCount);
		Result->SetBoolField(TEXT("builtRenderLodAvailable"), bLodAvailable);
		if (!bLodAvailable)
		{
			Result->SetNumberField(TEXT("builtRenderTriangleCount"), 0);
			Result->SetNumberField(TEXT("builtRenderVertexCount"), 0);
			Result->SetNumberField(TEXT("builtRenderSectionCount"), 0);
			++Totals.BuiltRenderMissingMeshCount;
			return;
		}

		// UStaticMesh's count accessors read the built render buffers. These can
		// differ from MeshDescription source counts after reduction/build.
		const int32 BuiltTriangleCount = Mesh->GetNumTriangles(LodIndex);
		const int32 BuiltVertexCount = Mesh->GetNumVertices(LodIndex);
		const int32 BuiltSectionCount = Mesh->GetNumSections(LodIndex);
		Result->SetNumberField(TEXT("builtRenderTriangleCount"), BuiltTriangleCount);
		Result->SetNumberField(TEXT("builtRenderVertexCount"), BuiltVertexCount);
		Result->SetNumberField(TEXT("builtRenderSectionCount"), BuiltSectionCount);

		Totals.BuiltRenderTriangleCount += BuiltTriangleCount;
		Totals.BuiltRenderVertexCount += BuiltVertexCount;
		Totals.BuiltRenderSectionCount += BuiltSectionCount;
		++Totals.BuiltRenderAvailableMeshCount;
	}

	TSharedPtr<FJsonObject> AuditProxyMesh(
		UStaticMesh* Mesh,
		double ToleranceCm,
		int32 MaxExamples,
		int32 LodIndex,
		FProxyAuditTotals& Totals)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
		Result->SetStringField(TEXT("assetName"), Mesh->GetName());

		TArray<TSharedPtr<FJsonValue>> MaterialSlots;
		int32 PixelDepthOffsetSlotCount = 0;
		const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
		for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
		{
			const FStaticMaterial& Slot = StaticMaterials[SlotIndex];
			UMaterialInterface* MaterialInterface = Slot.MaterialInterface;
			const UMaterial* BaseMaterial = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			const bool bHasPixelDepthOffset = BaseMaterial && BaseMaterial->HasPixelDepthOffsetConnected();
			if (bHasPixelDepthOffset) ++PixelDepthOffsetSlotCount;

			TSharedPtr<FJsonObject> SlotResult = MakeShared<FJsonObject>();
			SlotResult->SetNumberField(TEXT("slotIndex"), SlotIndex);
			SlotResult->SetStringField(TEXT("slotName"), Slot.MaterialSlotName.ToString());
			SlotResult->SetStringField(TEXT("importedSlotName"), Slot.ImportedMaterialSlotName.ToString());
			SlotResult->SetStringField(TEXT("materialPath"), MaterialInterface ? MaterialInterface->GetPathName() : TEXT(""));
			SlotResult->SetStringField(TEXT("baseMaterialPath"), BaseMaterial ? BaseMaterial->GetPathName() : TEXT(""));
			SlotResult->SetBoolField(TEXT("isMaterialInstance"), MaterialInterface && MaterialInterface != BaseMaterial);
			SlotResult->SetBoolField(TEXT("pixelDepthOffsetConnected"), bHasPixelDepthOffset);
			MaterialSlots.Add(MakeShared<FJsonValueObject>(SlotResult));
		}

		Result->SetNumberField(TEXT("materialSlotCount"), StaticMaterials.Num());
		Result->SetNumberField(TEXT("pixelDepthOffsetSlotCount"), PixelDepthOffsetSlotCount);
		Result->SetStringField(
			TEXT("pixelDepthOffsetCoverage"),
			StaticMaterials.Num() == 0 || PixelDepthOffsetSlotCount == 0
				? TEXT("none")
				: (PixelDepthOffsetSlotCount == StaticMaterials.Num() ? TEXT("all") : TEXT("partial")));
		Result->SetArrayField(TEXT("materialSlots"), MaterialSlots);
		Totals.MaterialSlotCount += StaticMaterials.Num();
		Totals.PixelDepthOffsetSlotCount += PixelDepthOffsetSlotCount;
		AddBuiltRenderLodStats(Mesh, LodIndex, Result, Totals);

		FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			Result->SetStringField(TEXT("status"), TEXT("missing_lod0_mesh_description"));
			Result->SetStringField(TEXT("riskLevel"), TEXT("unknown"));
			Result->SetStringField(TEXT("message"), TEXT("LOD0 source mesh description is unavailable; geometry was not audited."));
			return Result;
		}

		FStaticMeshConstAttributes Attributes(*MeshDescription);
		TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TMap<FQuantizedProxyTriangle, FDuplicateProxyTriangleGroup> TriangleGroups;
		int32 DegenerateTriangleCount = 0;

		for (const FTriangleID TriangleId : MeshDescription->Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> TriangleVertices = MeshDescription->GetTriangleVertices(TriangleId);
			if (TriangleVertices.Num() != 3) continue;
			const FVector3f& A = VertexPositions[TriangleVertices[0]];
			const FVector3f& B = VertexPositions[TriangleVertices[1]];
			const FVector3f& C = VertexPositions[TriangleVertices[2]];

			if (IsProxyTriangleDegenerate(A, B, C, ToleranceCm)) ++DegenerateTriangleCount;
			FDuplicateProxyTriangleGroup& Group = TriangleGroups.FindOrAdd(MakeProxyTriangleKey(A, B, C, ToleranceCm));
			Group.AddTriangle(TriangleId.GetValue(), A, B, C);
		}

		struct FDuplicateGroupSummary
		{
			int32 Count = 0;
			int32 SameFacingDuplicateTriangleCount = 0;
			int32 OppositeFacingPairCount = 0;
			int32 UnclassifiedFacingTriangleCount = 0;
			TArray<int32> TriangleIds;
		};
		TArray<FDuplicateGroupSummary> DuplicateGroups;
		int32 DuplicateTriangleCount = 0;
		int32 SameFacingDuplicateTriangleCount = 0;
		int32 OppositeFacingPairCount = 0;
		for (const TPair<FQuantizedProxyTriangle, FDuplicateProxyTriangleGroup>& Pair : TriangleGroups)
		{
			if (Pair.Value.Count <= 1) continue;
			DuplicateTriangleCount += Pair.Value.Count - 1;
			const int32 GroupSameFacingCount = Pair.Value.GetSameFacingDuplicateTriangleCount();
			const int32 GroupOppositeFacingPairCount = Pair.Value.GetOppositeFacingPairCount();
			SameFacingDuplicateTriangleCount += GroupSameFacingCount;
			OppositeFacingPairCount += GroupOppositeFacingPairCount;
			DuplicateGroups.Add({
				Pair.Value.Count,
				GroupSameFacingCount,
				GroupOppositeFacingPairCount,
				Pair.Value.UnclassifiedFacingTriangleCount,
				Pair.Value.TriangleIds
			});
		}
		DuplicateGroups.Sort([](const FDuplicateGroupSummary& Left, const FDuplicateGroupSummary& Right)
		{
			const int32 LeftId = Left.TriangleIds.Num() > 0 ? Left.TriangleIds[0] : MAX_int32;
			const int32 RightId = Right.TriangleIds.Num() > 0 ? Right.TriangleIds[0] : MAX_int32;
			return LeftId < RightId;
		});

		TArray<TSharedPtr<FJsonValue>> DuplicateExamples;
		for (int32 GroupIndex = 0; GroupIndex < DuplicateGroups.Num() && GroupIndex < MaxExamples; ++GroupIndex)
		{
			const FDuplicateGroupSummary& Group = DuplicateGroups[GroupIndex];
			TSharedPtr<FJsonObject> GroupResult = MakeShared<FJsonObject>();
			GroupResult->SetNumberField(TEXT("triangleCount"), Group.Count);
			GroupResult->SetNumberField(TEXT("sameFacingDuplicateTriangleCount"), Group.SameFacingDuplicateTriangleCount);
			GroupResult->SetNumberField(TEXT("oppositeFacingPairCount"), Group.OppositeFacingPairCount);
			GroupResult->SetNumberField(TEXT("unclassifiedFacingTriangleCount"), Group.UnclassifiedFacingTriangleCount);
			GroupResult->SetStringField(
				TEXT("facingClassification"),
				Group.SameFacingDuplicateTriangleCount > 0
					? (Group.OppositeFacingPairCount > 0 ? TEXT("mixed") : TEXT("same_facing"))
					: (Group.OppositeFacingPairCount > 0 ? TEXT("opposite_facing") : TEXT("unclassified")));
			TArray<TSharedPtr<FJsonValue>> TriangleIds;
			for (int32 TriangleId : Group.TriangleIds)
			{
				TriangleIds.Add(MakeShared<FJsonValueNumber>(TriangleId));
			}
			GroupResult->SetArrayField(TEXT("triangleIds"), TriangleIds);
			DuplicateExamples.Add(MakeShared<FJsonValueObject>(GroupResult));
		}

		const int32 TriangleCount = MeshDescription->Triangles().Num();
		const bool bHasDuplicateTriangles = DuplicateTriangleCount > 0;
		const bool bHasSameFacingDuplicateTriangles = SameFacingDuplicateTriangleCount > 0;
		const bool bHasOppositeFacingPairs = OppositeFacingPairCount > 0;
		const bool bHasDegenerateTriangles = DegenerateTriangleCount > 0;
		const TCHAR* RiskLevel = bHasSameFacingDuplicateTriangles
			? TEXT("high")
			: ((bHasOppositeFacingPairs || bHasDegenerateTriangles) ? TEXT("warning") : TEXT("none"));
		TArray<TSharedPtr<FJsonValue>> RiskFactors;
		if (bHasSameFacingDuplicateTriangles) RiskFactors.Add(MakeShared<FJsonValueString>(TEXT("same_facing_duplicate_coplanar_triangles")));
		if (bHasOppositeFacingPairs) RiskFactors.Add(MakeShared<FJsonValueString>(TEXT("opposite_facing_coincident_triangles")));
		if (bHasDegenerateTriangles) RiskFactors.Add(MakeShared<FJsonValueString>(TEXT("degenerate_triangles")));
		if (bHasSameFacingDuplicateTriangles && PixelDepthOffsetSlotCount == 0)
		{
			RiskFactors.Add(MakeShared<FJsonValueString>(TEXT("same_facing_duplicates_without_pixel_depth_offset")));
		}

		Result->SetStringField(TEXT("status"), TEXT("audited"));
		Result->SetStringField(TEXT("riskLevel"), RiskLevel);
		Result->SetBoolField(TEXT("hasZFightRisk"), bHasSameFacingDuplicateTriangles);
		Result->SetNumberField(TEXT("triangleCount"), TriangleCount);
		Result->SetNumberField(TEXT("degenerateTriangleCount"), DegenerateTriangleCount);
		Result->SetNumberField(TEXT("duplicateTriangleCount"), DuplicateTriangleCount);
		Result->SetNumberField(TEXT("sameFacingDuplicateTriangleCount"), SameFacingDuplicateTriangleCount);
		Result->SetNumberField(TEXT("oppositeFacingPairCount"), OppositeFacingPairCount);
		Result->SetNumberField(TEXT("duplicateTriangleGroupCount"), DuplicateGroups.Num());
		Result->SetBoolField(TEXT("duplicateExamplesTruncated"), DuplicateGroups.Num() > MaxExamples);
		Result->SetArrayField(TEXT("duplicateExamples"), DuplicateExamples);
		Result->SetArrayField(TEXT("riskFactors"), RiskFactors);

		Totals.TriangleCount += TriangleCount;
		Totals.DegenerateTriangleCount += DegenerateTriangleCount;
		Totals.DuplicateTriangleCount += DuplicateTriangleCount;
		Totals.SameFacingDuplicateTriangleCount += SameFacingDuplicateTriangleCount;
		Totals.OppositeFacingPairCount += OppositeFacingPairCount;
		Totals.DuplicateTriangleGroupCount += DuplicateGroups.Num();
		if (bHasSameFacingDuplicateTriangles) ++Totals.HighRiskMeshCount;
		else if (bHasOppositeFacingPairs || bHasDegenerateTriangles || bHasDuplicateTriangles) ++Totals.WarningMeshCount;
		return Result;
	}
}

TSharedPtr<FJsonValue> FAssetHandlers::ProxyOverlapAudit(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = OptionalString(Params, TEXT("assetPath"));
	const FString Directory = OptionalString(Params, TEXT("directory"));
	if (AssetPath.IsEmpty() && Directory.IsEmpty())
	{
		return MCPError(TEXT("Provide 'assetPath' and/or 'directory' for proxy_overlap_audit."));
	}

	const double ToleranceCm = OptionalNumber(Params, TEXT("toleranceCm"), 0.001);
	if (!FMath::IsFinite(ToleranceCm) || ToleranceCm <= 0.0 || ToleranceCm > 10.0)
	{
		return MCPError(TEXT("toleranceCm must be finite, greater than 0, and no more than 10 cm."));
	}
	const bool bRecursive = OptionalBool(Params, TEXT("recursive"), true);
	const int32 RequestedOffset = FMath::Max(OptionalInt(Params, TEXT("offset"), 0), 0);
	const int32 Offset = Directory.IsEmpty() ? 0 : RequestedOffset;
	const int32 MaxResults = FMath::Clamp(OptionalInt(Params, TEXT("maxResults"), 200), 1, 2000);
	const int32 MaxExamples = FMath::Clamp(OptionalInt(Params, TEXT("maxExamples"), 20), 1, 200);
	const int32 LodIndex = OptionalInt(Params, TEXT("lodIndex"), 0);
	if (LodIndex < 0)
	{
		return MCPError(TEXT("lodIndex must be a non-negative integer."));
	}

	TArray<FString> MeshPaths;
	TSet<FString> SeenPaths;
	if (!AssetPath.IsEmpty())
	{
		UStaticMesh* ExplicitMesh = LoadAssetByPath<UStaticMesh>(AssetPath);
		if (!ExplicitMesh)
		{
			return MCPError(FString::Printf(TEXT("StaticMesh not found: %s"), *AssetPath));
		}
		MeshPaths.Add(ExplicitMesh->GetPathName());
		SeenPaths.Add(ExplicitMesh->GetPathName());
	}

	TArray<FString> DirectoryMeshPaths;
	if (!Directory.IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Directory), Assets, bRecursive);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath.GetAssetName() != UStaticMesh::StaticClass()->GetFName()) continue;
			DirectoryMeshPaths.Add(Asset.GetObjectPathString());
		}
		DirectoryMeshPaths.Sort();
		DirectoryMeshPaths.RemoveAll([&SeenPaths](const FString& Path)
		{
			return SeenPaths.Contains(Path);
		});
	}

	const int32 DirectoryCandidateMeshCount = DirectoryMeshPaths.Num();
	const int32 PageStart = FMath::Min(Offset, DirectoryCandidateMeshCount);
	const int32 PageEnd = FMath::Min(PageStart + MaxResults, DirectoryCandidateMeshCount);
	for (int32 PathIndex = PageStart; PathIndex < PageEnd; ++PathIndex)
	{
		MeshPaths.Add(DirectoryMeshPaths[PathIndex]);
	}
	const int32 CandidateMeshCount = MeshPaths.Num() + DirectoryCandidateMeshCount - (PageEnd - PageStart);
	const bool bHasMore = PageEnd < DirectoryCandidateMeshCount;
	const int32 NextOffset = bHasMore ? PageEnd : -1;

	FProxyAuditTotals Totals;
	int32 FailedMeshCount = 0;
	TArray<TSharedPtr<FJsonValue>> MeshResults;
	for (const FString& Path : MeshPaths)
	{
		UStaticMesh* Mesh = LoadAssetByPath<UStaticMesh>(Path);
		if (!Mesh)
		{
			++FailedMeshCount;
			TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetStringField(TEXT("assetPath"), Path);
			Failure->SetStringField(TEXT("status"), TEXT("load_failed"));
			MeshResults.Add(MakeShared<FJsonValueObject>(Failure));
			continue;
		}
		MeshResults.Add(MakeShared<FJsonValueObject>(AuditProxyMesh(Mesh, ToleranceCm, MaxExamples, LodIndex, Totals)));
	}

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("triangleCount"), static_cast<double>(Totals.TriangleCount));
	Summary->SetNumberField(TEXT("builtRenderTriangleCount"), static_cast<double>(Totals.BuiltRenderTriangleCount));
	Summary->SetNumberField(TEXT("builtRenderVertexCount"), static_cast<double>(Totals.BuiltRenderVertexCount));
	Summary->SetNumberField(TEXT("builtRenderSectionCount"), static_cast<double>(Totals.BuiltRenderSectionCount));
	Summary->SetNumberField(TEXT("builtRenderAvailableMeshCount"), Totals.BuiltRenderAvailableMeshCount);
	Summary->SetNumberField(TEXT("builtRenderMissingMeshCount"), Totals.BuiltRenderMissingMeshCount);
	Summary->SetNumberField(TEXT("degenerateTriangleCount"), static_cast<double>(Totals.DegenerateTriangleCount));
	Summary->SetNumberField(TEXT("duplicateTriangleCount"), static_cast<double>(Totals.DuplicateTriangleCount));
	Summary->SetNumberField(TEXT("sameFacingDuplicateTriangleCount"), static_cast<double>(Totals.SameFacingDuplicateTriangleCount));
	Summary->SetNumberField(TEXT("oppositeFacingPairCount"), static_cast<double>(Totals.OppositeFacingPairCount));
	Summary->SetNumberField(TEXT("duplicateTriangleGroupCount"), static_cast<double>(Totals.DuplicateTriangleGroupCount));
	Summary->SetNumberField(TEXT("materialSlotCount"), static_cast<double>(Totals.MaterialSlotCount));
	Summary->SetNumberField(TEXT("pixelDepthOffsetSlotCount"), static_cast<double>(Totals.PixelDepthOffsetSlotCount));
	Summary->SetNumberField(TEXT("highRiskMeshCount"), Totals.HighRiskMeshCount);
	Summary->SetNumberField(TEXT("warningMeshCount"), Totals.WarningMeshCount);

	TSharedPtr<FJsonObject> Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("directory"), Directory);
	Result->SetNumberField(TEXT("toleranceCm"), ToleranceCm);
	Result->SetNumberField(TEXT("lodIndex"), LodIndex);
	Result->SetBoolField(TEXT("recursive"), bRecursive);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("nextOffset"), NextOffset);
	Result->SetBoolField(TEXT("hasMore"), bHasMore);
	Result->SetNumberField(TEXT("candidateMeshCount"), CandidateMeshCount);
	Result->SetNumberField(TEXT("directoryCandidateMeshCount"), DirectoryCandidateMeshCount);
	Result->SetNumberField(TEXT("pageMeshCount"), MeshPaths.Num());
	Result->SetNumberField(TEXT("auditedMeshCount"), MeshResults.Num() - FailedMeshCount);
	Result->SetNumberField(TEXT("failedMeshCount"), FailedMeshCount);
	Result->SetBoolField(TEXT("truncated"), bHasMore);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetArrayField(TEXT("meshes"), MeshResults);
	return MCPResult(Result);
}
