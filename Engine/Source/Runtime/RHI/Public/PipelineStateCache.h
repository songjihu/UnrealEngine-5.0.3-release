// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PipelineStateCache.h: Pipeline state cache definition.
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "Misc/EnumClassFlags.h"

class FComputePipelineState;
class FGraphicsPipelineState;
class FRayTracingPipelineState;

// Utility flags for modifying render target behavior on a PSO
enum class EApplyRendertargetOption : int
{
	DoNothing = 0,			// Just use the PSO from initializer's values, no checking and no modifying (used for PSO precompilation only)
	CheckApply = 1 << 0,	// Verify that the PSO's RT formats match the last Render Target formats set into the CmdList
	ForceApply = CheckApply,// Deprecated. Do not use
};

ENUM_CLASS_FLAGS(EApplyRendertargetOption);

enum class ERayTracingPipelineCacheFlags : int
{
	// Query the pipeline cache, create pipeline if necessary.
	// Compilation may happen on a task, but RHIThread will block on it before translating the RHICmdList.
	// Therefore the RHIThread may stall when creating large / complex pipelines.
	Default = 0,

	// Query the pipeline cache, create a background task to create the pipeline if necessary.
	// GetAndOrCreateRayTracingPipelineState() may return NULL if pipeline is not ready.
	// Caller must use an alternative fallback PSO to render current frame and may retry next frame.
	// Pipeline creation task will not block RenderThread or RHIThread, allowing hitch-free rendering.
	NonBlocking = 1 << 0,
};
ENUM_CLASS_FLAGS(ERayTracingPipelineCacheFlags);

extern RHI_API void SetComputePipelineState(FRHIComputeCommandList& RHICmdList, FRHIComputeShader* ComputeShader);
extern RHI_API void SetGraphicsPipelineState(FRHICommandList& RHICmdList, const FGraphicsPipelineStateInitializer& Initializer, uint32 StencilRef, EApplyRendertargetOption ApplyFlags = EApplyRendertargetOption::CheckApply, bool bApplyAdditionalState = true);

UE_DEPRECATED(5.0, "SetGraphicsPipelineState now requires a StencilRef argument and EApplyRendertargetOption::ForceApply will soon be removed")
inline void SetGraphicsPipelineState(FRHICommandList& RHICmdList, const FGraphicsPipelineStateInitializer& Initializer, EApplyRendertargetOption ApplyFlags = EApplyRendertargetOption::CheckApply, bool bApplyAdditionalState = true)
{
	SetGraphicsPipelineState(RHICmdList, Initializer, 0, ApplyFlags, bApplyAdditionalState);
}

namespace PipelineStateCache
{
	extern RHI_API uint64					RetrieveGraphicsPipelineStateSortKey(const FGraphicsPipelineState* GraphicsPipelineState);

	extern RHI_API FComputePipelineState*	GetAndOrCreateComputePipelineState(FRHIComputeCommandList& RHICmdList, FRHIComputeShader* ComputeShader, bool bFromFileCache);

	extern RHI_API FGraphicsPipelineState*	GetAndOrCreateGraphicsPipelineState(FRHICommandList& RHICmdList, const FGraphicsPipelineStateInitializer& OriginalInitializer, EApplyRendertargetOption ApplyFlags);

	extern RHI_API FRHIVertexDeclaration*	GetOrCreateVertexDeclaration(const FVertexDeclarationElementList& Elements);

	// Retrieves RTPSO object from cache or adds a task to create it, which will be waited on by RHI thread.
	// May return NULL in non-blocking mode if pipeline is not already in cache.
	extern RHI_API FRayTracingPipelineState* GetAndOrCreateRayTracingPipelineState(
		FRHICommandList& RHICmdList,
		const FRayTracingPipelineStateInitializer& Initializer,
		ERayTracingPipelineCacheFlags Flags = ERayTracingPipelineCacheFlags::Default);

	// Retrieves RTPSO object from cache or returns NULL if it's not found.
	extern RHI_API FRayTracingPipelineState* GetRayTracingPipelineState(const FRayTracingPipelineStateSignature& Signature);

	/* Evicts unused state entries based on r.pso.evictiontime time. Called in RHICommandList::BeginFrame */
	extern RHI_API void FlushResources();

	extern RHI_API void ReportFrameHitchToCSV();

	/* Clears all pipeline cached state. Called on shutdown, calling GetAndOrCreate after this will recreate state */
	extern RHI_API void Shutdown();
}

// Returns the hit group index within the ray tracing pipeline or INDEX_NONE if given shader does not exist.
// Asserts if shader is not found but bRequired is true.
extern RHI_API int32 FindRayTracingHitGroupIndex(FRayTracingPipelineState* Pipeline, FRHIRayTracingShader* HitGroupShader, bool bRequired = true);

