// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "RHI.h"
#include "HitProxies.h"
#include "Shader.h"
#include "SceneUtils.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "MaterialShaderType.h"
#include "MeshMaterialShader.h"
#include "ShaderBaseClasses.h"
#include "DepthRendering.h"
#include "DecalRenderingCommon.h"
#include "DecalRenderingShared.h"
#include "CompositionLighting/PostProcessDeferredDecals.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "UnrealEngine.h"
#include "DebugViewModeRendering.h"
#include "MeshPassProcessor.inl"

class FMeshDecalsVS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FMeshDecalsVS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return (Parameters.MaterialParameters.MaterialDomain == MD_DeferredDecal) &&
			DecalRendering::GetBaseRenderStage(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters)) != EDecalRenderStage::None;
	}

	FMeshDecalsVS() = default;
	FMeshDecalsVS(const ShaderMetaType::CompiledShaderInitializerType & Initializer)
		: FMeshMaterialShader(Initializer)
	{}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(,FMeshDecalsVS,TEXT("/Engine/Private/MeshDecals.usf"),TEXT("MainVS"),SF_Vertex); 

class FMeshDecalsPS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FMeshDecalsPS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return (Parameters.MaterialParameters.MaterialDomain == MD_DeferredDecal) &&
			DecalRendering::GetBaseRenderStage(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters)) != EDecalRenderStage::None;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		DecalRendering::ModifyCompilationEnvironment(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters), EDecalRenderStage::None, OutEnvironment);
	}

	FMeshDecalsPS() = default;
	FMeshDecalsPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(,FMeshDecalsPS,TEXT("/Engine/Private/MeshDecals.usf"),TEXT("MainPS"),SF_Pixel);

class FMeshDecalsEmissivePS : public FMeshDecalsPS
{
public:
	DECLARE_SHADER_TYPE(FMeshDecalsEmissivePS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return (Parameters.MaterialParameters.MaterialDomain == MD_DeferredDecal) &&
			DecalRendering::IsCompatibleWithRenderStage(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters), EDecalRenderStage::Emissive);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		DecalRendering::ModifyCompilationEnvironment(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters), EDecalRenderStage::Emissive, OutEnvironment);
	}

	FMeshDecalsEmissivePS() = default;
	FMeshDecalsEmissivePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshDecalsPS(Initializer)
	{}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FMeshDecalsEmissivePS, TEXT("/Engine/Private/MeshDecals.usf"), TEXT("MainPS"), SF_Pixel);

class FMeshDecalsAmbientOcclusionPS : public FMeshDecalsPS
{
public:
	DECLARE_SHADER_TYPE(FMeshDecalsAmbientOcclusionPS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return (Parameters.MaterialParameters.MaterialDomain == MD_DeferredDecal) &&
			DecalRendering::IsCompatibleWithRenderStage(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters), EDecalRenderStage::AmbientOcclusion);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		DecalRendering::ModifyCompilationEnvironment(DecalRendering::ComputeDecalBlendDesc(Parameters.Platform, Parameters.MaterialParameters), EDecalRenderStage::AmbientOcclusion, OutEnvironment);
	}

	FMeshDecalsAmbientOcclusionPS() = default;
	FMeshDecalsAmbientOcclusionPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshDecalsPS(Initializer)
	{}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FMeshDecalsAmbientOcclusionPS, TEXT("/Engine/Private/MeshDecals.usf"), TEXT("MainPS"), SF_Pixel);

class FMeshDecalMeshProcessor : public FMeshPassProcessor
{
public:
	FMeshDecalMeshProcessor(const FScene* Scene, 
		const FSceneView* InViewIfDynamicMeshCommand, 
		EDecalRenderStage InPassDecalStage, 
		EDecalRenderTargetMode InRenderTargetMode,
		FMeshPassDrawListContext* InDrawListContext);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;

private:
	bool TryAddMeshBatch(
		const FMeshBatch& RESTRICT MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material);

	bool Process(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		int32 StaticMeshId,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		ERasterizerFillMode MeshFillMode,
		ERasterizerCullMode MeshCullMode);

	FMeshPassProcessorRenderState PassDrawRenderState;
	const EDecalRenderStage PassDecalStage;
	const EDecalRenderTargetMode RenderTargetMode;
};

IMPLEMENT_STATIC_UNIFORM_BUFFER_SLOT(DeferredDecals);
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FDeferredDecalUniformParameters, "DeferredDecal", DeferredDecals);

FMeshDecalMeshProcessor::FMeshDecalMeshProcessor(const FScene* Scene, 
	const FSceneView* InViewIfDynamicMeshCommand, 
	EDecalRenderStage InPassDecalStage, 
	EDecalRenderTargetMode InRenderTargetMode,
	FMeshPassDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, PassDecalStage(InPassDecalStage)
	, RenderTargetMode(InRenderTargetMode)
{
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
}

void FMeshDecalMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	if (MeshBatch.bUseForMaterial && MeshBatch.IsDecal(FeatureLevel))
	{
		const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
		while (MaterialRenderProxy)
		{
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (Material)
			{
				if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
				{
					break;
				}
			}

			MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
		}
	}
}

bool FMeshDecalMeshProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	if (Material.IsDeferredDecal())
	{
		// We have no special engine material for decals since we don't want to eat the compilation & memory cost, so just skip if it failed to compile
		if (Material.GetRenderingThreadShaderMap())
		{
			const EShaderPlatform ShaderPlatform = ViewIfDynamicMeshCommand->GetShaderPlatform();
			const FDecalBlendDesc DecalBlendDesc = DecalRendering::ComputeDecalBlendDesc(ShaderPlatform, &Material);

			const bool bShouldRender =
				DecalRendering::IsCompatibleWithRenderStage(DecalBlendDesc, PassDecalStage) &&
				DecalRendering::GetRenderTargetMode(DecalBlendDesc, PassDecalStage) == RenderTargetMode;

			if (bShouldRender)
			{
				const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
				ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);
				ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);

				if (ViewIfDynamicMeshCommand->Family->UseDebugViewPS())
				{
					// Deferred decals can only use translucent blend mode
					if (ViewIfDynamicMeshCommand->Family->EngineShowFlags.ShaderComplexity)
					{
						// If we are in the translucent pass then override the blend mode, otherwise maintain additive blending.
						PassDrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_One>::GetRHI());
					}
					else if (ViewIfDynamicMeshCommand->Family->GetDebugViewShaderMode() != DVSM_OutputMaterialTextureScales)
					{
						// Otherwise, force translucent blend mode (shaders will use an hardcoded alpha).
						PassDrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha>::GetRHI());
					}
				}
				else
				{
					PassDrawRenderState.SetBlendState(DecalRendering::GetDecalBlendState(DecalBlendDesc, PassDecalStage, RenderTargetMode));
				}

				return Process(MeshBatch, BatchElementMask, StaticMeshId, PrimitiveSceneProxy, MaterialRenderProxy, Material, MeshFillMode, MeshCullMode);
			}
		}
	}

	return true;
}

bool FMeshDecalMeshProcessor::Process(
	const FMeshBatch& MeshBatch,
	uint64 BatchElementMask,
	int32 StaticMeshId,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;
	FVertexFactoryType* VertexFactoryType = VertexFactory->GetType();

	FMaterialShaderTypes ShaderTypes;
	ShaderTypes.AddShaderType<FMeshDecalsVS>();

	if (PassDecalStage == EDecalRenderStage::Emissive)
	{
		ShaderTypes.AddShaderType<FMeshDecalsEmissivePS>();
	}
	else if (PassDecalStage == EDecalRenderStage::AmbientOcclusion)
	{
		ShaderTypes.AddShaderType<FMeshDecalsAmbientOcclusionPS>();
	}
	else
	{
		ShaderTypes.AddShaderType<FMeshDecalsPS>();
	}

	FMaterialShaders Shaders;
	if (!MaterialResource.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
	{
		// Skip rendering if any shaders missing
		return false;
	}

	TMeshProcessorShaders<
		FMeshDecalsVS,
		FMeshDecalsPS> MeshDecalPassShaders;
	Shaders.TryGetVertexShader(MeshDecalPassShaders.VertexShader);
	Shaders.TryGetPixelShader(MeshDecalPassShaders.PixelShader);

	FMeshMaterialShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, true);

	const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(MeshDecalPassShaders.VertexShader, MeshDecalPassShaders.PixelShader);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		MaterialResource,
		PassDrawRenderState,
		MeshDecalPassShaders,
		MeshFillMode,
		MeshCullMode,
		SortKey,
		EMeshPassFeatures::Default,
		ShaderElementData);

	return true;
}

void DrawDecalMeshCommands(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FDeferredDecalPassTextures& DecalPassTextures,
	EDecalRenderStage DecalRenderStage,
	EDecalRenderTargetMode RenderTargetMode)
{
	auto* PassParameters = GraphBuilder.AllocParameters<FDeferredDecalPassParameters>();
	GetDeferredDecalPassParameters(GraphBuilder, View, DecalPassTextures, RenderTargetMode, *PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MeshDecals"),
		PassParameters,
		ERDGPassFlags::Raster,
		[&View, DecalRenderStage, RenderTargetMode](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

		const FScene& Scene = *View.Family->Scene->GetRenderScene();

		DrawDynamicMeshPass(View, RHICmdList,
			[&View, DecalRenderStage, RenderTargetMode](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
		{
			FMeshDecalMeshProcessor PassMeshProcessor(
				View.Family->Scene->GetRenderScene(),
				&View,
				DecalRenderStage,
				RenderTargetMode,
				DynamicMeshPassContext);

			for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.MeshDecalBatches.Num(); ++MeshBatchIndex)
			{
				const FMeshBatch* Mesh = View.MeshDecalBatches[MeshBatchIndex].Mesh;
				const FPrimitiveSceneProxy* PrimitiveSceneProxy = View.MeshDecalBatches[MeshBatchIndex].Proxy;
				const uint64 DefaultBatchElementMask = ~0ull;

				PassMeshProcessor.AddMeshBatch(*Mesh, DefaultBatchElementMask, PrimitiveSceneProxy);
			}
		}, true);
	});
}

void RenderMeshDecals(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FDeferredDecalPassTextures& DecalPassTextures,
	EDecalRenderStage DecalRenderStage)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FSceneRenderer_RenderMeshDecals);

	switch (DecalRenderStage)
	{
	case EDecalRenderStage::BeforeBasePass:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::DBuffer);
		break;

	case EDecalRenderStage::BeforeLighting:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::SceneColorAndGBuffer);
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::SceneColorAndGBufferNoNormal);
		break;

	case EDecalRenderStage::Mobile:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::SceneColor);
		break;

	case EDecalRenderStage::MobileBeforeLighting:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::SceneColorAndGBuffer);
		break;

	case EDecalRenderStage::Emissive:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::SceneColor);
		break;

	case EDecalRenderStage::AmbientOcclusion:
		DrawDecalMeshCommands(GraphBuilder, View, DecalPassTextures, DecalRenderStage, EDecalRenderTargetMode::AmbientOcclusion);
		break;
	}
}

void RenderMeshDecalsMobile(FRHICommandListImmediate& RHICmdList, const FViewInfo& View, EDecalRenderStage DecalRenderStage, EDecalRenderTargetMode RenderTargetMode)
{
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	DrawDynamicMeshPass(View, RHICmdList, [&View, DecalRenderStage, RenderTargetMode](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
	{
		FMeshDecalMeshProcessor PassMeshProcessor(
			View.Family->Scene->GetRenderScene(),
			&View,
			DecalRenderStage,
			RenderTargetMode,
			DynamicMeshPassContext);

		for (int32 MeshBatchIndex = 0; MeshBatchIndex < View.MeshDecalBatches.Num(); ++MeshBatchIndex)
		{
			const FMeshBatch* Mesh = View.MeshDecalBatches[MeshBatchIndex].Mesh;
			const FPrimitiveSceneProxy* PrimitiveSceneProxy = View.MeshDecalBatches[MeshBatchIndex].Proxy;
			const uint64 DefaultBatchElementMask = ~0ull;

			PassMeshProcessor.AddMeshBatch(*Mesh, DefaultBatchElementMask, PrimitiveSceneProxy);
		}
	}, true);
}