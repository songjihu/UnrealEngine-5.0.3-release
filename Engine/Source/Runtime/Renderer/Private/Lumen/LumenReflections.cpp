// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenReflections.cpp
=============================================================================*/

#include "LumenReflections.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "SingleLayerWaterRendering.h"
#include "LumenTracingUtils.h"

extern FLumenGatherCvarState GLumenGatherCvars;

static TAutoConsoleVariable<int> CVarLumenAllowReflections(
	TEXT("r.Lumen.Reflections.Allow"),
	1,
	TEXT("Whether to allow Lumen Reflections.  Lumen Reflections is enabled in the project settings, this cvar can only disable it."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionDownsampleFactor = 1;
FAutoConsoleVariableRef GVarLumenReflectionDownsampleFactor(
	TEXT("r.Lumen.Reflections.DownsampleFactor"),
	GLumenReflectionDownsampleFactor,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTraceMeshSDFs = 1;
FAutoConsoleVariableRef GVarLumenReflectionTraceMeshSDFs(
	TEXT("r.Lumen.Reflections.TraceMeshSDFs"),
	GLumenReflectionTraceMeshSDFs,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionsSurfaceCacheFeedback = 1;
FAutoConsoleVariableRef CVarLumenReflectionsSurfaceCacheFeedback(
	TEXT("r.Lumen.Reflections.SurfaceCacheFeedback"),
	GLumenReflectionsSurfaceCacheFeedback,
	TEXT("Whether to allow writing into virtual surface cache feedback buffer from reflection rays."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionsUseRadianceCache = 0;
FAutoConsoleVariableRef CVarLumenReflectionsUseRadianceCache(
	TEXT("r.Lumen.Reflections.RadianceCache"),
	GLumenReflectionsUseRadianceCache,
	TEXT("Whether to reuse Lumen's ScreenProbeGather Radiance Cache, when it is available.  When enabled, reflection rays from rough surfaces are shortened and distant lighting comes from interpolating from the Radiance Cache, speeding up traces."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionRadianceCacheAngleThresholdScale = 1.0f;
FAutoConsoleVariableRef CVarLumenReflectionRadianceCacheAngleThresholdScale(
	TEXT("r.Lumen.Reflections.RadianceCache.AngleThresholdScale"),
	GLumenReflectionRadianceCacheAngleThresholdScale,
	TEXT("Controls when the Radiance Cache is used for distant lighting.  A value of 1 means only use the Radiance Cache when appropriate for the reflection cone, lower values are more aggressive."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionRadianceCacheReprojectionRadiusScale = 10.0f;
FAutoConsoleVariableRef CVarLumenReflectionRadianceCacheReprojectionRadiusScale(
	TEXT("r.Lumen.Reflections.RadianceCache.ReprojectionRadiusScale"),
	GLumenReflectionRadianceCacheReprojectionRadiusScale,
	TEXT("Scales the radius of the sphere around each Radiance Cache probe that is intersected for parallax correction when interpolating from the Radiance Cache."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionMaxRoughnessToTrace = .4f;
FAutoConsoleVariableRef GVarLumenReflectionMaxRoughnessToTrace(
	TEXT("r.Lumen.Reflections.MaxRoughnessToTrace"),
	GLumenReflectionMaxRoughnessToTrace,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionRoughnessFadeLength = .1f;
FAutoConsoleVariableRef GVarLumenReflectionRoughnessFadeLength(
	TEXT("r.Lumen.Reflections.RoughnessFadeLength"),
	GLumenReflectionRoughnessFadeLength,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionGGXSamplingBias = .1f;
FAutoConsoleVariableRef GVarLumenReflectionGGXSamplingBias(
	TEXT("r.Lumen.Reflections.GGXSamplingBias"),
	GLumenReflectionGGXSamplingBias,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTemporalFilter = 1;
FAutoConsoleVariableRef CVarLumenReflectionTemporalFilter(
	TEXT("r.Lumen.Reflections.Temporal"),
	GLumenReflectionTemporalFilter,
	TEXT("Whether to use a temporal filter"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenReflectionTemporalMaxFramesAccumulated = 32.0f;
FAutoConsoleVariableRef CVarLumenReflectionTemporalMaxFramesAccumulated(
	TEXT("r.Lumen.Reflections.Temporal.MaxFramesAccumulated"),
	GLumenReflectionTemporalMaxFramesAccumulated,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GLumenReflectionHistoryDistanceThreshold = .03f;
FAutoConsoleVariableRef CVarLumenReflectionHistoryDistanceThreshold(
	TEXT("r.Lumen.Reflections.Temporal.DistanceThreshold"),
	GLumenReflectionHistoryDistanceThreshold,
	TEXT("World space distance threshold needed to discard last frame's lighting results.  Lower values reduce ghosting from characters when near a wall but increase flickering artifacts."),
	ECVF_RenderThreadSafe
	);

float GLumenReflectionMaxRayIntensity = 100;
FAutoConsoleVariableRef GVarLumenReflectionMaxRayIntensity(
	TEXT("r.Lumen.Reflections.MaxRayIntensity"),
	GLumenReflectionMaxRayIntensity,
	TEXT("Clamps the maximum ray lighting intensity (with PreExposure) to reduce fireflies."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionSmoothBias = 0.0f;
FAutoConsoleVariableRef GVarLumenReflectionSmoothBias(
	TEXT("r.Lumen.Reflections.SmoothBias"),
	GLumenReflectionSmoothBias,
	TEXT("Values larger than 0 apply a global material roughness bias for Lumen Reflections, where 1 is fully mirror."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionScreenSpaceReconstruction = 1;
FAutoConsoleVariableRef CVarLumenReflectionScreenSpaceReconstruction(
	TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction"),
	GLumenReflectionScreenSpaceReconstruction,
	TEXT("Whether to use the screen space BRDF reweighting reconstruction"),
	ECVF_RenderThreadSafe
	);

int32 GLumenReflectionScreenSpaceReconstructionNumSamples = 5;
FAutoConsoleVariableRef CVarLumenReflectionScreenSpaceReconstructionNumSamples(
	TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.NumSamples"),
	GLumenReflectionScreenSpaceReconstructionNumSamples,
	TEXT("Number of samples to use for the screen space BRDF reweighting reconstruction"),
	ECVF_RenderThreadSafe
	);

float GLumenReflectionScreenSpaceReconstructionKernelRadius = 8.0;
FAutoConsoleVariableRef CVarLumenReflectionScreenSpaceReconstructionKernelScreenWidth(
	TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.KernelRadius"),
	GLumenReflectionScreenSpaceReconstructionKernelRadius,
	TEXT("Screen space reflection filter kernel radius in pixels"),
	ECVF_RenderThreadSafe
	);

float GLumenReflectionScreenSpaceReconstructionRoughnessScale = 1.0f;
FAutoConsoleVariableRef CVarLumenReflectionScreenSpaceReconstructionRoughnessScale(
	TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.RoughnessScale"),
	GLumenReflectionScreenSpaceReconstructionRoughnessScale,
	TEXT("Values higher than 1 allow neighbor traces to be blurred together more aggressively, but is not physically correct."),
	ECVF_RenderThreadSafe
	);

int32 GLumenReflectionBilateralFilter = 1;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilter(
	TEXT("r.Lumen.Reflections.BilateralFilter"),
	GLumenReflectionBilateralFilter,
	TEXT("Whether to do a bilateral filter as a last step in denoising Lumen Reflections."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenReflectionBilateralFilterSpatialKernelRadius = .002f;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilterSpatialKernelRadius(
	TEXT("r.Lumen.Reflections.BilateralFilter.SpatialKernelRadius"),
	GLumenReflectionBilateralFilterSpatialKernelRadius,
	TEXT("Spatial kernel radius, as a fraction of the viewport size"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenReflectionBilateralFilterNumSamples = 4;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilterNumSamples(
	TEXT("r.Lumen.Reflections.BilateralFilter.NumSamples"),
	GLumenReflectionBilateralFilterNumSamples,
	TEXT("Number of bilateral filter samples."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenReflectionBilateralFilterDepthWeightScale = 10000.0f;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilterDepthWeightScale(
	TEXT("r.Lumen.Reflections.BilateralFilter.DepthWeightScale"),
	GLumenReflectionBilateralFilterDepthWeightScale,
	TEXT("Scales the depth weight of the bilateral filter"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenReflectionBilateralFilterNormalAngleThresholdScale = 1.0f;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilterNormalAngleThresholdScale(
	TEXT("r.Lumen.Reflections.BilateralFilter.NormalAngleThresholdScale"),
	GLumenReflectionBilateralFilterNormalAngleThresholdScale,
	TEXT("Scales the Normal angle threshold of the bilateral filter"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenReflectionBilateralFilterStrongBlurVarianceThreshold = .5f;
FAutoConsoleVariableRef CVarLumenReflectionBilateralFilterStrongBlurVarianceThreshold(
	TEXT("r.Lumen.Reflections.BilateralFilter.StrongBlurVarianceThreshold"),
	GLumenReflectionBilateralFilterStrongBlurVarianceThreshold,
	TEXT("Pixels whose variance from the spatial resolve filter are higher than this value get a stronger bilateral blur."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenReflectionsVisualizeTracingCoherency = 0;
FAutoConsoleVariableRef GVarLumenReflectionsVisualizeTracingCoherency(
	TEXT("r.Lumen.Reflections.VisualizeTracingCoherency"),
	GLumenReflectionsVisualizeTracingCoherency,
	TEXT("Set to 1 to capture traces from a random wavefront and draw them on the screen. Set to 1 again to re-capture.  Shaders must enable support first, see DEBUG_SUPPORT_VISUALIZE_TRACE_COHERENCY"),
	ECVF_RenderThreadSafe
);

TRefCountPtr<FRDGPooledBuffer> GVisualizeReflectionTracesData;

FRDGBufferRef SetupVisualizeReflectionTraces(FRDGBuilder& GraphBuilder, FLumenReflectionsVisualizeTracesParameters& VisualizeTracesParameters)
{
	FRDGBufferRef VisualizeTracesData = nullptr;

	if (GVisualizeReflectionTracesData.IsValid())
	{
		VisualizeTracesData = GraphBuilder.RegisterExternalBuffer(GVisualizeReflectionTracesData);
	}

	const int32 VisualizeBufferNumElements = 32 * 3;

	if (!VisualizeTracesData || VisualizeTracesData->Desc.NumElements != VisualizeBufferNumElements)
	{
		VisualizeTracesData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VisualizeBufferNumElements), TEXT("VisualizeTracesData"));
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(VisualizeTracesData, PF_A32B32G32R32F), 0.0f);
	}

	VisualizeTracesParameters.VisualizeTraceCoherency = 0;
	VisualizeTracesParameters.RWVisualizeTracesData = GraphBuilder.CreateUAV(VisualizeTracesData, PF_A32B32G32R32F);

	if (GLumenReflectionsVisualizeTracingCoherency == 1)
	{
		GLumenReflectionsVisualizeTracingCoherency = 2;
		VisualizeTracesParameters.VisualizeTraceCoherency = 1;
	}

	return VisualizeTracesData;
}

void GetReflectionsVisualizeTracesBuffer(TRefCountPtr<FRDGPooledBuffer>& VisualizeTracesData)
{
	if (GVisualizeReflectionTracesData.IsValid() && GLumenReflectionsVisualizeTracingCoherency != 0)
	{
		VisualizeTracesData = GVisualizeReflectionTracesData;
	}
}

class FReflectionClearTileIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionClearTileIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionClearTileIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionResolveTileIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionTracingTileIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionClearTileIndirectArgsCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionClearTileIndirectArgsCS", SF_Compute);

// Must match usf RESOLVE_TILE_SIZE
const int32 GReflectionResolveTileSize = 8;

class FReflectionTileClassificationMarkCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTileClassificationMarkCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTileClassificationMarkCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWDownsampledDepth)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionResolveTileIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionTracingTileIndirectArgs)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWResolveTileUsed)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	using FPermutationDomain = TShaderPermutationDomain<>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTileClassificationMarkCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionTileClassificationMarkCS", SF_Compute);


class FReflectionTileClassificationBuildListsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTileClassificationBuildListsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTileClassificationBuildListsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionTileIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionTileData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ResolveTileUsed)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER(FIntPoint, TileViewportDimensions)
		SHADER_PARAMETER(FIntPoint, ResolveTileViewportDimensions)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	class FSupportDownsample : SHADER_PERMUTATION_BOOL("SUPPORT_DOWNSAMPLE_FACTOR");
	using FPermutationDomain = TShaderPermutationDomain<FSupportDownsample>;

	static int32 GetGroupSize()
	{
		return 8;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTileClassificationBuildListsCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionTileClassificationBuildListsCS", SF_Compute);



class FReflectionGenerateRaysCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionGenerateRaysCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionGenerateRaysCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWRayBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWDownsampledDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWRayTraceDistance)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER(float, MaxTraceDistance)
		SHADER_PARAMETER(float, RadianceCacheAngleThresholdScale)
		SHADER_PARAMETER(float, GGXSamplingBias)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	using FPermutationDomain = TShaderPermutationDomain<FRadianceCache>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionGenerateRaysCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionGenerateRaysCS", SF_Compute);


class FReflectionResolveCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionResolveCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionResolveCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWSpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWResolveVariance)
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER(float, InvRoughnessFadeLength)
		SHADER_PARAMETER(uint32, NumSpatialReconstructionSamples)
		SHADER_PARAMETER(float, SpatialReconstructionKernelRadius)
		SHADER_PARAMETER(float, SpatialReconstructionRoughnessScale)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	class FSpatialReconstruction : SHADER_PERMUTATION_BOOL("USE_SPATIAL_RECONSTRUCTION");
	class FBilateralFilter : SHADER_PERMUTATION_BOOL("USE_BILATERAL_FILTER");
	using FPermutationDomain = TShaderPermutationDomain<FSpatialReconstruction, FBilateralFilter>;
};

IMPLEMENT_GLOBAL_SHADER(FReflectionResolveCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionResolveCS", SF_Compute);


class FReflectionTemporalReprojectionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTemporalReprojectionCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTemporalReprojectionCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWSpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWNumHistoryFramesAccumulated)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWResolveVariance)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SpecularIndirectHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistoryNumFramesAccumulated)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolveVariance)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolveVarianceHistory)
		SHADER_PARAMETER(float,HistoryDistanceThreshold)
		SHADER_PARAMETER(float,PrevInvPreExposure)
		SHADER_PARAMETER(float,MaxFramesAccumulated)
		SHADER_PARAMETER(FVector2f,InvDiffuseIndirectBufferSize)
		SHADER_PARAMETER(FVector4f,HistoryScreenPositionScaleBias)
		SHADER_PARAMETER(FVector4f,HistoryUVMinMax)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, VelocityTextureSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolvedReflections)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FBilateralFilter : SHADER_PERMUTATION_BOOL("USE_BILATERAL_FILTER");
	using FPermutationDomain = TShaderPermutationDomain<FBilateralFilter>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTemporalReprojectionCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionTemporalReprojectionCS", SF_Compute);


class FReflectionBilateralFilterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionBilateralFilterCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionBilateralFilterCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWSpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float3>, SpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolveVariance)
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER(float, BilateralFilterSpatialKernelRadius)
		SHADER_PARAMETER(uint32, BilateralFilterNumSamples)
		SHADER_PARAMETER(float, BilateralFilterDepthWeightScale)
		SHADER_PARAMETER(float, BilateralFilterNormalAngleThresholdScale)
		SHADER_PARAMETER(float, BilateralFilterStrongBlurVarianceThreshold)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionBilateralFilterCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionBilateralFilterCS", SF_Compute);


class FReflectionPassthroughCopyCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionPassthroughCopyCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionPassthroughCopyCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWSpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWNumHistoryFramesAccumulated)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWResolveVariance)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolveVariance)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ResolvedReflections)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FBilateralFilter : SHADER_PERMUTATION_BOOL("USE_BILATERAL_FILTER");
	using FPermutationDomain = TShaderPermutationDomain<FBilateralFilter>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionPassthroughCopyCS, "/Engine/Private/Lumen/LumenReflections.usf", "ReflectionPassthroughCopyCS", SF_Compute);


bool ShouldRenderLumenReflections(const FViewInfo& View, bool bSkipTracingDataCheck, bool bSkipProjectCheck)
{
	const FScene* Scene = (const FScene*)View.Family->Scene;
	if (Scene)
	{
		//@todo - support standalone Lumen Reflections
		return ShouldRenderLumenDiffuseGI(Scene, View, bSkipTracingDataCheck, bSkipProjectCheck)
			&& Lumen::IsLumenFeatureAllowedForView(Scene, View, bSkipTracingDataCheck, bSkipProjectCheck) 
			&& View.FinalPostProcessSettings.ReflectionMethod == EReflectionMethod::Lumen
			&& View.Family->EngineShowFlags.LumenReflections 
			&& CVarLumenAllowReflections.GetValueOnAnyThread()
			&& (bSkipTracingDataCheck || Lumen::UseHardwareRayTracedReflections() || Lumen::IsSoftwareRayTracingSupported());
	}
	
	return false;
}

FLumenReflectionTileParameters ReflectionTileClassification(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters)
{
	FLumenReflectionTileParameters ReflectionTileParameters;

	const FIntPoint ResolveTileViewportDimensions(
		FMath::DivideAndRoundUp(View.ViewRect.Size().X, GReflectionResolveTileSize), 
		FMath::DivideAndRoundUp(View.ViewRect.Size().Y, GReflectionResolveTileSize));

	const FIntPoint ResolveTileBufferDimensions(
		FMath::DivideAndRoundUp(SceneTextures.Config.Extent.X, GReflectionResolveTileSize), 
		FMath::DivideAndRoundUp(SceneTextures.Config.Extent.Y, GReflectionResolveTileSize));

	const int32 TracingTileSize = GReflectionResolveTileSize * ReflectionTracingParameters.ReflectionDownsampleFactor;

	const FIntPoint TracingTileViewportDimensions(
		FMath::DivideAndRoundUp(View.ViewRect.Size().X, TracingTileSize), 
		FMath::DivideAndRoundUp(View.ViewRect.Size().Y, TracingTileSize));

	const FIntPoint TracingTileBufferDimensions(
		FMath::DivideAndRoundUp(SceneTextures.Config.Extent.X, TracingTileSize), 
		FMath::DivideAndRoundUp(SceneTextures.Config.Extent.Y, TracingTileSize));

	const int32 NumResolveTiles = ResolveTileBufferDimensions.X * ResolveTileBufferDimensions.Y;
	const int32 NumTracingTiles = TracingTileBufferDimensions.X * TracingTileBufferDimensions.Y;

	FRDGBufferRef ReflectionResolveTileData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumResolveTiles), TEXT("Lumen.Reflections.ReflectionResolveTileData"));
	FRDGBufferRef ReflectionResolveTileIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.ReflectionResolveTileIndirectArgs"));
	FRDGBufferRef ReflectionTracingTileIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.ReflectionTracingTileIndirectArgs"));

	FRDGTextureDesc ResolveTileUsedDesc = FRDGTextureDesc::Create2D(ResolveTileBufferDimensions, PF_R8_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef ResolveTileUsed = GraphBuilder.CreateTexture(ResolveTileUsedDesc, TEXT("Lumen.Reflections.ResolveTileUsed"));

	{
		FReflectionTileClassificationMarkCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTileClassificationMarkCS::FParameters>();
		PassParameters->RWDownsampledDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.DownsampledDepth));
		PassParameters->RWReflectionResolveTileIndirectArgs = GraphBuilder.CreateUAV(ReflectionResolveTileIndirectArgs, PF_R32_UINT);
		PassParameters->RWReflectionTracingTileIndirectArgs = GraphBuilder.CreateUAV(ReflectionTracingTileIndirectArgs, PF_R32_UINT);
		PassParameters->RWResolveTileUsed = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ResolveTileUsed));
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;

		FReflectionTileClassificationMarkCS::FPermutationDomain PermutationVector;
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionTileClassificationMarkCS>(PermutationVector);

		checkf(ResolveTileViewportDimensions.X > 0 && ResolveTileViewportDimensions.Y > 0, TEXT("FReflectionTileClassificationMarkCS needs non-zero dispatch to clear next pass's indirect args"));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TileClassificationMark"),
			ComputeShader,
			PassParameters,
			FIntVector(ResolveTileViewportDimensions.X, ResolveTileViewportDimensions.Y, 1));
	}

	{
		FReflectionTileClassificationBuildListsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTileClassificationBuildListsCS::FParameters>();
		PassParameters->RWReflectionTileIndirectArgs = GraphBuilder.CreateUAV(ReflectionResolveTileIndirectArgs, PF_R32_UINT);
		PassParameters->RWReflectionTileData = GraphBuilder.CreateUAV(ReflectionResolveTileData, PF_R32_UINT);
		PassParameters->ResolveTileUsed = ResolveTileUsed;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->TileViewportDimensions = ResolveTileViewportDimensions;
		PassParameters->ResolveTileViewportDimensions = ResolveTileViewportDimensions;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;

		FReflectionTileClassificationBuildListsCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionTileClassificationBuildListsCS::FSupportDownsample >(false);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionTileClassificationBuildListsCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TileClassificationBuildLists"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(ResolveTileViewportDimensions, FReflectionTileClassificationBuildListsCS::GetGroupSize()));
	}

	FRDGBufferRef ReflectionTracingTileData;

	if (ReflectionTracingParameters.ReflectionDownsampleFactor == 1)
	{
		ReflectionTracingTileIndirectArgs = ReflectionResolveTileIndirectArgs;
		ReflectionTracingTileData = ReflectionResolveTileData;
	}
	else
	{
		ReflectionTracingTileData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTracingTiles), TEXT("Lumen.Reflections.ReflectionTracingTileData"));

		FReflectionTileClassificationBuildListsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTileClassificationBuildListsCS::FParameters>();
		PassParameters->RWReflectionTileIndirectArgs = GraphBuilder.CreateUAV(ReflectionTracingTileIndirectArgs, PF_R32_UINT);
		PassParameters->RWReflectionTileData = GraphBuilder.CreateUAV(ReflectionTracingTileData, PF_R32_UINT);
		PassParameters->ResolveTileUsed = ResolveTileUsed;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->TileViewportDimensions = TracingTileViewportDimensions;
		PassParameters->ResolveTileViewportDimensions = ResolveTileViewportDimensions;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;

		FReflectionTileClassificationBuildListsCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionTileClassificationBuildListsCS::FSupportDownsample >(true);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionTileClassificationBuildListsCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TileClassificationBuildTracingLists"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TracingTileViewportDimensions, FReflectionTileClassificationBuildListsCS::GetGroupSize()));
	}

	ReflectionTileParameters.ResolveIndirectArgs = ReflectionResolveTileIndirectArgs;
	ReflectionTileParameters.TracingIndirectArgs = ReflectionTracingTileIndirectArgs;
	ReflectionTileParameters.ReflectionResolveTileData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionResolveTileData, PF_R32_UINT));
	ReflectionTileParameters.ReflectionTracingTileData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionTracingTileData, PF_R32_UINT));
	ReflectionTileParameters.ResolveTileUsed = ResolveTileUsed;
	return ReflectionTileParameters;
}

void UpdateHistoryReflections(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FSceneTextures& SceneTextures,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	FRDGTextureRef ResolvedReflections,
	FRDGTextureRef ResolveVariance,
	FRDGTextureRef FinalSpecularIndirect,
	FRDGTextureRef AccumulatedResolveVariance)
{
	LLM_SCOPE_BYTAG(Lumen);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
	FRDGTextureRef VelocityTexture = GetIfProduced(SceneTextures.Velocity, SystemTextures.Black);
	const bool bUseBilaterialFilter = GLumenReflectionBilateralFilter != 0;
		 
	FRDGTextureDesc NumHistoryFramesAccumulatedDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_G8, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef NewNumHistoryFramesAccumulated = GraphBuilder.CreateTexture(NumHistoryFramesAccumulatedDesc, TEXT("Lumen.Reflections.NumHistoryFramesAccumulated"));

	if (GLumenReflectionTemporalFilter
		&& View.ViewState
		&& View.ViewState->Lumen.ReflectionState.SpecularIndirectHistoryRT
		&& (!bUseBilaterialFilter || View.ViewState->Lumen.ReflectionState.ResolveVarianceHistoryRT)
		&& !View.bCameraCut 
		&& !View.bPrevTransformsReset
		// If the scene render targets reallocate, toss the history so we don't read uninitialized data
		&& View.ViewState->Lumen.ReflectionState.SpecularIndirectHistoryRT->GetDesc().Extent == SceneTextures.Config.Extent
		&& (!bUseBilaterialFilter || View.ViewState->Lumen.ReflectionState.ResolveVarianceHistoryRT->GetDesc().Extent == SceneTextures.Config.Extent))
	{
		FReflectionTemporalState& ReflectionTemporalState = View.ViewState->Lumen.ReflectionState;
		TRefCountPtr<IPooledRenderTarget>* SpecularIndirectHistoryState = &ReflectionTemporalState.SpecularIndirectHistoryRT;
		TRefCountPtr<IPooledRenderTarget>* NumFramesAccumulatedState = &ReflectionTemporalState.NumFramesAccumulatedRT;
		TRefCountPtr<IPooledRenderTarget>* ResolveVarianceHistoryState = &ReflectionTemporalState.ResolveVarianceHistoryRT;
		FIntRect* HistoryViewRect = &ReflectionTemporalState.HistoryViewRect;
		FVector4f* HistoryScreenPositionScaleBias = &ReflectionTemporalState.HistoryScreenPositionScaleBias;

		FRDGTextureRef OldDepthHistory = View.ViewState->Lumen.DepthHistoryRT ? GraphBuilder.RegisterExternalTexture(View.ViewState->Lumen.DepthHistoryRT) : SceneTextures.Depth.Target;

		{
			FRDGTextureRef OldSpecularIndirectHistory = GraphBuilder.RegisterExternalTexture(*SpecularIndirectHistoryState);
			FRDGTextureRef ResolveVarianceHistory = GraphBuilder.RegisterExternalTexture(ResolveVarianceHistoryState->IsValid() ? *ResolveVarianceHistoryState : GSystemTextures.BlackDummy);

			FReflectionTemporalReprojectionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTemporalReprojectionCS::FParameters>();
			PassParameters->RWSpecularIndirect = GraphBuilder.CreateUAV(FinalSpecularIndirect);
			PassParameters->RWNumHistoryFramesAccumulated = GraphBuilder.CreateUAV(NewNumHistoryFramesAccumulated);
			PassParameters->RWResolveVariance = GraphBuilder.CreateUAV(AccumulatedResolveVariance);
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->SpecularIndirectHistory = OldSpecularIndirectHistory;
			PassParameters->HistoryNumFramesAccumulated = GraphBuilder.RegisterExternalTexture(*NumFramesAccumulatedState);
			PassParameters->DepthHistory = OldDepthHistory;
			PassParameters->HistoryDistanceThreshold = GLumenReflectionHistoryDistanceThreshold;
			PassParameters->PrevInvPreExposure = 1.0f / View.PrevViewInfo.SceneColorPreExposure;
			const FVector2f InvBufferSize(1.0f / SceneTextures.Config.Extent.X, 1.0f / SceneTextures.Config.Extent.Y);
			PassParameters->InvDiffuseIndirectBufferSize = InvBufferSize;
			PassParameters->HistoryScreenPositionScaleBias = *HistoryScreenPositionScaleBias;

			// Pull in the max UV to exclude the region which will read outside the viewport due to bilinear filtering
			PassParameters->HistoryUVMinMax = FVector4f(
				(HistoryViewRect->Min.X + 0.5f) * InvBufferSize.X,
				(HistoryViewRect->Min.Y + 0.5f) * InvBufferSize.Y,
				(HistoryViewRect->Max.X - 0.5f) * InvBufferSize.X,
				(HistoryViewRect->Max.Y - 0.5f) * InvBufferSize.Y);
			PassParameters->MaxFramesAccumulated = GLumenReflectionTemporalMaxFramesAccumulated;

			PassParameters->VelocityTexture = VelocityTexture;
			PassParameters->VelocityTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
			PassParameters->ResolvedReflections = ResolvedReflections;
			PassParameters->ResolveVariance = ResolveVariance;
			PassParameters->ResolveVarianceHistory = ResolveVarianceHistory;
			PassParameters->ReflectionTileParameters = ReflectionTileParameters;

			FReflectionTemporalReprojectionCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< FReflectionTemporalReprojectionCS::FBilateralFilter >(bUseBilaterialFilter);
			auto ComputeShader = View.ShaderMap->GetShader<FReflectionTemporalReprojectionCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Temporal Reprojection"),
				ComputeShader,
				PassParameters,
				ReflectionTileParameters.ResolveIndirectArgs,
				0);
		}
	}
	else
	{
		FReflectionPassthroughCopyCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionPassthroughCopyCS::FParameters>();
		PassParameters->RWSpecularIndirect = GraphBuilder.CreateUAV(FinalSpecularIndirect);
		PassParameters->RWNumHistoryFramesAccumulated = GraphBuilder.CreateUAV(NewNumHistoryFramesAccumulated);
		PassParameters->RWResolveVariance = GraphBuilder.CreateUAV(AccumulatedResolveVariance);
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->ResolvedReflections = ResolvedReflections;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->ResolveVariance = ResolveVariance;

		FReflectionPassthroughCopyCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionPassthroughCopyCS::FBilateralFilter >(bUseBilaterialFilter);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionPassthroughCopyCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Passthrough"),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.ResolveIndirectArgs,
			0);
	}

	if (View.ViewState && !View.bStatePrevViewInfoIsReadOnly)
	{
		FReflectionTemporalState& ReflectionTemporalState = View.ViewState->Lumen.ReflectionState;
		ReflectionTemporalState.HistoryViewRect = View.ViewRect;
		ReflectionTemporalState.HistoryScreenPositionScaleBias = View.GetScreenPositionScaleBias(SceneTextures.Config.Extent, View.ViewRect);

		// Queue updating the view state's render target reference with the new values
		GraphBuilder.QueueTextureExtraction(FinalSpecularIndirect, &ReflectionTemporalState.SpecularIndirectHistoryRT);
		GraphBuilder.QueueTextureExtraction(NewNumHistoryFramesAccumulated, &ReflectionTemporalState.NumFramesAccumulatedRT);

		if (bUseBilaterialFilter)
		{
			GraphBuilder.QueueTextureExtraction(AccumulatedResolveVariance, &ReflectionTemporalState.ResolveVarianceHistoryRT);
		}
	}
}

DECLARE_GPU_STAT(LumenReflections);

FRDGTextureRef FDeferredShadingSceneRenderer::RenderLumenReflections(
	FRDGBuilder& GraphBuilder, 
	const FViewInfo& View,
	const FSceneTextures& SceneTextures,
	FLumenSceneFrameTemporaries& FrameTemporaries,
	const FLumenMeshSDFGridParameters& MeshSDFGridParameters,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& ScreenProbeRadianceCacheParameters,
	FLumenReflectionCompositeParameters& OutCompositeParameters)
{
	OutCompositeParameters.MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
	OutCompositeParameters.InvRoughnessFadeLength = 1.0f / GLumenReflectionRoughnessFadeLength;

	check(ShouldRenderLumenReflections(View));

	LumenRadianceCache::FRadianceCacheInterpolationParameters RadianceCacheParameters = ScreenProbeRadianceCacheParameters;
	RadianceCacheParameters.RadianceCacheInputs.ReprojectionRadiusScale = FMath::Clamp<float>(GLumenReflectionRadianceCacheReprojectionRadiusScale, 1.0f, 100000.0f);

	LLM_SCOPE_BYTAG(Lumen);
	RDG_EVENT_SCOPE(GraphBuilder, "LumenReflections");
	RDG_GPU_STAT_SCOPE(GraphBuilder, LumenReflections);

	FLumenReflectionTracingParameters ReflectionTracingParameters;

	FRDGBufferRef VisualizeTracesData = SetupVisualizeReflectionTraces(GraphBuilder, ReflectionTracingParameters.VisualizeTracesParameters);

	const int32 UserDownsampleFactor = View.FinalPostProcessSettings.LumenReflectionQuality <= .25f ? 2 : 1;
	ReflectionTracingParameters.ReflectionDownsampleFactor = FMath::Clamp(GLumenReflectionDownsampleFactor * UserDownsampleFactor, 1, 4);
	ReflectionTracingParameters.ReflectionTracingViewSize = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), (int32)ReflectionTracingParameters.ReflectionDownsampleFactor);
	ReflectionTracingParameters.ReflectionTracingBufferSize = FIntPoint::DivideAndRoundUp(SceneTextures.Config.Extent, (int32)ReflectionTracingParameters.ReflectionDownsampleFactor);
	ReflectionTracingParameters.MaxRayIntensity = GLumenReflectionMaxRayIntensity;
	ReflectionTracingParameters.ReflectionSmoothBias = GLumenReflectionSmoothBias;

	FRDGTextureDesc RayBufferDesc(FRDGTextureDesc::Create2D(ReflectionTracingParameters.ReflectionTracingBufferSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ReflectionTracingParameters.RayBuffer = GraphBuilder.CreateTexture(RayBufferDesc, TEXT("Lumen.Reflections.ReflectionRayBuffer"));

	FRDGTextureDesc DownsampledDepthDesc(FRDGTextureDesc::Create2D(ReflectionTracingParameters.ReflectionTracingBufferSize, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ReflectionTracingParameters.DownsampledDepth = GraphBuilder.CreateTexture(DownsampledDepthDesc, TEXT("Lumen.Reflections.ReflectionDownsampledDepth"));

	FRDGTextureDesc RayTraceDistanceDesc(FRDGTextureDesc::Create2D(ReflectionTracingParameters.ReflectionTracingBufferSize, PF_R16_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ReflectionTracingParameters.RayTraceDistance = GraphBuilder.CreateTexture(RayTraceDistanceDesc, TEXT("Lumen.Reflections.RayTraceDistance"));

	FBlueNoise BlueNoise;
	InitializeBlueNoise(BlueNoise);
	ReflectionTracingParameters.BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

	FLumenReflectionTileParameters ReflectionTileParameters = ReflectionTileClassification(GraphBuilder, View, SceneTextures, ReflectionTracingParameters);

	const bool bUseRadianceCache = GLumenReflectionsUseRadianceCache != 0 && RadianceCacheParameters.RadianceProbeIndirectionTexture != nullptr;

	{
		FReflectionGenerateRaysCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionGenerateRaysCS::FParameters>();
		PassParameters->RWRayBuffer = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.RayBuffer));
		PassParameters->RWDownsampledDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.DownsampledDepth));
		PassParameters->RWRayTraceDistance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.RayTraceDistance));
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
		PassParameters->MaxTraceDistance = Lumen::GetMaxTraceDistance(View);
		PassParameters->RadianceCacheAngleThresholdScale = FMath::Clamp<float>(GLumenReflectionRadianceCacheAngleThresholdScale, .05f, 4.0f);
		PassParameters->GGXSamplingBias = GLumenReflectionGGXSamplingBias;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->RadianceCacheParameters = RadianceCacheParameters;

		FReflectionGenerateRaysCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FReflectionGenerateRaysCS::FRadianceCache>(bUseRadianceCache);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionGenerateRaysCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GenerateRays%s", bUseRadianceCache ? TEXT(" RadianceCache") : TEXT("")),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}

	FLumenCardTracingInputs TracingInputs(GraphBuilder, Scene, View, FrameTemporaries, /*bSurfaceCacheFeedback*/ GLumenReflectionsSurfaceCacheFeedback != 0);

	FRDGTextureDesc TraceRadianceDesc(FRDGTextureDesc::Create2D(ReflectionTracingParameters.ReflectionTracingBufferSize, PF_FloatRGB, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ReflectionTracingParameters.TraceRadiance = GraphBuilder.CreateTexture(TraceRadianceDesc, TEXT("Lumen.Reflections.ReflectionTraceRadiance"));
	ReflectionTracingParameters.RWTraceRadiance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.TraceRadiance));

	FRDGTextureDesc TraceHitDesc(FRDGTextureDesc::Create2D(ReflectionTracingParameters.ReflectionTracingBufferSize, PF_R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ReflectionTracingParameters.TraceHit = GraphBuilder.CreateTexture(TraceHitDesc, TEXT("Lumen.Reflections.ReflectionTraceHit"));
	ReflectionTracingParameters.RWTraceHit = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReflectionTracingParameters.TraceHit));

	TraceReflections(
		GraphBuilder, 
		Scene,
		View, 
		GLumenReflectionTraceMeshSDFs != 0 && Lumen::UseMeshSDFTracing(ViewFamily),
		SceneTextures,
		TracingInputs,
		ReflectionTracingParameters,
		ReflectionTileParameters,
		MeshSDFGridParameters,
		bUseRadianceCache,
		RadianceCacheParameters);
	
	if (VisualizeTracesData)
	{
		GVisualizeReflectionTracesData = GraphBuilder.ConvertToExternalBuffer(VisualizeTracesData);
	}

	FRDGTextureDesc SpecularIndirectDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef ResolvedSpecularIndirect = GraphBuilder.CreateTexture(SpecularIndirectDesc, TEXT("Lumen.Reflections.ResolvedSpecularIndirect"));

	FRDGTextureDesc ResolveVarianceDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef ResolveVariance = GraphBuilder.CreateTexture(ResolveVarianceDesc, TEXT("Lumen.Reflections.ResolveVariance"));

	const int32 NumReconstructionSamples = FMath::Clamp(FMath::RoundToInt(View.FinalPostProcessSettings.LumenReflectionQuality * GLumenReflectionScreenSpaceReconstructionNumSamples), GLumenReflectionScreenSpaceReconstructionNumSamples, 64);

	{
		FReflectionResolveCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionResolveCS::FParameters>();
		PassParameters->RWSpecularIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ResolvedSpecularIndirect));
		PassParameters->RWResolveVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ResolveVariance));
		PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
		PassParameters->InvRoughnessFadeLength = 1.0f / GLumenReflectionRoughnessFadeLength;
		PassParameters->NumSpatialReconstructionSamples = NumReconstructionSamples;
		PassParameters->SpatialReconstructionKernelRadius = GLumenReflectionScreenSpaceReconstructionKernelRadius;
		PassParameters->SpatialReconstructionRoughnessScale = GLumenReflectionScreenSpaceReconstructionRoughnessScale;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;

		FReflectionResolveCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionResolveCS::FSpatialReconstruction >(GLumenReflectionScreenSpaceReconstruction != 0);
		PermutationVector.Set< FReflectionResolveCS::FBilateralFilter >(GLumenReflectionBilateralFilter != 0);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionResolveCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ReflectionResolve"),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.ResolveIndirectArgs,
			0);
	}

	FRDGTextureRef SpecularIndirect = GraphBuilder.CreateTexture(SpecularIndirectDesc, TEXT("Lumen.Reflections.SpecularIndirect"));
	FRDGTextureRef AccumulatedResolveVariance = GraphBuilder.CreateTexture(ResolveVarianceDesc, TEXT("Lumen.Reflections.AccumulatedResolveVariance"));

	//@todo - only clear tiles not written to by history pass
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpecularIndirect)), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));

	UpdateHistoryReflections(
		GraphBuilder,
		View,
		SceneTextures,
		ReflectionTileParameters,
		ResolvedSpecularIndirect,
		ResolveVariance,
		SpecularIndirect,
		AccumulatedResolveVariance);

	if (GLumenReflectionBilateralFilter)
	{
		FReflectionBilateralFilterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionBilateralFilterCS::FParameters>();
		PassParameters->RWSpecularIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ResolvedSpecularIndirect));
		PassParameters->SpecularIndirect = SpecularIndirect;
		PassParameters->ResolveVariance = AccumulatedResolveVariance;
		PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
		PassParameters->BilateralFilterSpatialKernelRadius = GLumenReflectionBilateralFilterSpatialKernelRadius;
		PassParameters->BilateralFilterNumSamples = GLumenReflectionBilateralFilterNumSamples;
		PassParameters->BilateralFilterDepthWeightScale = GLumenReflectionBilateralFilterDepthWeightScale;
		PassParameters->BilateralFilterNormalAngleThresholdScale = GLumenReflectionBilateralFilterNormalAngleThresholdScale;
		PassParameters->BilateralFilterStrongBlurVarianceThreshold = GLumenReflectionBilateralFilterStrongBlurVarianceThreshold;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;

		auto ComputeShader = View.ShaderMap->GetShader<FReflectionBilateralFilterCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("BilateralFilter"),
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.ResolveIndirectArgs,
			0);

		SpecularIndirect = ResolvedSpecularIndirect;
	}

	return SpecularIndirect;
}

void Lumen::Shutdown()
{
	GVisualizeReflectionTracesData.SafeRelease();
}