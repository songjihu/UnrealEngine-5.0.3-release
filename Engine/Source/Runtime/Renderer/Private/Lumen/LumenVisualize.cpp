// Copyright Epic Games, Inc. All Rights Reserved.

#include "LumenVisualize.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "ReflectionEnvironment.h"
#include "LumenMeshCards.h"
#include "LumenRadianceCache.h"
#include "DynamicMeshBuilder.h"
#include "ShaderPrintParameters.h"
#include "LumenScreenProbeGather.h"
#include "DistanceFieldAtlas.h"
#include "LumenSurfaceCacheFeedback.h"
#include "LumenVisualizationData.h"

// Must be in sync with VISUALIZE_MODE_* in LumenVisualize.h
int32 GLumenVisualize = 0;
FAutoConsoleVariableRef CVarLumenVisualize(
	TEXT("r.Lumen.Visualize"),
	GLumenVisualize,
	TEXT("Lumen scene visualization mode.\n")
	TEXT("0 - Disable\n")
	TEXT("1 - Final lighting\n")
	TEXT("2 - Reflection View\n")
	TEXT("3 - Surface Cache Coverage\n")
	TEXT("4 - Overview\n")
	TEXT("5 - Albedo\n")
	TEXT("6 - Geometry normals\n")
	TEXT("7 - Normals\n")
	TEXT("8 - Emissive\n")
	TEXT("9 - Opacity\n")
	TEXT("10 - Card weights\n")
	TEXT("11 - Direct lighting\n")
	TEXT("12 - Indirect lighting\n")
	TEXT("13 - Local Position (hardware ray-tracing only)\n")
	TEXT("14 - Velocity (hardware ray-tracing only)\n")
	TEXT("15 - Direct lighting updates\n")
	TEXT("16 - Indirect lighting updates\n")
	TEXT("17 - Last used pages\n")
	TEXT("18 - Last used high res pages"),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneGridPixelSize = 32;
FAutoConsoleVariableRef CVarVisualizeLumenSceneGridPixelSize(
	TEXT("r.Lumen.Visualize.GridPixelSize"),
	GVisualizeLumenSceneGridPixelSize,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GLumenVisualizeVoxels = 0;
FAutoConsoleVariableRef CVarLumenVisualizeVoxels(
	TEXT("r.Lumen.Visualize.Voxels"),
	GLumenVisualizeVoxels,
	TEXT("Visualize Lumen voxel Representation."),
	ECVF_RenderThreadSafe
);

int32 GLumenVisualizeIndirectDiffuse = 0;
FAutoConsoleVariableRef CVarLumenVisualizeIndirectDiffuse(
	TEXT("r.Lumen.Visualize.IndirectDiffuse"),
	GLumenVisualizeIndirectDiffuse,
	TEXT("Visualize Lumen Indirect Diffuse."),
	ECVF_RenderThreadSafe
);

int32 GLumenVisualizeStats = 0;
FAutoConsoleVariableRef CVarLumenVisualizeStats(
	TEXT("r.Lumen.Visualize.Stats"),
	GLumenVisualizeStats,
	TEXT("Print out Lumen scene stats."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneTraceMeshSDFs = 1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneTraceMeshSDFs(
	TEXT("r.Lumen.Visualize.TraceMeshSDFs"),
	GVisualizeLumenSceneTraceMeshSDFs,
	TEXT("Whether to use Mesh SDF tracing for lumen scene visualization."),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenSceneMaxMeshSDFTraceDistance = -1.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardMaxTraceDistance(
	TEXT("r.Lumen.Visualize.MaxMeshSDFTraceDistance"),
	GVisualizeLumenSceneMaxMeshSDFTraceDistance,
	TEXT("Max trace distance for Lumen scene visualization rays. Values below 0 will automatically derrive this from cone angle."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneHiResSurface = 1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneHiResSurface(
	TEXT("r.Lumen.Visualize.HiResSurface"),
	GVisualizeLumenSceneHiResSurface,
	TEXT("Whether visualization should sample highest available surface data or use lowest res always resident pages."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneSurfaceCacheFeedback = 1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneSurfaceCacheFeedback(
	TEXT("r.Lumen.Visualize.SurfaceCacheFeedback"),
	GVisualizeLumenSceneSurfaceCacheFeedback,
	TEXT("Whether visualization should write surface cache feedback requests into the feedback buffer."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneTraceRadianceCache = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneTraceRadianceCache(
	TEXT("r.Lumen.Visualize.TraceRadianceCache"),
	GVisualizeLumenSceneTraceRadianceCache,
	TEXT("Whether to use radiance cache for Lumen scene visualization."),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenSceneConeAngle = 0.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneConeAngle(
	TEXT("r.Lumen.Visualize.ConeAngle"),
	GVisualizeLumenSceneConeAngle,
	TEXT("Visualize cone angle, in degrees."),
	ECVF_RenderThreadSafe
	);

float GVisualizeLumenSceneConeStepFactor = 2.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneConeStepFactor(
	TEXT("r.Lumen.Visualize.ConeStepFactor"),
	GVisualizeLumenSceneConeStepFactor,
	TEXT("Cone step scale on sphere radius step size."),
	ECVF_RenderThreadSafe
	);

float GVisualizeLumenSceneVoxelStepFactor = 1.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneVoxelStepFactor(
	TEXT("r.Lumen.Visualize.VoxelStepFactor"),
	GVisualizeLumenSceneVoxelStepFactor,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

float GVisualizeLumenSceneMinTraceDistance = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneMinTraceDistance(
	TEXT("r.Lumen.Visualize.MinTraceDistance"),
	GVisualizeLumenSceneMinTraceDistance,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenSceneMaxTraceDistance = 100000;
FAutoConsoleVariableRef CVarVisualizeLumenSceneMaxTraceDistance(
	TEXT("r.Lumen.Visualize.MaxTraceDistance"),
	GVisualizeLumenSceneMaxTraceDistance,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneClipmapIndex = -1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneClipmapIndex(
	TEXT("r.Lumen.Visualize.ClipmapIndex"),
	GVisualizeLumenSceneClipmapIndex,
	TEXT("Which clipmap to use for the Lumen scene visualization. -1 uses all possible clipmaps."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenSceneVoxelFaceIndex = -1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneVoxelFaceIndex(
	TEXT("r.Lumen.Visualize.VoxelFaceIndex"),
	GVisualizeLumenSceneVoxelFaceIndex,
	TEXT("Which voxel face to use for the Lumen scene visualization -X,+X,-Y,+Y,-Z,+Z. -1 uses all voxel faces."),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardGenerationSurfels = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardGenerationSurfels(
	TEXT("r.Lumen.Visualize.CardGenerationSurfels"),
	GVisualizeLumenCardGenerationSurfels,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenCardGenerationSurfelScale = 1.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardGenerationSurfelScale(
	TEXT("r.Lumen.Visualize.CardGenerationSurfelScale"),
	GVisualizeLumenCardGenerationSurfelScale,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardGenerationCluster = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardGenerationCluster(
	TEXT("r.Lumen.Visualize.CardGenerationCluster"),
	GVisualizeLumenCardGenerationCluster,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardGenerationMaxSurfel = -1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardGenerationMaxSurfel(
	TEXT("r.Lumen.Visualize.CardGenerationMaxSurfel"),
	GVisualizeLumenCardGenerationMaxSurfel,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardPlacement = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardPlacement(
	TEXT("r.Lumen.Visualize.CardPlacement"),
	GVisualizeLumenCardPlacement,
	TEXT(""),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenCardPlacementDistance = 5000.0f;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardPlacementDistance(
	TEXT("r.Lumen.Visualize.CardPlacementDistance"),
	GVisualizeLumenCardPlacementDistance,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardPlacementLOD = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardPlacementLOD(
	TEXT("r.Lumen.Visualize.CardPlacementLOD"),
	GVisualizeLumenCardPlacementLOD,
	TEXT("0 - all\n")
	TEXT("1 - only primitives\n")
	TEXT("2 - only merged instances\n")
	TEXT("3 - only merged components\n")
	TEXT("4 - only far field\n"),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardPlacementPrimitives = 0;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardPlacementPrimitives(
	TEXT("r.Lumen.Visualize.CardPlacementPrimitives"),
	GVisualizeLumenCardPlacementPrimitives,
	TEXT("Whether to visualize primitive bounding boxes.\n"),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenRayTracingGroups = 0;
FAutoConsoleVariableRef CVarVisualizeLumenRayTracingGroups(
	TEXT("r.Lumen.Visualize.RayTracingGroups"),
	GVisualizeLumenRayTracingGroups,
	TEXT("0 - disable\n")
	TEXT("1 - all groups\n")
	TEXT("2 - groups with a single instance"),
	ECVF_RenderThreadSafe
);

int32 GVisualizeLumenCardPlacementIndex = -1;
FAutoConsoleVariableRef CVarVisualizeLumenSceneCardPlacementIndex(
	TEXT("r.Lumen.Visualize.CardPlacementIndex"),
	GVisualizeLumenCardPlacementIndex,
	TEXT("Visualize only a single card per mesh."),
	ECVF_RenderThreadSafe
);

int32 GLumenSceneDumpStats = 0;
FAutoConsoleVariableRef CVarLumenSceneDumpStats(
	TEXT("r.LumenScene.DumpStats"),
	GLumenSceneDumpStats,
	TEXT("Whether to log Lumen scene stats on the next frame. 2 - dump mesh DF. 3 - dump LumenScene objects."),
	ECVF_RenderThreadSafe
);

float GVisualizeLumenSceneCardInterpolateInfluenceRadius = 10.0f;
FAutoConsoleVariableRef CVarCardInterpolateInfluenceRadius(
	TEXT("r.Lumen.Visualize.CardInterpolateInfluenceRadius"),
	GVisualizeLumenSceneCardInterpolateInfluenceRadius,
	TEXT(""),
	ECVF_RenderThreadSafe
	);

bool Lumen::ShouldVisualizeScene(const FSceneViewFamily& ViewFamily)
{
	return ViewFamily.EngineShowFlags.VisualizeLumen || GLumenVisualize > 0;
}

BEGIN_SHADER_PARAMETER_STRUCT(FLumenVisualizeSceneSoftwareRayTracingParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenVisualizeSceneParameters, CommonParameters)
	SHADER_PARAMETER(FIntVector, VoxelLightingGridResolution)
	SHADER_PARAMETER(float, VisualizeStepFactor)
	SHADER_PARAMETER(float, VoxelStepFactor)
	SHADER_PARAMETER(float, MinTraceDistance)
	SHADER_PARAMETER(float, MaxTraceDistance)
	SHADER_PARAMETER(float, MaxMeshSDFTraceDistanceForVoxelTracing)
	SHADER_PARAMETER(float, MaxMeshSDFTraceDistance)
	SHADER_PARAMETER(float, CardInterpolateInfluenceRadius)
	SHADER_PARAMETER(int, VisualizeClipmapIndex)
	SHADER_PARAMETER(int, VisualizeVoxelFaceIndex)
	SHADER_PARAMETER(int, HeightfieldMaxTracingSteps)
END_SHADER_PARAMETER_STRUCT()

class FVisualizeLumenSceneCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeLumenSceneCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeLumenSceneCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenMeshSDFGridParameters, MeshSDFGridParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenVisualizeSceneSoftwareRayTracingParameters, VisualizeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWSceneColor)
	END_SHADER_PARAMETER_STRUCT()

	class FTraceMeshSDF : SHADER_PERMUTATION_BOOL("TRACE_MESH_SDF");
	class FTraceGlobalSDF : SHADER_PERMUTATION_BOOL("TRACE_GLOBAL_SDF");
	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	class FTraceHeightfields : SHADER_PERMUTATION_BOOL("SCENE_TRACE_HEIGHTFIELDS");

	using FPermutationDomain = TShaderPermutationDomain<FTraceMeshSDF, FTraceGlobalSDF, FRadianceCache, FTraceHeightfields>;

public:
	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
		OutEnvironment.SetDefine(TEXT("ENABLE_VISUALIZE_MODE"), 1);

		// Workaround for an internal PC FXC compiler crash when compiling with disabled optimizations
		if (Parameters.Platform == SP_PCD3D_SM5)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_ForceOptimization);
		}
	}

	static int32 GetGroupSize()
	{
		return 8;
	}
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeLumenSceneCS, "/Engine/Private/Lumen/LumenVisualize.usf", "VisualizeQuadsCS", SF_Compute);

class FVisualizeLumenSceneStatsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeLumenSceneStatsCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeLumenSceneStatsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GlobalDistanceFieldPageFreeListAllocatorBuffer)
		SHADER_PARAMETER(uint32, GlobalDistanceFieldMaxPageNum)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeLumenSceneStatsCS, "/Engine/Private/Lumen/LumenVisualize.usf", "VisualizeStatsCS", SF_Compute);

class FVisualizeLumenVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeLumenVoxelsCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeLumenVoxelsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntRect, ViewDimensions)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenVisualizeSceneSoftwareRayTracingParameters, VisualizeParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, RWSceneColor)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}

	static int32 GetGroupSize()
	{
		return 8;
	}
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeLumenVoxelsCS, "/Engine/Private/Lumen/LumenVisualize.usf", "VisualizeLumenVoxelsCS", SF_Compute);


class FVisualizeTracesVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeTracesVS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeTracesVS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float3>, VisualizeTracesData)
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

IMPLEMENT_GLOBAL_SHADER(FVisualizeTracesVS, "/Engine/Private/Lumen/LumenVisualize.usf", "VisualizeTracesVS", SF_Vertex);


class FVisualizeTracesPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeTracesPS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeTracesPS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
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

IMPLEMENT_GLOBAL_SHADER(FVisualizeTracesPS, "/Engine/Private/Lumen/LumenVisualize.usf", "VisualizeTracesPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FVisualizeTraces, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FVisualizeTracesVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVisualizeTracesPS::FParameters, PS)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()


class FVisualizeTracesVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	/** Destructor. */
	virtual ~FVisualizeTracesVertexDeclaration() {}

	virtual void InitRHI()
	{
		FVertexDeclarationElementList Elements;
		VertexDeclarationRHI = RHICreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI()
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVisualizeTracesVertexDeclaration> GVisualizeTracesVertexDeclaration;


void RenderVisualizeTraces(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures)
{
	extern void GetReflectionsVisualizeTracesBuffer(TRefCountPtr<FRDGPooledBuffer>& VisualizeTracesData);
	extern void GetScreenProbeVisualizeTracesBuffer(TRefCountPtr<FRDGPooledBuffer>& VisualizeTracesData);

	TRefCountPtr<FRDGPooledBuffer> PooledVisualizeTracesData;
	GetReflectionsVisualizeTracesBuffer(PooledVisualizeTracesData);
	GetScreenProbeVisualizeTracesBuffer(PooledVisualizeTracesData);

	if (PooledVisualizeTracesData.IsValid())
	{
		FRDGBufferRef VisualizeTracesData = GraphBuilder.RegisterExternalBuffer(PooledVisualizeTracesData);

		FVisualizeTraces* PassParameters = GraphBuilder.AllocParameters<FVisualizeTraces>();
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilNop);
		PassParameters->VS.View = View.ViewUniformBuffer;
		PassParameters->VS.VisualizeTracesData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VisualizeTracesData, PF_A32B32G32R32F));

		auto VertexShader = View.ShaderMap->GetShader<FVisualizeTracesVS>();
		auto PixelShader = View.ShaderMap->GetShader<FVisualizeTracesPS>();

		const int32 NumPrimitives = LumenScreenProbeGather::GetTracingOctahedronResolution(View) * LumenScreenProbeGather::GetTracingOctahedronResolution(View);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VisualizeTraces"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, VertexShader, PixelShader, &View, NumPrimitives](FRHICommandListImmediate& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB>::GetRHI();

				GraphicsPSOInit.PrimitiveType = PT_LineList;

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GVisualizeTracesVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);
			
				RHICmdList.SetStreamSource(0, nullptr, 0);
				RHICmdList.DrawPrimitive(0, NumPrimitives, 1);
			});
	}
}

void GetVisualizeTileOutputView(const FIntRect& ViewRect, int32 TileIndex, FIntPoint& OutputViewOffset, FIntPoint& OutputViewSize)
{
	if (TileIndex >= 0)
	{
		const FIntPoint TileSize = FMath::DivideAndRoundDown<FIntPoint>(ViewRect.Size() - LumenVisualize::OverviewTileMargin * (LumenVisualize::NumOverviewTilesPerRow + 1), LumenVisualize::NumOverviewTilesPerRow);

		OutputViewSize = TileSize;
		OutputViewOffset.X = ViewRect.Min.X + TileSize.X * TileIndex + LumenVisualize::OverviewTileMargin * (TileIndex + 1);
		OutputViewOffset.Y = ViewRect.Min.Y + LumenVisualize::OverviewTileMargin;
	}
	else
	{
		OutputViewOffset = ViewRect.Min;
		OutputViewSize = ViewRect.Size();
	}
}

void SetupVisualizeParameters(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FIntRect& ViewRect, 
	FRDGTextureRef ColorGradingTexture,
	FRDGTextureRef EyeAdaptationTexture,
	const FLumenCardTracingInputs& TracingInputs, 
	int32 VisualizeMode, 
	int32 VisualizeTileIndex, 
	FLumenVisualizeSceneSoftwareRayTracingParameters& VisualizeParameters)
{
	// FLumenVisualizeSceneParameters
	{
		FLumenVisualizeSceneParameters& CommonParameters = VisualizeParameters.CommonParameters;

		// Texture Level-of-Detail Strategies for Real-Time Ray Tracing https://developer.nvidia.com/raytracinggems Equation 20
		const float RadFOV = (PI / 180.0f) * View.FOV;
		const float PreviewConeAngle = FMath::Max(
			FMath::Clamp(GVisualizeLumenSceneConeAngle, 0.0f, 45.0f) * (float)PI / 180.0f,
			(2.0f * FPlatformMath::Tan(RadFOV * 0.5f)) / ViewRect.Height());

		CommonParameters.PreviewConeAngle = PreviewConeAngle;
		CommonParameters.TanPreviewConeAngle = FMath::Tan(PreviewConeAngle);
		CommonParameters.VisualizeHiResSurface = GVisualizeLumenSceneHiResSurface ? 1 : 0;
		CommonParameters.Tonemap = (EyeAdaptationTexture != nullptr && ColorGradingTexture != nullptr) ? 1 : 0;
		CommonParameters.VisualizeMode = VisualizeMode;

		CommonParameters.InputViewOffset = ViewRect.Min;
		CommonParameters.OutputViewOffset = ViewRect.Min;
		CommonParameters.InputViewSize = ViewRect.Size();
		CommonParameters.OutputViewSize = ViewRect.Size();
		CommonParameters.ColorGradingLUT = ColorGradingTexture;
		CommonParameters.ColorGradingLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		CommonParameters.EyeAdaptationTexture = EyeAdaptationTexture;

		if (!CommonParameters.EyeAdaptationTexture)
		{
			CommonParameters.EyeAdaptationTexture = FRDGSystemTextures::Get(GraphBuilder).Black;
		}

		if (!CommonParameters.ColorGradingLUT)
		{
			CommonParameters.ColorGradingLUT = FRDGSystemTextures::Get(GraphBuilder).VolumetricBlack;
		}

		GetVisualizeTileOutputView(ViewRect, VisualizeTileIndex, CommonParameters.OutputViewOffset, CommonParameters.OutputViewSize);
	}

	// FLumenVisualizeSceneSoftwareRayTracingParameters
	{
		float MaxMeshSDFTraceDistance = GVisualizeLumenSceneMaxMeshSDFTraceDistance;
		float MaxTraceDistance = GVisualizeLumenSceneMaxTraceDistance;

		// Reflection scene view uses reflection setup
		if (VisualizeMode == VISUALIZE_MODE_REFLECTION_VIEW)
		{
			extern FLumenGatherCvarState GLumenGatherCvars;
			MaxMeshSDFTraceDistance = GLumenGatherCvars.MeshSDFTraceDistance;
			MaxTraceDistance = Lumen::GetMaxTraceDistance(View);
		}

		bool bTraceMeshSDF = GVisualizeLumenSceneTraceMeshSDFs != 0 && View.Family->EngineShowFlags.LumenDetailTraces;
		if (!bTraceMeshSDF)
		{
			MaxMeshSDFTraceDistance = 0.0f;
		}

		VisualizeParameters.VoxelLightingGridResolution = TracingInputs.VoxelGridResolution;
		VisualizeParameters.VisualizeStepFactor = FMath::Clamp(GVisualizeLumenSceneConeStepFactor, .1f, 10.0f);
		VisualizeParameters.VoxelStepFactor = FMath::Clamp(GVisualizeLumenSceneVoxelStepFactor, .1f, 10.0f);
		VisualizeParameters.MinTraceDistance = FMath::Clamp(GVisualizeLumenSceneMinTraceDistance, .01f, 1000.0f);
		VisualizeParameters.MaxTraceDistance = FMath::Clamp(MaxTraceDistance, .01f, Lumen::MaxTraceDistance);
		VisualizeParameters.VisualizeClipmapIndex = FMath::Clamp(GVisualizeLumenSceneClipmapIndex, -1, TracingInputs.NumClipmapLevels - 1);
		VisualizeParameters.VisualizeVoxelFaceIndex = FMath::Clamp(GVisualizeLumenSceneVoxelFaceIndex, -1, 5);
		VisualizeParameters.CardInterpolateInfluenceRadius = GVisualizeLumenSceneCardInterpolateInfluenceRadius;

		if (MaxMeshSDFTraceDistance <= 0)
		{
			MaxMeshSDFTraceDistance = FMath::Clamp<float>(
				TracingInputs.ClipmapVoxelSizeAndRadius[0].W / FMath::Max(VisualizeParameters.CommonParameters.TanPreviewConeAngle, 0.001f),
				VisualizeParameters.MinTraceDistance,
				VisualizeParameters.MaxTraceDistance);
		}

		VisualizeParameters.MaxMeshSDFTraceDistanceForVoxelTracing = FMath::Clamp(MaxMeshSDFTraceDistance, VisualizeParameters.MinTraceDistance, VisualizeParameters.MaxTraceDistance);
		VisualizeParameters.MaxMeshSDFTraceDistance = FMath::Clamp(MaxMeshSDFTraceDistance, VisualizeParameters.MinTraceDistance, VisualizeParameters.MaxTraceDistance);
		VisualizeParameters.HeightfieldMaxTracingSteps = Lumen::GetHeightfieldMaxTracingSteps();
	}
}

void FDeferredShadingSceneRenderer::RenderLumenMiscVisualizations(FRDGBuilder& GraphBuilder, const FMinimalSceneTextures& SceneTextures, FLumenSceneFrameTemporaries& FrameTemporaries)
{
	const FViewInfo& View = Views[0];
	const FPerViewPipelineState& ViewPipelineState = GetViewPipelineState(View);
	const bool bAnyLumenActive = ViewPipelineState.DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen || ViewPipelineState.ReflectionsMethod == EReflectionsMethod::Lumen;

	if (Lumen::IsLumenFeatureAllowedForView(Scene, View) && bAnyLumenActive)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "MiscLumenVisualization");

		RenderVisualizeTraces(GraphBuilder, View, SceneTextures);

		if (Lumen::ShouldVisualizeHardwareRayTracing(ViewFamily) || Lumen::IsSoftwareRayTracingSupported())
		{
			FLumenCardTracingInputs TracingInputs(GraphBuilder, Scene, View, FrameTemporaries, /*bSurfaceCacheFeedback*/ GVisualizeLumenSceneSurfaceCacheFeedback != 0);

			if (GLumenVisualizeVoxels != 0)
			{
				FLumenVisualizeSceneSoftwareRayTracingParameters VisualizeParameters;
				SetupVisualizeParameters(GraphBuilder, View, View.ViewRect, TryRegisterExternalTexture(GraphBuilder, View.GetTonemappingLUT()), GetEyeAdaptationTexture(GraphBuilder, View), TracingInputs, VISUALIZE_MODE_LUMEN_SCENE, -1, VisualizeParameters);

				FVisualizeLumenVoxelsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeLumenVoxelsCS::FParameters>();
				PassParameters->ViewDimensions = View.ViewRect;
				PassParameters->RWSceneColor = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SceneTextures.Color.Resolve));
				PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
				PassParameters->VisualizeParameters = VisualizeParameters;
				GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);

				auto ComputeShader = View.ShaderMap->GetShader< FVisualizeLumenVoxelsCS >();
				FIntPoint GroupSize(FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), FVisualizeLumenVoxelsCS::GetGroupSize()));

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("LumenVoxelsVisualization"),
					ComputeShader,
					PassParameters,
					FIntVector(GroupSize.X, GroupSize.Y, 1));
			}
		}
	}

	if (bAnyLumenActive
		&& GLumenVisualizeStats != 0
		&& View.GlobalDistanceFieldInfo.PageFreeListAllocatorBuffer)
	{
		FRDGBufferRef GlobalDistanceFieldPageFreeListAllocatorBuffer = GraphBuilder.RegisterExternalBuffer(View.GlobalDistanceFieldInfo.PageFreeListAllocatorBuffer, TEXT("PageFreeListAllocator"));

		FVisualizeLumenSceneStatsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeLumenSceneStatsCS::FParameters>();
		ShaderPrint::SetParameters(GraphBuilder, View, PassParameters->ShaderPrintUniformBuffer);
		PassParameters->GlobalDistanceFieldPageFreeListAllocatorBuffer = GraphBuilder.CreateSRV(GlobalDistanceFieldPageFreeListAllocatorBuffer, PF_R32_UINT);
		PassParameters->GlobalDistanceFieldMaxPageNum = View.GlobalDistanceFieldInfo.ParameterData.MaxPageNum;

		auto ComputeShader = View.ShaderMap->GetShader<FVisualizeLumenSceneStatsCS>();

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("LumenSceneStats"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	RenderLumenRadianceCacheVisualization(GraphBuilder, SceneTextures);

	if (GLumenSceneDumpStats)
	{
		FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;
		const FDistanceFieldSceneData& DistanceFieldSceneData = Scene->DistanceFieldSceneData;

		LumenSceneData.DumpStats(
			DistanceFieldSceneData,
			/*bDumpMeshDistanceFields*/ GLumenSceneDumpStats == 2,
			/*bDumpPrimitiveGroups*/ GLumenSceneDumpStats == 3);

		GLumenSceneDumpStats = 0;
	}
}

LumenRadianceCache::FRadianceCacheInputs GetFinalGatherRadianceCacheInputsForVisualize(const FViewInfo& View)
{
	if (GLumenIrradianceFieldGather)
	{
		return LumenIrradianceFieldGather::SetupRadianceCacheInputs();
	}
	else
	{
		return LumenScreenProbeGatherRadianceCache::SetupRadianceCacheInputs(View);
	}
}

void VisualizeLumenScene(
	const FScene* Scene,
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FScreenPassTexture Output,
	FRDGTextureRef ColorGradingTexture,
	FRDGTextureRef EyeAdaptationTexture,
	const FLumenCardTracingInputs& TracingInputs,
	int32 VisualizeMode,
	int32 VisualizeTileIndex)
{
	FRDGTextureUAVRef SceneColorUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Output.Texture));

	FLumenVisualizeSceneSoftwareRayTracingParameters VisualizeParameters;
	SetupVisualizeParameters(GraphBuilder, View, Output.ViewRect, ColorGradingTexture, EyeAdaptationTexture, TracingInputs, VisualizeMode, VisualizeTileIndex, VisualizeParameters);

	const FRadianceCacheState& RadianceCacheState = View.ViewState->RadianceCacheState;
	const LumenRadianceCache::FRadianceCacheInputs RadianceCacheInputs = GetFinalGatherRadianceCacheInputsForVisualize(View);

	if (Lumen::ShouldVisualizeHardwareRayTracing(*View.Family))
	{
		FLumenIndirectTracingParameters IndirectTracingParameters;
		IndirectTracingParameters.CardInterpolateInfluenceRadius = VisualizeParameters.CardInterpolateInfluenceRadius;
		IndirectTracingParameters.MinTraceDistance = VisualizeParameters.MinTraceDistance;
		IndirectTracingParameters.MaxTraceDistance = VisualizeParameters.MaxTraceDistance;
		IndirectTracingParameters.MaxMeshSDFTraceDistance = VisualizeParameters.MaxMeshSDFTraceDistance;

		const bool bVisualizeModeWithHitLighting = (VisualizeMode == VISUALIZE_MODE_LUMEN_SCENE || VisualizeMode == VISUALIZE_MODE_REFLECTION_VIEW);

		LumenVisualize::VisualizeHardwareRayTracing(
			GraphBuilder,
			Scene,
			GetSceneTextureParameters(GraphBuilder),
			View,
			TracingInputs,
			IndirectTracingParameters,
			VisualizeParameters.CommonParameters,
			Output.Texture,
			bVisualizeModeWithHitLighting);
	}
	else
	{
		const uint32 CullGridPixelSize = FMath::Clamp(GVisualizeLumenSceneGridPixelSize, 8, 1024);
		const FIntPoint CullGridSizeXY = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), CullGridPixelSize);
		const FIntVector CullGridSize = FIntVector(CullGridSizeXY.X, CullGridSizeXY.Y, 1);

		FLumenMeshSDFGridParameters MeshSDFGridParameters;
		MeshSDFGridParameters.CardGridPixelSizeShift = FMath::FloorLog2(CullGridPixelSize);
		MeshSDFGridParameters.CullGridSize = CullGridSize;

		{
			const float CardTraceEndDistanceFromCamera = VisualizeParameters.MaxMeshSDFTraceDistance;

			CullMeshObjectsToViewGrid(
				View,
				Scene,
				0,
				CardTraceEndDistanceFromCamera,
				CullGridPixelSize,
				1,
				FVector::ZeroVector,
				GraphBuilder,
				MeshSDFGridParameters);
		}

		const bool bTraceGlobalSDF = Lumen::UseGlobalSDFTracing(*View.Family);
		const bool bTraceMeshSDF = Lumen::UseMeshSDFTracing(*View.Family)
			&& MeshSDFGridParameters.TracingParameters.DistanceFieldObjectBuffers.NumSceneObjects > 0
			&& VisualizeParameters.MaxMeshSDFTraceDistance > VisualizeParameters.MinTraceDistance;

		FVisualizeLumenSceneCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeLumenSceneCS::FParameters>();
		PassParameters->RWSceneColor = SceneColorUAV;
		PassParameters->MeshSDFGridParameters = MeshSDFGridParameters;
		PassParameters->VisualizeParameters = VisualizeParameters;
		LumenRadianceCache::GetInterpolationParameters(View, GraphBuilder, RadianceCacheState, RadianceCacheInputs, PassParameters->RadianceCacheParameters);
		GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);

		FVisualizeLumenSceneCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVisualizeLumenSceneCS::FTraceMeshSDF>(bTraceMeshSDF);
		PermutationVector.Set<FVisualizeLumenSceneCS::FTraceGlobalSDF>(bTraceGlobalSDF);
		PermutationVector.Set<FVisualizeLumenSceneCS::FRadianceCache>(GVisualizeLumenSceneTraceRadianceCache != 0 && LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set<FVisualizeLumenSceneCS::FTraceHeightfields>(Lumen::UseHeightfieldTracing(*View.Family, *Scene->LumenSceneData));
		PermutationVector = FVisualizeLumenSceneCS::RemapPermutation(PermutationVector);

		auto ComputeShader = View.ShaderMap->GetShader<FVisualizeLumenSceneCS>(PermutationVector);
		FIntPoint GroupSize(FIntPoint::DivideAndRoundUp(VisualizeParameters.CommonParameters.OutputViewSize, FVisualizeLumenSceneCS::GetGroupSize()));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("LumenSceneVisualization"),
			ComputeShader,
			PassParameters,
			FIntVector(GroupSize.X, GroupSize.Y, 1));
	}
}

int32 GetLumenVisualizeMode(const FViewInfo& View)
{
	const FLumenVisualizationData& VisualizationData = GetLumenVisualizationData();
	const int32 VisualizeMode = GLumenVisualize > 0 ? GLumenVisualize : VisualizationData.GetModeID(View.CurrentLumenVisualizationMode);
	return VisualizeMode;
}

FScreenPassTexture AddVisualizeLumenScenePass(FRDGBuilder& GraphBuilder, const FViewInfo& View, bool bAnyLumenActive, const FVisualizeLumenSceneInputs& Inputs, FLumenSceneFrameTemporaries& FrameTemporaries)
{
	check(Inputs.SceneColor.IsValid());

	FScreenPassTexture Output = Inputs.SceneColor;

	const FScene* Scene = (const FScene*)View.Family->Scene;
	const FSceneViewFamily& ViewFamily = *View.Family;

	if (Lumen::IsLumenFeatureAllowedForView(Scene, View) && bAnyLumenActive)
	{
		const bool bVisualizeScene = Lumen::ShouldVisualizeScene(ViewFamily);

		if (bVisualizeScene && (Lumen::ShouldVisualizeHardwareRayTracing(ViewFamily) || Lumen::IsSoftwareRayTracingSupported()))
		{
			RDG_EVENT_SCOPE(GraphBuilder, "VisualizeLumenScene");

			// Create a new output just to make sure the right flags are set
			FRDGTextureDesc VisualizeOutputDesc = Inputs.SceneColor.Texture->Desc;
			VisualizeOutputDesc.Flags |= TexCreate_UAV | TexCreate_RenderTargetable;
			Output = FScreenPassTexture(GraphBuilder.CreateTexture(VisualizeOutputDesc, TEXT("VisualizeLumenScene")), Inputs.SceneColor.ViewRect);

			const int32 VisualizeMode = GetLumenVisualizeMode(View);

			// In the overview mode we don't fully overwrite, copy the old Scene Color
			if (VisualizeMode == VISUALIZE_MODE_OVERVIEW)
			{
				FRHICopyTextureInfo CopyInfo;

				AddCopyTexturePass(
					GraphBuilder,
					Inputs.SceneColor.Texture,
					Output.Texture,
					CopyInfo);
			}

			FLumenCardTracingInputs TracingInputs(GraphBuilder, Scene, View, FrameTemporaries, /*bSurfaceCacheFeedback*/ GVisualizeLumenSceneSurfaceCacheFeedback != 0);

			if (VisualizeMode == VISUALIZE_MODE_OVERVIEW)
			{
				struct FVisualizeTile
				{
					int32 Mode;
					const TCHAR* Name;
				};

				FVisualizeTile VisualizeTiles[LumenVisualize::NumOverviewTilesPerRow];
				VisualizeTiles[0].Mode = VISUALIZE_MODE_LUMEN_SCENE;
				VisualizeTiles[0].Name = TEXT("Lumen Scene");
				VisualizeTiles[1].Mode = VISUALIZE_MODE_REFLECTION_VIEW;
				VisualizeTiles[1].Name = TEXT("Reflection View");
				VisualizeTiles[2].Mode = VISUALIZE_MODE_SURFACE_CACHE;
				VisualizeTiles[2].Name = TEXT("Surface Cache");

				for (int32 TileIndex = 0; TileIndex < LumenVisualize::NumOverviewTilesPerRow; ++TileIndex)
				{
					VisualizeLumenScene(Scene, GraphBuilder, View, Output, Inputs.ColorGradingTexture, Inputs.EyeAdaptationTexture, TracingInputs, VisualizeTiles[TileIndex].Mode, TileIndex);	
				}

				AddDrawCanvasPass(GraphBuilder, RDG_EVENT_NAME("LumenVisualizeLabels"), View, FScreenPassRenderTarget(Output, ERenderTargetLoadAction::ELoad),
					[&ViewRect = Inputs.SceneColor.ViewRect, &VisualizeTiles](FCanvas& Canvas)
				{
					const FLinearColor LabelColor(1, 1, 0);

					for (int32 TileIndex = 0; TileIndex < LumenVisualize::NumOverviewTilesPerRow; ++TileIndex)
					{
						FIntPoint OutputViewSize;
						FIntPoint OutputViewOffset;
						GetVisualizeTileOutputView(ViewRect, TileIndex, OutputViewOffset, OutputViewSize);

						Canvas.DrawShadowedString(OutputViewOffset.X + 2 * LumenVisualize::OverviewTileMargin, OutputViewOffset.Y + OutputViewSize.Y - 20, VisualizeTiles[TileIndex].Name, GetStatsFont(), LabelColor);
					}
				});
			}
			else
			{
				VisualizeLumenScene(Scene, GraphBuilder, View, Output, Inputs.ColorGradingTexture, Inputs.EyeAdaptationTexture, TracingInputs, VisualizeMode, /*VisualizeTileIndex*/ -1);
			}
		}
	}

	if (Inputs.OverrideOutput.IsValid())
	{
		AddDrawTexturePass(GraphBuilder, View, Output, Inputs.OverrideOutput);
		return Inputs.OverrideOutput;
	}

	return MoveTemp(Output);
}

void AddBoxFaceTriangles(FDynamicMeshBuilder& MeshBuilder, int32 FaceIndex)
{
	const int32 BoxIndices[6][4] =
	{
		{ 0, 2, 3, 1 },	// back, -z
		{ 4, 5, 7, 6 },	// front, +z
		{ 0, 4, 6, 2 },	// left, -x
		{ 1, 3, 7, 5 },	// right, +x,
		{ 0, 4, 5, 1 },	// bottom, -y
		{ 2, 3, 7, 6 }	// top, +y
	};

	MeshBuilder.AddTriangle(BoxIndices[FaceIndex][0], BoxIndices[FaceIndex][2], BoxIndices[FaceIndex][1]);
	MeshBuilder.AddTriangle(BoxIndices[FaceIndex][0], BoxIndices[FaceIndex][3], BoxIndices[FaceIndex][2]);
}

void DrawPrimitiveBounds(const FLumenPrimitiveGroup& PrimitiveGroup, FLinearColor BoundsColor, FViewElementPDI& ViewPDI)
{
	const uint8 DepthPriority = SDPG_World;

	for (const FPrimitiveSceneInfo* ScenePrimitiveInfo : PrimitiveGroup.Primitives)
	{
		const FMatrix& PrimitiveToWorld = ScenePrimitiveInfo->Proxy->GetLocalToWorld();
		const TConstArrayView<FPrimitiveInstance> InstanceSceneData = ScenePrimitiveInfo->Proxy->GetInstanceSceneData();

		if (InstanceSceneData.Num() > 0)
		{
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceSceneData.Num(); ++InstanceIndex)
			{
				const FPrimitiveInstance& PrimitiveInstance = InstanceSceneData[InstanceIndex];
				const FBox LocalBoundingBox = ScenePrimitiveInfo->Proxy->GetInstanceLocalBounds(InstanceIndex).ToBox();
				FMatrix LocalToWorld = PrimitiveInstance.LocalToPrimitive.ToMatrix() * PrimitiveToWorld;
				DrawWireBox(&ViewPDI, LocalToWorld, LocalBoundingBox, BoundsColor, DepthPriority);
			}
		}
		else
		{
			const FBox LocalBoundingBox = ScenePrimitiveInfo->Proxy->GetLocalBounds().GetBox();
			DrawWireBox(&ViewPDI, PrimitiveToWorld, LocalBoundingBox, BoundsColor, DepthPriority);
		}
	}
}

void DrawSurfels(const TArray<FLumenCardBuildDebugData::FSurfel>& Surfels, const FMatrix& PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType SurfelType, FLinearColor SurfelColor, FViewElementPDI& ViewPDI, float SurfelRadius = 2.0f)
{
	FColoredMaterialRenderProxy* MaterialRenderProxy = new(FMemStack::Get()) FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), SurfelColor);

	FDynamicMeshBuilder MeshBuilder(ViewPDI.View->GetFeatureLevel());

	int32 NumSurfels = 0;
	FVector3f NormalSum(0.0f, 0.0f, 0.0f);
	FBox LocalBounds;
	LocalBounds.Init();

	const FMatrix WorldToPrimitiveT = PrimitiveToWorld.Inverse().GetTransposed();

	int32 BaseVertex = 0;
	for (int32 SurfelIndex = 0; SurfelIndex < Surfels.Num(); ++SurfelIndex)
	{
		if (GVisualizeLumenCardGenerationMaxSurfel >= 0 && NumSurfels >= GVisualizeLumenCardGenerationMaxSurfel)
		{
			break;
		}

		const FLumenCardBuildDebugData::FSurfel& Surfel = Surfels[SurfelIndex];
		if (Surfel.Type == SurfelType)
		{
			FVector3f DiskPosition = (FVector4f)PrimitiveToWorld.TransformPosition((FVector)Surfel.Position);
			FVector3f DiskNormal = (FVector4f)WorldToPrimitiveT.TransformVector((FVector)Surfel.Normal).GetSafeNormal();

			// Surface bias
			DiskPosition += DiskNormal * 0.5f;

			FVector3f AxisX;
			FVector3f AxisY;
			DiskNormal.FindBestAxisVectors(AxisX, AxisY);

			const int32 NumSides = 6;
			const float	AngleDelta = 2.0f * PI / NumSides;
			for (int32 SideIndex = 0; SideIndex < NumSides; ++SideIndex)
			{
				const FVector3f VertexPosition = DiskPosition + (AxisX * FMath::Cos(AngleDelta * (SideIndex)) + AxisY * FMath::Sin(AngleDelta * (SideIndex))) * SurfelRadius * GVisualizeLumenCardGenerationSurfelScale;

				MeshBuilder.AddVertex(VertexPosition, FVector2f(0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1), FColor::White);
			}

			for (int32 SideIndex = 0; SideIndex < NumSides - 1; ++SideIndex)
			{
				int32 V0 = BaseVertex + 0;
				int32 V1 = BaseVertex + SideIndex;
				int32 V2 = BaseVertex + (SideIndex + 1);

				MeshBuilder.AddTriangle(V0, V1, V2);
			}
			BaseVertex += NumSides;
			NormalSum += DiskNormal;
			++NumSurfels;

			LocalBounds += (FVector)Surfel.Position;
		}
	}

	const uint8 DepthPriority = SDPG_World;
	MeshBuilder.Draw(&ViewPDI, FMatrix::Identity, MaterialRenderProxy, DepthPriority, false);

	if (SurfelType == FLumenCardBuildDebugData::ESurfelType::Cluster 
		&& GVisualizeLumenCardGenerationMaxSurfel >= 0)
	{
		LocalBounds = LocalBounds.ExpandBy(1.0f);

		DrawWireBox(&ViewPDI, PrimitiveToWorld, LocalBounds, FLinearColor::Yellow, DepthPriority);

		const FVector Start = PrimitiveToWorld.TransformPosition(LocalBounds.GetCenter());
		const FVector End = PrimitiveToWorld.TransformPosition(LocalBounds.GetCenter() + (FVector)NormalSum.GetSafeNormal() * 1000.0f);
		ViewPDI.DrawLine(Start, End, FLinearColor::Red, 0, 0.2f, 0.0f, false);
	}
}

void VisualizeRayTracingGroups(const FViewInfo& View, const FLumenSceneData& LumenSceneData, FViewElementPDI& ViewPDI)
{
	if (GVisualizeLumenRayTracingGroups == 0)
	{
		return;
	}

	FConvexVolume ViewFrustum;
	GetViewFrustumBounds(ViewFrustum, View.ViewMatrices.GetViewProjectionMatrix(), true);
	
	for (const FLumenPrimitiveGroup& PrimitiveGroup : LumenSceneData.PrimitiveGroups)
	{
		if ((GVisualizeLumenRayTracingGroups != 2 || !PrimitiveGroup.HasMergedInstances())
			&& PrimitiveGroup.HasMergedPrimitives()
			&& PrimitiveGroup.WorldSpaceBoundingBox.ComputeSquaredDistanceToPoint((FVector3f)View.ViewMatrices.GetViewOrigin()) < GVisualizeLumenCardPlacementDistance * GVisualizeLumenCardPlacementDistance
			&& ViewFrustum.IntersectBox((FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetCenter(), (FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetExtent()))
		{
			const uint32 GroupIdHash = GetTypeHash(PrimitiveGroup.RayTracingGroupMapElementId.GetIndex());
			const uint8 DepthPriority = SDPG_World;
			const uint8 Hue = GroupIdHash & 0xFF;
			const uint8 Saturation = 0xFF;
			const uint8 Value = 0xFF;

			FLinearColor GroupColor = FLinearColor::MakeFromHSV8(Hue, Saturation, Value);
			GroupColor.A = 1.0f;

			DrawPrimitiveBounds(PrimitiveGroup, GroupColor, ViewPDI);
		}
	}
}

void VisualizeCardPlacement(const FViewInfo& View, const FLumenSceneData& LumenSceneData, FViewElementPDI& ViewPDI)
{
	if (GVisualizeLumenCardPlacement == 0 && GVisualizeLumenCardGenerationCluster == 0)
	{
		return;
	}

	FConvexVolume ViewFrustum;
	GetViewFrustumBounds(ViewFrustum, View.ViewMatrices.GetViewProjectionMatrix(), true);

	for (const FLumenPrimitiveGroup& PrimitiveGroup : LumenSceneData.PrimitiveGroups)
	{
		bool bVisible = PrimitiveGroup.MeshCardsIndex >= 0;

		switch (GVisualizeLumenCardPlacementLOD)
		{
		case 1:
			bVisible = bVisible && !PrimitiveGroup.HasMergedInstances();
			break;

		case 2:
			bVisible = bVisible && PrimitiveGroup.HasMergedInstances() && !PrimitiveGroup.HasMergedPrimitives();
			break;

		case 3:
			bVisible = bVisible && PrimitiveGroup.HasMergedInstances() && PrimitiveGroup.HasMergedPrimitives();
			break;

		case 4:
			bVisible = bVisible && PrimitiveGroup.bFarField;
			break;
		}

		if (bVisible
			&& PrimitiveGroup.WorldSpaceBoundingBox.ComputeSquaredDistanceToPoint((FVector3f)View.ViewMatrices.GetViewOrigin()) < GVisualizeLumenCardPlacementDistance * GVisualizeLumenCardPlacementDistance
			&& ViewFrustum.IntersectBox((FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetCenter(), (FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetExtent()))
		{
			const FLumenMeshCards& MeshCardsEntry = LumenSceneData.MeshCards[PrimitiveGroup.MeshCardsIndex];

			for (uint32 CardIndex = MeshCardsEntry.FirstCardIndex; CardIndex < MeshCardsEntry.FirstCardIndex + MeshCardsEntry.NumCards; ++CardIndex)
			{
				const FLumenCard& Card = LumenSceneData.Cards[CardIndex];

				bVisible = Card.bVisible;

				if (GVisualizeLumenCardPlacementIndex >= 0 && Card.IndexInMeshCards != GVisualizeLumenCardPlacementIndex)
				{
					bVisible = false;
				}

				if (bVisible)
				{
					uint32 CardHash = HashCombine(GetTypeHash(Card.LocalOBB.Origin), GetTypeHash(Card.LocalOBB.Extent));
					CardHash = HashCombine(CardHash, GetTypeHash(Card.LocalOBB.AxisZ));
					CardHash = HashCombine(CardHash, GetTypeHash(CardIndex));

					const uint8 DepthPriority = SDPG_World;
					const uint8 CardHue = CardHash & 0xFF;
					const uint8 CardSaturation = 0xFF;
					const uint8 CardValue = 0xFF;

					FLinearColor CardColor = FLinearColor::MakeFromHSV8(CardHue, CardSaturation, CardValue);
					CardColor.A = 1.0f;

					const FMatrix CardToWorld(Card.WorldOBB.GetCardToLocal());
					const FBox LocalBounds(-Card.WorldOBB.Extent, Card.WorldOBB.Extent);

					DrawWireBox(&ViewPDI, CardToWorld, LocalBounds, CardColor, DepthPriority);

					// Visualize bounds of primitives which make current card
					if (GVisualizeLumenCardPlacementPrimitives != 0 && PrimitiveGroup.HasMergedInstances())
					{
						DrawPrimitiveBounds(PrimitiveGroup, CardColor, ViewPDI);
					}

					// Draw card "projection face"
					{
						CardColor.A = 0.25f;

						FColoredMaterialRenderProxy* MaterialRenderProxy = new(FMemStack::Get()) FColoredMaterialRenderProxy(GEngine->EmissiveMeshMaterial->GetRenderProxy(), CardColor, NAME_Color);

						FDynamicMeshBuilder MeshBuilder(ViewPDI.View->GetFeatureLevel());

						for (int32 VertIndex = 0; VertIndex < 8; ++VertIndex)
						{
							FVector BoxVertex;
							BoxVertex.X = VertIndex & 0x1 ? LocalBounds.Max.X : LocalBounds.Min.X;
							BoxVertex.Y = VertIndex & 0x2 ? LocalBounds.Max.Y : LocalBounds.Min.Y;
							BoxVertex.Z = VertIndex & 0x4 ? LocalBounds.Max.Z : LocalBounds.Min.Z;
							MeshBuilder.AddVertex((FVector3f)BoxVertex, FVector2f(0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1), FColor::White);
						}

						AddBoxFaceTriangles(MeshBuilder, 1);

						MeshBuilder.Draw(&ViewPDI, CardToWorld, MaterialRenderProxy, DepthPriority, false);
					}
				}
			}
		}
	}
}

void VisualizeCardGeneration(const FViewInfo& View, const FLumenSceneData& LumenSceneData, FViewElementPDI& ViewPDI)
{
	if (GVisualizeLumenCardGenerationSurfels == 0 
		&& GVisualizeLumenCardGenerationCluster == 0)
	{
		return;
	}

	FConvexVolume ViewFrustum;
	GetViewFrustumBounds(ViewFrustum, View.ViewMatrices.GetViewProjectionMatrix(), true);

	for (const FLumenPrimitiveGroup& PrimitiveGroup : LumenSceneData.PrimitiveGroups)
	{
		if (PrimitiveGroup.WorldSpaceBoundingBox.ComputeSquaredDistanceToPoint((FVector3f)View.ViewMatrices.GetViewOrigin()) < GVisualizeLumenCardPlacementDistance * GVisualizeLumenCardPlacementDistance
			&& ViewFrustum.IntersectBox((FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetCenter(), (FVector)PrimitiveGroup.WorldSpaceBoundingBox.GetExtent()))
		{
			for (const FPrimitiveSceneInfo* PrimitiveSceneInfo : PrimitiveGroup.Primitives)
			{
				if (PrimitiveSceneInfo && PrimitiveSceneInfo->Proxy)
				{
					const FCardRepresentationData* CardRepresentationData = PrimitiveSceneInfo->Proxy->GetMeshCardRepresentation();
					if (CardRepresentationData)
					{
						const uint8 DepthPriority = SDPG_World;
						const FMatrix PrimitiveToWorld = PrimitiveSceneInfo->Proxy->GetLocalToWorld();
						const FLumenCardBuildDebugData& DebugData = CardRepresentationData->MeshCardsBuildData.DebugData;

						if (GVisualizeLumenCardGenerationSurfels)
						{
							DrawSurfels(DebugData.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Valid, FLinearColor::Green, ViewPDI);
							DrawSurfels(DebugData.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Invalid, FLinearColor::Red, ViewPDI);

							for (const FLumenCardBuildDebugData::FRay& Ray : DebugData.SurfelRays)
							{
								const FVector Start = PrimitiveToWorld.TransformPosition((FVector)Ray.RayStart);
								const FVector End = PrimitiveToWorld.TransformPosition((FVector)Ray.RayEnd);
								ViewPDI.DrawLine(Start, End, Ray.bHit ? FLinearColor::Red : FLinearColor::White, 0, 0.2f, 0.0f, false);
							}
						}

						if (GVisualizeLumenCardGenerationSurfels == 0 && GVisualizeLumenCardGenerationCluster != 0 && GVisualizeLumenCardPlacementIndex >= 0 && PrimitiveGroup.MeshCardsIndex >= 0)
						{
							const FLumenMeshCards& MeshCardsEntry = LumenSceneData.MeshCards[PrimitiveGroup.MeshCardsIndex];
							for (uint32 CardIndex = MeshCardsEntry.FirstCardIndex; CardIndex < MeshCardsEntry.FirstCardIndex + MeshCardsEntry.NumCards; ++CardIndex)
							{
								const FLumenCard& Card = LumenSceneData.Cards[CardIndex];

								if (Card.IndexInMeshCards == GVisualizeLumenCardPlacementIndex && Card.IndexInBuildData < DebugData.Clusters.Num())
								{
									const FLumenCardBuildDebugData::FSurfelCluster& Cluster = DebugData.Clusters[Card.IndexInBuildData];

									DrawSurfels(Cluster.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Cluster, FLinearColor::Green, ViewPDI);
									DrawSurfels(Cluster.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Used, FLinearColor::Gray, ViewPDI);
									DrawSurfels(Cluster.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Idle, FLinearColor::Blue, ViewPDI);
									DrawSurfels(Cluster.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Seed, FLinearColor::Yellow, ViewPDI, 10.0f);
									DrawSurfels(Cluster.Surfels, PrimitiveToWorld, FLumenCardBuildDebugData::ESurfelType::Seed2, FLinearColor::Red, ViewPDI, 8.0f);

									for (const FLumenCardBuildDebugData::FRay& Ray : Cluster.Rays)
									{
										const FVector Start = PrimitiveToWorld.TransformPosition((FVector)Ray.RayStart);
										const FVector End = PrimitiveToWorld.TransformPosition((FVector)Ray.RayEnd);
										ViewPDI.DrawLine(Start, End, Ray.bHit ? FLinearColor::Red : FLinearColor::White, 0, 0.2f, 0.0f, false);
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void FDeferredShadingSceneRenderer::LumenScenePDIVisualization()
{
	FLumenSceneData& LumenSceneData = *Scene->LumenSceneData;

	const bool bAnyLumenEnabled = ShouldRenderLumenDiffuseGI(Scene, Views[0])
		|| ShouldRenderLumenReflections(Views[0]);

	if (bAnyLumenEnabled)
	{
		if (GVisualizeLumenCardPlacement != 0
			|| GVisualizeLumenRayTracingGroups != 0
			|| GVisualizeLumenCardGenerationCluster != 0
			|| GVisualizeLumenCardGenerationSurfels != 0)
		{
			FViewElementPDI ViewPDI(&Views[0], nullptr, &Views[0].DynamicPrimitiveCollector);
			VisualizeRayTracingGroups(Views[0], LumenSceneData, ViewPDI);
			VisualizeCardPlacement(Views[0], LumenSceneData, ViewPDI);
			VisualizeCardGeneration(Views[0], LumenSceneData, ViewPDI);
		}
	}

	static bool bVisualizeLumenSceneViewOrigin = false;

	if (bVisualizeLumenSceneViewOrigin)
	{
		const int32 NumClipmaps = GetNumLumenVoxelClipmaps(Views[0].FinalPostProcessSettings.LumenSceneViewDistance);

		for (int32 ClipmapIndex = 0; ClipmapIndex < NumClipmaps; ClipmapIndex++)
		{
			FViewElementPDI ViewPDI(&Views[0], nullptr, &Views[0].DynamicPrimitiveCollector);
			const uint8 MarkerHue = (ClipmapIndex * 100) & 0xFF;
			const uint8 MarkerSaturation = 0xFF;
			const uint8 MarkerValue = 0xFF;

			FLinearColor MarkerColor = FLinearColor::MakeFromHSV8(MarkerHue, MarkerSaturation, MarkerValue);
			MarkerColor.A = 0.5f;
			const FVector LumenSceneCameraOrigin = GetLumenSceneViewOrigin(Views[0], ClipmapIndex);
			DrawWireSphere(&ViewPDI, LumenSceneCameraOrigin, MarkerColor, 10 * (1 << ClipmapIndex), 32, SDPG_World);
		}
	}
}