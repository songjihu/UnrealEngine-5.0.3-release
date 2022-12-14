// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
D3D12CommandContext.h: D3D12 Command Context Interfaces
=============================================================================*/

#pragma once
#define AFR_ENGINE_CHANGES_PRESENT WITH_MGPU

// TODO: Because the upper engine is yet to implement these interfaces we can't 'override' something that doesn't exist.
//       Remove when upper engine is ready
#if AFR_ENGINE_CHANGES_PRESENT
#define AFR_API_OVERRIDE override
#else
#define AFR_API_OVERRIDE
#endif

#include "D3D12RHIPrivate.h"
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <delayimp.h>

#if USE_PIX
	#include "pix3.h"
#endif
#include "Windows/HideWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_END

#include "RHICoreShader.h"

struct FRayTracingShaderBindings;

// Base class used to define commands that are not device specific, or that broadcast to all devices.
class FD3D12CommandContextBase : public IRHICommandContext, public FD3D12AdapterChild
{
public:

	FD3D12CommandContextBase(class FD3D12Adapter* InParent, FRHIGPUMask InGPUMask, ED3D12CommandQueueType InCommandQueueType, bool InIsDefaultContext);

	void RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) final override;
	void RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) final override;

	void RHIBeginFrame() final override;
	void RHIEndFrame() final override;

	virtual void SetRenderTargets(uint32 NumSimultaneousRenderTargets, const FRHIRenderTargetView* NewRenderTargets, const FRHIDepthRenderTargetView* NewDepthStencilTarget) = 0;

	virtual void UpdateMemoryStats();

	FRHIGPUMask GetGPUMask() const { return GPUMask; }

	ED3D12CommandQueueType GetCommandQueueType() const { return CommandQueueType; }
	bool IsDefaultContext() const { return bIsDefaultContext; }
	bool IsAsyncComputeContext() const { return (CommandQueueType == ED3D12CommandQueueType::Async); }

	virtual void RHISetAsyncComputeBudget(EAsyncComputeBudget Budget) {}

	bool IsDrawingSceneOrViewport() const {	return bDrawingScene || bDrawingViewport; }

protected:
	virtual FD3D12CommandContext* GetContext(uint32 InGPUIndex) = 0;

	void SignalTransitionFences(TArrayView<const FRHITransition*> Transitions);
	void WaitForTransitionFences(TArrayView<const FRHITransition*> Transitions);

	FRHIGPUMask GPUMask;

	bool bDrawingViewport = false;
	bool bDrawingScene = false;
	bool bTrackingEvents;
	const ED3D12CommandQueueType CommandQueueType;
	const bool bIsDefaultContext;
};

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class FD3D12CommandContext : public FD3D12CommandContextBase, public FD3D12DeviceChild
{
public:
	enum EFlushCommandsExtraAction
	{
		FCEA_None,
		FCEA_StartProfilingGPU,
		FCEA_EndProfilingGPU,
		FCEA_Num
	};

	FD3D12CommandContext(class FD3D12Device* InParent, ED3D12CommandQueueType InCommandQueueType, bool InIsDefaultContext);
	virtual ~FD3D12CommandContext();

	FD3D12CommandListManager& GetCommandListManager();

	template<typename TRHIType>
	static FORCEINLINE typename TD3D12ResourceTraits<TRHIType>::TConcreteType* ResourceCast(TRHIType* Resource)
	{
		return static_cast<typename TD3D12ResourceTraits<TRHIType>::TConcreteType*>(Resource);
	}

	void EndFrame()
	{
		StateCache.GetDescriptorCache()->EndFrame();

		// Return the current command allocator to the pool so it can be reused for a future frame
		// Note: the default context releases it's command allocator before Present.
		if (!IsDefaultContext())
		{
			ReleaseCommandAllocator();
		}
	}

	// If necessary, this gets a new command allocator for this context.
	void ConditionalObtainCommandAllocator();

	// Next time a command list is opened on this context, it will use a different command allocator.
	void ReleaseCommandAllocator();

	// Cycle to a new command list, but don't execute the current one yet.
	void OpenCommandList();
	void CloseCommandList();

	// Close the D3D command list and execute it.  Optionally wait for the GPU to finish. Returns the handle to the command list so you can wait for it later.
	FD3D12CommandListHandle FlushCommands(bool WaitForCompletion = false, EFlushCommandsExtraAction ExtraAction = FCEA_None);

	void Finish(TArray<FD3D12CommandListHandle>& CommandLists);

	void ClearState();
	void ConditionalClearShaderResource(FD3D12ResourceLocation* Resource);
	void ClearAllShaderResources();

	void ConditionalFlushCommandList();

	FD3D12FastConstantAllocator ConstantsAllocator;

	// Handles to the command list and direct command allocator this context owns (granted by the command list manager/command allocator manager), and a direct pointer to the D3D command list/command allocator.
	FD3D12CommandListHandle CommandListHandle;
	FD3D12CommandAllocator* CommandAllocator;
	FD3D12CommandAllocatorManager CommandAllocatorManager;

	// Sync point with copy queue which needs to be checked before kicking this command lists
	FD3D12SyncPoint CopyQueueSyncPoint;

	// Current GPU event stack
	TArray<uint32> GPUEventStack;

	FD3D12StateCache StateCache;

	FD3D12DynamicRHI& OwningRHI;

	// Tracks the currently set state blocks.
	FD3D12RenderTargetView* CurrentRenderTargets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
	FD3D12DepthStencilView* CurrentDepthStencilTarget;
	FD3D12TextureBase* CurrentDepthTexture;
	uint32 NumSimultaneousRenderTargets;

	/** Track the currently bound uniform buffers. */
	FD3D12UniformBuffer* BoundUniformBuffers[SF_NumStandardFrequencies][MAX_CBS];
	FUniformBufferRHIRef BoundUniformBufferRefs[SF_NumStandardFrequencies][MAX_CBS];

	/** Bit array to track which uniform buffers have changed since the last draw call. */
	uint16 DirtyUniformBuffers[SF_NumStandardFrequencies];

	/** Tracks the current depth stencil access type. */
	FExclusiveDepthStencil CurrentDSVAccessType;

	/** Handle for the dummy outer occlusion query we optionally insert for performance reasons */
	FRenderQueryRHIRef OuterOcclusionQuery;
	bool bOuterOcclusionQuerySubmitted;

	/** When a new graphics PSO is set, we discard all old constants set for the previous shader. */
	bool bDiscardSharedGraphicsConstants;

	/** When a new compute PSO is set, we discard all old constants set for the previous shader. */
	bool bDiscardSharedComputeConstants;

	/** Used by variable rate shading to cache the current state of the combiners and the constant shading rate*/
#if PLATFORM_SUPPORTS_VARIABLE_RATE_SHADING
	D3D12_SHADING_RATE_COMBINER		VRSCombiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT];
	D3D12_SHADING_RATE				VRSShadingRate;
#endif

	virtual void FlushMetadata(FRHITexture** InTextures, int32 NumTextures) {};

	D3D12_RESOURCE_STATES SkipFastClearEliminateState;
	D3D12_RESOURCE_STATES ValidResourceStates;

#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	bool bNeedFlushTextureCache;
	void InvalidateTextureCache() { bNeedFlushTextureCache = true; }
	inline void FlushTextureCacheIfNeeded()
	{
		if (bNeedFlushTextureCache)
		{
			FlushTextureCache();

			bNeedFlushTextureCache = false;
		}
	}
	virtual void FlushTextureCache() {};
#endif

	bool bIsDoingQuery = false;

	uint32 numPrimitives;
	uint32 numVertices;
	uint32 numDraws;
	uint32 numDispatches;
	uint32 numClears;
	uint32 numBarriers;
	uint32 numPendingBarriers;
	uint32 numCopies;
	uint32 numInitialResourceCopies;
	uint32 otherWorkCounter;

	uint32 GetTotalWorkCount() const
	{
		return numDraws + numDispatches + numClears + numBarriers + numPendingBarriers + numCopies + numInitialResourceCopies + otherWorkCounter;
	}

	bool HasDoneWork() const
	{
		return GetTotalWorkCount() > 0;
	}

	/** Constant buffers for Set*ShaderParameter calls. */
	FD3D12ConstantBuffer VSConstantBuffer;
	FD3D12ConstantBuffer MSConstantBuffer;
	FD3D12ConstantBuffer ASConstantBuffer;
	FD3D12ConstantBuffer PSConstantBuffer;
	FD3D12ConstantBuffer GSConstantBuffer;
	FD3D12ConstantBuffer CSConstantBuffer;

	/** needs to be called before each draw call */
	void CommitNonComputeShaderConstants();

	/** needs to be called before each dispatch call */
	void CommitComputeShaderConstants();

	template <class ShaderType> void SetResourcesFromTables(const ShaderType* RESTRICT);
	template <class ShaderType> uint32 SetUAVPSResourcesFromTables(const ShaderType* RESTRICT Shader);

	void CommitGraphicsResourceTables();
	void CommitComputeResourceTables(FD3D12ComputeShader* ComputeShader);

	void ValidateExclusiveDepthStencilAccess(FExclusiveDepthStencil Src) const;

	void CommitRenderTargetsAndUAVs();

	template<typename TPixelShader>
	void ResolveTextureUsingShader(
		FRHICommandList_RecursiveHazardous& RHICmdList,
		FD3D12Texture2D* SourceTexture,
		FD3D12Texture2D* DestTexture,
		FD3D12RenderTargetView* DestSurfaceRTV,
		FD3D12DepthStencilView* DestSurfaceDSV,
		const D3D12_RESOURCE_DESC& ResolveTargetDesc,
		const FResolveRect& SourceRect,
		const FResolveRect& DestRect,
		typename TPixelShader::FParameter PixelShaderParameter
		);

	virtual void SetDepthBounds(float MinDepth, float MaxDepth);
	virtual void SetShadingRate(EVRSShadingRate ShadingRate, EVRSRateCombiner Combiner);

	virtual void SetAsyncComputeBudgetInternal(EAsyncComputeBudget Budget) {}

	void RHIBeginTransitionsWithoutFencing(TArrayView<const FRHITransition*> Transitions);
	virtual void RHIBeginTransitions(TArrayView<const FRHITransition*> Transitions) final override;
	virtual void RHIEndTransitions(TArrayView<const FRHITransition*> Transitions) final override;

	// IRHIComputeContext interface
	virtual void RHISetComputeShader(FRHIComputeShader* ComputeShader) final override;
	virtual void RHISetComputePipelineState(FRHIComputePipelineState* ComputePipelineState) final override;
	virtual void RHIDispatchComputeShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) final override;
	virtual void RHIDispatchIndirectComputeShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override;
	virtual void RHISetStaticUniformBuffers(const FUniformBufferStaticBindings& InUniformBuffers) final override;
	virtual void RHISetShaderTexture(FRHIComputeShader* PixelShader, uint32 TextureIndex, FRHITexture* NewTexture) final override;
	virtual void RHISetShaderSampler(FRHIComputeShader* ComputeShader, uint32 SamplerIndex, FRHISamplerState* NewState) final override;
	virtual void RHISetUAVParameter(FRHIPixelShader* PixelShaderRHI, uint32 UAVIndex, FRHIUnorderedAccessView* UAVRHI) final override;
	virtual void RHISetUAVParameter(FRHIComputeShader* ComputeShader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV) final override;
	virtual void RHISetUAVParameter(FRHIComputeShader* ComputeShader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV, uint32 InitialCount) final override;
	virtual void RHISetShaderResourceViewParameter(FRHIComputeShader* ComputeShader, uint32 SamplerIndex, FRHIShaderResourceView* SRV) final override;
	virtual void RHISetShaderUniformBuffer(FRHIComputeShader* ComputeShader, uint32 BufferIndex, FRHIUniformBuffer* Buffer) final override;
	virtual void RHISetShaderParameter(FRHIComputeShader* ComputeShader, uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue) final override;
	virtual void RHIPushEvent(const TCHAR* Name, FColor Color) final override;
	virtual void RHIPopEvent() final override;
	virtual void RHISubmitCommandsHint() final override;

	// IRHICommandContext interface
	virtual void RHISetMultipleViewports(uint32 Count, const FViewportBounds* Data) final override;
	virtual void RHIClearUAVFloat(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FVector4f& Values) final override;
	virtual void RHIClearUAVUint(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FUintVector4& Values) final override;
	virtual void RHICopyToResolveTarget(FRHITexture* SourceTexture, FRHITexture* DestTexture, const FResolveParams& ResolveParams) override;
	virtual void RHICopyTexture(FRHITexture* SourceTexture, FRHITexture* DestTexture, const FRHICopyTextureInfo& CopyInfo) final override;
	virtual void RHICopyBufferRegion(FRHIBuffer* DestBuffer, uint64 DstOffset, FRHIBuffer* SourceBuffer, uint64 SrcOffset, uint64 NumBytes) final override;
	virtual void RHICopyToStagingBuffer(FRHIBuffer* SourceBuffer, FRHIStagingBuffer* DestinationStagingBuffer, uint32 Offset, uint32 NumBytes) final override;
	virtual void RHIWriteGPUFence(FRHIGPUFence* Fence) final override;
	virtual void RHIBeginRenderQuery(FRHIRenderQuery* RenderQuery) final override;
	virtual void RHIEndRenderQuery(FRHIRenderQuery* RenderQuery) final override;
	virtual void RHICalibrateTimers(FRHITimestampCalibrationQuery* CalibrationQuery) final override;
	void RHIBeginOcclusionQueryBatch(uint32 NumQueriesInBatch);
	void RHIEndOcclusionQueryBatch();
	virtual void RHIBeginScene() final override;
	virtual void RHIEndScene() final override;
	virtual void RHISetStreamSource(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) final override;
	virtual void RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) final override;
	virtual void RHISetScissorRect(bool bEnable, uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY) final override;
	virtual void RHISetGraphicsPipelineState(FRHIGraphicsPipelineState* GraphicsPipelineState, uint32 StencilRef, bool bApplyAdditionalState) final override;
	virtual void RHISetShaderTexture(FRHIGraphicsShader* Shader, uint32 TextureIndex, FRHITexture* NewTexture) final override;
	virtual void RHISetShaderSampler(FRHIGraphicsShader* Shader, uint32 SamplerIndex, FRHISamplerState* NewState) final override;
	virtual void RHISetShaderResourceViewParameter(FRHIGraphicsShader* Shader, uint32 SamplerIndex, FRHIShaderResourceView* SRV) final override;
	virtual void RHISetShaderUniformBuffer(FRHIGraphicsShader* Shader, uint32 BufferIndex, FRHIUniformBuffer* Buffer) final override;
	virtual void RHISetShaderParameter(FRHIGraphicsShader* Shader, uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue) final override;
	virtual void RHISetStencilRef(uint32 StencilRef) final override;
	virtual void RHISetBlendFactor(const FLinearColor& BlendFactor) final override;
	virtual void SetRenderTargets(uint32 NumSimultaneousRenderTargets, const FRHIRenderTargetView* NewRenderTargets, const FRHIDepthRenderTargetView* NewDepthStencilTarget) final override;
	void SetRenderTargetsAndClear(const FRHISetRenderTargetsInfo& RenderTargetsInfo);
	virtual void RHIDrawPrimitive(uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances) final override;
	virtual void RHIDrawPrimitiveIndirect(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override;
	virtual void RHIDrawIndexedIndirect(FRHIBuffer* IndexBufferRHI, FRHIBuffer* ArgumentsBufferRHI, int32 DrawArgumentsIndex, uint32 NumInstances) final override;
	virtual void RHIDrawIndexedPrimitive(FRHIBuffer* IndexBuffer, int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices, uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances) final override;
	virtual void RHIDrawIndexedPrimitiveIndirect(FRHIBuffer* IndexBuffer, FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override;
#if PLATFORM_SUPPORTS_MESH_SHADERS
	virtual void RHIDispatchMeshShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) final override;
	virtual void RHIDispatchIndirectMeshShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override;
#endif
	virtual void RHISetDepthBounds(float MinDepth, float MaxDepth) final override;
    virtual void RHISetShadingRate(EVRSShadingRate ShadingRate, EVRSRateCombiner Combiner) final override;
#if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING
	virtual void RHIBackBufferWaitTrackingBeginFrame(uint64 FrameToken, bool bDeferred) final override;
#endif // #if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING

	virtual void RHIClearMRTImpl(bool* bClearColorArray, int32 NumClearColors, const FLinearColor* ColorArray, bool bClearDepth, float Depth, bool bClearStencil, uint32 Stencil);


	virtual void RHIBeginRenderPass(const FRHIRenderPassInfo& InInfo, const TCHAR* InName)
	{
		FRHISetRenderTargetsInfo RTInfo;
		InInfo.ConvertToRenderTargetsInfo(RTInfo);
		SetRenderTargetsAndClear(RTInfo);

		RenderPassInfo = InInfo;

		if (InInfo.bOcclusionQueries)
		{
			RHIBeginOcclusionQueryBatch(InInfo.NumOcclusionQueries);
		}
	}

	virtual void RHIEndRenderPass()
	{
		if (RenderPassInfo.bOcclusionQueries)
		{
			RHIEndOcclusionQueryBatch();
		}

		for (int32 Index = 0; Index < MaxSimultaneousRenderTargets; ++Index)
		{
			if (!RenderPassInfo.ColorRenderTargets[Index].RenderTarget)
			{
				break;
			}
			if (RenderPassInfo.ColorRenderTargets[Index].ResolveTarget)
			{
				RHICopyToResolveTarget(RenderPassInfo.ColorRenderTargets[Index].RenderTarget, RenderPassInfo.ColorRenderTargets[Index].ResolveTarget, RenderPassInfo.ResolveParameters);
			}
		}

		if (RenderPassInfo.DepthStencilRenderTarget.DepthStencilTarget && RenderPassInfo.DepthStencilRenderTarget.ResolveTarget)
		{
			RHICopyToResolveTarget(RenderPassInfo.DepthStencilRenderTarget.DepthStencilTarget, RenderPassInfo.DepthStencilRenderTarget.ResolveTarget, RenderPassInfo.ResolveParameters);
		}

		FRHIRenderTargetView RTV(nullptr, ERenderTargetLoadAction::ENoAction);
		FRHIDepthRenderTargetView DepthRTV(nullptr, ERenderTargetLoadAction::ENoAction, ERenderTargetStoreAction::ENoAction);
		SetRenderTargets(1, &RTV, &DepthRTV);
	}

	// When using Alternate Frame Rendering some temporal effects i.e. effects which consume GPU work from previous frames must synchronize their resources
	// to prevent visual corruption.

	// This should be called right before the effect consumes it's temporal resources.
	virtual void RHIWaitForTemporalEffect(const FName& InEffectName) final AFR_API_OVERRIDE;

	// This should be called right after the effect generates the resources which will be used in subsequent frame(s).
	virtual void RHIBroadcastTemporalEffect(const FName& InEffectName, const TArrayView<FRHITexture*> InTextures) final AFR_API_OVERRIDE;
	virtual void RHIBroadcastTemporalEffect(const FName& InEffectName, const TArrayView<FRHIBuffer*> InBuffers) final AFR_API_OVERRIDE;

#if D3D12_RHI_RAYTRACING
	virtual void RHICopyBufferRegions(const TArrayView<const FCopyBufferRegionParams> Params) final override;
	virtual void RHIBindAccelerationStructureMemory(FRHIRayTracingScene* Scene, FRHIBuffer* Buffer, uint32 BufferOffset) final override;
	virtual void BuildAccelerationStructuresInternal(const TArrayView<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC> BuildDesc);
#if WITH_MGPU
	// Should be called before RHIBuildAccelerationStructures when multiple GPU support is present (for example, from FD3D12CommandContextRedirector::RHIBuildAccelerationStructures)
	static void UnregisterAccelerationStructuresInternalMGPU(const TArrayView<const FRayTracingGeometryBuildParams> Params, FRHIGPUMask GPUMask);
#endif
	virtual void RHIBuildAccelerationStructures(const TArrayView<const FRayTracingGeometryBuildParams> Params, const FRHIBufferRange& ScratchBufferRange) final override;
	virtual void RHIBuildAccelerationStructure(const FRayTracingSceneBuildParams& SceneBuildParams) final override;
	virtual void RHIClearRayTracingBindings(FRHIRayTracingScene* Scene) final override;
	virtual void RHIRayTraceOcclusion(FRHIRayTracingScene* Scene,
		FRHIShaderResourceView* Rays,
		FRHIUnorderedAccessView* Output,
		uint32 NumRays) final override;
	virtual void RHIRayTraceIntersection(FRHIRayTracingScene* Scene,
		FRHIShaderResourceView* Rays,
		FRHIUnorderedAccessView* Output,
		uint32 NumRays) final override;
	virtual void RHIRayTraceDispatch(FRHIRayTracingPipelineState* RayTracingPipelineState, FRHIRayTracingShader* RayGenShader,
		FRHIRayTracingScene* Scene,
		const FRayTracingShaderBindings& GlobalResourceBindings,
		uint32 Width, uint32 Height) final override;
	virtual void RHIRayTraceDispatchIndirect(FRHIRayTracingPipelineState* RayTracingPipelineState, FRHIRayTracingShader* RayGenShader,
		FRHIRayTracingScene* Scene,
		const FRayTracingShaderBindings& GlobalResourceBindings,
		FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override;
	virtual void RHISetRayTracingHitGroups(
		FRHIRayTracingScene* Scene, FRHIRayTracingPipelineState* Pipeline,
		uint32 NumBindings, const FRayTracingLocalShaderBindings* Bindings) final override;
	virtual void RHISetRayTracingHitGroup(
		FRHIRayTracingScene* Scene, uint32 InstanceIndex, uint32 SegmentIndex, uint32 ShaderSlot,
		FRHIRayTracingPipelineState* Pipeline, uint32 HitGroupIndex,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 LooseParameterDataSize, const void* LooseParameterData,
		uint32 UserData) final override;
	virtual void RHISetRayTracingCallableShader(
		FRHIRayTracingScene* Scene, uint32 ShaderSlotInScene,
		FRHIRayTracingPipelineState* Pipeline, uint32 ShaderIndexInPipeline,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 UserData) final override;
	virtual void RHISetRayTracingMissShader(
		FRHIRayTracingScene* Scene, uint32 ShaderSlotInScene,
		FRHIRayTracingPipelineState* Pipeline, uint32 ShaderIndexInPipeline,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 UserData) final override;
#endif // D3D12_RHI_RAYTRACING

	template<typename ObjectType, typename RHIType>
	static FORCEINLINE_DEBUGGABLE ObjectType* RetrieveObject(RHIType RHIObject, uint32 GPUIndex)
	{
		return FD3D12DynamicRHI::ResourceCast(RHIObject, GPUIndex);
	}

	template<typename ObjectType, typename RHIType>
	FORCEINLINE_DEBUGGABLE ObjectType* RetrieveObject(RHIType RHIObject)
	{
		return RetrieveObject<ObjectType, RHIType>(RHIObject, GetGPUIndex());
	}

	static inline FD3D12TextureBase* RetrieveTextureBase(FRHITexture* Texture, uint32 GPUIndex)
	{
		FD3D12TextureBase* RHITexture = GetD3D12TextureFromRHITexture(Texture);
		return RHITexture ? RHITexture->GetLinkedObject(GPUIndex) : nullptr;
	}

	FORCEINLINE_DEBUGGABLE FD3D12TextureBase* RetrieveTextureBase(FRHITexture* Texture)
	{
		return RetrieveTextureBase(Texture, GetGPUIndex());
	}

	uint32 GetGPUIndex() const { return GPUMask.ToIndex(); }

	virtual void RHISetGPUMask(FRHIGPUMask InGPUMask) final override
	{
		// This is a single-GPU context so it doesn't make sense to ever change its GPU
		// mask. If multiple GPUs are supported we should be using the redirector context.
		ensure(InGPUMask == GPUMask);
	}

protected:

	FD3D12CommandContext* GetContext(uint32 InGPUIndex) final override 
	{  
		return InGPUIndex == GetGPUIndex() ? this : nullptr; 
	}
	
	void WriteGPUEventStackToBreadCrumbData(bool bBeginEvent);

private:

	template <typename TD3D12Resource, typename TCopyFunction>
	void RHIBroadcastTemporalEffect(const FName& InEffectName, const TArrayView<TD3D12Resource*> InResources, const TCopyFunction& InCopyFunction);

	void RHIClearMRT(bool* bClearColorArray, int32 NumClearColors, const FLinearColor* ColorArray, bool bClearDepth, float Depth, bool bClearStencil, uint32 Stencil);

	static void ClearUAV(TRHICommandList_RecursiveHazardous<FD3D12CommandContext>& RHICmdList, FD3D12UnorderedAccessView* UAV, const void* ClearValues, bool bFloat);

	template <typename TRHIShader>
	void ApplyStaticUniformBuffers(TRHIShader* Shader)
	{
		if (Shader)
		{
			UE::RHICore::ApplyStaticUniformBuffers(this, Shader, Shader->StaticSlots, Shader->ShaderResourceTable.ResourceTableLayoutHashes, GlobalUniformBuffers);
		}
	}

	TArray<FRHIUniformBuffer*> GlobalUniformBuffers;
};
PRAGMA_ENABLE_DEPRECATION_WARNINGS

// This class is a shim to get AFR working. Currently the upper engine only queries for the 'Immediate Context'
// once. However when in AFR we need to switch which context is active every frame so we return an instance of this class
// as the default context so that we can control when to swap which device we talk to.
// Because IRHICommandContext is pure virtual we can return the normal FD3D12CommandContext when not using mGPU thus there
// is no additional overhead for the common case i.e. 1 GPU.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
class FD3D12CommandContextRedirector final : public FD3D12CommandContextBase
{
public:
	FD3D12CommandContextRedirector(class FD3D12Adapter* InParent, ED3D12CommandQueueType InCommandQueueType, bool InIsDefaultContext);

#define ContextRedirect(Call) { for (uint32 GPUIndex : GPUMask) PhysicalContexts[GPUIndex]->##Call; }
#define ContextGPU0(Call) { PhysicalContexts[0]->##Call; }

	FORCEINLINE virtual void RHISetComputeShader(FRHIComputeShader* ComputeShader) final override
	{
		ContextRedirect(RHISetComputeShader(ComputeShader));
	}
	FORCEINLINE virtual void RHISetComputePipelineState(FRHIComputePipelineState* ComputePipelineState) final override
	{
		ContextRedirect(RHISetComputePipelineState(ComputePipelineState));
	}
	FORCEINLINE virtual void RHIDispatchComputeShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) final override
	{
		ContextRedirect(RHIDispatchComputeShader(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ));
	}
	FORCEINLINE virtual void RHIDispatchIndirectComputeShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override
	{
		ContextRedirect(RHIDispatchIndirectComputeShader(ArgumentBuffer, ArgumentOffset));
	}

	// Special implementation that only signal the fence once.
	virtual void RHIBeginTransitions(TArrayView<const FRHITransition*> Transitions) final override;
	virtual void RHIEndTransitions(TArrayView<const FRHITransition*> Transitions) final override;

	virtual void RHITransferResources(const TArrayView<const FTransferResourceParams> Params) final override;

	FORCEINLINE virtual void RHICopyToStagingBuffer(FRHIBuffer* SourceBuffer, FRHIStagingBuffer* DestinationStagingBuffer, uint32 Offset, uint32 NumBytes) final override
	{
		ContextRedirect(RHICopyToStagingBuffer(SourceBuffer, DestinationStagingBuffer, Offset, NumBytes));
	}
	FORCEINLINE virtual void RHIWriteGPUFence(FRHIGPUFence* Fence) final override
	{
		ContextRedirect(RHIWriteGPUFence(Fence));
	}
	FORCEINLINE virtual void RHISetShaderTexture(FRHIComputeShader* PixelShader, uint32 TextureIndex, FRHITexture* NewTexture) final override
	{
		ContextRedirect(RHISetShaderTexture(PixelShader, TextureIndex, NewTexture));
	}
	FORCEINLINE virtual void RHISetShaderSampler(FRHIComputeShader* ComputeShader, uint32 SamplerIndex, FRHISamplerState* NewState) final override
	{
		ContextRedirect(RHISetShaderSampler(ComputeShader, SamplerIndex, NewState));
	}
	FORCEINLINE virtual void RHISetUAVParameter(FRHIPixelShader* PixelShader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV) final override
	{
		ContextRedirect(RHISetUAVParameter(PixelShader, UAVIndex, UAV));
	}
	FORCEINLINE virtual void RHISetUAVParameter(FRHIComputeShader* ComputeShader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV) final override
	{
		ContextRedirect(RHISetUAVParameter(ComputeShader, UAVIndex, UAV));
	}
	FORCEINLINE virtual void RHISetUAVParameter(FRHIComputeShader* ComputeShader, uint32 UAVIndex, FRHIUnorderedAccessView* UAV, uint32 InitialCount) final override
	{
		ContextRedirect(RHISetUAVParameter(ComputeShader, UAVIndex, UAV, InitialCount));
	}
	FORCEINLINE virtual void RHISetShaderResourceViewParameter(FRHIComputeShader* ComputeShader, uint32 SamplerIndex, FRHIShaderResourceView* SRV) final override
	{
		ContextRedirect(RHISetShaderResourceViewParameter(ComputeShader, SamplerIndex, SRV));
	}
	FORCEINLINE virtual void RHISetShaderUniformBuffer(FRHIComputeShader* ComputeShader, uint32 BufferIndex, FRHIUniformBuffer* Buffer) final override
	{
		ContextRedirect(RHISetShaderUniformBuffer(ComputeShader, BufferIndex, Buffer));
	}
	FORCEINLINE virtual void RHISetShaderParameter(FRHIComputeShader* ComputeShader, uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue) final override
	{
		ContextRedirect(RHISetShaderParameter(ComputeShader, BufferIndex, BaseIndex, NumBytes, NewValue));
	}
	FORCEINLINE virtual void RHIPushEvent(const TCHAR* Name, FColor Color) final override
	{
		ContextRedirect(RHIPushEvent(Name, Color));
	}
	FORCEINLINE virtual void RHIPopEvent() final override
	{
		ContextRedirect(RHIPopEvent());
	}
	FORCEINLINE virtual void RHISubmitCommandsHint() final override
	{
		ContextRedirect(RHISubmitCommandsHint());
	}

	// IRHICommandContext interface
	FORCEINLINE virtual void RHISetMultipleViewports(uint32 Count, const FViewportBounds* Data) final override
	{
		ContextRedirect(RHISetMultipleViewports(Count, Data));
	}
	FORCEINLINE virtual void RHIClearUAVFloat(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FVector4f& Values) final override
	{
		ContextRedirect(RHIClearUAVFloat(UnorderedAccessViewRHI, Values));
	}
	FORCEINLINE virtual void RHIClearUAVUint(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FUintVector4& Values) final override
	{
		ContextRedirect(RHIClearUAVUint(UnorderedAccessViewRHI, Values));
	}
	FORCEINLINE virtual void RHICopyToResolveTarget(FRHITexture* SourceTexture, FRHITexture* DestTexture, const FResolveParams& ResolveParams) final override
	{
		ContextRedirect(RHICopyToResolveTarget(SourceTexture, DestTexture, ResolveParams));
	}
	FORCEINLINE virtual void RHICopyTexture(FRHITexture* SourceTextureRHI, FRHITexture* DestTextureRHI, const FRHICopyTextureInfo& CopyInfo) final override
	{
		ContextRedirect(RHICopyTexture(SourceTextureRHI, DestTextureRHI, CopyInfo));
	}
	FORCEINLINE virtual void RHICopyBufferRegion(FRHIBuffer* DestBuffer, uint64 DstOffset, FRHIBuffer* SourceBuffer, uint64 SrcOffset, uint64 NumBytes) final override
	{
		ContextRedirect(RHICopyBufferRegion(DestBuffer, DstOffset, SourceBuffer, SrcOffset, NumBytes));
	}
	FORCEINLINE virtual void RHIBeginRenderQuery(FRHIRenderQuery* RenderQuery) final override
	{
		ContextRedirect(RHIBeginRenderQuery(RenderQuery));
	}
	FORCEINLINE virtual void RHIEndRenderQuery(FRHIRenderQuery* RenderQuery) final override
	{
		ContextRedirect(RHIEndRenderQuery(RenderQuery));
	}
	FORCEINLINE virtual void RHICalibrateTimers(FRHITimestampCalibrationQuery* CalibrationQuery) final override
	{
		ContextRedirect(RHICalibrateTimers(CalibrationQuery));
	}
	FORCEINLINE virtual void RHIBeginScene() final override
	{
		ContextRedirect(RHIBeginScene());
	}
	FORCEINLINE virtual void RHIEndScene() final override
	{
		ContextRedirect(RHIEndScene());
	}
	FORCEINLINE virtual void RHISetStreamSource(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) final override
	{
		ContextRedirect(RHISetStreamSource(StreamIndex, VertexBuffer, Offset));
	}
	FORCEINLINE virtual void RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) final override
	{
		ContextRedirect(RHISetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ));
	}
	FORCEINLINE virtual void RHISetScissorRect(bool bEnable, uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY) final override
	{
		ContextRedirect(RHISetScissorRect(bEnable, MinX, MinY, MaxX, MaxY));
	}
	FORCEINLINE void RHISetGraphicsPipelineState(FRHIGraphicsPipelineState* GraphicsPipelineState, uint32 StencilRef, bool bApplyAdditionalState) final override
	{
		ContextRedirect(RHISetGraphicsPipelineState(GraphicsPipelineState, StencilRef, bApplyAdditionalState));
	}
	FORCEINLINE virtual void RHISetShaderTexture(FRHIGraphicsShader* Shader, uint32 TextureIndex, FRHITexture* NewTexture) final override
	{
		ContextRedirect(RHISetShaderTexture(Shader, TextureIndex, NewTexture));
	}
	FORCEINLINE virtual void RHISetShaderSampler(FRHIGraphicsShader* Shader, uint32 SamplerIndex, FRHISamplerState* NewState) final override
	{
		ContextRedirect(RHISetShaderSampler(Shader, SamplerIndex, NewState));
	}
	FORCEINLINE virtual void RHISetShaderResourceViewParameter(FRHIGraphicsShader* Shader, uint32 SamplerIndex, FRHIShaderResourceView* SRV) final override
	{
		ContextRedirect(RHISetShaderResourceViewParameter(Shader, SamplerIndex, SRV));
	}
	FORCEINLINE virtual void RHISetStaticUniformBuffers(const FUniformBufferStaticBindings& InUniformBuffers) final override
	{
		ContextRedirect(RHISetStaticUniformBuffers(InUniformBuffers));
	}
	FORCEINLINE virtual void RHISetShaderUniformBuffer(FRHIGraphicsShader* Shader, uint32 BufferIndex, FRHIUniformBuffer* Buffer) final override
	{
		ContextRedirect(RHISetShaderUniformBuffer(Shader, BufferIndex, Buffer));
	}
	FORCEINLINE virtual void RHISetShaderParameter(FRHIGraphicsShader* Shader, uint32 BufferIndex, uint32 BaseIndex, uint32 NumBytes, const void* NewValue) final override
	{
		ContextRedirect(RHISetShaderParameter(Shader, BufferIndex, BaseIndex, NumBytes, NewValue));
	}
	FORCEINLINE virtual void RHISetStencilRef(uint32 StencilRef) final override
	{
		ContextRedirect(RHISetStencilRef(StencilRef));
	}
	FORCEINLINE void RHISetBlendFactor(const FLinearColor& BlendFactor) final override
	{
		ContextRedirect(RHISetBlendFactor(BlendFactor));
	}
	FORCEINLINE virtual void SetRenderTargets(uint32 NumSimultaneousRenderTargets, const FRHIRenderTargetView* NewRenderTargets, const FRHIDepthRenderTargetView* NewDepthStencilTarget) final override
	{
		ContextRedirect(SetRenderTargets(NumSimultaneousRenderTargets, NewRenderTargets, NewDepthStencilTarget));
	}
	FORCEINLINE void SetRenderTargetsAndClear(const FRHISetRenderTargetsInfo& RenderTargetsInfo)
	{
		ContextRedirect(SetRenderTargetsAndClear(RenderTargetsInfo));
	}
	FORCEINLINE virtual void RHIDrawPrimitive(uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances) final override
	{
		ContextRedirect(RHIDrawPrimitive(BaseVertexIndex, NumPrimitives, NumInstances));
	}
	FORCEINLINE virtual void RHIDrawPrimitiveIndirect(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override
	{
		ContextRedirect(RHIDrawPrimitiveIndirect(ArgumentBuffer, ArgumentOffset));
	}
	FORCEINLINE virtual void RHIDrawIndexedIndirect(FRHIBuffer* IndexBufferRHI, FRHIBuffer* ArgumentsBufferRHI, int32 DrawArgumentsIndex, uint32 NumInstances) final override
	{
		ContextRedirect(RHIDrawIndexedIndirect(IndexBufferRHI, ArgumentsBufferRHI, DrawArgumentsIndex, NumInstances));
	}
	FORCEINLINE virtual void RHIDrawIndexedPrimitive(FRHIBuffer* IndexBuffer, int32 BaseVertexIndex, uint32 FirstInstance, uint32 NumVertices, uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances) final override
	{
		ContextRedirect(RHIDrawIndexedPrimitive(IndexBuffer, BaseVertexIndex, FirstInstance, NumVertices, StartIndex, NumPrimitives, NumInstances));
	}
	FORCEINLINE virtual void RHIDrawIndexedPrimitiveIndirect(FRHIBuffer* IndexBuffer, FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override
	{
		ContextRedirect(RHIDrawIndexedPrimitiveIndirect(IndexBuffer, ArgumentBuffer, ArgumentOffset));
	}
#if PLATFORM_SUPPORTS_MESH_SHADERS
	FORCEINLINE virtual void RHIDispatchMeshShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY, uint32 ThreadGroupCountZ) final override
	{
		ContextRedirect(RHIDispatchMeshShader(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ));
	}
	FORCEINLINE virtual void RHIDispatchIndirectMeshShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override
	{
		ContextRedirect(RHIDispatchIndirectMeshShader(ArgumentBuffer, ArgumentOffset));
	}
#endif
	FORCEINLINE virtual void RHISetDepthBounds(float MinDepth, float MaxDepth) final override
	{
		ContextRedirect(RHISetDepthBounds(MinDepth, MaxDepth));
	}
	
	FORCEINLINE virtual void RHISetShadingRate(EVRSShadingRate ShadingRate, EVRSRateCombiner Combiner) final override
	{
		ContextRedirect(RHISetShadingRate(ShadingRate, Combiner));
	}

	FORCEINLINE virtual void RHIWaitForTemporalEffect(const FName& InEffectName) final AFR_API_OVERRIDE
	{
		ContextRedirect(RHIWaitForTemporalEffect(InEffectName));
	}

	FORCEINLINE virtual void RHIBroadcastTemporalEffect(const FName& InEffectName, const TArrayView<FRHITexture*> InTextures) final AFR_API_OVERRIDE
	{
		ContextRedirect(RHIBroadcastTemporalEffect(InEffectName, InTextures));
	}

	FORCEINLINE virtual void RHIBroadcastTemporalEffect(const FName& InEffectName, const TArrayView<FRHIBuffer*> InBuffers) final AFR_API_OVERRIDE
	{
		ContextRedirect(RHIBroadcastTemporalEffect(InEffectName, InBuffers));
	}

	virtual void RHIBeginRenderPass(const FRHIRenderPassInfo& InInfo, const TCHAR* InName) final override
	{
		ContextRedirect(RHIBeginRenderPass(InInfo, InName));
	}

	virtual void RHIEndRenderPass() final override
	{
		ContextRedirect(RHIEndRenderPass());
	}

	virtual void RHIBuildAccelerationStructures(const TArrayView<const FRayTracingGeometryBuildParams> Params, const FRHIBufferRange& ScratchBufferRange) final override
	{
#if WITH_MGPU
		FD3D12CommandContext::UnregisterAccelerationStructuresInternalMGPU(Params, GPUMask);
#endif 

		ContextRedirect(RHIBuildAccelerationStructures(Params, ScratchBufferRange));
	}

	virtual void RHIBuildAccelerationStructure(const FRayTracingSceneBuildParams& SceneBuildParams) final override
	{
		ContextRedirect(RHIBuildAccelerationStructure(SceneBuildParams));
	}

	virtual void RHIRayTraceOcclusion(FRHIRayTracingScene* Scene,
		FRHIShaderResourceView* Rays,
		FRHIUnorderedAccessView* Output,
		uint32 NumRays) final override
	{
		ContextRedirect(RHIRayTraceOcclusion(Scene, Rays, Output, NumRays));
	}

	virtual void RHIRayTraceIntersection(FRHIRayTracingScene* Scene,
		FRHIShaderResourceView* Rays,
		FRHIUnorderedAccessView* Output,
		uint32 NumRays) final override
	{
		ContextRedirect(RHIRayTraceIntersection(Scene, Rays, Output, NumRays));
	}

	virtual void RHIRayTraceDispatch(FRHIRayTracingPipelineState* RayTracingPipelineState, FRHIRayTracingShader* RayGenShader,
		FRHIRayTracingScene* Scene,
		const FRayTracingShaderBindings& GlobalResourceBindings,
		uint32 Width, uint32 Height) final override
	{
		ContextRedirect(RHIRayTraceDispatch(RayTracingPipelineState, RayGenShader, Scene, GlobalResourceBindings, Width, Height));
	}

	virtual void RHIRayTraceDispatchIndirect(FRHIRayTracingPipelineState* RayTracingPipelineState, FRHIRayTracingShader* RayGenShader,
		FRHIRayTracingScene* Scene,
		const FRayTracingShaderBindings& GlobalResourceBindings,
		FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset) final override
	{
		ContextRedirect(RHIRayTraceDispatchIndirect(RayTracingPipelineState, RayGenShader, Scene, GlobalResourceBindings, ArgumentBuffer, ArgumentOffset));
	}

	virtual void RHISetRayTracingHitGroup(
		FRHIRayTracingScene* Scene, uint32 InstanceIndex, uint32 SegmentIndex, uint32 ShaderSlot,
		FRHIRayTracingPipelineState* Pipeline, uint32 HitGroupIndex,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 LooseParameterDataSize, const void* LooseParameterData,
		uint32 UserData) final override
	{
		ContextRedirect(RHISetRayTracingHitGroup(Scene, InstanceIndex, SegmentIndex, ShaderSlot, Pipeline, HitGroupIndex, NumUniformBuffers, UniformBuffers, LooseParameterDataSize, LooseParameterData, UserData));
	}

	virtual void RHISetRayTracingHitGroups(FRHIRayTracingScene* Scene, FRHIRayTracingPipelineState* Pipeline, uint32 NumBindings, const FRayTracingLocalShaderBindings* Bindings) final override
	{
		ContextRedirect(RHISetRayTracingHitGroups(Scene, Pipeline, NumBindings, Bindings));
	}

	virtual void RHISetRayTracingCallableShader(
		FRHIRayTracingScene* Scene, uint32 ShaderSlotInScene,
		FRHIRayTracingPipelineState* Pipeline, uint32 ShaderIndexInPipeline,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 UserData) final override
	{
		ContextRedirect(RHISetRayTracingCallableShader(Scene, ShaderSlotInScene, Pipeline, ShaderIndexInPipeline, NumUniformBuffers, UniformBuffers, UserData));
	}

	virtual void RHISetRayTracingMissShader(
		FRHIRayTracingScene* Scene, uint32 ShaderSlotInScene,
		FRHIRayTracingPipelineState* Pipeline, uint32 ShaderIndexInPipeline,
		uint32 NumUniformBuffers, FRHIUniformBuffer* const* UniformBuffers,
		uint32 UserData) final override
	{
		ContextRedirect(RHISetRayTracingMissShader(Scene, ShaderSlotInScene, Pipeline, ShaderIndexInPipeline, NumUniformBuffers, UniformBuffers, UserData));
	}

	virtual void RHISetGPUMask(FRHIGPUMask InGPUMask) final override
	{
		GPUMask = InGPUMask;
		check(PhysicalGPUMask.ContainsAll(GPUMask));
	}

	virtual FRHIGPUMask RHIGetGPUMask() const final override
	{
		return GPUMask;
	}

	// Sets the mask of which GPUs can be supported, as opposed to the currently active
	// set. RHISetGPUMask checks that the active mask is a subset of the physical mask.
	FORCEINLINE void SetPhysicalGPUMask(FRHIGPUMask InGPUMask)
	{
		PhysicalGPUMask = InGPUMask;
	}

	virtual void RHIClearRayTracingBindings(FRHIRayTracingScene* Scene) final override
	{
		ContextRedirect(RHIClearRayTracingBindings(Scene));
	}

	virtual void RHIBindAccelerationStructureMemory(FRHIRayTracingScene* Scene, FRHIBuffer* Buffer, uint32 BufferOffset) final override
	{
		ContextRedirect(RHIBindAccelerationStructureMemory(Scene, Buffer, BufferOffset));
	}

#if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING
	virtual void RHIBackBufferWaitTrackingBeginFrame(uint64 FrameToken, bool bDeferred) final override
	{
		ContextRedirect(RHIBackBufferWaitTrackingBeginFrame(FrameToken, bDeferred));
	}
#endif // #if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING

	FORCEINLINE void SetPhysicalContext(FD3D12CommandContext* Context)
	{
		check(Context);
		const uint32 GPUIndex = Context->GetGPUIndex();
		check(PhysicalGPUMask.Contains(GPUIndex));
		PhysicalContexts[GPUIndex] = Context;
	}

	FORCEINLINE FD3D12CommandContext* GetContext(uint32 GPUIndex) final override
	{
		return PhysicalContexts[GPUIndex];
	}

#if ENABLE_RHI_VALIDATION || WITH_MGPU
	IRHIComputeContext& GetLowestLevelContext() final override
	{
		return *PhysicalContexts[0];
	}
#endif

private:

	// Make every GPU in the provided mask to wait on one another.
	void RHIMultiGPULockstep(FRHIGPUMask InGPUMask);

	FRHIGPUMask PhysicalGPUMask;
	FD3D12CommandContext* PhysicalContexts[MAX_NUM_GPUS];
};
PRAGMA_ENABLE_DEPRECATION_WARNINGS


class FD3D12TemporalEffect : public FD3D12AdapterChild
{
public:
	FD3D12TemporalEffect(FD3D12Adapter* Parent, const FName& InEffectName);

	void Init();
	void Destroy();

	bool ShouldWaitForPrevious(uint32 GPUIndex) const;
	void WaitForPrevious(uint32 GPUIndex, ED3D12CommandQueueType InQueueType);
	void SignalSyncComplete(uint32 GPUIndex, ED3D12CommandQueueType InQueueType);

private:
	FName EffectName;
	struct FCrossGPUFence
	{
		FCrossGPUFence(FRHIGPUMask InGPUMask, uint64 InLastSignaledFence, FD3D12FenceCore* InFenceCore)
			: GPUMask(InGPUMask)
			, LastSignaledFence(InLastSignaledFence)
			, LastWaitedFence(InLastSignaledFence)
			, FenceCore(InFenceCore)
		{}
		FRHIGPUMask GPUMask;
		uint64 LastSignaledFence;
		uint64 LastWaitedFence;
		FD3D12FenceCore* FenceCore;
	};
	TArray<FCrossGPUFence> EffectFences;

	const FCrossGPUFence* GetFenceForGPU(uint32 GPUIndex) const
	{
		return const_cast<FD3D12TemporalEffect*>(this)->GetFenceForGPU(GPUIndex);
	}

	FCrossGPUFence* GetFenceForGPU(uint32 GPUIndex)
	{
		return EffectFences.FindByPredicate([GPUIndex](const auto& Other) { return Other.GPUMask.Contains(GPUIndex); });
	}
};

struct FD3D12TransitionData
{
	ERHIPipeline SrcPipelines, DstPipelines;
	ERHITransitionCreateFlags CreateFlags = ERHITransitionCreateFlags::None;

	TArray<FRHITransitionInfo, TInlineAllocator<4>> TransitionInfos;
	TArray<FRHITransientAliasingInfo, TInlineAllocator<4>> AliasingInfos;
	TArray<FRHITransientAliasingOverlap, TInlineAllocator<4>> AliasingOverlaps;
	TRefCountPtr<FD3D12Fence> Fence;

	bool bCrossPipeline = false;
};