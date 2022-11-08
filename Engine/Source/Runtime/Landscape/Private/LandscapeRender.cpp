// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
LandscapeRender.cpp: New terrain rendering
=============================================================================*/

#include "LandscapeRender.h"
#include "LightMap.h"
#include "ShadowMap.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapePrivate.h"
#include "LandscapeMeshProxyComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ShaderParameterUtils.h"
#include "LandscapeEdit.h"
#include "Engine/LevelStreaming.h"
#include "LevelUtils.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "LandscapeMaterialInstanceConstant.h"
#include "Engine/ShadowMapTexture2D.h"
#include "EngineGlobals.h"
#include "EngineModule.h"
#include "UnrealEngine.h"
#include "LandscapeLight.h"
#include "Algo/Find.h"
#include "Engine/StaticMesh.h"
#include "LandscapeInfo.h"
#include "LandscapeDataAccess.h"
#include "DrawDebugHelpers.h"
#include "PrimitiveSceneInfo.h"
#include "SceneView.h"
#include "Runtime/Renderer/Private/SceneCore.h"
#include "LandscapeProxy.h"
#include "HAL/LowLevelMemTracker.h"
#include "MeshMaterialShader.h"
#include "VT/RuntimeVirtualTexture.h"
#include "RayTracingInstance.h"
#include "ProfilingDebugging/LoadTimeTracker.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeUniformShaderParameters, "LandscapeParameters");
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeFixedGridUniformShaderParameters, "LandscapeFixedGrid");
IMPLEMENT_TYPE_LAYOUT(FLandscapeVertexFactoryPixelShaderParameters);

#if !UE_BUILD_SHIPPING
static void OnLODDistributionScaleChanged(IConsoleVariable* CVar)
{
	for (auto* LandscapeComponent : TObjectRange<ULandscapeComponent>(RF_ClassDefaultObject | RF_ArchetypeObject, true, EInternalObjectFlags::Garbage))
	{
		LandscapeComponent->MarkRenderStateDirty();
	}
}
#endif

float GLandscapeLOD0DistributionScale = 1.f;
FAutoConsoleVariableRef CVarLandscapeLOD0DistributionScale(
	TEXT("r.LandscapeLOD0DistributionScale"),
	GLandscapeLOD0DistributionScale,
	TEXT("Multiplier for the landscape LOD0DistributionSetting property"),
#if !UE_BUILD_SHIPPING
	FConsoleVariableDelegate::CreateStatic(&OnLODDistributionScaleChanged),
#endif
	ECVF_Scalability
);

float GLandscapeLODDistributionScale = 1.f;
FAutoConsoleVariableRef CVarLandscapeLODDistributionScale(
	TEXT("r.LandscapeLODDistributionScale"),
	GLandscapeLODDistributionScale,
	TEXT("Multiplier for the landscape LODDistributionSetting property"),
#if !UE_BUILD_SHIPPING
	FConsoleVariableDelegate::CreateStatic(&OnLODDistributionScaleChanged),
#endif
	ECVF_Scalability
);

int32 GAllowLandscapeShadows = 1;
static FAutoConsoleVariableRef CVarAllowLandscapeShadows(
	TEXT("r.AllowLandscapeShadows"),
	GAllowLandscapeShadows,
	TEXT("Allow Landscape Shadows")
);

#if WITH_EDITOR
extern TAutoConsoleVariable<int32> CVarLandscapeShowDirty;
#endif

extern RENDERER_API TAutoConsoleVariable<float> CVarStaticMeshLODDistanceScale;

#if !UE_BUILD_SHIPPING
int32 GVarDumpLandscapeLODsCurrentFrame = 0;
bool GVarDumpLandscapeLODs = false;

static void OnDumpLandscapeLODs(const TArray< FString >& Args)
{
	if (Args.Num() >= 1)
	{
		GVarDumpLandscapeLODs = FCString::Atoi(*Args[0]) == 0 ? false : true;
	}

	// Add some buffer to be able to correctly catch the frame during the rendering
	GVarDumpLandscapeLODsCurrentFrame = GVarDumpLandscapeLODs ? GFrameNumberRenderThread + 3 : INDEX_NONE;
}

static FAutoConsoleCommand CVarDumpLandscapeLODs(
	TEXT("Landscape.DumpLODs"),
	TEXT("Will dump the current status of LOD value and current texture streaming status"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&OnDumpLandscapeLODs)
);
#endif

#if WITH_EDITOR
LANDSCAPE_API int32 GLandscapeViewMode = ELandscapeViewMode::Normal;
FAutoConsoleVariableRef CVarLandscapeDebugViewMode(
	TEXT("Landscape.DebugViewMode"),
	GLandscapeViewMode,
	TEXT("Change the view mode of the landscape rendering. Valid Input: 0 = Normal, 2 = DebugLayer, 3 = LayerDensity, 4 = LayerUsage, 5 = LOD Distribution, 6 = WireframeOnTop, 7 = LayerContribution"),
	ECVF_Cheat
);
#endif

#if RHI_RAYTRACING
static TAutoConsoleVariable<int32> CVarRayTracingLandscape(
	TEXT("r.RayTracing.Geometry.Landscape"),
	1,
	TEXT("Include landscapes in ray tracing effects (default = 1 (landscape enabled in ray tracing))"));

int32 GLandscapeRayTracingGeometryLODsThatUpdateEveryFrame = 0;
static FAutoConsoleVariableRef CVarLandscapeRayTracingGeometryLODsThatUpdateEveryFrame(
	TEXT("r.RayTracing.Geometry.Landscape.LODsUpdateEveryFrame"),
	GLandscapeRayTracingGeometryLODsThatUpdateEveryFrame,
	TEXT("If on, LODs that are lower than the specified level will be updated every frame, which can be used to workaround some artifacts caused by texture streaming if you're using WorldPositionOffset on the landscape")
);

int32 GLandscapeRayTracingGeometryDetectTextureStreaming = 1;
static FAutoConsoleVariableRef CVarLandscapeRayTracingGeometryDetectTextureStreaming(
	TEXT("r.RayTracing.Geometry.Landscape.DetectTextureStreaming"),
	GLandscapeRayTracingGeometryDetectTextureStreaming,
	TEXT("If on, update ray tracing geometry when texture streaming state changes. Useful when WorldPositionOffset is used in the landscape material")
);
#endif

/*------------------------------------------------------------------------------
Forsyth algorithm for cache optimizing index buffers.
------------------------------------------------------------------------------*/

// Forsyth algorithm to optimize post-transformed vertex cache
namespace
{
	// code for computing vertex score was taken, as much as possible
	// directly from the original publication.
	float ComputeVertexCacheScore(int32 CachePosition, uint32 VertexCacheSize)
	{
		const float FindVertexScoreCacheDecayPower = 1.5f;
		const float FindVertexScoreLastTriScore = 0.75f;

		float Score = 0.0f;
		if (CachePosition < 0)
		{
			// Vertex is not in FIFO cache - no score.
		}
		else
		{
			if (CachePosition < 3)
			{
				// This vertex was used in the last triangle,
				// so it has a fixed score, whichever of the three
				// it's in. Otherwise, you can get very different
				// answers depending on whether you add
				// the triangle 1,2,3 or 3,1,2 - which is silly.
				Score = FindVertexScoreLastTriScore;
			}
			else
			{
				check(CachePosition < (int32)VertexCacheSize);
				// Points for being high in the cache.
				const float Scaler = 1.0f / (VertexCacheSize - 3);
				Score = 1.0f - (CachePosition - 3) * Scaler;
				Score = FMath::Pow(Score, FindVertexScoreCacheDecayPower);
			}
		}

		return Score;
	}

	float ComputeVertexValenceScore(uint32 numActiveFaces)
	{
		const float FindVertexScoreValenceBoostScale = 2.0f;
		const float FindVertexScoreValenceBoostPower = 0.5f;

		float Score = 0.f;

		// Bonus points for having a low number of tris still to
		// use the vert, so we get rid of lone verts quickly.
		float ValenceBoost = FMath::Pow(float(numActiveFaces), -FindVertexScoreValenceBoostPower);
		Score += FindVertexScoreValenceBoostScale * ValenceBoost;

		return Score;
	}

	const uint32 MaxVertexCacheSize = 64;
	const uint32 MaxPrecomputedVertexValenceScores = 64;
	float VertexCacheScores[MaxVertexCacheSize + 1][MaxVertexCacheSize];
	float VertexValenceScores[MaxPrecomputedVertexValenceScores];
	bool bVertexScoresComputed = false; //ComputeVertexScores();

	bool ComputeVertexScores()
	{
		for (uint32 CacheSize = 0; CacheSize <= MaxVertexCacheSize; ++CacheSize)
		{
			for (uint32 CachePos = 0; CachePos < CacheSize; ++CachePos)
			{
				VertexCacheScores[CacheSize][CachePos] = ComputeVertexCacheScore(CachePos, CacheSize);
			}
		}

		for (uint32 Valence = 0; Valence < MaxPrecomputedVertexValenceScores; ++Valence)
		{
			VertexValenceScores[Valence] = ComputeVertexValenceScore(Valence);
		}

		return true;
	}

	inline float FindVertexCacheScore(uint32 CachePosition, uint32 MaxSizeVertexCache)
	{
		return VertexCacheScores[MaxSizeVertexCache][CachePosition];
	}

	inline float FindVertexValenceScore(uint32 NumActiveTris)
	{
		return VertexValenceScores[NumActiveTris];
	}

	float FindVertexScore(uint32 NumActiveFaces, uint32 CachePosition, uint32 VertexCacheSize)
	{
		check(bVertexScoresComputed);

		if (NumActiveFaces == 0)
		{
			// No tri needs this vertex!
			return -1.0f;
		}

		float Score = 0.f;
		if (CachePosition < VertexCacheSize)
		{
			Score += VertexCacheScores[VertexCacheSize][CachePosition];
		}

		if (NumActiveFaces < MaxPrecomputedVertexValenceScores)
		{
			Score += VertexValenceScores[NumActiveFaces];
		}
		else
		{
			Score += ComputeVertexValenceScore(NumActiveFaces);
		}

		return Score;
	}

	struct OptimizeVertexData
	{
		float  Score;
		uint32  ActiveFaceListStart;
		uint32  ActiveFaceListSize;
		uint32  CachePos0;
		uint32  CachePos1;
		OptimizeVertexData() : Score(0.f), ActiveFaceListStart(0), ActiveFaceListSize(0), CachePos0(0), CachePos1(0) { }
	};

	//-----------------------------------------------------------------------------
	//  OptimizeFaces
	//-----------------------------------------------------------------------------
	//  Parameters:
	//      InIndexList
	//          input index list
	//      OutIndexList
	//          a pointer to a preallocated buffer the same size as indexList to
	//          hold the optimized index list
	//      LRUCacheSize
	//          the size of the simulated post-transform cache (max:64)
	//-----------------------------------------------------------------------------

	template <typename INDEX_TYPE>
	void OptimizeFaces(const TArray<INDEX_TYPE>& InIndexList, TArray<INDEX_TYPE>& OutIndexList, uint16 LRUCacheSize)
	{
		uint32 VertexCount = 0;
		const uint32 IndexCount = InIndexList.Num();

		// compute face count per vertex
		for (uint32 i = 0; i < IndexCount; ++i)
		{
			uint32 Index = InIndexList[i];
			VertexCount = FMath::Max(Index, VertexCount);
		}
		VertexCount++;

		TArray<OptimizeVertexData> VertexDataList;
		VertexDataList.Empty(VertexCount);
		for (uint32 i = 0; i < VertexCount; i++)
		{
			VertexDataList.Add(OptimizeVertexData());
		}

		OutIndexList.Empty(IndexCount);
		OutIndexList.AddZeroed(IndexCount);

		// compute face count per vertex
		for (uint32 i = 0; i < IndexCount; ++i)
		{
			uint32 Index = InIndexList[i];
			OptimizeVertexData& VertexData = VertexDataList[Index];
			VertexData.ActiveFaceListSize++;
		}

		TArray<uint32> ActiveFaceList;

		const uint32 EvictedCacheIndex = TNumericLimits<uint32>::Max();

		{
			// allocate face list per vertex
			uint32 CurActiveFaceListPos = 0;
			for (uint32 i = 0; i < VertexCount; ++i)
			{
				OptimizeVertexData& VertexData = VertexDataList[i];
				VertexData.CachePos0 = EvictedCacheIndex;
				VertexData.CachePos1 = EvictedCacheIndex;
				VertexData.ActiveFaceListStart = CurActiveFaceListPos;
				CurActiveFaceListPos += VertexData.ActiveFaceListSize;
				VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos0, LRUCacheSize);
				VertexData.ActiveFaceListSize = 0;
			}
			ActiveFaceList.Empty(CurActiveFaceListPos);
			ActiveFaceList.AddZeroed(CurActiveFaceListPos);
		}

		// fill out face list per vertex
		for (uint32 i = 0; i < IndexCount; i += 3)
		{
			for (uint32 j = 0; j < 3; ++j)
			{
				uint32 Index = InIndexList[i + j];
				OptimizeVertexData& VertexData = VertexDataList[Index];
				ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize] = i;
				VertexData.ActiveFaceListSize++;
			}
		}

		TArray<uint8> ProcessedFaceList;
		ProcessedFaceList.Empty(IndexCount);
		ProcessedFaceList.AddZeroed(IndexCount);

		uint32 VertexCacheBuffer[(MaxVertexCacheSize + 3) * 2];
		uint32* Cache0 = VertexCacheBuffer;
		uint32* Cache1 = VertexCacheBuffer + (MaxVertexCacheSize + 3);
		uint32 EntriesInCache0 = 0;

		uint32 BestFace = 0;
		float BestScore = -1.f;

		const float MaxValenceScore = FindVertexScore(1, EvictedCacheIndex, LRUCacheSize) * 3.f;

		for (uint32 i = 0; i < IndexCount; i += 3)
		{
			if (BestScore < 0.f)
			{
				// no verts in the cache are used by any unprocessed faces so
				// search all unprocessed faces for a new starting point
				for (uint32 j = 0; j < IndexCount; j += 3)
				{
					if (ProcessedFaceList[j] == 0)
					{
						uint32 Face = j;
						float FaceScore = 0.f;
						for (uint32 k = 0; k < 3; ++k)
						{
							uint32 Index = InIndexList[Face + k];
							OptimizeVertexData& VertexData = VertexDataList[Index];
							check(VertexData.ActiveFaceListSize > 0);
							check(VertexData.CachePos0 >= LRUCacheSize);
							FaceScore += VertexData.Score;
						}

						if (FaceScore > BestScore)
						{
							BestScore = FaceScore;
							BestFace = Face;

							check(BestScore <= MaxValenceScore);
							if (BestScore >= MaxValenceScore)
							{
								break;
							}
						}
					}
				}
				check(BestScore >= 0.f);
			}

			ProcessedFaceList[BestFace] = 1;
			uint32 EntriesInCache1 = 0;

			// add bestFace to LRU cache and to newIndexList
			for (uint32 V = 0; V < 3; ++V)
			{
				INDEX_TYPE Index = InIndexList[BestFace + V];
				OutIndexList[i + V] = Index;

				OptimizeVertexData& VertexData = VertexDataList[Index];

				if (VertexData.CachePos1 >= EntriesInCache1)
				{
					VertexData.CachePos1 = EntriesInCache1;
					Cache1[EntriesInCache1++] = Index;

					if (VertexData.ActiveFaceListSize == 1)
					{
						--VertexData.ActiveFaceListSize;
						continue;
					}
				}

				check(VertexData.ActiveFaceListSize > 0);
				uint32 FindIndex;
				for (FindIndex = VertexData.ActiveFaceListStart; FindIndex < VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize; FindIndex++)
				{
					if (ActiveFaceList[FindIndex] == BestFace)
					{
						break;
					}
				}
				check(FindIndex != VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize);

				if (FindIndex != VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1)
				{
					uint32 SwapTemp = ActiveFaceList[FindIndex];
					ActiveFaceList[FindIndex] = ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1];
					ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1] = SwapTemp;
				}

				--VertexData.ActiveFaceListSize;
				VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos1, LRUCacheSize);

			}

			// move the rest of the old verts in the cache down and compute their new scores
			for (uint32 C0 = 0; C0 < EntriesInCache0; ++C0)
			{
				uint32 Index = Cache0[C0];
				OptimizeVertexData& VertexData = VertexDataList[Index];

				if (VertexData.CachePos1 >= EntriesInCache1)
				{
					VertexData.CachePos1 = EntriesInCache1;
					Cache1[EntriesInCache1++] = Index;
					VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos1, LRUCacheSize);
				}
			}

			// find the best scoring triangle in the current cache (including up to 3 that were just evicted)
			BestScore = -1.f;
			for (uint32 C1 = 0; C1 < EntriesInCache1; ++C1)
			{
				uint32 Index = Cache1[C1];
				OptimizeVertexData& VertexData = VertexDataList[Index];
				VertexData.CachePos0 = VertexData.CachePos1;
				VertexData.CachePos1 = EvictedCacheIndex;
				for (uint32 j = 0; j < VertexData.ActiveFaceListSize; ++j)
				{
					uint32 Face = ActiveFaceList[VertexData.ActiveFaceListStart + j];
					float FaceScore = 0.f;
					for (uint32 V = 0; V < 3; V++)
					{
						uint32 FaceIndex = InIndexList[Face + V];
						OptimizeVertexData& FaceVertexData = VertexDataList[FaceIndex];
						FaceScore += FaceVertexData.Score;
					}
					if (FaceScore > BestScore)
					{
						BestScore = FaceScore;
						BestFace = Face;
					}
				}
			}

			uint32* SwapTemp = Cache0;
			Cache0 = Cache1;
			Cache1 = SwapTemp;

			EntriesInCache0 = FMath::Min(EntriesInCache1, (uint32)LRUCacheSize);
		}
	}

} // namespace 


//
// FLandscapeDebugOptions
//
FLandscapeDebugOptions::FLandscapeDebugOptions()
	: bShowPatches(false)
	, bDisableStatic(false)
	, CombineMode(eCombineMode_Default)
	, PatchesConsoleCommand(
		TEXT("Landscape.Patches"),
		TEXT("Show/hide Landscape patches"),
		FConsoleCommandDelegate::CreateRaw(this, &FLandscapeDebugOptions::Patches))
	, StaticConsoleCommand(
		TEXT("Landscape.Static"),
		TEXT("Enable/disable Landscape static drawlists"),
		FConsoleCommandDelegate::CreateRaw(this, &FLandscapeDebugOptions::Static))
	, CombineConsoleCommand(
		TEXT("Landscape.Combine"),
		TEXT("Set landscape component combining mode : 0 = Default, 1 = Combine All, 2 = Disabled"),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FLandscapeDebugOptions::Combine))
{
}

void FLandscapeDebugOptions::Patches()
{
	bShowPatches = !bShowPatches;
	UE_LOG(LogLandscape, Display, TEXT("Landscape.Patches: %s"), bShowPatches ? TEXT("Show") : TEXT("Hide"));
}

void FLandscapeDebugOptions::Static()
{
	bDisableStatic = !bDisableStatic;
	UE_LOG(LogLandscape, Display, TEXT("Landscape.Static: %s"), bDisableStatic ? TEXT("Disabled") : TEXT("Enabled"));
}

void FLandscapeDebugOptions::Combine(const TArray<FString>& Args)
{
	if (Args.Num() >= 1)
	{
		CombineMode = (eCombineMode)FCString::Atoi(*Args[0]);
		UE_LOG(LogLandscape, Display, TEXT("Landscape.Combine: %d"), (int32)CombineMode);
	}
}

FLandscapeDebugOptions GLandscapeDebugOptions;


#if WITH_EDITOR
LANDSCAPE_API bool GLandscapeEditModeActive = false;
LANDSCAPE_API int32 GLandscapeEditRenderMode = ELandscapeEditRenderMode::None;
UMaterialInterface* GLayerDebugColorMaterial = nullptr;
UMaterialInterface* GSelectionColorMaterial = nullptr;
UMaterialInterface* GSelectionRegionMaterial = nullptr;
UMaterialInterface* GMaskRegionMaterial = nullptr;
UMaterialInterface* GColorMaskRegionMaterial = nullptr;
UTexture2D* GLandscapeBlackTexture = nullptr;
UMaterialInterface* GLandscapeLayerUsageMaterial = nullptr;
UMaterialInterface* GLandscapeDirtyMaterial = nullptr;
#endif

void ULandscapeComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// TODO - investigate whether this is correct

	ALandscapeProxy* Actor = GetLandscapeProxy();

	if (Actor != nullptr && Actor->bUseDynamicMaterialInstance)
	{
		OutMaterials.Append(MaterialInstancesDynamic.FilterByPredicate([](UMaterialInstanceDynamic* MaterialInstance) { return MaterialInstance != nullptr; }));
	}
	else
	{
		OutMaterials.Append(MaterialInstances.FilterByPredicate([](UMaterialInstanceConstant* MaterialInstance) { return MaterialInstance != nullptr; }));
	}

	if (OverrideMaterial)
	{
		OutMaterials.Add(OverrideMaterial);
	}

	if (OverrideHoleMaterial)
	{
		OutMaterials.Add(OverrideHoleMaterial);
	}

	OutMaterials.Append(MobileMaterialInterfaces);

#if WITH_EDITORONLY_DATA
	if (EditToolRenderData.ToolMaterial)
	{
		OutMaterials.Add(EditToolRenderData.ToolMaterial);
	}

	if (EditToolRenderData.GizmoMaterial)
	{
		OutMaterials.Add(EditToolRenderData.GizmoMaterial);
	}
#endif

#if WITH_EDITOR
	//if (bGetDebugMaterials) // TODO: This should be tested and enabled
	{
		OutMaterials.Add(GLayerDebugColorMaterial);
		OutMaterials.Add(GSelectionColorMaterial);
		OutMaterials.Add(GSelectionRegionMaterial);
		OutMaterials.Add(GMaskRegionMaterial);
		OutMaterials.Add(GColorMaskRegionMaterial);
		OutMaterials.Add(GLandscapeLayerUsageMaterial);
		OutMaterials.Add(GLandscapeDirtyMaterial);
	}
#endif
}

/**
 * Return any global Lod override for landscape.
 * A return value less than 0 means no override.
 * Any positive value must still be clamped into the valid Lod range for the landscape.
 */
static int32 GetViewLodOverride(FSceneView const& View)
{
	// Apply r.ForceLOD override
	int32 LodOverride = GetCVarForceLOD_AnyThread();
#if WITH_EDITOR
	// Apply editor landscape lod override
	LodOverride = View.Family->LandscapeLODOverride >= 0 ? View.Family->LandscapeLODOverride : LodOverride;
#endif
	// Use lod 0 if lodding is disabled
	LodOverride = View.Family->EngineShowFlags.LOD == 0 ? 0 : LodOverride;
	return LodOverride;
}

static int32 GetDrawCollisionLodOverride(bool bShowCollisionPawn, bool bShowCollisionVisibility, int32 DrawCollisionPawnLOD, int32 DrawCollisionVisibilityLOD)
{
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	return bShowCollisionPawn ? FMath::Max(DrawCollisionPawnLOD, DrawCollisionVisibilityLOD) : bShowCollisionVisibility ? DrawCollisionVisibilityLOD : -1;
#else
	return -1;
#endif
}

static int32 GetDrawCollisionLodOverride(FSceneView const& View, FCollisionResponseContainer const& CollisionResponse, int32 CollisionLod, int32 SimpleCollisionLod)
{
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	bool bShowCollisionPawn = View.Family->EngineShowFlags.CollisionPawn;
	bool bShowCollisionVisibility = View.Family->EngineShowFlags.CollisionVisibility;
	int32 DrawCollisionPawnLOD = CollisionResponse.GetResponse(ECC_Pawn) == ECR_Ignore ? -1 : SimpleCollisionLod;
	int32 DrawCollisionVisibilityLOD = CollisionResponse.GetResponse(ECC_Visibility) == ECR_Ignore ? -1 : CollisionLod;
	return GetDrawCollisionLodOverride(bShowCollisionPawn, bShowCollisionVisibility, DrawCollisionPawnLOD, DrawCollisionVisibilityLOD);
#else
	return -1;
#endif
}


//
// FLandscapeComponentSceneProxy
//
TMap<uint32, FLandscapeSharedBuffers*>FLandscapeComponentSceneProxy::SharedBuffersMap;

const static FName NAME_LandscapeResourceNameForDebugging(TEXT("Landscape"));

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeSectionLODUniformParameters, "LandscapeContinuousLODParameters");

TMap<uint32, FLandscapeRenderSystem*> LandscapeRenderSystems;

TBitArray<> FLandscapeRenderSystem::LandscapeIndexAllocator;


//
// FLandscapeRenderSystem
//
FLandscapeRenderSystem::FLandscapeRenderSystem()
	: Min(MAX_int32, MAX_int32)
	, Size(EForceInit::ForceInitToZero)
	, ReferenceCount(0)
{
	SectionLODBiases.SetAllowCPUAccess(true);

	LandscapeIndex = LandscapeIndexAllocator.FindAndSetFirstZeroBit();
	if (LandscapeIndex == INDEX_NONE)
	{
		LandscapeIndex = LandscapeIndexAllocator.Add(true);
	}

	FLandscapeSectionLODUniformParameters Parameters;
	Parameters.LandscapeIndex = LandscapeIndex;
	Parameters.Size = FIntPoint(1, 1);
	Parameters.SectionLODBias = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
	SectionLODUniformBuffer = TUniformBufferRef<FLandscapeSectionLODUniformParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_MultiFrame);
}

FLandscapeRenderSystem::~FLandscapeRenderSystem()
{
	check(LandscapeIndexAllocator[LandscapeIndex]);
	LandscapeIndexAllocator[LandscapeIndex] = false;

	// Clear slack in the array
	int32 LastSetIndex = LandscapeIndexAllocator.FindLast(true);
	LandscapeIndexAllocator.SetNumUninitialized(LastSetIndex + 1);
}

void FLandscapeRenderSystem::CreateResources(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());

	FLandscapeRenderSystem*& LandscapeRenderSystem = LandscapeRenderSystems.FindOrAdd(SectionInfo->LandscapeKey);
	if (!LandscapeRenderSystem)
	{
		LandscapeRenderSystem = new FLandscapeRenderSystem();
	}

	LandscapeRenderSystem->CreateResources_Internal(SectionInfo);
}

void FLandscapeRenderSystem::DestroyResources(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());

	FLandscapeRenderSystem* LandscapeRenderSystem = LandscapeRenderSystems.FindChecked(SectionInfo->LandscapeKey);
	LandscapeRenderSystem->DestroyResources_Internal(SectionInfo);

	if (LandscapeRenderSystem->ReferenceCount == 0)
	{
		delete LandscapeRenderSystem;
		LandscapeRenderSystems.Remove(SectionInfo->LandscapeKey);
	}
}

void FLandscapeRenderSystem::CreateResources_Internal(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());
	check(SectionInfo != nullptr);
	check(!SectionInfo->bRegistered);

	if (!SectionInfos.IsEmpty())
	{
		// Calculate new bounding rect of landscape components
		FIntPoint OriginalMin = Min;
		FIntPoint OriginalMax = Min + Size - FIntPoint(1, 1);
		FIntPoint NewMin(FMath::Min(Min.X, SectionInfo->ComponentBase.X), FMath::Min(Min.Y, SectionInfo->ComponentBase.Y));
		FIntPoint NewMax(FMath::Max(OriginalMax.X, SectionInfo->ComponentBase.X), FMath::Max(OriginalMax.Y, SectionInfo->ComponentBase.Y));

		FIntPoint SizeRequired = (NewMax - NewMin) + FIntPoint(1, 1);

		if (NewMin != Min || Size != SizeRequired)
		{
			ResizeAndMoveTo(NewMin, SizeRequired);
		}
	}
	else
	{
		ResizeAndMoveTo(SectionInfo->ComponentBase, FIntPoint(1, 1));
	}

	ReferenceCount++;
}

void FLandscapeRenderSystem::DestroyResources_Internal(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());
	check(SectionInfo != nullptr);
	check(!SectionInfo->bRegistered);

	ReferenceCount--;
}

void FLandscapeRenderSystem::RegisterSection(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());
	check(SectionInfo != nullptr);
	check(!SectionInfo->bRegistered);

	// With HLODs, it's possible to have multiple loaded sections representing the same
	// landscape patch. For example, raytracing may keep the HLOD proxy around (far field),
	// even if the actual landscape is loaded & visible.
	// We keep a linked list of the section infos, sorted by priority, so that unregistration can
	// properly restore a previously registered section info.

	FLandscapeRenderSystem*& LandscapeRenderSystem = LandscapeRenderSystems.FindChecked(SectionInfo->LandscapeKey);
	FLandscapeSectionInfo* ExistingSection = LandscapeRenderSystem->GetSectionInfo(SectionInfo->ComponentBase);
	if (ExistingSection == nullptr)
	{
		LandscapeRenderSystem->SetSectionInfo(SectionInfo->ComponentBase, SectionInfo);
	}
	else
	{
		FLandscapeSectionInfo* CurrentSection = nullptr;
		FLandscapeSectionInfo::TIterator SectionIt(ExistingSection);
		for (; SectionIt; ++SectionIt)
		{
			CurrentSection = &*SectionIt;

			// Sort on insertion
			if (SectionInfo->GetSectionPriority() < CurrentSection->GetSectionPriority())
			{
				SectionInfo->LinkBefore(CurrentSection);
				break;
			}
		}

		if (!SectionIt)
		{
			// Set as tail
			SectionInfo->LinkAfter(CurrentSection);
		}
		else if (CurrentSection == ExistingSection)
		{
			// Set as head
			LandscapeRenderSystem->SetSectionInfo(SectionInfo->ComponentBase, SectionInfo);
		}
	}

	SectionInfo->bRegistered = true;
}

void FLandscapeRenderSystem::UnregisterSection(FLandscapeSectionInfo* SectionInfo)
{
	check(IsInRenderingThread());
	check(SectionInfo != nullptr);

	if (SectionInfo->bRegistered)
	{
		FLandscapeRenderSystem* LandscapeRenderSystem = LandscapeRenderSystems.FindChecked(SectionInfo->LandscapeKey);
		FLandscapeSectionInfo* ExistingSection = LandscapeRenderSystem->GetSectionInfo(SectionInfo->ComponentBase);
		if (ExistingSection == SectionInfo)
		{
			LandscapeRenderSystem->SetSectionInfo(SectionInfo->ComponentBase, SectionInfo->GetNextLink());
		}

		SectionInfo->Unlink();

		SectionInfo->bRegistered = false;
	}
}

void FLandscapeRenderSystem::ResizeAndMoveTo(FIntPoint NewMin, FIntPoint NewSize)
{
	SectionLODBiasBuffer.SafeRelease();

	TResourceArray<float> NewSectionLODBiases;
	TArray<FLandscapeSectionInfo*> NewSectionInfos;

	NewSectionLODBiases.AddZeroed(NewSize.X * NewSize.Y);
	NewSectionInfos.AddZeroed(NewSize.X * NewSize.Y);

	for (int32 Y = 0; Y < Size.Y; Y++)
	{
		for (int32 X = 0; X < Size.X; X++)
		{
			int32 LinearIndex = Y * Size.X + X;
			int32 NewLinearIndex = (Y + (Min.Y - NewMin.Y)) * NewSize.X + (X + (Min.X - NewMin.X));

			if (NewLinearIndex >= 0 && NewLinearIndex < NewSize.X * NewSize.Y)
			{
				NewSectionLODBiases[NewLinearIndex] = SectionLODBiases[LinearIndex];
				NewSectionInfos[NewLinearIndex] = SectionInfos[LinearIndex];
			}
		}
	}

	Min = NewMin;
	Size = NewSize;
	SectionLODBiases = MoveTemp(NewSectionLODBiases);
	SectionInfos = MoveTemp(NewSectionInfos);

	SectionLODBiases.SetAllowCPUAccess(true);
}

void FLandscapeRenderSystem::BeginRender()
{
	// 5.0.2 hotfix : this function should be renamed to PreRenderViewFamily_RenderThread but has not been changed yet for keeping binary compatibility
	TRACE_CPUPROFILER_EVENT_SCOPE(FLandscapeRenderSystem::BeginRender);
	check(IsInRenderingThread());

	CachedSectionLODValues.Reset();
	FetchHeightmapLODBiases();
	UpdateBuffers();
}

const TResourceArray<float>& FLandscapeRenderSystem::ComputeSectionsLODForView(const FSceneView& InView)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLandscapeRenderSystem::ComputeSectionsLODForView);

	TResourceArray<float>& SectionLODValues = CachedSectionLODValues.Add(InView.GetViewKey());
	SectionLODValues.AddZeroed(SectionInfos.Num());

	for (int32 SectionIndex = 0; SectionIndex < SectionInfos.Num(); SectionIndex++)
	{
		const float DefaultLODValue = 0.0f;

		FLandscapeSectionInfo* SectionInfo = SectionInfos[SectionIndex];
		SectionLODValues[SectionIndex] = SectionInfo ? SectionInfo->ComputeLODForView(InView) : DefaultLODValue;
	}

	return SectionLODValues;
}

void FLandscapeRenderSystem::FetchHeightmapLODBiases()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FLandscapeRenderSystem::FetchHeightmapLODBiases);

	for (int32 SectionIndex = 0; SectionIndex < SectionInfos.Num(); SectionIndex++)
	{
		const float DefaultLODBias = 0.0f;

		FLandscapeSectionInfo* SectionInfo = SectionInfos[SectionIndex];
		SectionLODBiases[SectionIndex] = SectionInfo ? SectionInfo->ComputeLODBias() : DefaultLODBias;
	}
}

void FLandscapeRenderSystem::UpdateBuffers()
{
	check(IsInRenderingThread());

	TRACE_CPUPROFILER_EVENT_SCOPE(FLandscapeRenderSystem::UpdateBuffers);

	bool bUpdateUB = false;

	if (Size != FIntPoint::ZeroValue)
	{
		if (!SectionLODBiasBuffer.IsValid())
		{
			FRHIResourceCreateInfo CreateInfo(TEXT("SectionLODBiasBuffer"), &SectionLODBiases);
			SectionLODBiasBuffer = RHICreateVertexBuffer(SectionLODBiases.GetResourceDataSize(), BUF_ShaderResource | BUF_Dynamic, CreateInfo);
			SectionLODBiasSRV = RHICreateShaderResourceView(SectionLODBiasBuffer, sizeof(float), PF_R32_FLOAT);
			bUpdateUB = true;
		}
		else
		{
			float* Data = (float*)RHILockBuffer(SectionLODBiasBuffer, 0, SectionLODBiases.GetResourceDataSize(), RLM_WriteOnly);
			FMemory::Memcpy(Data, SectionLODBiases.GetData(), SectionLODBiases.GetResourceDataSize());
			RHIUnlockBuffer(SectionLODBiasBuffer);
		}

		if (bUpdateUB)
		{
			FLandscapeSectionLODUniformParameters Parameters;
			Parameters.LandscapeIndex = LandscapeIndex;
			Parameters.Min = Min;
			Parameters.Size = Size;
			Parameters.SectionLODBias = SectionLODBiasSRV;

			RHIUpdateUniformBuffer(SectionLODUniformBuffer, &Parameters);
		}
	}
}


void FLandscapeRenderSystem::BeginFrame()
{
	// 5.0.2 hotfix : this function is not needed anymore but has not been removed for keeping binary compatibility
}

void FLandscapeRenderSystem::EndFrame()
{
	// 5.0.2 hotfix : this function is not needed anymore but has not been removed for keeping binary compatibility
}

void FLandscapeRenderSystem::WaitForTasksCompletion()
{
	// 5.0.2 hotfix : this function is not needed anymore but has not been removed for keeping binary compatibility
}


//
// FLandscapeSceneViewExtension
//
FLandscapeSceneViewExtension::FLandscapeSceneViewExtension(const FAutoRegister& AutoReg) : FSceneViewExtensionBase(AutoReg)
{
	FCoreDelegates::OnEndFrameRT.AddRaw(this, &FLandscapeSceneViewExtension::EndFrame_RenderThread);
}

FLandscapeSceneViewExtension::~FLandscapeSceneViewExtension()
{
	FCoreDelegates::OnEndFrameRT.RemoveAll(this);
}

void FLandscapeSceneViewExtension::BeginFrame_RenderThread()
{
	// 5.0.2 hotfix : this function is not needed anymore but has not been removed for keeping binary compatibility
}

void FLandscapeSceneViewExtension::PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	for (auto& Pair : LandscapeRenderSystems)
	{
		FLandscapeRenderSystem& RenderSystem = *Pair.Value;
		RenderSystem.BeginRender();
	}
}

void FLandscapeSceneViewExtension::PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView)
{
	FShaderResourceViewRHIRef LODDataSRV;
	FShaderResourceViewRHIRef IndirectionSRV;

	if (!LandscapeRenderSystems.IsEmpty())
	{
		TResourceArray<uint32> LandscapeIndirection;
		TResourceArray<float> LandscapeLODData;

		LandscapeIndirection.SetNum(FLandscapeRenderSystem::LandscapeIndexAllocator.Num());

		for (auto& Pair : LandscapeRenderSystems)
		{
			FLandscapeRenderSystem& RenderSystem = *Pair.Value;

			// Store index where the LOD data for this landscape starts
			uint32& IndirectionEntry = LandscapeIndirection[RenderSystem.LandscapeIndex];
			IndirectionEntry = LandscapeLODData.Num();

			// Compute sections lod values for this view & append to the global landscape LOD data
			const TResourceArray<float>& SectionsLODValues = RenderSystem.ComputeSectionsLODForView(InView);
			LandscapeLODData.Append(SectionsLODValues);
		}

		FRHIResourceCreateInfo CreateInfoLODBuffer(TEXT("LandscapeLODDataBuffer"), &LandscapeLODData);
		LandscapeLODDataBuffer = RHICreateVertexBuffer(LandscapeLODData.GetResourceDataSize(), BUF_ShaderResource | BUF_Volatile, CreateInfoLODBuffer);
		LODDataSRV = RHICreateShaderResourceView(LandscapeLODDataBuffer, sizeof(float), PF_R32_FLOAT);

		FRHIResourceCreateInfo CreateInfoIndirection(TEXT("LandscapeIndirectionBuffer"), &LandscapeIndirection);
		LandscapeIndirectionBuffer = RHICreateVertexBuffer(LandscapeIndirection.GetResourceDataSize(), BUF_ShaderResource | BUF_Volatile, CreateInfoIndirection);
		IndirectionSRV = RHICreateShaderResourceView(LandscapeIndirectionBuffer, sizeof(uint32), PF_R32_UINT);
	}
	else
	{
		LODDataSRV = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
		IndirectionSRV = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
	}

	InView.LandscapePerComponentDataBuffer = LODDataSRV;
	InView.LandscapeIndirectionBuffer = IndirectionSRV;
}

// TODO [jonathan.bard] Ideally this should be symmetrical with FLandscapeSceneViewExtension::PreRenderView_RenderThread and should be called in FLandscapeSceneViewExtension::PostRenderView_RenderThread
//  but PostRenderView_RenderThread is not currently called on all code paths (scene/reflection captures, etc.)
void FLandscapeSceneViewExtension::EndFrame_RenderThread()
{
	LandscapeLODDataBuffer = FBufferRHIRef();
	LandscapeIndirectionBuffer = FBufferRHIRef();
}

//
// FLandscapeVisibilityHelper
//
void FLandscapeVisibilityHelper::Init(UPrimitiveComponent* LandscapeComponent, FPrimitiveSceneProxy* ProxyIn)
{
	// Flag components to render only after level will be fully added to the world
	ULevel* ComponentLevel = LandscapeComponent->GetComponentLevel();
	bRequiresVisibleLevelToRender = (ComponentLevel && ComponentLevel->bRequireFullVisibilityToRender);
	bIsComponentLevelVisible = (!ComponentLevel || ComponentLevel->bIsVisible);
}

bool FLandscapeVisibilityHelper::OnAddedToWorld()
{
	if (bIsComponentLevelVisible)
	{
		return false;
	}

	bIsComponentLevelVisible = true;
	return true;
}

bool FLandscapeVisibilityHelper::OnRemoveFromWorld()
{
	if (!bIsComponentLevelVisible)
	{
		return false;
	}

	bIsComponentLevelVisible = false;
	return true;
}

FLandscapeComponentSceneProxy::FLandscapeComponentSceneProxy(ULandscapeComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent, NAME_LandscapeResourceNameForDebugging)
	, FLandscapeSectionInfo(InComponent->GetWorld(), InComponent->GetLandscapeProxy()->GetLandscapeGuid(), InComponent->GetSectionBase() / InComponent->ComponentSizeQuads)
	, MaxLOD(FMath::CeilLogTwo(InComponent->SubsectionSizeQuads + 1) - 1)
	, NumWeightmapLayerAllocations(InComponent->GetWeightmapLayerAllocations().Num())
	, StaticLightingLOD(InComponent->GetLandscapeProxy()->StaticLightingLOD)
	, WeightmapSubsectionOffset(InComponent->WeightmapSubsectionOffset)
	, FirstLOD(0)
	, LastLOD(MaxLOD)
	, ComponentMaxExtend(0.0f)
	, ComponentSquaredScreenSizeToUseSubSections(FMath::Square(InComponent->GetLandscapeProxy()->ComponentScreenSizeToUseSubSections))
	, NumSubsections(InComponent->NumSubsections)
	, SubsectionSizeQuads(InComponent->SubsectionSizeQuads)
	, SubsectionSizeVerts(InComponent->SubsectionSizeQuads + 1)
	, ComponentSizeQuads(InComponent->ComponentSizeQuads)
	, ComponentSizeVerts(InComponent->ComponentSizeQuads + 1)
	, SectionBase(InComponent->GetSectionBase())
	, LandscapeComponent(InComponent)
	, WeightmapScaleBias(InComponent->WeightmapScaleBias)
	, WeightmapTextures(InComponent->GetWeightmapTextures())
	, VisibilityWeightmapTexture(nullptr)
	, VisibilityWeightmapChannel(-1)
	, HeightmapTexture(InComponent->GetHeightmap())
	, NormalmapTexture(InComponent->GetHeightmap())
	, BaseColorForGITexture(InComponent->GIBakedBaseColorTexture)
	, HeightmapScaleBias(InComponent->HeightmapScaleBias)
	, XYOffsetmapTexture(InComponent->XYOffsetmapTexture)
	, BlendableLayerMask(InComponent->MobileBlendableLayerMask)
	, SharedBuffersKey(0)
	, SharedBuffers(nullptr)
	, VertexFactory(nullptr)
	, ComponentLightInfo(nullptr)
#if WITH_EDITORONLY_DATA
	, EditToolRenderData(InComponent->EditToolRenderData)
	, LODFalloff_DEPRECATED(InComponent->GetLandscapeProxy()->LODFalloff_DEPRECATED)
#endif
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	, CollisionMipLevel(InComponent->CollisionMipLevel)
	, SimpleCollisionMipLevel(InComponent->SimpleCollisionMipLevel)
	, CollisionResponse(InComponent->GetLandscapeProxy()->BodyInstance.GetResponseToChannels())
#endif
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	, LightMapResolution(InComponent->GetStaticLightMapResolution())
#endif
{
	// Landscape meshes do not deform internally (save by material effects such as WPO and PDO, which is allowed).
	// They do however have continuous LOD which is problematic, considered static as the LODs (are intended to) represent the same static surface.
	bHasDeformableMesh = false;

	VisibilityHelper.Init(InComponent, this);

	if (!VisibilityHelper.ShouldBeVisible())
	{
		SetForceHidden(true);
	}

	if (VisibilityHelper.RequiresVisibleLevelToRender())
	{
		bShouldNotifyOnWorldAddRemove = true;
	}

	EnableGPUSceneSupportFlags();

	const ERHIFeatureLevel::Type FeatureLevel = GetScene().GetFeatureLevel();
	if (FeatureLevel >= ERHIFeatureLevel::SM5)
	{
		if (InComponent->GetLandscapeProxy()->bUseDynamicMaterialInstance)
		{
			AvailableMaterials.Append(InComponent->MaterialInstancesDynamic);
		}
		else
		{
			AvailableMaterials.Append(InComponent->MaterialInstances);
		}
	}
	else
	{
		AvailableMaterials.Append(InComponent->MobileMaterialInterfaces);
	}

	LODIndexToMaterialIndex = InComponent->LODIndexToMaterialIndex;
	check(LODIndexToMaterialIndex.Num() == MaxLOD + 1);

	SetLevelColor(FLinearColor(1.f, 1.f, 1.f));

	if (FeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		HeightmapTexture = nullptr;
		HeightmapSubsectionOffsetU = 0;
		HeightmapSubsectionOffsetV = 0;
	}
	else
	{
		HeightmapSubsectionOffsetU = ((float)(InComponent->SubsectionSizeQuads + 1) / (float)FMath::Max<int32>(1, HeightmapTexture->GetSizeX()));
		HeightmapSubsectionOffsetV = ((float)(InComponent->SubsectionSizeQuads + 1) / (float)FMath::Max<int32>(1, HeightmapTexture->GetSizeY()));
	}

	float ScreenSizeRatioDivider = FMath::Max(InComponent->GetLandscapeProxy()->LOD0DistributionSetting * GLandscapeLOD0DistributionScale, 1.01f);
	// Cancel out so that landscape is not affected by r.StaticMeshLODDistanceScale
	float CurrentScreenSizeRatio = InComponent->GetLandscapeProxy()->LOD0ScreenSize / CVarStaticMeshLODDistanceScale.GetValueOnAnyThread();

	LODScreenRatioSquared.AddUninitialized(MaxLOD + 1);

	// LOD 0 handling
	LODScreenRatioSquared[0] = FMath::Square(CurrentScreenSizeRatio);
	LODSettings.LOD0ScreenSizeSquared = FMath::Square(CurrentScreenSizeRatio);
	CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
	LODSettings.LOD1ScreenSizeSquared = FMath::Square(CurrentScreenSizeRatio);
	ScreenSizeRatioDivider = FMath::Max(InComponent->GetLandscapeProxy()->LODDistributionSetting * GLandscapeLODDistributionScale, 1.01f);
	LODSettings.LODOnePlusDistributionScalarSquared = FMath::Square(ScreenSizeRatioDivider);

	// Other LODs
	for (int32 LODIndex = 1; LODIndex <= MaxLOD; ++LODIndex) // This should ALWAYS be calculated from the component size, not user MaxLOD override
	{
		LODScreenRatioSquared[LODIndex] = FMath::Square(CurrentScreenSizeRatio);
		CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
	}

	FirstLOD = 0;
	LastLOD = MaxLOD;	// we always need to go to MaxLOD regardless of LODBias as we could need the lowest LODs due to streaming.

	// Make sure out LastLOD is > of MinStreamedLOD otherwise we would not be using the right LOD->MIP, the only drawback is a possible minor memory usage for overallocating static mesh element batch
	const int32 MinStreamedLOD = HeightmapTexture ? FMath::Min<int32>(HeightmapTexture->GetNumMips() - HeightmapTexture->GetNumResidentMips(), FMath::CeilLogTwo(SubsectionSizeVerts) - 1) : 0;
	LastLOD = FMath::Max(MinStreamedLOD, LastLOD);

	// Clamp to MaxLODLevel
	const int32 MaxLODLevel = InComponent->GetLandscapeProxy()->MaxLODLevel;
	if (MaxLODLevel >= 0)
	{
		MaxLOD = FMath::Min<int8>(MaxLODLevel, MaxLOD);
		LastLOD = FMath::Min<int32>(MaxLODLevel, LastLOD);
	}

	// Clamp ForcedLOD to the valid range and then apply
	int8 ForcedLOD = InComponent->ForcedLOD;
	ForcedLOD = ForcedLOD >= 0 ? FMath::Clamp<int32>(ForcedLOD, FirstLOD, LastLOD) : ForcedLOD;
	FirstLOD = ForcedLOD >= 0 ? ForcedLOD : FirstLOD;
	LastLOD = ForcedLOD >= 0 ? ForcedLOD : LastLOD;

	LODSettings.LastLODIndex = LastLOD;
	LODSettings.LastLODScreenSizeSquared = LODScreenRatioSquared[LastLOD];
	LODSettings.ForcedLOD = ForcedLOD;

	LastVirtualTextureLOD = MaxLOD;
	FirstVirtualTextureLOD = FMath::Max(MaxLOD - InComponent->GetLandscapeProxy()->VirtualTextureNumLods, 0);
	VirtualTextureLodBias = InComponent->GetLandscapeProxy()->VirtualTextureLodBias;

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	LODSettings.DrawCollisionPawnLOD = CollisionResponse.GetResponse(ECC_Pawn) == ECR_Ignore ? -1 : SimpleCollisionMipLevel;
	LODSettings.DrawCollisionVisibilityLOD = CollisionResponse.GetResponse(ECC_Visibility) == ECR_Ignore ? -1 : CollisionMipLevel;
#else
	LODSettings.DrawCollisionPawnLOD = LODSettings.DrawCollisionVisibilityLOD = -1;
#endif

	ComponentMaxExtend = SubsectionSizeQuads * FMath::Max(InComponent->GetComponentTransform().GetScale3D().X, InComponent->GetComponentTransform().GetScale3D().Y);

	if (NumSubsections > 1)
	{
		FRotator ComponentRotator = LandscapeComponent->GetComponentRotation();
		float SubSectionMaxExtend = ComponentMaxExtend / 2.0f;
		FVector ComponentTopLeftCorner = LandscapeComponent->Bounds.Origin - ComponentRotator.RotateVector(FVector(SubSectionMaxExtend, SubSectionMaxExtend, 0.0f));

		SubSectionScreenSizeTestingPosition.AddUninitialized(MAX_SUBSECTION_COUNT);

		for (int32 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int32 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				int32 SubSectionIndex = SubX + SubY * NumSubsections;
				SubSectionScreenSizeTestingPosition[SubSectionIndex] = ComponentTopLeftCorner + ComponentRotator.RotateVector(FVector(ComponentMaxExtend * SubX, ComponentMaxExtend * SubY, 0.0f));
			}
		}
	}

	if (InComponent->StaticLightingResolution > 0.f)
	{
		StaticLightingResolution = InComponent->StaticLightingResolution;
	}
	else
	{
		StaticLightingResolution = InComponent->GetLandscapeProxy()->StaticLightingResolution;
	}

	ComponentLightInfo = MakeUnique<FLandscapeLCI>(InComponent);
	check(ComponentLightInfo);

	const bool bHasStaticLighting = ComponentLightInfo->GetLightMap() || ComponentLightInfo->GetShadowMap();

	// Check material usage
	if (ensure(AvailableMaterials.Num() > 0))
	{
		for (UMaterialInterface*& MaterialInterface : AvailableMaterials)
		{
			if (MaterialInterface == nullptr ||
				(bHasStaticLighting && !MaterialInterface->CheckMaterialUsage_Concurrent(MATUSAGE_StaticLighting)))
			{
				MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
			}
		}
	}
	else
	{
		AvailableMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
	}

	MaterialRelevances.Reserve(AvailableMaterials.Num());

	for (UMaterialInterface*& MaterialInterface : AvailableMaterials)
	{
		const UMaterial* LandscapeMaterial = MaterialInterface != nullptr ? MaterialInterface->GetMaterial_Concurrent() : nullptr;

		if (LandscapeMaterial != nullptr)
		{
			UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(MaterialInterface);

			// In some case it's possible that the Material Instance we have and the Material are not related, for example, in case where content was force deleted, we can have a MIC with no parent, so GetMaterial will fallback to the default material.
			// and since the MIC is not really valid, dont generate the relevance.
			if (MaterialInstance == nullptr || MaterialInstance->IsChildOf(LandscapeMaterial))
			{
				MaterialRelevances.Add(MaterialInterface->GetRelevance_Concurrent(FeatureLevel));
			}
		}
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST) || (UE_BUILD_SHIPPING && WITH_EDITOR)
	if (GIsEditor)
	{
		ALandscapeProxy* Proxy = InComponent->GetLandscapeProxy();
		// Try to find a color for level coloration.
		if (Proxy)
		{
			ULevel* Level = Proxy->GetLevel();
			ULevelStreaming* LevelStreaming = FLevelUtils::FindStreamingLevel(Level);
			if (LevelStreaming)
			{
				SetLevelColor(LevelStreaming->LevelColor);
			}
		}
	}
#endif

	const int8 SubsectionSizeLog2 = FMath::CeilLogTwo(InComponent->SubsectionSizeQuads + 1);
	SharedBuffersKey = (SubsectionSizeLog2 & 0xf) | ((NumSubsections & 0xf) << 4) |
		(FeatureLevel <= ERHIFeatureLevel::ES3_1 ? 0 : 1 << 30) | (XYOffsetmapTexture == nullptr ? 0 : 1 << 31);

	bSupportsHeightfieldRepresentation = FeatureLevel > ERHIFeatureLevel::ES3_1;
	bSupportsMeshCardRepresentation = true;

#if WITH_EDITOR
	const TArray<FWeightmapLayerAllocationInfo>& ComponentWeightmapLayerAllocations = InComponent->GetWeightmapLayerAllocations();
	for (const FWeightmapLayerAllocationInfo& Allocation : ComponentWeightmapLayerAllocations)
	{
		if (Allocation.LayerInfo != nullptr)
		{
			LayerColors.Add(Allocation.LayerInfo->LayerUsageDebugColor);
		}
	}

	for (int32 Idx = 0; Idx < InComponent->WeightmapLayerAllocations.Num(); Idx++)
	{
		const FWeightmapLayerAllocationInfo& Allocation = InComponent->WeightmapLayerAllocations[Idx];
		if (Allocation.LayerInfo == ALandscapeProxy::VisibilityLayer && Allocation.IsAllocated())
		{
			VisibilityWeightmapTexture = WeightmapTextures[Allocation.WeightmapTextureIndex];
			VisibilityWeightmapChannel = Allocation.WeightmapTextureChannel;
			break;
		}
	}
#endif

	bSupportsInstanceDataBuffer = true;
	UpdateDefaultInstanceSceneData();
}

void FLandscapeComponentSceneProxy::CreateRenderThreadResources()
{
	LLM_SCOPE(ELLMTag::Landscape);

	check(HeightmapTexture != nullptr);

	FLandscapeRenderSystem::CreateResources(this);

	if (VisibilityHelper.ShouldBeVisible())
	{
		RegisterSection();
	}

	auto FeatureLevel = GetScene().GetFeatureLevel();

	SharedBuffers = FLandscapeComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey);
	if (SharedBuffers == nullptr)
	{
		SharedBuffers = new FLandscapeSharedBuffers(
			SharedBuffersKey, SubsectionSizeQuads, NumSubsections,
			FeatureLevel);

		FLandscapeComponentSceneProxy::SharedBuffersMap.Add(SharedBuffersKey, SharedBuffers);

		if (!XYOffsetmapTexture)
		{
			FLandscapeVertexFactory* LandscapeVertexFactory = new FLandscapeVertexFactory(FeatureLevel);
			LandscapeVertexFactory->Data.PositionComponent = FVertexStreamComponent(SharedBuffers->VertexBuffer, 0, sizeof(FLandscapeVertex), VET_Float4);
			LandscapeVertexFactory->InitResource();
			SharedBuffers->VertexFactory = LandscapeVertexFactory;
		}
		else
		{
			FLandscapeXYOffsetVertexFactory* LandscapeXYOffsetVertexFactory = new FLandscapeXYOffsetVertexFactory(FeatureLevel);
			LandscapeXYOffsetVertexFactory->Data.PositionComponent = FVertexStreamComponent(SharedBuffers->VertexBuffer, 0, sizeof(FLandscapeVertex), VET_Float4);
			LandscapeXYOffsetVertexFactory->InitResource();
			SharedBuffers->VertexFactory = LandscapeXYOffsetVertexFactory;
		}

		// we need the fixed grid vertex factory for both virtual texturing and grass : 
		bool bNeedsFixedGridVertexFactory = UseVirtualTexturing(FeatureLevel);

#if WITH_EDITOR
		bNeedsFixedGridVertexFactory |= (SharedBuffers->GrassIndexBuffer != nullptr);
#endif // WITH_EDITOR

		if (bNeedsFixedGridVertexFactory)
		{
			//todo[vt]: We will need a version of this to support XYOffsetmapTexture
			FLandscapeFixedGridVertexFactory* LandscapeVertexFactory = new FLandscapeFixedGridVertexFactory(FeatureLevel);
			LandscapeVertexFactory->Data.PositionComponent = FVertexStreamComponent(SharedBuffers->VertexBuffer, 0, sizeof(FLandscapeVertex), VET_Float4);
			LandscapeVertexFactory->InitResource();
			SharedBuffers->FixedGridVertexFactory = LandscapeVertexFactory;
		}
	}

	SharedBuffers->AddRef();

	// Assign vertex factory
	VertexFactory = SharedBuffers->VertexFactory;
	FixedGridVertexFactory = SharedBuffers->FixedGridVertexFactory;

	// Assign LandscapeUniformShaderParameters
	LandscapeUniformShaderParameters.InitResource();

	// Create per Lod uniform buffers
	const int32 NumMips = FMath::CeilLogTwo(SubsectionSizeVerts);
	// create as many as there are potential mips (even if MaxLOD can be inferior than that), because the grass could need that much :
	LandscapeFixedGridUniformShaderParameters.AddDefaulted(NumMips);
	for (int32 LodIndex = 0; LodIndex < NumMips; ++LodIndex)
	{
		LandscapeFixedGridUniformShaderParameters[LodIndex].InitResource();
		FLandscapeFixedGridUniformShaderParameters Parameters;
		Parameters.LodValues = FVector4f(
			LodIndex,
			0.f,
			(float)((SubsectionSizeVerts >> LodIndex) - 1),
			1.f / (float)((SubsectionSizeVerts >> LodIndex) - 1));
		LandscapeFixedGridUniformShaderParameters[LodIndex].SetContents(Parameters);
	}

#if WITH_EDITOR
	// Create MeshBatch for grass rendering
	if (SharedBuffers->GrassIndexBuffer)
	{
		check(FixedGridVertexFactory != nullptr);

		GrassMeshBatch.Elements.Empty(NumMips);
		GrassMeshBatch.Elements.AddDefaulted(NumMips);
		GrassBatchParams.Empty(NumMips);
		GrassBatchParams.AddDefaulted(NumMips);

		// Grass is being generated using LOD0 material only
		// It uses the fixed grid vertex factory so it doesn't support XY offsets
		FMaterialRenderProxy* RenderProxy = AvailableMaterials[LODIndexToMaterialIndex[0]]->GetRenderProxy();
		GrassMeshBatch.VertexFactory = FixedGridVertexFactory;
		GrassMeshBatch.MaterialRenderProxy = RenderProxy;
		GrassMeshBatch.LCI = nullptr;
		GrassMeshBatch.ReverseCulling = false;
		GrassMeshBatch.CastShadow = false;
		GrassMeshBatch.Type = PT_PointList;
		GrassMeshBatch.DepthPriorityGroup = SDPG_World;

		// Combined grass rendering batch element
		FMeshBatchElement* GrassBatchElement = &GrassMeshBatch.Elements[0];
		FLandscapeBatchElementParams* BatchElementParams = &GrassBatchParams[0];
		BatchElementParams->LandscapeUniformShaderParametersResource = &LandscapeUniformShaderParameters;
		BatchElementParams->FixedGridUniformShaderParameters = &LandscapeFixedGridUniformShaderParameters;
		BatchElementParams->LandscapeSectionLODUniformParameters = nullptr; // Not needed for grass rendering
		BatchElementParams->SceneProxy = this;
		BatchElementParams->CurrentLOD = 0;
		GrassBatchElement->UserData = BatchElementParams;
		GrassBatchElement->PrimitiveUniformBuffer = GetUniformBuffer();
		GrassBatchElement->IndexBuffer = SharedBuffers->GrassIndexBuffer;
		GrassBatchElement->NumPrimitives = FMath::Square(NumSubsections) * FMath::Square(SubsectionSizeVerts);
		GrassBatchElement->FirstIndex = 0;
		GrassBatchElement->MinVertexIndex = 0;
		GrassBatchElement->MaxVertexIndex = SharedBuffers->NumVertices - 1;

		// Grass system is also used to bake out heights which are source for collision data when bBakeMaterialPositionOffsetIntoCollision is enabled
		for (int32 Mip = 1; Mip < NumMips; ++Mip)
		{
			const int32 MipSubsectionSizeVerts = SubsectionSizeVerts >> Mip;

			FMeshBatchElement* CollisionBatchElement = &GrassMeshBatch.Elements[Mip];
			*CollisionBatchElement = *GrassBatchElement;
			FLandscapeBatchElementParams* CollisionBatchElementParams = &GrassBatchParams[Mip];
			*CollisionBatchElementParams = *BatchElementParams;
			CollisionBatchElementParams->CurrentLOD = Mip;
			CollisionBatchElement->UserData = CollisionBatchElementParams;
			CollisionBatchElement->NumPrimitives = FMath::Square(NumSubsections) * FMath::Square(MipSubsectionSizeVerts);
			CollisionBatchElement->FirstIndex = SharedBuffers->GrassIndexMipOffsets[Mip];
		}
	}
#endif

#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				const int8 SubSectionIdx = SubX + SubY * NumSubsections;

				FRayTracingGeometryInitializer Initializer;
				static const FName DebugName("FLandscapeComponentSceneProxy");
				static int32 DebugNumber = 0;
				Initializer.DebugName = FName(DebugName, DebugNumber++);
				Initializer.IndexBuffer = nullptr;
				Initializer.GeometryType = RTGT_Triangles;
				Initializer.bFastBuild = true;
				Initializer.bAllowUpdate = true;
				FRayTracingGeometrySegment Segment;
				Segment.VertexBuffer = nullptr;
				Segment.VertexBufferStride = sizeof(FVector3f);
				Segment.VertexBufferElementType = VET_Float3;
				Segment.MaxVertices = FMath::Square(SubsectionSizeVerts);
				Initializer.Segments.Add(Segment);
				SectionRayTracingStates[SubSectionIdx].Geometry.SetInitializer(Initializer);
				SectionRayTracingStates[SubSectionIdx].Geometry.InitResource();

				FLandscapeVertexFactoryMVFParameters UniformBufferParams;
				UniformBufferParams.SubXY = FIntPoint(SubX, SubY);
				SectionRayTracingStates[SubSectionIdx].UniformBuffer = FLandscapeVertexFactoryMVFUniformBufferRef::CreateUniformBufferImmediate(UniformBufferParams, UniformBuffer_MultiFrame);
			}
		}
	}
#endif
}

void FLandscapeComponentSceneProxy::DestroyRenderThreadResources()
{
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
	FLandscapeRenderSystem::UnregisterSection(this);
	FLandscapeRenderSystem::DestroyResources(this);
}

bool FLandscapeComponentSceneProxy::OnLevelAddedToWorld_RenderThread()
{
	if (VisibilityHelper.OnAddedToWorld())
	{
		SetForceHidden(false);
		FLandscapeRenderSystem::RegisterSection(this);
		return true;
	}
	return false;
}

void FLandscapeComponentSceneProxy::OnLevelRemovedFromWorld_RenderThread()
{
	if (VisibilityHelper.OnRemoveFromWorld())
	{
		SetForceHidden(true);
		FLandscapeRenderSystem::UnregisterSection(this);
	}
}

FLandscapeComponentSceneProxy::~FLandscapeComponentSceneProxy()
{
	// Free the subsection uniform buffer
	LandscapeUniformShaderParameters.ReleaseResource();

	// Free the lod uniform buffers
	for (int32 i = 0; i < LandscapeFixedGridUniformShaderParameters.Num(); ++i)
	{
		LandscapeFixedGridUniformShaderParameters[i].ReleaseResource();
	}

	if (SharedBuffers)
	{
		check(SharedBuffers == FLandscapeComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey));
		if (SharedBuffers->Release() == 0)
		{
			FLandscapeComponentSceneProxy::SharedBuffersMap.Remove(SharedBuffersKey);
		}
		SharedBuffers = nullptr;
	}

#if RHI_RAYTRACING
	for (int32 SubY = 0; SubY < NumSubsections; SubY++)
	{
		for (int32 SubX = 0; SubX < NumSubsections; SubX++)
		{
			const int8 SubSectionIdx = SubX + SubY * NumSubsections;
			SectionRayTracingStates[SubSectionIdx].Geometry.ReleaseResource();
			SectionRayTracingStates[SubSectionIdx].RayTracingDynamicVertexBuffer.Release();
		}
	}
#endif
}

bool FLandscapeComponentSceneProxy::CanBeOccluded() const
{
	if (IsVirtualTextureOnly())
	{
		return false;
	}

	for (const FMaterialRelevance& Relevance : MaterialRelevances)
	{
		if (!Relevance.bDisableDepthTest)
		{
			return true;
		}
	}

	return false;
}

FPrimitiveViewRelevance FLandscapeComponentSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	const bool bCollisionView = (View->Family->EngineShowFlags.CollisionVisibility || View->Family->EngineShowFlags.CollisionPawn);
	Result.bDrawRelevance = (IsShown(View) || bCollisionView) && View->Family->EngineShowFlags.Landscape;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;

	auto FeatureLevel = View->GetFeatureLevel();

#if WITH_EDITOR
	if (!GLandscapeEditModeActive)
	{
		// No tools to render, just use the cached material relevance.
#endif
		for (const FMaterialRelevance& MaterialRelevance : MaterialRelevances)
		{
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
		}

#if WITH_EDITOR
	}
	else
	{
		for (const FMaterialRelevance& MaterialRelevance : MaterialRelevances)
		{
			// Also add the tool material(s)'s relevance to the MaterialRelevance
			FMaterialRelevance ToolRelevance = MaterialRelevance;

			// Tool brushes and Gizmo
			if (EditToolRenderData.ToolMaterial)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= EditToolRenderData.ToolMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			if (EditToolRenderData.GizmoMaterial)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= EditToolRenderData.GizmoMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			// Region selection
			if (EditToolRenderData.SelectedType)
			{
				if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::SelectRegion) && (EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_REGION)
					&& !(GLandscapeEditRenderMode & ELandscapeEditRenderMode::Mask) && GSelectionRegionMaterial)
				{
					Result.bDynamicRelevance = true;
					ToolRelevance |= GSelectionRegionMaterial->GetRelevance_Concurrent(FeatureLevel);
				}
				if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::SelectComponent) && (EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_COMPONENT) && GSelectionColorMaterial)
				{
					Result.bDynamicRelevance = true;
					ToolRelevance |= GSelectionColorMaterial->GetRelevance_Concurrent(FeatureLevel);
				}
			}

			// Mask
			if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::Mask) && GMaskRegionMaterial != nullptr &&
				(((EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_REGION)) || (!(GLandscapeEditRenderMode & ELandscapeEditRenderMode::InvertedMask))))
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= GMaskRegionMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			if (GLandscapeViewMode == ELandscapeViewMode::LayerContribution)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= GColorMaskRegionMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			if (CVarLandscapeShowDirty.GetValueOnRenderThread() && GLandscapeDirtyMaterial)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= GLandscapeDirtyMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			ToolRelevance.SetPrimitiveViewRelevance(Result);
		}
	}

	// Various visualizations need to render using dynamic relevance
	if ((View->Family->EngineShowFlags.Bounds && IsSelected()) ||
		GLandscapeDebugOptions.bShowPatches)
	{
		Result.bDynamicRelevance = true;
	}
#endif

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const bool bInCollisionView = View->Family->EngineShowFlags.CollisionVisibility || View->Family->EngineShowFlags.CollisionPawn;
#endif

	// Use the dynamic path for rendering landscape components pass only for Rich Views or if the static path is disabled for debug.
	if (IsRichView(*View->Family) ||
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		bInCollisionView ||
#endif
		GLandscapeDebugOptions.bDisableStatic ||
		View->Family->EngineShowFlags.Wireframe ||
#if WITH_EDITOR
		(IsSelected() && !GLandscapeEditModeActive) ||
		(GLandscapeViewMode != ELandscapeViewMode::Normal) ||
		(CVarLandscapeShowDirty.GetValueOnAnyThread() && GLandscapeDirtyMaterial) ||
		(GetViewLodOverride(*View) >= 0)
#else
		IsSelected()
#endif
		)
	{
		Result.bDynamicRelevance = true;
	}
	else
	{
		Result.bStaticRelevance = true;
	}

	Result.bShadowRelevance = (GAllowLandscapeShadows > 0) && IsShadowCast(View) && View->Family->EngineShowFlags.Landscape;
	return Result;
}

/**
*	Determines the relevance of this primitive's elements to the given light.
*	@param	LightSceneProxy			The light to determine relevance for
*	@param	bDynamic (output)		The light is dynamic for this primitive
*	@param	bRelevant (output)		The light is relevant for this primitive
*	@param	bLightMapped (output)	The light is light mapped for this primitive
*/
void FLandscapeComponentSceneProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	// Attach the light to the primitive's static meshes.
	bDynamic = true;
	bRelevant = false;
	bLightMapped = true;
	bShadowMapped = true;

	if (ComponentLightInfo)
	{
		ELightInteractionType InteractionType = ComponentLightInfo->GetInteraction(LightSceneProxy).GetType();

		if (InteractionType != LIT_CachedIrrelevant)
		{
			bRelevant = true;
		}

		if (InteractionType != LIT_CachedLightMap && InteractionType != LIT_CachedIrrelevant)
		{
			bLightMapped = false;
		}

		if (InteractionType != LIT_Dynamic)
		{
			bDynamic = false;
		}

		if (InteractionType != LIT_CachedSignedDistanceFieldShadowMap2D)
		{
			bShadowMapped = false;
		}
	}
	else
	{
		bRelevant = true;
		bLightMapped = false;
	}
}

SIZE_T FLandscapeComponentSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

FLightInteraction FLandscapeComponentSceneProxy::FLandscapeLCI::GetInteraction(const class FLightSceneProxy* LightSceneProxy) const
{
	// ask base class
	ELightInteractionType LightInteraction = GetStaticInteraction(LightSceneProxy, IrrelevantLights);

	if (LightInteraction != LIT_MAX)
	{
		return FLightInteraction(LightInteraction);
	}

	// Use dynamic lighting if the light doesn't have static lighting.
	return FLightInteraction::Dynamic();
}

#if WITH_EDITOR
namespace DebugColorMask
{
	const FLinearColor Masks[5] =
	{
		FLinearColor(1.f, 0.f, 0.f, 0.f),
		FLinearColor(0.f, 1.f, 0.f, 0.f),
		FLinearColor(0.f, 0.f, 1.f, 0.f),
		FLinearColor(0.f, 0.f, 0.f, 1.f),
		FLinearColor(0.f, 0.f, 0.f, 0.f)
	};
};
#endif

void FLandscapeComponentSceneProxy::OnTransformChanged()
{
	// Set Lightmap ScaleBias
	int32 PatchExpandCountX = 0;
	int32 PatchExpandCountY = 0;
	int32 DesiredSize = 1; // output by GetTerrainExpandPatchCount but not used below
	const float LightMapRatio = ::GetTerrainExpandPatchCount(StaticLightingResolution, PatchExpandCountX, PatchExpandCountY, ComponentSizeQuads, (NumSubsections * (SubsectionSizeQuads + 1)), DesiredSize, StaticLightingLOD);
	const float LightmapLODScaleX = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountX);
	const float LightmapLODScaleY = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountY);
	const float LightmapBiasX = PatchExpandCountX * LightmapLODScaleX;
	const float LightmapBiasY = PatchExpandCountY * LightmapLODScaleY;
	const float LightmapScaleX = LightmapLODScaleX * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapScaleY = LightmapLODScaleY * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapExtendFactorX = (float)SubsectionSizeQuads * LightmapScaleX;
	const float LightmapExtendFactorY = (float)SubsectionSizeQuads * LightmapScaleY;

	// cache component's WorldToLocal
	FMatrix LtoW = GetLocalToWorld();
	WorldToLocal = LtoW.Inverse();

	// cache component's LocalToWorldNoScaling
	LocalToWorldNoScaling = LtoW;
	LocalToWorldNoScaling.RemoveScaling();

	// Set FLandscapeUniformVSParameters for this subsection
	FLandscapeUniformShaderParameters LandscapeParams;
	LandscapeParams.ComponentBaseX = ComponentBase.X;
	LandscapeParams.ComponentBaseY = ComponentBase.Y;
	LandscapeParams.SubsectionSizeVerts = SubsectionSizeVerts;
	LandscapeParams.NumSubsections = NumSubsections;
	LandscapeParams.LastLOD = LastLOD;
	LandscapeParams.HeightmapUVScaleBias = HeightmapScaleBias;
	LandscapeParams.WeightmapUVScaleBias = WeightmapScaleBias;
	LandscapeParams.LocalToWorldNoScaling = FMatrix44f(LocalToWorldNoScaling);			// LWC_TODO: Precision loss

	LandscapeParams.LandscapeLightmapScaleBias = FVector4f(
		LightmapScaleX,
		LightmapScaleY,
		LightmapBiasY,
		LightmapBiasX);
	LandscapeParams.SubsectionSizeVertsLayerUVPan = FVector4f(
		SubsectionSizeVerts,
		1.f / (float)SubsectionSizeQuads,
		SectionBase.X,
		SectionBase.Y
	);
	LandscapeParams.SubsectionOffsetParams = FVector4f(
		HeightmapSubsectionOffsetU,
		HeightmapSubsectionOffsetV,
		WeightmapSubsectionOffset,
		SubsectionSizeQuads
	);
	LandscapeParams.LightmapSubsectionOffsetParams = FVector4f(
		LightmapExtendFactorX,
		LightmapExtendFactorY,
		0,
		0
	);
	LandscapeParams.BlendableLayerMask = FVector4f(
		BlendableLayerMask & (1 << 0) ? 1 : 0,
		BlendableLayerMask & (1 << 1) ? 1 : 0,
		BlendableLayerMask & (1 << 2) ? 1 : 0,
		0
	);

	if (HeightmapTexture)
	{
		LandscapeParams.HeightmapTexture = HeightmapTexture->TextureReference.TextureReferenceRHI;
		LandscapeParams.HeightmapTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();
	}
	else
	{
		LandscapeParams.HeightmapTexture = GBlackTexture->TextureRHI;
		LandscapeParams.HeightmapTextureSampler = GBlackTexture->SamplerStateRHI;
	}

	if (XYOffsetmapTexture)
	{
		LandscapeParams.XYOffsetmapTexture = XYOffsetmapTexture->TextureReference.TextureReferenceRHI;
		LandscapeParams.XYOffsetmapTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();
	}
	else
	{
		LandscapeParams.XYOffsetmapTexture = GBlackTexture->TextureRHI;
		LandscapeParams.XYOffsetmapTextureSampler = GBlackTexture->SamplerStateRHI;
	}

	if (NormalmapTexture)
	{
		LandscapeParams.NormalmapTexture = NormalmapTexture->TextureReference.TextureReferenceRHI;
		LandscapeParams.NormalmapTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	}
	else
	{
		LandscapeParams.NormalmapTexture = GBlackTexture->TextureRHI;
		LandscapeParams.NormalmapTextureSampler = GBlackTexture->SamplerStateRHI;
	}

	LandscapeUniformShaderParameters.SetContents(LandscapeParams);

	// Recache mesh draw commands for changed uniform buffers
	GetScene().UpdateCachedRenderStates(this);

	UpdateDefaultInstanceSceneData();
}

/** Creates a mesh batch for virtual texture rendering. Will render a simple fixed grid with combined subsections. */
bool FLandscapeComponentSceneProxy::GetMeshElementForVirtualTexture(int32 InLodIndex, ERuntimeVirtualTextureMaterialType MaterialType, UMaterialInterface* InMaterialInterface, FMeshBatch& OutMeshBatch, TArray<FLandscapeBatchElementParams>& OutStaticBatchParamArray) const
{
	if (InMaterialInterface == nullptr)
	{
		return false;
	}

	OutMeshBatch.VertexFactory = FixedGridVertexFactory;
	OutMeshBatch.MaterialRenderProxy = InMaterialInterface->GetRenderProxy();
	OutMeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
	OutMeshBatch.CastShadow = false;
	OutMeshBatch.bUseForDepthPass = false;
	OutMeshBatch.bUseAsOccluder = false;
	OutMeshBatch.bUseForMaterial = false;
	OutMeshBatch.Type = PT_TriangleList;
	OutMeshBatch.DepthPriorityGroup = SDPG_World;
	OutMeshBatch.LODIndex = InLodIndex;
	OutMeshBatch.bDitheredLODTransition = false;
	OutMeshBatch.bRenderToVirtualTexture = true;
	OutMeshBatch.RuntimeVirtualTextureMaterialType = (uint32)MaterialType;

	OutMeshBatch.Elements.Empty(1);

	const FLandscapeRenderSystem& RenderSystem = *LandscapeRenderSystems.FindChecked(LandscapeKey);

	FLandscapeBatchElementParams* BatchElementParams = new(OutStaticBatchParamArray) FLandscapeBatchElementParams;
	BatchElementParams->SceneProxy = this;
	BatchElementParams->LandscapeUniformShaderParametersResource = &LandscapeUniformShaderParameters;
	BatchElementParams->FixedGridUniformShaderParameters = &LandscapeFixedGridUniformShaderParameters;
	BatchElementParams->LandscapeSectionLODUniformParameters = RenderSystem.SectionLODUniformBuffer;
	BatchElementParams->CurrentLOD = InLodIndex;

	int32 LodSubsectionSizeVerts = SubsectionSizeVerts >> InLodIndex;

	FMeshBatchElement BatchElement;
	BatchElement.UserData = BatchElementParams;
	BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
	BatchElement.IndexBuffer = SharedBuffers->IndexBuffers[InLodIndex];
	BatchElement.NumPrimitives = FMath::Square((LodSubsectionSizeVerts - 1)) * FMath::Square(NumSubsections) * 2;
	BatchElement.FirstIndex = 0;
	BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[InLodIndex].MinIndexFull;
	BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[InLodIndex].MaxIndexFull;

	OutMeshBatch.Elements.Add(BatchElement);

	return true;
}

void FLandscapeComponentSceneProxy::ApplyWorldOffset(FVector InOffset)
{
	FPrimitiveSceneProxy::ApplyWorldOffset(InOffset);

	if (NumSubsections > 1)
	{
		for (int32 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int32 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				int32 SubSectionIndex = SubX + SubY * NumSubsections;
				SubSectionScreenSizeTestingPosition[SubSectionIndex] += InOffset;
			}
		}
	}
}

template<class ArrayType>
bool FLandscapeComponentSceneProxy::GetStaticMeshElement(int32 LODIndex, bool bForToolMesh, FMeshBatch& MeshBatch, ArrayType& OutStaticBatchParamArray) const
{
	UMaterialInterface* MaterialInterface = nullptr;

	{
		int32 MaterialIndex = LODIndexToMaterialIndex[LODIndex];

		// Defaults to the material interface w/ potential tessellation
		MaterialInterface = AvailableMaterials[MaterialIndex];

		if (!MaterialInterface)
		{
			return false;
		}
	}

	{
		MeshBatch.VertexFactory = VertexFactory;
		MeshBatch.MaterialRenderProxy = MaterialInterface->GetRenderProxy();

		MeshBatch.LCI = ComponentLightInfo.Get();
		MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
		MeshBatch.CastShadow = bForToolMesh ? false : true;
		MeshBatch.bUseForDepthPass = true;
		MeshBatch.bUseAsOccluder = ShouldUseAsOccluder() && GetScene().GetShadingPath() == EShadingPath::Deferred && !IsMovable();
		MeshBatch.bUseForMaterial = true;
		MeshBatch.Type = PT_TriangleList;
		MeshBatch.DepthPriorityGroup = SDPG_World;
		MeshBatch.LODIndex = LODIndex;
		MeshBatch.bDitheredLODTransition = false;

		const FLandscapeRenderSystem& RenderSystem = *LandscapeRenderSystems.FindChecked(LandscapeKey);

		FLandscapeBatchElementParams* BatchElementParams = new(OutStaticBatchParamArray) FLandscapeBatchElementParams;
		BatchElementParams->LandscapeUniformShaderParametersResource = &LandscapeUniformShaderParameters;
		BatchElementParams->FixedGridUniformShaderParameters = &LandscapeFixedGridUniformShaderParameters;
		BatchElementParams->LandscapeSectionLODUniformParameters = RenderSystem.SectionLODUniformBuffer;
		BatchElementParams->SceneProxy = this;
		BatchElementParams->CurrentLOD = LODIndex;

		// Combined batch element
		FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
		BatchElement.UserData = BatchElementParams;
		BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
		BatchElement.IndexBuffer = SharedBuffers->IndexBuffers[LODIndex];
		BatchElement.NumPrimitives = FMath::Square((SubsectionSizeVerts >> LODIndex) - 1) * FMath::Square(NumSubsections) * 2;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[LODIndex].MinIndexFull;
		BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[LODIndex].MaxIndexFull;

		// The default is overridden here only by mobile landscape to punch holes in the geometry
		ApplyMeshElementModifier(BatchElement, LODIndex);
	}

	return true;
}

void FLandscapeComponentSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	if (AvailableMaterials.Num() == 0)
	{
		return;
	}

	int32 TotalBatchCount = 1 + LastLOD - FirstLOD;
	TotalBatchCount += (1 + LastVirtualTextureLOD - FirstVirtualTextureLOD) * RuntimeVirtualTextureMaterialTypes.Num();

	StaticBatchParamArray.Empty(TotalBatchCount);
	PDI->ReserveMemoryForMeshes(TotalBatchCount);

	// Add fixed grid mesh batches for runtime virtual texture usage
	for (ERuntimeVirtualTextureMaterialType MaterialType : RuntimeVirtualTextureMaterialTypes)
	{
		const int32 MaterialIndex = LODIndexToMaterialIndex[FirstLOD];

		for (int32 LODIndex = FirstVirtualTextureLOD; LODIndex <= LastVirtualTextureLOD; ++LODIndex)
		{
			FMeshBatch RuntimeVirtualTextureMeshBatch;
			if (GetMeshElementForVirtualTexture(LODIndex, MaterialType, AvailableMaterials[MaterialIndex], RuntimeVirtualTextureMeshBatch, StaticBatchParamArray))
			{
				PDI->DrawMesh(RuntimeVirtualTextureMeshBatch, FLT_MAX);
			}
		}
	}

	for (int32 LODIndex = FirstLOD; LODIndex <= LastLOD; LODIndex++)
	{
		FMeshBatch MeshBatch;

		if (GetStaticMeshElement(LODIndex, false, MeshBatch, StaticBatchParamArray))
		{
			PDI->DrawMesh(MeshBatch, LODIndex == FirstLOD ? FLT_MAX : (FMath::Sqrt(LODScreenRatioSquared[LODIndex]) * 2.0f));
		}
	}

	check(StaticBatchParamArray.Num() <= TotalBatchCount);
}

int8 FLandscapeComponentSceneProxy::GetLODFromScreenSize(float InScreenSizeSquared, float InViewLODScale) const
{
	float FractionalLOD;

	return FLandscapeRenderSystem::GetLODFromScreenSize(LODSettings, InScreenSizeSquared, InViewLODScale, FractionalLOD);
}

namespace
{
	FLinearColor GetColorForLod(int32 CurrentLOD, int32 ForcedLOD, bool DisplayCombinedBatch)
	{
		int32 ColorIndex = INDEX_NONE;
		if (GEngine->LODColorationColors.Num() > 0)
		{
			ColorIndex = CurrentLOD;
			ColorIndex = FMath::Clamp(ColorIndex, 0, GEngine->LODColorationColors.Num() - 1);
		}
		const FLinearColor& LODColor = ColorIndex != INDEX_NONE ? GEngine->LODColorationColors[ColorIndex] : FLinearColor::Gray;

		if (ForcedLOD >= 0)
		{
			return LODColor;
		}

		if (DisplayCombinedBatch)
		{
			return LODColor * 0.2f;
		}

		return LODColor * 0.1f;
	}
}

void FLandscapeComponentSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FLandscapeComponentSceneProxy_GetMeshElements);
	SCOPE_CYCLE_COUNTER(STAT_LandscapeDynamicDrawTime);

	if (!bRegistered)
	{
		return;
	}

	int32 NumPasses = 0;
	int32 NumTriangles = 0;
	int32 NumDrawCalls = 0;
	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	const FLandscapeRenderSystem& RenderSystem = *LandscapeRenderSystems.FindChecked(LandscapeKey);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			FLandscapeElementParamArray& ParameterArray = Collector.AllocateOneFrameResource<FLandscapeElementParamArray>();
			ParameterArray.ElementParams.AddDefaulted(1);

			const FSceneView* View = Views[ViewIndex];

			int32 LODToRender = RenderSystem.GetSectionLODValue(*View, ComponentBase);

			FMeshBatch& Mesh = Collector.AllocateMesh();
			GetStaticMeshElement(LODToRender, false, Mesh, ParameterArray.ElementParams);

#if WITH_EDITOR
			FMeshBatch& MeshTools = Collector.AllocateMesh();
			// No Tessellation on tool material
			GetStaticMeshElement(LODToRender, true, MeshTools, ParameterArray.ElementParams);
#endif

			// Render the landscape component
#if WITH_EDITOR
			switch (GLandscapeViewMode)
			{
			case ELandscapeViewMode::DebugLayer:
			{
				if (GLayerDebugColorMaterial)
				{
					auto DebugColorMaterialInstance = new FLandscapeDebugMaterialRenderProxy(GLayerDebugColorMaterial->GetRenderProxy(),
						(EditToolRenderData.DebugChannelR >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelR / 4] : nullptr),
						(EditToolRenderData.DebugChannelG >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelG / 4] : nullptr),
						(EditToolRenderData.DebugChannelB >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelB / 4] : nullptr),
						(EditToolRenderData.DebugChannelR >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelR % 4] : DebugColorMask::Masks[4]),
						(EditToolRenderData.DebugChannelG >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelG % 4] : DebugColorMask::Masks[4]),
						(EditToolRenderData.DebugChannelB >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelB % 4] : DebugColorMask::Masks[4])
					);

					MeshTools.MaterialRenderProxy = DebugColorMaterialInstance;
					Collector.RegisterOneFrameMaterialProxy(DebugColorMaterialInstance);

					MeshTools.bCanApplyViewModeOverrides = true;
					MeshTools.bUseWireframeSelectionColoring = IsSelected();

					Collector.AddMesh(ViewIndex, MeshTools);

					NumPasses++;
					NumTriangles += MeshTools.GetNumPrimitives();
					NumDrawCalls += MeshTools.Elements.Num();
				}
			}
			break;

			case ELandscapeViewMode::LayerDensity:
			{
				int32 ColorIndex = FMath::Min<int32>(NumWeightmapLayerAllocations, GEngine->ShaderComplexityColors.Num());
				auto LayerDensityMaterialInstance = new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), ColorIndex ? GEngine->ShaderComplexityColors[ColorIndex - 1] : FLinearColor::Black);

				MeshTools.MaterialRenderProxy = LayerDensityMaterialInstance;
				Collector.RegisterOneFrameMaterialProxy(LayerDensityMaterialInstance);

				MeshTools.bCanApplyViewModeOverrides = true;
				MeshTools.bUseWireframeSelectionColoring = IsSelected();

				Collector.AddMesh(ViewIndex, MeshTools);

				NumPasses++;
				NumTriangles += MeshTools.GetNumPrimitives();
				NumDrawCalls += MeshTools.Elements.Num();
			}
			break;

			case ELandscapeViewMode::LayerUsage:
			{
				if (GLandscapeLayerUsageMaterial)
				{
					float Rotation = ((SectionBase.X / ComponentSizeQuads) ^ (SectionBase.Y / ComponentSizeQuads)) & 1 ? 0 : 2.f * PI;
					auto LayerUsageMaterialInstance = new FLandscapeLayerUsageRenderProxy(GLandscapeLayerUsageMaterial->GetRenderProxy(), ComponentSizeVerts, LayerColors, Rotation);
					MeshTools.MaterialRenderProxy = LayerUsageMaterialInstance;
					Collector.RegisterOneFrameMaterialProxy(LayerUsageMaterialInstance);
					MeshTools.bCanApplyViewModeOverrides = true;
					MeshTools.bUseWireframeSelectionColoring = IsSelected();
					Collector.AddMesh(ViewIndex, MeshTools);
					NumPasses++;
					NumTriangles += MeshTools.GetNumPrimitives();
					NumDrawCalls += MeshTools.Elements.Num();
				}
			}
			break;

			case ELandscapeViewMode::LOD:
			{

				const bool bMaterialModifiesMeshPosition = Mesh.MaterialRenderProxy->GetIncompleteMaterialWithFallback(View->GetFeatureLevel()).MaterialModifiesMeshPosition_RenderThread();

				auto& TemplateMesh = bIsWireframe ? Mesh : MeshTools;
				for (int32 i = 0; i < TemplateMesh.Elements.Num(); i++)
				{
					FMeshBatch& LODMesh = Collector.AllocateMesh();
					LODMesh = TemplateMesh;
					LODMesh.Elements.Empty(1);
					LODMesh.Elements.Add(TemplateMesh.Elements[i]);
					int32 CurrentLOD = ((FLandscapeBatchElementParams*)TemplateMesh.Elements[i].UserData)->CurrentLOD;
					LODMesh.VisualizeLODIndex = CurrentLOD;
					FLinearColor Color = GetColorForLod(CurrentLOD, LODSettings.ForcedLOD, true);
					FMaterialRenderProxy* LODMaterialProxy = (FMaterialRenderProxy*)new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), Color);
					Collector.RegisterOneFrameMaterialProxy(LODMaterialProxy);
					LODMesh.MaterialRenderProxy = LODMaterialProxy;
					LODMesh.bCanApplyViewModeOverrides = !bIsWireframe;
					LODMesh.bWireframe = bIsWireframe;
					LODMesh.bUseWireframeSelectionColoring = IsSelected();
					Collector.AddMesh(ViewIndex, LODMesh);

					NumTriangles += TemplateMesh.Elements[i].NumPrimitives;
					NumDrawCalls++;
				}
				NumPasses++;

			}
			break;

			case ELandscapeViewMode::WireframeOnTop:
			{
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
				NumPasses++;
				NumTriangles += Mesh.GetNumPrimitives();
				NumDrawCalls += Mesh.Elements.Num();

				// wireframe on top
				FMeshBatch& WireMesh = Collector.AllocateMesh();
				WireMesh = MeshTools;
				auto WireMaterialInstance = new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), FLinearColor(0, 0, 1));
				WireMesh.MaterialRenderProxy = WireMaterialInstance;
				Collector.RegisterOneFrameMaterialProxy(WireMaterialInstance);
				WireMesh.bCanApplyViewModeOverrides = false;
				WireMesh.bWireframe = true;
				Collector.AddMesh(ViewIndex, WireMesh);
				NumPasses++;
				NumTriangles += WireMesh.GetNumPrimitives();
				NumDrawCalls++;
			}
			break;

			case ELandscapeViewMode::LayerContribution:
			{
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
				NumPasses++;
				NumTriangles += Mesh.GetNumPrimitives();
				NumDrawCalls += Mesh.Elements.Num();

				FMeshBatch& MaskMesh = Collector.AllocateMesh();
				MaskMesh = MeshTools;
				auto ColorMaskMaterialInstance = new FLandscapeMaskMaterialRenderProxy(GColorMaskRegionMaterial->GetRenderProxy(), EditToolRenderData.LayerContributionTexture ? ToRawPtr(EditToolRenderData.LayerContributionTexture) : GLandscapeBlackTexture, true);
				MaskMesh.MaterialRenderProxy = ColorMaskMaterialInstance;
				Collector.RegisterOneFrameMaterialProxy(ColorMaskMaterialInstance);
				Collector.AddMesh(ViewIndex, MaskMesh);
				NumPasses++;
				NumTriangles += MaskMesh.GetNumPrimitives();
				NumDrawCalls += MaskMesh.Elements.Num();
			}
			break;

			default:

#endif // WITH_EDITOR

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				const bool bInCollisionView = View->Family->EngineShowFlags.CollisionVisibility || View->Family->EngineShowFlags.CollisionPawn;
				if (AllowDebugViewmodes() && bInCollisionView)
				{
					const bool bDrawSimpleCollision = View->Family->EngineShowFlags.CollisionPawn && CollisionResponse.GetResponse(ECC_Pawn) != ECR_Ignore;
					const bool bDrawComplexCollision = View->Family->EngineShowFlags.CollisionVisibility && CollisionResponse.GetResponse(ECC_Visibility) != ECR_Ignore;
					if (bDrawSimpleCollision || bDrawComplexCollision)
					{
						// Override the mesh's material with our material that draws the collision color
						auto CollisionMaterialInstance = new FColoredMaterialRenderProxy(
							GEngine->ShadedLevelColorationUnlitMaterial->GetRenderProxy(),
							GetWireframeColor()
						);
						Collector.RegisterOneFrameMaterialProxy(CollisionMaterialInstance);

						Mesh.MaterialRenderProxy = CollisionMaterialInstance;
						Mesh.bCanApplyViewModeOverrides = true;
						Mesh.bUseWireframeSelectionColoring = IsSelected();

						Collector.AddMesh(ViewIndex, Mesh);

						NumPasses++;
						NumTriangles += Mesh.GetNumPrimitives();
						NumDrawCalls += Mesh.Elements.Num();
					}
				}
#if WITH_EDITOR
				else if (CVarLandscapeShowDirty.GetValueOnRenderThread() && GLandscapeDirtyMaterial)
				{
					Mesh.bCanApplyViewModeOverrides = false;
					Collector.AddMesh(ViewIndex, Mesh);
					NumPasses++;
					NumTriangles += Mesh.GetNumPrimitives();
					NumDrawCalls += Mesh.Elements.Num();

					FMeshBatch& MaskMesh = Collector.AllocateMesh();
					MaskMesh = MeshTools;

					auto DirtyMaterialInstance = new FLandscapeMaskMaterialRenderProxy(GLandscapeDirtyMaterial->GetRenderProxy(), EditToolRenderData.DirtyTexture ? ToRawPtr(EditToolRenderData.DirtyTexture) : GLandscapeBlackTexture, true);
					MaskMesh.MaterialRenderProxy = DirtyMaterialInstance;
					Collector.RegisterOneFrameMaterialProxy(DirtyMaterialInstance);
					Collector.AddMesh(ViewIndex, MaskMesh);
					NumPasses++;
					NumTriangles += MaskMesh.GetNumPrimitives();
					NumDrawCalls += MaskMesh.Elements.Num();
				}
#endif
				else
#endif
					// Regular Landscape rendering. Only use the dynamic path if we're rendering a rich view or we've disabled the static path for debugging.
					if (IsRichView(ViewFamily) ||
						GLandscapeDebugOptions.bDisableStatic ||
						bIsWireframe ||
#if WITH_EDITOR
						(IsSelected() && !GLandscapeEditModeActive) ||
						(GetViewLodOverride(*View) >= 0)
#else
						IsSelected()
#endif
						)
					{
						Mesh.bCanApplyViewModeOverrides = true;
						Mesh.bUseWireframeSelectionColoring = IsSelected();

						Collector.AddMesh(ViewIndex, Mesh);

						NumPasses++;
						NumTriangles += Mesh.GetNumPrimitives();
						NumDrawCalls += Mesh.Elements.Num();
					}

#if WITH_EDITOR
			} // switch
#endif

#if WITH_EDITOR
			  // Extra render passes for landscape tools
			if (GLandscapeEditModeActive)
			{
				// Region selection
				if (EditToolRenderData.SelectedType)
				{
					if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::SelectRegion) && (EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_REGION)
						&& !(GLandscapeEditRenderMode & ELandscapeEditRenderMode::Mask))
					{
						FMeshBatch& SelectMesh = Collector.AllocateMesh();
						SelectMesh = MeshTools;
						auto SelectMaterialInstance = new FLandscapeSelectMaterialRenderProxy(GSelectionRegionMaterial->GetRenderProxy(), EditToolRenderData.DataTexture ? ToRawPtr(EditToolRenderData.DataTexture) : GLandscapeBlackTexture);
						SelectMesh.MaterialRenderProxy = SelectMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(SelectMaterialInstance);
						Collector.AddMesh(ViewIndex, SelectMesh);
						NumPasses++;
						NumTriangles += SelectMesh.GetNumPrimitives();
						NumDrawCalls += SelectMesh.Elements.Num();
					}

					if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::SelectComponent) && (EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_COMPONENT))
					{
						FMeshBatch& SelectMesh = Collector.AllocateMesh();
						SelectMesh = MeshTools;
						SelectMesh.MaterialRenderProxy = GSelectionColorMaterial->GetRenderProxy();
						Collector.AddMesh(ViewIndex, SelectMesh);
						NumPasses++;
						NumTriangles += SelectMesh.GetNumPrimitives();
						NumDrawCalls += SelectMesh.Elements.Num();
					}
				}

				// Mask
				if ((GLandscapeEditRenderMode & ELandscapeEditRenderMode::SelectRegion) && (GLandscapeEditRenderMode & ELandscapeEditRenderMode::Mask))
				{
					if (EditToolRenderData.SelectedType & FLandscapeEditToolRenderData::ST_REGION)
					{
						FMeshBatch& MaskMesh = Collector.AllocateMesh();
						MaskMesh = MeshTools;
						auto MaskMaterialInstance = new FLandscapeMaskMaterialRenderProxy(GMaskRegionMaterial->GetRenderProxy(), EditToolRenderData.DataTexture ? ToRawPtr(EditToolRenderData.DataTexture) : GLandscapeBlackTexture, !!(GLandscapeEditRenderMode & ELandscapeEditRenderMode::InvertedMask));
						MaskMesh.MaterialRenderProxy = MaskMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(MaskMaterialInstance);
						Collector.AddMesh(ViewIndex, MaskMesh);
						NumPasses++;
						NumTriangles += MaskMesh.GetNumPrimitives();
						NumDrawCalls += MaskMesh.Elements.Num();
					}
					else if (!(GLandscapeEditRenderMode & ELandscapeEditRenderMode::InvertedMask))
					{
						FMeshBatch& MaskMesh = Collector.AllocateMesh();
						MaskMesh = MeshTools;
						auto MaskMaterialInstance = new FLandscapeMaskMaterialRenderProxy(GMaskRegionMaterial->GetRenderProxy(), GLandscapeBlackTexture, false);
						MaskMesh.MaterialRenderProxy = MaskMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(MaskMaterialInstance);
						Collector.AddMesh(ViewIndex, MaskMesh);
						NumPasses++;
						NumTriangles += MaskMesh.GetNumPrimitives();
						NumDrawCalls += MaskMesh.Elements.Num();
					}
				}

				// Edit mode tools
				if (EditToolRenderData.ToolMaterial)
				{
					FMeshBatch& EditMesh = Collector.AllocateMesh();
					EditMesh = MeshTools;
					EditMesh.MaterialRenderProxy = EditToolRenderData.ToolMaterial->GetRenderProxy();
					Collector.AddMesh(ViewIndex, EditMesh);
					NumPasses++;
					NumTriangles += EditMesh.GetNumPrimitives();
					NumDrawCalls += EditMesh.Elements.Num();
				}

				if (EditToolRenderData.GizmoMaterial && GLandscapeEditRenderMode & ELandscapeEditRenderMode::Gizmo)
				{
					FMeshBatch& EditMesh = Collector.AllocateMesh();
					EditMesh = MeshTools;
					EditMesh.MaterialRenderProxy = EditToolRenderData.GizmoMaterial->GetRenderProxy();
					Collector.AddMesh(ViewIndex, EditMesh);
					NumPasses++;
					NumTriangles += EditMesh.GetNumPrimitives();
					NumDrawCalls += EditMesh.Elements.Num();
				}
			}
#endif // WITH_EDITOR

			if (GLandscapeDebugOptions.bShowPatches)
			{
				DrawWireBox(Collector.GetPDI(ViewIndex), GetBounds().GetBox(), FColor(255, 255, 0), SDPG_World);
			}

			if (ViewFamily.EngineShowFlags.Bounds)
			{
				RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
			}
		}
	}

	INC_DWORD_STAT_BY(STAT_LandscapeComponentRenderPasses, NumPasses);
	INC_DWORD_STAT_BY(STAT_LandscapeDrawCalls, NumDrawCalls);
	INC_DWORD_STAT_BY(STAT_LandscapeTriangles, NumTriangles * NumPasses);
}

#if RHI_RAYTRACING
void FLandscapeComponentSceneProxy::GetDynamicRayTracingInstances(FRayTracingMaterialGatheringContext& Context, TArray<FRayTracingInstance>& OutRayTracingInstances)
{
	if (!bRegistered || !CVarRayTracingLandscape.GetValueOnRenderThread())
	{
		return;
	}

	const FSceneView& SceneView = *Context.ReferenceView;
	const FLandscapeRenderSystem& RenderSystem = *LandscapeRenderSystems.FindChecked(LandscapeKey);

	int32 LODToRender = RenderSystem.GetSectionLODValue(SceneView, ComponentBase);

	FLandscapeElementParamArray& ParameterArray = Context.RayTracingMeshResourceCollector.AllocateOneFrameResource<FLandscapeElementParamArray>();
	ParameterArray.ElementParams.AddDefaulted(NumSubsections * NumSubsections);

	if (AvailableMaterials.Num() == 0)
	{
		return;
	}

	const int8 CurrentLODIndex = LODToRender;
	int8 MaterialIndex = LODIndexToMaterialIndex.IsValidIndex(CurrentLODIndex) ? LODIndexToMaterialIndex[CurrentLODIndex] : INDEX_NONE;
	UMaterialInterface* SelectedMaterial = MaterialIndex != INDEX_NONE ? AvailableMaterials[MaterialIndex] : nullptr;

	// this is really not normal that we have no material at this point, so do not continue
	if (SelectedMaterial == nullptr)
	{
		return;
	}

	FMeshBatch BaseMeshBatch;
	BaseMeshBatch.VertexFactory = VertexFactory;
	BaseMeshBatch.MaterialRenderProxy = SelectedMaterial->GetRenderProxy();
	BaseMeshBatch.LCI = ComponentLightInfo.Get();
	BaseMeshBatch.CastShadow = true;
	BaseMeshBatch.CastRayTracedShadow = true;
	BaseMeshBatch.bUseForMaterial = true;
	BaseMeshBatch.SegmentIndex = 0;

	BaseMeshBatch.Elements.Empty();

	for (int32 SubY = 0; SubY < NumSubsections; SubY++)
	{
		for (int32 SubX = 0; SubX < NumSubsections; SubX++)
		{
			const int8 SubSectionIdx = SubX + SubY * NumSubsections;
			const int8 CurrentLOD = LODToRender;

			FMeshBatch MeshBatch = BaseMeshBatch;

			FMeshBatchElement BatchElement;
			FLandscapeBatchElementParams& BatchElementParams = ParameterArray.ElementParams[SubSectionIdx];

			BatchElementParams.LandscapeUniformShaderParametersResource = &LandscapeUniformShaderParameters;
			BatchElementParams.FixedGridUniformShaderParameters = &LandscapeFixedGridUniformShaderParameters;
			BatchElementParams.LandscapeSectionLODUniformParameters = RenderSystem.SectionLODUniformBuffer;
			BatchElementParams.SceneProxy = this;
			BatchElementParams.CurrentLOD = CurrentLOD;
			BatchElement.UserData = &BatchElementParams;
			BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

			int32 LodSubsectionSizeVerts = SubsectionSizeVerts >> CurrentLOD;

			if (LodSubsectionSizeVerts <= 0)
			{
				continue;
			}

			uint32 NumPrimitives = FMath::Square(LodSubsectionSizeVerts - 1) * 2;

			BatchElement.IndexBuffer = SharedBuffers->ZeroOffsetIndexBuffers[CurrentLOD];
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = NumPrimitives;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = 0;

			MeshBatch.Elements.Add(BatchElement);

			SectionRayTracingStates[SubSectionIdx].Geometry.Initializer.IndexBuffer = BatchElement.IndexBuffer->IndexBufferRHI;

			BatchElementParams.LandscapeVertexFactoryMVFUniformBuffer = SectionRayTracingStates[SubSectionIdx].UniformBuffer;

			bool bNeedsRayTracingGeometryUpdate = false;

			// Detect force update CVar
			bNeedsRayTracingGeometryUpdate |= (CurrentLOD <= GLandscapeRayTracingGeometryLODsThatUpdateEveryFrame) ? true : false;

			// Detect continuous LOD parameter changes. This is for far-away high LODs - they change rarely yet the BLAS refit time is not ideal, even if they contains tiny amount of triangles
			{
				if (SectionRayTracingStates[SubSectionIdx].CurrentLOD != CurrentLOD)
				{
					bNeedsRayTracingGeometryUpdate = true;
					SectionRayTracingStates[SubSectionIdx].CurrentLOD = CurrentLOD;
					SectionRayTracingStates[SubSectionIdx].RayTracingDynamicVertexBuffer.Release();
				}
				if (SectionRayTracingStates[SubSectionIdx].HeightmapLODBias != RenderSystem.GetSectionLODBias(ComponentBase))
				{
					bNeedsRayTracingGeometryUpdate = true;
					SectionRayTracingStates[SubSectionIdx].HeightmapLODBias = RenderSystem.GetSectionLODBias(ComponentBase);
				}

				if (SectionRayTracingStates[SubSectionIdx].FractionalLOD != RenderSystem.GetSectionLODValue(SceneView, ComponentBase))
				{
					bNeedsRayTracingGeometryUpdate = true;
					SectionRayTracingStates[SubSectionIdx].FractionalLOD = RenderSystem.GetSectionLODValue(SceneView, ComponentBase);
				}
			}

			if (GLandscapeRayTracingGeometryDetectTextureStreaming > 0)
			{
				const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
				const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(((FSceneInterface*)Context.Scene)->GetFeatureLevel(), FallbackMaterialRenderProxyPtr);

				if (Material.HasVertexPositionOffsetConnected())
				{
					const FMaterialRenderProxy* MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? FallbackMaterialRenderProxyPtr : MeshBatch.MaterialRenderProxy;

					FMaterialRenderContext MaterialRenderContext(MaterialRenderProxy, Material, Context.ReferenceView);

					const FUniformExpressionSet& UniformExpressionSet = Material.GetRenderingThreadShaderMap()->GetUniformExpressionSet();
					const uint32 Hash = UniformExpressionSet.GetReferencedTexture2DRHIHash(MaterialRenderContext);

					if (SectionRayTracingStates[SubSectionIdx].ReferencedTextureRHIHash != Hash)
					{
						bNeedsRayTracingGeometryUpdate = true;
						SectionRayTracingStates[SubSectionIdx].ReferencedTextureRHIHash = Hash;
					}
				}
			}

			FRayTracingInstance RayTracingInstance;
			RayTracingInstance.Geometry = &SectionRayTracingStates[SubSectionIdx].Geometry;
			RayTracingInstance.InstanceTransforms.Add(GetLocalToWorld());
			RayTracingInstance.Materials.Add(MeshBatch);
			RayTracingInstance.BuildInstanceMaskAndFlags(GetScene().GetFeatureLevel());
			OutRayTracingInstances.Add(RayTracingInstance);

			if (bNeedsRayTracingGeometryUpdate && VertexFactory->GetType()->SupportsRayTracingDynamicGeometry())
			{
				// Use the internal managed vertex buffer because landscape dynamic RT geometries are not updated every frame
				// which is a requirement for the shared vertex buffer usage

				Context.DynamicRayTracingGeometriesToUpdate.Add(
					FRayTracingDynamicGeometryUpdateParams
					{
						RayTracingInstance.Materials,
						false,
						(uint32)FMath::Square(LodSubsectionSizeVerts),
						FMath::Square(LodSubsectionSizeVerts) * (uint32)sizeof(FVector3f),
						(uint32)FMath::Square(LodSubsectionSizeVerts - 1) * 2,
						&SectionRayTracingStates[SubSectionIdx].Geometry,
						&SectionRayTracingStates[SubSectionIdx].RayTracingDynamicVertexBuffer,
						true
					}
				);
			}
		}
	}
}
#endif

//
// FLandscapeVertexBuffer
//

/**
* Initialize the RHI for this rendering resource
*/
void FLandscapeVertexBuffer::InitRHI()
{
	SCOPED_LOADTIMER(FLandscapeVertexBuffer_InitRHI);

	// create a static vertex buffer
	FRHIResourceCreateInfo CreateInfo(TEXT("FLandscapeVertexBuffer"));
	VertexBufferRHI = RHICreateBuffer(NumVertices * sizeof(FLandscapeVertex), BUF_Static | BUF_VertexBuffer, 0, ERHIAccess::VertexOrIndexBuffer, CreateInfo);
	FLandscapeVertex* Vertex = (FLandscapeVertex*)RHILockBuffer(VertexBufferRHI, 0, NumVertices * sizeof(FLandscapeVertex), RLM_WriteOnly);
	int32 VertexIndex = 0;
	for (int32 SubY = 0; SubY < NumSubsections; SubY++)
	{
		for (int32 SubX = 0; SubX < NumSubsections; SubX++)
		{
			for (int32 y = 0; y < SubsectionSizeVerts; y++)
			{
				for (int32 x = 0; x < SubsectionSizeVerts; x++)
				{
					Vertex->VertexX = x;
					Vertex->VertexY = y;
					Vertex->SubX = SubX;
					Vertex->SubY = SubY;
					Vertex++;
					VertexIndex++;
				}
			}
		}
	}
	check(NumVertices == VertexIndex);
	RHIUnlockBuffer(VertexBufferRHI);
}

//
// FLandscapeSharedBuffers
//

template <typename INDEX_TYPE>
void FLandscapeSharedBuffers::CreateIndexBuffers(ERHIFeatureLevel::Type InFeatureLevel)
{
	if (InFeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		if (!bVertexScoresComputed)
		{
			bVertexScoresComputed = ComputeVertexScores();
		}
	}

	TArray<INDEX_TYPE> VertexToIndexMap;
	VertexToIndexMap.AddUninitialized(FMath::Square(SubsectionSizeVerts * NumSubsections));
	FMemory::Memset(VertexToIndexMap.GetData(), 0xff, NumVertices * sizeof(INDEX_TYPE));

	INDEX_TYPE VertexCount = 0;
	int32 SubsectionSizeQuads = SubsectionSizeVerts - 1;

	// Layout index buffer to determine best vertex order
	int32 MaxLOD = NumIndexBuffers - 1;
	for (int32 Mip = MaxLOD; Mip >= 0; Mip--)
	{
		int32 LodSubsectionSizeQuads = (SubsectionSizeVerts >> Mip) - 1;

		TArray<INDEX_TYPE> NewIndices;
		int32 ExpectedNumIndices = FMath::Square(NumSubsections) * FMath::Square(LodSubsectionSizeQuads) * 6;
		NewIndices.Empty(ExpectedNumIndices);

		int32& MaxIndexFull = IndexRanges[Mip].MaxIndexFull;
		int32& MinIndexFull = IndexRanges[Mip].MinIndexFull;
		MaxIndexFull = 0;
		MinIndexFull = MAX_int32;

		if (InFeatureLevel <= ERHIFeatureLevel::ES3_1)
		{
			// mobile version shares vertices across LODs to save memory
			float MipRatio = (float)SubsectionSizeQuads / (float)LodSubsectionSizeQuads; // Morph current MIP to base MIP

			for (int32 SubY = 0; SubY < NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < NumSubsections; SubX++)
				{
					TArray<INDEX_TYPE> SubIndices;
					SubIndices.Empty(FMath::Square(LodSubsectionSizeQuads) * 6);

					int32& MaxIndex = IndexRanges[Mip].MaxIndex[SubX][SubY];
					int32& MinIndex = IndexRanges[Mip].MinIndex[SubX][SubY];
					MaxIndex = 0;
					MinIndex = MAX_int32;

					for (int32 Y = 0; Y < LodSubsectionSizeQuads; Y++)
					{
						for (int32 X = 0; X < LodSubsectionSizeQuads; X++)
						{
							INDEX_TYPE QuadIndices[4];

							for (int32 CornerId = 0; CornerId < 4; CornerId++)
							{
								const int32 CornerX = FMath::RoundToInt((float)(X + (CornerId & 1)) * MipRatio);
								const int32 CornerY = FMath::RoundToInt((float)(Y + (CornerId >> 1)) * MipRatio);
								const FLandscapeVertexRef VertexRef(CornerX, CornerY, SubX, SubY);

								const INDEX_TYPE VertexIndex = FLandscapeVertexRef::GetVertexIndex(VertexRef, NumSubsections, SubsectionSizeVerts);
								if (VertexToIndexMap[VertexIndex] == INDEX_TYPE(-1))
								{
									VertexToIndexMap[VertexIndex] = QuadIndices[CornerId] = VertexCount++;
								}
								else
								{
									QuadIndices[CornerId] = VertexToIndexMap[VertexIndex];
								}

								// update the min/max index ranges
								MaxIndex = FMath::Max<int32>(MaxIndex, QuadIndices[CornerId]);
								MinIndex = FMath::Min<int32>(MinIndex, QuadIndices[CornerId]);
							}

							SubIndices.Add(QuadIndices[0]);
							SubIndices.Add(QuadIndices[3]);
							SubIndices.Add(QuadIndices[1]);

							SubIndices.Add(QuadIndices[0]);
							SubIndices.Add(QuadIndices[2]);
							SubIndices.Add(QuadIndices[3]);
						}
					}

					// update min/max for full subsection
					MaxIndexFull = FMath::Max<int32>(MaxIndexFull, MaxIndex);
					MinIndexFull = FMath::Min<int32>(MinIndexFull, MinIndex);

					TArray<INDEX_TYPE> NewSubIndices;
					::OptimizeFaces<INDEX_TYPE>(SubIndices, NewSubIndices, 32);
					NewIndices.Append(NewSubIndices);
				}
			}
		}
		else
		{
			// non-mobile version
			int32 SubOffset = 0;
			for (int32 SubY = 0; SubY < NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < NumSubsections; SubX++)
				{
					int32& MaxIndex = IndexRanges[Mip].MaxIndex[SubX][SubY];
					int32& MinIndex = IndexRanges[Mip].MinIndex[SubX][SubY];
					MaxIndex = 0;
					MinIndex = MAX_int32;

					for (int32 y = 0; y < LodSubsectionSizeQuads; y++)
					{
						for (int32 x = 0; x < LodSubsectionSizeQuads; x++)
						{
							INDEX_TYPE i00 = (x + 0) + (y + 0) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i10 = (x + 1) + (y + 0) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i11 = (x + 1) + (y + 1) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i01 = (x + 0) + (y + 1) * SubsectionSizeVerts + SubOffset;

							NewIndices.Add(i00);
							NewIndices.Add(i11);
							NewIndices.Add(i10);

							NewIndices.Add(i00);
							NewIndices.Add(i01);
							NewIndices.Add(i11);

							// Update the min/max index ranges
							MaxIndex = FMath::Max<int32>(MaxIndex, i00);
							MinIndex = FMath::Min<int32>(MinIndex, i00);
							MaxIndex = FMath::Max<int32>(MaxIndex, i10);
							MinIndex = FMath::Min<int32>(MinIndex, i10);
							MaxIndex = FMath::Max<int32>(MaxIndex, i11);
							MinIndex = FMath::Min<int32>(MinIndex, i11);
							MaxIndex = FMath::Max<int32>(MaxIndex, i01);
							MinIndex = FMath::Min<int32>(MinIndex, i01);
						}
					}

					// update min/max for full subsection
					MaxIndexFull = FMath::Max<int32>(MaxIndexFull, MaxIndex);
					MinIndexFull = FMath::Min<int32>(MinIndexFull, MinIndex);

					SubOffset += FMath::Square(SubsectionSizeVerts);
				}
			}

			check(MinIndexFull <= (uint32)((INDEX_TYPE)(~(INDEX_TYPE)0)));
			check(NewIndices.Num() == ExpectedNumIndices);
		}

		// Create and init new index buffer with index data
		FRawStaticIndexBuffer16or32<INDEX_TYPE>* IndexBuffer = (FRawStaticIndexBuffer16or32<INDEX_TYPE>*)IndexBuffers[Mip];
		if (!IndexBuffer)
		{
			IndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
		}
		IndexBuffer->AssignNewBuffer(NewIndices);

		IndexBuffer->InitResource();

		IndexBuffers[Mip] = IndexBuffer;

#if RHI_RAYTRACING
		if (IsRayTracingEnabled())
		{
			TArray<INDEX_TYPE> ZeroOffsetIndices;

			for (int32 y = 0; y < LodSubsectionSizeQuads; y++)
			{
				for (int32 x = 0; x < LodSubsectionSizeQuads; x++)
				{
					INDEX_TYPE i00 = (x + 0) + (y + 0) * (SubsectionSizeVerts >> Mip);
					INDEX_TYPE i10 = (x + 1) + (y + 0) * (SubsectionSizeVerts >> Mip);
					INDEX_TYPE i11 = (x + 1) + (y + 1) * (SubsectionSizeVerts >> Mip);
					INDEX_TYPE i01 = (x + 0) + (y + 1) * (SubsectionSizeVerts >> Mip);

					ZeroOffsetIndices.Add(i00);
					ZeroOffsetIndices.Add(i11);
					ZeroOffsetIndices.Add(i10);

					ZeroOffsetIndices.Add(i00);
					ZeroOffsetIndices.Add(i01);
					ZeroOffsetIndices.Add(i11);
				}
			}

			FRawStaticIndexBuffer16or32<INDEX_TYPE>* ZeroOffsetIndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
			ZeroOffsetIndexBuffer->AssignNewBuffer(ZeroOffsetIndices);
			ZeroOffsetIndexBuffer->InitResource();
			ZeroOffsetIndexBuffers[Mip] = ZeroOffsetIndexBuffer;
		}
#endif
	}
}

#if WITH_EDITOR
template <typename INDEX_TYPE>
void FLandscapeSharedBuffers::CreateGrassIndexBuffer()
{
	TArray<INDEX_TYPE> NewIndices;

	int32 ExpectedNumIndices = FMath::Square(NumSubsections) * (FMath::Square(SubsectionSizeVerts) * 4 / 3 - 1); // *4/3 is for mips, -1 because we only go down to 2x2 not 1x1
	NewIndices.Empty(ExpectedNumIndices);

	int32 NumMips = FMath::CeilLogTwo(SubsectionSizeVerts);

	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		// Store offset to the start of this mip in the index buffer
		GrassIndexMipOffsets.Add(NewIndices.Num());

		int32 MipSubsectionSizeVerts = SubsectionSizeVerts >> Mip;
		int32 SubOffset = 0;
		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				for (int32 y = 0; y < MipSubsectionSizeVerts; y++)
				{
					for (int32 x = 0; x < MipSubsectionSizeVerts; x++)
					{
						// intentionally using SubsectionSizeVerts not MipSubsectionSizeVerts, this is a vert buffer index not a mip vert index
						NewIndices.Add(x + y * SubsectionSizeVerts + SubOffset);
					}
				}

				// intentionally using SubsectionSizeVerts not MipSubsectionSizeVerts (as above)
				SubOffset += FMath::Square(SubsectionSizeVerts);
			}
		}
	}

	check(NewIndices.Num() == ExpectedNumIndices);

	// Create and init new index buffer with index data
	FRawStaticIndexBuffer16or32<INDEX_TYPE>* IndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
	IndexBuffer->AssignNewBuffer(NewIndices);
	IndexBuffer->InitResource();
	GrassIndexBuffer = IndexBuffer;
}
#endif

FLandscapeSharedBuffers::FLandscapeSharedBuffers(const int32 InSharedBuffersKey, const int32 InSubsectionSizeQuads, const int32 InNumSubsections, const ERHIFeatureLevel::Type InFeatureLevel)
	: SharedBuffersKey(InSharedBuffersKey)
	, NumIndexBuffers(FMath::CeilLogTwo(InSubsectionSizeQuads + 1))
	, SubsectionSizeVerts(InSubsectionSizeQuads + 1)
	, NumSubsections(InNumSubsections)
	, VertexFactory(nullptr)
	, FixedGridVertexFactory(nullptr)
	, VertexBuffer(nullptr)
	, bUse32BitIndices(false)
#if WITH_EDITOR
	, GrassIndexBuffer(nullptr)
#endif
{
	NumVertices = FMath::Square(SubsectionSizeVerts) * FMath::Square(NumSubsections);
	if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
	{
		// Vertex Buffer cannot be shared
		VertexBuffer = new FLandscapeVertexBuffer(InFeatureLevel, NumVertices, SubsectionSizeVerts, NumSubsections);
	}
	IndexBuffers = new FIndexBuffer * [NumIndexBuffers];
	FMemory::Memzero(IndexBuffers, sizeof(FIndexBuffer*) * NumIndexBuffers);
	IndexRanges = new FLandscapeIndexRanges[NumIndexBuffers]();

#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		ZeroOffsetIndexBuffers.AddZeroed(NumIndexBuffers);
	}
#endif

	// See if we need to use 16 or 32-bit index buffers
	if (NumVertices > 65535)
	{
		bUse32BitIndices = true;
		CreateIndexBuffers<uint32>(InFeatureLevel);
#if WITH_EDITOR
		if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
		{
			CreateGrassIndexBuffer<uint32>();
		}
#endif
	}
	else
	{
		CreateIndexBuffers<uint16>(InFeatureLevel);
#if WITH_EDITOR
		if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
		{
			CreateGrassIndexBuffer<uint16>();
		}
#endif
	}
}

FLandscapeSharedBuffers::~FLandscapeSharedBuffers()
{
	delete VertexBuffer;

	for (int32 i = 0; i < NumIndexBuffers; i++)
	{
		IndexBuffers[i]->ReleaseResource();
		delete IndexBuffers[i];
	}
	delete[] IndexBuffers;
	delete[] IndexRanges;

#if RHI_RAYTRACING
	if (IsRayTracingEnabled())
	{
		while (ZeroOffsetIndexBuffers.Num() > 0)
		{
			FIndexBuffer* Buffer = ZeroOffsetIndexBuffers.Pop();
			Buffer->ReleaseResource();
			delete Buffer;
		}
	}
#endif

#if WITH_EDITOR
	if (GrassIndexBuffer)
	{
		GrassIndexBuffer->ReleaseResource();
		delete GrassIndexBuffer;
	}
#endif

	delete VertexFactory;
}

//
// FLandscapeVertexFactoryVertexShaderParameters
//

/** Shader parameters for use with FLandscapeVertexFactory */
class FLandscapeVertexFactoryVertexShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FLandscapeVertexFactoryVertexShaderParameters, NonVirtual);
public:
	/**
	* Bind shader constants by name
	* @param	ParameterMap - mapping of named shader constants to indices
	*/
	void Bind(const FShaderParameterMap& ParameterMap)
	{
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimeVS);

		const FLandscapeBatchElementParams* BatchElementParams = (const FLandscapeBatchElementParams*)BatchElement.UserData;
		check(BatchElementParams);

		const FLandscapeComponentSceneProxy* SceneProxy = BatchElementParams->SceneProxy;

		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeUniformShaderParameters>(), *BatchElementParams->LandscapeUniformShaderParametersResource);
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeSectionLODUniformParameters>(), BatchElementParams->LandscapeSectionLODUniformParameters);

#if RHI_RAYTRACING
		if (IsRayTracingEnabled())
		{
			ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeVertexFactoryMVFParameters>(), BatchElementParams->LandscapeVertexFactoryMVFUniformBuffer);
		}
#endif
	}
};

IMPLEMENT_TYPE_LAYOUT(FLandscapeVertexFactoryVertexShaderParameters);

/**
  * Shader parameters for use with FLandscapeFixedGridVertexFactory
  * Simple grid rendering (without dynamic lod blend) needs a simpler fixed setup.
  */
class FLandscapeFixedGridVertexFactoryVertexShaderParameters : public FLandscapeVertexFactoryVertexShaderParameters
{
	DECLARE_TYPE_LAYOUT(FLandscapeFixedGridVertexFactoryVertexShaderParameters, NonVirtual);
public:
	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimeVS);

		const FLandscapeBatchElementParams* BatchElementParams = (const FLandscapeBatchElementParams*)BatchElement.UserData;
		check(BatchElementParams);

		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeUniformShaderParameters>(), *BatchElementParams->LandscapeUniformShaderParametersResource);
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeFixedGridUniformShaderParameters>(), (*BatchElementParams->FixedGridUniformShaderParameters)[BatchElementParams->CurrentLOD]);

#if RHI_RAYTRACING
		if (IsRayTracingEnabled())
		{
			ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeVertexFactoryMVFParameters>(), BatchElementParams->LandscapeVertexFactoryMVFUniformBuffer);
		}
#endif
	}
};

IMPLEMENT_TYPE_LAYOUT(FLandscapeFixedGridVertexFactoryVertexShaderParameters);

//
// FLandscapeVertexFactoryPixelShaderParameters
//

void FLandscapeVertexFactoryPixelShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* InView,
	const class FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	class FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams
) const
{
	SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimePS);

	const FLandscapeBatchElementParams* BatchElementParams = (const FLandscapeBatchElementParams*)BatchElement.UserData;

	ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeUniformShaderParameters>(), *BatchElementParams->LandscapeUniformShaderParametersResource);
}

//
// FLandscapeVertexFactory
//

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeVertexFactoryMVFParameters, "LandscapeMVF");

void FLandscapeVertexFactory::InitRHI()
{
	// list of declaration items
	FVertexDeclarationElementList Elements;

	// position decls
	Elements.Add(AccessStreamComponent(Data.PositionComponent, 0));

	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, Elements, 1, 0xff);
	// create the actual device decls
	InitDeclaration(Elements);
}

FLandscapeVertexFactory::FLandscapeVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
{
}

bool FLandscapeVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// only compile landscape materials for landscape vertex factory
	// The special engine materials must be compiled for the landscape vertex factory because they are used with it for wireframe, etc.
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
		(Parameters.MaterialParameters.bIsUsedWithLandscape || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
}

void FLandscapeVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), Parameters.VertexFactoryType->SupportsPrimitiveIdStream() && UseGPUScene(Parameters.Platform, GetMaxSupportedFeatureLevel(Parameters.Platform)));

	// Make sure landscape vertices go back to local space so that we have consistency between the transform on normals and geometry
	OutEnvironment.SetDefine(TEXT("RAY_TRACING_DYNAMIC_MESH_IN_LOCAL_SPACE"), TEXT("1"));
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactory, SF_Vertex, FLandscapeVertexFactoryVertexShaderParameters);
#if RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactory, SF_Compute, FLandscapeVertexFactoryVertexShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactory, SF_RayHitGroup, FLandscapeVertexFactoryVertexShaderParameters);
#endif // RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactory, SF_Pixel, FLandscapeVertexFactoryPixelShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FLandscapeVertexFactory, "/Engine/Private/LandscapeVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsRayTracing
	| EVertexFactoryFlags::SupportsRayTracingDynamicGeometry
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
);

/**
* Copy the data from another vertex factory
* @param Other - factory to copy from
*/
void FLandscapeVertexFactory::Copy(const FLandscapeVertexFactory& Other)
{
	//SetSceneProxy(Other.Proxy());
	FLandscapeVertexFactory* VertexFactory = this;
	const FDataType* DataCopy = &Other.Data;
	ENQUEUE_RENDER_COMMAND(FLandscapeVertexFactoryCopyData)(
		[VertexFactory, DataCopy](FRHICommandListImmediate& RHICmdList)
	{
		VertexFactory->Data = *DataCopy;
	});
	BeginUpdateResourceRHI(this);
}

//
// FLandscapeXYOffsetVertexFactory
//

void FLandscapeXYOffsetVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLandscapeVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("LANDSCAPE_XYOFFSET"), TEXT("1"));
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeXYOffsetVertexFactory, SF_Vertex, FLandscapeVertexFactoryVertexShaderParameters);
#if RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeXYOffsetVertexFactory, SF_Compute, FLandscapeVertexFactoryVertexShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeXYOffsetVertexFactory, SF_RayHitGroup, FLandscapeVertexFactoryVertexShaderParameters);
#endif // RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeXYOffsetVertexFactory, SF_Pixel, FLandscapeVertexFactoryPixelShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FLandscapeXYOffsetVertexFactory, "/Engine/Private/LandscapeVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsRayTracing
	| EVertexFactoryFlags::SupportsRayTracingDynamicGeometry
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
);

//
// FLandscapeFixedGridVertexFactory
//

void FLandscapeFixedGridVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLandscapeVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("FIXED_GRID"), TEXT("1"));
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactory, SF_Vertex, FLandscapeFixedGridVertexFactoryVertexShaderParameters);
#if RHI_RAYTRACING
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactory, SF_Compute, FLandscapeFixedGridVertexFactoryVertexShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactory, SF_RayHitGroup, FLandscapeFixedGridVertexFactoryVertexShaderParameters);
#endif
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactory, SF_Pixel, FLandscapeVertexFactoryPixelShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FLandscapeFixedGridVertexFactory, "/Engine/Private/LandscapeVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsStaticLighting
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsCachingMeshDrawCommands
	| EVertexFactoryFlags::SupportsRayTracing
	| EVertexFactoryFlags::SupportsRayTracingDynamicGeometry
	| EVertexFactoryFlags::SupportsPrimitiveIdStream
);

/** ULandscapeMaterialInstanceConstant */
ULandscapeMaterialInstanceConstant::ULandscapeMaterialInstanceConstant(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsLayerThumbnail = false;
}

void ULandscapeMaterialInstanceConstant::PostLoad()
{
	Super::PostLoad();
#if WITH_EDITOR
	UpdateCachedTextureStreaming();
#endif // WITH_EDITOR
}

float ULandscapeMaterialInstanceConstant::GetLandscapeTexelFactor(const FName& TextureName) const
{
	for (const FLandscapeMaterialTextureStreamingInfo& Info : TextureStreamingInfo)
	{
		if (Info.TextureName == TextureName)
		{
			return Info.TexelFactor;
		}
	}
	return 1.0f;
}

#if WITH_EDITOR

void ULandscapeMaterialInstanceConstant::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateCachedTextureStreaming();
}

FLandscapeMaterialTextureStreamingInfo& ULandscapeMaterialInstanceConstant::AcquireTextureStreamingInfo(const FName& TextureName)
{
	for (FLandscapeMaterialTextureStreamingInfo& Info : TextureStreamingInfo)
	{
		if (Info.TextureName == TextureName)
		{
			return Info;
		}
	}
	FLandscapeMaterialTextureStreamingInfo& Info = TextureStreamingInfo.AddDefaulted_GetRef();
	Info.TextureName = TextureName;
	Info.TexelFactor = 1.0f;
	return Info;
}

void ULandscapeMaterialInstanceConstant::UpdateCachedTextureStreaming()
{
	// Remove outdated elements that no longer match the material's expressions.
	TextureStreamingInfo.Empty();

	const UMaterial* Material = GetMaterial();
	if (Material)
	{
		int32 NumExpressions = Material->Expressions.Num();
		for (int32 ExpressionIndex = 0; ExpressionIndex < NumExpressions; ExpressionIndex++)
		{
			UMaterialExpression* Expression = Material->Expressions[ExpressionIndex];
			UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression);

			// TODO: This is only works for direct Coordinate Texture Sample cases
			if (TextureSample && TextureSample->Texture && TextureSample->Coordinates.IsConnected())
			{
				if (UMaterialExpressionTextureCoordinate* TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(TextureSample->Coordinates.Expression))
				{
					FLandscapeMaterialTextureStreamingInfo& Info = AcquireTextureStreamingInfo(TextureSample->Texture->GetFName());
					Info.TexelFactor *= FPlatformMath::Max(TextureCoordinate->UTiling, TextureCoordinate->VTiling);
				}
				else if (UMaterialExpressionLandscapeLayerCoords* TerrainTextureCoordinate = Cast<UMaterialExpressionLandscapeLayerCoords>(TextureSample->Coordinates.Expression))
				{
					FLandscapeMaterialTextureStreamingInfo& Info = AcquireTextureStreamingInfo(TextureSample->Texture->GetFName());
					Info.TexelFactor *= TerrainTextureCoordinate->MappingScale;
				}
			}
		}
	}
}

#endif // WITH_EDITOR

class FLandscapeMaterialResource : public FMaterialResource
{
	const bool bIsLayerThumbnail;
	const bool bMobile;
	const bool bEditorToolUsage;

public:
	FLandscapeMaterialResource(ULandscapeMaterialInstanceConstant* Parent)
		: bIsLayerThumbnail(Parent->bIsLayerThumbnail)
		, bMobile(Parent->bMobile)
		, bEditorToolUsage(Parent->bEditorToolUsage)
	{
	}

	bool IsUsedWithLandscape() const override
	{
		return !bIsLayerThumbnail;
	}

	bool IsUsedWithStaticLighting() const override
	{
		if (bIsLayerThumbnail)
		{
			return false;
		}
		return FMaterialResource::IsUsedWithStaticLighting();
	}

	bool IsUsedWithSkeletalMesh()          const override { return false; }
	bool IsUsedWithParticleSystem()        const override { return false; }
	bool IsUsedWithParticleSprites()       const override { return false; }
	bool IsUsedWithBeamTrails()            const override { return false; }
	bool IsUsedWithMeshParticles()         const override { return false; }
	bool IsUsedWithNiagaraSprites()       const override { return false; }
	bool IsUsedWithNiagaraRibbons()       const override { return false; }
	bool IsUsedWithNiagaraMeshParticles()       const override { return false; }
	bool IsUsedWithMorphTargets()          const override { return false; }
	bool IsUsedWithSplineMeshes()          const override { return false; }
	bool IsUsedWithInstancedStaticMeshes() const override { return false; }
	bool IsUsedWithAPEXCloth()             const override { return false; }
	bool IsUsedWithGeometryCache()         const override { return false; }

	bool ShouldCache(EShaderPlatform Platform, const FShaderType* ShaderType, const FVertexFactoryType* VertexFactoryType) const override
	{
		// Don't compile if this is a mobile shadermap and a desktop MIC, and vice versa, unless it's a tool material
		if (!(IsPCPlatform(Platform) && bEditorToolUsage) && bMobile != IsMobilePlatform(Platform))
		{
			// @todo For some reason this causes this resource to return true for IsCompilationFinished. For now we will needlessly compile this shader until this is fixed.
			//return false;
		}

		if (VertexFactoryType)
		{
			// Always check against FLocalVertexFactory in editor builds as it is required to render thumbnails
			// Thumbnail MICs are only rendered in the preview scene using a simple LocalVertexFactory
			if (bIsLayerThumbnail)
			{
				static const FName LocalVertexFactory = FName(TEXT("FLocalVertexFactory"));
				if (!IsMobilePlatform(Platform) && VertexFactoryType->GetFName() == LocalVertexFactory)
				{
					if (Algo::Find(GetAllowedShaderTypesInThumbnailRender(), ShaderType->GetFName()))
					{
						return FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
					}
					else
					{
						// No ray tracing on thumbnails : we don't need any variation of ray hit group shaders : 
						const bool bIsRayHitGroupShader = (ShaderType->GetFrequency() == SF_RayHitGroup);
						if (bIsRayHitGroupShader
							|| Algo::Find(GetExcludedShaderTypesInThumbnailRender(), ShaderType->GetFName()))
						{
							UE_LOG(LogLandscape, VeryVerbose, TEXT("Excluding shader %s from landscape thumbnail material"), ShaderType->GetName());
							return false;
						}
						else
						{
							if (Platform == EShaderPlatform::SP_PCD3D_SM5)
							{
								UE_LOG(LogLandscape, Warning, TEXT("Shader %s unknown by landscape thumbnail material, please add to either AllowedShaderTypes or ExcludedShaderTypes"), ShaderType->GetName());
							}
							return FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
						}
					}
				}
			}
			else
			{
				// Landscape MICs are only for use with the Landscape vertex factories

				// For now only compile FLandscapeFixedGridVertexFactory for grass and runtime virtual texture page rendering (can change if we need for other cases)
				// Todo: only compile LandscapeXYOffsetVertexFactory if we are using it
				bool bIsGrassShaderType = Algo::Find(GetGrassShaderTypes(), ShaderType->GetFName()) != nullptr;
				bool bIsGPULightmassShaderType = Algo::Find(GetGPULightmassShaderTypes(), ShaderType->GetFName()) != nullptr;
				bool bIsRuntimeVirtualTextureShaderType = Algo::Find(GetRuntimeVirtualTextureShaderTypes(), ShaderType->GetFName()) != nullptr;

				bool bIsShaderTypeUsingFixedGrid = bIsGrassShaderType || bIsRuntimeVirtualTextureShaderType || bIsGPULightmassShaderType;

				bool bIsRayTracingShaderType = FName(TEXT("FRayTracingDynamicGeometryConverterCS")) == ShaderType->GetFName();

				static const FName LandscapeVertexFactory = FName(TEXT("FLandscapeVertexFactory"));
				static const FName LandscapeXYOffsetVertexFactory = FName(TEXT("FLandscapeXYOffsetVertexFactory"));
				static const FName LandscapeVertexFactoryMobile = FName(TEXT("FLandscapeVertexFactoryMobile"));
				if (VertexFactoryType->GetFName() == LandscapeVertexFactory ||
					VertexFactoryType->GetFName() == LandscapeXYOffsetVertexFactory ||
					VertexFactoryType->GetFName() == LandscapeVertexFactoryMobile)
				{
					return (bIsRayTracingShaderType || !bIsShaderTypeUsingFixedGrid) && FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
				}

				static const FName LandscapeFixedGridVertexFactory = FName(TEXT("FLandscapeFixedGridVertexFactory"));
				static const FName LandscapeFixedGridVertexFactoryMobile = FName(TEXT("FLandscapeFixedGridVertexFactoryMobile"));
				if (VertexFactoryType->GetFName() == LandscapeFixedGridVertexFactory ||
					VertexFactoryType->GetFName() == LandscapeFixedGridVertexFactoryMobile)
				{
					return (bIsRayTracingShaderType || bIsShaderTypeUsingFixedGrid) && FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
				}
			}
		}

		return false;
	}

	static const TArray<FName>& GetAllowedShaderTypesInThumbnailRender()
	{
		// reduce the number of shaders compiled for the thumbnail materials by only compiling with shader types known to be used by the preview scene
		static const TArray<FName> AllowedShaderTypes =
		{
			FName(TEXT("TBasePassVSFNoLightMapPolicy")),
			FName(TEXT("TBasePassPSFNoLightMapPolicy")),
			FName(TEXT("TBasePassVSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleDirectionalLightLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleDirectionalLightLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleDirectionalLightLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleNoLightmapLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleNoLightmapLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleNoLightmapLightingPolicy")),
			FName(TEXT("TBasePassVSFSimpleNoLightmapLightingPolicySkyAtmosphereAP")),
			FName(TEXT("FAnisotropyVS")),
			FName(TEXT("FAnisotropyPS")),
			FName(TEXT("TDepthOnlyVS<false>")),
			FName(TEXT("TDepthOnlyVS<true>")),
			FName(TEXT("FDepthOnlyPS<true>")),
			FName(TEXT("FDepthOnlyPS<false>")),
			// UE-44519, masked material with landscape layers requires FHitProxy shaders.
			FName(TEXT("FHitProxyVS")),
			FName(TEXT("FHitProxyPS")),
			FName(TEXT("FVelocityVS")),
			FName(TEXT("FVelocityPS")),

			FName(TEXT("TBasePassPSFSimpleStationaryLightSingleSampleShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightSingleSampleShadowsLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightSingleSampleShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightPrecomputedShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightPrecomputedShadowsLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightPrecomputedShadowsLightingPolicy")),
			FName(TEXT("TBasePassVSFNoLightMapPolicySkyAtmosphereAP")),
			FName(TEXT("TLightMapDensityVSFNoLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSFNoLightMapPolicy")),

			// Mobile
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDirectionalLightCSMAndSHIndirectPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDirectionalLightAndSHIndirectPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndCSMPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndCSMPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndCSMPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndCSMPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDirectionalLightAndCSMPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFNoLightMapPolicyHDRLinear64")),

			// Forward shading required
			FName(TEXT("TBasePassPSFCachedPointIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassPSFNoLightMapPolicySkylight")),

			// Runtime virtual texture
			FName(TEXT("TVirtualTextureVSBaseColor")),
			FName(TEXT("TVirtualTextureVSBaseColorNormal")),
			FName(TEXT("TVirtualTextureVSBaseColorNormalSpecular")),
			FName(TEXT("TVirtualTextureVSBaseColorNormalRoughness")),
			FName(TEXT("TVirtualTextureVSWorldHeight")),
			FName(TEXT("TVirtualTexturePSBaseColor")),
			FName(TEXT("TVirtualTexturePSBaseColorNormal")),
			FName(TEXT("TVirtualTexturePSBaseColorNormalSpecular")),
			FName(TEXT("TVirtualTexturePSBaseColorNormalRoughness")),
			FName(TEXT("TVirtualTexturePSWorldHeight")),
		};
		return AllowedShaderTypes;
	}

	static const TArray<FName>& GetExcludedShaderTypesInThumbnailRender()
	{
		// shader types known *not* to be used by the preview scene
		static const TArray<FName> ExcludedShaderTypes =
		{
			// This is not an exhaustive list
			FName(TEXT("FDebugViewModeVS")),

			// No lightmap on thumbnails
			FName(TEXT("TLightMapDensityVSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityVSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityPSTLightMapPolicyLQ")),
			FName(TEXT("TLightMapDensityVSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassPSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTDistanceFieldShadowsAndLightMapPolicyHQSkylight")),
			FName(TEXT("TBasePassVSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyHQSkylight")),
			FName(TEXT("TBasePassVSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyLQSkylight")),
			FName(TEXT("TBasePassVSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightVolumetricLightmapShadowsLightingPolicy")),

			// Debug materials : 
			FName(TEXT("FDebugViewModePS")),

			// Mobile
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightCSMWithLightmapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightWithLightmapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDistanceFieldShadowsAndLQLightMapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQ0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQ0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSTLightMapPolicyLQHDRLinear64")),

			FName(TEXT("TBasePassVSFCachedVolumeIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedVolumeIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedVolumeIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassPSFPrecomputedVolumetricLightmapLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFPrecomputedVolumetricLightmapLightingPolicy")),
			FName(TEXT("TBasePassPSFPrecomputedVolumetricLightmapLightingPolicy")),

			FName(TEXT("TBasePassPSFSimpleStationaryLightVolumetricLightmapShadowsLightingPolicy")),

			FName(TEXT("TBasePassVSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassVSFSelfShadowedCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedCachedPointIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSelfShadowedTranslucencyPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedTranslucencyPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedTranslucencyPolicySkylight")),

			FName(TEXT("TShadowDepthVSVertexShadowDepth_PerspectiveCorrect")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OutputDepth")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OnePassPointLight")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VirtualShadowMap")),

			FName(TEXT("TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VirtualShadowMapPositionOnly")),

			FName(TEXT("TShadowDepthPSPixelShadowDepth_VirtualShadowMap")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_PerspectiveCorrect")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_OnePassPointLight")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_NonPerspectiveCorrect")),

			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OnePassPointLight")),
			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OnePassPointLightPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VSLayer")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VSLayerPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VSLayerGS")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_VSLayerGSPositionOnly")),

			FName(TEXT("FOnePassPointShadowDepthGS")),

			FName(TEXT("TTranslucencyShadowDepthVS<TranslucencyShadowDepth_Standard>")),
			FName(TEXT("TTranslucencyShadowDepthPS<TranslucencyShadowDepth_Standard>")),
			FName(TEXT("TTranslucencyShadowDepthVS<TranslucencyShadowDepth_PerspectiveCorrect>")),
			FName(TEXT("TTranslucencyShadowDepthPS<TranslucencyShadowDepth_PerspectiveCorrect>")),

			FName(TEXT("TBasePassVSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassVSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassVSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassPSFSelfShadowedVolumetricLightmapPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedVolumetricLightmapPolicySkylight")),
			FName(TEXT("TBasePassVSFSelfShadowedVolumetricLightmapPolicy")),

			FName(TEXT("TBasePassPSFSimpleLightmapOnlyLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleLightmapOnlyLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleLightmapOnlyLightingPolicy")),

#if RHI_RAYTRACING
			// No ray tracing on thumbnails
			FName(TEXT("FRayTracingDynamicGeometryConverterCS")),
			FName(TEXT("FTrivialMaterialCHS")),
#endif // RHI_RAYTRACING

			FName(TEXT("FLumenCardVS")),
			FName(TEXT("FLumenCardPS<true>")),
			FName(TEXT("FLumenCardPS<false>")),
		};
		return ExcludedShaderTypes;
	}

	static const TArray<FName>& GetGPULightmassShaderTypes()
	{
		static const TArray<FName> ShaderTypes =
		{
			FName(TEXT("TLightmapMaterialCHS<true>")),
			FName(TEXT("TLightmapMaterialCHS<false>")),
			FName(TEXT("FVLMVoxelizationVS")),
			FName(TEXT("FVLMVoxelizationGS")),
			FName(TEXT("FVLMVoxelizationPS")),
			FName(TEXT("FLightmapGBufferVS")),
			FName(TEXT("FLightmapGBufferPS")),
		};
		return ShaderTypes;
	}

	static const TArray<FName>& GetGrassShaderTypes()
	{
		static const TArray<FName> ShaderTypes =
		{
			FName(TEXT("FLandscapeGrassWeightVS")),
			FName(TEXT("FLandscapeGrassWeightPS")),
			FName(TEXT("FLandscapePhysicalMaterialVS")),
			FName(TEXT("FLandscapePhysicalMaterialPS")),
		};
		return ShaderTypes;
	}

	static const TArray<FName>& GetRuntimeVirtualTextureShaderTypes()
	{
		static const TArray<FName> ShaderTypes =
		{
			FName(TEXT("TVirtualTextureVSBaseColor")),
			FName(TEXT("TVirtualTextureVSBaseColorNormal")),
			FName(TEXT("TVirtualTextureVSBaseColorNormalSpecular")),
			FName(TEXT("TVirtualTextureVSBaseColorNormalRoughness")),
			FName(TEXT("TVirtualTextureVSWorldHeight")),
			FName(TEXT("TVirtualTexturePSBaseColor")),
			FName(TEXT("TVirtualTexturePSBaseColorNormal")),
			FName(TEXT("TVirtualTexturePSBaseColorNormalSpecular")),
			FName(TEXT("TVirtualTexturePSBaseColorNormalRoughness")),
			FName(TEXT("TVirtualTexturePSWorldHeight")),
		};
		return ShaderTypes;
	}
};

FMaterialResource* ULandscapeMaterialInstanceConstant::AllocatePermutationResource()
{
	return new FLandscapeMaterialResource(this);
}

bool ULandscapeMaterialInstanceConstant::HasOverridenBaseProperties() const
{
	if (Parent)
	{
		// force a static permutation for ULandscapeMaterialInstanceConstants
		if (!Parent->IsA<ULandscapeMaterialInstanceConstant>())
		{
			return true;
		}
	}

	return Super::HasOverridenBaseProperties();
}

//////////////////////////////////////////////////////////////////////////

void ULandscapeComponent::GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const
{
	ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(GetOuter());
	FSphere BoundingSphere = Bounds.GetSphere();
	float LocalStreamingDistanceMultiplier = 1.f;
	float TexelFactor = 0.0f;
	if (Proxy)
	{
		LocalStreamingDistanceMultiplier = FMath::Max(0.0f, Proxy->StreamingDistanceMultiplier);
		TexelFactor = 0.75f * LocalStreamingDistanceMultiplier * ComponentSizeQuads * FMath::Abs(Proxy->GetRootComponent()->GetRelativeScale3D().X);
	}

	ERHIFeatureLevel::Type FeatureLevel = LevelContext.GetFeatureLevel();
	int32 MaterialInstanceCount = FeatureLevel >= ERHIFeatureLevel::SM5 ? GetMaterialInstanceCount() : MobileMaterialInterfaces.Num();

	for (int32 MaterialIndex = 0; MaterialIndex < MaterialInstanceCount; ++MaterialIndex)
	{
		const UMaterialInterface* MaterialInterface = FeatureLevel >= ERHIFeatureLevel::SM5 ? GetMaterialInstance(MaterialIndex) : ToRawPtr(MobileMaterialInterfaces[MaterialIndex]);

		// Normal usage...
		// Enumerate the textures used by the material.
		if (MaterialInterface)
		{
			TArray<UTexture*> Textures;
			MaterialInterface->GetUsedTextures(Textures, EMaterialQualityLevel::Num, false, FeatureLevel, false);

			const ULandscapeMaterialInstanceConstant* LandscapeMaterial = Cast<ULandscapeMaterialInstanceConstant>(MaterialInterface);

			// Add each texture to the output with the appropriate parameters.
			// TODO: Take into account which UVIndex is being used.
			for (int32 TextureIndex = 0; TextureIndex < Textures.Num(); TextureIndex++)
			{
				UTexture2D* Texture2D = Cast<UTexture2D>(Textures[TextureIndex]);
				if (!Texture2D) continue;

				FStreamingRenderAssetPrimitiveInfo& StreamingTexture = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
				StreamingTexture.Bounds = BoundingSphere;
				StreamingTexture.TexelFactor = TexelFactor;
				StreamingTexture.RenderAsset = Texture2D;

				if (LandscapeMaterial)
				{
					const float MaterialTexelFactor = LandscapeMaterial->GetLandscapeTexelFactor(Texture2D->GetFName());
					StreamingTexture.TexelFactor *= MaterialTexelFactor;
				}
			}

			// Lightmap
			const FMeshMapBuildData* MapBuildData = GetMeshMapBuildData();

			FLightMap2D* Lightmap = MapBuildData && MapBuildData->LightMap ? MapBuildData->LightMap->GetLightMap2D() : nullptr;
			uint32 LightmapIndex = AllowHighQualityLightmaps(FeatureLevel) ? 0 : 1;
			if (Lightmap && Lightmap->IsValid(LightmapIndex))
			{
				const FVector2D& Scale = Lightmap->GetCoordinateScale();
				if (Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER)
				{
					const float LightmapTexelFactor = TexelFactor / FMath::Min(Scale.X, Scale.Y);
					new (OutStreamingRenderAssets) FStreamingRenderAssetPrimitiveInfo(Lightmap->GetTexture(LightmapIndex), Bounds, LightmapTexelFactor);
					new (OutStreamingRenderAssets) FStreamingRenderAssetPrimitiveInfo(Lightmap->GetAOMaterialMaskTexture(), Bounds, LightmapTexelFactor);
					new (OutStreamingRenderAssets) FStreamingRenderAssetPrimitiveInfo(Lightmap->GetSkyOcclusionTexture(), Bounds, LightmapTexelFactor);
				}
			}

			// Shadowmap
			FShadowMap2D* Shadowmap = MapBuildData && MapBuildData->ShadowMap ? MapBuildData->ShadowMap->GetShadowMap2D() : nullptr;
			if (Shadowmap && Shadowmap->IsValid())
			{
				const FVector2D& Scale = Shadowmap->GetCoordinateScale();
				if (Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER)
				{
					const float ShadowmapTexelFactor = TexelFactor / FMath::Min(Scale.X, Scale.Y);
					new (OutStreamingRenderAssets) FStreamingRenderAssetPrimitiveInfo(Shadowmap->GetTexture(), Bounds, ShadowmapTexelFactor);
				}
			}
		}
	}

	// Weightmap
	for (int32 TextureIndex = 0; TextureIndex < WeightmapTextures.Num(); TextureIndex++)
	{
		FStreamingRenderAssetPrimitiveInfo& StreamingWeightmap = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
		StreamingWeightmap.Bounds = BoundingSphere;
		StreamingWeightmap.TexelFactor = TexelFactor;
		StreamingWeightmap.RenderAsset = WeightmapTextures[TextureIndex];
	}

	// Heightmap
	if (HeightmapTexture)
	{
		FStreamingRenderAssetPrimitiveInfo& StreamingHeightmap = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
		StreamingHeightmap.Bounds = BoundingSphere;

		float HeightmapTexelFactor = TexelFactor * (static_cast<float>(HeightmapTexture->GetSizeY()) / (ComponentSizeQuads + 1));
		StreamingHeightmap.TexelFactor = ForcedLOD >= 0 ? -(1 << (13 - ForcedLOD)) : HeightmapTexelFactor; // Minus Value indicate forced resolution (Mip 13 for 8k texture)
		StreamingHeightmap.RenderAsset = HeightmapTexture;
	}

	// XYOffset
	if (XYOffsetmapTexture)
	{
		FStreamingRenderAssetPrimitiveInfo& StreamingXYOffset = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
		StreamingXYOffset.Bounds = BoundingSphere;
		StreamingXYOffset.TexelFactor = TexelFactor;
		StreamingXYOffset.RenderAsset = XYOffsetmapTexture;
	}

#if WITH_EDITOR
	if (GIsEditor)
	{
		if (EditToolRenderData.DataTexture)
		{
			FStreamingRenderAssetPrimitiveInfo& StreamingDatamap = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
			StreamingDatamap.Bounds = BoundingSphere;
			StreamingDatamap.TexelFactor = TexelFactor;
			StreamingDatamap.RenderAsset = EditToolRenderData.DataTexture;
		}

		if (EditToolRenderData.LayerContributionTexture)
		{
			FStreamingRenderAssetPrimitiveInfo& StreamingDatamap = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
			StreamingDatamap.Bounds = BoundingSphere;
			StreamingDatamap.TexelFactor = TexelFactor;
			StreamingDatamap.RenderAsset = EditToolRenderData.LayerContributionTexture;
		}

		if (EditToolRenderData.DirtyTexture)
		{
			FStreamingRenderAssetPrimitiveInfo& StreamingDatamap = *new(OutStreamingRenderAssets)FStreamingRenderAssetPrimitiveInfo;
			StreamingDatamap.Bounds = BoundingSphere;
			StreamingDatamap.TexelFactor = TexelFactor;
			StreamingDatamap.RenderAsset = EditToolRenderData.DirtyTexture;
		}
	}
#endif

	if (LODStreamingProxy && LODStreamingProxy->IsStreamable())
	{
		const float MeshTexelFactor = ForcedLOD >= 0 ?
			-FMath::Max<int32>(LODStreamingProxy->GetStreamableResourceState().MaxNumLODs - ForcedLOD, 1) :
			(IsRegistered() ? Bounds.SphereRadius * 2.f : 0.f);
		new (OutStreamingRenderAssets) FStreamingRenderAssetPrimitiveInfo(LODStreamingProxy, Bounds, MeshTexelFactor, PackedRelativeBox_Identity, true);
	}
}

void ALandscapeProxy::ChangeComponentScreenSizeToUseSubSections(float InComponentScreenSizeToUseSubSections)
{
	ComponentScreenSizeToUseSubSections = FMath::Clamp<float>(InComponentScreenSizeToUseSubSections, 0.01f, 1.0f);

	if (LandscapeComponents.Num() > 0)
	{
		int32 ComponentCount = LandscapeComponents.Num();
		FLandscapeComponentSceneProxy** RenderProxies = new FLandscapeComponentSceneProxy * [ComponentCount];
		for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
		{
			RenderProxies[Idx] = (FLandscapeComponentSceneProxy*)(LandscapeComponents[Idx]->SceneProxy);
		}

		float ComponentScreenSizeToUseSubSectionsLocal = ComponentScreenSizeToUseSubSections;
		ENQUEUE_RENDER_COMMAND(LandscapeChangeComponentScreenSizeToUseSubSectionsCommand)(
			[RenderProxies, ComponentCount, ComponentScreenSizeToUseSubSectionsLocal](FRHICommandListImmediate& RHICmdList)
		{
			for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
			{
				if (RenderProxies[Idx] != nullptr)
				{
					RenderProxies[Idx]->ChangeComponentScreenSizeToUseSubSections_RenderThread(ComponentScreenSizeToUseSubSectionsLocal);
				}
			}

			delete[] RenderProxies;
		}
		);
	}
}

void ALandscapeProxy::ChangeLODDistanceFactor(float InLODDistanceFactor)
{
	// Deprecated
}

void FLandscapeComponentSceneProxy::ChangeComponentScreenSizeToUseSubSections_RenderThread(float InComponentScreenSizeToUseSubSections)
{
	ComponentSquaredScreenSizeToUseSubSections = FMath::Square(InComponentScreenSizeToUseSubSections);
}

bool FLandscapeComponentSceneProxy::HeightfieldHasPendingStreaming() const
{
	bool bHeightmapTextureStreaming = false;

	if (HeightmapTexture)
	{
		bHeightmapTextureStreaming |= HeightmapTexture->bHasStreamingUpdatePending;
#if WITH_EDITOR
		bHeightmapTextureStreaming |= HeightmapTexture->IsCompiling();
#endif
	}

	bool bVisibilityTextureStreaming = false;

	if (VisibilityWeightmapTexture)
	{
		bVisibilityTextureStreaming |= VisibilityWeightmapTexture->bHasStreamingUpdatePending;
#if WITH_EDITOR
		bVisibilityTextureStreaming |= VisibilityWeightmapTexture->IsCompiling();
#endif
	}

	return bHeightmapTextureStreaming || bVisibilityTextureStreaming;
}

void FLandscapeComponentSceneProxy::GetHeightfieldRepresentation(UTexture2D*& OutHeightmapTexture, UTexture2D*& OutDiffuseColorTexture, UTexture2D*& OutVisibilityTexture, FHeightfieldComponentDescription& OutDescription) const
{
	OutHeightmapTexture = HeightmapTexture;
	OutDiffuseColorTexture = BaseColorForGITexture;
	OutVisibilityTexture = VisibilityWeightmapTexture;

	OutDescription.HeightfieldScaleBias = HeightmapScaleBias;

	OutDescription.MinMaxUV = FVector4f(
		HeightmapScaleBias.Z,
		HeightmapScaleBias.W,
		HeightmapScaleBias.Z + SubsectionSizeVerts * NumSubsections * HeightmapScaleBias.X - HeightmapScaleBias.X,
		HeightmapScaleBias.W + SubsectionSizeVerts * NumSubsections * HeightmapScaleBias.Y - HeightmapScaleBias.Y);

	OutDescription.HeightfieldRect = FIntRect(SectionBase.X, SectionBase.Y, SectionBase.X + NumSubsections * SubsectionSizeQuads, SectionBase.Y + NumSubsections * SubsectionSizeQuads);

	OutDescription.NumSubsections = NumSubsections;

	OutDescription.SubsectionScaleAndBias = FVector4(SubsectionSizeQuads, SubsectionSizeQuads, HeightmapSubsectionOffsetU, HeightmapSubsectionOffsetV);

	OutDescription.VisibilityChannel = VisibilityWeightmapChannel;
}

void FLandscapeComponentSceneProxy::GetLCIs(FLCIArray& LCIs)
{
	FLightCacheInterface* LCI = ComponentLightInfo.Get();
	if (LCI)
	{
		LCIs.Push(LCI);
	}
}

float FLandscapeComponentSceneProxy::ComputeLODForView(const FSceneView& InView) const
{
	// TODO: this function generates A LOT OF cache misses - it should be much better if we have an event of FTexture2DResource::UpdateTexture

	int32 ViewLODOverride = GetViewLodOverride(InView);
	float ViewLODDistanceFactor = InView.LODDistanceFactor;
	bool ViewEngineShowFlagCollisionPawn = InView.Family->EngineShowFlags.CollisionPawn;
	bool ViewEngineShowFlagCollisionVisibility = InView.Family->EngineShowFlags.CollisionVisibility;
	const FVector& ViewOrigin = GetLODView(InView).ViewMatrices.GetViewOrigin();
	const FMatrix& ViewProjectionMatrix = GetLODView(InView).ViewMatrices.GetProjectionMatrix();

	float LODScale = ViewLODDistanceFactor * CVarStaticMeshLODDistanceScale.GetValueOnRenderThread();

	int32 ForcedLODLevel = LODSettings.ForcedLOD;
	ForcedLODLevel = ViewLODOverride >= 0 ? ViewLODOverride : ForcedLODLevel;
	const int32 DrawCollisionLODOverride = GetDrawCollisionLodOverride(ViewEngineShowFlagCollisionPawn, ViewEngineShowFlagCollisionVisibility, LODSettings.DrawCollisionPawnLOD, LODSettings.DrawCollisionVisibilityLOD);
	ForcedLODLevel = DrawCollisionLODOverride >= 0 ? DrawCollisionLODOverride : ForcedLODLevel;
	ForcedLODLevel = FMath::Min<int32>(ForcedLODLevel, LODSettings.LastLODIndex);

	float LODLevel = ForcedLODLevel;
	if (ForcedLODLevel < 0)
	{
		float MeshScreenSizeSquared = ComputeBoundsScreenRadiusSquared(GetBounds().Origin, GetBounds().SphereRadius, ViewOrigin, ViewProjectionMatrix);

		float FractionalLOD;
		FLandscapeRenderSystem::GetLODFromScreenSize(LODSettings, MeshScreenSizeSquared, LODScale * LODScale, FractionalLOD);
		LODLevel = FractionalLOD;
	}

#if PLATFORM_SUPPORTS_LANDSCAPE_VISUAL_MESH_LOD_STREAMING
	const float CurFirstLODIdx = GetCurrentFirstLODIdx_RenderThread();
#else
	constexpr float CurFirstLODIdx = 0.f;
#endif

	return FMath::Max(LODLevel, CurFirstLODIdx);
}

float FLandscapeComponentSceneProxy::ComputeLODBias() const
{
	float ComputedLODBias = 0;

	if (HeightmapTexture)
	{
		if (const FTexture2DResource* TextureResource = (const FTexture2DResource*)HeightmapTexture->GetResource())
		{
			ComputedLODBias = HeightmapTexture->GetNumMips() - HeightmapTexture->GetNumResidentMips();
		}
	}

	// TODO: support mipmap LOD bias of XY offset map
	//XYOffsetmapTexture ? ((FTexture2DResource*)XYOffsetmapTexture->Resource)->GetCurrentFirstMip() : 0.0f);

	return ComputedLODBias;
}

//
// FLandscapeSectionInfo
//
FLandscapeSectionInfo::FLandscapeSectionInfo(const UWorld* InWorld, const FGuid& InLandscapeGuid, const FIntPoint& InSectionBase)
	: LandscapeKey(HashCombine(GetTypeHash(InWorld), GetTypeHash(InLandscapeGuid)))
	, ComponentBase(InSectionBase)
	, bRegistered(false)
{
}

void FLandscapeSectionInfo::RegisterSection()
{
	FLandscapeRenderSystem::RegisterSection(this);
}

void FLandscapeSectionInfo::UnregisterSection()
{
	FLandscapeRenderSystem::UnregisterSection(this);
}

//
// FLandscapeProxySectionInfo
//
class FLandscapeProxySectionInfo : public FLandscapeSectionInfo
{
public:
	FLandscapeProxySectionInfo(const UWorld* InWorld, const FGuid& InLandscapeGuid, const FIntPoint& InSectionBase, int8 InProxyLOD)
		: FLandscapeSectionInfo(InWorld, InLandscapeGuid, InSectionBase)
		, ProxyLOD(InProxyLOD)
	{
	}

	virtual float ComputeLODForView(const FSceneView& InView) const override
	{
		return ProxyLOD;
	}

	virtual float ComputeLODBias() const override
	{
		return 0.0f;
	}

	virtual int32 GetSectionPriority() const override
	{
		return ProxyLOD;
	}

private:
	int8 ProxyLOD;
};

//
// FLandscapeMeshProxySceneProxy
//
FLandscapeMeshProxySceneProxy::FLandscapeMeshProxySceneProxy(UStaticMeshComponent* InComponent, const FGuid& InLandscapeGuid, const TArray<FIntPoint>& InProxySectionsBases, int8 InProxyLOD)
	: FStaticMeshSceneProxy(InComponent, false)
{
	VisibilityHelper.Init(InComponent, this);

	if (VisibilityHelper.RequiresVisibleLevelToRender())
	{
		bShouldNotifyOnWorldAddRemove = true;
	}

	ProxySectionsInfos.Empty(InProxySectionsBases.Num());
	for (FIntPoint SectionBase : InProxySectionsBases)
	{
		ProxySectionsInfos.Emplace(MakeUnique<FLandscapeProxySectionInfo>(InComponent->GetWorld(), InLandscapeGuid, SectionBase, InProxyLOD));
	}
}

void FLandscapeMeshProxySceneProxy::RegisterSections()
{
	for (auto& Info : ProxySectionsInfos)
	{
		Info->RegisterSection();
	}
}

void FLandscapeMeshProxySceneProxy::UnregisterSections()
{
	for (auto& Info : ProxySectionsInfos)
	{
		Info->UnregisterSection();
	}
}

SIZE_T FLandscapeMeshProxySceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}


void FLandscapeMeshProxySceneProxy::CreateRenderThreadResources()
{
	FStaticMeshSceneProxy::CreateRenderThreadResources();

	for (auto& Info : ProxySectionsInfos)
	{
		FLandscapeRenderSystem::CreateResources(Info.Get());
	}

	if (VisibilityHelper.ShouldBeVisible())
	{
		RegisterSections();
	}
}

bool FLandscapeMeshProxySceneProxy::OnLevelAddedToWorld_RenderThread()
{
	if (VisibilityHelper.OnAddedToWorld())
	{
		SetForceHidden(false);
		RegisterSections();
		return true;
	}

	return false;
}

void FLandscapeMeshProxySceneProxy::OnLevelRemovedFromWorld_RenderThread()
{
	if (VisibilityHelper.OnRemoveFromWorld())
	{
		SetForceHidden(true);
		UnregisterSections();
	}
}

void FLandscapeMeshProxySceneProxy::DestroyRenderThreadResources()
{
	FStaticMeshSceneProxy::DestroyRenderThreadResources();
	UnregisterSections();

	for (auto& Info : ProxySectionsInfos)
	{
		FLandscapeRenderSystem::DestroyResources(Info.Get());
	}
}

FPrimitiveSceneProxy* ULandscapeMeshProxyComponent::CreateSceneProxy()
{
	if (GetStaticMesh() == nullptr
		|| GetStaticMesh()->IsCompiling()
		|| GetStaticMesh()->GetRenderData() == nullptr
		|| GetStaticMesh()->GetRenderData()->LODResources.Num() == 0
		|| GetStaticMesh()->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() == 0)
	{
		return nullptr;
	}

	return new FLandscapeMeshProxySceneProxy(this, LandscapeGuid, ProxyComponentBases, ProxyLOD);
}
