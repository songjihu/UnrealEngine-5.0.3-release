// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PrimitiveSceneInfo.cpp: Primitive scene info implementation.
=============================================================================*/

#include "PrimitiveSceneInfo.h"
#include "PrimitiveSceneProxy.h"
#include "Components/PrimitiveComponent.h"
#include "SceneManagement.h"
#include "SceneCore.h"
#include "VelocityRendering.h"
#include "ScenePrivate.h"
#include "RendererModule.h"
#include "HAL/LowLevelMemTracker.h"
#include "RayTracing/RayTracingMaterialHitShaders.h"
#include "VT/RuntimeVirtualTextureSceneProxy.h"
#include "VT/VirtualTextureSystem.h"
#include "GPUScene.h"
#include "Async/ParallelFor.h"
#include "ProfilingDebugging/ExternalProfiler.h"
#include "Nanite/Nanite.h"
#include "Rendering/NaniteResources.h"
#include "Lumen/LumenSceneRendering.h"
#include "NaniteSceneProxy.h"
#include "RayTracingDefinitions.h"

extern int32 GGPUSceneInstanceClearList;
extern int32 GGPUSceneInstanceBVH;

#if RHI_RAYTRACING
static int32 GCachedRayTracingInstancesCacheLocalTransform = 0;
static FAutoConsoleVariableRef CVarCachedRayTracingInstancesUseInstanceData(
	TEXT("r.CachedRayTracingInstances.CacheLocalTransform"),
	GCachedRayTracingInstancesCacheLocalTransform,
	TEXT("Cache Local Transform instead of using InstanceData (increases memory usage)."),
	ECVF_ReadOnly);

static int32 GCachedRayTracingInstancesLazyUpdate = 1;
static FAutoConsoleVariableRef CVarCachedRayTracingInstancesLazyUpdate(
	TEXT("r.CachedRayTracingInstances.LazyUpdate"),
	GCachedRayTracingInstancesLazyUpdate,
	TEXT("Lazy update cached ray tracing instances world transforms. \n")
	TEXT("Reduces memory usage by only caching world transforms of primitives when necessary."),
	ECVF_ReadOnly);
#endif

static int32 GMeshDrawCommandsCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarDrawCommandsCacheMultithreaded(
	TEXT("r.MeshDrawCommands.CacheMultithreaded"),
	GMeshDrawCommandsCacheMultithreaded,
	TEXT("Enable multithreading of draw command caching for static meshes. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

static int32 GNaniteDrawCommandCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarNaniteDrawCommandCacheMultithreaded(
	TEXT("r.Nanite.MeshDrawCommands.CacheMultithreaded"),
	GNaniteDrawCommandCacheMultithreaded,
	TEXT("Enable multithreading of draw command caching for Nanite materials. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingPrimitiveCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarRayTracingPrimitiveCacheMultithreaded(
	TEXT("r.RayTracing.MeshDrawCommands.CacheMultithreaded"),
	GRayTracingPrimitiveCacheMultithreaded,
	TEXT("Enable multithreading of raytracing primitive mesh command caching. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

/** An implementation of FStaticPrimitiveDrawInterface that stores the drawn elements for the rendering thread to use. */
class FBatchingSPDI : public FStaticPrimitiveDrawInterface
{
public:

	// Constructor.
	FBatchingSPDI(FPrimitiveSceneInfo* InPrimitiveSceneInfo):
		PrimitiveSceneInfo(InPrimitiveSceneInfo)
	{}

	// FStaticPrimitiveDrawInterface.
	virtual void SetHitProxy(HHitProxy* HitProxy) final override
	{
		CurrentHitProxy = HitProxy;

		if(HitProxy)
		{
			// Only use static scene primitive hit proxies in the editor.
			if(GIsEditor)
			{
				// Keep a reference to the hit proxy from the FPrimitiveSceneInfo, to ensure it isn't deleted while the static mesh still
				// uses its id.
				PrimitiveSceneInfo->HitProxies.Add(HitProxy);
			}
		}
	}

	virtual void ReserveMemoryForMeshes(int32 MeshNum)
	{
		PrimitiveSceneInfo->StaticMeshRelevances.Reserve(PrimitiveSceneInfo->StaticMeshRelevances.Num() + MeshNum);
		PrimitiveSceneInfo->StaticMeshes.Reserve(PrimitiveSceneInfo->StaticMeshes.Num() + MeshNum);
	}

	virtual void DrawMesh(const FMeshBatch& Mesh, float ScreenSize) final override
	{
		if (Mesh.HasAnyDrawCalls())
		{
			checkSlow(IsInParallelRenderingThread());

			FPrimitiveSceneProxy* PrimitiveSceneProxy = PrimitiveSceneInfo->Proxy;
			const ERHIFeatureLevel::Type FeatureLevel = PrimitiveSceneInfo->Scene->GetFeatureLevel();

			if (!Mesh.Validate(PrimitiveSceneProxy, FeatureLevel))
			{
				return;
			}

			FStaticMeshBatch* StaticMesh = new(PrimitiveSceneInfo->StaticMeshes) FStaticMeshBatch(
				PrimitiveSceneInfo,
				Mesh,
				CurrentHitProxy ? CurrentHitProxy->Id : FHitProxyId()
			);

			StaticMesh->PreparePrimitiveUniformBuffer(PrimitiveSceneProxy, FeatureLevel);
			// Volumetric self shadow mesh commands need to be generated every frame, as they depend on single frame uniform buffers with self shadow data.
			const bool bSupportsCachingMeshDrawCommands = SupportsCachingMeshDrawCommands(*StaticMesh, FeatureLevel) && !PrimitiveSceneProxy->CastsVolumetricTranslucentShadow();

			const FMaterial& Material = Mesh.MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel);
			bool bUseSkyMaterial = Material.IsSky();
			bool bUseSingleLayerWaterMaterial = Material.GetShadingModels().HasShadingModel(MSM_SingleLayerWater);
			bool bUseAnisotropy = Material.GetShadingModels().HasAnyShadingModel({MSM_DefaultLit, MSM_ClearCoat}) && Material.MaterialUsesAnisotropy_RenderThread();
			bool bSupportsNaniteRendering = SupportsNaniteRendering(StaticMesh->VertexFactory, PrimitiveSceneProxy, Mesh.MaterialRenderProxy, FeatureLevel);
			bool bSupportsGPUScene = StaticMesh->VertexFactory->SupportsGPUScene(FeatureLevel);

			FStaticMeshBatchRelevance* StaticMeshRelevance = new(PrimitiveSceneInfo->StaticMeshRelevances) FStaticMeshBatchRelevance(
				*StaticMesh, 
				ScreenSize, 
				bSupportsCachingMeshDrawCommands,
				bUseSkyMaterial,
				bUseSingleLayerWaterMaterial,
				bUseAnisotropy,
				bSupportsNaniteRendering,
				bSupportsGPUScene,
				FeatureLevel
				);
		}
	}

private:
	FPrimitiveSceneInfo* PrimitiveSceneInfo;
	TRefCountPtr<HHitProxy> CurrentHitProxy;
};

FPrimitiveSceneInfo::FPrimitiveSceneInfoEvent FPrimitiveSceneInfo::OnGPUSceneInstancesAllocated;
FPrimitiveSceneInfo::FPrimitiveSceneInfoEvent FPrimitiveSceneInfo::OnGPUSceneInstancesFreed;

FPrimitiveFlagsCompact::FPrimitiveFlagsCompact(const FPrimitiveSceneProxy* Proxy)
	: bCastDynamicShadow(Proxy->CastsDynamicShadow())
	, bStaticLighting(Proxy->HasStaticLighting())
	, bCastStaticShadow(Proxy->CastsStaticShadow())
	, bIsNaniteMesh(Proxy->IsNaniteMesh())
	, bSupportsGPUScene(Proxy->SupportsGPUScene())
{}

FPrimitiveSceneInfoCompact::FPrimitiveSceneInfoCompact(FPrimitiveSceneInfo* InPrimitiveSceneInfo) :
	PrimitiveFlagsCompact(InPrimitiveSceneInfo->Proxy)
{
	PrimitiveSceneInfo = InPrimitiveSceneInfo;
	Proxy = PrimitiveSceneInfo->Proxy;
	Bounds = FCompactBoxSphereBounds(PrimitiveSceneInfo->Proxy->GetBounds());
	MinDrawDistance = PrimitiveSceneInfo->Proxy->GetMinDrawDistance();
	MaxDrawDistance = PrimitiveSceneInfo->Proxy->GetMaxDrawDistance();

	VisibilityId = PrimitiveSceneInfo->Proxy->GetVisibilityId();
}

FPrimitiveSceneInfo::FPrimitiveSceneInfo(UPrimitiveComponent* InComponent,FScene* InScene):
	Proxy(InComponent->SceneProxy),
	PrimitiveComponentId(InComponent->ComponentId),
	RegistrationSerialNumber(InComponent->RegistrationSerialNumber),
	OwnerLastRenderTime(FActorLastRenderTime::GetPtr(InComponent->GetOwner())),
	IndirectLightingCacheAllocation(NULL),
	CachedPlanarReflectionProxy(NULL),
	CachedReflectionCaptureProxy(NULL),
	bNeedsCachedReflectionCaptureUpdate(true),
	DefaultDynamicHitProxy(NULL),
	LightList(NULL),
	LastRenderTime(-FLT_MAX),
	Scene(InScene),
	NumMobileMovablePointLights(0),
	bShouldRenderInMainPass(InComponent->SceneProxy->ShouldRenderInMainPass()),
	bVisibleInRealTimeSkyCapture(InComponent->SceneProxy->IsVisibleInRealTimeSkyCaptures()),
#if RHI_RAYTRACING
	bDrawInGame(Proxy->IsDrawnInGame()),
	bIsVisibleInSceneCaptures(!InComponent->SceneProxy->IsHiddenInSceneCapture()),
	bIsRayTracingRelevant(InComponent->SceneProxy->IsRayTracingRelevant()),
	bIsRayTracingStaticRelevant(InComponent->SceneProxy->IsRayTracingStaticRelevant()),
	bIsVisibleInRayTracing(InComponent->SceneProxy->IsVisibleInRayTracing()),
	CoarseMeshStreamingHandle(INDEX_NONE),
#endif
	PackedIndex(INDEX_NONE),
	PersistentIndex(FPersistentPrimitiveIndex{ INDEX_NONE }),
	ComponentForDebuggingOnly(InComponent),
	bNeedsStaticMeshUpdateWithoutVisibilityCheck(false),
	bNeedsUniformBufferUpdate(false),
	bIndirectLightingCacheBufferDirty(false),
	bRegisteredVirtualTextureProducerCallback(false),
	bRegisteredWithVelocityData(false),
	LevelUpdateNotificationIndex(INDEX_NONE),
	InstanceSceneDataOffset(INDEX_NONE),
	NumInstanceSceneDataEntries(0),
	InstancePayloadDataOffset(INDEX_NONE),
	InstancePayloadDataStride(0),
	LightmapDataOffset(INDEX_NONE),
	NumLightmapDataEntries(0)
{
	check(ComponentForDebuggingOnly);
	check(PrimitiveComponentId.IsValid());
	check(Proxy);

	const UPrimitiveComponent* SearchParentComponent = InComponent->GetLightingAttachmentRoot();

	if (SearchParentComponent && SearchParentComponent != InComponent)
	{
		LightingAttachmentRoot = SearchParentComponent->ComponentId;
	}

	// Only create hit proxies in the Editor as that's where they are used.
	if (GIsEditor)
	{
		// Create a dynamic hit proxy for the primitive. 
		DefaultDynamicHitProxy = Proxy->CreateHitProxies(InComponent,HitProxies);
		if( DefaultDynamicHitProxy )
		{
			DefaultDynamicHitProxyId = DefaultDynamicHitProxy->Id;
		}
	}

	// set LOD parent info if exists
	UPrimitiveComponent* LODParent = InComponent->GetLODParentPrimitive();
	if (LODParent)
	{
		LODParentComponentId = LODParent->ComponentId;
	}

	FMemory::Memzero(CachedReflectionCaptureProxies);

#if RHI_RAYTRACING
	RayTracingGeometries = InComponent->SceneProxy->MoveRayTracingGeometries();
#endif
}

FPrimitiveSceneInfo::~FPrimitiveSceneInfo()
{
	check(!OctreeId.IsValidId());
	for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
	{
		check(StaticMeshCommandInfos.Num() == 0);
	}

#if RHI_RAYTRACING
	DEC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, CachedRayTracingInstanceLocalTransforms.Num() * sizeof(FMatrix));
	DEC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, CachedRayTracingInstanceWorldTransforms.Num() * sizeof(FMatrix));
#endif
}

#if RHI_RAYTRACING
FRHIRayTracingGeometry* FPrimitiveSceneInfo::GetStaticRayTracingGeometryInstance(int LodLevel) const
{
	if (RayTracingGeometries.Num() > LodLevel)
	{
		// TODO: Select different LOD, when build is still pending for this LOD?
		if (RayTracingGeometries[LodLevel]->HasPendingBuildRequest())
		{
			RayTracingGeometries[LodLevel]->BoostBuildPriority();
			return nullptr;
		}
		else if (RayTracingGeometries[LodLevel]->IsValid())
		{
			return RayTracingGeometries[LodLevel]->RayTracingGeometryRHI;
		}
		else
		{
			return nullptr;
		}
	}
	else
	{
		return nullptr;
	}
}
#endif

void FPrimitiveSceneInfo::CacheMeshDrawCommands(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	//@todo - only need material uniform buffers to be created since we are going to cache pointers to them
	// Any updates (after initial creation) don't need to be forced here
	FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheMeshDrawCommands, FColor::Emerald);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheMeshDrawCommands);

	QUICK_SCOPE_CYCLE_COUNTER(STAT_CacheMeshDrawCommands);
	FMemMark Mark(FMemStack::Get());

	static constexpr int BATCH_SIZE = 64;
	const int NumBatches = (SceneInfos.Num() + BATCH_SIZE - 1) / BATCH_SIZE;

	auto DoWorkLambda = [Scene, SceneInfos](FCachedPassMeshDrawListContext& DrawListContext, int32 Index)
	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheMeshDrawCommand, FColor::Green);

		FMemMark Mark(FMemStack::Get());

		struct FMeshInfoAndIndex
		{
			int32 InfoIndex;
			int32 MeshIndex;
		};

		TArray<FMeshInfoAndIndex, TMemStackAllocator<>> MeshBatches;
		MeshBatches.Reserve(3 * BATCH_SIZE);

		int LocalNum = FMath::Min((Index * BATCH_SIZE) + BATCH_SIZE, SceneInfos.Num());
		for (int LocalIndex = (Index * BATCH_SIZE); LocalIndex < LocalNum; LocalIndex++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[LocalIndex];
			check(SceneInfo->StaticMeshCommandInfos.Num() == 0);
			SceneInfo->StaticMeshCommandInfos.AddDefaulted(EMeshPass::Num * SceneInfo->StaticMeshes.Num());
			FPrimitiveSceneProxy* SceneProxy = SceneInfo->Proxy;

			// Volumetric self shadow mesh commands need to be generated every frame, as they depend on single frame uniform buffers with self shadow data.
			if (!SceneProxy->CastsVolumetricTranslucentShadow())
			{
				for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
				{
					FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshIndex];
					if (SupportsCachingMeshDrawCommands(Mesh))
					{
						MeshBatches.Add(FMeshInfoAndIndex{ LocalIndex, MeshIndex });
					}
				}
			}
		}

		for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
		{
			const EShadingPath ShadingPath = Scene->GetShadingPath();
			EMeshPass::Type PassType = (EMeshPass::Type)PassIndex;

			if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::CachedMeshCommands) != EMeshPassFlags::None)
			{
				FCachedPassMeshDrawListContext::FMeshPassScope MeshPassScope(DrawListContext, PassType);

				PassProcessorCreateFunction CreateFunction = FPassProcessorManager::GetCreateFunction(ShadingPath, PassType);
				FMeshPassProcessor* PassMeshProcessor = CreateFunction(Scene, nullptr, &DrawListContext);

				if (PassMeshProcessor != nullptr)
				{
					for (const FMeshInfoAndIndex& MeshAndInfo : MeshBatches)
					{
						FPrimitiveSceneInfo* SceneInfo = SceneInfos[MeshAndInfo.InfoIndex];
						FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshAndInfo.MeshIndex];

						FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshAndInfo.MeshIndex];

						check(!MeshRelevance.CommandInfosMask.Get(PassType));

						uint64 BatchElementMask = ~0ull;
						// NOTE: AddMeshBatch calls FCachedPassMeshDrawListContext::FinalizeCommand
						PassMeshProcessor->AddMeshBatch(Mesh, BatchElementMask, SceneInfo->Proxy);

						FCachedMeshDrawCommandInfo CommandInfo = DrawListContext.GetCommandInfoAndReset();
						if (CommandInfo.CommandIndex != -1 || CommandInfo.StateBucketId != -1)
						{
							static_assert(sizeof(MeshRelevance.CommandInfosMask) * 8 >= EMeshPass::Num, "CommandInfosMask is too small to contain all mesh passes.");
							MeshRelevance.CommandInfosMask.Set(PassType);
							MeshRelevance.CommandInfosBase++;

							int CommandInfoIndex = MeshAndInfo.MeshIndex * EMeshPass::Num + PassType;
							check(SceneInfo->StaticMeshCommandInfos[CommandInfoIndex].MeshPass == EMeshPass::Num);
							SceneInfo->StaticMeshCommandInfos[CommandInfoIndex] = CommandInfo;
						}
					}

					PassMeshProcessor->~FMeshPassProcessor();
				}
			}
		}

		for (int LocalIndex = (Index * BATCH_SIZE); LocalIndex < LocalNum; LocalIndex++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[LocalIndex];
			int PrefixSum = 0;
			for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
			{
				FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshIndex];
				if (MeshRelevance.CommandInfosBase > 0)
				{
					EMeshPass::Type PassType = EMeshPass::DepthPass;
					int NewPrefixSum = PrefixSum;
					for (;;)
					{
						PassType = MeshRelevance.CommandInfosMask.SkipEmpty(PassType);
						if (PassType == EMeshPass::Num)
						{
							break;
						}

						int CommandInfoIndex = MeshIndex * EMeshPass::Num + PassType;
						checkSlow(CommandInfoIndex >= NewPrefixSum);
						SceneInfo->StaticMeshCommandInfos[NewPrefixSum] = SceneInfo->StaticMeshCommandInfos[CommandInfoIndex];
						NewPrefixSum++;
						PassType = EMeshPass::Type(PassType + 1);
					}

#if DO_GUARD_SLOW
					int NumBits = MeshRelevance.CommandInfosMask.GetNum();
					check(PrefixSum + NumBits == NewPrefixSum);
					int LastPass = -1;
					for (int32 TestIndex = PrefixSum; TestIndex < NewPrefixSum; TestIndex++)
					{
						int MeshPass = SceneInfo->StaticMeshCommandInfos[TestIndex].MeshPass;
						check(MeshPass > LastPass);
						LastPass = MeshPass;
					}
#endif
					MeshRelevance.CommandInfosBase = PrefixSum;
					PrefixSum = NewPrefixSum;
				}
			}
			SceneInfo->StaticMeshCommandInfos.SetNum(PrefixSum, true);
		}
	};

	bool bAnyLooseParameterBuffers = false;
	if (GMeshDrawCommandsCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
	{
		TArray<FCachedPassMeshDrawListContextDeferred, TMemStackAllocator<>> DrawListContexts;
		DrawListContexts.Reserve(NumBatches);
		for(int32 ContextIndex = 0; ContextIndex < NumBatches; ++ContextIndex)
		{
			DrawListContexts.Emplace(*Scene);
		}

		ParallelForTemplate(
			NumBatches, 
			[&DrawListContexts, &DoWorkLambda](int32 Index)
			{
				FTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
				DoWorkLambda(DrawListContexts[Index], Index);
			},
			EParallelForFlags::PumpRenderingThread | EParallelForFlags::Unbalanced
		);

		if (NumBatches > 0)
		{
			SCOPED_NAMED_EVENT(DeferredFinalizeMeshDrawCommands, FColor::Emerald);

			for (int32 Index = 0; Index < NumBatches; ++Index)
			{
				FCachedPassMeshDrawListContextDeferred& DrawListContext = DrawListContexts[Index];
				const int32 Start = Index * BATCH_SIZE;
				const int32 End = FMath::Min((Index * BATCH_SIZE) + BATCH_SIZE, SceneInfos.Num());
				DrawListContext.DeferredFinalizeMeshDrawCommands(SceneInfos, Start, End);
				bAnyLooseParameterBuffers |= DrawListContext.HasAnyLooseParameterBuffers();
			}
		}
	}
	else
	{
		FCachedPassMeshDrawListContextImmediate DrawListContext(*Scene);
		for (int Idx = 0; Idx < NumBatches; Idx++)
		{
			DoWorkLambda(DrawListContext, Idx);
		}
		bAnyLooseParameterBuffers = DrawListContext.HasAnyLooseParameterBuffers();
	}

#if DO_GUARD_SLOW
	{
		static int32 LogCount = 0;
		if (bAnyLooseParameterBuffers && (LogCount++ % 1000) == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("One or more Cached Mesh Draw commands use loose parameters. This causes overhead and will break dynamic instancing, potentially reducing performance further. Use Uniform Buffers instead."));
		}
	}
#endif

	if (!FParallelMeshDrawCommandPass::IsOnDemandShaderCreationEnabled())
	{
		FGraphicsMinimalPipelineStateId::InitializePersistentIds();
	}
}

void FPrimitiveSceneInfo::RemoveCachedMeshDrawCommands()
{
	checkSlow(IsInRenderingThread());

	for (int32 CommandIndex = 0; CommandIndex < StaticMeshCommandInfos.Num(); ++CommandIndex)
	{
		const FCachedMeshDrawCommandInfo& CachedCommand = StaticMeshCommandInfos[CommandIndex];

		if (CachedCommand.StateBucketId != INDEX_NONE)
		{
			EMeshPass::Type PassIndex = CachedCommand.MeshPass;
			FGraphicsMinimalPipelineStateId CachedPipelineId;

			{
				auto& ElementKVP = Scene->CachedMeshDrawCommandStateBuckets[PassIndex].GetByElementId(CachedCommand.StateBucketId);
				CachedPipelineId = ElementKVP.Key.CachedPipelineId;

				FMeshDrawCommandCount& StateBucketCount = ElementKVP.Value;
				check(StateBucketCount.Num > 0);
				StateBucketCount.Num--;
				if (StateBucketCount.Num == 0)
				{
					Scene->CachedMeshDrawCommandStateBuckets[PassIndex].RemoveByElementId(CachedCommand.StateBucketId);
				}
			}

			FGraphicsMinimalPipelineStateId::RemovePersistentId(CachedPipelineId);
		}
		else if (CachedCommand.CommandIndex >= 0)
		{
			FCachedPassMeshDrawList& PassDrawList = Scene->CachedDrawLists[CachedCommand.MeshPass];
			FGraphicsMinimalPipelineStateId CachedPipelineId = PassDrawList.MeshDrawCommands[CachedCommand.CommandIndex].CachedPipelineId;

			PassDrawList.MeshDrawCommands.RemoveAt(CachedCommand.CommandIndex);
			FGraphicsMinimalPipelineStateId::RemovePersistentId(CachedPipelineId);

			// Track the lowest index that might be free for faster AddAtLowestFreeIndex
			PassDrawList.LowestFreeIndexSearchStart = FMath::Min(PassDrawList.LowestFreeIndexSearchStart, CachedCommand.CommandIndex);
		}

	}

	for (int32 MeshIndex = 0; MeshIndex < StaticMeshRelevances.Num(); ++MeshIndex)
	{
		FStaticMeshBatchRelevance& MeshRelevance = StaticMeshRelevances[MeshIndex];

		MeshRelevance.CommandInfosMask.Reset();
	}

	StaticMeshCommandInfos.Empty();
}

static void BuildNaniteDrawCommands(FRHICommandListImmediate& RHICmdList, FScene* Scene, FPrimitiveSceneInfo* PrimitiveSceneInfo, FNaniteDrawListContext& DrawListContext);

void FPrimitiveSceneInfo::CacheNaniteDrawCommands(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheNaniteDrawCommands, FColor::Emerald);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheNaniteDrawCommands);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_CacheNaniteDrawCommands);

	FMemMark Mark(FMemStack::Get());
	FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

	const bool bNaniteEnabled = DoesPlatformSupportNanite(GMaxRHIShaderPlatform);
	if (bNaniteEnabled)
	{
		if (GNaniteDrawCommandCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
		{
			TArray<FNaniteDrawListContextDeferred> DrawListContexts;
			ParallelForWithTaskContext(
				DrawListContexts,
				SceneInfos.Num(),
				[&RHICmdList, Scene, &SceneInfos](FNaniteDrawListContextDeferred& Context, int32 Index)
				{
					FMemMark Mark(FMemStack::Get());
					FTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
					BuildNaniteDrawCommands(RHICmdList, Scene, SceneInfos[Index], Context);
				}
			);

			if (DrawListContexts.Num() > 0)
			{
				SCOPED_NAMED_EVENT(RegisterDeferredCommands, FColor::Emerald);

				for(FNaniteDrawListContextDeferred& Context : DrawListContexts)
				{
					Context.RegisterDeferredCommands(*Scene);
				}
			}
		}
		else
		{
			FNaniteDrawListContextImmediate DrawListContext(*Scene);
			for (FPrimitiveSceneInfo* PrimitiveSceneInfo : SceneInfos)
			{
				BuildNaniteDrawCommands(RHICmdList, Scene, PrimitiveSceneInfo, DrawListContext);
			}
		}		
	}
}

void BuildNaniteDrawCommands(FRHICommandListImmediate& RHICmdList, FScene* Scene, FPrimitiveSceneInfo* PrimitiveSceneInfo, FNaniteDrawListContext& DrawListContext)
{
	FPrimitiveSceneProxy* Proxy = PrimitiveSceneInfo->Proxy;
	if (Proxy->IsNaniteMesh())
	{
		FNaniteDrawListContext::FPrimitiveSceneInfoScope PrimInfoScope(DrawListContext, *PrimitiveSceneInfo);
	
		auto PassBody = [PrimitiveSceneInfo, Proxy, &DrawListContext](ENaniteMeshPass::Type MeshPass, FMeshPassProcessor* const NaniteMeshProcessor)
		{
			FNaniteDrawListContext::FMeshPassScope MeshPassScope(DrawListContext, MeshPass);

			int32 StaticMeshesCount = PrimitiveSceneInfo->StaticMeshes.Num();
			for (int32 MeshIndex = 0; MeshIndex < StaticMeshesCount; ++MeshIndex)
			{
				FStaticMeshBatchRelevance& MeshRelevance = PrimitiveSceneInfo->StaticMeshRelevances[MeshIndex];
				FStaticMeshBatch& Mesh = PrimitiveSceneInfo->StaticMeshes[MeshIndex];

				if (MeshRelevance.bSupportsNaniteRendering && Mesh.bUseForMaterial)
				{
					uint64 BatchElementMask = ~0ull;
					NaniteMeshProcessor->AddMeshBatch(Mesh, BatchElementMask, Proxy);
				}
			}
		};

		// ENaniteMeshPass::BasePass
		{
			FMeshPassProcessor* NaniteMeshProcessor = CreateNaniteMeshProcessor(Scene, nullptr, &DrawListContext);
			PassBody(ENaniteMeshPass::BasePass, NaniteMeshProcessor);
			NaniteMeshProcessor->~FMeshPassProcessor();
		}

		// ENaniteMeshPass::LumenCardCapture
		if (Lumen::HasPrimitiveNaniteMeshBatches(Proxy) && DoesPlatformSupportLumenGI(GetFeatureLevelShaderPlatform(Scene->GetFeatureLevel())))
		{
			FMeshPassProcessor* NaniteMeshProcessor = CreateLumenCardNaniteMeshProcessor(Scene, nullptr, &DrawListContext);
			PassBody(ENaniteMeshPass::LumenCardCapture, NaniteMeshProcessor);
			NaniteMeshProcessor->~FMeshPassProcessor();
		}

		static_assert(ENaniteMeshPass::Num == 2, "Change BuildNaniteDrawCommands() to account for more Nanite mesh passes");
	}
}

void FPrimitiveSceneInfo::RemoveCachedNaniteDrawCommands()
{
	checkSlow(IsInRenderingThread());

	if (!Proxy->IsNaniteMesh())
	{
		return;
	}

	QUICK_SCOPE_CYCLE_COUNTER(STAT_RemoveCachedNaniteDrawCommands);

	for (int32 NaniteMeshPassIndex = 0; NaniteMeshPassIndex < ENaniteMeshPass::Num; ++NaniteMeshPassIndex)
	{
		FNaniteMaterialCommands& NaniteMaterials = Scene->NaniteMaterials[NaniteMeshPassIndex];
		TArray<FNaniteCommandInfo>& NanitePassCommandInfo = NaniteCommandInfos[NaniteMeshPassIndex];

		for (int32 CommandIndex = 0; CommandIndex < NanitePassCommandInfo.Num(); ++CommandIndex)
		{
			const FNaniteCommandInfo& CommandInfo = NanitePassCommandInfo[CommandIndex];
			NaniteMaterials.Unregister(CommandInfo);
		}

		NanitePassCommandInfo.Reset();
		NaniteMaterialSlots[NaniteMeshPassIndex].Reset();
	}

#if WITH_EDITOR
	NaniteHitProxyIds.Reset();
#endif
}

#if RHI_RAYTRACING
void FScene::RefreshRayTracingMeshCommandCache()
{
	// Get rid of all existing cached commands
	CachedRayTracingMeshCommands.Empty(CachedRayTracingMeshCommands.Num());

	// Re-cache all current primitives
	FPrimitiveSceneInfo::CacheRayTracingPrimitives(this, Primitives);
}

void FScene::RefreshRayTracingInstances()
{
	// Re-cache all current primitives
	FPrimitiveSceneInfo::UpdateCachedRayTracingInstances(this, Primitives);
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (IsRayTracingEnabled() && !(Scene->World->WorldType == EWorldType::EditorPreview || Scene->World->WorldType == EWorldType::GamePreview))
	{
		checkf(GRHISupportsMultithreadedShaderCreation, TEXT("Raytracing code needs the ability to create shaders from task threads."));

		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// Write group id
			const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
			if (RayTracingGroupId != -1)
			{
				Scene->PrimitiveRayTracingGroupIds[SceneInfo->GetIndex()] = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
			}

			FRayTracingInstance CachedRayTracingInstance;
			ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[SceneInfo->GetIndex()];

			// Cache the coarse mesh streaming handle
			SceneInfo->CoarseMeshStreamingHandle = SceneInfo->Proxy->GetCoarseMeshStreamingHandle();

			// Write flags
			Flags = SceneInfo->Proxy->GetCachedRayTracingInstance(CachedRayTracingInstance);
			UpdateCachedRayTracingInstance(SceneInfo, CachedRayTracingInstance, Flags);
		}
	}
}

struct DeferredMeshLODCommandIndex
{
	FPrimitiveSceneInfo* SceneInfo;
	int8 MeshLODIndex;
	int32 CommandIndex;
};

template<class T>
class FCacheRayTracingPrimitivesContext
{
public:
	FCacheRayTracingPrimitivesContext(FScene* Scene)
		: CommandContext(Commands)
		, PassDrawRenderState(Scene->UniformBuffers.ViewUniformBuffer)
		, RayTracingMeshProcessor(&CommandContext, Scene, nullptr, PassDrawRenderState, Scene->CachedRayTracingMeshCommandsMode)
	{ }

	FTempRayTracingMeshCommandStorage Commands;
	FCachedRayTracingMeshCommandContext<T> CommandContext;
	FMeshPassProcessorRenderState PassDrawRenderState;
	FRayTracingMeshProcessor RayTracingMeshProcessor;
	TArray<DeferredMeshLODCommandIndex> DeferredMeshLODCommandIndices;
};

template<bool bDeferLODCommandIndices, class T>
void CacheRayTracingPrimitive(
	FScene* Scene, 
	FPrimitiveSceneInfo* SceneInfo,
	T& Commands,
	FCachedRayTracingMeshCommandContext<T>& CommandContext,
	FRayTracingMeshProcessor& RayTracingMeshProcessor,
	TArray<DeferredMeshLODCommandIndex>* DeferredMeshLODCommandIndices,
	FRayTracingInstance& CachedRayTracingInstance, 
	ERayTracingPrimitiveFlags& Flags)
{
	if (SceneInfo->GetRayTracingGeometryNum() > 0 && SceneInfo->StaticMeshes.Num() > 0)
	{
		int32 MaxLOD = -1;
		for (const FStaticMeshBatch& Mesh : SceneInfo->StaticMeshes)
		{
			MaxLOD = MaxLOD < Mesh.LODIndex ? Mesh.LODIndex : MaxLOD;
		}

		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.Empty(MaxLOD + 1);
		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.AddDefaulted(MaxLOD + 1);

		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.Empty(MaxLOD + 1);
		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.AddZeroed(MaxLOD + 1);

		for (const FStaticMeshBatch& Mesh : SceneInfo->StaticMeshes)
		{
			// Why do we pass a full mask here when the dynamic case only uses a mask of 1?
			// Also note that the code below assumes only a single command was generated per batch.
			const uint64 BatchElementMask = ~0ull;
			RayTracingMeshProcessor.AddMeshBatch(Mesh, BatchElementMask, SceneInfo->Proxy);

			if (CommandContext.CommandIndex >= 0)
			{
				uint64& Hash = SceneInfo->CachedRayTracingMeshCommandsHashPerLOD[Mesh.LODIndex];
				Hash <<= 1;
				Hash ^= Commands[CommandContext.CommandIndex].ShaderBindings.GetDynamicInstancingHash();

				if (bDeferLODCommandIndices)
				{
					DeferredMeshLODCommandIndices->Add({ SceneInfo,Mesh.LODIndex, CommandContext.CommandIndex });
				}
				else
				{
					SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[Mesh.LODIndex].Add(CommandContext.CommandIndex);
				}

				CommandContext.CommandIndex = -1;
			}
		}
	}

	// This path is mutually exclusive with the old path (used by normal static meshes) and is only used by Nanite proxies now.
	// TODO: move normal static meshes to this path, but needs testing to not break FN

	// Write group id
	const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
	if (RayTracingGroupId != -1)
	{
		Scene->PrimitiveRayTracingGroupIds[SceneInfo->GetIndex()] = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
	}

	// Write flags
	Flags = SceneInfo->Proxy->GetCachedRayTracingInstance(CachedRayTracingInstance);

	// Cache the coarse mesh streaming handle
	SceneInfo->CoarseMeshStreamingHandle = SceneInfo->Proxy->GetCoarseMeshStreamingHandle();

	if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::CacheMeshCommands))
	{
		// TODO: LOD w/ screen size support. Probably needs another array parallel to OutRayTracingInstances
		// We assume it is exactly 1 LOD now (true for Nanite proxies)
		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.Empty(1);
		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.AddDefaulted(1);

		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.Empty(1);
		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.AddZeroed(1);

		for (const FMeshBatch& Mesh : CachedRayTracingInstance.Materials)
		{
			// Why do we pass a full mask here when the dynamic case only uses a mask of 1?
			// Also note that the code below assumes only a single command was generated per batch.
			const uint64 BatchElementMask = ~0ull;
			RayTracingMeshProcessor.AddMeshBatch(Mesh, BatchElementMask, SceneInfo->Proxy);

			// The material section must emit a command. Otherwise, it should have been excluded earlier
			check(CommandContext.CommandIndex >= 0);
			
			uint64& Hash = SceneInfo->CachedRayTracingMeshCommandsHashPerLOD[Mesh.LODIndex];
			Hash <<= 1;
			Hash ^= Commands[CommandContext.CommandIndex].ShaderBindings.GetDynamicInstancingHash();

			if (bDeferLODCommandIndices)
			{
				DeferredMeshLODCommandIndices->Add({ SceneInfo,Mesh.LODIndex, CommandContext.CommandIndex });
			}
			else
			{
				SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[Mesh.LODIndex].Add(CommandContext.CommandIndex);
			}

			CommandContext.CommandIndex = -1;
		}
	}
}

void FPrimitiveSceneInfo::CacheRayTracingPrimitives(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (IsRayTracingEnabled() && !(Scene->World->WorldType == EWorldType::EditorPreview || Scene->World->WorldType == EWorldType::GamePreview))
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheRayTracingPrimitives)
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheRayTracingPrimitives, FColor::Emerald);

		checkf(GRHISupportsMultithreadedShaderCreation, TEXT("Raytracing code needs the ability to create shaders from task threads."));
		
		FCachedRayTracingMeshCommandStorage& CachedRayTracingMeshCommands = Scene->CachedRayTracingMeshCommands;

		if (GRayTracingPrimitiveCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
		{
			TArray<FCacheRayTracingPrimitivesContext<FTempRayTracingMeshCommandStorage>> Contexts;
			ParallelForWithTaskContext(
				Contexts,
				SceneInfos.Num(),
				[Scene](int32 ContextIndex, int32 NumContexts) { return Scene; },
				[Scene, &SceneInfos](FCacheRayTracingPrimitivesContext<FTempRayTracingMeshCommandStorage>& Context, int32 Index)
				{
					FMemMark Mark(FMemStack::Get());
					FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);

					FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
					FRayTracingInstance CachedInstance;
					ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[SceneInfo->GetIndex()];
					CacheRayTracingPrimitive<true>(Scene, SceneInfo, Context.Commands, Context.CommandContext, Context.RayTracingMeshProcessor, &Context.DeferredMeshLODCommandIndices, CachedInstance, Flags);
					UpdateCachedRayTracingInstance(SceneInfo, CachedInstance, Flags);
				}
			);

			if (Contexts.Num() > 0)
			{
				CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheRayTracingPrimitives_Merge)
				SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheRayTracingPrimitives_Merge, FColor::Emerald);

				// copy commands generated by multiple threads to the sparse array in FScene
				// and set each mesh LOD command index
				for (const auto& Context : Contexts)
				{
					for (const DeferredMeshLODCommandIndex& Entry : Context.DeferredMeshLODCommandIndices)
					{
						int32 CommandIndex = CachedRayTracingMeshCommands.Add(Context.Commands[Entry.CommandIndex]);
						Entry.SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[Entry.MeshLODIndex].Add(CommandIndex);
					}
				}
			}
		}
		else
		{
			FCachedRayTracingMeshCommandContext CommandContext(CachedRayTracingMeshCommands);
			FMeshPassProcessorRenderState PassDrawRenderState(Scene->UniformBuffers.ViewUniformBuffer);
			FRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, Scene, nullptr, PassDrawRenderState, Scene->CachedRayTracingMeshCommandsMode);

			for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
			{
				FRayTracingInstance CachedRayTracingInstance;
				ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[SceneInfo->GetIndex()];
				CacheRayTracingPrimitive<false>(Scene, SceneInfo, CachedRayTracingMeshCommands, CommandContext, RayTracingMeshProcessor, nullptr, CachedRayTracingInstance, Flags);
				UpdateCachedRayTracingInstance(SceneInfo, CachedRayTracingInstance, Flags);
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstanceWorldTransforms()
{
	if (bUpdateCachedRayTracingInstanceWorldTransforms && GCachedRayTracingInstancesLazyUpdate)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UpdateCachedRayTracingInstanceWorldTransforms);

		bUpdateCachedRayTracingInstanceWorldTransforms = false;

		DEC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, CachedRayTracingInstanceWorldTransforms.Num() * sizeof(FMatrix));

		CachedRayTracingInstanceWorldTransforms.SetNumUninitialized(CachedRayTracingInstance.NumTransforms);

		INC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, CachedRayTracingInstanceWorldTransforms.Num() * sizeof(FMatrix));

		// Apply local offset to far-field object
		FMatrix LocalToWorld = Proxy->GetLocalToWorld();
		if (Proxy->IsRayTracingFarField())
		{
			LocalToWorld = LocalToWorld.ConcatTranslation(Lumen::GetFarFieldReferencePos());
		}

		TConstArrayView<FPrimitiveInstance> InstanceSceneData = Proxy->GetInstanceSceneData();

		for (int32 Index = 0; Index < CachedRayTracingInstanceWorldTransforms.Num(); Index++)
		{
			FMatrix LocalTransform = GCachedRayTracingInstancesCacheLocalTransform ? CachedRayTracingInstanceLocalTransforms[Index] : InstanceSceneData[Index].LocalToPrimitive.ToMatrix();

			CachedRayTracingInstanceWorldTransforms[Index] = LocalTransform * LocalToWorld;
		}

		CachedRayTracingInstance.Transforms = MakeArrayView(CachedRayTracingInstanceWorldTransforms);
		check(CachedRayTracingInstance.NumTransforms >= uint32(CachedRayTracingInstance.Transforms.Num()));
	}
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstance(FPrimitiveSceneInfo* SceneInfo, const FRayTracingInstance& CachedRayTracingInstance, const ERayTracingPrimitiveFlags Flags)
{
	if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::CacheInstances))
	{
		if (GCachedRayTracingInstancesCacheLocalTransform)
		{
			// Cache a copy of local transforms so that they can be updated in the future
			// TODO: this is actually not needed for static meshes with non-movable mobility (except in editor)
			DEC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, SceneInfo->CachedRayTracingInstanceLocalTransforms.Num() * sizeof(FMatrix));
			SceneInfo->CachedRayTracingInstanceLocalTransforms = CachedRayTracingInstance.InstanceTransforms;
			INC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, SceneInfo->CachedRayTracingInstanceLocalTransforms.Num() * sizeof(FMatrix));
		}
		// TODO: allocate from FRayTracingScene & do better low-level caching
		SceneInfo->CachedRayTracingInstance.NumTransforms = CachedRayTracingInstance.NumTransforms;
		if (!GCachedRayTracingInstancesLazyUpdate)
		{
			DEC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, SceneInfo->CachedRayTracingInstanceWorldTransforms.Num() * sizeof(FMatrix));
			SceneInfo->CachedRayTracingInstanceWorldTransforms.Empty();
			SceneInfo->CachedRayTracingInstanceWorldTransforms.AddUninitialized(CachedRayTracingInstance.NumTransforms);
			INC_MEMORY_STAT_BY(STAT_CachedRayTracingInstancesMemory, SceneInfo->CachedRayTracingInstanceWorldTransforms.Num() * sizeof(FMatrix));
		}

		// Apply local offset to far-field object
		FMatrix LocalToWorld = SceneInfo->Proxy->GetLocalToWorld();
		if (SceneInfo->Proxy->IsRayTracingFarField())
		{
			LocalToWorld = LocalToWorld.ConcatTranslation(Lumen::GetFarFieldReferencePos());
		}

		SceneInfo->CachedRayTracingInstanceWorldBounds.Empty();
		SceneInfo->CachedRayTracingInstanceWorldBounds.AddUninitialized(CachedRayTracingInstance.NumTransforms);

		SceneInfo->UpdateCachedRayTracingInstanceTransforms(LocalToWorld);

		if (!GCachedRayTracingInstancesLazyUpdate)
		{
			SceneInfo->CachedRayTracingInstance.Transforms = MakeArrayView(SceneInfo->CachedRayTracingInstanceWorldTransforms);
			check(SceneInfo->CachedRayTracingInstance.NumTransforms >= uint32(SceneInfo->CachedRayTracingInstance.Transforms.Num()));
		}

		SceneInfo->CachedRayTracingInstance.GeometryRHI = CachedRayTracingInstance.Geometry->RayTracingGeometryRHI;

		// At this point (in AddToScene()) PrimitiveIndex has been set
		check(SceneInfo->GetIndex() != INDEX_NONE);
		SceneInfo->CachedRayTracingInstance.DefaultUserData = (uint32)SceneInfo->GetIndex();
		SceneInfo->CachedRayTracingInstance.Mask = CachedRayTracingInstance.Mask; // When no cached command is found, InstanceMask == 0 and the instance is effectively filtered out

		if (CachedRayTracingInstance.bForceOpaque)
		{
			SceneInfo->CachedRayTracingInstance.Flags |= ERayTracingInstanceFlags::ForceOpaque;
		}

		if (CachedRayTracingInstance.bDoubleSided)
		{
			SceneInfo->CachedRayTracingInstance.Flags |= ERayTracingInstanceFlags::TriangleCullDisable;
		}
	}
}

void FPrimitiveSceneInfo::RemoveCachedRayTracingPrimitives()
{
	if (IsRayTracingEnabled())
	{
		for (auto& CachedRayTracingMeshCommandIndices : CachedRayTracingMeshCommandIndicesPerLOD)
		{
			for (auto CommandIndex : CachedRayTracingMeshCommandIndices)
			{
				if (CommandIndex >= 0)
				{
					Scene->CachedRayTracingMeshCommands.RemoveAt(CommandIndex);
				}
			}
		}

		CachedRayTracingMeshCommandIndicesPerLOD.Empty();

		CachedRayTracingMeshCommandsHashPerLOD.Empty();
	}
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstanceTransforms(const FMatrix& NewPrimitiveLocalToWorld)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UpdateCachedRayTracingInstanceTransforms);
	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCachedRayTracingInstanceTransforms);

	TConstArrayView<FPrimitiveInstance> InstanceSceneData = Proxy->GetInstanceSceneData();
	
	SmallestRayTracingInstanceWorldBoundsIndex = 0;

	for (int32 Index = 0; Index < CachedRayTracingInstanceWorldBounds.Num(); Index++)
	{
		const FRenderBounds& LocalBoundingBox = Proxy->GetInstanceLocalBounds(Index);

		FMatrix LocalTransform = GCachedRayTracingInstancesCacheLocalTransform ? CachedRayTracingInstanceLocalTransforms[Index] : InstanceSceneData[Index].LocalToPrimitive.ToMatrix();

		CachedRayTracingInstanceWorldBounds[Index] = LocalBoundingBox.TransformBy(LocalTransform * NewPrimitiveLocalToWorld).ToBoxSphereBounds();
		if(!GCachedRayTracingInstancesLazyUpdate)
		{
			CachedRayTracingInstanceWorldTransforms[Index] = LocalTransform * NewPrimitiveLocalToWorld;
		}
		SmallestRayTracingInstanceWorldBoundsIndex = CachedRayTracingInstanceWorldBounds[Index].SphereRadius < CachedRayTracingInstanceWorldBounds[SmallestRayTracingInstanceWorldBoundsIndex].SphereRadius ? Index : SmallestRayTracingInstanceWorldBoundsIndex;
	}

	bUpdateCachedRayTracingInstanceWorldTransforms = true;
}
#endif

void FPrimitiveSceneInfo::AddStaticMeshes(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos, bool bAddToStaticDrawLists)
{
	LLM_SCOPE(ELLMTag::StaticMesh);

	{
		ParallelForTemplate(SceneInfos.Num(), [Scene, &SceneInfos](int32 Index)
		{
			FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
			SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddStaticMeshes_DrawStaticElements, FColor::Magenta);
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
			// Cache the primitive's static mesh elements.
			FBatchingSPDI BatchingSPDI(SceneInfo);
			BatchingSPDI.SetHitProxy(SceneInfo->DefaultDynamicHitProxy);
			SceneInfo->Proxy->DrawStaticElements(&BatchingSPDI);
			SceneInfo->StaticMeshes.Shrink();
			SceneInfo->StaticMeshRelevances.Shrink();

			check(SceneInfo->StaticMeshRelevances.Num() == SceneInfo->StaticMeshes.Num());
		});
	}

	{
		const ERHIFeatureLevel::Type FeatureLevel = Scene->GetFeatureLevel();

		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddStaticMeshes_UpdateSceneArrays, FColor::Blue);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// Allocate OIT index buffer where needed
			const bool bAllocateSortedTriangles = OIT::IsEnabled(GMaxRHIShaderPlatform) && SceneInfo->Proxy->SupportsSortedTriangles();

			for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
			{
				FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshIndex];
				FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshIndex];

				// Add the static mesh to the scene's static mesh list.
				FSparseArrayAllocationInfo SceneArrayAllocation = Scene->StaticMeshes.AddUninitialized();
				Scene->StaticMeshes[SceneArrayAllocation.Index] = &Mesh;
				Mesh.Id = SceneArrayAllocation.Index;
				MeshRelevance.Id = SceneArrayAllocation.Index;

				if (bAllocateSortedTriangles && OIT::IsCompatible(Mesh, FeatureLevel))
				{
					FSortedTriangleData Allocation = Scene->OITSceneData.Allocate(Mesh.Elements[0].IndexBuffer, EPrimitiveType(Mesh.Type), Mesh.Elements[0].FirstIndex, Mesh.Elements[0].NumPrimitives);
					OIT::ConvertSortedIndexToDynamicIndex(&Allocation, &Mesh.Elements[0].DynamicIndexBuffer);
				}
			}
		}
	}

	if (bAddToStaticDrawLists)
	{
		CacheMeshDrawCommands(RHICmdList, Scene, SceneInfos);
		CacheNaniteDrawCommands(RHICmdList, Scene, SceneInfos);
	#if RHI_RAYTRACING
		CacheRayTracingPrimitives(Scene, SceneInfos);
	#endif
	}
}

static void OnVirtualTextureDestroyed(const FVirtualTextureProducerHandle& InHandle, void* Baton)
{
	FPrimitiveSceneInfo* PrimitiveSceneInfo = static_cast<FPrimitiveSceneInfo*>(Baton);

	// Update the main uniform buffer
	PrimitiveSceneInfo->UpdateStaticLightingBuffer();

	// Also need to update lightmap data inside GPUScene, if that's enabled
	PrimitiveSceneInfo->Scene->GPUScene.AddPrimitiveToUpdate(PrimitiveSceneInfo->GetIndex(), EPrimitiveDirtyState::ChangedStaticLighting);
}

static void GetRuntimeVirtualTextureLODRange(TArray<class FStaticMeshBatchRelevance> const& MeshRelevances, int8& OutMinLOD, int8& OutMaxLOD)
{
	OutMinLOD = MAX_int8;
	OutMaxLOD = 0;

	for (int32 MeshIndex = 0; MeshIndex < MeshRelevances.Num(); ++MeshIndex)
	{
		const FStaticMeshBatchRelevance& MeshRelevance = MeshRelevances[MeshIndex];
		if (MeshRelevance.bRenderToVirtualTexture)
		{
			OutMinLOD = FMath::Min(OutMinLOD, MeshRelevance.LODIndex);
			OutMaxLOD = FMath::Max(OutMaxLOD, MeshRelevance.LODIndex);
		}
	}

	check(OutMinLOD <= OutMaxLOD);
}

int32 FPrimitiveSceneInfo::UpdateStaticLightingBuffer()
{
	checkSlow(IsInRenderingThread());

	if (bRegisteredVirtualTextureProducerCallback)
	{
		// Remove any previous VT callbacks
		FVirtualTextureSystem::Get().RemoveAllProducerDestroyedCallbacks(this);
		bRegisteredVirtualTextureProducerCallback = false;
	}

	FPrimitiveSceneProxy::FLCIArray LCIs;
	Proxy->GetLCIs(LCIs);
	for (int32 i = 0; i < LCIs.Num(); ++i)
	{
		FLightCacheInterface* LCI = LCIs[i];

		if (LCI)
		{
			LCI->CreatePrecomputedLightingUniformBuffer_RenderingThread(Scene->GetFeatureLevel());

			// If lightmap is using virtual texture, need to set a callback to update our uniform buffers if VT is destroyed,
			// since we cache VT parameters inside these uniform buffers
			FVirtualTextureProducerHandle VTProducerHandle;
			if (LCI->GetVirtualTextureLightmapProducer(Scene->GetFeatureLevel(), VTProducerHandle))
			{
				FVirtualTextureSystem::Get().AddProducerDestroyedCallback(VTProducerHandle, &OnVirtualTextureDestroyed, this);
				bRegisteredVirtualTextureProducerCallback = true;
			}
		}
	}

	return LCIs.Num();
}

void FPrimitiveSceneInfo::AllocateGPUSceneInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (Scene->GPUScene.IsEnabled())
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateGPUSceneTime);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			check
			(
				SceneInfo->InstanceSceneDataOffset == INDEX_NONE &&
				SceneInfo->NumInstanceSceneDataEntries == 0 &&
				SceneInfo->InstancePayloadDataOffset == INDEX_NONE &&
				SceneInfo->InstancePayloadDataStride == 0
			);

			if (SceneInfo->Proxy->SupportsInstanceDataBuffer())
			{
				const TConstArrayView<FPrimitiveInstance> InstanceSceneData = SceneInfo->Proxy->GetInstanceSceneData();

				SceneInfo->NumInstanceSceneDataEntries = InstanceSceneData.Num();
				if (SceneInfo->NumInstanceSceneDataEntries > 0)
				{
					SceneInfo->InstanceSceneDataOffset = Scene->GPUScene.AllocateInstanceSceneDataSlots(SceneInfo->NumInstanceSceneDataEntries);

					SceneInfo->InstancePayloadDataStride = SceneInfo->Proxy->GetPayloadDataStride(); // Returns number of float4 optional data values
					if (SceneInfo->InstancePayloadDataStride > 0)
					{
						const uint32 TotalFloat4Count = SceneInfo->NumInstanceSceneDataEntries * SceneInfo->InstancePayloadDataStride;
						SceneInfo->InstancePayloadDataOffset = Scene->GPUScene.AllocateInstancePayloadDataSlots(TotalFloat4Count);
					}

					if (GGPUSceneInstanceBVH)
					{
						// TODO: Replace Instance BVH FBounds with FRenderBounds
						for (int32 InstanceIndex = 0; InstanceIndex < SceneInfo->NumInstanceSceneDataEntries; ++InstanceIndex)
						{
							const FPrimitiveInstance& PrimitiveInstance = InstanceSceneData[InstanceIndex];
							FRenderBounds WorldBounds = SceneInfo->Proxy->GetInstanceLocalBounds(InstanceIndex);
							WorldBounds.TransformBy(PrimitiveInstance.ComputeLocalToWorld(SceneInfo->Proxy->GetLocalToWorld()));
							Scene->InstanceBVH.Add(FBounds({ WorldBounds.GetMin(), WorldBounds.GetMax() }), SceneInfo->InstanceSceneDataOffset + InstanceIndex);
						}
					}
				}
			}
			else
			{
				// Allocate a single 'dummy/fallback' instance for the primitive that gets automatically populated with the data from the primitive
				SceneInfo->InstanceSceneDataOffset = Scene->GPUScene.AllocateInstanceSceneDataSlots(1);
				SceneInfo->NumInstanceSceneDataEntries = 1;
				
				// TODO: Hook up for dummy instances?
				SceneInfo->InstancePayloadDataOffset = INDEX_NONE;
				SceneInfo->InstancePayloadDataStride = 0;
			}

			// Force a primitive update in the GPU scene, 
			// NOTE: does not set Added as this is handled elsewhere.
			Scene->GPUScene.AddPrimitiveToUpdate(SceneInfo->PackedIndex, EPrimitiveDirtyState::ChangedAll);

			// Force a primitive update in the Lumen scene
			if (Scene->LumenSceneData)
			{
				Scene->LumenSceneData->UpdatePrimitiveInstanceOffset(SceneInfo->PackedIndex);
			}
		}

		OnGPUSceneInstancesAllocated.Broadcast();
	}
}

void FPrimitiveSceneInfo::ReallocateGPUSceneInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	SCOPED_NAMED_EVENT(ReallocateGPUSceneInstances, FColor::Emerald);

	// Free each scene info.
	for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
	{
		SceneInfo->FreeGPUSceneInstances();
	}

	// Allocate them all.
	AllocateGPUSceneInstances(Scene, SceneInfos);
}

void FPrimitiveSceneInfo::FreeGPUSceneInstances()
{
	if (!Scene->GPUScene.IsEnabled())
	{
		return;
	}

	// Release all instance data slots associated with this primitive.
	if (InstanceSceneDataOffset != INDEX_NONE)
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateGPUSceneTime);

		check(Proxy->SupportsInstanceDataBuffer() || NumInstanceSceneDataEntries == 1);
		if (GGPUSceneInstanceBVH)
		{
			for (int32 InstanceIndex = 0; InstanceIndex < NumInstanceSceneDataEntries; InstanceIndex++)
			{
				Scene->InstanceBVH.Remove(InstanceSceneDataOffset + InstanceIndex);
			}
		}

		// Release all instance payload data slots associated with this primitive.
		if (InstancePayloadDataOffset != INDEX_NONE)
		{
			check(InstancePayloadDataStride > 0);

			const uint32 TotalFloat4Count = NumInstanceSceneDataEntries * InstancePayloadDataStride;
			Scene->GPUScene.FreeInstancePayloadDataSlots(InstancePayloadDataOffset, TotalFloat4Count);
			InstancePayloadDataOffset = INDEX_NONE;
			InstancePayloadDataStride = 0;
		}

		Scene->GPUScene.FreeInstanceSceneDataSlots(InstanceSceneDataOffset, NumInstanceSceneDataEntries);
		InstanceSceneDataOffset = INDEX_NONE;
		NumInstanceSceneDataEntries = 0;

		OnGPUSceneInstancesFreed.Broadcast();
	}
}

void FPrimitiveSceneInfo::AddToScene(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos, bool bUpdateStaticDrawLists, bool bAddToStaticDrawLists, bool bAsyncCreateLPIs)
{
	check(IsInRenderingThread());

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_IndirectLightingCacheUniformBuffer, FColor::Turquoise);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			// Create an indirect lighting cache uniform buffer if we attaching a primitive that may require it, as it may be stored inside a cached mesh command.
			if (IsIndirectLightingCacheAllowed(Scene->GetFeatureLevel())
				&& Proxy->WillEverBeLit()
				&& ((Proxy->HasStaticLighting() && Proxy->NeedsUnbuiltPreviewLighting()) || (Proxy->IsMovable() && Proxy->GetIndirectLightingCacheQuality() != ILCQ_Off) || Proxy->GetLightmapType() == ELightmapType::ForceVolumetric))
			{
				if (!SceneInfo->IndirectLightingCacheUniformBuffer)
				{
					FIndirectLightingCacheUniformParameters Parameters;

					GetIndirectLightingCacheParameters(
						Scene->GetFeatureLevel(),
						Parameters,
						nullptr,
						nullptr,
						FVector(0.0f, 0.0f, 0.0f),
						0,
						nullptr);

					SceneInfo->IndirectLightingCacheUniformBuffer = TUniformBufferRef<FIndirectLightingCacheUniformParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
				}
			}
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_IndirectLightingCacheAllocation, FColor::Orange);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			// If we are attaching a primitive that should be statically lit but has unbuilt lighting,
			// Allocate space in the indirect lighting cache so that it can be used for previewing indirect lighting
			if (Proxy->HasStaticLighting()
				&& Proxy->NeedsUnbuiltPreviewLighting()
				&& IsIndirectLightingCacheAllowed(Scene->GetFeatureLevel()))
			{
				FIndirectLightingCacheAllocation* PrimitiveAllocation = Scene->IndirectLightingCache.FindPrimitiveAllocation(SceneInfo->PrimitiveComponentId);

				if (PrimitiveAllocation)
				{
					SceneInfo->IndirectLightingCacheAllocation = PrimitiveAllocation;
					PrimitiveAllocation->SetDirty();
				}
				else
				{
					PrimitiveAllocation = Scene->IndirectLightingCache.AllocatePrimitive(SceneInfo, true);
					PrimitiveAllocation->SetDirty();
					SceneInfo->IndirectLightingCacheAllocation = PrimitiveAllocation;
				}
			}
			SceneInfo->MarkIndirectLightingCacheBufferDirty();
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_LightmapDataOffset, FColor::Green);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			const bool bAllowStaticLighting = FReadOnlyCVARCache::Get().bAllowStaticLighting;
			if (bAllowStaticLighting)
			{
				SceneInfo->NumLightmapDataEntries = SceneInfo->UpdateStaticLightingBuffer();
				if (SceneInfo->NumLightmapDataEntries > 0 && UseGPUScene(GMaxRHIShaderPlatform, Scene->GetFeatureLevel()))
				{
					SceneInfo->LightmapDataOffset = Scene->GPUScene.LightmapDataAllocator.Allocate(SceneInfo->NumLightmapDataEntries);
				}
			}
		}
	}


	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_ReflectionCaptures, FColor::Yellow);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// Cache the nearest reflection proxy if needed
			if (SceneInfo->NeedsReflectionCaptureUpdate())
			{
				SceneInfo->CacheReflectionCaptures();
			}
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_AddStaticMeshes, FColor::Magenta);
		if (bUpdateStaticDrawLists)
		{
			AddStaticMeshes(RHICmdList, Scene, SceneInfos, bAddToStaticDrawLists);
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_AddToPrimitiveOctree, FColor::Red);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// create potential storage for our compact info
			FPrimitiveSceneInfoCompact CompactPrimitiveSceneInfo(SceneInfo);

			// Add the primitive to the octree.
			check(!SceneInfo->OctreeId.IsValidId());
			Scene->PrimitiveOctree.AddElement(CompactPrimitiveSceneInfo);
			check(SceneInfo->OctreeId.IsValidId());
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_UpdateBounds, FColor::Cyan);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			int32 PackedIndex = SceneInfo->PackedIndex;

			if (Proxy->CastsDynamicIndirectShadow())
			{
				Scene->DynamicIndirectCasterPrimitives.Add(SceneInfo);
			}

			Scene->PrimitiveSceneProxies[PackedIndex] = Proxy;
			Scene->PrimitiveTransforms[PackedIndex] = Proxy->GetLocalToWorld();

			// Set bounds.
			FPrimitiveBounds& PrimitiveBounds = Scene->PrimitiveBounds[PackedIndex];
			FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds();
			PrimitiveBounds.BoxSphereBounds = BoxSphereBounds;
			PrimitiveBounds.MinDrawDistance = Proxy->GetMinDrawDistance();
			PrimitiveBounds.MaxDrawDistance = Proxy->GetMaxDrawDistance();
			PrimitiveBounds.MaxCullDistance = PrimitiveBounds.MaxDrawDistance;

			Scene->PrimitiveFlagsCompact[PackedIndex] = FPrimitiveFlagsCompact(Proxy);

			// Store precomputed visibility ID.
			int32 VisibilityBitIndex = Proxy->GetVisibilityId();
			FPrimitiveVisibilityId& VisibilityId = Scene->PrimitiveVisibilityIds[PackedIndex];
			VisibilityId.ByteIndex = VisibilityBitIndex / 8;
			VisibilityId.BitMask = (1 << (VisibilityBitIndex & 0x7));

			// Store occlusion flags.
			uint8 OcclusionFlags = EOcclusionFlags::None;
			if (Proxy->CanBeOccluded())
			{
				OcclusionFlags |= EOcclusionFlags::CanBeOccluded;
			}
			if (Proxy->HasSubprimitiveOcclusionQueries())
			{
				OcclusionFlags |= EOcclusionFlags::HasSubprimitiveQueries;
			}
			if (Proxy->AllowApproximateOcclusion()
				// Allow approximate occlusion if attached, even if the parent does not have bLightAttachmentsAsGroup enabled
				|| SceneInfo->LightingAttachmentRoot.IsValid())
			{
				OcclusionFlags |= EOcclusionFlags::AllowApproximateOcclusion;
			}
			if (VisibilityBitIndex >= 0)
			{
				OcclusionFlags |= EOcclusionFlags::HasPrecomputedVisibility;
			}
			Scene->PrimitiveOcclusionFlags[PackedIndex] = OcclusionFlags;

			// Store occlusion bounds.
			FBoxSphereBounds OcclusionBounds = BoxSphereBounds;
			if (Proxy->HasCustomOcclusionBounds())
			{
				OcclusionBounds = Proxy->GetCustomOcclusionBounds();
			}
			OcclusionBounds.BoxExtent.X = OcclusionBounds.BoxExtent.X + OCCLUSION_SLOP;
			OcclusionBounds.BoxExtent.Y = OcclusionBounds.BoxExtent.Y + OCCLUSION_SLOP;
			OcclusionBounds.BoxExtent.Z = OcclusionBounds.BoxExtent.Z + OCCLUSION_SLOP;
			OcclusionBounds.SphereRadius = OcclusionBounds.SphereRadius + OCCLUSION_SLOP;
			Scene->PrimitiveOcclusionBounds[PackedIndex] = OcclusionBounds;

			// Store the component.
			Scene->PrimitiveComponentIds[PackedIndex] = SceneInfo->PrimitiveComponentId;

#if RHI_RAYTRACING
			// Set group id
			const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
			if (RayTracingGroupId != -1)
			{
				Scene->PrimitiveRayTracingGroupIds[PackedIndex] = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
			}
#endif
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_UpdateVirtualTexture, FColor::Emerald);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			// Store the runtime virtual texture flags.
			SceneInfo->UpdateRuntimeVirtualTextureFlags();
			Scene->PrimitiveVirtualTextureFlags[SceneInfo->PackedIndex] = SceneInfo->RuntimeVirtualTextureFlags;

			// Store the runtime virtual texture Lod info.
			if (SceneInfo->RuntimeVirtualTextureFlags.bRenderToVirtualTexture)
			{
				int8 MinLod, MaxLod;
				GetRuntimeVirtualTextureLODRange(SceneInfo->StaticMeshRelevances, MinLod, MaxLod);

				FPrimitiveVirtualTextureLodInfo& LodInfo = Scene->PrimitiveVirtualTextureLod[SceneInfo->PackedIndex];
				LodInfo.MinLod = FMath::Clamp((int32)MinLod, 0, 15);
				LodInfo.MaxLod = FMath::Clamp((int32)MaxLod, 0, 15);
				LodInfo.LodBias = FMath::Clamp(Proxy->GetVirtualTextureLodBias() + FPrimitiveVirtualTextureLodInfo::LodBiasOffset, 0, 15);
				LodInfo.CullMethod = Proxy->GetVirtualTextureMinCoverage() == 0 ? 0 : 1;
				LodInfo.CullValue = LodInfo.CullMethod == 0 ? Proxy->GetVirtualTextureCullMips() : Proxy->GetVirtualTextureMinCoverage();
			}
		}
	}

	// Find lights that affect the primitive in the light octree.
	for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
	{
		Scene->CreateLightPrimitiveInteractionsForPrimitive(SceneInfo, bAsyncCreateLPIs);

		FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
		INC_MEMORY_STAT_BY(STAT_PrimitiveInfoMemory, sizeof(*SceneInfo) + SceneInfo->StaticMeshes.GetAllocatedSize() + SceneInfo->StaticMeshRelevances.GetAllocatedSize() + Proxy->GetMemoryFootprint());
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_LevelNotifyPrimitives, FColor::Blue);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			if (SceneInfo->Proxy->ShouldNotifyOnWorldAddRemove())
			{
				TArray<FPrimitiveSceneInfo*>& LevelNotifyPrimitives = Scene->PrimitivesNeedingLevelUpdateNotification.FindOrAdd(SceneInfo->Proxy->GetLevelName());
				SceneInfo->LevelUpdateNotificationIndex = LevelNotifyPrimitives.Num();
				LevelNotifyPrimitives.Add(SceneInfo);
			}
		}
	}
}

void FPrimitiveSceneInfo::RemoveStaticMeshes()
{
	// Deallocate potential OIT dynamic index buffer
	if (OIT::IsEnabled(GMaxRHIShaderPlatform))
	{
		for (int32 MeshIndex = 0; MeshIndex < StaticMeshes.Num(); MeshIndex++)
		{
			FStaticMeshBatch& Mesh = StaticMeshes[MeshIndex];
			if (Mesh.Elements.Num() > 0 && Mesh.Elements[0].DynamicIndexBuffer.IsValid())
			{
				Scene->OITSceneData.Deallocate(Mesh.Elements[0].DynamicIndexBuffer.IndexBuffer);
			}
		}
	}

	// Remove static meshes from the scene.
	StaticMeshes.Empty();
	StaticMeshRelevances.Empty();
	RemoveCachedMeshDrawCommands();
	RemoveCachedNaniteDrawCommands();
#if RHI_RAYTRACING
	RemoveCachedRayTracingPrimitives();
#endif
}

void FPrimitiveSceneInfo::RemoveFromScene(bool bUpdateStaticDrawLists)
{
	check(IsInRenderingThread());

	// implicit linked list. The destruction will update this "head" pointer to the next item in the list.
	while (LightList)
	{
		FLightPrimitiveInteraction::Destroy(LightList);
	}

	// Remove the primitive from the octree.
	check(OctreeId.IsValidId());
	check(Scene->PrimitiveOctree.GetElementById(OctreeId).PrimitiveSceneInfo == this);
	Scene->PrimitiveOctree.RemoveElement(OctreeId);
	OctreeId = FOctreeElementId2();

	if (LightmapDataOffset != INDEX_NONE && UseGPUScene(GMaxRHIShaderPlatform, Scene->GetFeatureLevel()))
	{
		Scene->GPUScene.LightmapDataAllocator.Free(LightmapDataOffset, NumLightmapDataEntries);
	}

	if (Proxy->CastsDynamicIndirectShadow())
	{
		Scene->DynamicIndirectCasterPrimitives.RemoveSingleSwap(this);
	}

	IndirectLightingCacheAllocation = NULL;

	if (Proxy->IsOftenMoving())
	{
		MarkIndirectLightingCacheBufferDirty();
	}

	DEC_MEMORY_STAT_BY(STAT_PrimitiveInfoMemory, sizeof(*this) + StaticMeshes.GetAllocatedSize() + StaticMeshRelevances.GetAllocatedSize() + Proxy->GetMemoryFootprint());

	if (bUpdateStaticDrawLists)
	{
		if (IsIndexValid()) // PackedIndex
		{
			Scene->PrimitivesNeedingStaticMeshUpdate[PackedIndex] = false;
		}

		if (bNeedsStaticMeshUpdateWithoutVisibilityCheck)
		{
			Scene->PrimitivesNeedingStaticMeshUpdateWithoutVisibilityCheck.Remove(this);

			bNeedsStaticMeshUpdateWithoutVisibilityCheck = false;
		}

		// IndirectLightingCacheUniformBuffer may be cached inside cached mesh draw commands, so we 
		// can't delete it unless we also update cached mesh command.
		IndirectLightingCacheUniformBuffer.SafeRelease();

		RemoveStaticMeshes();
	}

	if (bRegisteredVirtualTextureProducerCallback)
	{
		FVirtualTextureSystem::Get().RemoveAllProducerDestroyedCallbacks(this);
		bRegisteredVirtualTextureProducerCallback = false;
	}

	if (Proxy->ShouldNotifyOnWorldAddRemove())
	{
		TArray<FPrimitiveSceneInfo*>* LevelNotifyPrimitives = Scene->PrimitivesNeedingLevelUpdateNotification.Find(Proxy->GetLevelName());
		if (LevelNotifyPrimitives != nullptr)
		{
			checkSlow(LevelUpdateNotificationIndex != INDEX_NONE);
			LevelNotifyPrimitives->RemoveAtSwap(LevelUpdateNotificationIndex, 1, false);
			if (LevelNotifyPrimitives->Num() == 0)
			{
				Scene->PrimitivesNeedingLevelUpdateNotification.Remove(Proxy->GetLevelName());
			}
			else if (LevelUpdateNotificationIndex < LevelNotifyPrimitives->Num())
			{
				// Update swapped element's LevelUpdateNotificationIndex
				((*LevelNotifyPrimitives)[LevelUpdateNotificationIndex])->LevelUpdateNotificationIndex = LevelUpdateNotificationIndex;
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateRuntimeVirtualTextureFlags()
{
	RuntimeVirtualTextureFlags.bRenderToVirtualTexture = false;
	RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask = 0;

	if (Proxy->WritesVirtualTexture())
	{
		if (Proxy->IsNaniteMesh())
		{
			UE_LOG(LogRenderer, Warning, TEXT("Rendering a nanite mesh to a runtime virtual texture isn't yet supported. Please disable this option on primitive component : %s"), *Proxy->GetOwnerName().ToString());
		}
		else if (StaticMeshes.Num() == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("Rendering a primitive in a runtime virtual texture implies that there is a mesh to render. Please disable this option on primitive component : %s"), *Proxy->GetOwnerName().ToString());
		}
		else
		{
			RuntimeVirtualTextureFlags.bRenderToVirtualTexture = true;

			// Performance assumption: The arrays of runtime virtual textures are small (less that 5?) so that O(n^2) scan isn't expensive
			for (TSparseArray<FRuntimeVirtualTextureSceneProxy*>::TConstIterator It(Scene->RuntimeVirtualTextures); It; ++It)
			{
				int32 SceneIndex = It.GetIndex();
				if (SceneIndex < FPrimitiveVirtualTextureFlags::RuntimeVirtualTexture_BitCount)
				{
					URuntimeVirtualTexture* SceneVirtualTexture = (*It)->VirtualTexture;
					if (Proxy->WritesVirtualTexture(SceneVirtualTexture))
					{
						RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask |= 1 << SceneIndex;
					}
				}
			}
		}
	}
}

bool FPrimitiveSceneInfo::NeedsUpdateStaticMeshes()
{
	return Scene->PrimitivesNeedingStaticMeshUpdate[PackedIndex];
}

void FPrimitiveSceneInfo::UpdateStaticMeshes(FRHICommandListImmediate& RHICmdList, FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos, EUpdateStaticMeshFlags UpdateFlags, bool bReAddToDrawLists)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FPrimitiveSceneInfo_UpdateStaticMeshes);
	TRACE_CPUPROFILER_EVENT_SCOPE(FPrimitiveSceneInfo_UpdateStaticMeshes);

	const bool bUpdateRayTracingCommands = EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands) || !IsRayTracingEnabled();
	const bool bUpdateAllCommands = EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands) && bUpdateRayTracingCommands;

	const bool bNeedsStaticMeshUpdate = !(bReAddToDrawLists && bUpdateAllCommands);

	for (int32 Index = 0; Index < SceneInfos.Num(); Index++)
	{
		FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
		Scene->PrimitivesNeedingStaticMeshUpdate[SceneInfo->PackedIndex] = bNeedsStaticMeshUpdate;

		if (!bNeedsStaticMeshUpdate && SceneInfo->bNeedsStaticMeshUpdateWithoutVisibilityCheck)
		{
			Scene->PrimitivesNeedingStaticMeshUpdateWithoutVisibilityCheck.Remove(SceneInfo);

			SceneInfo->bNeedsStaticMeshUpdateWithoutVisibilityCheck = false;
		}

		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands))
		{
			SceneInfo->RemoveCachedMeshDrawCommands();
			SceneInfo->RemoveCachedNaniteDrawCommands();
		}

	#if RHI_RAYTRACING
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands))
		{
			SceneInfo->RemoveCachedRayTracingPrimitives();
		}
	#endif

		if (SceneInfo->Proxy && SceneInfo->Proxy->IsNaniteMesh())
		{
			// Make sure material table indirections are kept in sync with GPU Scene and cached Nanite MDCs
			SceneInfo->RequestGPUSceneUpdate(EPrimitiveDirtyState::ChangedOther);
		}
	}

	if (bReAddToDrawLists)
	{
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands))
		{
			CacheMeshDrawCommands(RHICmdList, Scene, SceneInfos);
			CacheNaniteDrawCommands(RHICmdList, Scene, SceneInfos);
		}

	#if RHI_RAYTRACING
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands))
		{
			CacheRayTracingPrimitives(Scene, SceneInfos);
		}
	#endif
	}
}

#if RHI_RAYTRACING
void FPrimitiveSceneInfo::UpdateCachedRaytracingData(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (SceneInfos.Num() > 0)
	{
		for (int32 Index = 0; Index < SceneInfos.Num(); Index++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index]; 
			// should have been marked dirty by calling UpdateCachedRayTracingState on the scene before
			// scene info is being updated here
			check(SceneInfo->bCachedRaytracingDataDirty);
			SceneInfo->RemoveCachedRayTracingPrimitives();
			SceneInfo->bCachedRaytracingDataDirty = false;
		}

		CacheRayTracingPrimitives(Scene, SceneInfos);
	}
}
#endif //RHI_RAYTRACING

void FPrimitiveSceneInfo::UpdateUniformBuffer(FRHICommandListImmediate& RHICmdList)
{
	checkSlow(bNeedsUniformBufferUpdate);
	bNeedsUniformBufferUpdate = false;
	Proxy->UpdateUniformBuffer();
	// TODO: Figure out when and why this is called
	Scene->GPUScene.AddPrimitiveToUpdate(PackedIndex, EPrimitiveDirtyState::ChangedAll);
}

void FPrimitiveSceneInfo::BeginDeferredUpdateStaticMeshes()
{
	// Set a flag which causes InitViews to update the static meshes the next time the primitive is visible.
	if (IsIndexValid()) // PackedIndex
	{
		Scene->PrimitivesNeedingStaticMeshUpdate[PackedIndex] = true;
	}
}

void FPrimitiveSceneInfo::BeginDeferredUpdateStaticMeshesWithoutVisibilityCheck()
{
	if (NeedsUpdateStaticMeshes() && !bNeedsStaticMeshUpdateWithoutVisibilityCheck)
	{
		bNeedsStaticMeshUpdateWithoutVisibilityCheck = true;

		Scene->PrimitivesNeedingStaticMeshUpdateWithoutVisibilityCheck.Add(this);
	}
}

void FPrimitiveSceneInfo::FlushRuntimeVirtualTexture()
{
	if (RuntimeVirtualTextureFlags.bRenderToVirtualTexture)
	{
		uint32 RuntimeVirtualTextureIndex = 0;
		uint32 Mask = RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask;
		while (Mask != 0)
		{
			if (Mask & 1)
			{
				Scene->RuntimeVirtualTextures[RuntimeVirtualTextureIndex]->Dirty(Proxy->GetBounds());
			}
			Mask >>= 1;
			RuntimeVirtualTextureIndex++;
		}
	}
}

void FPrimitiveSceneInfo::LinkLODParentComponent()
{
	if (LODParentComponentId.IsValid())
	{
		Scene->SceneLODHierarchy.AddChildNode(LODParentComponentId, this);
	}
}

void FPrimitiveSceneInfo::UnlinkLODParentComponent()
{
	if(LODParentComponentId.IsValid())
	{
		Scene->SceneLODHierarchy.RemoveChildNode(LODParentComponentId, this);
	}
}

void FPrimitiveSceneInfo::LinkAttachmentGroup()
{
	// Add the primitive to its attachment group.
	if (LightingAttachmentRoot.IsValid())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(LightingAttachmentRoot);

		if (!AttachmentGroup)
		{
			// If this is the first primitive attached that uses this attachment parent, create a new attachment group.
			AttachmentGroup = &Scene->AttachmentGroups.Add(LightingAttachmentRoot, FAttachmentGroupSceneInfo());
		}

		AttachmentGroup->Primitives.Add(this);
	}
	else if (Proxy->LightAttachmentsAsGroup())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (!AttachmentGroup)
		{
			// Create an empty attachment group 
			AttachmentGroup = &Scene->AttachmentGroups.Add(PrimitiveComponentId, FAttachmentGroupSceneInfo());
		}

		AttachmentGroup->ParentSceneInfo = this;
	}
}

void FPrimitiveSceneInfo::UnlinkAttachmentGroup()
{
	// Remove the primitive from its attachment group.
	if (LightingAttachmentRoot.IsValid())
	{
		FAttachmentGroupSceneInfo& AttachmentGroup = Scene->AttachmentGroups.FindChecked(LightingAttachmentRoot);
		AttachmentGroup.Primitives.RemoveSwap(this);

		if (AttachmentGroup.Primitives.Num() == 0 && AttachmentGroup.ParentSceneInfo == nullptr)
		{
			// If this was the last primitive attached that uses this attachment group and the root has left the building, free the group.
			Scene->AttachmentGroups.Remove(LightingAttachmentRoot);
		}
	}
	else if (Proxy->LightAttachmentsAsGroup())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);
		
		if (AttachmentGroup)
		{
			AttachmentGroup->ParentSceneInfo = NULL;
			if (AttachmentGroup->Primitives.Num() == 0)
			{
				// If this was the owner and the group is empty, remove it (otherwise the above will remove when the last attached goes).
				Scene->AttachmentGroups.Remove(PrimitiveComponentId);
			}
		}
	}
}

bool FPrimitiveSceneInfo::RequestGPUSceneUpdate(EPrimitiveDirtyState PrimitiveDirtyState)
{
	if (Scene && IsIndexValid())
	{
		Scene->GPUScene.AddPrimitiveToUpdate(GetIndex(), PrimitiveDirtyState);
		return true;
	}

	return false;
}

void FPrimitiveSceneInfo::GatherLightingAttachmentGroupPrimitives(TArray<FPrimitiveSceneInfo*, SceneRenderingAllocator>& OutChildSceneInfos)
{
#if ENABLE_NAN_DIAGNOSTIC
	// local function that returns full name of object
	auto GetObjectName = [](const UPrimitiveComponent* InPrimitive)->FString
	{
		return (InPrimitive) ? InPrimitive->GetFullName() : FString(TEXT("Unknown Object"));
	};

	// verify that the current object has a valid bbox before adding it
	const float& BoundsRadius = this->Proxy->GetBounds().SphereRadius;
	if (ensureMsgf(!FMath::IsNaN(BoundsRadius) && FMath::IsFinite(BoundsRadius),
		TEXT("%s had an ill-formed bbox and was skipped during shadow setup, contact DavidH."), *GetObjectName(this->ComponentForDebuggingOnly)))
	{
		OutChildSceneInfos.Add(this);
	}
	else
	{
		// return, leaving the TArray empty
		return;
	}

#else 
	// add self at the head of this queue
	OutChildSceneInfos.Add(this);
#endif

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			
			for (int32 ChildIndex = 0, ChildIndexMax = AttachmentGroup->Primitives.Num(); ChildIndex < ChildIndexMax; ChildIndex++)
			{
				FPrimitiveSceneInfo* ShadowChild = AttachmentGroup->Primitives[ChildIndex];
#if ENABLE_NAN_DIAGNOSTIC
				// Only enqueue objects with valid bounds using the normality of the SphereRaduis as criteria.

				const float& ShadowChildBoundsRadius = ShadowChild->Proxy->GetBounds().SphereRadius;

				if (ensureMsgf(!FMath::IsNaN(ShadowChildBoundsRadius) && FMath::IsFinite(ShadowChildBoundsRadius),
					TEXT("%s had an ill-formed bbox and was skipped during shadow setup, contact DavidH."), *GetObjectName(ShadowChild->ComponentForDebuggingOnly)))
				{
					checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
				    OutChildSceneInfos.Add(ShadowChild);
				}
#else
				// enqueue all objects.
				checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
			    OutChildSceneInfos.Add(ShadowChild);
#endif
			}
		}
	}
}

void FPrimitiveSceneInfo::GatherLightingAttachmentGroupPrimitives(TArray<const FPrimitiveSceneInfo*, SceneRenderingAllocator>& OutChildSceneInfos) const
{
	OutChildSceneInfos.Add(this);

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			for (int32 ChildIndex = 0, ChildIndexMax = AttachmentGroup->Primitives.Num(); ChildIndex < ChildIndexMax; ChildIndex++)
			{
				const FPrimitiveSceneInfo* ShadowChild = AttachmentGroup->Primitives[ChildIndex];

				checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
			    OutChildSceneInfos.Add(ShadowChild);
			}
		}
	}
}

FBoxSphereBounds FPrimitiveSceneInfo::GetAttachmentGroupBounds() const
{
	FBoxSphereBounds Bounds = Proxy->GetBounds();

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			for (int32 ChildIndex = 0; ChildIndex < AttachmentGroup->Primitives.Num(); ChildIndex++)
			{
				FPrimitiveSceneInfo* AttachmentChild = AttachmentGroup->Primitives[ChildIndex];
				Bounds = Bounds + AttachmentChild->Proxy->GetBounds();
			}
		}
	}

	return Bounds;
}

uint32 FPrimitiveSceneInfo::GetMemoryFootprint()
{
	return( sizeof( *this ) + HitProxies.GetAllocatedSize() + StaticMeshes.GetAllocatedSize() + StaticMeshRelevances.GetAllocatedSize() );
}

void FPrimitiveSceneInfo::ApplyWorldOffset(FVector InOffset)
{
	Proxy->ApplyWorldOffset(InOffset);
}

void FPrimitiveSceneInfo::UpdateIndirectLightingCacheBuffer(
	const FIndirectLightingCache* LightingCache,
	const FIndirectLightingCacheAllocation* LightingAllocation,
	FVector VolumetricLightmapLookupPosition,
	uint32 SceneFrameNumber,
	FVolumetricLightmapSceneData* VolumetricLightmapSceneData)
{
	FIndirectLightingCacheUniformParameters Parameters;

	GetIndirectLightingCacheParameters(
		Scene->GetFeatureLevel(),
		Parameters,
		LightingCache,
		LightingAllocation,
		VolumetricLightmapLookupPosition,
		SceneFrameNumber,
		VolumetricLightmapSceneData);

	if (IndirectLightingCacheUniformBuffer)
	{
		IndirectLightingCacheUniformBuffer.UpdateUniformBufferImmediate(Parameters);
	}
}

void FPrimitiveSceneInfo::UpdateIndirectLightingCacheBuffer()
{
	if (bIndirectLightingCacheBufferDirty)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UpdateIndirectLightingCacheBuffer);

		if (Scene->GetFeatureLevel() < ERHIFeatureLevel::SM5
			&& Scene->VolumetricLightmapSceneData.HasData()
			&& (Proxy->IsMovable() || Proxy->NeedsUnbuiltPreviewLighting() || Proxy->GetLightmapType() == ELightmapType::ForceVolumetric)
			&& Proxy->WillEverBeLit())
		{
			UpdateIndirectLightingCacheBuffer(
				nullptr, 
				nullptr,
				Proxy->GetBounds().Origin,
				Scene->GetFrameNumber(),
				&Scene->VolumetricLightmapSceneData);
		}
		// The update is invalid if the lighting cache allocation was not in a functional state.
		else if (IndirectLightingCacheAllocation && (Scene->IndirectLightingCache.IsInitialized() && IndirectLightingCacheAllocation->bHasEverUpdatedSingleSample))
		{
			UpdateIndirectLightingCacheBuffer(
				&Scene->IndirectLightingCache,
				IndirectLightingCacheAllocation,
				FVector(0, 0, 0),
				0,
				nullptr);
		}
		else
		{
			// Fallback to the global empty buffer parameters
			UpdateIndirectLightingCacheBuffer(nullptr, nullptr, FVector(0.0f, 0.0f, 0.0f), 0, nullptr);
		}

		bIndirectLightingCacheBufferDirty = false;
	}
}

void FPrimitiveSceneInfo::GetStaticMeshesLODRange(int8& OutMinLOD, int8& OutMaxLOD) const
{
	OutMinLOD = MAX_int8;
	OutMaxLOD = 0;

	for (int32 MeshIndex = 0; MeshIndex < StaticMeshRelevances.Num(); ++MeshIndex)
	{
		const FStaticMeshBatchRelevance& MeshRelevance = StaticMeshRelevances[MeshIndex];
		OutMinLOD = FMath::Min(OutMinLOD, MeshRelevance.LODIndex);
		OutMaxLOD = FMath::Max(OutMaxLOD, MeshRelevance.LODIndex);
	}
}

const FMeshBatch* FPrimitiveSceneInfo::GetMeshBatch(int8 InLODIndex) const
{
	if (StaticMeshes.IsValidIndex(InLODIndex))
	{
		return &StaticMeshes[InLODIndex];
	}

	return nullptr;
}

bool FPrimitiveSceneInfo::NeedsReflectionCaptureUpdate() const
{
	return bNeedsCachedReflectionCaptureUpdate && 
		// For mobile, the per-object reflection is used for everything
		(Scene->GetShadingPath() == EShadingPath::Mobile || IsForwardShadingEnabled(Scene->GetShaderPlatform()));
}

void FPrimitiveSceneInfo::CacheReflectionCaptures()
{
	// do not use Scene->PrimitiveBounds here, as it may be not initialized yet
	FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds(); 
	
	CachedReflectionCaptureProxy = Scene->FindClosestReflectionCapture(BoxSphereBounds.Origin);
	CachedPlanarReflectionProxy = Scene->FindClosestPlanarReflection(BoxSphereBounds);
	if (Scene->GetShadingPath() == EShadingPath::Mobile)
	{
		// mobile HQ reflections
		Scene->FindClosestReflectionCaptures(BoxSphereBounds.Origin, CachedReflectionCaptureProxies);
	}
	
	bNeedsCachedReflectionCaptureUpdate = false;
}

void FPrimitiveSceneInfo::RemoveCachedReflectionCaptures()
{
	CachedReflectionCaptureProxy = nullptr;
	CachedPlanarReflectionProxy = nullptr;
	FMemory::Memzero(CachedReflectionCaptureProxies);
	bNeedsCachedReflectionCaptureUpdate = true;
}

void FPrimitiveSceneInfo::UpdateComponentLastRenderTime(float CurrentWorldTime, bool bUpdateLastRenderTimeOnScreen) const
{
	ComponentForDebuggingOnly->LastRenderTime = CurrentWorldTime;
	if (bUpdateLastRenderTimeOnScreen)
	{
		ComponentForDebuggingOnly->LastRenderTimeOnScreen = CurrentWorldTime;
	}
	if (OwnerLastRenderTime)
	{
		*OwnerLastRenderTime = CurrentWorldTime; // Sets OwningActor->LastRenderTime
	}
}

void FPrimitiveOctreeSemantics::SetOctreeNodeIndex(const FPrimitiveSceneInfoCompact& Element, FOctreeElementId2 Id)
{
	// When a Primitive is removed from the renderer, it's index will be invalidated.  Only update if the primitive still
	// has a valid index.
	if (Element.PrimitiveSceneInfo->IsIndexValid())
	{
		Element.PrimitiveSceneInfo->Scene->PrimitiveOctreeIndex[Element.PrimitiveSceneInfo->GetIndex()] = Id.GetNodeIndex();
	}
}

FString FPrimitiveSceneInfo::GetFullnameForDebuggingOnly() const
{
	// This is not correct to access component from rendering thread, but this is for debugging only
	if (ComponentForDebuggingOnly)
	{	
		return ComponentForDebuggingOnly->GetFullGroupName(false);
	}
	return FString(TEXT("Unknown Object"));
}
