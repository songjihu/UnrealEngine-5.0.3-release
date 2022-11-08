// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RaytracingOptions.h declares ray tracing options for use in rendering
=============================================================================*/

#pragma once

#include "RHIDefinitions.h"

class FSkyLightSceneProxy;
class FViewInfo;
class FLightSceneInfoCompact;
class FLightSceneInfo;

// be sure to also update the definition in the `RayTracingPrimaryRays.usf`
enum class ERayTracingPrimaryRaysFlag: uint32 
{
	None                      =      0,
	UseGBufferForMaxDistance  = 1 << 0,
	ConsiderSurfaceScatter	  = 1 << 1,
	AllowSkipSkySample		  = 1 << 2,
};

ENUM_CLASS_FLAGS(ERayTracingPrimaryRaysFlag);

struct FRayTracingPrimaryRaysOptions
{
	bool bEnabled;
	int32 SamplerPerPixel;
	int32 ApplyHeightFog;
	float PrimaryRayBias;
	float MaxRoughness;
	int32 MaxRefractionRays;
	int32 EnableEmmissiveAndIndirectLighting;
	int32 EnableDirectLighting;
	int32 EnableShadows;
	float MinRayDistance;
	float MaxRayDistance;
	int32 EnableRefraction;
};

enum class ERayTracingPipelineCompatibilityFlags
{
	// Rendering feature can use the full ray tracing pipeline, with raygen, hit and miss shaders.
	FullPipeline = 1 << 0,

	// Rendering feature can use inline ray tracing
	Inline  = 1 << 1,
};
ENUM_CLASS_FLAGS(ERayTracingPipelineCompatibilityFlags);


#if RHI_RAYTRACING

// Whether a particular effect should be used, taking into account debug override
extern bool ShouldRenderRayTracingEffect(bool bEffectEnabled, ERayTracingPipelineCompatibilityFlags CompatibilityFlags, const FSceneView* View);

extern bool AnyRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View);
extern bool AnyInlineRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View);
extern FRayTracingPrimaryRaysOptions GetRayTracingTranslucencyOptions(const FViewInfo& View);

extern bool ShouldRenderRayTracingSkyLight(const FSkyLightSceneProxy* SkyLightSceneProxy);
extern bool ShouldRenderRayTracingAmbientOcclusion(const FViewInfo& View);
extern bool ShouldRenderRayTracingReflections(const FViewInfo& View);
extern bool ShouldRenderRayTracingGlobalIllumination(const FViewInfo& View);
extern bool ShouldRenderRayTracingTranslucency(const FViewInfo& View);
extern bool ShouldRenderRayTracingShadows();
extern bool ShouldRenderRayTracingShadowsForLight(const FLightSceneProxy& LightProxy);
extern bool ShouldRenderRayTracingShadowsForLight(const FLightSceneInfoCompact& LightInfo);
extern bool ShouldRenderPluginRayTracingGlobalIllumination(const FViewInfo& View);
extern bool HasRayTracedOverlay(const FSceneViewFamily& ViewFamily);

extern bool EnableRayTracingShadowTwoSidedGeometry();
extern float GetRaytracingMaxNormalBias();
extern int32 GetRayTracingCulling();
extern float GetRayTracingCullingRadius();

extern bool CanUseRayTracingAMDHitToken();

#else // RHI_RAYTRACING

FORCEINLINE bool ShouldRenderRayTracingEffect(bool bEffectEnabled, ERayTracingPipelineCompatibilityFlags CompatibilityFlags, const FSceneView* View)
{
	return false;
}

FORCEINLINE bool AnyRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingSkyLight(const FSkyLightSceneProxy* SkyLightSceneProxy)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingAmbientOcclusion(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingReflections(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingGlobalIllumination(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingTranslucency(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingShadows()
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingShadowsForLight(const FLightSceneProxy& LightProxy)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingShadowsForLight(const FLightSceneInfoCompact& LightInfo)
{
	return false;
}

FORCEINLINE bool ShouldRenderPluginRayTracingGlobalIllumination(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool HasRayTracedOverlay(const FSceneViewFamily& ViewFamily)
{
	return false;
}

FORCEINLINE int32 GetRayTracingCulling()
{
	return 0;
}

FORCEINLINE float GetRayTracingCullingRadius()
{
	return 0.0;
}

FORCEINLINE bool CanUseRayTracingAMDHitToken()
{
	return false;
}

#endif // RHI_RAYTRACING

FORCEINLINE bool ShouldRenderRayTracingEffect(ERayTracingPipelineCompatibilityFlags CompatibilityFlags)
{
	return ShouldRenderRayTracingEffect(true, CompatibilityFlags, nullptr);
}