// Copyright Epic Games, Inc. All Rights Reserved.

#include "CADKernel/Topo/TopologicalFace.h"

#include "CADKernel/Core/KernelParameters.h"
#include "CADKernel/Core/System.h"
#include "CADKernel/Geo/Curves/RestrictionCurve.h"
#include "CADKernel/Geo/Curves/SegmentCurve.h"
#include "CADKernel/Geo/GeoPoint.h"
#include "CADKernel/Geo/Sampler/SamplerOnChord.h"
#include "CADKernel/Geo/Sampling/Polyline.h"
#include "CADKernel/Geo/Surfaces/Surface.h"
#include "CADKernel/Mesh/Structure/FaceMesh.h"
#include "CADKernel/Mesh/Structure/Grid.h"
#include "CADKernel/Topo/Shell.h"
#include "CADKernel/Topo/TopologicalEdge.h"
#include "CADKernel/Topo/TopologyReport.h"

namespace CADKernel
{

void FTopologicalFace::ComputeBoundary() const
{
	Boundary->Init();
	TArray<TArray<FPoint2D>> TmpLoops;
	Get2DLoopSampling(TmpLoops);

	for (const TArray<FPoint2D>& Loop : TmpLoops)
	{
		for (const FPoint2D& Point : Loop)
		{
			Boundary->ExtendTo(Point);
		}
	}

	// Check with the carrier surface bounds
	CarrierSurface->ExtendBoundaryTo(Boundary);

	Boundary->WidenIfDegenerated();
	Boundary.SetReady();
}

void FTopologicalFace::Presample()
{
	const FSurfacicBoundary& FaceBoundaries = GetBoundary();
	CarrierSurface->Presample(FaceBoundaries, CrossingCoordinates);
}

#ifdef DEBUG_GET_BBOX
#include "CADKernel/Math/Aabb.h"
#endif

void UpdateSubPolylineBBox(const FPolyline3D& Polyline, const FLinearBoundary& IntersectionBoundary, FPolylineBBox& IsoBBox);

void FTopologicalFace::UpdateBBox(int32 IsoCount, const double ApproximationFactor, FBBoxWithNormal& BBox)
{
	const double SAG = GetCarrierSurface()->Get3DTolerance() * ApproximationFactor;

	TArray<TArray<FPoint2D>> BoundaryApproximation;
	Get2DLoopSampling(BoundaryApproximation);

	FPolyline3D Polyline;
	const FSurface& Surface = GetCarrierSurface().Get();
	FIsoCurve3DSamplerOnChord Sampler(Surface, SAG, Polyline);

	IsoCount++;

	TFunction <void(const EIso)> UpdateBBoxWithIsos = [&](const EIso IsoType)
	{
		const FLinearBoundary& Bounds = GetBoundary().Get(IsoType);

		EIso Other = IsoType == EIso::IsoU ? EIso::IsoV : EIso::IsoU;

		double Coordinate = Bounds.Min;
		const double Step = (Bounds.Max - Bounds.Min) / IsoCount;

		for (int32 iIso = 1; iIso < IsoCount; iIso++)
		{
			FPolylineBBox IsoBBox;

			Coordinate += Step;

			TArray<double> Intersections;
			FindLoopIntersectionsWithIso(IsoType, Coordinate, BoundaryApproximation, Intersections);
			int32 IntersectionCount = Intersections.Num();
			if (IntersectionCount == 0)
			{
				continue;
			}

			FLinearBoundary CurveBounds(Intersections[0], Intersections.Last());

			Polyline.Empty();
			Sampler.Set(IsoType, Coordinate, CurveBounds);
			Sampler.Sample();

			if (IntersectionCount % 2 != 0)
			{
				TArray<FPoint> SubPolyline;
				FLinearBoundary IntersectionBoundary(Intersections[IntersectionCount - 1], CurveBounds.GetMax());

#ifdef DEBUG_GET_BBOX2
				Polyline.GetSubPolyline(Boundary, EOrientation::Front, SubPolyline);
				Draw(SubPolyline, EVisuProperty::Iso);
#endif

				UpdateSubPolylineBBox(Polyline, IntersectionBoundary, IsoBBox);

				Intersections.Pop();
				IntersectionCount--;
			}

			if (IntersectionCount == 0)
			{
				continue;
			}

			for (int32 ISection = 0; ISection < IntersectionCount; ISection += 2)
			{
				TArray<FPoint> SubPolyline;
				FLinearBoundary IntersectionBoundary(Intersections[ISection], Intersections[ISection + 1]);

#ifdef DEBUG_GET_BBOX2
				Polyline.GetSubPolyline(Boundary, EOrientation::Front, SubPolyline);
				Draw(SubPolyline, EVisuProperty::Iso);
#endif
				UpdateSubPolylineBBox(Polyline, IntersectionBoundary, IsoBBox);
			}

#ifdef DEBUG_GET_BBOX2
			for (int32 Index = 0; Index < 3; ++Index)
			{
				CADKernel::DisplayPoint(IsoBBox.MaxPoints[Index], EVisuProperty::YellowPoint);
				CADKernel::DisplayPoint(IsoBBox.MinPoints[Index], EVisuProperty::YellowPoint);
			}
#endif

			BBox.Update(IsoBBox, IsoType, Coordinate);
		}
	};

	UpdateBBoxWithIsos(EIso::IsoV);
	UpdateBBoxWithIsos(EIso::IsoU);

	BBox.UpdateNormal(*this);

#ifdef DEBUG_GET_BBOX
	{
		F3DDebugSession _(TEXT("BBox Face"));

		FAABB AABB(BBox.Min, BBox.Max);
		CADKernel::DisplayAABB(AABB);

		for (int32 Index = 0; Index < 3; ++Index)
		{
			CADKernel::DisplayPoint(BBox.MaxPoints[Index], EVisuProperty::YellowPoint);
			CADKernel::DisplayPoint(BBox.MinPoints[Index], EVisuProperty::YellowPoint);
			CADKernel::DisplaySegment(BBox.MaxPoints[Index], BBox.MaxPoints[Index] + BBox.MaxPointNormals[Index], 0, EVisuProperty::YellowCurve);
			CADKernel::DisplaySegment(BBox.MinPoints[Index], BBox.MinPoints[Index] + BBox.MinPointNormals[Index], 0, EVisuProperty::YellowCurve);
		}
		Wait();
	}
#endif
}


void FTopologicalFace::ApplyNaturalLoops()
{
	const FSurfacicBoundary& Boundaries = CarrierSurface->GetBoundary();
	ApplyNaturalLoops(Boundaries);
}

void FTopologicalFace::ApplyNaturalLoops(const FSurfacicBoundary& Boundaries)
{
	ensureCADKernel(Loops.Num() == 0);

	TArray<TSharedPtr<FTopologicalEdge>> Edges;
	Edges.Reserve(4);
	TFunction<void(const FPoint&, const FPoint&)> BuildEdge = [&](const FPoint& StartPoint, const FPoint& EndPoint)
	{
		double Tolerance3D = CarrierSurface->Get3DTolerance();
		TSharedRef<FCurve> Curve2D = FEntity::MakeShared<FSegmentCurve>(StartPoint, EndPoint, 2);
		TSharedRef<FRestrictionCurve> Curve3D = FEntity::MakeShared<FRestrictionCurve>(CarrierSurface.ToSharedRef(), Curve2D);
		TSharedPtr<FTopologicalEdge> Edge = FTopologicalEdge::Make(Curve3D);
		if (!Edge.IsValid())
		{
			return;
		}
		Edges.Add(Edge);
	};

	FPoint StartPoint;
	FPoint EndPoint;

	// Build 4 bounding edges of the surface
	StartPoint.Set(Boundaries[EIso::IsoU].Min, Boundaries[EIso::IsoV].Min);
	EndPoint.Set(Boundaries[EIso::IsoU].Min, Boundaries[EIso::IsoV].Max);
	BuildEdge(StartPoint, EndPoint);

	StartPoint.Set(Boundaries[EIso::IsoU].Min, Boundaries[EIso::IsoV].Max);
	EndPoint.Set(Boundaries[EIso::IsoU].Max, Boundaries[EIso::IsoV].Max);
	BuildEdge(StartPoint, EndPoint);

	StartPoint.Set(Boundaries[EIso::IsoU].Max, Boundaries[EIso::IsoV].Max);
	EndPoint.Set(Boundaries[EIso::IsoU].Max, Boundaries[EIso::IsoV].Min);
	BuildEdge(StartPoint, EndPoint);

	StartPoint.Set(Boundaries[EIso::IsoU].Max, Boundaries[EIso::IsoV].Min);
	EndPoint.Set(Boundaries[EIso::IsoU].Min, Boundaries[EIso::IsoV].Min);
	BuildEdge(StartPoint, EndPoint);

	TSharedPtr<FTopologicalEdge> PreviousEdge = Edges.Last();
	for (TSharedPtr<FTopologicalEdge>& Edge : Edges)
	{
		PreviousEdge->GetEndVertex()->Link(*Edge->GetStartVertex());
		PreviousEdge = Edge;
	}

	TArray<EOrientation> Orientations;
	Orientations.Init(EOrientation::Front, Edges.Num());

	TSharedPtr<FTopologicalLoop> Loop = FTopologicalLoop::Make(Edges, Orientations, CarrierSurface->Get3DTolerance());
	AddLoop(Loop);
}

void FTopologicalFace::AddLoops(const TArray<TSharedPtr<FTopologicalLoop>>& InLoops, int32& DoubtfulLoopOrientationCount)
{
	for (TSharedPtr<FTopologicalLoop> Loop : InLoops)
	{
		AddLoop(Loop);
	}

	for (TSharedPtr<FTopologicalLoop> Loop : InLoops)
	{
		if (!Loop->Orient())
		{
			DoubtfulLoopOrientationCount++;
		}
	}
}

void FTopologicalFace::AddLoop(const TSharedPtr<FTopologicalLoop>& InLoop)
{
	InLoop->SetSurface(this);
	Loops.Add(InLoop);
}

void FTopologicalFace::RemoveLoop(const TSharedPtr<FTopologicalLoop>& Loop)
{
	int32 Index = Loops.Find(Loop);
	if (Index != INDEX_NONE)
	{
		Loop->ResetSurface();
		Loops.RemoveAt(Index);
	}

	if (Loops.Num() == 0)
	{
		SetDeleted();
	}
}

void FTopologicalFace::RemoveLinksWithNeighbours()
{
	for (const TSharedPtr<FTopologicalLoop>& Loop : GetLoops())
	{
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			Edge.Entity->RemoveFromLink();
		}
	}
}

bool FTopologicalFace::HasSameBoundariesAs(const TSharedPtr<FTopologicalFace>& OtherFace) const
{
	int32 EdgeCount = 0;
	for (const TSharedPtr<FTopologicalLoop>& Loop : GetLoops())
	{
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			if (Edge.Entity->IsDegenerated())
			{
				continue;
			}
			Edge.Entity->GetLinkActiveEntity()->SetMarker1();
			++EdgeCount;
		}
	}

	bool bSameBoundary = true;
	int32 OtherFaceEdgeCount = 0;
	for (const TSharedPtr<FTopologicalLoop>& Loop : OtherFace->GetLoops())
	{
		OtherFaceEdgeCount += Loop->EdgeCount();
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			if (Edge.Entity->IsDegenerated())
			{
				continue;
			}
			if (!Edge.Entity->GetLinkActiveEntity()->HasMarker1())
			{
				bSameBoundary = false;
				break;
			}
			++OtherFaceEdgeCount;
		}
	}

	for (const TSharedPtr<FTopologicalLoop>& Loop : GetLoops())
	{
		EdgeCount += Loop->EdgeCount();
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			Edge.Entity->GetLinkActiveEntity()->ResetMarkers();
		}
	}

	if (EdgeCount != OtherFaceEdgeCount)
	{
		bSameBoundary = false;
	}

	return bSameBoundary;
}

const FTopologicalEdge* FTopologicalFace::GetLinkedEdge(const FTopologicalEdge& LinkedEdge) const
{
	for (FTopologicalEdge* TwinEdge : LinkedEdge.GetTwinEntities())
	{
		if (&*TwinEdge->GetLoop()->GetFace() == this)
		{
			return TwinEdge;
		}
	}

	return nullptr;
}

void FTopologicalFace::FillTopologyReport(FTopologyReport& Report) const
{
	Report.Add(this);

	for (const TSharedPtr<FTopologicalLoop>& Loop : GetLoops())
	{
		for (const FOrientedEdge& Edge : Loop->GetEdges())
		{
			Report.Add(Edge.Entity.Get());
		}
	}
}

void FTopologicalFace::GetEdgeIndex(const FTopologicalEdge& Edge, int32& OutBoundaryIndex, int32& OutEdgeIndex) const
{
	OutEdgeIndex = INDEX_NONE;
	for (OutBoundaryIndex = 0; OutBoundaryIndex < Loops.Num(); ++OutBoundaryIndex)
	{
		TSharedPtr<FTopologicalLoop> Loop = Loops[OutBoundaryIndex];
		if ((OutEdgeIndex = Loop->GetEdgeIndex(Edge)) >= 0)
		{
			return;
		}
	}
	OutBoundaryIndex = INDEX_NONE;
}

void FTopologicalFace::EvaluateGrid(FGrid& Grid) const
{
	CarrierSurface->EvaluateGrid(Grid);
}

const void FTopologicalFace::Get2DLoopSampling(TArray<TArray<FPoint2D>>& LoopSamplings) const
{
	LoopSamplings.Empty(GetLoops().Num());

	for (const TSharedPtr<FTopologicalLoop>& Loop : GetLoops())
	{
		TArray<FPoint2D>& LoopSampling2D = LoopSamplings.Emplace_GetRef();
		Loop->Get2DSampling(LoopSampling2D);
	}
}

void FTopologicalFace::SpawnIdent(FDatabase& Database)
{
	if (!FEntity::SetId(Database))
	{
		return;
	}

	SpawnIdentOnEntities(Loops, Database);
	CarrierSurface->SpawnIdent(Database);
	if (Mesh.IsValid())
	{
		Mesh->SpawnIdent(Database);
	}
}

#ifdef CADKERNEL_DEV
FInfoEntity& FTopologicalFace::GetInfo(FInfoEntity& Info) const
{
	return FTopologicalShapeEntity::GetInfo(Info)
		.Add(TEXT("Carrier Surface"), CarrierSurface)
		.Add(TEXT("Boundary"), (FSurfacicBoundary&) Boundary)
		.Add(TEXT("Loops"), Loops)
		.Add(TEXT("QuadCriteria"), QuadCriteria)
		.Add(TEXT("Mesh"), Mesh);
}
#endif

TSharedRef<FFaceMesh> FTopologicalFace::GetOrCreateMesh(FModelMesh& MeshModel)
{
	if (!Mesh.IsValid())
	{
		Mesh = FEntity::MakeShared<FFaceMesh>(MeshModel, *this);
	}
	return Mesh.ToSharedRef();
}

void FTopologicalFace::InitDeltaUs()
{
	CrossingPointDeltaMins[EIso::IsoU].Init(SMALL_NUMBER, CrossingCoordinates[EIso::IsoU].Num() - 1);
	CrossingPointDeltaMaxs[EIso::IsoU].Init(HUGE_VALUE, CrossingCoordinates[EIso::IsoU].Num() - 1);

	CrossingPointDeltaMins[EIso::IsoV].Init(SMALL_NUMBER, CrossingCoordinates[EIso::IsoV].Num() - 1);
	CrossingPointDeltaMaxs[EIso::IsoV].Init(HUGE_VALUE, CrossingCoordinates[EIso::IsoV].Num() - 1);
}

void FTopologicalFace::ChooseFinalDeltaUs()
{
	TFunction<void(const TArray<double>&, TArray<double>&)> ChooseFinalDeltas = [](const TArray<double>& DeltaUMins, TArray<double>& DeltaUMaxs)
	{
		int32 Index = 0;
		for (; Index < DeltaUMins.Num(); ++Index)
		{
			if (DeltaUMins[Index] > DeltaUMaxs[Index])
			{
				DeltaUMaxs[Index] = DeltaUMins[Index];
			}
		}
	};

	ChooseFinalDeltas(CrossingPointDeltaMins[EIso::IsoU], CrossingPointDeltaMaxs[EIso::IsoU]);
	ChooseFinalDeltas(CrossingPointDeltaMins[EIso::IsoV], CrossingPointDeltaMaxs[EIso::IsoV]);
}

// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
//
//
//                                                                            NOT YET REVIEWED
//
//
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================
// =========================================================================================================================================================================================================

// Quad ==============================================================================================================================================================================================================================

double FTopologicalFace::GetQuadCriteria()
{
	if (GetQuadType() == EQuadType::Unset)
	{
		return 0;
	}
	return QuadCriteria;
}

void FTopologicalFace::ComputeQuadCriteria()
{
	if (GetQuadType() != EQuadType::Unset)
	{
		QuadCriteria = FMath::Max(Curvatures[EIso::IsoU].Max, Curvatures[EIso::IsoU].Max);
	}
}

void FTopologicalFace::ComputeSurfaceSideProperties()
{
	TFunction<double(const int32)> GetSideLength = [&](const int32 SideIndex)
	{
		TSharedPtr<FTopologicalLoop> Loop = GetLoops()[0];

		double Length = 0;
		int32 NextSideIndex = SideIndex + 1;
		if (NextSideIndex == GetStartSideIndices().Num())
		{
			NextSideIndex = 0;
		}
		int32 EndIndex = GetStartSideIndices()[NextSideIndex];
		for (int32 Index = GetStartSideIndices()[SideIndex]; Index != EndIndex;)
		{
			Length += Loop->GetEdge(Index)->Length();
			if (++Index == Loop->EdgeCount())
			{
				Index = 0;
			}
		}
		return Length;
	};

	Loops[0]->FindSurfaceCorners(SurfaceCorners, StartSideIndices);
	Loops[0]->ComputeBoundaryProperties(StartSideIndices, SideProperties);

	LoopLength = 0;
	for (int32 Index = 0; Index < SurfaceCorners.Num(); ++Index)
	{
		SideProperties[Index].Length3D = GetSideLength(Index);
		LoopLength += SideProperties[Index].Length3D;
	}
}

void FTopologicalFace::DefineSurfaceType()
{
	if (!CarrierSurface.IsValid())
	{
		return;
	}

	const double Tolerance3D = CarrierSurface->Get3DTolerance();
	const double GeometricTolerance = 20.0 * Tolerance3D;

	switch (SurfaceCorners.Num())
	{
	case 3:
		QuadType = EQuadType::Triangular;
		break;

	case 4:
		QuadType = EQuadType::Other;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			// If the type is not ISO, the neighbor surface is checked, if it's quad so it's ok...
			if (SideProperties[Index].IsoType == EIso::UndefinedIso)
			{
				TSharedPtr<FTopologicalEdge> Edge = Loops[0]->GetEdge(StartSideIndices[Index]);
				int32 NeighborsNum = Edge->GetTwinEntityCount();
				// If non manifold Edge => Stop
				if (NeighborsNum != 2)
				{
					return;
				}

				{
					int32 OppositIndex = (Index + 2) % 4;
					SideProperties[Index].IsoType = SideProperties[OppositIndex].IsoType;
					if (SideProperties[Index].IsoType == EIso::UndefinedIso)
					{
						int32 AdjacentIndex = (Index + 1) % 4;
						if (SideProperties[AdjacentIndex].IsoType != EIso::UndefinedIso)
						{
							SideProperties[Index].IsoType = (SideProperties[AdjacentIndex].IsoType == EIso::IsoU) ? EIso::IsoV : EIso::IsoU;
						}
					}
				}

				FTopologicalFace* Neighbor = nullptr;
				{
					for(FTopologicalEdge* NeighborEdge : Edge->GetTwinEntities() )
					{
						if (NeighborEdge == Edge.Get())
						{
							continue;
						}
						Neighbor = Edge->GetLoop()->GetFace();
					}
				}

				// it's not a quad surface
				if (Neighbor == nullptr || Neighbor->SurfaceCorners.Num() == 0)
				{
					return;
				}

				FTopologicalEdge* TwinEdge = Edge->GetFirstTwinEdge();
				if(!TwinEdge)
				{
					return;
				}

				int32 SideIndex = Neighbor->GetSideIndex(*TwinEdge);
				if (SideIndex < 0)
				{
					return;
				}

				const FEdge2DProperties& Property = Neighbor->GetSideProperty(SideIndex);
				if (Property.IsoType == EIso::UndefinedIso)
				{
					return;
				}

				double SideLength = SideProperties[Index].Length3D;
				double OtherSideLength = Property.Length3D;

				if (FMath::Abs(SideLength - OtherSideLength) < GeometricTolerance)
				{
					int32 OppositIndex = (Index + 2) % 4;
					if (SideProperties[OppositIndex].IsoType == EIso::UndefinedIso)
					{
						if (Index < 2)
						{
							if (SideProperties[!Index].IsoType == EIso::IsoU)
							{
								SideProperties[Index].IsoType = EIso::IsoV;
							}
							else
							{
								SideProperties[Index].IsoType = EIso::IsoU;
							}
						}
						return;
					}
					SideProperties[Index].IsoType = SideProperties[OppositIndex].IsoType;
				}
			}
		}

		if ((SideProperties[0].IsoType != EIso::UndefinedIso) && (SideProperties[1].IsoType != EIso::UndefinedIso) && (SideProperties[0].IsoType == SideProperties[2].IsoType) && (SideProperties[1].IsoType == SideProperties[3].IsoType))
		{
			QuadType = EQuadType::Quadrangular;
		}
		break;

	default:
		QuadType = EQuadType::Other;
		break;
	}
}


void FFaceSubset::SetMainShell(TMap<FTopologicalShapeEntity*, int32>& ShellToFaceCount)
{
	if (ShellToFaceCount.Num() == 0)
	{
		return;
	}

	int32 MaxFaceCount = 0;
	FTopologicalShapeEntity* CandidateShell = nullptr;

	for (TPair<FTopologicalShapeEntity*, int32>& Pair : ShellToFaceCount)
	{
		if (Pair.Value > MaxFaceCount)
		{
			MaxFaceCount = Pair.Value;
			CandidateShell = Pair.Key;
		}
	}

	if ((CandidateShell != nullptr) && ((CandidateShell->FaceCount() / 2 + 1) < MaxFaceCount))
	{
		MainShell = CandidateShell;
	}
}

void FFaceSubset::SetMainBody(TMap<FTopologicalShapeEntity*, int32>& BodyToFaceCount)
{
	if (BodyToFaceCount.Num() == 0)
	{
		return;
	}

	FTopologicalShapeEntity* CandidateBody = nullptr;
	int32 MaxFaceCount = 0;
	for (TPair<FTopologicalShapeEntity*, int32>& Pair : BodyToFaceCount)
	{
		if (Pair.Value > MaxFaceCount)
		{
			MaxFaceCount = Pair.Value;
			CandidateBody = Pair.Key;
		}
	}

	// if Faces come mainly from MainBody
	if ((CandidateBody != nullptr) && ((Faces.Num() / 2) <= MaxFaceCount))
	{
		MainBody = CandidateBody;
	}
}

void FFaceSubset::SetMainName(TMap<FString, int32>& NameToFaceCount)
{
	if (NameToFaceCount.Num() == 0)
	{
		return;
	}

	int32 MaxInstance = Faces.Num() / 3;
	for (TPair<FString, int32>& Pair : NameToFaceCount)
	{
		if (Pair.Value > MaxInstance)
		{
			MaxInstance = Pair.Value;
			MainName = Pair.Key;
		}
	}
}

void FFaceSubset::SetMainColor(TMap<uint32, int32>& ColorToFaceCount)
{
	int32 MaxInstance = 0;
	for (TPair<uint32, int32>& Pair : ColorToFaceCount)
	{
		if (Pair.Value > MaxInstance)
		{
			MaxInstance = Pair.Value;
			MainColor = Pair.Key;
		}
	}
}

void FFaceSubset::SetMainMaterial(TMap<uint32, int32>& MaterialToFaceCount)
{
	int32 MaxInstance = 0;
	for (TPair<uint32, int32>& Pair : MaterialToFaceCount)
	{
		if (Pair.Value > MaxInstance)
		{
			MaxInstance = Pair.Value;
			MainColor = Pair.Key;
		}
	}
}

}