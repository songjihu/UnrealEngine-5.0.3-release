// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SceneRendering.cpp: Scene rendering.
=============================================================================*/

#include "SceneRendering.h"
#include "ProfilingDebugging/ProfilingHelpers.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "EngineGlobals.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "DeferredShadingRenderer.h"
#include "DynamicPrimitiveDrawing.h"
#include "RenderTargetTemp.h"
#include "RendererModule.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PostProcess/PostProcessEyeAdaptation.h"
#include "PostProcess/PostProcessSubsurface.h"
#include "PostProcess/TemporalAA.h"
#include "PostProcess/PostProcessUpscale.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessTonemap.h"
#include "CompositionLighting/CompositionLighting.h"
#include "LegacyScreenPercentageDriver.h"
#include "SceneViewExtension.h"
#include "Matinee/MatineeActor.h"
#include "ComponentRecreateRenderStateContext.h"
#include "PostProcess/PostProcessSubsurface.h"
#include "PhysicsField/PhysicsFieldComponent.h"
#include "HdrCustomResolveShaders.h"
#include "WideCustomResolveShaders.h"
#include "PipelineStateCache.h"
#include "GPUSkinCache.h"
#include "PrecomputedVolumetricLightmap.h"
#include "RenderUtils.h"
#include "SceneUtils.h"
#include "ResolveShader.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "PostProcess/PostProcessing.h"
#include "VirtualTexturing.h"
#include "VisualizeTexturePresent.h"
#include "GPUScene.h"
#include "TranslucentRendering.h"
#include "VisualizeTexture.h"
#include "VisualizeTexturePresent.h"
#include "MeshDrawCommands.h"
#include "VT/VirtualTextureSystem.h"
#include "VT/VirtualTextureFeedback.h"
#include "HAL/LowLevelMemTracker.h"
#include "IXRTrackingSystem.h"
#include "IXRCamera.h"
#include "IHeadMountedDisplay.h"
#include "DiaphragmDOF.h" 
#include "SingleLayerWaterRendering.h"
#include "HairStrands/HairStrandsVisibility.h"
#include "SystemTextures.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"
#include "Misc/AutomationTest.h"
#include "Engine/TextureCube.h"
#if WITH_EDITOR
#include "Rendering/StaticLightingSystemInterface.h"
#endif
#include "RayTracing/RayTracingScene.h"
#include "FXSystem.h"
#include "Lumen/Lumen.h"
#include "Nanite/Nanite.h"
#include "DistanceFieldLightingShared.h"
#include "RendererOnScreenNotification.h"
#include "Rendering/NaniteCoarseMeshStreamingManager.h"
#include "Rendering/NaniteStreamingManager.h"

/*-----------------------------------------------------------------------------
	Globals
-----------------------------------------------------------------------------*/

static TGlobalResource<FVirtualTextureFeedbackBuffer> GVirtualTextureFeedbackBuffer;

static TAutoConsoleVariable<int32> CVarCachedMeshDrawCommands(
	TEXT("r.MeshDrawCommands.UseCachedCommands"),
	1,
	TEXT("Whether to render from cached mesh draw commands (on vertex factories that support it), or to generate draw commands every frame."),
	ECVF_RenderThreadSafe);

bool UseCachedMeshDrawCommands()
{
	return CVarCachedMeshDrawCommands.GetValueOnRenderThread() > 0;
}

bool UseCachedMeshDrawCommands_AnyThread()
{
	return CVarCachedMeshDrawCommands.GetValueOnAnyThread() > 0;
}

static TAutoConsoleVariable<int32> CVarMeshDrawCommandsDynamicInstancing(
	TEXT("r.MeshDrawCommands.DynamicInstancing"),
	1,
	TEXT("Whether to dynamically combine multiple compatible visible Mesh Draw Commands into one instanced draw on vertex factories that support it."),
	ECVF_RenderThreadSafe);

bool IsDynamicInstancingEnabled(ERHIFeatureLevel::Type FeatureLevel)
{
	return CVarMeshDrawCommandsDynamicInstancing.GetValueOnRenderThread() > 0
		&& UseGPUScene(GMaxRHIShaderPlatform, FeatureLevel);
}

int32 GDumpInstancingStats = 0;
FAutoConsoleVariableRef CVarDumpInstancingStats(
	TEXT("r.MeshDrawCommands.LogDynamicInstancingStats"),
	GDumpInstancingStats,
	TEXT("Whether to log dynamic instancing stats on the next frame"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GDumpMeshDrawCommandMemoryStats = 0;
FAutoConsoleVariableRef CVarDumpMeshDrawCommandMemoryStats(
	TEXT("r.MeshDrawCommands.LogMeshDrawCommandMemoryStats"),
	GDumpMeshDrawCommandMemoryStats,
	TEXT("Whether to log mesh draw command memory stats on the next frame"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

/**
 * Console variable controlling whether or not occlusion queries are allowed.
 */
static TAutoConsoleVariable<int32> CVarAllowOcclusionQueries(
	TEXT("r.AllowOcclusionQueries"),
	1,
	TEXT("If zero, occlusion queries will not be used to cull primitives."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<float> CVarDemosaicVposOffset(
	TEXT("r.DemosaicVposOffset"),
	0.0f,
	TEXT("This offset is added to the rasterized position used for demosaic in the mobile tonemapping shader. It exists to workaround driver bugs on some Android devices that have a half-pixel offset."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarDecalDepthBias(
	TEXT("r.DecalDepthBias"),
	0.005f,
	TEXT("Global depth bias used by mesh decals. Default is 0.005"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRefractionQuality(
	TEXT("r.RefractionQuality"),
	2,
	TEXT("Defines the distorion/refraction quality which allows to adjust for quality or performance.\n")
	TEXT("<=0: off (fastest)\n")
	TEXT("  1: low quality (not yet implemented)\n")
	TEXT("  2: normal quality (default)\n")
	TEXT("  3: high quality (e.g. color fringe, not yet implemented)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarInstancedStereo(
	TEXT("vr.InstancedStereo"),
	0,
	TEXT("0 to disable instanced stereo (default), 1 to enable."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMobileMultiView(
	TEXT("vr.MobileMultiView"),
	0,
	TEXT("0 to disable mobile multi-view, 1 to enable.\n"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRoundRobinOcclusion(
	TEXT("vr.RoundRobinOcclusion"),
	0,
	TEXT("0 to disable round-robin occlusion queries for stereo rendering (default), 1 to enable."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarODSCapture(
	TEXT("vr.ODSCapture"),
	0,
	TEXT("Experimental")
	TEXT("0 to disable Omni-directional stereo capture (default), 1 to enable."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarViewRectUseScreenBottom(
	TEXT("r.ViewRectUseScreenBottom"),
	0,
	TEXT("WARNING: This is an experimental, unsupported feature and does not work with all postprocesses (e.g DOF and DFAO)\n")
	TEXT("If enabled, the view rectangle will use the bottom left corner instead of top left"),
	ECVF_RenderThreadSafe
);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
static TAutoConsoleVariable<float> CVarGeneralPurposeTweak(
	TEXT("r.GeneralPurposeTweak"),
	1.0f,
	TEXT("Useful for low level shader development to get quick iteration time without having to change any c++ code.\n")
	TEXT("Value maps to Frame.GeneralPurposeTweak inside the shaders.\n")
	TEXT("Example usage: Multiplier on some value to tweak, toggle to switch between different algorithms (Default: 1.0)\n")
	TEXT("DON'T USE THIS FOR ANYTHING THAT IS CHECKED IN. Compiled out in SHIPPING to make cheating a bit harder."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGeneralPurposeTweak2(
	TEXT("r.GeneralPurposeTweak2"),
	1.0f,
	TEXT("Useful for low level shader development to get quick iteration time without having to change any c++ code.\n")
	TEXT("Value maps to Frame.GeneralPurposeTweak2 inside the shaders.\n")
	TEXT("Example usage: Multiplier on some value to tweak, toggle to switch between different algorithms (Default: 1.0)\n")
	TEXT("DON'T USE THIS FOR ANYTHING THAT IS CHECKED IN. Compiled out in SHIPPING to make cheating a bit harder."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDisplayInternals(
	TEXT("r.DisplayInternals"),
	0,
	TEXT("Allows to enable screen printouts that show the internals on the engine/renderer\n")
	TEXT("This is mostly useful to be able to reason why a screenshots looks different.\n")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: enabled"),
	ECVF_RenderThreadSafe | ECVF_Cheat);
#endif

/**
 * Console variable controlling the maximum number of shadow cascades to render with.
 *   DO NOT READ ON THE RENDERING THREAD. Use FSceneView::MaxShadowCascades.
 */
static TAutoConsoleVariable<int32> CVarMaxShadowCascades(
	TEXT("r.Shadow.CSM.MaxCascades"),
	10,
	TEXT("The maximum number of cascades with which to render dynamic directional light shadows."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarMaxMobileShadowCascades(
	TEXT("r.Shadow.CSM.MaxMobileCascades"),
	2,
	TEXT("The maximum number of cascades with which to render dynamic directional light shadows when using the mobile renderer."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarSupportSimpleForwardShading(
	TEXT("r.SupportSimpleForwardShading"),
	0,
	TEXT("Whether to compile the shaders to support r.SimpleForwardShading being enabled (PC only)."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarSimpleForwardShading(
	TEXT("r.SimpleForwardShading"),
	0,
	TEXT("Whether to use the simple forward shading base pass shaders which only support lightmaps + stationary directional light + stationary skylight\n")
	TEXT("All other lighting features are disabled when true.  This is useful for supporting very low end hardware, and is only supported on PC platforms.\n")
	TEXT("0:off, 1:on"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

// Keep track of the previous value for CVarSimpleForwardShading so we can avoid costly updates when it hasn't actually changed
static int32 CVarSimpleForwardShading_PreviousValue = 0;

static TAutoConsoleVariable<float> CVarNormalCurvatureToRoughnessBias(
	TEXT("r.NormalCurvatureToRoughnessBias"),
	0.0f,
	TEXT("Biases the roughness resulting from screen space normal changes for materials with NormalCurvatureToRoughness enabled.  Valid range [-1, 1]"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarNormalCurvatureToRoughnessExponent(
	TEXT("r.NormalCurvatureToRoughnessExponent"),
	0.333f,
	TEXT("Exponent on the roughness resulting from screen space normal changes for materials with NormalCurvatureToRoughness enabled."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarNormalCurvatureToRoughnessScale(
	TEXT("r.NormalCurvatureToRoughnessScale"),
	1.0f,
	TEXT("Scales the roughness resulting from screen space normal changes for materials with NormalCurvatureToRoughness enabled.  Valid range [0, 2]"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarEnableMultiGPUForkAndJoin(
	TEXT("r.EnableMultiGPUForkAndJoin"),
	1,
	TEXT("Whether to allow unused GPUs to speedup rendering by sharing work.\n"),
	ECVF_Default
	);

/*-----------------------------------------------------------------------------
	FParallelCommandListSet
-----------------------------------------------------------------------------*/


static TAutoConsoleVariable<int32> CVarRHICmdSpewParallelListBalance(
	TEXT("r.RHICmdSpewParallelListBalance"),
	0,
	TEXT("For debugging, spews the size of the parallel command lists. This stalls and otherwise wrecks performance.\n")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: enabled (default)"));

static TAutoConsoleVariable<int32> CVarRHICmdBalanceParallelLists(
	TEXT("r.RHICmdBalanceParallelLists"),
	2,
	TEXT("Allows to enable a preprocess of the drawlists to try to balance the load equally among the command lists.\n")
	TEXT(" 0: off \n")
	TEXT(" 1: enabled")
	TEXT(" 2: experiemental, uses previous frame results (does not do anything in split screen etc)"));

static TAutoConsoleVariable<int32> CVarRHICmdMinCmdlistForParallelSubmit(
	TEXT("r.RHICmdMinCmdlistForParallelSubmit"),
	1,
	TEXT("Minimum number of parallel translate command lists to submit. If there are fewer than this number, they just run on the RHI thread and immediate context."));

static TAutoConsoleVariable<int32> CVarRHICmdMinDrawsPerParallelCmdList(
	TEXT("r.RHICmdMinDrawsPerParallelCmdList"),
	64,
	TEXT("The minimum number of draws per cmdlist. If the total number of draws is less than this, then no parallel work will be done at all. This can't always be honored or done correctly. More effective with RHICmdBalanceParallelLists."));

static TAutoConsoleVariable<int32> CVarWideCustomResolve(
	TEXT("r.WideCustomResolve"),
	0,
	TEXT("Use a wide custom resolve filter when MSAA is enabled")
	TEXT("0: Disabled [hardware box filter]")
	TEXT("1: Wide (r=1.25, 12 samples)")
	TEXT("2: Wider (r=1.4, 16 samples)")
	TEXT("3: Widest (r=1.5, 20 samples)"),
	ECVF_RenderThreadSafe | ECVF_Scalability
	);

static TAutoConsoleVariable<int32> CVarBasePassForceOutputsVelocity(
	TEXT("r.BasePassForceOutputsVelocity"), 0,
	TEXT("Force the base pass to compute motion vector, regardless of FPrimitiveUniformShaderParameters.")
	TEXT("0: Disabled (default)")
	TEXT("1: Enabled"),
	ECVF_RenderThreadSafe);

static int32 GParallelCmdListInheritBreadcrumbs = 1;
static FAutoConsoleVariableRef CVarParallelCmdListInheritBreadcrumbs(
	TEXT("r.ParallelCmdListInheritBreadcrumbs"),
	GParallelCmdListInheritBreadcrumbs,
	TEXT("Whether to inherit breadcrumbs to parallel cmd lists"),
	ECVF_ReadOnly
);

static TAutoConsoleVariable<int32> CVarFilmGrain(
	TEXT("r.FilmGrain"), 1,
	TEXT("Whether to enable film grain."),
	ECVF_RenderThreadSafe);

#if !UE_BUILD_SHIPPING

static TAutoConsoleVariable<int32> CVarTestInternalViewRectOffset(
	TEXT("r.Test.ViewRectOffset"),
	0,
	TEXT("Moves the view rect within the renderer's internal render target.\n")
	TEXT(" 0: disabled (default);"));

static TAutoConsoleVariable<int32> CVarTestCameraCut(
	TEXT("r.Test.CameraCut"),
	0,
	TEXT("Force enabling camera cut for testing purposes.\n")
	TEXT(" 0: disabled (default); 1: enabled."));

static TAutoConsoleVariable<int32> CVarTestScreenPercentageInterface(
	TEXT("r.Test.DynamicResolutionHell"),
	0,
	TEXT("Override the screen percentage interface for all view family with dynamic resolution hell.\n")
	TEXT(" 0: off (default);\n")
	TEXT(" 1: Dynamic resolution hell."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarTestPrimaryScreenPercentageMethodOverride(
	TEXT("r.Test.PrimaryScreenPercentageMethodOverride"),
	0,
	TEXT("Override the screen percentage method for all view family.\n")
	TEXT(" 0: view family's screen percentage interface choose; (default)\n")
	TEXT(" 1: old fashion upscaling pass at the very end right before before UI;\n")
	TEXT(" 2: TemporalAA upsample."));

static TAutoConsoleVariable<int32> CVarTestSecondaryUpscaleOverride(
	TEXT("r.Test.SecondaryUpscaleOverride"),
	0,
	TEXT("Override the secondary upscale.\n")
	TEXT(" 0: disabled; (default)\n")
	TEXT(" 1: use secondary view fraction = 0.5 with nearest secondary upscale."));

#endif

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

static TAutoConsoleVariable<int32> CVarNaniteShowUnsupportedError(
	TEXT("r.Nanite.ShowUnsupportedError"),
	1,
	TEXT("Specify behavior of Nanite unsupported screen error message.\n")
	TEXT(" 0: disabled\n")
	TEXT(" 1: show error if Nanite is present in the scene but unsupported, and fallback meshes are not used for rendering; (default)")
	TEXT(" 2: show error if Nanite is present in the scene but unsupported, even if fallback meshes are used for rendering")
);

#endif

static FParallelCommandListSet* GOutstandingParallelCommandListSet = nullptr;
FOcclusionSubmittedFenceState FSceneRenderer::OcclusionSubmittedFence[FOcclusionQueryHelpers::MaxBufferedOcclusionFrames];

extern int32 GetTranslucencyLightingVolumeDim();

DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer ViewExtensionPostRenderView"), STAT_FDeferredShadingSceneRenderer_ViewExtensionPostRenderView, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer ViewExtensionPreRenderView"), STAT_FDeferredShadingSceneRenderer_ViewExtensionPreRenderView, STATGROUP_SceneRendering);

#define FASTVRAM_CVAR(Name,DefaultValue) static TAutoConsoleVariable<int32> CVarFastVRam_##Name(TEXT("r.FastVRam."#Name), DefaultValue, TEXT(""))

FASTVRAM_CVAR(GBufferA, 0);
FASTVRAM_CVAR(GBufferB, 1);
FASTVRAM_CVAR(GBufferC, 0);
FASTVRAM_CVAR(GBufferD, 0);
FASTVRAM_CVAR(GBufferE, 0);
FASTVRAM_CVAR(GBufferF, 0);
FASTVRAM_CVAR(GBufferVelocity, 0);
FASTVRAM_CVAR(HZB, 1);
FASTVRAM_CVAR(SceneDepth, 1);
FASTVRAM_CVAR(SceneColor, 1);
FASTVRAM_CVAR(BokehDOF, 1);
FASTVRAM_CVAR(CircleDOF, 1);
FASTVRAM_CVAR(CombineLUTs, 1);
FASTVRAM_CVAR(Downsample, 1);
FASTVRAM_CVAR(EyeAdaptation, 1);
FASTVRAM_CVAR(Histogram, 1);
FASTVRAM_CVAR(HistogramReduce, 1);
FASTVRAM_CVAR(VelocityFlat, 1);
FASTVRAM_CVAR(VelocityMax, 1);
FASTVRAM_CVAR(MotionBlur, 1);
FASTVRAM_CVAR(Tonemap, 1);
FASTVRAM_CVAR(Upscale, 1);
FASTVRAM_CVAR(DistanceFieldNormal, 1);
FASTVRAM_CVAR(DistanceFieldAOHistory, 1);
FASTVRAM_CVAR(DistanceFieldAODownsampledBentNormal, 1); 
FASTVRAM_CVAR(DistanceFieldAOBentNormal, 0); 
FASTVRAM_CVAR(DistanceFieldIrradiance, 0); 
FASTVRAM_CVAR(DistanceFieldShadows, 1);
FASTVRAM_CVAR(Distortion, 1);
FASTVRAM_CVAR(ScreenSpaceShadowMask, 1);
FASTVRAM_CVAR(VolumetricFog, 1);
FASTVRAM_CVAR(SeparateTranslucency, 0); 
FASTVRAM_CVAR(SeparateTranslucencyModulate, 0);
FASTVRAM_CVAR(ScreenSpaceAO,0);
FASTVRAM_CVAR(SSR, 0);
FASTVRAM_CVAR(DBufferA, 0);
FASTVRAM_CVAR(DBufferB, 0);
FASTVRAM_CVAR(DBufferC, 0); 
FASTVRAM_CVAR(DBufferMask, 0);
FASTVRAM_CVAR(DOFSetup, 1);
FASTVRAM_CVAR(DOFReduce, 1);
FASTVRAM_CVAR(DOFPostfilter, 1);
FASTVRAM_CVAR(PostProcessMaterial, 1);

FASTVRAM_CVAR(CustomDepth, 0);
FASTVRAM_CVAR(ShadowPointLight, 0);
FASTVRAM_CVAR(ShadowPerObject, 0);
FASTVRAM_CVAR(ShadowCSM, 0);

FASTVRAM_CVAR(DistanceFieldCulledObjectBuffers, 1);
FASTVRAM_CVAR(DistanceFieldTileIntersectionResources, 1);
FASTVRAM_CVAR(DistanceFieldAOScreenGridResources, 1);
FASTVRAM_CVAR(ForwardLightingCullingResources, 1);
FASTVRAM_CVAR(GlobalDistanceFieldCullGridBuffers, 1);

bool IsStaticLightingAllowed()
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowStaticLighting"));
	return CVar->GetValueOnRenderThread() != 0;
}

TSharedPtr<FVirtualShadowMapClipmap> FVisibleLightInfo::FindShadowClipmapForView(const FViewInfo* View) const
{
	for (const auto& Clipmap : VirtualShadowMapClipmaps)
	{
		if (Clipmap->GetDependentView() == View)
		{
			return Clipmap;
		}
	}
	
	// This has to mirror the if (IStereoRendering::IsAPrimaryView(View)) test in ShadowSetup.cpp, which ensures only one view dependent shadow is set up for a stereo pair.
	// TODO: this should very much be explicitly linked.
	if (!IStereoRendering::IsAPrimaryView(*View) && VirtualShadowMapClipmaps.Num() > 0)
	{
		return VirtualShadowMapClipmaps[0];
	}

	return TSharedPtr<FVirtualShadowMapClipmap>();
}

int32 FVisibleLightInfo::GetVirtualShadowMapId( const FViewInfo* View ) const
{
	if( VirtualShadowMapClipmaps.Num() )
	{
		return FindShadowClipmapForView( View )->GetVirtualShadowMap(0)->ID;
	}
	else
	{
		return VirtualShadowMapId;
	}
}


#if !UE_BUILD_SHIPPING
namespace
{

/*
 * Screen percentage interface that is just constantly changing res to test resolution changes.
 */
class FScreenPercentageHellDriver : public ISceneViewFamilyScreenPercentage
{
public:

	FScreenPercentageHellDriver(const FSceneViewFamily& InViewFamily)
		: ViewFamily(InViewFamily)
	{ 
		if (InViewFamily.GetTemporalUpscalerInterface())
		{
			MinResolutionFraction = InViewFamily.GetTemporalUpscalerInterface()->GetMinUpsampleResolutionFraction();
			MaxResolutionFraction = InViewFamily.GetTemporalUpscalerInterface()->GetMaxUpsampleResolutionFraction();
		}

		check(MinResolutionFraction <= MaxResolutionFraction);
		check(MinResolutionFraction > 0.0f);
		check(MaxResolutionFraction > 0.0f);
	}

	virtual float GetPrimaryResolutionFractionUpperBound() const override
	{
		return MaxResolutionFraction;
	}

	virtual ISceneViewFamilyScreenPercentage* Fork_GameThread(const class FSceneViewFamily& ForkedViewFamily) const override
	{
		check(IsInGameThread());

		if (ForkedViewFamily.Views[0]->State)
		{
			return new FScreenPercentageHellDriver(ForkedViewFamily);
		}

		return new FLegacyScreenPercentageDriver(
			ForkedViewFamily, /* GlobalResolutionFraction = */ MaxResolutionFraction);
	}

	virtual float GetPrimaryResolutionFraction_RenderThread() const override
	{
		check(IsInRenderingThread());

		// Early return if no screen percentage should be done.
		if (!ViewFamily.EngineShowFlags.ScreenPercentage)
		{
			return 1.0f;
		}

		uint32 FrameId = 0;

		const FSceneViewState* ViewState = static_cast<const FSceneViewState*>(ViewFamily.Views[0]->State);
		if (ViewState)
		{
			FrameId = ViewState->GetFrameIndex(8);
		}
		return FrameId == 0 ? MaxResolutionFraction : FMath::Lerp(MinResolutionFraction, MaxResolutionFraction, 0.5f + 0.5f * FMath::Cos((FrameId + 0.25) * PI / 8));
	}

private:
	// View family to take care of.
	const FSceneViewFamily& ViewFamily;
	float MinResolutionFraction = 0.5f;
	float MaxResolutionFraction = 1.0f;

};

} // namespace
#endif // !UE_BUILD_SHIPPING

void FRDGParallelCommandListSet::SetStateOnCommandList(FRHICommandList& RHICmdList)
{
	FParallelCommandListSet::SetStateOnCommandList(RHICmdList);
	Bindings.SetOnCommandList(RHICmdList);
	SceneRenderer.SetStereoViewport(RHICmdList, View, ViewportScale);
}

FFastVramConfig::FFastVramConfig()
{
	FMemory::Memset(*this, 0);
}

void FFastVramConfig::Update()
{
	bDirty = false;
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferA, GBufferA);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferB, GBufferB);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferC, GBufferC);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferD, GBufferD);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferE, GBufferE);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferF, GBufferF);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_GBufferVelocity, GBufferVelocity);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_HZB, HZB);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_SceneDepth, SceneDepth);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_SceneColor, SceneColor);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_BokehDOF, BokehDOF);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_CircleDOF, CircleDOF);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_CombineLUTs, CombineLUTs);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_Downsample, Downsample);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_EyeAdaptation, EyeAdaptation);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_Histogram, Histogram);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_HistogramReduce, HistogramReduce);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_VelocityFlat, VelocityFlat);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_VelocityMax, VelocityMax);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_MotionBlur, MotionBlur);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_Tonemap, Tonemap);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_Upscale, Upscale);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldNormal, DistanceFieldNormal);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldAOHistory, DistanceFieldAOHistory);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldAODownsampledBentNormal, DistanceFieldAODownsampledBentNormal);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldAOBentNormal, DistanceFieldAOBentNormal);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldIrradiance, DistanceFieldIrradiance);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DistanceFieldShadows, DistanceFieldShadows);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_Distortion, Distortion);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_ScreenSpaceShadowMask, ScreenSpaceShadowMask);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_VolumetricFog, VolumetricFog);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_SeparateTranslucency, SeparateTranslucency);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_SeparateTranslucencyModulate, SeparateTranslucencyModulate);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_ScreenSpaceAO, ScreenSpaceAO);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_SSR, SSR);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DBufferA, DBufferA);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DBufferB, DBufferB);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DBufferC, DBufferC);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DBufferMask, DBufferMask);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DOFSetup, DOFSetup);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DOFReduce, DOFReduce);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_DOFPostfilter, DOFPostfilter);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_CustomDepth, CustomDepth);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_ShadowPointLight, ShadowPointLight);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_ShadowPerObject, ShadowPerObject);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_ShadowCSM, ShadowCSM);
	bDirty |= UpdateTextureFlagFromCVar(CVarFastVRam_PostProcessMaterial, PostProcessMaterial);

	bDirty |= UpdateBufferFlagFromCVar(CVarFastVRam_DistanceFieldCulledObjectBuffers, DistanceFieldCulledObjectBuffers);
	bDirty |= UpdateBufferFlagFromCVar(CVarFastVRam_DistanceFieldTileIntersectionResources, DistanceFieldTileIntersectionResources);
	bDirty |= UpdateBufferFlagFromCVar(CVarFastVRam_DistanceFieldAOScreenGridResources, DistanceFieldAOScreenGridResources);
	bDirty |= UpdateBufferFlagFromCVar(CVarFastVRam_ForwardLightingCullingResources, ForwardLightingCullingResources);
	bDirty |= UpdateBufferFlagFromCVar(CVarFastVRam_GlobalDistanceFieldCullGridBuffers, GlobalDistanceFieldCullGridBuffers);
}

bool FFastVramConfig::UpdateTextureFlagFromCVar(TAutoConsoleVariable<int32>& CVar, ETextureCreateFlags& InOutValue)
{
	ETextureCreateFlags OldValue = InOutValue;
	int32 CVarValue = CVar.GetValueOnRenderThread();
	InOutValue = TexCreate_None;
	if (CVarValue == 1)
	{
		InOutValue = TexCreate_FastVRAM;
	}
	else if (CVarValue == 2)
	{
		InOutValue = TexCreate_FastVRAM | TexCreate_FastVRAMPartialAlloc;
	}
	return OldValue != InOutValue;
}

bool FFastVramConfig::UpdateBufferFlagFromCVar(TAutoConsoleVariable<int32>& CVar, EBufferUsageFlags& InOutValue)
{
	EBufferUsageFlags OldValue = InOutValue;
	InOutValue = CVar.GetValueOnRenderThread() ? ( BUF_FastVRAM ) : BUF_None;
	return OldValue != InOutValue;
}

FFastVramConfig GFastVRamConfig;


FParallelCommandListSet::FParallelCommandListSet(TStatId InExecuteStat, const FViewInfo& InView, FRHICommandListImmediate& InParentCmdList)
	: View(InView)
	, ParentCmdList(InParentCmdList)
	, ExecuteStat(InExecuteStat)
	, NumAlloc(0)
{
	Width = CVarRHICmdWidth.GetValueOnRenderThread();
	MinDrawsPerCommandList = CVarRHICmdMinDrawsPerParallelCmdList.GetValueOnRenderThread();
	bSpewBalance = !!CVarRHICmdSpewParallelListBalance.GetValueOnRenderThread();
	int32 IntBalance = CVarRHICmdBalanceParallelLists.GetValueOnRenderThread();
	bBalanceCommands = !!IntBalance;
	CommandLists.Reserve(Width * 8);
	Events.Reserve(Width * 8);
	NumDrawsIfKnown.Reserve(Width * 8);
	check(!GOutstandingParallelCommandListSet);
	GOutstandingParallelCommandListSet = this;
}

FRHICommandList* FParallelCommandListSet::AllocCommandList()
{
	NumAlloc++;
	return new FRHICommandList(ParentCmdList.GetGPUMask());
}

void FParallelCommandListSet::Dispatch(bool bHighPriority)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FParallelCommandListSet_Dispatch);
	check(IsInRenderingThread() && FMemStack::Get().GetNumMarks() == 1); // we do not want this popped before the end of the scene and it better be the scene allocator
	check(CommandLists.Num() == Events.Num());
	check(CommandLists.Num() == NumAlloc);

	// We should not be submitting work off a parent command list if it's still in the middle of a renderpass.
	// This is a bit weird since we will (likely) end up opening one in the parallel translate case but until we have
	// a cleaner way for the RHI to specify parallel passes this is what we've got.
	check(ParentCmdList.IsOutsideRenderPass());

	ENamedThreads::Type RenderThread_Local = ENamedThreads::GetRenderThread_Local();
	if (bSpewBalance)
	{
		// finish them all
		for (auto& Event : Events)
		{
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Event, RenderThread_Local);
		}
		// spew sizes
		int32 Index = 0;
		for (auto CmdList : CommandLists)
		{
			UE_LOG(LogTemp, Display, TEXT("CmdList %2d/%2d  : %8dKB"), Index, CommandLists.Num(), (CmdList->GetUsedMemory() + 1023) / 1024);
			Index++;
		}
	}
	bool bActuallyDoParallelTranslate = GRHISupportsParallelRHIExecute && CommandLists.Num() >= CVarRHICmdMinCmdlistForParallelSubmit.GetValueOnRenderThread();
	if (bActuallyDoParallelTranslate)
	{
		int32 Total = 0;
		bool bIndeterminate = false;
		for (int32 Count : NumDrawsIfKnown)
		{
			if (Count < 0)
			{
				bIndeterminate = true;
				break; // can't determine how many are in this one; assume we should run parallel translate
			}
			Total += Count;
		}
		if (!bIndeterminate && Total < MinDrawsPerCommandList)
		{
			UE_CLOG(bSpewBalance, LogTemp, Display, TEXT("Disabling parallel translate because the number of draws is known to be small."));
			bActuallyDoParallelTranslate = false;
		}
	}

	if (bActuallyDoParallelTranslate)
	{
		UE_CLOG(bSpewBalance, LogTemp, Display, TEXT("%d cmdlists for parallel translate"), CommandLists.Num());
		check(GRHISupportsParallelRHIExecute);
		NumAlloc -= CommandLists.Num();
		ParentCmdList.QueueParallelAsyncCommandListSubmit(&Events[0], bHighPriority, &CommandLists[0], &NumDrawsIfKnown[0], CommandLists.Num(), (MinDrawsPerCommandList * 4) / 3, bSpewBalance);
		// #todo-renderpasses PS4 breaks if this isn't here. Why?
		SetStateOnCommandList(ParentCmdList);
		ParentCmdList.EndRenderPass();
	}
	else
	{
		UE_CLOG(bSpewBalance, LogTemp, Display, TEXT("%d cmdlists (no parallel translate desired)"), CommandLists.Num());
		for (int32 Index = 0; Index < CommandLists.Num(); Index++)
		{
			ParentCmdList.QueueAsyncCommandListSubmit(Events[Index], CommandLists[Index]);
			NumAlloc--;
		}
	}
	CommandLists.Reset();
	Events.Reset();
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FParallelCommandListSet_Dispatch_ServiceLocalQueue);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(RenderThread_Local);
}

FParallelCommandListSet::~FParallelCommandListSet()
{
	check(GOutstandingParallelCommandListSet == this);
	GOutstandingParallelCommandListSet = nullptr;

	check(IsInRenderingThread() && FMemStack::Get().GetNumMarks() == 1); // we do not want this popped before the end of the scene and it better be the scene allocator
	checkf(CommandLists.Num() == 0, TEXT("Derived class of FParallelCommandListSet did not call Dispatch in virtual destructor"));
	checkf(NumAlloc == 0, TEXT("Derived class of FParallelCommandListSet did not call Dispatch in virtual destructor"));
}

FRHICommandList* FParallelCommandListSet::NewParallelCommandList()
{
	FRHICommandList* Result = AllocCommandList();
	Result->ExecuteStat = ExecuteStat;

#if RHI_WANT_BREADCRUMB_EVENTS
	if (GParallelCmdListInheritBreadcrumbs)
	{
		Result->InheritBreadcrumbs(ParentCmdList);
	}
#endif

	SetStateOnCommandList(*Result);
	return Result;
}

void FParallelCommandListSet::AddParallelCommandList(FRHICommandList* CmdList, FGraphEventRef& CompletionEvent, int32 InNumDrawsIfKnown)
{
	check(IsInRenderingThread() && FMemStack::Get().GetNumMarks() == 1); // we do not want this popped before the end of the scene and it better be the scene allocator
	check(CommandLists.Num() == Events.Num());
	CommandLists.Add(CmdList);
	Events.Add(CompletionEvent);
	NumDrawsIfKnown.Add(InNumDrawsIfKnown);
}

void FParallelCommandListSet::WaitForTasks()
{
	if (GOutstandingParallelCommandListSet)
	{
		GOutstandingParallelCommandListSet->WaitForTasksInternal();
	}
}

void FParallelCommandListSet::WaitForTasksInternal()
{
	check(IsInRenderingThread());
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FParallelCommandListSet_WaitForTasks);
	FGraphEventArray WaitOutstandingTasks;
	for (int32 Index = 0; Index < Events.Num(); Index++)
	{
		if (!Events[Index]->IsComplete())
		{
			WaitOutstandingTasks.Add(Events[Index]);
		}
	}
	if (WaitOutstandingTasks.Num())
	{
		ENamedThreads::Type RenderThread_Local = ENamedThreads::GetRenderThread_Local();
		check(!FTaskGraphInterface::Get().IsThreadProcessingTasks(RenderThread_Local));
		FTaskGraphInterface::Get().WaitUntilTasksComplete(WaitOutstandingTasks, RenderThread_Local);
	}
}

bool IsHMDHiddenAreaMaskActive()
{
	// Query if we have a custom HMD post process mesh to use
	static const auto* const HiddenAreaMaskCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.HiddenAreaMask"));

	return
		HiddenAreaMaskCVar != nullptr &&
		// Any thread is used due to FViewInfo initialization.
		HiddenAreaMaskCVar->GetValueOnAnyThread() == 1 &&
		GEngine &&
		GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice() &&
		GEngine->XRSystem->GetHMDDevice()->HasVisibleAreaMesh();
}

/*-----------------------------------------------------------------------------
	FViewInfo
-----------------------------------------------------------------------------*/

/** 
 * Initialization constructor. Passes all parameters to FSceneView constructor
 */
FViewInfo::FViewInfo(const FSceneViewInitOptions& InitOptions)
	:	FSceneView(InitOptions)
	,	IndividualOcclusionQueries((FSceneViewState*)InitOptions.SceneViewStateInterface, 1)	
	,	GroupedOcclusionQueries((FSceneViewState*)InitOptions.SceneViewStateInterface, FOcclusionQueryBatcher::OccludedPrimitiveQueryBatchSize)
	,	CustomVisibilityQuery(nullptr)
{
	Init();
}

/** 
 * Initialization constructor. 
 * @param InView - copy to init with
 */
FViewInfo::FViewInfo(const FSceneView* InView)
	:	FSceneView(*InView)
	,	IndividualOcclusionQueries((FSceneViewState*)InView->State,1)	
	,	GroupedOcclusionQueries((FSceneViewState*)InView->State,FOcclusionQueryBatcher::OccludedPrimitiveQueryBatchSize)
	,	CustomVisibilityQuery(nullptr)
{
	Init();
}

void FViewInfo::Init()
{
	ViewRect = FIntRect(0, 0, 0, 0);

	CachedViewUniformShaderParameters = nullptr;
	bHasNoVisiblePrimitive = false;
	bHasTranslucentViewMeshElements = 0;
	bPrevTransformsReset = false;
	bIgnoreExistingQueries = false;
	bDisableQuerySubmissions = false;
	bDisableDistanceBasedFadeTransitions = false;	
	ShadingModelMaskInView = 0;
	bSceneHasSkyMaterial = 0;
	bHasSingleLayerWaterMaterial = 0;
	bHasTranslucencySeparateModulation = 0;
	bLumenPropagateGlobalLightingChange = 0;

	NumVisibleStaticMeshElements = 0;
	PrecomputedVisibilityData = 0;
	bSceneHasDecals = 0;

	bIsViewInfo = true;
	
	bStatePrevViewInfoIsReadOnly = true;
	bUsesGlobalDistanceField = false;
	bUsesLightingChannels = false;
	bTranslucentSurfaceLighting = false;
	bUsesSceneDepth = false;
	bFogOnlyOnRenderedOpaque = false;

	ExponentialFogParameters = FVector4f(0,1,1,0);
	ExponentialFogParameters2 = FVector4f(0, 1, 0, 0);
	ExponentialFogColor = FVector3f::ZeroVector;
	FogMaxOpacity = 1;
	ExponentialFogParameters3 = FVector4f(0, 0, 0, 0);
	SinCosInscatteringColorCubemapRotation = FVector2f::ZeroVector;
	FogInscatteringColorCubemap = nullptr;
	FogInscatteringTextureParameters = FVector::ZeroVector;

	SkyAtmosphereCameraAerialPerspectiveVolume = nullptr;
	SkyAtmosphereUniformShaderParameters = nullptr;

	VolumetricCloudSkyAO = nullptr;

	bUseDirectionalInscattering = false;
	DirectionalInscatteringExponent = 0;
	DirectionalInscatteringStartDistance = 0;
	InscatteringLightDirection = FVector(0);
	DirectionalInscatteringColor = FLinearColor(ForceInit);

	for (int32 CascadeIndex = 0; CascadeIndex < TVC_MAX; CascadeIndex++)
	{
		TranslucencyLightingVolumeMin[CascadeIndex] = FVector(0);
		TranslucencyVolumeVoxelSize[CascadeIndex] = 0;
		TranslucencyLightingVolumeSize[CascadeIndex] = FVector(0);
	}

	const int32 MaxMobileShadowCascadeCount = FMath::Clamp(CVarMaxMobileShadowCascades.GetValueOnAnyThread(), 0, MAX_MOBILE_SHADOWCASCADES);
	const int32 MaxShadowCascadeCountUpperBound = GetFeatureLevel() >= ERHIFeatureLevel::SM5 ? 10 : MaxMobileShadowCascadeCount;

	MaxShadowCascades = FMath::Clamp<int32>(CVarMaxShadowCascades.GetValueOnAnyThread(), 0, MaxShadowCascadeCountUpperBound);

	ShaderMap = GetGlobalShaderMap(FeatureLevel);

	ViewState = (FSceneViewState*)State;
	bIsSnapshot = false;
	bHMDHiddenAreaMaskActive = IsHMDHiddenAreaMaskActive();
	bUseComputePasses = IsPostProcessingWithComputeEnabled(FeatureLevel);
	bHasCustomDepthPrimitives = false;
	bHasDistortionPrimitives = false;
	bAllowStencilDither = false;
	bCustomDepthStencilValid = false;
	bUsesCustomDepthStencilInTranslucentMaterials = false;

	NumBoxReflectionCaptures = 0;
	NumSphereReflectionCaptures = 0;
	FurthestReflectionCaptureDistance = 0;

	// Disable HDR encoding for editor elements.
	EditorSimpleElementCollector.BatchedElements.EnableMobileHDREncoding(false);
	
	TemporalJitterSequenceLength = 1;
	TemporalJitterIndex = 0;
	TemporalJitterPixels = FVector2D::ZeroVector;

	PreExposure = 1.0f;

	// Cache TEXTUREGROUP_World's for the render thread to create the material textures' shared sampler.
	if (IsInGameThread())
	{
		WorldTextureGroupSamplerFilter = (ESamplerFilter)UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings()->GetSamplerFilter(TEXTUREGROUP_World);
		bIsValidWorldTextureGroupSamplerFilter = true;
	}
	else
	{
		bIsValidWorldTextureGroupSamplerFilter = false;
	}

	PrimitiveSceneDataOverrideSRV = nullptr;
	PrimitiveSceneDataTextureOverrideRHI = nullptr;
	InstanceSceneDataOverrideSRV = nullptr;
	InstancePayloadDataOverrideSRV = nullptr;
	LightmapSceneDataOverrideSRV = nullptr;

	DitherFadeInUniformBuffer = nullptr;
	DitherFadeOutUniformBuffer = nullptr;

	for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; ++PassIndex)
	{
		NumVisibleDynamicMeshElements[PassIndex] = 0;
	}

	NumVisibleDynamicPrimitives = 0;
	NumVisibleDynamicEditorPrimitives = 0;

	StrataSceneData = nullptr;
	HairStrandsViewData = FHairStrandsViewData();

	GPUSceneViewId = INDEX_NONE;
}

void FViewInfo::WaitForTasks(FParallelMeshDrawCommandPass::EWaitThread WaitThread)
{
	for (int32 MeshDrawIndex = 0; MeshDrawIndex < EMeshPass::Num; MeshDrawIndex++)
	{
		ParallelMeshDrawCommandPasses[MeshDrawIndex].WaitForTasksAndEmpty(WaitThread);
	}
}

FViewInfo::~FViewInfo()
{
	for(int32 ResourceIndex = 0;ResourceIndex < DynamicResources.Num();ResourceIndex++)
	{
		DynamicResources[ResourceIndex]->ReleasePrimitiveResource();
	}
	if (CustomVisibilityQuery)
	{
		CustomVisibilityQuery->Release();
	}

	//this uses memstack allocation for strongrefs, so we need to manually empty to get the destructor called to not leak the uniformbuffers stored here.
	TranslucentSelfShadowUniformBufferMap.Empty();

#if RHI_RAYTRACING
	// Per-task structures are allocated using memstack so we have to call destructors explicitly.
	for (FRayTracingMeshCommandOneFrameArray* It : VisibleRayTracingMeshCommandsPerTask)
	{
		It->~FRayTracingMeshCommandOneFrameArray();
	}
	for (FDynamicRayTracingMeshCommandStorage* It : DynamicRayTracingMeshCommandStoragePerTask)
	{
		It->~FDynamicRayTracingMeshCommandStorage();
	}
#endif // RHI_RAYTRACING
}

#if RHI_RAYTRACING
bool FViewInfo::HasRayTracingScene() const
{
	check(Family);
	FScene* Scene = Family->Scene ? Family->Scene->GetRenderScene() : nullptr;
	if (Scene)
	{
		return Scene->RayTracingScene.IsCreated();
	}
	return false;
}

FRHIRayTracingScene* FViewInfo::GetRayTracingSceneChecked() const
{
	check(Family);
	if (Family->Scene)
	{
		if (FScene* Scene = Family->Scene->GetRenderScene())
		{
			FRHIRayTracingScene* Result = Scene->RayTracingScene.GetRHIRayTracingScene();
			checkf(Result, TEXT("Ray tracing scene is expected to be created at this point."));
			return Result;
		}
	}
	return nullptr;
}

FRHIShaderResourceView* FViewInfo::GetRayTracingSceneViewChecked() const
{
	FRHIShaderResourceView* Result = nullptr;
	check(Family);
	if (Family->Scene)
	{
		if (FScene* Scene = Family->Scene->GetRenderScene())
		{
			Result = Scene->RayTracingScene.GetShaderResourceViewChecked();
		}
	}
	checkf(Result, TEXT("Ray tracing scene SRV is expected to be created at this point."));
	return Result;
}
#endif // RHI_RAYTRACING

#if DO_CHECK || USING_CODE_ANALYSIS
bool FViewInfo::VerifyMembersChecks() const
{
	FSceneView::VerifyMembersChecks();

	check(ViewState == State);

	return true;
}
#endif

RENDERER_API void SetupSkyIrradianceEnvironmentMapConstantsFromSkyIrradiance(FVector4f* OutSkyIrradianceEnvironmentMap, const FSHVectorRGB3 SkyIrradiance)
{
	const float SqrtPI = FMath::Sqrt(PI);
	const float Coefficient0 = 1.0f / (2 * SqrtPI);
	const float Coefficient1 = FMath::Sqrt(3.f) / (3 * SqrtPI);
	const float Coefficient2 = FMath::Sqrt(15.f) / (8 * SqrtPI);
	const float Coefficient3 = FMath::Sqrt(5.f) / (16 * SqrtPI);
	const float Coefficient4 = .5f * Coefficient2;

	// Pack the SH coefficients in a way that makes applying the lighting use the least shader instructions
	// This has the diffuse convolution coefficients baked in
	// See "Stupid Spherical Harmonics (SH) Tricks"
	OutSkyIrradianceEnvironmentMap[0].X = -Coefficient1 * SkyIrradiance.R.V[3];
	OutSkyIrradianceEnvironmentMap[0].Y = -Coefficient1 * SkyIrradiance.R.V[1];
	OutSkyIrradianceEnvironmentMap[0].Z = Coefficient1 * SkyIrradiance.R.V[2];
	OutSkyIrradianceEnvironmentMap[0].W = Coefficient0 * SkyIrradiance.R.V[0] - Coefficient3 * SkyIrradiance.R.V[6];

	OutSkyIrradianceEnvironmentMap[1].X = -Coefficient1 * SkyIrradiance.G.V[3];
	OutSkyIrradianceEnvironmentMap[1].Y = -Coefficient1 * SkyIrradiance.G.V[1];
	OutSkyIrradianceEnvironmentMap[1].Z = Coefficient1 * SkyIrradiance.G.V[2];
	OutSkyIrradianceEnvironmentMap[1].W = Coefficient0 * SkyIrradiance.G.V[0] - Coefficient3 * SkyIrradiance.G.V[6];

	OutSkyIrradianceEnvironmentMap[2].X = -Coefficient1 * SkyIrradiance.B.V[3];
	OutSkyIrradianceEnvironmentMap[2].Y = -Coefficient1 * SkyIrradiance.B.V[1];
	OutSkyIrradianceEnvironmentMap[2].Z = Coefficient1 * SkyIrradiance.B.V[2];
	OutSkyIrradianceEnvironmentMap[2].W = Coefficient0 * SkyIrradiance.B.V[0] - Coefficient3 * SkyIrradiance.B.V[6];

	OutSkyIrradianceEnvironmentMap[3].X = Coefficient2 * SkyIrradiance.R.V[4];
	OutSkyIrradianceEnvironmentMap[3].Y = -Coefficient2 * SkyIrradiance.R.V[5];
	OutSkyIrradianceEnvironmentMap[3].Z = 3 * Coefficient3 * SkyIrradiance.R.V[6];
	OutSkyIrradianceEnvironmentMap[3].W = -Coefficient2 * SkyIrradiance.R.V[7];

	OutSkyIrradianceEnvironmentMap[4].X = Coefficient2 * SkyIrradiance.G.V[4];
	OutSkyIrradianceEnvironmentMap[4].Y = -Coefficient2 * SkyIrradiance.G.V[5];
	OutSkyIrradianceEnvironmentMap[4].Z = 3 * Coefficient3 * SkyIrradiance.G.V[6];
	OutSkyIrradianceEnvironmentMap[4].W = -Coefficient2 * SkyIrradiance.G.V[7];

	OutSkyIrradianceEnvironmentMap[5].X = Coefficient2 * SkyIrradiance.B.V[4];
	OutSkyIrradianceEnvironmentMap[5].Y = -Coefficient2 * SkyIrradiance.B.V[5];
	OutSkyIrradianceEnvironmentMap[5].Z = 3 * Coefficient3 * SkyIrradiance.B.V[6];
	OutSkyIrradianceEnvironmentMap[5].W = -Coefficient2 * SkyIrradiance.B.V[7];

	OutSkyIrradianceEnvironmentMap[6].X = Coefficient4 * SkyIrradiance.R.V[8];
	OutSkyIrradianceEnvironmentMap[6].Y = Coefficient4 * SkyIrradiance.G.V[8];
	OutSkyIrradianceEnvironmentMap[6].Z = Coefficient4 * SkyIrradiance.B.V[8];
	OutSkyIrradianceEnvironmentMap[6].W = 1;
}

void UpdateNoiseTextureParameters(FViewUniformShaderParameters& ViewUniformShaderParameters)
{
	if (GSystemTextures.PerlinNoiseGradient.GetReference())
	{
		ViewUniformShaderParameters.PerlinNoiseGradientTexture = (FTexture2DRHIRef&)GSystemTextures.PerlinNoiseGradient->GetRenderTargetItem().ShaderResourceTexture;
		SetBlack2DIfNull(ViewUniformShaderParameters.PerlinNoiseGradientTexture);
	}
	check(ViewUniformShaderParameters.PerlinNoiseGradientTexture);
	ViewUniformShaderParameters.PerlinNoiseGradientTextureSampler = TStaticSamplerState<SF_Point, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

	if (GSystemTextures.PerlinNoise3D.GetReference())
	{
		ViewUniformShaderParameters.PerlinNoise3DTexture = (FTexture3DRHIRef&)GSystemTextures.PerlinNoise3D->GetRenderTargetItem().ShaderResourceTexture;
		SetBlack3DIfNull(ViewUniformShaderParameters.PerlinNoise3DTexture);
	}
	check(ViewUniformShaderParameters.PerlinNoise3DTexture);
	ViewUniformShaderParameters.PerlinNoise3DTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

	if (GSystemTextures.SobolSampling.GetReference())
	{
		ViewUniformShaderParameters.SobolSamplingTexture = (FTexture2DRHIRef&)GSystemTextures.SobolSampling->GetRenderTargetItem().ShaderResourceTexture;
		SetBlack2DIfNull(ViewUniformShaderParameters.SobolSamplingTexture);
	}
	check(ViewUniformShaderParameters.SobolSamplingTexture);
}

void SetupPrecomputedVolumetricLightmapUniformBufferParameters(const FScene* Scene, FEngineShowFlags EngineShowFlags, FViewUniformShaderParameters& ViewUniformShaderParameters)
{
	if (Scene && Scene->VolumetricLightmapSceneData.GetLevelVolumetricLightmap() && EngineShowFlags.VolumetricLightmap)
	{
		const FPrecomputedVolumetricLightmapData* VolumetricLightmapData = Scene->VolumetricLightmapSceneData.GetLevelVolumetricLightmap()->Data;

		FVector BrickDimensions;
		const FVolumetricLightmapBasicBrickDataLayers* BrickData = nullptr;

#if WITH_EDITOR
		if (FStaticLightingSystemInterface::GetPrecomputedVolumetricLightmap(Scene->GetWorld()))
		{
			BrickDimensions = FVector(VolumetricLightmapData->BrickDataDimensions);
			BrickData = &VolumetricLightmapData->BrickData;
		}
		else
#endif
		{
			BrickDimensions = FVector(GVolumetricLightmapBrickAtlas.TextureSet.BrickDataDimensions);
			BrickData = &GVolumetricLightmapBrickAtlas.TextureSet;
		}

		ViewUniformShaderParameters.VolumetricLightmapIndirectionTexture = OrBlack3DUintIfNull(VolumetricLightmapData->IndirectionTexture.Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickAmbientVector = OrBlack3DIfNull(BrickData->AmbientVector.Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients0 = OrBlack3DIfNull(BrickData->SHCoefficients[0].Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients1 = OrBlack3DIfNull(BrickData->SHCoefficients[1].Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients2 = OrBlack3DIfNull(BrickData->SHCoefficients[2].Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients3 = OrBlack3DIfNull(BrickData->SHCoefficients[3].Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients4 = OrBlack3DIfNull(BrickData->SHCoefficients[4].Texture);
		ViewUniformShaderParameters.VolumetricLightmapBrickSHCoefficients5 = OrBlack3DIfNull(BrickData->SHCoefficients[5].Texture);
		ViewUniformShaderParameters.SkyBentNormalBrickTexture = OrBlack3DIfNull(BrickData->SkyBentNormal.Texture);
		ViewUniformShaderParameters.DirectionalLightShadowingBrickTexture = OrBlack3DIfNull(BrickData->DirectionalLightShadowing.Texture);

		const FBox VolumeBounds = VolumetricLightmapData->GetBounds();
		const FVector VolumeSize = VolumeBounds.GetSize();
		const FVector InvVolumeSize = VolumeSize.Reciprocal();

		const FVector InvBrickDimensions = BrickDimensions.Reciprocal();

		ViewUniformShaderParameters.VolumetricLightmapWorldToUVScale = (FVector3f)InvVolumeSize;
		ViewUniformShaderParameters.VolumetricLightmapWorldToUVAdd = FVector3f(-VolumeBounds.Min * InvVolumeSize);
		ViewUniformShaderParameters.VolumetricLightmapIndirectionTextureSize = FVector3f(VolumetricLightmapData->IndirectionTextureDimensions);
		ViewUniformShaderParameters.VolumetricLightmapBrickSize = VolumetricLightmapData->BrickSize;
		ViewUniformShaderParameters.VolumetricLightmapBrickTexelSize = (FVector3f)InvBrickDimensions;
	}
	else
	{
		// Resources are initialized in FViewUniformShaderParameters ctor, only need to set defaults for non-resource types

		ViewUniformShaderParameters.VolumetricLightmapWorldToUVScale = FVector3f::ZeroVector;
		ViewUniformShaderParameters.VolumetricLightmapWorldToUVAdd = FVector3f::ZeroVector;
		ViewUniformShaderParameters.VolumetricLightmapIndirectionTextureSize = FVector3f::ZeroVector;
		ViewUniformShaderParameters.VolumetricLightmapBrickSize = 0;
		ViewUniformShaderParameters.VolumetricLightmapBrickTexelSize = FVector3f::ZeroVector;
	}
}

void SetupPhysicsFieldUniformBufferParameters(const FScene* Scene, FEngineShowFlags EngineShowFlags, FViewUniformShaderParameters& ViewUniformShaderParameters)
{
	if (Scene && Scene->PhysicsField && Scene->PhysicsField->FieldResource)
	{
		FPhysicsFieldResource* FieldResource = Scene->PhysicsField->FieldResource;
		ViewUniformShaderParameters.PhysicsFieldClipmapBuffer = FieldResource->ClipmapBuffer.SRV.GetReference();
		ViewUniformShaderParameters.PhysicsFieldClipmapCenter = (FVector3f)FieldResource->FieldInfos.ClipmapCenter;
		ViewUniformShaderParameters.PhysicsFieldClipmapDistance = FieldResource->FieldInfos.ClipmapDistance;
		ViewUniformShaderParameters.PhysicsFieldClipmapResolution = FieldResource->FieldInfos.ClipmapResolution;
		ViewUniformShaderParameters.PhysicsFieldClipmapExponent = FieldResource->FieldInfos.ClipmapExponent;
		ViewUniformShaderParameters.PhysicsFieldClipmapCount = FieldResource->FieldInfos.ClipmapCount;
		ViewUniformShaderParameters.PhysicsFieldTargetCount = FieldResource->FieldInfos.TargetCount;
		for (int32 Index = 0; Index < MAX_PHYSICS_FIELD_TARGETS; ++Index)
		{
			ViewUniformShaderParameters.PhysicsFieldTargets[Index].X = FieldResource->FieldInfos.VectorTargets[Index];
			ViewUniformShaderParameters.PhysicsFieldTargets[Index].Y = FieldResource->FieldInfos.ScalarTargets[Index];
			ViewUniformShaderParameters.PhysicsFieldTargets[Index].Z = FieldResource->FieldInfos.IntegerTargets[Index];
			ViewUniformShaderParameters.PhysicsFieldTargets[Index].W = 0; // Padding
		}
	}
	else
	{
		TStaticArray<FIntVector4, MAX_PHYSICS_FIELD_TARGETS, 16> EmptyTargets;
		ViewUniformShaderParameters.PhysicsFieldClipmapBuffer = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
		ViewUniformShaderParameters.PhysicsFieldClipmapCenter = FVector3f::ZeroVector;
		ViewUniformShaderParameters.PhysicsFieldClipmapDistance = 1.0;
		ViewUniformShaderParameters.PhysicsFieldClipmapResolution = 2;
		ViewUniformShaderParameters.PhysicsFieldClipmapExponent = 1;
		ViewUniformShaderParameters.PhysicsFieldClipmapCount = 1;
		ViewUniformShaderParameters.PhysicsFieldTargetCount = 0;
		ViewUniformShaderParameters.PhysicsFieldTargets = EmptyTargets;
	}
}


FIntPoint FViewInfo::GetSecondaryViewRectSize() const
{
	return FIntPoint(
		FMath::CeilToInt(UnscaledViewRect.Width() * Family->SecondaryViewFraction),
		FMath::CeilToInt(UnscaledViewRect.Height() * Family->SecondaryViewFraction));
}

/** Creates the view's uniform buffers given a set of view transforms. */
void FViewInfo::SetupUniformBufferParameters(
	const FViewMatrices& InViewMatrices,
	const FViewMatrices& InPrevViewMatrices,
	FBox* OutTranslucentCascadeBoundsArray,
	int32 NumTranslucentCascades,
	FViewUniformShaderParameters& ViewUniformShaderParameters) const
{
	check(Family);

	const FSceneTexturesConfig& SceneTexturesConfig = FSceneTexturesConfig::Get();

	// Create the view's uniform buffer.

	// Mobile multi-view is not side by side
	const FIntRect EffectiveViewRect = (bIsMobileMultiViewEnabled) ? FIntRect(0, 0, ViewRect.Width(), ViewRect.Height()) : ViewRect;

	// Scene render targets may not be created yet; avoids NaNs.
	FIntPoint EffectiveBufferSize = SceneTexturesConfig.Extent;
	EffectiveBufferSize.X = FMath::Max(EffectiveBufferSize.X, 1);
	EffectiveBufferSize.Y = FMath::Max(EffectiveBufferSize.Y, 1);

	// TODO: We should use a view and previous view uniform buffer to avoid code duplication and keep consistency
	SetupCommonViewUniformBufferParameters(
		ViewUniformShaderParameters,
		EffectiveBufferSize,
		SceneTexturesConfig.NumSamples,
		EffectiveViewRect,
		InViewMatrices,
		InPrevViewMatrices
	);

	const bool bCheckerboardSubsurfaceRendering = IsSubsurfaceCheckerboardFormat(SceneTexturesConfig.ColorFormat);
	ViewUniformShaderParameters.bCheckerboardSubsurfaceProfileRendering = bCheckerboardSubsurfaceRendering ? 1.0f : 0.0f;

	ViewUniformShaderParameters.IndirectLightingCacheShowFlag = Family->EngineShowFlags.IndirectLightingCache;

	FScene* Scene = nullptr;

	if (Family->Scene)
	{
		Scene = Family->Scene->GetRenderScene();
	}

	const FVector DefaultSunDirection(0.0f, 0.0f, 1.0f); // Up vector so that the AtmosphericLightVector node always output a valid direction.
	auto ClearAtmosphereLightData = [&](uint32 Index)
	{
		check(Index < NUM_ATMOSPHERE_LIGHTS);
		ViewUniformShaderParameters.AtmosphereLightDiscCosHalfApexAngle[Index] = FVector4f(1.0f);
		ViewUniformShaderParameters.AtmosphereLightDiscLuminance[Index] = FLinearColor::Black;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index] = FLinearColor::Black;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index].A = 0.0f;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[Index] = FLinearColor::Black;

		// We must set a default atmospheric light0 direction because this is use for instance by the height fog directional lobe. And we do not want to add an in shader test for that.
		ViewUniformShaderParameters.AtmosphereLightDirection[Index] = FVector3f(Index == 0 && Scene && Scene->SimpleDirectionalLight && Scene->SimpleDirectionalLight->Proxy ? -Scene->SimpleDirectionalLight->Proxy->GetDirection() : DefaultSunDirection);
	};

	if (Scene)
	{
		if (Scene->SimpleDirectionalLight)
		{
			ViewUniformShaderParameters.DirectionalLightColor = Scene->SimpleDirectionalLight->Proxy->GetAtmosphereTransmittanceTowardSun() * Scene->SimpleDirectionalLight->Proxy->GetColor() / PI;
			ViewUniformShaderParameters.DirectionalLightDirection = -(FVector3f)Scene->SimpleDirectionalLight->Proxy->GetDirection();
		}
		else
		{
			ViewUniformShaderParameters.DirectionalLightColor = FLinearColor::Black;
			ViewUniformShaderParameters.DirectionalLightDirection = FVector3f::ZeroVector;
		}

		// Set default atmosphere lights parameters
		FLightSceneInfo* SunLight = Scene->AtmosphereLights[0];	// Atmospheric fog only takes into account the a single sun light with index 0.
		const float SunLightDiskHalfApexAngleRadian = SunLight ? SunLight->Proxy->GetSunLightHalfApexAngleRadian() : FLightSceneProxy::GetSunOnEarthHalfApexAngleRadian();

		ViewUniformShaderParameters.AtmosphereLightDiscCosHalfApexAngle[0] = FVector4f(FMath::Cos(SunLightDiskHalfApexAngleRadian));
		//Added check so atmospheric light color and vector can use a directional light without needing an atmospheric fog actor in the scene
		ViewUniformShaderParameters.AtmosphereLightDiscLuminance[0] = SunLight ? SunLight->Proxy->GetOuterSpaceLuminance() : FLinearColor::Black;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[0] = SunLight ? SunLight->Proxy->GetColor() : FLinearColor::Black;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[0].A = 0.0f;
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[0] = ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[0];
		ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[0].A = 0.0f;
		ViewUniformShaderParameters.AtmosphereLightDirection[0] = FVector3f(SunLight ? -SunLight->Proxy->GetDirection() : DefaultSunDirection);

		// Do not clear the first AtmosphereLight data, it has been setup above
		for (uint8 Index = 1; Index < NUM_ATMOSPHERE_LIGHTS; ++Index)
		{
			ClearAtmosphereLightData(Index);
		}
	}

	FRHITexture* TransmittanceLutTextureFound = nullptr;
	FRHITexture* SkyViewLutTextureFound = nullptr;
	FRHITexture* CameraAerialPerspectiveVolumeFound = nullptr;
	FRHITexture* DistantSkyLightLutTextureFound = nullptr;
	if (ShouldRenderSkyAtmosphere(Scene, Family->EngineShowFlags))
	{
		ViewUniformShaderParameters.SkyAtmospherePresentInScene = 1.0f;

		FSkyAtmosphereRenderSceneInfo* SkyAtmosphere = Scene->SkyAtmosphere;
		const FSkyAtmosphereSceneProxy& SkyAtmosphereSceneProxy = SkyAtmosphere->GetSkyAtmosphereSceneProxy();

		// Get access to texture resource if we have valid pointer.
		// (Valid pointer checks are needed because some resources might not have been initialized when coming from FCanvasTileRendererItem or FCanvasTriangleRendererItem)

		const TRefCountPtr<IPooledRenderTarget>& PooledTransmittanceLutTexture = SkyAtmosphere->GetTransmittanceLutTexture();
		if (PooledTransmittanceLutTexture.IsValid())
		{
			TransmittanceLutTextureFound = PooledTransmittanceLutTexture->GetRenderTargetItem().ShaderResourceTexture;
		}
		const TRefCountPtr<IPooledRenderTarget>& PooledDistantSkyLightLutTexture = SkyAtmosphere->GetDistantSkyLightLutTexture();
		if (PooledDistantSkyLightLutTexture.IsValid())
		{
			DistantSkyLightLutTextureFound = PooledDistantSkyLightLutTexture->GetRenderTargetItem().ShaderResourceTexture;
		}

		if (this->SkyAtmosphereCameraAerialPerspectiveVolume.IsValid())
		{
			CameraAerialPerspectiveVolumeFound = this->SkyAtmosphereCameraAerialPerspectiveVolume->GetRenderTargetItem().ShaderResourceTexture;
		}

		float SkyViewLutWidth = 1.0f;
		float SkyViewLutHeight = 1.0f;
		if (this->SkyAtmosphereViewLutTexture.IsValid())
		{
			SkyViewLutTextureFound = this->SkyAtmosphereViewLutTexture->GetRenderTargetItem().ShaderResourceTexture;
			SkyViewLutWidth = float(this->SkyAtmosphereViewLutTexture->GetDesc().GetSize().X);
			SkyViewLutHeight = float(this->SkyAtmosphereViewLutTexture->GetDesc().GetSize().Y);
		}
		ViewUniformShaderParameters.SkyViewLutSizeAndInvSize = FVector4f(SkyViewLutWidth, SkyViewLutHeight, 1.0f / SkyViewLutWidth, 1.0f / SkyViewLutHeight);

		// Now initialize remaining view parameters.

		const FAtmosphereSetup& AtmosphereSetup = SkyAtmosphereSceneProxy.GetAtmosphereSetup();
		ViewUniformShaderParameters.SkyAtmosphereBottomRadiusKm = AtmosphereSetup.BottomRadiusKm;
		ViewUniformShaderParameters.SkyAtmosphereTopRadiusKm = AtmosphereSetup.TopRadiusKm;

		FSkyAtmosphereViewSharedUniformShaderParameters OutParameters;
		SetupSkyAtmosphereViewSharedUniformShaderParameters(*this, SkyAtmosphereSceneProxy, OutParameters);
		ViewUniformShaderParameters.SkyAtmosphereAerialPerspectiveStartDepthKm = OutParameters.AerialPerspectiveStartDepthKm;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeSizeAndInvSize = OutParameters.CameraAerialPerspectiveVolumeSizeAndInvSize;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthResolution = OutParameters.CameraAerialPerspectiveVolumeDepthResolution;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthResolutionInv = OutParameters.CameraAerialPerspectiveVolumeDepthResolutionInv;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthSliceLengthKm = OutParameters.CameraAerialPerspectiveVolumeDepthSliceLengthKm;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthSliceLengthKmInv = OutParameters.CameraAerialPerspectiveVolumeDepthSliceLengthKmInv;
		ViewUniformShaderParameters.SkyAtmosphereApplyCameraAerialPerspectiveVolume = OutParameters.ApplyCameraAerialPerspectiveVolume;
		ViewUniformShaderParameters.SkyAtmosphereSkyLuminanceFactor = SkyAtmosphereSceneProxy.GetSkyLuminanceFactor();
		ViewUniformShaderParameters.SkyAtmosphereHeightFogContribution = SkyAtmosphereSceneProxy.GetHeightFogContribution();

		// Fill atmosphere lights shader parameters
		for (uint8 Index = 0; Index < NUM_ATMOSPHERE_LIGHTS; ++Index)
		{
			FLightSceneInfo* Light = Scene->AtmosphereLights[Index];
			if (Light)
			{
				ViewUniformShaderParameters.AtmosphereLightDiscCosHalfApexAngle[Index] = FVector4f(FMath::Cos(Light->Proxy->GetSunLightHalfApexAngleRadian()));
				ViewUniformShaderParameters.AtmosphereLightDiscLuminance[Index] = Light->Proxy->GetOuterSpaceLuminance();
				ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index] = Light->Proxy->GetSunIlluminanceOnGroundPostTransmittance();
				ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index].A = 1.0f; // interactions with HeightFogComponent
				ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[Index] = Light->Proxy->GetOuterSpaceIlluminance();
				ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[Index].A = 1.0f;
				ViewUniformShaderParameters.AtmosphereLightDirection[Index] = FVector3f(SkyAtmosphereSceneProxy.GetAtmosphereLightDirection(Index, -Light->Proxy->GetDirection()));
			}
			else
			{
				ClearAtmosphereLightData(Index);
			}
		}

		// Regular view sampling of the SkyViewLUT. This is only changed when sampled from a sky material for the real time reflection capture around sky light position)
		FVector3f SkyCameraTranslatedWorldOrigin;
		FMatrix44f SkyViewLutReferential;
		FVector4f TempSkyPlanetData;
		AtmosphereSetup.ComputeViewData(
			InViewMatrices.GetViewOrigin(), InViewMatrices.GetPreViewTranslation(), ViewUniformShaderParameters.ViewForward, ViewUniformShaderParameters.ViewRight,
			SkyCameraTranslatedWorldOrigin, TempSkyPlanetData, SkyViewLutReferential);
		// LWC_TODO: Precision loss
		ViewUniformShaderParameters.SkyPlanetTranslatedWorldCenterAndViewHeight = FVector4f(TempSkyPlanetData);
		ViewUniformShaderParameters.SkyCameraTranslatedWorldOrigin = SkyCameraTranslatedWorldOrigin;
		ViewUniformShaderParameters.SkyViewLutReferential = SkyViewLutReferential;
	}
	else
	{
		ViewUniformShaderParameters.SkyAtmospherePresentInScene = 0.0f;
		ViewUniformShaderParameters.SkyAtmosphereHeightFogContribution = 0.0f;
		ViewUniformShaderParameters.SkyViewLutSizeAndInvSize = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		ViewUniformShaderParameters.SkyAtmosphereBottomRadiusKm = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereTopRadiusKm = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereSkyLuminanceFactor = FLinearColor::White;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeSizeAndInvSize = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		ViewUniformShaderParameters.SkyAtmosphereAerialPerspectiveStartDepthKm = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthResolution = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthResolutionInv = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthSliceLengthKm = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereCameraAerialPerspectiveVolumeDepthSliceLengthKmInv = 1.0f;
		ViewUniformShaderParameters.SkyAtmosphereApplyCameraAerialPerspectiveVolume = 0.0f;
		ViewUniformShaderParameters.SkyCameraTranslatedWorldOrigin = ViewUniformShaderParameters.RelativeWorldCameraOrigin;
		ViewUniformShaderParameters.SkyPlanetTranslatedWorldCenterAndViewHeight = FVector4f(ForceInitToZero);
		ViewUniformShaderParameters.SkyViewLutReferential = FMatrix44f::Identity;

		if(Scene)
		{
			// Fill atmosphere lights shader parameters even without any SkyAtmosphere component.
			// This is to always make these parameters usable, for instance by the VolumetricCloud component.
			for (uint8 Index = 0; Index < NUM_ATMOSPHERE_LIGHTS; ++Index)
			{
				FLightSceneInfo* Light = Scene->AtmosphereLights[Index];
				if (Light)
				{
					ViewUniformShaderParameters.AtmosphereLightDiscCosHalfApexAngle[Index] = FVector4f(1.0f);
					ViewUniformShaderParameters.AtmosphereLightDiscLuminance[Index] = FLinearColor::Black;
					ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index] = Light->Proxy->GetColor();
					ViewUniformShaderParameters.AtmosphereLightIlluminanceOnGroundPostTransmittance[Index].A = 0.0f; // no interactions with HeightFogComponent
					ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[Index] = Light->Proxy->GetColor();
					ViewUniformShaderParameters.AtmosphereLightIlluminanceOuterSpace[0].A = 0.0f;
					ViewUniformShaderParameters.AtmosphereLightDirection[Index] = FVector3f(-Light->Proxy->GetDirection());
				}
				else
				{
					ClearAtmosphereLightData(Index);
				}
			}
		}
		else if (!Scene)
		{
			for (uint8 Index = 0; Index < NUM_ATMOSPHERE_LIGHTS; ++Index)
			{
				ClearAtmosphereLightData(Index);
			}
		}
	}

	ViewUniformShaderParameters.TransmittanceLutTexture = OrWhite2DIfNull(TransmittanceLutTextureFound);
	ViewUniformShaderParameters.TransmittanceLutTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	ViewUniformShaderParameters.DistantSkyLightLutTexture = OrBlack2DIfNull(DistantSkyLightLutTextureFound);
	ViewUniformShaderParameters.DistantSkyLightLutTextureSampler = TStaticSamplerState<SF_Point, AM_Wrap, AM_Wrap>::GetRHI();
	ViewUniformShaderParameters.SkyViewLutTexture = OrBlack2DIfNull(SkyViewLutTextureFound);
	ViewUniformShaderParameters.SkyViewLutTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	ViewUniformShaderParameters.CameraAerialPerspectiveVolume = OrBlack3DAlpha1IfNull(CameraAerialPerspectiveVolumeFound);
	ViewUniformShaderParameters.CameraAerialPerspectiveVolumeSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

	ViewUniformShaderParameters.AtmosphereTransmittanceTexture = OrBlack2DIfNull(AtmosphereTransmittanceTexture);
	ViewUniformShaderParameters.AtmosphereIrradianceTexture = OrBlack2DIfNull(AtmosphereIrradianceTexture);
	ViewUniformShaderParameters.AtmosphereInscatterTexture = OrBlack3DIfNull(AtmosphereInscatterTexture);

	ViewUniformShaderParameters.AtmosphereTransmittanceTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	ViewUniformShaderParameters.AtmosphereIrradianceTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	ViewUniformShaderParameters.AtmosphereInscatterTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

	// This should probably be in SetupCommonViewUniformBufferParameters, but drags in too many dependencies
	UpdateNoiseTextureParameters(ViewUniformShaderParameters);

	SetupDefaultGlobalDistanceFieldUniformBufferParameters(ViewUniformShaderParameters);

	SetupVolumetricFogUniformBufferParameters(ViewUniformShaderParameters);

	SetupPrecomputedVolumetricLightmapUniformBufferParameters(Scene, Family->EngineShowFlags, ViewUniformShaderParameters);

	SetupPhysicsFieldUniformBufferParameters(Scene, Family->EngineShowFlags, ViewUniformShaderParameters);

	// Setup view's shared sampler for material texture sampling.
	{
		const float GlobalMipBias = UTexture2D::GetGlobalMipMapLODBias();

		float FinalMaterialTextureMipBias = GlobalMipBias;

		if (bIsValidWorldTextureGroupSamplerFilter && !FMath::IsNearlyZero(MaterialTextureMipBias))
		{
			ViewUniformShaderParameters.MaterialTextureMipBias = MaterialTextureMipBias;
			ViewUniformShaderParameters.MaterialTextureDerivativeMultiply = FMath::Pow(2.0f, MaterialTextureMipBias);

			FinalMaterialTextureMipBias += MaterialTextureMipBias;
		}

		FSamplerStateRHIRef WrappedSampler = nullptr;
		FSamplerStateRHIRef ClampedSampler = nullptr;

		if (FMath::Abs(FinalMaterialTextureMipBias - GlobalMipBias) < KINDA_SMALL_NUMBER)
		{
			WrappedSampler = Wrap_WorldGroupSettings->SamplerStateRHI;
			ClampedSampler = Clamp_WorldGroupSettings->SamplerStateRHI;
		}
		else if (ViewState && FMath::Abs(ViewState->MaterialTextureCachedMipBias - FinalMaterialTextureMipBias) < KINDA_SMALL_NUMBER)
		{
			WrappedSampler = ViewState->MaterialTextureBilinearWrapedSamplerCache;
			ClampedSampler = ViewState->MaterialTextureBilinearClampedSamplerCache;
		}
		else
		{
			check(bIsValidWorldTextureGroupSamplerFilter);

			WrappedSampler = RHICreateSamplerState(FSamplerStateInitializerRHI(WorldTextureGroupSamplerFilter, AM_Wrap,  AM_Wrap,  AM_Wrap,  FinalMaterialTextureMipBias));
			ClampedSampler = RHICreateSamplerState(FSamplerStateInitializerRHI(WorldTextureGroupSamplerFilter, AM_Clamp, AM_Clamp, AM_Clamp, FinalMaterialTextureMipBias));
		}

		// At this point, a sampler must be set.
		check(WrappedSampler.IsValid());
		check(ClampedSampler.IsValid());

		ViewUniformShaderParameters.MaterialTextureBilinearWrapedSampler = WrappedSampler;
		ViewUniformShaderParameters.MaterialTextureBilinearClampedSampler = ClampedSampler;

		// Update view state's cached sampler.
		if (ViewState && ViewState->MaterialTextureBilinearWrapedSamplerCache != WrappedSampler)
		{
			ViewState->MaterialTextureCachedMipBias = FinalMaterialTextureMipBias;
			ViewState->MaterialTextureBilinearWrapedSamplerCache = WrappedSampler;
			ViewState->MaterialTextureBilinearClampedSamplerCache = ClampedSampler;
		}
	}

	{
		ensureMsgf(TemporalJitterSequenceLength == 1 || IsTemporalAccumulationBasedMethod(AntiAliasingMethod),
			TEXT("TemporalJitterSequenceLength = %i is invalid"), TemporalJitterSequenceLength);
		ensureMsgf(TemporalJitterIndex >= 0 && TemporalJitterIndex < TemporalJitterSequenceLength,
			TEXT("TemporalJitterIndex = %i is invalid (TemporalJitterSequenceLength = %i)"), TemporalJitterIndex, TemporalJitterSequenceLength);
		ViewUniformShaderParameters.TemporalAAParams = FVector4f(
			TemporalJitterIndex, 
			TemporalJitterSequenceLength,
			TemporalJitterPixels.X,
			TemporalJitterPixels.Y);
	}

	{
		EMainTAAPassConfig MainTAAPass = ITemporalUpscaler::GetMainTAAPassConfig(*this);

		// Gen4 TAA have the AA_DYNAMIC_ANTIGHOST heuristic that reject history based on whether the pixel is static or dynamic geometry
		// through whether the velocity has been drawn by the base pass.
		ViewUniformShaderParameters.ForceDrawAllVelocities = (
			CVarBasePassForceOutputsVelocity.GetValueOnRenderThread() ||
			MainTAAPass != EMainTAAPassConfig::TAA);
	}

	uint32 FrameIndex = 0;
	if (ViewState)
	{
		FrameIndex = ViewState->GetFrameIndex();
	}

	// TODO(GA): kill StateFrameIndexMod8 because this is only a scalar bit mask with StateFrameIndex anyway.
	ViewUniformShaderParameters.StateFrameIndexMod8 = FrameIndex % 8;
	ViewUniformShaderParameters.StateFrameIndex = FrameIndex;

	{
		// If rendering in stereo, the other stereo passes uses the left eye's translucency lighting volume.
		const FViewInfo* PrimaryView = GetPrimaryView();
		PrimaryView->CalcTranslucencyLightingVolumeBounds(OutTranslucentCascadeBoundsArray, NumTranslucentCascades);

		const int32 TranslucencyLightingVolumeDim = GetTranslucencyLightingVolumeDim();
		for (int32 CascadeIndex = 0; CascadeIndex < NumTranslucentCascades; CascadeIndex++)
		{
			const float VolumeVoxelSize = (OutTranslucentCascadeBoundsArray[CascadeIndex].Max.X - OutTranslucentCascadeBoundsArray[CascadeIndex].Min.X) / TranslucencyLightingVolumeDim;
			const FVector VolumeWorldMin = OutTranslucentCascadeBoundsArray[CascadeIndex].Min;
			const FVector3f VolumeSize = FVector3f(OutTranslucentCascadeBoundsArray[CascadeIndex].Max - VolumeWorldMin);
			const FVector3f VolumeTranslatedWorldMin = FVector3f(VolumeWorldMin + PrimaryView->ViewMatrices.GetPreViewTranslation());

			ViewUniformShaderParameters.TranslucencyLightingVolumeMin[CascadeIndex] = FVector4f(VolumeTranslatedWorldMin, 1.0f / TranslucencyLightingVolumeDim);
			ViewUniformShaderParameters.TranslucencyLightingVolumeInvSize[CascadeIndex] = FVector4f(FVector3f(1.0f) / VolumeSize, VolumeVoxelSize);
		}
	}
	
	ViewUniformShaderParameters.PreExposure = PreExposure;
	ViewUniformShaderParameters.OneOverPreExposure = 1.f / PreExposure;

	ViewUniformShaderParameters.DepthOfFieldFocalDistance = FinalPostProcessSettings.DepthOfFieldFocalDistance;
	ViewUniformShaderParameters.DepthOfFieldSensorWidth = FinalPostProcessSettings.DepthOfFieldSensorWidth;
	ViewUniformShaderParameters.DepthOfFieldFocalRegion = FinalPostProcessSettings.DepthOfFieldFocalRegion;
	// clamped to avoid div by 0 in shader
	ViewUniformShaderParameters.DepthOfFieldNearTransitionRegion = FMath::Max(0.01f, FinalPostProcessSettings.DepthOfFieldNearTransitionRegion);
	// clamped to avoid div by 0 in shader
	ViewUniformShaderParameters.DepthOfFieldFarTransitionRegion = FMath::Max(0.01f, FinalPostProcessSettings.DepthOfFieldFarTransitionRegion);
	ViewUniformShaderParameters.DepthOfFieldScale = FinalPostProcessSettings.DepthOfFieldScale;
	ViewUniformShaderParameters.DepthOfFieldFocalLength = 50.0f;

	// Subsurface
	{
		ViewUniformShaderParameters.bSubsurfacePostprocessEnabled = IsSubsurfaceEnabled() ? 1.0f : 0.0f;

		// Profiles
		{
			FRHITexture* Texture = GetSubsurfaceProfileTextureWithFallback();
			FIntVector TextureSize = Texture->GetSizeXYZ();
			ViewUniformShaderParameters.SSProfilesTextureSizeAndInvSize = FVector4f(TextureSize.X, TextureSize.Y, 1.0f / TextureSize.X, 1.0f / TextureSize.Y);
			ViewUniformShaderParameters.SSProfilesTexture = Texture;
			ViewUniformShaderParameters.SSProfilesSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			ViewUniformShaderParameters.SSProfilesTransmissionSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}

		// Pre-integrated profiles
		{
			FRHITexture* Texture = GetSSProfilesPreIntegratedTextureWithFallback();
			FIntVector TextureSize = Texture->GetSizeXYZ();
			ViewUniformShaderParameters.SSProfilesPreIntegratedTextureSizeAndInvSize = FVector4f(TextureSize.X, TextureSize.Y, 1.0f / TextureSize.X, 1.0f / TextureSize.Y);
			ViewUniformShaderParameters.SSProfilesPreIntegratedTexture = Texture;
			ViewUniformShaderParameters.SSProfilesPreIntegratedSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}
	}

	{
		// This is the CVar default
		float Value = 1.0f;
		float Value2 = 1.0f;

		// Compiled out in SHIPPING to make cheating a bit harder.
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		Value = CVarGeneralPurposeTweak.GetValueOnRenderThread();
		Value2 = CVarGeneralPurposeTweak2.GetValueOnRenderThread();
#endif

		ViewUniformShaderParameters.GeneralPurposeTweak = Value;
		ViewUniformShaderParameters.GeneralPurposeTweak2 = Value2;
	}

	ViewUniformShaderParameters.DemosaicVposOffset = 0.0f;
	{
		ViewUniformShaderParameters.DemosaicVposOffset = CVarDemosaicVposOffset.GetValueOnRenderThread();
	}

	ViewUniformShaderParameters.DecalDepthBias = CVarDecalDepthBias.GetValueOnRenderThread();

	ERHIFeatureLevel::Type RHIFeatureLevel = Scene == nullptr ? GMaxRHIFeatureLevel : Scene->GetFeatureLevel();
	EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[RHIFeatureLevel];

	ViewUniformShaderParameters.IndirectLightingColorScale = FVector3f(FinalPostProcessSettings.IndirectLightingColor.R * FinalPostProcessSettings.IndirectLightingIntensity,
		FinalPostProcessSettings.IndirectLightingColor.G * FinalPostProcessSettings.IndirectLightingIntensity,
		FinalPostProcessSettings.IndirectLightingColor.B * FinalPostProcessSettings.IndirectLightingIntensity);

	ViewUniformShaderParameters.PrecomputedIndirectLightingColorScale = ViewUniformShaderParameters.IndirectLightingColorScale;

	// If Lumen Dynamic GI is enabled then we don't want GI from Lightmaps
	// Note: this has the side effect of removing direct lighting from Static Lights
	if (ShouldRenderLumenDiffuseGI(Scene, *this))
	{
		ViewUniformShaderParameters.PrecomputedIndirectLightingColorScale = FVector3f::ZeroVector;
	}

	ViewUniformShaderParameters.PrecomputedIndirectSpecularColorScale = ViewUniformShaderParameters.IndirectLightingColorScale;

	// If Lumen Reflections are enabled then we don't want precomputed reflections from reflection captures
	// Note: this has the side effect of removing direct specular from Static Lights
	if (ShouldRenderLumenReflections(*this))
	{
		ViewUniformShaderParameters.PrecomputedIndirectSpecularColorScale = FVector3f::ZeroVector;
	}

	ViewUniformShaderParameters.NormalCurvatureToRoughnessScaleBias.X = FMath::Clamp(CVarNormalCurvatureToRoughnessScale.GetValueOnAnyThread(), 0.0f, 2.0f);
	ViewUniformShaderParameters.NormalCurvatureToRoughnessScaleBias.Y = FMath::Clamp(CVarNormalCurvatureToRoughnessBias.GetValueOnAnyThread(), -1.0f, 1.0f);
	ViewUniformShaderParameters.NormalCurvatureToRoughnessScaleBias.Z = FMath::Clamp(CVarNormalCurvatureToRoughnessExponent.GetValueOnAnyThread(), .05f, 20.0f);

	ViewUniformShaderParameters.RenderingReflectionCaptureMask = bIsReflectionCapture ? 1.0f : 0.0f;
	ViewUniformShaderParameters.RealTimeReflectionCapture = 0.0f;
	ViewUniformShaderParameters.RealTimeReflectionCapturePreExposure = 1.0f; // This must be 1 for now. If changed, we need to update the SkyLight AverageExposure and take it into account when sampling sky specular and diffuse irradiance.

	ViewUniformShaderParameters.AmbientCubemapTint = FinalPostProcessSettings.AmbientCubemapTint;
	ViewUniformShaderParameters.AmbientCubemapIntensity = FinalPostProcessSettings.AmbientCubemapIntensity;

	ViewUniformShaderParameters.CircleDOFParams = DiaphragmDOF::CircleDofHalfCoc(*this);

	if (Scene && Scene->SkyLight)
	{
		FSkyLightSceneProxy* SkyLight = Scene->SkyLight;

		// Setup the sky color mulitpler, and use it to nullify the sky contribution in case SkyLighting is disabled.
		// Note: we cannot simply select the base pass shader permutation skylight=0 because we would need to trigger bScenesPrimitivesNeedStaticMeshElementUpdate.
		// However, this would need to be done per view (showflag is per view) and this is not possible today as it is selected within the scene. 
		// So we simply nullify the sky light diffuse contribution. Reflection are handled by the indirect lighting render pass.
		ViewUniformShaderParameters.SkyLightColor = Family->EngineShowFlags.SkyLighting ? SkyLight->GetEffectiveLightColor() : FLinearColor::Black;

		bool bApplyPrecomputedBentNormalShadowing = 
			SkyLight->bCastShadows 
			&& SkyLight->bWantsStaticShadowing;

		ViewUniformShaderParameters.SkyLightApplyPrecomputedBentNormalShadowingFlag = bApplyPrecomputedBentNormalShadowing ? 1.0f : 0.0f;
		ViewUniformShaderParameters.SkyLightAffectReflectionFlag = SkyLight->bAffectReflection ? 1.0f : 0.0f;
		ViewUniformShaderParameters.SkyLightAffectGlobalIlluminationFlag = SkyLight->bAffectGlobalIllumination ? 1.0f : 0.0f;
	}
	else
	{
		ViewUniformShaderParameters.SkyLightColor = FLinearColor::Black;
		ViewUniformShaderParameters.SkyLightApplyPrecomputedBentNormalShadowingFlag = 0.0f;
		ViewUniformShaderParameters.SkyLightAffectReflectionFlag = 0.0f;
		ViewUniformShaderParameters.SkyLightAffectGlobalIlluminationFlag = 0.0f;
	}

	if (RHIFeatureLevel == ERHIFeatureLevel::ES3_1)
	{
		// Make sure there's no padding since we're going to cast to FVector4f*
		static_assert(sizeof(ViewUniformShaderParameters.MobileSkyIrradianceEnvironmentMap) == sizeof(FVector4f) * 7, "unexpected sizeof ViewUniformShaderParameters.MobileSkyIrradianceEnvironmentMap");

		const bool bSetupSkyIrradiance = Scene
			&& Scene->SkyLight
			// Skylights with static lighting already had their diffuse contribution baked into lightmaps
			&& !Scene->SkyLight->bHasStaticLighting
			&& Family->EngineShowFlags.SkyLighting;

		if (bSetupSkyIrradiance)
		{
			const FSHVectorRGB3& SkyIrradiance = Scene->SkyLight->IrradianceEnvironmentMap;
			SetupSkyIrradianceEnvironmentMapConstantsFromSkyIrradiance((FVector4f*)&ViewUniformShaderParameters.MobileSkyIrradianceEnvironmentMap, SkyIrradiance);
		}
		else
		{
			FMemory::Memzero((FVector4f*)&ViewUniformShaderParameters.MobileSkyIrradianceEnvironmentMap, sizeof(FVector4f) * 7);
		}
	}
	else
	{
		if (Scene && Scene->SkyIrradianceEnvironmentMap.SRV)
		{
			ViewUniformShaderParameters.SkyIrradianceEnvironmentMap = Scene->SkyIrradianceEnvironmentMap.SRV;
		}
		else
		{
			ViewUniformShaderParameters.SkyIrradianceEnvironmentMap = GIdentityPrimitiveBuffer.SkyIrradianceEnvironmentMapSRV;
		}
	}
	ViewUniformShaderParameters.MobilePreviewMode =
		(GIsEditor &&
		(RHIFeatureLevel == ERHIFeatureLevel::ES3_1) &&
		GMaxRHIFeatureLevel > ERHIFeatureLevel::ES3_1) ? 1.0f : 0.0f;

	// Padding between the left and right eye may be introduced by an HMD, which instanced stereo needs to account for.
	if ((IStereoRendering::IsStereoEyePass(StereoPass)) && (Family->Views.Num() > 1))
	{
		check(Family->Views.Num() >= 2);

		// The static_cast<const FViewInfo*> is fine because when executing this method, we know that
		// Family::Views point to multiple FViewInfo, since of them is <this>.
		const float StereoViewportWidth = float(
			static_cast<const FViewInfo*>(Family->Views[1])->ViewRect.Max.X - 
			static_cast<const FViewInfo*>(Family->Views[0])->ViewRect.Min.X);
		const float EyePaddingSize = float(
			static_cast<const FViewInfo*>(Family->Views[1])->ViewRect.Min.X -
			static_cast<const FViewInfo*>(Family->Views[0])->ViewRect.Max.X);

		ViewUniformShaderParameters.HMDEyePaddingOffset = (StereoViewportWidth - EyePaddingSize) / StereoViewportWidth;
	}
	else
	{
		ViewUniformShaderParameters.HMDEyePaddingOffset = 1.0f;
	}

	ViewUniformShaderParameters.ReflectionCubemapMaxMip = FMath::FloorLog2(UReflectionCaptureComponent::GetReflectionCaptureSize());

	ViewUniformShaderParameters.ShowDecalsMask = Family->EngineShowFlags.Decals ? 1.0f : 0.0f;

	extern int32 GDistanceFieldAOSpecularOcclusionMode;
	ViewUniformShaderParameters.DistanceFieldAOSpecularOcclusionMode = GDistanceFieldAOSpecularOcclusionMode;

	ViewUniformShaderParameters.IndirectCapsuleSelfShadowingIntensity = Scene ? Scene->DynamicIndirectShadowsSelfShadowingIntensity : 1.0f;

	extern FVector GetReflectionEnvironmentRoughnessMixingScaleBiasAndLargestWeight();
	ViewUniformShaderParameters.ReflectionEnvironmentRoughnessMixingScaleBiasAndLargestWeight = (FVector3f)GetReflectionEnvironmentRoughnessMixingScaleBiasAndLargestWeight();

	ViewUniformShaderParameters.StereoPassIndex = StereoViewIndex != INDEX_NONE ? StereoViewIndex : 0;
	ViewUniformShaderParameters.StereoIPD = StereoIPD;

	{
		auto XRCamera = GEngine->XRSystem ? GEngine->XRSystem->GetXRCamera() : nullptr;
		TArray<FVector2D> CameraUVs;
		if (XRCamera.IsValid() && XRCamera->GetPassthroughCameraUVs_RenderThread(CameraUVs) && CameraUVs.Num() == 4)
		{
			ViewUniformShaderParameters.XRPassthroughCameraUVs[0] = FVector4f(FVector2f(CameraUVs[0]), FVector2f(CameraUVs[1]));
			ViewUniformShaderParameters.XRPassthroughCameraUVs[1] = FVector4f(FVector2f(CameraUVs[2]), FVector2f(CameraUVs[3]));
		}
		else
		{
			ViewUniformShaderParameters.XRPassthroughCameraUVs[0] = FVector4f(0, 0, 0, 1);
			ViewUniformShaderParameters.XRPassthroughCameraUVs[1] = FVector4f(1, 0, 1, 1);
		}
	}

	ViewUniformShaderParameters.OverrideLandscapeLOD = -1.0f;
	if (DrawDynamicFlags & EDrawDynamicFlags::FarShadowCascade)
	{
		extern ENGINE_API int32 GFarShadowStaticMeshLODBias;
		ViewUniformShaderParameters.FarShadowStaticMeshLODBias = GFarShadowStaticMeshLODBias;
	}
	else
	{
		ViewUniformShaderParameters.FarShadowStaticMeshLODBias = 0;
	}

	ViewUniformShaderParameters.PreIntegratedBRDF = GEngine->PreIntegratedSkinBRDFTexture->GetResource()->TextureRHI;

	ViewUniformShaderParameters.GlobalVirtualTextureMipBias = FVirtualTextureSystem::Get().GetGlobalMipBias();

	const uint32 VirtualTextureFeedbackScale = GetVirtualTextureFeedbackScale();
	check(VirtualTextureFeedbackScale == 1 << FMath::FloorLog2(VirtualTextureFeedbackScale));
	ViewUniformShaderParameters.VirtualTextureFeedbackShift = FMath::FloorLog2(VirtualTextureFeedbackScale);
	ViewUniformShaderParameters.VirtualTextureFeedbackMask = VirtualTextureFeedbackScale - 1;
	ViewUniformShaderParameters.VirtualTextureFeedbackStride = GetVirtualTextureFeedbackBufferSize(SceneTexturesConfig.Extent).X;
	// Use some low(ish) discrepancy sequence to run over every pixel in the virtual texture feedback tile.
	ViewUniformShaderParameters.VirtualTextureFeedbackJitterOffset = SampleVirtualTextureFeedbackSequence(FrameIndex);
	// Offset the selected sample index for each frame and add an additional offset each time we iterate over a full virtual texture feedback tile to ensure we get full coverage of sample indices over time.
	const uint32 NumPixelsInTile = FMath::Square(VirtualTextureFeedbackScale);
	ViewUniformShaderParameters.VirtualTextureFeedbackSampleOffset = (FrameIndex % NumPixelsInTile) + (FrameIndex / NumPixelsInTile);

	ViewUniformShaderParameters.RuntimeVirtualTextureMipLevel = FVector4f(ForceInitToZero);
	ViewUniformShaderParameters.RuntimeVirtualTexturePackHeight = FVector2f(ForceInitToZero);
	ViewUniformShaderParameters.RuntimeVirtualTextureDebugParams = FVector4f(ForceInitToZero);
	
	if (UseGPUScene(GMaxRHIShaderPlatform, RHIFeatureLevel))
	{
		if (PrimitiveSceneDataOverrideSRV)
		{
			ViewUniformShaderParameters.PrimitiveSceneData = PrimitiveSceneDataOverrideSRV;
		}
		else if (Scene && Scene->GPUScene.PrimitiveBuffer.SRV != nullptr)
		{
			ViewUniformShaderParameters.PrimitiveSceneData = Scene->GPUScene.PrimitiveBuffer.SRV;
		}

		if (InstanceSceneDataOverrideSRV)
		{
			ViewUniformShaderParameters.InstanceSceneData = InstanceSceneDataOverrideSRV;
			ViewUniformShaderParameters.InstanceSceneDataSOAStride = 1;
		}
		else if (Scene && Scene->GPUScene.InstanceSceneDataBuffer.SRV)
		{
			ViewUniformShaderParameters.InstanceSceneData = Scene->GPUScene.InstanceSceneDataBuffer.SRV;
			ViewUniformShaderParameters.InstanceSceneDataSOAStride = Scene->GPUScene.InstanceSceneDataSOAStride;
		}

		if (InstancePayloadDataOverrideSRV)
		{
			ViewUniformShaderParameters.InstancePayloadData = InstancePayloadDataOverrideSRV;
		}
		else if (Scene && Scene->GPUScene.InstancePayloadDataBuffer.SRV)
		{
			ViewUniformShaderParameters.InstancePayloadData = Scene->GPUScene.InstancePayloadDataBuffer.SRV;
		}

		if (LightmapSceneDataOverrideSRV)
		{
			ViewUniformShaderParameters.LightmapSceneData = LightmapSceneDataOverrideSRV;
		}
		else if (Scene && Scene->GPUScene.LightmapDataBuffer.SRV)
		{
			ViewUniformShaderParameters.LightmapSceneData = Scene->GPUScene.LightmapDataBuffer.SRV;
		}
	}

	// Rect area light
	if (GSystemTextures.LTCMat.IsValid() && GSystemTextures.LTCAmp.IsValid())
	{
		ViewUniformShaderParameters.LTCMatTexture = GSystemTextures.LTCMat->GetRHI();
		ViewUniformShaderParameters.LTCMatSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		ViewUniformShaderParameters.LTCAmpTexture = GSystemTextures.LTCAmp->GetRHI();
		ViewUniformShaderParameters.LTCAmpSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	}
	ViewUniformShaderParameters.LTCMatTexture = OrBlack2DIfNull(ViewUniformShaderParameters.LTCMatTexture);
	ViewUniformShaderParameters.LTCAmpTexture = OrBlack2DIfNull(ViewUniformShaderParameters.LTCAmpTexture);

	// Hair global resources 
	SetUpViewHairRenderInfo(*this, ViewUniformShaderParameters.HairRenderInfo, ViewUniformShaderParameters.HairRenderInfoBits, ViewUniformShaderParameters.HairComponents);
	ViewUniformShaderParameters.HairScatteringLUTTexture = nullptr;
	if (GSystemTextures.HairLUT0.IsValid() && GSystemTextures.HairLUT0->GetRenderTargetItem().ShaderResourceTexture)
	{
		ViewUniformShaderParameters.HairScatteringLUTTexture = GSystemTextures.HairLUT0->GetRenderTargetItem().ShaderResourceTexture;
	}
	ViewUniformShaderParameters.HairScatteringLUTTexture = OrBlack3DIfNull(ViewUniformShaderParameters.HairScatteringLUTTexture);
	ViewUniformShaderParameters.HairScatteringLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// Shading energy conservation
	ViewUniformShaderParameters.bShadingEnergyConservation = 0u;
	ViewUniformShaderParameters.bShadingEnergyPreservation = 0u;
	ViewUniformShaderParameters.ShadingEnergySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	if (ViewState)
	{
		ViewUniformShaderParameters.bShadingEnergyConservation		= ViewState->ShadingEnergyConservationData.bEnergyConservation ? 1u : 0u;
		ViewUniformShaderParameters.bShadingEnergyPreservation		= ViewState->ShadingEnergyConservationData.bEnergyPreservation ? 1u : 0u;
		ViewUniformShaderParameters.ShadingEnergyGGXSpecTexture		= ViewState->ShadingEnergyConservationData.GGXSpecEnergyTexture ? ViewState->ShadingEnergyConservationData.GGXSpecEnergyTexture->GetRHI() : nullptr;
		ViewUniformShaderParameters.ShadingEnergyGGXGlassTexture	= ViewState->ShadingEnergyConservationData.GGXGlassEnergyTexture ? ViewState->ShadingEnergyConservationData.GGXGlassEnergyTexture->GetRHI() : nullptr;
		ViewUniformShaderParameters.ShadingEnergyClothSpecTexture	= ViewState->ShadingEnergyConservationData.ClothEnergyTexture ? ViewState->ShadingEnergyConservationData.ClothEnergyTexture->GetRHI() : nullptr;
		ViewUniformShaderParameters.ShadingEnergyDiffuseTexture		= ViewState->ShadingEnergyConservationData.DiffuseEnergyTexture ? ViewState->ShadingEnergyConservationData.DiffuseEnergyTexture->GetRHI() : nullptr;
	}
	ViewUniformShaderParameters.ShadingEnergyGGXSpecTexture		 = OrBlack2DIfNull(ViewUniformShaderParameters.ShadingEnergyGGXSpecTexture);
	ViewUniformShaderParameters.ShadingEnergyGGXGlassTexture	 = OrBlack3DIfNull(ViewUniformShaderParameters.ShadingEnergyGGXGlassTexture);
	ViewUniformShaderParameters.ShadingEnergyClothSpecTexture	 = OrBlack2DIfNull(ViewUniformShaderParameters.ShadingEnergyClothSpecTexture);
	ViewUniformShaderParameters.ShadingEnergyDiffuseTexture		 = OrBlack2DIfNull(ViewUniformShaderParameters.ShadingEnergyDiffuseTexture);

	// Water global resources
	if (WaterDataBuffer.IsValid() && WaterIndirectionBuffer.IsValid())
	{
		ViewUniformShaderParameters.WaterIndirection = WaterIndirectionBuffer.GetReference();
		ViewUniformShaderParameters.WaterData = WaterDataBuffer.GetReference();
	}
	else
	{
		ViewUniformShaderParameters.WaterIndirection = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
		ViewUniformShaderParameters.WaterData = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
	}

	// Landscape global resources
	if (LandscapePerComponentDataBuffer.IsValid() && LandscapeIndirectionBuffer.IsValid())
	{
		ViewUniformShaderParameters.LandscapeIndirection = LandscapeIndirectionBuffer.GetReference();
		ViewUniformShaderParameters.LandscapePerComponentData = LandscapePerComponentDataBuffer.GetReference();
	}
	else
	{
		ViewUniformShaderParameters.LandscapeIndirection = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
		ViewUniformShaderParameters.LandscapePerComponentData = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI;
	}

	ViewUniformShaderParameters.VTFeedbackBuffer = GVirtualTextureFeedbackBuffer.GetUAV();

	ViewUniformShaderParameters.GPUSceneViewId = GPUSceneViewId;

#if WITH_EDITOR
	if (EditorVisualizeLevelInstanceBuffer.SRV)
	{
		ViewUniformShaderParameters.EditorVisualizeLevelInstanceIds = EditorVisualizeLevelInstanceBuffer.SRV;
	}
	if( EditorSelectedBuffer.SRV )
	{
		ViewUniformShaderParameters.EditorSelectedHitProxyIds = EditorSelectedBuffer.SRV;
	}
#endif
}

void FViewInfo::InitRHIResources(uint32 OverrideNumMSAASamples)
{
	FBox VolumeBounds[TVC_MAX];

	check(IsInRenderingThread());

	CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();

	SetupUniformBufferParameters(
		VolumeBounds,
		TVC_MAX,
		*CachedViewUniformShaderParameters);

	if (OverrideNumMSAASamples > 0)
	{
		CachedViewUniformShaderParameters->NumSceneColorMSAASamples = OverrideNumMSAASamples;
	}

	CreateViewUniformBuffers(*CachedViewUniformShaderParameters);

	const int32 TranslucencyLightingVolumeDim = GetTranslucencyLightingVolumeDim();

	for (int32 CascadeIndex = 0; CascadeIndex < TVC_MAX; CascadeIndex++)
	{
		TranslucencyLightingVolumeMin[CascadeIndex] = VolumeBounds[CascadeIndex].Min;
		TranslucencyVolumeVoxelSize[CascadeIndex] = (VolumeBounds[CascadeIndex].Max.X - VolumeBounds[CascadeIndex].Min.X) / TranslucencyLightingVolumeDim;
		TranslucencyLightingVolumeSize[CascadeIndex] = VolumeBounds[CascadeIndex].Max - VolumeBounds[CascadeIndex].Min;
	}
}

void FViewInfo::CreateViewUniformBuffers(const FViewUniformShaderParameters& Params)
{
	ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Params, UniformBuffer_SingleFrame);
	if (bShouldBindInstancedViewUB)
	{
		if (const FViewInfo* InstancedView = GetInstancedView())
		{
			checkf(InstancedView->CachedViewUniformShaderParameters.IsValid(), TEXT("Instanced view should have had its RHI resources initialized first. Check InitViews order."));
			InstancedViewUniformBuffer = TUniformBufferRef<FInstancedViewUniformShaderParameters>::CreateUniformBufferImmediate(
				reinterpret_cast<FInstancedViewUniformShaderParameters&>(*InstancedView->CachedViewUniformShaderParameters),
				UniformBuffer_SingleFrame);
		}
		else
		{
			// If we don't render this view in stereo, we simply initialize with the existing contents.
			InstancedViewUniformBuffer = TUniformBufferRef<FInstancedViewUniformShaderParameters>::CreateUniformBufferImmediate(
				reinterpret_cast<const FInstancedViewUniformShaderParameters&>(Params),
				UniformBuffer_SingleFrame);
		}
	}
}

extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

void FViewInfo::BeginRenderView() const
{
	const bool bShouldWaitForPersistentViewUniformBufferExtensionsJobs = true;

	// Let the implementation of each extension decide whether it can cache the result for CachedView
	for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
	{
		Extension->BeginRenderView(this, bShouldWaitForPersistentViewUniformBufferExtensionsJobs);
	}
}

FViewShaderParameters FViewInfo::GetShaderParameters() const
{
	FViewShaderParameters Parameters;
	Parameters.View = ViewUniformBuffer;
	Parameters.InstancedView = InstancedViewUniformBuffer;
	// if we're a part of the stereo pair, make sure that the pointer isn't bogus
	checkf(InstancedViewUniformBuffer.IsValid() || !bShouldBindInstancedViewUB, TEXT("A view that is a part of the stereo pair has bogus state for InstancedView."));
	return Parameters;
}

const FViewInfo* FViewInfo::GetPrimaryView() const
{
	// It is valid for this function to return itself if it's already the primary view.
	if (Family && Family->Views.IsValidIndex(PrimaryViewIndex))
	{
		const FSceneView* PrimaryView = Family->Views[PrimaryViewIndex];
		check(PrimaryView->bIsViewInfo);
		return static_cast<const FViewInfo*>(PrimaryView);
	}
	return this;
}

const FViewInfo* FViewInfo::GetInstancedView() const
{
	// Extra checks are needed because some code relies on this function to return NULL if ISR is disabled.
	if (bIsInstancedStereoEnabled || bIsMobileMultiViewEnabled)
	{
		return static_cast<const FViewInfo*>(GetInstancedSceneView());
	}
	return nullptr;
}

// These are not real view infos, just dumb memory blocks
static TArray<FViewInfo*> ViewInfoSnapshots;
// these are never freed, even at program shutdown
static TArray<FViewInfo*> FreeViewInfoSnapshots;

extern TUniformBufferRef<FMobileDirectionalLightShaderParameters>& GetNullMobileDirectionalLightShaderParameters();

FViewInfo* FViewInfo::CreateSnapshot() const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FViewInfo_CreateSnapshot);

	check(IsInRenderingThread()); // we do not want this popped before the end of the scene and it better be the scene allocator
	FViewInfo* Result;
	if (FreeViewInfoSnapshots.Num())
	{
		Result = FreeViewInfoSnapshots.Pop(false);
	}
	else
	{
		Result = (FViewInfo*)FMemory::Malloc(sizeof(FViewInfo), alignof(FViewInfo));
	}
	FMemory::Memcpy(*Result, *this);

	// we want these to start null without a reference count, since we clear a ref later
	TUniformBufferRef<FViewUniformShaderParameters> NullViewUniformBuffer;
	FMemory::Memcpy(Result->ViewUniformBuffer, NullViewUniformBuffer);
	TUniformBufferRef<FInstancedViewUniformShaderParameters> NullInstancedViewUniformBuffer;
	FMemory::Memcpy(Result->InstancedViewUniformBuffer, NullInstancedViewUniformBuffer);

	TUniquePtr<FViewUniformShaderParameters> NullViewParameters;
	FMemory::Memcpy(Result->CachedViewUniformShaderParameters, NullViewParameters); 

	TArray<FPrimitiveUniformShaderParameters> NullDynamicPrimitiveShaderData;
	TStaticArray<FParallelMeshDrawCommandPass, EMeshPass::Num> NullParallelMeshDrawCommandPasses;
	FMemory::Memcpy(Result->ParallelMeshDrawCommandPasses, NullParallelMeshDrawCommandPasses);

	for (int i = 0; i < EMeshPass::Num; i++)
	{
		Result->ParallelMeshDrawCommandPasses[i].InitCreateSnapshot();
	}
	
	// Ensure the internal state is maintained, needed because we've just Memcpy'd the member data.
	static_assert(TIsTriviallyDestructible<FGPUScenePrimitiveCollector>::Value != 0, "The destructor is not invoked properly because of FMemory::Memcpy(*Result, *this) above");
	Result->DynamicPrimitiveCollector = FGPUScenePrimitiveCollector(DynamicPrimitiveCollector);

	Result->bIsSnapshot = true;
	ViewInfoSnapshots.Add(Result);
	return Result;
}

void FViewInfo::DestroyAllSnapshots(FParallelMeshDrawCommandPass::EWaitThread WaitThread)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FViewInfo_DestroyAllSnapshots);

	// we will only keep double the number actually used, plus a few
	int32 NumToRemove = FreeViewInfoSnapshots.Num() - (ViewInfoSnapshots.Num() + 2);
	if (NumToRemove > 0)
	{
		for (int32 Index = 0; Index < NumToRemove; Index++)
		{
			FMemory::Free(FreeViewInfoSnapshots[Index]);
		}
		FreeViewInfoSnapshots.RemoveAt(0, NumToRemove, false);
	}
	for (FViewInfo* Snapshot : ViewInfoSnapshots)
	{
		Snapshot->ViewUniformBuffer.SafeRelease();
		Snapshot->InstancedViewUniformBuffer.SafeRelease();
		Snapshot->CachedViewUniformShaderParameters.Reset();

		for (int32 Index = 0; Index < Snapshot->ParallelMeshDrawCommandPasses.Num(); ++Index)
		{
			Snapshot->ParallelMeshDrawCommandPasses[Index].WaitForTasksAndEmpty(WaitThread);
		}

		for (int i = 0; i < EMeshPass::Num; i++)
		{
			Snapshot->ParallelMeshDrawCommandPasses[i].FreeCreateSnapshot();
		}

		FreeViewInfoSnapshots.Add(Snapshot);
	}
	ViewInfoSnapshots.Reset();
}

FInt32Range FViewInfo::GetDynamicMeshElementRange(uint32 PrimitiveIndex) const
{
	int32 Start = 0;	// inclusive
	int32 AfterEnd = 0;	// exclusive

	// DynamicMeshEndIndices contains valid values only for visible primitives with bDynamicRelevance.
	if (PrimitiveVisibilityMap[PrimitiveIndex])
	{
		const FPrimitiveViewRelevance& ViewRelevance = PrimitiveViewRelevanceMap[PrimitiveIndex];
		if (ViewRelevance.bDynamicRelevance)
		{
			Start = (PrimitiveIndex == 0) ? 0 : DynamicMeshEndIndices[PrimitiveIndex - 1];
			AfterEnd = DynamicMeshEndIndices[PrimitiveIndex];
		}
	}

	return FInt32Range(Start, AfterEnd);
}

FSceneViewState* FViewInfo::GetEyeAdaptationViewState() const
{
	return static_cast<FSceneViewState*>(EyeAdaptationViewState);
}

IPooledRenderTarget* FViewInfo::GetEyeAdaptationTexture(FRHICommandList& RHICmdList) const
{
	checkf(FeatureLevel > ERHIFeatureLevel::ES3_1, TEXT("SM5 and above use RenderTarget for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		return EffectiveViewState->GetCurrentEyeAdaptationTexture(RHICmdList);
	}
	return nullptr;
}

void FViewInfo::SwapEyeAdaptationTextures() const
{
	checkf(FeatureLevel > ERHIFeatureLevel::ES3_1, TEXT("SM5 and above use RenderTarget for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->SwapEyeAdaptationTextures();
	}
}

void FViewInfo::UpdateEyeAdaptationLastExposureFromTexture() const
{
	checkf(FeatureLevel > ERHIFeatureLevel::ES3_1, TEXT("SM5 and above use RenderTarget for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->UpdateEyeAdaptationLastExposureFromTexture();
	}
}

void FViewInfo::EnqueueEyeAdaptationExposureTextureReadback(FRDGBuilder& GraphBuilder) const
{
	checkf(FeatureLevel > ERHIFeatureLevel::ES3_1, TEXT("SM5 and above use RenderTarget for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->EnqueueEyeAdaptationExposureTextureReadback(GraphBuilder);
	}
}

FRDGPooledBuffer* FViewInfo::GetEyeAdaptationBuffer(FRDGBuilder& GraphBuilder) const
{
	checkf(FeatureLevel == ERHIFeatureLevel::ES3_1, TEXT("ES3_1 use RWBuffer for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		return EffectiveViewState->GetCurrentEyeAdaptationBuffer(GraphBuilder);
	}
	return nullptr;
}

void FViewInfo::SwapEyeAdaptationBuffers() const
{
	checkf(FeatureLevel == ERHIFeatureLevel::ES3_1, TEXT("ES3_1 use RWBuffer for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->SwapEyeAdaptationBuffers();
	}
}

void FViewInfo::UpdateEyeAdaptationLastExposureFromBuffer() const
{
	checkf(FeatureLevel == ERHIFeatureLevel::ES3_1, TEXT("ES3_1 use RWBuffer for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->UpdateEyeAdaptationLastExposureFromBuffer();
	}
}

void FViewInfo::EnqueueEyeAdaptationExposureBufferReadback(FRDGBuilder& GraphBuilder) const
{
	checkf(FeatureLevel == ERHIFeatureLevel::ES3_1, TEXT("ES3_1 use RWBuffer for read back"));

	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->EnqueueEyeAdaptationExposureBufferReadback(GraphBuilder);
	}
}

#if WITH_MGPU
void FViewInfo::WaitForEyeAdaptationTemporalEffect(FRHICommandList& RHICmdList)
{
	FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState();

	if (EffectiveViewState)
	{
		EffectiveViewState->WaitForEyeAdaptationTemporalEffect(RHICmdList);
	}
}

void FViewInfo::BroadcastEyeAdaptationTemporalEffect(FRHICommandList& RHICmdList)
{
	FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState();

	if (EffectiveViewState)
	{
		EffectiveViewState->BroadcastEyeAdaptationTemporalEffect(RHICmdList);
	}
}
#endif // WITH_MGPU

float FViewInfo::GetLastEyeAdaptationExposure() const
{
	if (const FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		return EffectiveViewState->GetLastEyeAdaptationExposure();
	}
	return 0.0f; // Invalid exposure
}

float FViewInfo::GetLastAverageSceneLuminance() const
{
	if (const FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		return EffectiveViewState->GetLastAverageSceneLuminance();
	}
	return 0.0f; // Invalid scene luminance
}

ERenderTargetLoadAction FViewInfo::GetOverwriteLoadAction() const
{
	return bHMDHiddenAreaMaskActive ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ENoAction;
}

void FViewInfo::SetValidTonemappingLUT() const
{
	if (FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState())
	{
		EffectiveViewState->SetValidTonemappingLUT();
	}
}

IPooledRenderTarget* FViewInfo::GetTonemappingLUT() const
{
	FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState();
	if (EffectiveViewState && EffectiveViewState->HasValidTonemappingLUT())
	{
		return EffectiveViewState->GetTonemappingLUT();
	}
	return nullptr;
};

IPooledRenderTarget* FViewInfo::GetTonemappingLUT(FRHICommandList& RHICmdList, const int32 LUTSize, const bool bUseVolumeLUT, const bool bNeedUAV, const bool bNeedFloatOutput) const 
{
	FSceneViewState* EffectiveViewState = GetEyeAdaptationViewState();
	if (EffectiveViewState)
	{
		return EffectiveViewState->GetTonemappingLUT(RHICmdList, LUTSize, bUseVolumeLUT, bNeedUAV, bNeedFloatOutput);
	}
	return nullptr;
}

void FDisplayInternalsData::Setup(UWorld *World)
{
	DisplayInternalsCVarValue = 0;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	DisplayInternalsCVarValue = CVarDisplayInternals.GetValueOnGameThread();

	if(IsValid())
	{
#if WITH_AUTOMATION_TESTS
		// this variable is defined inside WITH_AUTOMATION_TESTS, 
		extern ENGINE_API uint32 GStreamAllResourcesStillInFlight;
		NumPendingStreamingRequests = GStreamAllResourcesStillInFlight;
#endif
	}
#endif
}

void FSortedShadowMaps::Release()
{
	for (int32 AtlasIndex = 0; AtlasIndex < ShadowMapAtlases.Num(); AtlasIndex++)
	{
		ShadowMapAtlases[AtlasIndex].RenderTargets.Release();
	}

	for (int32 AtlasIndex = 0; AtlasIndex < ShadowMapCubemaps.Num(); AtlasIndex++)
	{
		ShadowMapCubemaps[AtlasIndex].RenderTargets.Release();
	}

	PreshadowCache.RenderTargets.Release();
}

static bool PreparePostProcessSettingTextureForRenderer(const FViewInfo& View, UTexture2D* Texture2D, const TCHAR* TextureUsageName)
{
	check(IsInGameThread());

	bool bIsValid = Texture2D != nullptr;

	if (bIsValid)
	{
		const int32 CinematicTextureGroups = 0;
		const float Seconds = 5.0f;
		Texture2D->SetForceMipLevelsToBeResident(Seconds, CinematicTextureGroups);
	}

	const uint32 FramesPerWarning = 15;

	if (bIsValid && (Texture2D->IsFullyStreamedIn() == false || Texture2D->HasPendingInitOrStreaming()))
	{
		if ((View.Family->FrameNumber % FramesPerWarning) == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("The %s texture is not streamed in."), TextureUsageName);
		}

		bIsValid = false;
	}

	if (bIsValid && Texture2D->bHasStreamingUpdatePending == true)
	{
		if ((View.Family->FrameNumber % FramesPerWarning) == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("The %s texture has pending update."), TextureUsageName);
		}

		bIsValid = false;
	}

#if WITH_EDITOR
	if (bIsValid && Texture2D->IsDefaultTexture())
#else
	if (bIsValid && (!Texture2D->GetResource() || Texture2D->GetResource()->IsProxy()))
#endif
	{
		if ((View.Family->FrameNumber % FramesPerWarning) == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("The %s texture is still using the default texture proxy."), TextureUsageName);
		}

		bIsValid = false;
	}

	return bIsValid;
};

template <typename T>
inline T* CheckPointer(T* Ptr)
{
	check(Ptr != nullptr);
	return Ptr;
}
/*-----------------------------------------------------------------------------
	FSceneRenderer
-----------------------------------------------------------------------------*/

FSceneRenderer::FSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
:	Scene(CheckPointer(InViewFamily->Scene)->GetRenderScene())
,	ViewFamily(*CheckPointer(InViewFamily))
,	MeshCollector(InViewFamily->GetFeatureLevel())
,	RayTracingCollector(InViewFamily->GetFeatureLevel())
,	bHasRequestedToggleFreeze(false)
,	bUsedPrecomputedVisibility(false)
,	InstancedStereoWidth(0)
,	FamilySize(0, 0)
,	GPUSceneDynamicContext(CheckPointer(Scene)->GPUScene)
,	bShadowDepthRenderCompleted(false)
{
	check(Scene != NULL);

	check(IsInGameThread());
	ViewFamily.FrameNumber = Scene->GetFrameNumber();

	// Copy the individual views.
	bool bAnyViewIsLocked = false;
	Views.Empty(InViewFamily->Views.Num());
	for(int32 ViewIndex = 0;ViewIndex < InViewFamily->Views.Num();ViewIndex++)
	{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		for(int32 ViewIndex2 = 0;ViewIndex2 < InViewFamily->Views.Num();ViewIndex2++)
		{
			if (ViewIndex != ViewIndex2 && InViewFamily->Views[ViewIndex]->State != NULL)
			{
				// Verify that each view has a unique view state, as the occlusion query mechanism depends on it.
				check(InViewFamily->Views[ViewIndex]->State != InViewFamily->Views[ViewIndex2]->State);
			}
		}
#endif
		// Construct a FViewInfo with the FSceneView properties.
		FViewInfo* ViewInfo = new(Views) FViewInfo(InViewFamily->Views[ViewIndex]);
		ViewFamily.Views[ViewIndex] = ViewInfo;
		ViewInfo->Family = &ViewFamily;
		bAnyViewIsLocked |= ViewInfo->bIsLocked;

		// Must initialize to have a GPUScene connected to be able to collect dynamic primitives.
		ViewInfo->DynamicPrimitiveCollector = FGPUScenePrimitiveCollector(&GPUSceneDynamicContext);

		check(ViewInfo->ViewRect.Area() == 0);

#if WITH_EDITOR
		// Should we allow the user to select translucent primitives?
		ViewInfo->bAllowTranslucentPrimitivesInHitProxy =
			GEngine->AllowSelectTranslucent() ||		// User preference enabled?
			!ViewInfo->IsPerspectiveProjection();		// Is orthographic view?
#endif

		// Batch the view's elements for later rendering.
		if(ViewInfo->Drawer)
		{
			FViewElementPDI ViewElementPDI(ViewInfo,HitProxyConsumer,&ViewInfo->DynamicPrimitiveCollector);
			ViewInfo->Drawer->Draw(ViewInfo,&ViewElementPDI);
		}

		#if !UE_BUILD_SHIPPING
		if (CVarTestCameraCut.GetValueOnGameThread())
		{
			ViewInfo->bCameraCut = true;
		}
		#endif

		if (ShouldRenderLumenDiffuseGI(Scene, *ViewInfo) || ShouldRenderLumenReflections(*ViewInfo))
		{
			GEngine->LoadBlueNoiseTexture();
		}

		// Handle the FFT bloom kernel textire
		if (ViewInfo->FinalPostProcessSettings.BloomMethod == EBloomMethod::BM_FFT && ViewInfo->ViewState != nullptr)
		{
			UTexture2D* BloomConvolutionTexture = ViewInfo->FinalPostProcessSettings.BloomConvolutionTexture;
			if (BloomConvolutionTexture == nullptr)
			{
				GEngine->LoadDefaultBloomTexture();

				BloomConvolutionTexture = GEngine->DefaultBloomKernelTexture;
			}

			bool bIsValid = PreparePostProcessSettingTextureForRenderer(*ViewInfo, BloomConvolutionTexture, TEXT("convolution bloom"));

			if (bIsValid)
			{
				const FTextureResource* TextureResource = BloomConvolutionTexture->GetResource();
				if (TextureResource)
				{
					ViewInfo->FFTBloomKernelTexture = TextureResource->GetTexture2DResource();
					ViewInfo->FinalPostProcessSettings.BloomConvolutionTexture = BloomConvolutionTexture;
				}
				else
				{
					ViewInfo->FinalPostProcessSettings.BloomConvolutionTexture = nullptr;
				}
			}
		}

		// Handle the film grain texture
		if (ViewInfo->FinalPostProcessSettings.FilmGrainIntensity > 0.0f &&
			ViewFamily.EngineShowFlags.Grain &&
			CVarFilmGrain.GetValueOnGameThread() != 0 &&
			SupportsFilmGrain(ViewFamily.GetShaderPlatform()))
		{
			UTexture2D* FilmGrainTexture = ViewInfo->FinalPostProcessSettings.FilmGrainTexture;
			if (FilmGrainTexture == nullptr)
			{
				GEngine->LoadDefaultFilmGrainTexture();
				FilmGrainTexture = GEngine->DefaultFilmGrainTexture;
			}

			bool bIsValid = PreparePostProcessSettingTextureForRenderer(*ViewInfo, FilmGrainTexture, TEXT("film grain"));
			 
			if (bIsValid)
			{
				const FTextureResource* TextureResource = FilmGrainTexture->GetResource();
				if (TextureResource)
				{
					ViewInfo->FilmGrainTexture = TextureResource->GetTexture2DResource();
				}
			}
		}
	}

	// Catches inconsistency one engine show flags for screen percentage and whether it is supported or not.
	ensureMsgf(!(ViewFamily.EngineShowFlags.ScreenPercentage && !ViewFamily.SupportsScreenPercentage()),
		TEXT("Screen percentage is not supported, but show flag was incorectly set to true."));

	// Fork the plugin interfaces of the view family.
	{
		{
			check(InViewFamily->ScreenPercentageInterface);
			ViewFamily.ScreenPercentageInterface = nullptr;
			ViewFamily.SetScreenPercentageInterface(InViewFamily->ScreenPercentageInterface->Fork_GameThread(ViewFamily));
		}

		if (ViewFamily.PrimarySpatialUpscalerInterface)
		{
			ViewFamily.PrimarySpatialUpscalerInterface = nullptr;
			ViewFamily.SetPrimarySpatialUpscalerInterface(InViewFamily->PrimarySpatialUpscalerInterface->Fork_GameThread(ViewFamily));
		}

		if (ViewFamily.SecondarySpatialUpscalerInterface)
		{
			ViewFamily.SecondarySpatialUpscalerInterface = nullptr;
			ViewFamily.SetSecondarySpatialUpscalerInterface(InViewFamily->SecondarySpatialUpscalerInterface->Fork_GameThread(ViewFamily));
		}
	}

	#if !UE_BUILD_SHIPPING
	// Override screen percentage interface.
	if (int32 OverrideId = CVarTestScreenPercentageInterface.GetValueOnGameThread())
	{
		check(ViewFamily.ScreenPercentageInterface);

		// Replaces screen percentage interface with dynamic resolution hell's driver.
		if (OverrideId == 1 && ViewFamily.Views[0]->State)
		{
			delete ViewFamily.ScreenPercentageInterface;
			ViewFamily.ScreenPercentageInterface = nullptr;
			ViewFamily.EngineShowFlags.ScreenPercentage = true;
			ViewFamily.SetScreenPercentageInterface(new FScreenPercentageHellDriver(ViewFamily));
		}
	}

	// Override secondary screen percentage for testing purpose.
	if (CVarTestSecondaryUpscaleOverride.GetValueOnGameThread() > 0 && !Views[0].bIsReflectionCapture)
	{
		ViewFamily.SecondaryViewFraction = 1.0 / float(CVarTestSecondaryUpscaleOverride.GetValueOnGameThread());
		ViewFamily.SecondaryScreenPercentageMethod = ESecondaryScreenPercentageMethod::NearestSpatialUpscale;
	}
	#endif

	// If any viewpoint has been locked, set time to zero to avoid time-based
	// rendering differences in materials.
	if (bAnyViewIsLocked)
	{
		ViewFamily.Time = FGameTime::CreateDilated(0.0f, ViewFamily.Time.GetDeltaRealTimeSeconds(), 0.0f, ViewFamily.Time.GetDeltaWorldTimeSeconds());
	}
	
	if(HitProxyConsumer)
	{
		// Set the hit proxies show flag.
		ViewFamily.EngineShowFlags.SetHitProxies(1);
	}

	// launch custom visibility queries for views
	if (GCustomCullingImpl)
	{
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			FViewInfo& ViewInfo = Views[ViewIndex];
			ViewInfo.CustomVisibilityQuery = GCustomCullingImpl->CreateQuery(ViewInfo);
		}
	}

	// copy off the requests
	if(ensure(InViewFamily->RenderTarget))
	{
		// (I apologize for the const_cast, but didn't seem worth refactoring just for the freezerendering command)
		bHasRequestedToggleFreeze = const_cast<FRenderTarget*>(InViewFamily->RenderTarget)->HasToggleFreezeCommand();
	}

	FeatureLevel = Scene->GetFeatureLevel();
	ShaderPlatform = Scene->GetShaderPlatform();

	bDumpMeshDrawCommandInstancingStats = !!GDumpInstancingStats;
	GDumpInstancingStats = 0;
}

// static
FIntPoint FSceneRenderer::ApplyResolutionFraction(const FSceneViewFamily& ViewFamily, const FIntPoint& UnscaledViewSize, float ResolutionFraction)
{
	FIntPoint ViewSize;

	// CeilToInt so tha view size is at least 1x1 if ResolutionFraction == ISceneViewFamilyScreenPercentage::kMinResolutionFraction.
	ViewSize.X = FMath::CeilToInt(UnscaledViewSize.X * ResolutionFraction);
	ViewSize.Y = FMath::CeilToInt(UnscaledViewSize.Y * ResolutionFraction);

	check(ViewSize.GetMin() > 0);

	return ViewSize;
}

// static
FIntPoint FSceneRenderer::QuantizeViewRectMin(const FIntPoint& ViewRectMin)
{
	FIntPoint Out;
	QuantizeSceneBufferSize(ViewRectMin, Out);
	return Out;
}

// static
FIntPoint FSceneRenderer::GetDesiredInternalBufferSize(const FSceneViewFamily& ViewFamily)
{
	// If not supporting screen percentage, bypass all computation.
	if (!ViewFamily.SupportsScreenPercentage())
	{
		FIntPoint FamilySizeUpperBound(0, 0);

		for (const FSceneView* View : ViewFamily.Views)
		{
			FamilySizeUpperBound.X = FMath::Max(FamilySizeUpperBound.X, View->UnscaledViewRect.Max.X);
			FamilySizeUpperBound.Y = FMath::Max(FamilySizeUpperBound.Y, View->UnscaledViewRect.Max.Y);
		}

		FIntPoint DesiredBufferSize;
		QuantizeSceneBufferSize(FamilySizeUpperBound, DesiredBufferSize);
		return DesiredBufferSize;
	}

	float PrimaryResolutionFractionUpperBound = ViewFamily.GetPrimaryResolutionFractionUpperBound();

	// Compute final resolution fraction.
	float ResolutionFractionUpperBound = PrimaryResolutionFractionUpperBound * ViewFamily.SecondaryViewFraction;

	FIntPoint FamilySizeUpperBound(0, 0);

	for (const FSceneView* View : ViewFamily.Views)
	{
		FIntPoint ViewSize = ApplyResolutionFraction(ViewFamily, View->UnconstrainedViewRect.Size(), ResolutionFractionUpperBound);
		FIntPoint ViewRectMin = QuantizeViewRectMin(FIntPoint(
			FMath::CeilToInt(View->UnconstrainedViewRect.Min.X * ResolutionFractionUpperBound),
			FMath::CeilToInt(View->UnconstrainedViewRect.Min.Y * ResolutionFractionUpperBound)));

		FamilySizeUpperBound.X = FMath::Max(FamilySizeUpperBound.X, ViewRectMin.X + ViewSize.X);
		FamilySizeUpperBound.Y = FMath::Max(FamilySizeUpperBound.Y, ViewRectMin.Y + ViewSize.Y);
	}

	check(FamilySizeUpperBound.GetMin() > 0);

	FIntPoint DesiredBufferSize;
	QuantizeSceneBufferSize(FamilySizeUpperBound, DesiredBufferSize);

#if !UE_BUILD_SHIPPING
	{
		// Increase the size of desired buffer size by 2 when testing for view rectangle offset.
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Test.ViewRectOffset"));
		if (CVar->GetValueOnAnyThread() > 0)
		{
			DesiredBufferSize *= 2;
		}
	}
#endif

	return DesiredBufferSize;
}


void FSceneRenderer::PrepareViewRectsForRendering(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());

	// If not supporting screen percentage, bypass all computation.
	if (!ViewFamily.SupportsScreenPercentage())
	{
		// The base pass have to respect FSceneView::UnscaledViewRect.
		for (FViewInfo& View : Views)
		{
			View.ViewRect = View.UnscaledViewRect;
		}

		ComputeFamilySize();
		
		// Notify StereoRenderingDevice about new ViewRects
		if (GEngine->StereoRenderingDevice.IsValid())
		{
			for (int32 i = 0; i < Views.Num(); i++)
			{
				FViewInfo& View = Views[i];
				GEngine->StereoRenderingDevice->SetFinalViewRect(RHICmdList, View.StereoViewIndex, View.ViewRect);
			}
		}
		return;
	}

	// Checks that view rects were still not initialized.
	for (FViewInfo& View : Views)
	{
		// Make sure there was no attempt to configure ViewRect and screen percentage method before.
		check(View.ViewRect.Area() == 0);

		// Fallback to no anti aliasing.
		{
			const bool bWillApplyTemporalAA = (IsPostProcessingEnabled(View) || View.bIsPlanarReflection)
#if RHI_RAYTRACING
				// path tracer does its own anti-aliasing
				&& (!ViewFamily.EngineShowFlags.PathTracing)
#endif
			;

			if (!bWillApplyTemporalAA)
			{
				// Disable anti-aliasing if we are not going to be able to apply final post process effects
				View.AntiAliasingMethod = AAM_None;
			}
		}
	}

	check(ViewFamily.ScreenPercentageInterface);
	float PrimaryResolutionFraction = ViewFamily.ScreenPercentageInterface->GetPrimaryResolutionFraction_RenderThread();
	{
		// Ensure screen percentage show flag is respected. Prefer to check() rather rendering at a differen screen percentage
		// to make sure the renderer does not lie how a frame as been rendering to a dynamic resolution heuristic.
		if (!ViewFamily.EngineShowFlags.ScreenPercentage)
		{
			checkf(PrimaryResolutionFraction == 1.0f, TEXT("It is illegal to set ResolutionFraction != 1 if screen percentage show flag is disabled."));
		}

		// Make sure the screen percentage interface has not lied to the renderer about the upper bound.
		checkf(PrimaryResolutionFraction <= ViewFamily.GetPrimaryResolutionFractionUpperBound(),
			TEXT("ISceneViewFamilyScreenPercentage::GetPrimaryResolutionFractionUpperBound() should not lie to the renderer."));

		check(ISceneViewFamilyScreenPercentage::IsValidResolutionFraction(PrimaryResolutionFraction));
	}

	// Compute final resolution fraction.
	float ResolutionFraction = PrimaryResolutionFraction * ViewFamily.SecondaryViewFraction;

	// Checks that view rects are correctly initialized.
	for (int32 i = 0; i < Views.Num(); i++)
	{
		FViewInfo& View = Views[i];

		FIntPoint ViewSize = ApplyResolutionFraction(ViewFamily, View.UnscaledViewRect.Size(), ResolutionFraction);
		FIntPoint ViewRectMin = QuantizeViewRectMin(FIntPoint(
			FMath::CeilToInt(View.UnscaledViewRect.Min.X * ResolutionFraction),
			FMath::CeilToInt(View.UnscaledViewRect.Min.Y * ResolutionFraction)));

		// Use the bottom-left view rect if requested, instead of top-left
		if (CVarViewRectUseScreenBottom.GetValueOnRenderThread())
		{
			ViewRectMin.Y = FMath::CeilToInt( View.UnscaledViewRect.Max.Y * ViewFamily.SecondaryViewFraction ) - ViewSize.Y;
		}

		View.ViewRect.Min = ViewRectMin;
		View.ViewRect.Max = ViewRectMin + ViewSize;

		#if !UE_BUILD_SHIPPING
		// For testing purpose, override the screen percentage method.
		{
			switch (CVarTestPrimaryScreenPercentageMethodOverride.GetValueOnRenderThread())
			{
			case 1: View.PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale; break;
			case 2: View.PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::TemporalUpscale; break;
			case 3: View.PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::RawOutput; break;
			}
		}
		#endif

		// Automatic screen percentage fallback.
		{
			// Tenmporal upsample is supported only if TAA is turned on.
			if (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale &&
				(!IsTemporalAccumulationBasedMethod(View.AntiAliasingMethod) ||
				 ViewFamily.EngineShowFlags.VisualizeBuffer))
			{
				View.PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::SpatialUpscale;
			}
		}

		check(View.ViewRect.Area() != 0);
		check(View.VerifyMembersChecks());
	}

	// Shifts all view rects layout to the top left corner of the buffers, since post processing will just output the final
	// views in FSceneViewFamily::RenderTarget whereever it was requested with FSceneView::UnscaledViewRect.
	{
		FIntPoint TopLeftShift = Views[0].ViewRect.Min;
		for (int32 i = 1; i < Views.Num(); i++)
		{
			TopLeftShift.X = FMath::Min(TopLeftShift.X, Views[i].ViewRect.Min.X);
			TopLeftShift.Y = FMath::Min(TopLeftShift.Y, Views[i].ViewRect.Min.Y);
		}
		for (int32 i = 0; i < Views.Num(); i++)
		{
			Views[i].ViewRect -= TopLeftShift;
		}
	}

	#if !UE_BUILD_SHIPPING
	{
		int32 ViewRectOffset = CVarTestInternalViewRectOffset.GetValueOnRenderThread();

		if (Views.Num() == 1 && ViewRectOffset > 0)
		{
			FViewInfo& View = Views[0];

			FIntPoint DesiredBufferSize = GetDesiredInternalBufferSize(ViewFamily);
			FIntPoint Offset = (DesiredBufferSize - View.ViewRect.Size()) / 2;
			FIntPoint NewViewRectMin(0, 0);

			switch (ViewRectOffset)
			{
			// Move to the center of the buffer.
			case 1: NewViewRectMin = Offset; break;

			// Move to top left.
			case 2: break;

			// Move to top right.
			case 3: NewViewRectMin = FIntPoint(2 * Offset.X, 0); break;

			// Move to bottom right.
			case 4: NewViewRectMin = FIntPoint(0, 2 * Offset.Y); break;

			// Move to bottom left.
			case 5: NewViewRectMin = FIntPoint(2 * Offset.X, 2 * Offset.Y); break;
			}

			View.ViewRect += QuantizeViewRectMin(NewViewRectMin) - View.ViewRect.Min;

			check(View.VerifyMembersChecks());
		}
	}
	#endif

	ComputeFamilySize();

	// Notify StereoRenderingDevice about new ViewRects
	if (GEngine->StereoRenderingDevice.IsValid())
	{
		for (int32 i = 0; i < Views.Num(); i++)
		{
			FViewInfo& View = Views[i];
			GEngine->StereoRenderingDevice->SetFinalViewRect(RHICmdList, View.StereoViewIndex, View.ViewRect);
		}
	}
}

#if WITH_MGPU
FRHIGPUMask FSceneRenderer::ComputeGPUMasks(FRHICommandListImmediate& RHICmdList)
{
	FRHIGPUMask RenderTargetGPUMask = FRHIGPUMask::GPU0();
	
	if (GNumExplicitGPUsForRendering > 1 && ViewFamily.RenderTarget)
	{
		RenderTargetGPUMask = ViewFamily.RenderTarget->GetGPUMask(RHICmdList);
	}

	{
		static auto CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.GPUCount"));
		if (CVar && CVar->GetInt() > 1)
		{
			RenderTargetGPUMask = FRHIGPUMask::All(); // Broadcast to all GPUs 
		}
	}

	// First check whether we are in multi-GPU and if fork and join cross-gpu transfers are enabled.
	// Otherwise fallback on rendering the whole view family on each relevant GPU using broadcast logic.
	if (GNumExplicitGPUsForRendering > 1 && CVarEnableMultiGPUForkAndJoin.GetValueOnAnyThread() != 0)
	{
		// Check whether this looks like an AFR setup (note that the logic also applies when there is only one AFR group).
		// Each AFR group uses multiple GPU. AFRGroup(i) = { i, NumAFRGroups + i,  2 * NumAFRGroups + i, ... } up to NumGPUs.
		// Each view rendered gets assigned to the next GPU in that group. 
		const FRHIGPUMask UsableGPUMask = AFRUtils::GetGPUMaskForGroup(RenderTargetGPUMask);

		// Start iterating from RenderTargetGPUMask and then wrap around. This avoids an
		// unnecessary cross-gpu transfer in cases where you only have 1 view and the
		// render target is located on a GPU other than GPU 0.
		FRHIGPUMask::FIterator GPUIterator(FRHIGPUMask::FilterGPUsBefore(RenderTargetGPUMask.GetFirstIndex()) & UsableGPUMask);
		for (FViewInfo& ViewInfo : Views)
		{
			// Only handle views that are to be rendered (this excludes instance stereo).
			if (ViewInfo.ShouldRenderView())
			{
				// Multi-GPU support : This is inefficient for AFR if the reflection capture
				// updates every frame. Work is wasted on the GPUs that are not involved in
				// rendering the current frame.
				if (ViewInfo.bIsReflectionCapture)
				{
					ViewInfo.GPUMask = FRHIGPUMask::All();
				}
				else
				{
					if (!ViewInfo.bOverrideGPUMask)
					{
						ViewInfo.GPUMask = FRHIGPUMask::FromIndex(*GPUIterator);
					}

					ViewFamily.bMultiGPUForkAndJoin |= (ViewInfo.GPUMask != RenderTargetGPUMask);

					// Increment and wrap around if we reach the last index.
					++GPUIterator;
					if (!GPUIterator)
					{
						GPUIterator = FRHIGPUMask::FIterator(UsableGPUMask);
					}
				}
			}
		}
	}
	else
	{
		for (FViewInfo& ViewInfo : Views)
		{
			if (ViewInfo.ShouldRenderView())
			{
				ViewInfo.GPUMask = RenderTargetGPUMask;
			}
		}
	}

	AllViewsGPUMask = Views[0].GPUMask;
	for (int32 ViewIndex = 1; ViewIndex < Views.Num(); ++ViewIndex)
	{
		AllViewsGPUMask |= Views[ViewIndex].GPUMask;
	}

	return RenderTargetGPUMask;
}
#endif // WITH_MGPU

#if WITH_MGPU
DECLARE_GPU_STAT_NAMED(CrossGPUTransfers, TEXT("Cross GPU Transfer"));
#endif // WITH_MGPU

void FSceneRenderer::DoCrossGPUTransfers(FRDGBuilder& GraphBuilder, FRHIGPUMask RenderTargetGPUMask, FRDGTextureRef ViewFamilyTexture)
{
#if WITH_MGPU
	if (ViewFamily.bMultiGPUForkAndJoin)
	{
		// Must be all GPUs because context redirector only supports single or all GPUs
		RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());
		RDG_GPU_STAT_SCOPE(GraphBuilder, CrossGPUTransfers);

		// A readback pass is the closest analog to what this is doing. There isn't a way to express cross-GPU transfers via the RHI barrier API.
		AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("CrossGPUTransfers"), ViewFamilyTexture, 
			[this, RenderTargetGPUMask, ViewFamilyTexture] (FRHICommandListImmediate& RHICmdList)
		{
			const FIntPoint Extent = ViewFamilyTexture->Desc.Extent;

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				const FViewInfo& ViewInfo = Views[ViewIndex];
				if (ViewInfo.bAllowCrossGPUTransfer && ViewInfo.GPUMask != RenderTargetGPUMask)
				{
					// Clamp the view rect by the rendertarget rect to prevent issues when resizing the viewport.
					const FIntRect TransferRect(ViewInfo.UnscaledViewRect.Min.ComponentMin(Extent), ViewInfo.UnscaledViewRect.Max.ComponentMin(Extent));
					if (TransferRect.Width() > 0 && TransferRect.Height() > 0)
					{
						for (uint32 RenderTargetGPUIndex : RenderTargetGPUMask)
						{
							if (!ViewInfo.GPUMask.Contains(RenderTargetGPUIndex))
							{
								FTransferResourceParams Param(static_cast<FRHITexture2D*>(ViewFamilyTexture->GetRHI()), TransferRect, ViewInfo.GPUMask.GetFirstIndex(), RenderTargetGPUIndex, false, false);
								RHICmdList.TransferResources(TArrayView<const FTransferResourceParams>(&Param, 1));
							}
						}
					}
				}
			}
		});
	}
#endif
}


void FSceneRenderer::ComputeFamilySize()
{
	check(FamilySize.X == 0);
	check(IsInRenderingThread());

	// Calculate the screen extents of the view family.
	bool bInitializedExtents = false;
	float MaxFamilyX = 0;
	float MaxFamilyY = 0;

	for (FViewInfo& View : Views)
	{
		float FinalViewMaxX = (float)View.ViewRect.Max.X;
		float FinalViewMaxY = (float)View.ViewRect.Max.Y;

		// Derive the amount of scaling needed for screenpercentage from the scaled / unscaled rect
		const float XScale = FinalViewMaxX / (float)View.UnscaledViewRect.Max.X;
		const float YScale = FinalViewMaxY / (float)View.UnscaledViewRect.Max.Y;

		if (!bInitializedExtents)
		{
			// Note: using the unconstrained view rect to compute family size
			// In the case of constrained views (black bars) this means the scene render targets will fill the whole screen
			// Which is needed for mobile paths where we render directly to the backbuffer, and the scene depth buffer has to match in size
			MaxFamilyX = View.UnconstrainedViewRect.Max.X * XScale;
			MaxFamilyY = View.UnconstrainedViewRect.Max.Y * YScale;
			bInitializedExtents = true;
		}
		else
		{
			MaxFamilyX = FMath::Max(MaxFamilyX, View.UnconstrainedViewRect.Max.X * XScale);
			MaxFamilyY = FMath::Max(MaxFamilyY, View.UnconstrainedViewRect.Max.Y * YScale);
		}

		// floating point imprecision could cause MaxFamilyX to be less than View->ViewRect.Max.X after integer truncation.
		// since this value controls rendertarget sizes, we don't want to create rendertargets smaller than the view size.
		MaxFamilyX = FMath::Max(MaxFamilyX, FinalViewMaxX);
		MaxFamilyY = FMath::Max(MaxFamilyY, FinalViewMaxY);

		const FViewInfo* InstancedView = View.GetInstancedView();
		View.InstancedStereoWidth = InstancedView ? InstancedView->ViewRect.Max.X : View.ViewRect.Max.X;
	}

	// We render to the actual position of the viewports so with black borders we need the max.
	// We could change it by rendering all to left top but that has implications for splitscreen. 
	FamilySize.X = FMath::TruncToInt(MaxFamilyX);
	FamilySize.Y = FMath::TruncToInt(MaxFamilyY);

	check(FamilySize.X != 0);
	check(bInitializedExtents);
}

bool FSceneRenderer::DoOcclusionQueries() const
{
	return CVarAllowOcclusionQueries.GetValueOnRenderThread() != 0;
}

FSceneRenderer::~FSceneRenderer()
{
	for (FProjectedShadowInfo* ProjectedShadow : MemStackProjectedShadows)
	{
		// FProjectedShadowInfo's in MemStackProjectedShadows were allocated on the rendering thread mem stack, 
		// Their memory will be freed when the stack is freed with no destructor call, so invoke the destructor explicitly
		ProjectedShadow->~FProjectedShadowInfo();
	}

	// Manually release references to TRefCountPtrs that are allocated on the mem stack, which doesn't call dtors
	SortedShadowsForShadowDepthPass.Release();

	Views.Empty();
}


FSceneRenderer::FScreenMessageWriter::FScreenMessageWriter(FCanvas& InCanvas, int32 InY)
	: Canvas(InCanvas)
	, Y(InY)
{
}

void FSceneRenderer::FScreenMessageWriter::DrawLine(const FText& Message, int32 X, const FLinearColor& Color)
{
	Canvas.DrawShadowedText(X, Y, Message, GetStatsFont(), Color);
	Y += 14;
}

/** 
* Finishes the view family rendering.
*/
void FSceneRenderer::RenderFinish(FRDGBuilder& GraphBuilder, FRDGTextureRef ViewFamilyTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "RenderFinish");

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	{
		bool bShowPrecomputedVisibilityWarning = false;
		static const auto* CVarPrecomputedVisibilityWarning = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.PrecomputedVisibilityWarning"));
		if (CVarPrecomputedVisibilityWarning && CVarPrecomputedVisibilityWarning->GetValueOnRenderThread() == 1)
		{
			bShowPrecomputedVisibilityWarning = !bUsedPrecomputedVisibility;
		}

		bool bShowDemotedLocalMemoryWarning = false;
		static const auto* CVarDemotedLocalMemoryWarning = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DemotedLocalMemoryWarning"));
		if (CVarDemotedLocalMemoryWarning && CVarDemotedLocalMemoryWarning->GetValueOnRenderThread() == 1)
		{
			bShowDemotedLocalMemoryWarning = GDemotedLocalMemorySize > 0;
		}

		bool bShowGlobalClipPlaneWarning = false;

		if (Scene->PlanarReflections.Num() > 0)
		{
			static const auto* CVarClipPlane = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.AllowGlobalClipPlane"));
			
			const bool bShouldUseClipPlaneForPlanarReflection = (FeatureLevel > ERHIFeatureLevel::ES3_1 && GetMobilePlanarReflectionMode() != EMobilePlanarReflectionMode::MobilePPRExclusive)
															|| GetMobilePlanarReflectionMode() == EMobilePlanarReflectionMode::Usual;
			
			if (CVarClipPlane && CVarClipPlane->GetValueOnRenderThread() == 0
				&& bShouldUseClipPlaneForPlanarReflection)
			{
				bShowGlobalClipPlaneWarning = true;
			}
		}
		
		const FReadOnlyCVARCache& ReadOnlyCVARCache = Scene->ReadOnlyCVARCache;
		static auto* CVarSkinCacheOOM = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.SkinCache.SceneMemoryLimitInMB"));

		uint64 GPUSkinCacheExtraRequiredMemory = 0;
		extern ENGINE_API bool IsGPUSkinCacheAvailable(EShaderPlatform Platform);
		if (IsGPUSkinCacheAvailable(ShaderPlatform))
		{
			if (FGPUSkinCache* SkinCache = Scene->GetGPUSkinCache())
			{
				GPUSkinCacheExtraRequiredMemory = SkinCache->GetExtraRequiredMemoryAndReset();
			}
		}
		const bool bShowSkinCacheOOM = CVarSkinCacheOOM != nullptr && GPUSkinCacheExtraRequiredMemory > 0;

		const bool bMeshDistanceFieldEnabled = DoesProjectSupportDistanceFields();
		extern bool UseDistanceFieldAO();
		const bool bShowDFAODisabledWarning = !UseDistanceFieldAO() && (ViewFamily.EngineShowFlags.VisualizeDistanceFieldAO);
		const bool bShowDFDisabledWarning = !bMeshDistanceFieldEnabled && (ViewFamily.EngineShowFlags.VisualizeMeshDistanceFields || ViewFamily.EngineShowFlags.VisualizeGlobalDistanceField || ViewFamily.EngineShowFlags.VisualizeDistanceFieldAO);

		const bool bShowNoSkyAtmosphereComponentWarning = !Scene->HasSkyAtmosphere() && ViewFamily.EngineShowFlags.VisualizeSkyAtmosphere;

		const bool bStationarySkylight = Scene->SkyLight && Scene->SkyLight->bWantsStaticShadowing;
		const bool bShowSkylightWarning = bStationarySkylight && !ReadOnlyCVARCache.bEnableStationarySkylight;
		const bool bRealTimeSkyCaptureButNothingToCapture = Scene->SkyLight && Scene->SkyLight->bRealTimeCaptureEnabled && (!Scene->HasSkyAtmosphere() && !Scene->HasVolumetricCloud() && (Views.Num() > 0 && !Views[0].bSceneHasSkyMaterial));

		const bool bShowPointLightWarning = UsedWholeScenePointLightNames.Num() > 0 && !ReadOnlyCVARCache.bEnablePointLightShadows;
		const bool bShowShadowedLightOverflowWarning = Scene->OverflowingDynamicShadowedLights.Num() > 0;

		bool bLumenEnabledButHasNoDataForTracing = false;
		bool bLumenEnabledButDisabledForTheProject = false;
		bool bNaniteEnabledButDisabledInProject = false;

		bool bLocalExposureEnabledOnAnyView = false;

		for (int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			FViewInfo& View = Views[ViewIndex];
			bLumenEnabledButHasNoDataForTracing = bLumenEnabledButHasNoDataForTracing
				|| (!ShouldRenderLumenDiffuseGI(Scene, View) && ShouldRenderLumenDiffuseGI(Scene, View, /*bSkipTracingDataCheck*/ true))
				|| (!ShouldRenderLumenReflections(View) && ShouldRenderLumenReflections(View, /*bSkipTracingDataCheck*/ true));

			bLumenEnabledButDisabledForTheProject = bLumenEnabledButDisabledForTheProject
				|| (!ShouldRenderLumenDiffuseGI(Scene, View) && ShouldRenderLumenDiffuseGI(Scene, View, /*bSkipTracingDataCheck*/ false, /*bSkipProjectCheck*/ true))
				|| (!ShouldRenderLumenReflections(View) && ShouldRenderLumenReflections(View, /*bSkipTracingDataCheck*/ false, /*bSkipProjectCheck*/ true));

			bNaniteEnabledButDisabledInProject = bNaniteEnabledButDisabledInProject || (WouldRenderNanite(Scene, View, /*bCheckForAtomicSupport*/ false, /*bCheckForProjectSetting*/ false) && !WouldRenderNanite(Scene, View, /*bCheckForAtomicSupport*/ false, /*bCheckForProjectSetting*/ true));

			if (IsPostProcessingEnabled(View) && (!FMath::IsNearlyEqual(View.FinalPostProcessSettings.LocalExposureContrastScale, 1.0f) || !FMath::IsNearlyEqual(View.FinalPostProcessSettings.LocalExposureDetailStrength, 1.0f)))
				bLocalExposureEnabledOnAnyView = true;
		}

		const bool bShowLocalExposureDisabledWarning = ViewFamily.EngineShowFlags.VisualizeLocalExposure && !bLocalExposureEnabledOnAnyView;

		const int32 NaniteShowError = CVarNaniteShowUnsupportedError.GetValueOnRenderThread();
		// 0: disabled
		// 1: show error if Nanite is present in the scene but unsupported, and fallback meshes are not used for rendering
		// 2: show error if Nanite is present in the scene but unsupported, even if fallback meshes are used for rendering

		static const auto NaniteProxyRenderModeVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.ProxyRenderMode"));
		const int32 NaniteProxyRenderMode = (NaniteProxyRenderModeVar != nullptr) ? (NaniteProxyRenderModeVar->GetInt() != 0) : 0;
		// 0: Fall back to rendering Nanite proxy meshes if Nanite is unsupported.
		// 1: Disable rendering if Nanite is enabled on a mesh but is unsupported
		// 2: Disable rendering if Nanite is enabled on a mesh but is unsupported, except for static mesh editor toggle

		bool bNaniteEnabledButNoAtomics = false;

		bool bNaniteCheckError = (NaniteShowError == 1 && NaniteProxyRenderMode != 0) || (NaniteShowError == 2);
		if (bNaniteCheckError && !NaniteAtomicsSupported())
		{
			// We want to know when Nanite would've been rendered regardless of atomics being supported or not.
			const bool bCheckForAtomicSupport = false;

			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				FViewInfo& View = Views[ViewIndex];
				bNaniteEnabledButNoAtomics |= ShouldRenderNanite(Scene, View, bCheckForAtomicSupport);
			}
		}

		// Mobile-specific warnings
		const bool bMobile = (FeatureLevel <= ERHIFeatureLevel::ES3_1);
		const bool bShowMobileLowQualityLightmapWarning = bMobile && !ReadOnlyCVARCache.bEnableLowQualityLightmaps && ReadOnlyCVARCache.bAllowStaticLighting;
		const bool bShowMobileDynamicCSMWarning = bMobile && Scene->NumMobileStaticAndCSMLights_RenderThread > 0 && !(ReadOnlyCVARCache.bMobileEnableStaticAndCSMShadowReceivers && ReadOnlyCVARCache.bMobileAllowDistanceFieldShadows);
		const bool bShowMobileMovableDirectionalLightWarning = bMobile && Scene->NumMobileMovableDirectionalLights_RenderThread > 0 && !ReadOnlyCVARCache.bMobileAllowMovableDirectionalLights;
		const bool bMobileMissingSkyMaterial = (bMobile && Scene->HasSkyAtmosphere() && (Views.Num() > 0 && !Views[0].bSceneHasSkyMaterial));

		const bool bSingleLayerWaterWarning = ShouldRenderSingleLayerWaterSkippedRenderEditorNotification(Views);

		bool bShowWaitingSkylight = false;
#if WITH_EDITOR
		FSkyLightSceneProxy* SkyLight = Scene->SkyLight;
		if (SkyLight && !SkyLight->bRealTimeCaptureEnabled)
		{
			bShowWaitingSkylight = SkyLight->bCubemapSkyLightWaitingForCubeMapTexture || SkyLight->bCaptureSkyLightWaitingForShaders || SkyLight->bCaptureSkyLightWaitingForMeshesOrTextures;
		}
#endif

		FFXSystemInterface* FXInterface = Scene->GetFXSystem();
		const bool bFxDebugDraw = FXInterface && FXInterface->ShouldDebugDraw_RenderThread();

		const bool bHasDelegateWarnings = OnGetOnScreenMessages.IsBound();

		const bool bAnyWarning = bShowPrecomputedVisibilityWarning || bShowDemotedLocalMemoryWarning || bShowGlobalClipPlaneWarning || bShowSkylightWarning || bShowPointLightWarning
			|| bShowDFAODisabledWarning || bShowShadowedLightOverflowWarning || bShowMobileDynamicCSMWarning || bShowMobileLowQualityLightmapWarning || bShowMobileMovableDirectionalLightWarning
			|| bMobileMissingSkyMaterial || bShowSkinCacheOOM || bSingleLayerWaterWarning || bShowDFDisabledWarning || bShowNoSkyAtmosphereComponentWarning || bFxDebugDraw 
			|| bLumenEnabledButHasNoDataForTracing || bLumenEnabledButDisabledForTheProject || bNaniteEnabledButNoAtomics || bNaniteEnabledButDisabledInProject || bRealTimeSkyCaptureButNothingToCapture || bShowWaitingSkylight
			|| bShowLocalExposureDisabledWarning || bHasDelegateWarnings;

		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			FViewInfo& View = Views[ViewIndex];
			if (!View.bIsReflectionCapture && !View.bIsSceneCapture )
			{
				const FScreenPassRenderTarget Output(ViewFamilyTexture, View.UnconstrainedViewRect, ERenderTargetLoadAction::ELoad);

				// display a message saying we're frozen
				FSceneViewState* ViewState = (FSceneViewState*)View.State;
				bool bViewParentOrFrozen = ViewState && (ViewState->HasViewParent() || ViewState->bIsFrozen);
				bool bLocked = View.bIsLocked;

				// display a warning if an ambient cubemap uses non-angular mipmap filtering
				bool bShowAmbientCubemapMipGenSettingsWarning = false;

#if WITH_EDITORONLY_DATA
				for (FFinalPostProcessSettings::FCubemapEntry ContributingCubemap : View.FinalPostProcessSettings.ContributingCubemaps)
				{
					// platform configuration can't be loaded from the rendering thread, therefore the warning wont be displayed for TMGS_FromTextureGroup settings
					if (ContributingCubemap.AmbientCubemap &&
						ContributingCubemap.AmbientCubemap->MipGenSettings != TMGS_FromTextureGroup &&
						ContributingCubemap.AmbientCubemap->MipGenSettings != TMGS_Angular)
					{
						bShowAmbientCubemapMipGenSettingsWarning = true;
						break;
					}
				}
#endif
				if ((GAreScreenMessagesEnabled && !GEngine->bSuppressMapWarnings) && (bViewParentOrFrozen || bLocked || bShowAmbientCubemapMipGenSettingsWarning || bAnyWarning))
				{
					RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

					AddDrawCanvasPass(GraphBuilder, {}, View, Output,
						[this, &ReadOnlyCVARCache, ViewState, GPUSkinCacheExtraRequiredMemory,
						bLocked, bShowPrecomputedVisibilityWarning, bShowDemotedLocalMemoryWarning, bShowGlobalClipPlaneWarning, bShowDFAODisabledWarning, bShowDFDisabledWarning,
						bViewParentOrFrozen, bShowSkylightWarning, bShowPointLightWarning, bShowShadowedLightOverflowWarning,
						bShowMobileLowQualityLightmapWarning, bShowMobileMovableDirectionalLightWarning, bShowMobileDynamicCSMWarning, bMobileMissingSkyMaterial, 
						bShowSkinCacheOOM, bSingleLayerWaterWarning, bShowNoSkyAtmosphereComponentWarning, bFxDebugDraw, FXInterface, bShowLocalExposureDisabledWarning,
						bLumenEnabledButHasNoDataForTracing, bLumenEnabledButDisabledForTheProject, bNaniteEnabledButNoAtomics, bNaniteEnabledButDisabledInProject, bRealTimeSkyCaptureButNothingToCapture, bShowWaitingSkylight, bShowAmbientCubemapMipGenSettingsWarning]
						(FCanvas& Canvas)
					{
						// so it can get the screen size
						FScreenMessageWriter Writer(Canvas, 130);

						// Make sure draws to the canvas are not rendered upside down.
						Canvas.SetAllowSwitchVerticalAxis(true);
						if (bViewParentOrFrozen)
						{
							const FText StateText =
								ViewState->bIsFrozen ?
								NSLOCTEXT("SceneRendering", "RenderingFrozen", "Rendering frozen...")
								:
								NSLOCTEXT("SceneRendering", "OcclusionChild", "Occlusion Child");
							Writer.DrawLine(StateText, 10, FLinearColor(0.8, 1.0, 0.2, 1.0));
						}
						if (bShowPrecomputedVisibilityWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "NoPrecomputedVisibility", "NO PRECOMPUTED VISIBILITY");
							Writer.DrawLine(Message);
						}
						if (bShowGlobalClipPlaneWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "NoGlobalClipPlane", "PLANAR REFLECTION REQUIRES GLOBAL CLIP PLANE PROJECT SETTING ENABLED TO WORK PROPERLY");
							Writer.DrawLine(Message);
						}
						if (bShowDFAODisabledWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "DFAODisabled", "Distance Field AO is disabled through scalability");
							Writer.DrawLine(Message);
						}
						if (bShowDFDisabledWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "DFDisabled", "Mesh distance fields generation is disabled by project settings, cannot visualize DFAO, mesh or global distance field.");
							Writer.DrawLine(Message);
						}

						if (bShowNoSkyAtmosphereComponentWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "SkyAtmosphere", "There is no SkyAtmosphere component to visualize.");
							Writer.DrawLine(Message);
						}
						if (bShowSkylightWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "SkylightNotSuppported", "PROJECT DOES NOT SUPPORT STATIONARY SKYLIGHT: ");
							Writer.DrawLine(Message);
						}
						if (bRealTimeSkyCaptureButNothingToCapture)
						{
							static const FText Message = NSLOCTEXT("Renderer", "Skylight", "A sky light with real-time capture enable is in the scene. It requires at least a SkyAtmosphere component, A volumetricCloud component or a mesh with a material tagged as IsSky. Otherwise it will be black.");
							Writer.DrawLine(Message);
						}
						if (bShowPointLightWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "PointLight", "PROJECT DOES NOT SUPPORT WHOLE SCENE POINT LIGHT SHADOWS: ");
							Writer.DrawLine(Message);
							for (const FString& LightName : UsedWholeScenePointLightNames)
							{
								Writer.DrawLine(FText::FromString(LightName), 35);
							}
						}
						if (bShowShadowedLightOverflowWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "ShadowedLightOverflow", "TOO MANY OVERLAPPING SHADOWED MOVABLE LIGHTS, SHADOW CASTING DISABLED: ");
							Writer.DrawLine(Message);

							for (const FString& LightName : Scene->OverflowingDynamicShadowedLights)
							{
								Writer.DrawLine(FText::FromString(LightName));
							}
						}
						if (bShowMobileLowQualityLightmapWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "MobileLQLightmap", "MOBILE PROJECTS SUPPORTING STATIC LIGHTING MUST HAVE LQ LIGHTMAPS ENABLED");
							Writer.DrawLine(Message);
						}
						if (bShowMobileMovableDirectionalLightWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "MobileMovableDirectional", "PROJECT HAS MOVABLE DIRECTIONAL LIGHTS ON MOBILE DISABLED");
							Writer.DrawLine(Message);
						}
						if (bShowMobileDynamicCSMWarning)
						{
							static const FText Message = (!ReadOnlyCVARCache.bMobileEnableStaticAndCSMShadowReceivers)
								? NSLOCTEXT("Renderer", "MobileDynamicCSM", "PROJECT HAS MOBILE CSM SHADOWS FROM STATIONARY DIRECTIONAL LIGHTS DISABLED")
								: NSLOCTEXT("Renderer", "MobileDynamicCSMDistFieldShadows", "MOBILE CSM+STATIC REQUIRES DISTANCE FIELD SHADOWS ENABLED FOR PROJECT");
							Writer.DrawLine(Message);
						}

						if (bMobileMissingSkyMaterial)
						{
							static const FText Message = NSLOCTEXT("Renderer", "MobileMissingSkyMaterial", "On mobile the SkyAtmosphere component needs a mesh with a material tagged as IsSky and using the SkyAtmosphere nodes to visualize the Atmosphere.");
							Writer.DrawLine(Message);
						}

						if (bShowSkinCacheOOM)
						{
							FString String = FString::Printf(TEXT("OUT OF MEMORY FOR SKIN CACHE, REQUIRES %.3f extra MB (currently at %.3f)"), (float)GPUSkinCacheExtraRequiredMemory / 1048576.0f, CVarSkinCacheOOM->GetValueOnAnyThread());
							Writer.DrawLine(FText::FromString(String));
						}
						if (bShowLocalExposureDisabledWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "LocalExposureDisabled", "Local Exposure is disabled.");
							Writer.DrawLine(Message);
						}

						if (bLocked)
						{
							static const FText Message = NSLOCTEXT("Renderer", "ViewLocked", "VIEW LOCKED");
							Writer.DrawLine(Message, 10, FLinearColor(0.8, 1.0, 0.2, 1.0));
						}

						if (bSingleLayerWaterWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "SingleLayerWater", "r.Water.SingleLayer rendering is disabled with a view containing mesh(es) using water material. Meshes are not visible.");
							Writer.DrawLine(Message);
						}

						if (bLumenEnabledButHasNoDataForTracing)
						{
							static const FText Message = NSLOCTEXT("Renderer", "LumenCantDisplay", "Lumen is enabled, but has no ray tracing data and won't operate correctly.\nEither configure Lumen to use software distance field ray tracing and enable 'Generate Mesh Distancefields' in project settings\nor configure Lumen to use Hardware Ray Tracing and enable 'Support Hardware Ray Tracing' in project settings.");
							Writer.DrawLine(Message);
						}

						if (bLumenEnabledButDisabledForTheProject)
						{
							static const FText Message = NSLOCTEXT("Renderer", "LumenDisabledForProject", "Lumen is enabled but cannot render, because the project has Lumen disabled in an ini (r.Lumen.Supported = 0)");
							Writer.DrawLine(Message);
						}

						if (bNaniteEnabledButNoAtomics)
						{
							FString NaniteError = TEXT("Nanite is used in the scene but not supported by your graphics hardware and/or driver. Meshes will not render using Nanite.");
							Writer.DrawLine(FText::FromString(NaniteError));
						}

						if (bNaniteEnabledButDisabledInProject)
						{
							static const FText Message = NSLOCTEXT("Renderer", "NaniteDisabledForProject", "Nanite is enabled but cannot render, because the project has Nanite disabled in an ini (r.Nanite.ProjectEnabled = 0)");
							Writer.DrawLine(Message);
						}

						if (bShowDemotedLocalMemoryWarning)
						{
							FString String = FString::Printf(TEXT("Video memory has been exhausted (%.3f MB over budget). Expect extremely poor performance."), float(GDemotedLocalMemorySize) / 1048576.0f);
							Writer.DrawLine(FText::FromString(String));
						}

						if (bShowAmbientCubemapMipGenSettingsWarning)
						{
							static const FText Message = NSLOCTEXT("Renderer", "AmbientCubemapMipGenSettings", "Ambient cubemaps should use 'Angular' Mip Gen Settings.");
							Writer.DrawLine(Message);
						}

#if WITH_EDITOR
						FSkyLightSceneProxy* SkyLight = Scene->SkyLight;
						if (bShowWaitingSkylight && SkyLight)
						{
							const FLinearColor OrangeColor = FColor::Orange;

							FString String = TEXT("Sky Light waiting on ");
							bool bAddComma = false;
							if (SkyLight->bCubemapSkyLightWaitingForCubeMapTexture)
							{
								String += TEXT("CubeMap");
								bAddComma = true;
							}
							if (SkyLight->bCaptureSkyLightWaitingForShaders)
							{
								String += bAddComma ? TEXT(", ") : TEXT("");
								String += TEXT("Shaders");
								bAddComma = true;
							}
							if (SkyLight->bCaptureSkyLightWaitingForMeshesOrTextures)
							{
								String += bAddComma ? TEXT(", ") : TEXT("");
								String += TEXT("Meshes, Textures");
							}
							String += TEXT(" for final capture.");
							Writer.DrawLine(FText::FromString(String), 10, OrangeColor);
						}
#endif
						OnGetOnScreenMessages.Broadcast(Writer);
					});
					if (bFxDebugDraw)
					{
						FXInterface->DrawDebug_RenderThread(GraphBuilder, View, Output);
					}
				}
			}
		}
	}
	
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	// Save the post-occlusion visibility stats for the frame and freezing info
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		INC_DWORD_STAT_BY(STAT_VisibleStaticMeshElements, View.NumVisibleStaticMeshElements);
		INC_DWORD_STAT_BY(STAT_VisibleDynamicPrimitives, View.NumVisibleDynamicPrimitives);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		// update freezing info
		FSceneViewState* ViewState = (FSceneViewState*)View.State;
		if (ViewState)
		{
			// if we're finished freezing, now we are frozen
			if (ViewState->bIsFreezing)
			{
				ViewState->bIsFreezing = false;
				ViewState->bIsFrozen = true;
				ViewState->bIsFrozenViewMatricesCached = true;
				ViewState->CachedViewMatrices = View.ViewMatrices;
			}

			// handle freeze toggle request
			if (bHasRequestedToggleFreeze)
			{
				// do we want to start freezing or stop?
				ViewState->bIsFreezing = !ViewState->bIsFrozen;
				ViewState->bIsFrozen = false;
				ViewState->bIsFrozenViewMatricesCached = false;
				ViewState->FrozenPrimitives.Empty();
			}
		}
#endif
	}

#if SUPPORTS_VISUALIZE_TEXTURE
	// clear the commands
	bHasRequestedToggleFreeze = false;

	if(ViewFamily.EngineShowFlags.OnScreenDebug && ViewFamilyTexture)
	{
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];

			if(!View.IsPerspectiveProjection())
			{
				continue;
			}

			const FScreenPassRenderTarget Output(ViewFamilyTexture, View.UnconstrainedViewRect, ERenderTargetLoadAction::ELoad);

			FVisualizeTexturePresent::PresentContent(GraphBuilder, View, Output);
		}
	}
#endif //SUPPORTS_VISUALIZE_TEXTURE

	{
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_ViewExtensionPostRenderView);
		for(int32 ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ++ViewExt)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "ViewFamilyExtension(%d)", ViewExt);
			ISceneViewExtension& ViewExtension = *ViewFamily.ViewExtensions[ViewExt];
			ViewExtension.PostRenderViewFamily_RenderThread(GraphBuilder, ViewFamily);

			for(int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
			{
				RDG_EVENT_SCOPE(GraphBuilder, "ViewExtension(%d)", ViewIndex);
				ViewExtension.PostRenderView_RenderThread(GraphBuilder, Views[ViewIndex]);
			}
		}
	}

	AddPass(GraphBuilder, RDG_EVENT_NAME("EndScene"), [this](FRHICommandListImmediate& InRHICmdList)
	{
		// Notify the RHI we are done rendering a scene.
		InRHICmdList.EndScene();

		if (GDumpMeshDrawCommandMemoryStats)
		{
			GDumpMeshDrawCommandMemoryStats = 0;
			Scene->DumpMeshDrawCommandMemoryStats();
		}
	});
}

void FSceneRenderer::SetupMeshPass(FViewInfo& View, FExclusiveDepthStencil::Type BasePassDepthStencilAccess, FViewCommands& ViewCommands, FInstanceCullingManager& InstanceCullingManager)
{
	SCOPE_CYCLE_COUNTER(STAT_SetupMeshPass);

	const EShadingPath ShadingPath = Scene->GetShadingPath();

	for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
	{
		const EMeshPass::Type PassType = (EMeshPass::Type)PassIndex;

		if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::MainView) != EMeshPassFlags::None)
		{
			// Mobile: BasePass and MobileBasePassCSM lists need to be merged and sorted after shadow pass.
			if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM))
			{
				continue;
			}

			if (ViewFamily.UseDebugViewPS() && ShadingPath == EShadingPath::Deferred)
			{
				switch (PassType)
				{
					case EMeshPass::DepthPass:
					case EMeshPass::CustomDepth:
					case EMeshPass::DebugViewMode:
#if WITH_EDITOR
					case EMeshPass::HitProxy:
					case EMeshPass::HitProxyOpaqueOnly:
					case EMeshPass::EditorSelection:
					case EMeshPass::EditorLevelInstance:
#endif
						break;
					default:
						continue;
				}
			}

			PassProcessorCreateFunction CreateFunction = FPassProcessorManager::GetCreateFunction(ShadingPath, PassType);
			FMeshPassProcessor* MeshPassProcessor = CreateFunction(Scene, &View, nullptr);

			FParallelMeshDrawCommandPass& Pass = View.ParallelMeshDrawCommandPasses[PassIndex];

			if (ShouldDumpMeshDrawCommandInstancingStats())
			{
				Pass.SetDumpInstancingStats(GetMeshPassName(PassType));
			}

			TArray<int32, TInlineAllocator<2> > ViewIds;
			ViewIds.Add(View.GPUSceneViewId);
			// Only apply instancing for ISR to main view passes
			const bool bIsMainViewPass = PassType != EMeshPass::Num && (FPassProcessorManager::GetPassFlags(Scene->GetShadingPath(), PassType) & EMeshPassFlags::MainView) != EMeshPassFlags::None;

			EInstanceCullingMode InstanceCullingMode = bIsMainViewPass && View.IsInstancedStereoPass() ? EInstanceCullingMode::Stereo : EInstanceCullingMode::Normal;
			if (InstanceCullingMode == EInstanceCullingMode::Stereo)
			{
				check(View.GetInstancedView() != nullptr);
				ViewIds.Add(View.GetInstancedView()->GPUSceneViewId);
			}
			
			EInstanceCullingFlags CullingFlags = EInstanceCullingFlags::None;
			if (ViewFamily.EngineShowFlags.DrawOnlyVSMInvalidatingGeo != 0)
			{
				EnumAddFlags(CullingFlags, EInstanceCullingFlags::DrawOnlyVSMInvalidatingGeometry);
			}

			Pass.DispatchPassSetup(
				Scene,
				View,
				FInstanceCullingContext(FeatureLevel, &InstanceCullingManager, ViewIds, View.PrevViewInfo.HZB, InstanceCullingMode, CullingFlags),
				PassType,
				BasePassDepthStencilAccess,
				MeshPassProcessor,
				View.DynamicMeshElements,
				&View.DynamicMeshElementsPassRelevance,
				View.NumVisibleDynamicMeshElements[PassType],
				ViewCommands.DynamicMeshCommandBuildRequests[PassType],
				ViewCommands.NumDynamicMeshCommandBuildRequestElements[PassType],
				ViewCommands.MeshCommands[PassIndex]);
		}
	}
}

#if WITH_MGPU
TArray<FVector> GMultiViewFamilyOrigins;
#endif

// Unpublished Renderer function to provide an array of view family origins, used to make Lumen LOD calculations multi-view-family aware.
// Temporary hack fix for Virtual Production project using Lumen UE 5.0 -- in 5.1, scene rendering will be natively multi-view-family aware.
void RENDERER_API SetMultiViewFamilyOrigins(const TArray<FVector>& ViewOrigins)
{
#if WITH_MGPU
	GMultiViewFamilyOrigins = ViewOrigins;
#endif
}

FSceneRenderer* FSceneRenderer::CreateSceneRenderer(const FSceneViewFamily* InViewFamily, FHitProxyConsumer* HitProxyConsumer)
{
	EShadingPath ShadingPath = InViewFamily->Scene->GetShadingPath();
	FSceneRenderer* SceneRenderer = nullptr;

	if (ShadingPath == EShadingPath::Deferred)
	{
		SceneRenderer = new FDeferredShadingSceneRenderer(InViewFamily, HitProxyConsumer);

#if WITH_MGPU
		SceneRenderer->MultiViewFamilyOrigins = GMultiViewFamilyOrigins;
		GMultiViewFamilyOrigins.Empty();
#endif  // WITH_MGPU
	}
	else 
	{
		check(ShadingPath == EShadingPath::Mobile);
		SceneRenderer = new FMobileSceneRenderer(InViewFamily, HitProxyConsumer);
	}

	return SceneRenderer;
}

void FSceneRenderer::OnStartRender(FRHICommandListImmediate& RHICmdList)
{
	FVisualizeTexturePresent::OnStartRender(Views[0]);
}

bool FSceneRenderer::ShouldCompositeEditorPrimitives(const FViewInfo& View)
{
	if (View.Family->EngineShowFlags.VisualizeHDR || View.Family->EngineShowFlags.VisualizeStrataMaterial || View.Family->UseDebugViewPS())
	{
		// certain visualize modes get obstructed too much
		return false;
	}

	if (View.Family->EngineShowFlags.Wireframe)
	{
		// We want wireframe view use MSAA if possible.
		return true;
	}
	else if (View.Family->EngineShowFlags.CompositeEditorPrimitives)
	{
	    // Any elements that needed compositing were drawn then compositing should be done
	    if (View.ViewMeshElements.Num() 
		    || View.TopViewMeshElements.Num() 
		    || View.BatchedViewElements.HasPrimsToDraw() 
		    || View.TopBatchedViewElements.HasPrimsToDraw() 
		    || View.NumVisibleDynamicEditorPrimitives > 0
			|| IsMobileColorsRGB())
	    {
		    return true;
	    }
	}

	return false;
}

void FSceneRenderer::UpdatePrimitiveIndirectLightingCacheBuffers()
{
	// Use a bit array to prevent primitives from being updated more than once.
	FSceneBitArray UpdatedPrimitiveMap;
	UpdatedPrimitiveMap.Init(false, Scene->Primitives.Num());

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{		
		FViewInfo& View = Views[ViewIndex];

		for (int32 Index = 0; Index < View.DirtyIndirectLightingCacheBufferPrimitives.Num(); ++Index)
		{
			FPrimitiveSceneInfo* PrimitiveSceneInfo = View.DirtyIndirectLightingCacheBufferPrimitives[Index];

			FBitReference bInserted = UpdatedPrimitiveMap[PrimitiveSceneInfo->GetIndex()];
			if (!bInserted)
			{
				PrimitiveSceneInfo->UpdateIndirectLightingCacheBuffer();
				bInserted = true;
			}
			else
			{
				// This will prevent clearing it twice.
				View.DirtyIndirectLightingCacheBufferPrimitives[Index] = nullptr;
			}
		}
	}

	const uint32 CurrentSceneFrameNumber = Scene->GetFrameNumber();

	// Trim old CPUInterpolationCache entries occasionally
	if (CurrentSceneFrameNumber % 10 == 0)
	{
		for (TMap<FVector, FVolumetricLightmapInterpolation>::TIterator It(Scene->VolumetricLightmapSceneData.CPUInterpolationCache); It; ++It)
		{
			FVolumetricLightmapInterpolation& Interpolation = It.Value();

			if (Interpolation.LastUsedSceneFrameNumber < CurrentSceneFrameNumber - 100)
			{
				It.RemoveCurrent();
			}
		}
	}
}

/*-----------------------------------------------------------------------------
	FRendererModule
-----------------------------------------------------------------------------*/

/**
* Helper function performing actual work in render thread.
*
* @param SceneRenderer	Scene renderer to use for rendering.
*/
void FSceneRenderer::ViewExtensionPreRender_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer)
{
	if (SceneRenderer->ViewFamily.ViewExtensions.IsEmpty())
	{
		return;
	}

	FMemMark MemStackMark(FMemStack::Get());

	{
		FRDGBuilder GraphBuilder(RHICmdList);
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(PreRender);
		SCOPE_CYCLE_COUNTER(STAT_FDeferredShadingSceneRenderer_ViewExtensionPreRenderView);

		for (int ViewExt = 0; ViewExt < SceneRenderer->ViewFamily.ViewExtensions.Num(); ViewExt++)
		{
			SceneRenderer->ViewFamily.ViewExtensions[ViewExt]->PreRenderViewFamily_RenderThread(GraphBuilder, SceneRenderer->ViewFamily);
			for (int ViewIndex = 0; ViewIndex < SceneRenderer->ViewFamily.Views.Num(); ViewIndex++)
			{
				SceneRenderer->ViewFamily.ViewExtensions[ViewExt]->PreRenderView_RenderThread(GraphBuilder, SceneRenderer->Views[ViewIndex]);
			}
		}

		GraphBuilder.Execute();
	}
	
	// update any resources that needed a deferred update
	FDeferredUpdateResource::UpdateResources(RHICmdList);
}

static int32 GSceneRenderCleanUpMode = 2;
static FAutoConsoleVariableRef CVarSceneRenderCleanUpMode(
	TEXT("r.SceneRenderCleanUpMode"),
	GSceneRenderCleanUpMode,
	TEXT("Controls when to perform clean up of the scene renderer.\n")
	TEXT(" 0: clean up is performed immediately after render on the render thread.\n")
	TEXT(" 1: clean up deferred until the start of the next scene render on the render thread.\n")
	TEXT(" 2: clean up deferred until the start of the next scene render on the render thread, with some work distributed to an async task. (default)\n"),
	ECVF_RenderThreadSafe
);

enum class ESceneRenderCleanUpMode
	{
	Immediate,
	Deferred,
	DeferredAndAsync
};

inline ESceneRenderCleanUpMode GetSceneRenderCleanUpMode()
{
	if (GSceneRenderCleanUpMode != 1 && GSceneRenderCleanUpMode != 2)
	{
		return ESceneRenderCleanUpMode::Immediate;
	}

	static const IConsoleVariable* AsyncDispatch = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RHICmdAsyncRHIThreadDispatch"));

	if (!AsyncDispatch->GetInt() || !IsRunningRHIInSeparateThread())
	{
		return ESceneRenderCleanUpMode::Immediate;
	}

	return (ESceneRenderCleanUpMode)GSceneRenderCleanUpMode;
}

static void DeleteSceneRenderer(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer, FMemMark* MemStackMark)
{
	static const IConsoleVariable* AsyncDispatch = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RHICmdAsyncRHIThreadDispatch"));

	if (AsyncDispatch->GetInt() == 0)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_DeleteSceneRenderer_Dispatch);
		RHICmdList.ImmediateFlush(EImmediateFlushType::WaitForDispatchToRHIThread); // we want to make sure this all gets to the rhi thread this frame and doesn't hang around
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(DeleteSceneRenderer);
	delete SceneRenderer;
	delete MemStackMark;

	// Can release only after all mesh pass tasks are finished.
	GPrimitiveIdVertexBufferPool.DiscardAll();
	FGraphicsMinimalPipelineStateId::ResetLocalPipelineIdTableSize();
}

static void ReleaseSceneRenderer(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer)
{
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_DeleteSceneRenderer_WaitForTasks);
		RHICmdList.ImmediateFlush(EImmediateFlushType::WaitForOutstandingTasksOnly);
	}

	SceneRenderer->WaitForTasksAndClearSnapshots(FParallelMeshDrawCommandPass::EWaitThread::Render);
}

void FSceneRenderer::RenderThreadBegin(FRHICommandListImmediate& RHICmdList)
{
	CleanUp(RHICmdList);

	// Cache the FXSystem for the duration of the scene render
	// UWorld::CleanupWorldInternal() will mark the system as pending kill on the GameThread and then enqueue a delete command
	//-TODO: The call to IsPendingKill should no longer be required as we are caching & using within a single render command
	FXSystem = Scene ? Scene->FXSystem : nullptr;
	if (FXSystem && FXSystem->IsPendingKill())
	{
		FXSystem = nullptr;
	}

	MemStackMark = new FMemMark(FMemStack::Get());
}

struct FSceneRenderCleanUpState
{
	FSceneRenderer* Renderer{};
	FMemMark* MemStackMark{};
	FGraphEventRef Task;
	ESceneRenderCleanUpMode CompletionMode = ESceneRenderCleanUpMode::Immediate;
	bool bWaitForTasksComplete = false;
};

static FSceneRenderCleanUpState GSceneRenderCleanUpState;

void FSceneRenderer::RenderThreadEnd(FRHICommandListImmediate& RHICmdList)
{
	check(!GSceneRenderCleanUpState.Renderer);

	const ESceneRenderCleanUpMode SceneRenderCleanUpMode = GetSceneRenderCleanUpMode();

	GSceneRenderCleanUpState.CompletionMode = SceneRenderCleanUpMode;

	if (GSceneRenderCleanUpState.CompletionMode == ESceneRenderCleanUpMode::Immediate)
	{
		ReleaseSceneRenderer(RHICmdList, this);
		DeleteSceneRenderer(RHICmdList, this, MemStackMark);
	}
	else
	{
		GPUSceneDynamicContext.Release();

		GSceneRenderCleanUpState.Renderer = this;
		GSceneRenderCleanUpState.MemStackMark = MemStackMark;

		if (SceneRenderCleanUpMode == ESceneRenderCleanUpMode::DeferredAndAsync)
		{
			// Wait on all setup tasks now to ensure that no additional render commands are enqueued which
			// might mess with render state, since setup tasks are working with high-level render objects.
			{
				FGraphEventArray SetupTasks;

				for (FParallelMeshDrawCommandPass* DispatchedShadowDepthPass : DispatchedShadowDepthPasses)
				{
					if (DispatchedShadowDepthPass->GetTaskEvent())
					{
						SetupTasks.Add(DispatchedShadowDepthPass->GetTaskEvent());
					}
				}

				for (const FViewInfo& View : Views)
				{
					for (const FParallelMeshDrawCommandPass& Pass : View.ParallelMeshDrawCommandPasses)
					{
						if (Pass.GetTaskEvent())
						{
							SetupTasks.Add(Pass.GetTaskEvent());
						}
					}
				}

				if (!SetupTasks.IsEmpty())
				{
					FTaskGraphInterface::Get().WaitUntilTasksComplete(SetupTasks, ENamedThreads::GetRenderThread_Local());
				}
			}

			FGraphEventArray CommandListTasks = MoveTemp(RHICmdList.GetRenderThreadTaskArray());

			GSceneRenderCleanUpState.Task = FFunctionGraphTask::CreateAndDispatchWhenReady([this]
			{
				WaitForTasksAndClearSnapshots(FParallelMeshDrawCommandPass::EWaitThread::TaskAlreadyWaited);

			}, TStatId(), &CommandListTasks);
		}
	}
}

void FSceneRenderer::CleanUp(FRHICommandListImmediate& RHICmdList)
{
	if (GSceneRenderCleanUpState.CompletionMode == ESceneRenderCleanUpMode::Immediate || !GSceneRenderCleanUpState.Renderer)
	{
		return;
	}

	if (!GSceneRenderCleanUpState.bWaitForTasksComplete)
	{
		switch (GSceneRenderCleanUpState.CompletionMode)
		{
		case ESceneRenderCleanUpMode::Deferred:
			ReleaseSceneRenderer(RHICmdList, GSceneRenderCleanUpState.Renderer);
			break;
		case ESceneRenderCleanUpMode::DeferredAndAsync:
			GSceneRenderCleanUpState.Task->Wait(ENamedThreads::GetRenderThread_Local());
			break;
		}
	}

	DeleteSceneRenderer(RHICmdList, GSceneRenderCleanUpState.Renderer, GSceneRenderCleanUpState.MemStackMark);
	GSceneRenderCleanUpState = {};
}

void FSceneRenderer::WaitForCleanUpTasks(FRHICommandListImmediate& RHICmdList)
{
	if (GSceneRenderCleanUpState.CompletionMode == ESceneRenderCleanUpMode::Immediate || !GSceneRenderCleanUpState.Renderer || GSceneRenderCleanUpState.bWaitForTasksComplete)
	{
		return;
	}

	switch (GSceneRenderCleanUpState.CompletionMode)
	{
	case ESceneRenderCleanUpMode::Deferred:
		ReleaseSceneRenderer(RHICmdList, GSceneRenderCleanUpState.Renderer);
		break;
	case ESceneRenderCleanUpMode::DeferredAndAsync:
		GSceneRenderCleanUpState.Task->Wait(ENamedThreads::GetRenderThread_Local());
		GSceneRenderCleanUpState.Task = nullptr;
		break;
	}

	GSceneRenderCleanUpState.bWaitForTasksComplete = true;
}

void FSceneRenderer::WaitForTasksAndClearSnapshots(FParallelMeshDrawCommandPass::EWaitThread WaitThread)
{
	SCOPED_NAMED_EVENT_TEXT("FSceneRenderer::WaitForTasksAndClearSnapshots", FColor::Red);

	// Wait for all dispatched shadow mesh draw tasks.
	for (int32 PassIndex = 0; PassIndex < DispatchedShadowDepthPasses.Num(); ++PassIndex)
	{
		DispatchedShadowDepthPasses[PassIndex]->WaitForTasksAndEmpty(WaitThread);
	}

	for (FViewInfo& View : Views)
	{
		View.WaitForTasks(WaitThread);
	}

	FViewInfo::DestroyAllSnapshots(WaitThread);
}

void ResetAndShrinkModifiedBounds(TArray<FRenderBounds>& Bounds)
{
	const int32 MaxAllocatedSize = FMath::RoundUpToPowerOfTwo(FMath::Max<uint32>(DistanceField::MinPrimitiveModifiedBoundsAllocation, Bounds.Num()));

	if (Bounds.Max() > MaxAllocatedSize)
	{
		Bounds.Empty(MaxAllocatedSize);
	}

	Bounds.Reset();
}

/**
 * Helper function performing actual work in render thread.
 *
 * @param SceneRenderer	Scene renderer to use for rendering.
 */
static void RenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneRenderer* SceneRenderer)
{
	LLM_SCOPE(ELLMTag::SceneRender);

	// We need to execute the pre-render view extensions before we do any view dependent work.
	FSceneRenderer::ViewExtensionPreRender_RenderThread(RHICmdList, SceneRenderer);

	SceneRenderer->RenderThreadBegin(RHICmdList);

	// update any resources that needed a deferred update
	FDeferredUpdateResource::UpdateResources(RHICmdList);

	const FSceneViewFamily& ViewFamily = SceneRenderer->ViewFamily;

	{
		SCOPE_CYCLE_COUNTER_VERBOSE(STAT_TotalSceneRenderingTime, ViewFamily.ProfileDescription.IsEmpty() ? nullptr : *ViewFamily.ProfileDescription);

		{
			const ERHIFeatureLevel::Type FeatureLevel = SceneRenderer->FeatureLevel;

			FRDGBuilder GraphBuilder(
				RHICmdList,
				RDG_EVENT_NAME("SceneRenderer_%s(ViewFamily=%s)",
					ViewFamily.EngineShowFlags.HitProxies ? TEXT("RenderHitProxies") : TEXT("Render"),
					ViewFamily.bResolveScene ? TEXT("Primary") : TEXT("Auxiliary")
				),
				FSceneRenderer::GetRDGParalelExecuteFlags(FeatureLevel)
			);

#if WITH_MGPU
			if (ViewFamily.bForceCopyCrossGPU)
			{
				GraphBuilder.EnableForceCopyCrossGPU();
			}
#endif

			if (ViewFamily.EngineShowFlags.HitProxies)
			{
				// Render the scene's hit proxies.
				SceneRenderer->RenderHitProxies(GraphBuilder);
			}
			else
			{
				// Render the scene.
				SceneRenderer->Render(GraphBuilder);
			}
			GraphBuilder.Execute();

			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(PostRenderCleanUp);

			if (IsHairStrandsEnabled(EHairStrandsShaderType::All, SceneRenderer->Scene->GetShaderPlatform()) && SceneRenderer->Views.Num() > 0 && !ViewFamily.EngineShowFlags.HitProxies)
			{
				FHairStrandsBookmarkParameters Parameters = CreateHairStrandsBookmarkParameters(SceneRenderer->Scene, SceneRenderer->Views);
				if (Parameters.HasInstances())
				{
					RunHairStrandsBookmark(EHairStrandsBookmark::ProcessEndOfFrame, Parameters);
				}
			}

			// Only reset per-frame scene state once all views have processed their frame, including those in planar reflections
			for (int32 CacheType = 0; CacheType < UE_ARRAY_COUNT(SceneRenderer->Scene->DistanceFieldSceneData.PrimitiveModifiedBounds); CacheType++)
			{
				ResetAndShrinkModifiedBounds(SceneRenderer->Scene->DistanceFieldSceneData.PrimitiveModifiedBounds[CacheType]);
			}

			if (SceneRenderer->Scene->LumenSceneData)
			{
				ResetAndShrinkModifiedBounds(SceneRenderer->Scene->LumenSceneData->PrimitiveModifiedBounds);
			}

			// Immediately issue EndFrame() for all extensions in case any of the outstanding tasks they issued getting out of this frame
			extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

			for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
			{
				Extension->EndFrame();
			}
		}

#if STATS
		{
			QUICK_SCOPE_CYCLE_COUNTER(STAT_RenderViewFamily_RenderThread_MemStats);

			// Update scene memory stats that couldn't be tracked continuously
			SET_MEMORY_STAT(STAT_RenderingSceneMemory, SceneRenderer->Scene->GetSizeBytes());

			SIZE_T ViewStateMemory = 0;
			for (int32 ViewIndex = 0; ViewIndex < SceneRenderer->Views.Num(); ViewIndex++)
			{
				if (SceneRenderer->Views[ViewIndex].State)
				{
					ViewStateMemory += SceneRenderer->Views[ViewIndex].State->GetSizeBytes();
				}
			}
			SET_MEMORY_STAT(STAT_ViewStateMemory, ViewStateMemory);
			SET_MEMORY_STAT(STAT_RenderingMemStackMemory, FMemStack::Get().GetByteCount());
			SET_MEMORY_STAT(STAT_LightInteractionMemory, FLightPrimitiveInteraction::GetMemoryPoolSize());
		}
#endif

#if !UE_BUILD_SHIPPING
		// Update on screen notifications.
		FRendererOnScreenNotification::Get().Broadcast();
#endif
	}

#if STATS
	QUICK_SCOPE_CYCLE_COUNTER(STAT_RenderViewFamily_RenderThread_RHIGetGPUFrameCycles);
	if (FPlatformProperties::SupportsWindowedMode() == false)
	{
		/** Update STATS with the total GPU time taken to render the last frame. */
		SET_CYCLE_COUNTER(STAT_TotalGPUFrameTime, RHIGetGPUFrameCycles());
	}
#endif

	SceneRenderer->RenderThreadEnd(RHICmdList);
}

void OnChangeSimpleForwardShading(IConsoleVariable* Var)
{
	static const auto SupportSimpleForwardShadingCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SupportSimpleForwardShading"));
	static const auto SimpleForwardShadingCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SimpleForwardShading"));

	const bool bWasEnabled = CVarSimpleForwardShading_PreviousValue != 0;
	const bool bShouldBeEnabled = SimpleForwardShadingCVar->GetValueOnAnyThread() != 0;
	if( bWasEnabled != bShouldBeEnabled )
	{
	bool bWasIgnored = false;
	{
		if (SupportSimpleForwardShadingCVar->GetValueOnAnyThread() == 0)
		{
				if (bShouldBeEnabled)
				{
					UE_LOG( LogRenderer, Warning, TEXT( "r.SimpleForwardShading ignored as r.SupportSimpleForwardShading is not enabled" ) );
				}
			bWasIgnored = true;
		}
		else if (!PlatformSupportsSimpleForwardShading(GMaxRHIShaderPlatform))
		{
				if (bShouldBeEnabled)
				{
					UE_LOG( LogRenderer, Warning, TEXT( "r.SimpleForwardShading ignored, only supported on PC shader platforms.  Current shader platform %s" ), *LegacyShaderPlatformToShaderFormat( GMaxRHIShaderPlatform ).ToString() );
				}
			bWasIgnored = true;
		}
	}

	if( !bWasIgnored )
	{
		// Propagate cvar change to static draw lists
	FGlobalComponentRecreateRenderStateContext Context;
	}
	}

	CVarSimpleForwardShading_PreviousValue = SimpleForwardShadingCVar->GetValueOnAnyThread();
}

void OnChangeCVarRequiringRecreateRenderState(IConsoleVariable* Var)
{
	// Propgate cvar change to static draw lists
	FGlobalComponentRecreateRenderStateContext Context;
}

FRendererModule::FRendererModule()
{
	CVarSimpleForwardShading_PreviousValue = CVarSimpleForwardShading.AsVariable()->GetInt();
	CVarSimpleForwardShading.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnChangeSimpleForwardShading));

	static auto EarlyZPassVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EarlyZPass"));
	EarlyZPassVar->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnChangeCVarRequiringRecreateRenderState));

	static auto CVarVertexDeformationOutputsVelocity = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Velocity.EnableVertexDeformation"));
	CVarVertexDeformationOutputsVelocity->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnChangeCVarRequiringRecreateRenderState));

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	void InitDebugViewModeInterface();
	InitDebugViewModeInterface();
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
}

void FRendererModule::CreateAndInitSingleView(FRHICommandListImmediate& RHICmdList, class FSceneViewFamily* ViewFamily, const struct FSceneViewInitOptions* ViewInitOptions)
{
	// Create and add the new view
	FViewInfo* NewView = new FViewInfo(*ViewInitOptions);
	ViewFamily->Views.Add(NewView);
	FViewInfo* View = (FViewInfo*)ViewFamily->Views[0];
	View->ViewRect = View->UnscaledViewRect;
	View->InitRHIResources();
}

extern CORE_API bool GRenderThreadPollingOn;

void FRendererModule::BeginRenderingViewFamily(FCanvas* Canvas, FSceneViewFamily* ViewFamily)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(BeginRenderingViewFamily);
	check(Canvas);
	check(ViewFamily);
	check(ViewFamily->Scene);
	check(ViewFamily->GetScreenPercentageInterface());

	UWorld* World = nullptr;

	FScene* const Scene = ViewFamily->Scene->GetRenderScene();
	if (Scene)
	{
		World = Scene->GetWorld();
		if (World)
		{
			//guarantee that all render proxies are up to date before kicking off a BeginRenderViewFamily.
			World->SendAllEndOfFrameUpdates();
		}
	}

	ENQUEUE_RENDER_COMMAND(UpdateDeferredCachedUniformExpressions)(
		[](FRHICommandList& RHICmdList)
		{
			FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();
		});

	ENQUEUE_RENDER_COMMAND(UpdateFastVRamConfig)(
		[](FRHICommandList& RHICmdList)
		{
			GFastVRamConfig.Update();
		});

	// Flush the canvas first.
	Canvas->Flush_GameThread();

	if (Scene)
	{
		// We allow caching of per-frame, per-scene data
		Scene->IncrementFrameNumber();
		ViewFamily->FrameNumber = Scene->GetFrameNumber();
	}
	else
	{
		// this is passes to the render thread, better access that than GFrameNumberRenderThread
		ViewFamily->FrameNumber = GFrameNumber;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	{
		extern TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe> GetRendererViewExtension();

		ViewFamily->ViewExtensions.Add(GetRendererViewExtension());
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	// Force the spatial upscaler to be set no earlier than ISceneViewExtension::BeginRenderViewFamily();
	check(ViewFamily->GetPrimarySpatialUpscalerInterface() == nullptr);
	check(ViewFamily->GetSecondarySpatialUpscalerInterface() == nullptr);

	for (int ViewExt = 0; ViewExt < ViewFamily->ViewExtensions.Num(); ViewExt++)
	{
		ViewFamily->ViewExtensions[ViewExt]->BeginRenderViewFamily(*ViewFamily);
	}

	if (Scene)
	{		
		// Set the world's "needs full lighting rebuild" flag if the scene has any uncached static lighting interactions.
		if(World)
		{
			// Note: reading NumUncachedStaticLightingInteractions on the game thread here which is written to by the rendering thread
			// This is reliable because the RT uses interlocked mechanisms to update it
			World->SetMapNeedsLightingFullyRebuilt(Scene->NumUncachedStaticLightingInteractions, Scene->NumUnbuiltReflectionCaptures);
		}

		// Construct the scene renderer.  This copies the view family attributes into its own structures.
		FSceneRenderer* SceneRenderer = FSceneRenderer::CreateSceneRenderer(ViewFamily, Canvas->GetHitProxyConsumer());

		if (!SceneRenderer->ViewFamily.EngineShowFlags.HitProxies)
		{
			USceneCaptureComponent::UpdateDeferredCaptures(Scene);
		}

		if (!SceneRenderer->ViewFamily.EngineShowFlags.HitProxies)
		{
			for (int32 ReflectionIndex = 0; ReflectionIndex < SceneRenderer->Scene->PlanarReflections_GameThread.Num(); ReflectionIndex++)
			{
				UPlanarReflectionComponent* ReflectionComponent = SceneRenderer->Scene->PlanarReflections_GameThread[ReflectionIndex];
				SceneRenderer->Scene->UpdatePlanarReflectionContents(ReflectionComponent, *SceneRenderer);
			}
		}

		SceneRenderer->ViewFamily.DisplayInternalsData.Setup(World);

		const uint64 DrawSceneEnqueue = FPlatformTime::Cycles64();
		ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)(
			[SceneRenderer, DrawSceneEnqueue](FRHICommandListImmediate& RHICmdList)
			{
				// Cache the profiling results pointer, as SceneRenderer may be deleted after rendering finishes
				float* ProfileSceneRenderTime = SceneRenderer->ViewFamily.ProfileSceneRenderTime;

				uint64 SceneRenderStart = FPlatformTime::Cycles64();
				const float StartDelayMillisec = FPlatformTime::ToMilliseconds64(SceneRenderStart - DrawSceneEnqueue);
				CSV_CUSTOM_STAT_GLOBAL(DrawSceneCommand_StartDelay, StartDelayMillisec, ECsvCustomStatOp::Set);
				RenderViewFamily_RenderThread(RHICmdList, SceneRenderer);
				FlushPendingDeleteRHIResources_RenderThread();

				if (ProfileSceneRenderTime)
				{
					*ProfileSceneRenderTime = (float)FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - SceneRenderStart);
				}
			});

		// Force kick the RT if we've got RT polling on.
		// This saves us having to wait until the polling period before the scene draw starts executing.
		if (GRenderThreadPollingOn)
		{
			FTaskGraphInterface::Get().WakeNamedThread(ENamedThreads::GetRenderThread());
		}
	}
}

void FRendererModule::PostRenderAllViewports()
{
	// Increment FrameNumber before render the scene. Wrapping around is no problem.
	// This is the only spot we change GFrameNumber, other places can only read.
	++GFrameNumber;

#if RHI_RAYTRACING
	// Update the resource state after all viewports are done with rendering - all info collected for all views
	Nanite::FCoarseMeshStreamingManager* CoarseMeshSM = IStreamingManager::Get().GetNaniteCoarseMeshStreamingManager();
	if (CoarseMeshSM)
	{
		ENQUEUE_RENDER_COMMAND(NaniteCoarseMeshUpdateResourceStates)(
			[CoarseMeshSM](FRHICommandListImmediate& RHICmdList)
			{
				CoarseMeshSM->UpdateResourceStates();
			});
	}
#endif //#if RHI_RAYTRACING
}

void FRendererModule::PerFrameCleanupIfSkipRenderer()
{
	// Some systems (e.g. Slate) can still draw (via FRendererModule::DrawTileMesh for example) when scene renderer is not used
	ENQUEUE_RENDER_COMMAND(CmdPerFrameCleanupIfSkipRenderer)(
		[](FRHICommandListImmediate& RHICmdList)
	{
		FSceneRenderer::CleanUp(RHICmdList);
		GPrimitiveIdVertexBufferPool.DiscardAll();
	});
}

void FRendererModule::UpdateMapNeedsLightingFullyRebuiltState(UWorld* World)
{
	World->SetMapNeedsLightingFullyRebuilt(World->Scene->GetRenderScene()->NumUncachedStaticLightingInteractions, World->Scene->GetRenderScene()->NumUnbuiltReflectionCaptures);
}

void FRendererModule::DrawRectangle(
		FRHICommandList& RHICmdList,
		float X,
		float Y,
		float SizeX,
		float SizeY,
		float U,
		float V,
		float SizeU,
		float SizeV,
		FIntPoint TargetSize,
		FIntPoint TextureSize,
		const TShaderRef<FShader>& VertexShader,
		EDrawRectangleFlags Flags
		)
{
	::DrawRectangle( RHICmdList, X, Y, SizeX, SizeY, U, V, SizeU, SizeV, TargetSize, TextureSize, VertexShader, Flags );
}

FDelegateHandle FRendererModule::RegisterPostOpaqueRenderDelegate(const FPostOpaqueRenderDelegate& InPostOpaqueRenderDelegate)
{
	return PostOpaqueRenderDelegate.Add(InPostOpaqueRenderDelegate);
}

void FRendererModule::RemovePostOpaqueRenderDelegate(FDelegateHandle InPostOpaqueRenderDelegate)
{
	PostOpaqueRenderDelegate.Remove(InPostOpaqueRenderDelegate);
}

FDelegateHandle FRendererModule::RegisterOverlayRenderDelegate(const FPostOpaqueRenderDelegate& InOverlayRenderDelegate)
{
	return OverlayRenderDelegate.Add(InOverlayRenderDelegate);
}

void FRendererModule::RemoveOverlayRenderDelegate(FDelegateHandle InOverlayRenderDelegate)
{
	OverlayRenderDelegate.Remove(InOverlayRenderDelegate);
}

void FRendererModule::RenderPostOpaqueExtensions(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	const FSceneTextures& SceneTextures)
{
	if (PostOpaqueRenderDelegate.IsBound())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "PostOpaqueExtensions");

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			const FViewInfo& View = Views[ViewIndex];
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

			check(IsInRenderingThread());
			FPostOpaqueRenderParameters RenderParameters;
			RenderParameters.ViewMatrix = View.ViewMatrices.GetViewMatrix();
			RenderParameters.ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
			RenderParameters.ColorTexture = SceneTextures.Color.Target;
			RenderParameters.DepthTexture = SceneTextures.Depth.Target;
			RenderParameters.NormalTexture = SceneTextures.GBufferA;
			RenderParameters.VelocityTexture = SceneTextures.Velocity;
			RenderParameters.SmallDepthTexture = SceneTextures.SmallDepth;
			RenderParameters.ViewUniformBuffer = View.ViewUniformBuffer;
			RenderParameters.SceneTexturesUniformParams = SceneTextures.UniformBuffer;
			RenderParameters.MobileSceneTexturesUniformParams = SceneTextures.MobileUniformBuffer;
			RenderParameters.GlobalDistanceFieldParams = &View.GlobalDistanceFieldInfo.ParameterData;

			RenderParameters.ViewportRect = View.ViewRect;
			RenderParameters.GraphBuilder = &GraphBuilder;

			RenderParameters.Uid = (void*)(&View);
			RenderParameters.View = &View;
			PostOpaqueRenderDelegate.Broadcast(RenderParameters);
		}
	}
}

void FRendererModule::RenderOverlayExtensions(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	const FSceneTextures& SceneTextures)
{
	if (OverlayRenderDelegate.IsBound())
	{
		RDG_EVENT_SCOPE(GraphBuilder, "OverlayExtensions");

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			const FViewInfo& View = Views[ViewIndex];
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

			FPostOpaqueRenderParameters RenderParameters;
			RenderParameters.ViewMatrix = View.ViewMatrices.GetViewMatrix();
			RenderParameters.ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
			RenderParameters.ColorTexture = SceneTextures.Color.Target;
			RenderParameters.DepthTexture = SceneTextures.Depth.Target;
			RenderParameters.SmallDepthTexture = SceneTextures.SmallDepth;

			RenderParameters.ViewportRect = View.ViewRect;
			RenderParameters.GraphBuilder = &GraphBuilder;

			RenderParameters.Uid = (void*)(&View);
			RenderParameters.View = &View;
			OverlayRenderDelegate.Broadcast(RenderParameters);
		}
	}
}

void FRendererModule::RenderPostResolvedSceneColorExtension(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
{
	if (PostResolvedSceneColorCallbacks.IsBound())
	{
		PostResolvedSceneColorCallbacks.Broadcast(GraphBuilder, SceneTextures);
	}
}


class FScenePrimitiveRenderingContext : public IScenePrimitiveRenderingContext
{
public:
	FScenePrimitiveRenderingContext(FRDGBuilder& GraphBuilder, FScene& Scene) :
		GPUScene(Scene.GPUScene),
		GPUSceneDynamicContext(GPUScene)
	{
		Scene.UpdateAllPrimitiveSceneInfos(GraphBuilder, false);
		GPUScene.BeginRender(&Scene, GPUSceneDynamicContext);
		Scene.GPUScene.Update(GraphBuilder, Scene);
	}

	virtual ~FScenePrimitiveRenderingContext()
	{
		GPUScene.EndRender();
	}

	FGPUScene& GPUScene;
	FGPUSceneDynamicContext GPUSceneDynamicContext;
};


IScenePrimitiveRenderingContext* FRendererModule::BeginScenePrimitiveRendering(FRDGBuilder &GraphBuilder, FSceneViewFamily* ViewFamily)
{
	check(ViewFamily->Scene);
	FScene* Scene = ViewFamily->Scene->GetRenderScene();
	check(Scene);

	FScenePrimitiveRenderingContext* ScenePrimitiveRenderingContext = new FScenePrimitiveRenderingContext(GraphBuilder, *Scene);

	return ScenePrimitiveRenderingContext;
}


IAllocatedVirtualTexture* FRendererModule::AllocateVirtualTexture(const FAllocatedVTDescription& Desc)
{
	return FVirtualTextureSystem::Get().AllocateVirtualTexture(Desc);
}

void FRendererModule::DestroyVirtualTexture(IAllocatedVirtualTexture* AllocatedVT)
{
	FVirtualTextureSystem::Get().DestroyVirtualTexture(AllocatedVT);
}

IAdaptiveVirtualTexture* FRendererModule::AllocateAdaptiveVirtualTexture(const FAdaptiveVTDescription& AdaptiveVTDesc, const FAllocatedVTDescription& AllocatedVTDesc)
{
	return FVirtualTextureSystem::Get().AllocateAdaptiveVirtualTexture(AdaptiveVTDesc, AllocatedVTDesc);
}

void FRendererModule::DestroyAdaptiveVirtualTexture(IAdaptiveVirtualTexture* AdaptiveVT)
{
	FVirtualTextureSystem::Get().DestroyAdaptiveVirtualTexture(AdaptiveVT);
}

FVirtualTextureProducerHandle FRendererModule::RegisterVirtualTextureProducer(const FVTProducerDescription& Desc, IVirtualTexture* Producer)
{
	return FVirtualTextureSystem::Get().RegisterProducer(Desc, Producer);
}

void FRendererModule::ReleaseVirtualTextureProducer(const FVirtualTextureProducerHandle& Handle)
{
	FVirtualTextureSystem::Get().ReleaseProducer(Handle);
}

void FRendererModule::ReleaseVirtualTexturePendingResources()
{
	FVirtualTextureSystem::Get().ReleasePendingResources();
}

void FRendererModule::AddVirtualTextureProducerDestroyedCallback(const FVirtualTextureProducerHandle& Handle, FVTProducerDestroyedFunction* Function, void* Baton)
{
	FVirtualTextureSystem::Get().AddProducerDestroyedCallback(Handle, Function, Baton);
}

uint32 FRendererModule::RemoveAllVirtualTextureProducerDestroyedCallbacks(const void* Baton)
{
	return FVirtualTextureSystem::Get().RemoveAllProducerDestroyedCallbacks(Baton);
}

void FRendererModule::RequestVirtualTextureTiles(const FVector2D& InScreenSpaceSize, int32 InMipLevel)
{
	FVirtualTextureSystem::Get().RequestTiles(InScreenSpaceSize, InMipLevel);
}

void FRendererModule::RequestVirtualTextureTiles(const FMaterialRenderProxy* InMaterialRenderProxy, const FVector2D& InScreenSpaceSize, ERHIFeatureLevel::Type InFeatureLevel)
{
	FVirtualTextureSystem::Get().RequestTiles(InMaterialRenderProxy, InScreenSpaceSize, InFeatureLevel);
}

void FRendererModule::RequestVirtualTextureTilesForRegion(IAllocatedVirtualTexture* AllocatedVT, const FVector2D& InScreenSpaceSize, const FVector2D& InViewportPosition, const FVector2D& InViewportSize, const FVector2D& InUV0, const FVector2D& InUV1, int32 InMipLevel)
{
	FVirtualTextureSystem::Get().RequestTilesForRegion(AllocatedVT, InScreenSpaceSize, InViewportPosition, InViewportSize, InUV0, InUV1, InMipLevel);
}

void FRendererModule::LoadPendingVirtualTextureTiles(FRHICommandListImmediate& RHICmdList, ERHIFeatureLevel::Type FeatureLevel)
{
	FMemMark MemMark(FMemStack::Get());
	FRDGBuilder GraphBuilder(RHICmdList);
	FVirtualTextureSystem::Get().LoadPendingTiles(GraphBuilder, FeatureLevel);
	GraphBuilder.Execute();
}

void FRendererModule::SetVirtualTextureRequestRecordBuffer(uint64 Handle)
{
#if WITH_EDITOR
	FVirtualTextureSystem::Get().SetVirtualTextureRequestRecordBuffer(Handle);
#endif
}

uint64 FRendererModule::GetVirtualTextureRequestRecordBuffer(TSet<uint64>& OutPageRequests)
{
#if WITH_EDITOR
	return FVirtualTextureSystem::Get().GetVirtualTextureRequestRecordBuffer(OutPageRequests);
#else
	return (uint64)-1;
#endif
}

void FRendererModule::RequestVirtualTextureTiles(TArray<uint64>&& InPageRequests)
{
	FVirtualTextureSystem::Get().RequestRecordedTiles(MoveTemp(InPageRequests));
}

void FRendererModule::FlushVirtualTextureCache()
{
	FVirtualTextureSystem::Get().FlushCache();
}


uint64 FRendererModule::GetNaniteRequestRecordBuffer(TArray<uint32>& OutPageRequests)
{
#if WITH_EDITOR
	return Nanite::GStreamingManager.GetRequestRecordBuffer(OutPageRequests);
#else
	return (uint64)-1;
#endif
}

void FRendererModule::SetNaniteRequestRecordBuffer(uint64 Handle)
{
#if WITH_EDITOR
	Nanite::GStreamingManager.SetRequestRecordBuffer(Handle);
#endif
}

void FRendererModule::RequestNanitePages(TArrayView<uint32> RequestData)
{
	Nanite::GStreamingManager.RequestNanitePages(RequestData);
}

void FRendererModule::PrefetchNaniteResource(const Nanite::FResources* Resource, uint32 NumFramesUntilRender)
{
	Nanite::GStreamingManager.PrefetchResource(Resource, NumFramesUntilRender);
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

class FConsoleVariableAutoCompleteVisitor 
{
public:
	// @param Name must not be 0
	// @param CVar must not be 0
	static void OnConsoleVariable(const TCHAR *Name, IConsoleObject* CObj, uint32& Crc)
	{
		IConsoleVariable* CVar = CObj->AsVariable();
		if(CVar)
		{
			if(CObj->TestFlags(ECVF_Scalability) || CObj->TestFlags(ECVF_ScalabilityGroup))
			{
				// float should work on int32 as well
				float Value = CVar->GetFloat();
				Crc = FCrc::MemCrc32(&Value, sizeof(Value), Crc);
			}
		}
	}
};
static uint32 ComputeScalabilityCVarHash()
{
	uint32 Ret = 0;

	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateStatic< uint32& >(&FConsoleVariableAutoCompleteVisitor::OnConsoleVariable, Ret));

	return Ret;
}

static void DisplayInternals(FRHICommandListImmediate& RHICmdList, FViewInfo& InView)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	auto Family = InView.Family;
	// if r.DisplayInternals != 0
	if(Family->EngineShowFlags.OnScreenDebug && Family->DisplayInternalsData.IsValid())
	{
		// could be 0
		auto State = InView.ViewState;

		FCanvas Canvas((FRenderTarget*)Family->RenderTarget, NULL, Family->Time, InView.GetFeatureLevel());
		Canvas.SetRenderTargetRect(FIntRect(0, 0, Family->RenderTarget->GetSizeXY().X, Family->RenderTarget->GetSizeXY().Y));


		FRHIRenderPassInfo RenderPassInfo(Family->RenderTarget->GetRenderTargetTexture(), ERenderTargetActions::Load_Store);
		RHICmdList.BeginRenderPass(RenderPassInfo, TEXT("DisplayInternalsRenderPass"));

		// further down to not intersect with "LIGHTING NEEDS TO BE REBUILT"
		FVector2D Pos(30, 140);
		const int32 FontSizeY = 14;

		// dark background
		const uint32 BackgroundHeight = 30;
		Canvas.DrawTile(Pos.X - 4, Pos.Y - 4, 500 + 8, FontSizeY * BackgroundHeight + 8, 0, 0, 1, 1, FLinearColor(0,0,0,0.6f), 0, true);

		UFont* Font = GEngine->GetSmallFont();
		FCanvasTextItem SmallTextItem( Pos, FText::GetEmpty(), GEngine->GetSmallFont(), FLinearColor::White );

		SmallTextItem.SetColor(FLinearColor::White);
		SmallTextItem.Text = FText::FromString(FString::Printf(TEXT("r.DisplayInternals = %d"), Family->DisplayInternalsData.DisplayInternalsCVarValue));
		Canvas.DrawItem(SmallTextItem, Pos);
		SmallTextItem.SetColor(FLinearColor::Gray);
		Pos.Y += 2 * FontSizeY;

		FViewInfo& ViewInfo = (FViewInfo&)InView;
#define CANVAS_HEADER(txt) \
		{ \
			SmallTextItem.SetColor(FLinearColor::Gray); \
			SmallTextItem.Text = FText::FromString(txt); \
			Canvas.DrawItem(SmallTextItem, Pos); \
			Pos.Y += FontSizeY; \
		}
#define CANVAS_LINE(bHighlight, txt, ... ) \
		{ \
			SmallTextItem.SetColor(bHighlight ? FLinearColor::Red : FLinearColor::Gray); \
			SmallTextItem.Text = FText::FromString(FString::Printf(txt, __VA_ARGS__)); \
			Canvas.DrawItem(SmallTextItem, Pos); \
			Pos.Y += FontSizeY; \
		}

		CANVAS_HEADER(TEXT("command line options:"))
		{
			bool bHighlight = !(FApp::UseFixedTimeStep() && FApp::bUseFixedSeed);
			CANVAS_LINE(bHighlight, TEXT("  -UseFixedTimeStep: %u"), FApp::UseFixedTimeStep())
			CANVAS_LINE(bHighlight, TEXT("  -FixedSeed: %u"), FApp::bUseFixedSeed)
			CANVAS_LINE(false, TEXT("  -gABC= (changelist): %d"), GetChangeListNumberForPerfTesting())
		}

		CANVAS_HEADER(TEXT("Global:"))
		CANVAS_LINE(false, TEXT("  FrameNumberRT: %u"), GFrameNumberRenderThread)
		CANVAS_LINE(false, TEXT("  Scalability CVar Hash: %x (use console command \"Scalability\")"), ComputeScalabilityCVarHash())
		//not really useful as it is non deterministic and should not be used for rendering features:  CANVAS_LINE(false, TEXT("  FrameNumberRT: %u"), GFrameNumberRenderThread)
		CANVAS_LINE(false, TEXT("  FrameCounter: %llu"), (uint64)GFrameCounter)
		CANVAS_LINE(false, TEXT("  rand()/SRand: %x/%x"), FMath::Rand(), FMath::GetRandSeed())
		{
			bool bHighlight = Family->DisplayInternalsData.NumPendingStreamingRequests != 0;
			CANVAS_LINE(bHighlight, TEXT("  FStreamAllResourcesLatentCommand: %d"), bHighlight)
		}
		{
			static auto* Var = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Streaming.FramesForFullUpdate"));
			int32 Value = Var->GetValueOnRenderThread();
			bool bHighlight = Value != 0;
			CANVAS_LINE(bHighlight, TEXT("  r.Streaming.FramesForFullUpdate: %u%s"), Value, bHighlight ? TEXT(" (should be 0)") : TEXT(""));
		}

		if(State)
		{
			CANVAS_HEADER(TEXT("State:"))
			CANVAS_LINE(false, TEXT("  TemporalAASample: %u"), State->GetCurrentTemporalAASampleIndex())
			CANVAS_LINE(false, TEXT("  FrameIndexMod8: %u"), State->GetFrameIndex(8))
			CANVAS_LINE(false, TEXT("  LODTransition: %.2f"), State->GetTemporalLODTransition())
		}

		CANVAS_HEADER(TEXT("Family:"))
		CANVAS_LINE(false, TEXT("  Time (Real/World/DeltaWorld): %.2f/%.2f/%.2f"), Family->Time.GetRealTimeSeconds(), Family->Time.GetWorldTimeSeconds(), Family->Time.GetDeltaWorldTimeSeconds())
		CANVAS_LINE(false, TEXT("  FrameNumber: %u"), Family->FrameNumber)
		CANVAS_LINE(false, TEXT("  ExposureSettings: %s"), *Family->ExposureSettings.ToString())
		CANVAS_LINE(false, TEXT("  GammaCorrection: %.2f"), Family->GammaCorrection)

		CANVAS_HEADER(TEXT("View:"))
		CANVAS_LINE(false, TEXT("  TemporalJitter: %.2f/%.2f"), ViewInfo.TemporalJitterPixels.X, ViewInfo.TemporalJitterPixels.Y)
		CANVAS_LINE(false, TEXT("  ViewProjectionMatrix Hash: %x"), InView.ViewMatrices.GetViewProjectionMatrix().ComputeHash())
		CANVAS_LINE(false, TEXT("  ViewLocation: %s"), *InView.ViewLocation.ToString())
		CANVAS_LINE(false, TEXT("  ViewRotation: %s"), *InView.ViewRotation.ToString())
		CANVAS_LINE(false, TEXT("  ViewRect: %s"), *ViewInfo.ViewRect.ToString())

		CANVAS_LINE(false, TEXT("  DynMeshElements/TranslPrim: %d/%d"), ViewInfo.DynamicMeshElements.Num(), ViewInfo.TranslucentPrimCount.NumPrims())

#undef CANVAS_LINE
#undef CANVAS_HEADER

		RHICmdList.EndRenderPass();
		Canvas.Flush_RenderThread(RHICmdList);
	}
#endif

}

TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe> GetRendererViewExtension()
{
	class FRendererViewExtension : public ISceneViewExtension
	{
	public:
		virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) {}
		virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) {}
		virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) {}
		virtual void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) {}
		virtual void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) {}
		virtual int32 GetPriority() const { return 0; }
		virtual void PostRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView)
		{
			FViewInfo& View = static_cast<FViewInfo&>(InView);
			DisplayInternals(RHICmdList, View);
		}
	};
	TSharedRef<FRendererViewExtension, ESPMode::ThreadSafe> ref(new FRendererViewExtension);
	return StaticCastSharedRef<ISceneViewExtension>(ref);
}

#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

void FSceneRenderer::SetStereoViewport(FRHICommandList& RHICmdList, const FViewInfo& View, float ViewportScale) const
{
	if (View.IsInstancedStereoPass())
	{
		if (View.bIsMultiViewEnabled)
		{
			const FViewInfo& LeftView = View;
			const uint32 LeftMinX = LeftView.ViewRect.Min.X * ViewportScale;
			const uint32 LeftMaxX = LeftView.ViewRect.Max.X * ViewportScale;
			const uint32 LeftMaxY = LeftView.ViewRect.Max.Y * ViewportScale;

			const FViewInfo& RightView = static_cast<const FViewInfo&>(*View.GetInstancedView());
			const uint32 RightMinX = RightView.ViewRect.Min.X * ViewportScale;
			const uint32 RightMaxX = RightView.ViewRect.Max.X * ViewportScale;
			const uint32 RightMaxY = RightView.ViewRect.Max.Y * ViewportScale;

			RHICmdList.SetStereoViewport(LeftMinX, RightMinX, 0, 0, 0.0f, LeftMaxX, RightMaxX, LeftMaxY, RightMaxY, 1.0f);
		}
		else
		{
			RHICmdList.SetViewport(View.ViewRect.Min.X * ViewportScale, View.ViewRect.Min.Y * ViewportScale, 0.0f, View.InstancedStereoWidth * ViewportScale, View.ViewRect.Max.Y * ViewportScale, 1.0f);
		}
	}
	else
	{
		RHICmdList.SetViewport(View.ViewRect.Min.X * ViewportScale, View.ViewRect.Min.Y * ViewportScale, 0.0f, View.ViewRect.Max.X * ViewportScale, View.ViewRect.Max.Y * ViewportScale, 1.0f);
	}
}

/**
* Saves a previously rendered scene color target
*/

class FDummySceneColorResolveBuffer : public FVertexBuffer
{
public:
	virtual void InitRHI() override
	{
		const int32 NumDummyVerts = 3;
		const uint32 Size = sizeof(FVector4f) * NumDummyVerts;
		FRHIResourceCreateInfo CreateInfo(TEXT("FDummySceneColorResolveBuffer"));
		VertexBufferRHI = RHICreateBuffer(Size, BUF_Static | BUF_VertexBuffer, 0, ERHIAccess::VertexOrIndexBuffer, CreateInfo);
		void* BufferData = RHILockBuffer(VertexBufferRHI, 0, Size, RLM_WriteOnly);
		FMemory::Memset(BufferData, 0, Size);		
		RHIUnlockBuffer(VertexBufferRHI);		
	}
};

TGlobalResource<FDummySceneColorResolveBuffer> GResolveDummyVertexBuffer;
extern int32 GAllowCustomMSAAResolves;

BEGIN_SHADER_PARAMETER_STRUCT(FResolveSceneColorParameters, )
	RDG_TEXTURE_ACCESS(SceneColor, ERHIAccess::SRVGraphics)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColorFMask)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void AddResolveSceneColorPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureMSAA SceneColor)
{
	check(SceneColor.IsValid());

	const uint32 NumSamples = SceneColor.Target->Desc.NumSamples;
	const EShaderPlatform CurrentShaderPlatform = GetFeatureLevelShaderPlatform(View.FeatureLevel);

	if (NumSamples == 1 || !SceneColor.IsSeparate())
	{
		return;
	}

	if (!GAllowCustomMSAAResolves)
	{
		FResolveRect ResolveRect(View.ViewRect);
		if (View.IsInstancedStereoPass())
		{
			ResolveRect.X1 = 0;
			ResolveRect.X2 = View.InstancedStereoWidth;
		}
		AddCopyToResolveTargetPass(GraphBuilder, SceneColor.Target, SceneColor.Resolve, ResolveRect);
	}
	else
	{
		FRDGTextureSRVRef SceneColorFMask = nullptr;

		if (GRHISupportsExplicitFMask)
		{
			SceneColorFMask = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMetaData(SceneColor.Target, ERDGTextureMetaDataAccess::FMask));
		}

		FResolveSceneColorParameters* PassParameters = GraphBuilder.AllocParameters<FResolveSceneColorParameters>();
		PassParameters->SceneColor = SceneColor.Target;
		PassParameters->SceneColorFMask = SceneColorFMask;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColor.Resolve, SceneColor.Resolve->HasBeenProduced() ? ERenderTargetLoadAction::ELoad : ERenderTargetLoadAction::ENoAction);

		FRDGTextureRef SceneColorTargetable = SceneColor.Target;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ResolveSceneColor"),
			PassParameters,
			ERDGPassFlags::Raster,
			[&View, SceneColorTargetable, SceneColorFMask, NumSamples](FRHICommandList& RHICmdList)
		{
			FRHITexture* SceneColorTargetableRHI = SceneColorTargetable->GetRHI();
			SceneColorTargetable->MarkResourceAsUsed();

			FRHIShaderResourceView* SceneColorFMaskRHI = nullptr;
			if (SceneColorFMask)
			{
				SceneColorFMask->MarkResourceAsUsed();
				SceneColorFMaskRHI = SceneColorFMask->GetRHI();
			}

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			const FIntPoint SceneColorExtent = SceneColorTargetable->Desc.Extent;

			// Resolve views individually. In the case of adaptive resolution, the view family will be much larger than the views individually.
			RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, SceneColorExtent.X, SceneColorExtent.Y, 1.0f);
			RHICmdList.SetScissorRect(true, View.IsInstancedStereoPass() ? 0 : View.ViewRect.Min.X, View.ViewRect.Min.Y,
				View.IsInstancedStereoPass() ? View.InstancedStereoWidth : View.ViewRect.Max.X, View.ViewRect.Max.Y);

			int32 ResolveWidth = CVarWideCustomResolve.GetValueOnRenderThread();

			if (NumSamples <= 1)
			{
				ResolveWidth = 0;
			}

			if (ResolveWidth != 0)
			{
				ResolveFilterWide(RHICmdList, GraphicsPSOInit, View.FeatureLevel, SceneColorTargetableRHI, SceneColorFMaskRHI, FIntPoint(0, 0), NumSamples, ResolveWidth, GResolveDummyVertexBuffer.VertexBufferRHI);
			}
			else
			{
				TShaderMapRef<FHdrCustomResolveVS> VertexShader(View.ShaderMap);
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleList;

				if (SceneColorFMaskRHI)
				{
					if (NumSamples == 2)
					{
						TShaderMapRef<FHdrCustomResolveFMask2xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI, SceneColorFMaskRHI);
					}
					else if (NumSamples == 4)
					{
						TShaderMapRef<FHdrCustomResolveFMask4xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI, SceneColorFMaskRHI);
					}
					else if (NumSamples == 8)
					{
						TShaderMapRef<FHdrCustomResolveFMask8xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI, SceneColorFMaskRHI);
					}
					else
					{
						// Everything other than 2,4,8 samples is not implemented.
						checkNoEntry();
					}
				}
				else
				{
					if (NumSamples == 2)
					{
						TShaderMapRef<FHdrCustomResolve2xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI);
					}
					else if (NumSamples == 4)
					{
						TShaderMapRef<FHdrCustomResolve4xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI);
					}
					else if (NumSamples == 8)
					{
						TShaderMapRef<FHdrCustomResolve8xPS> PixelShader(View.ShaderMap);
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
						PixelShader->SetParameters(RHICmdList, SceneColorTargetableRHI);
					}
					else
					{
						// Everything other than 2,4,8 samples is not implemented.
						checkNoEntry();
					}
				}

				RHICmdList.SetStreamSource(0, GResolveDummyVertexBuffer.VertexBufferRHI, 0);
				RHICmdList.DrawPrimitive(0, 1, 1);
			}

			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
		});
	}
}

void AddResolveSceneColorPass(FRDGBuilder& GraphBuilder, TArrayView<const FViewInfo> Views, FRDGTextureMSAA SceneColor)
{
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		if (View.ShouldRenderView())
		{
			AddResolveSceneColorPass(GraphBuilder, View, SceneColor);
		}
	}
}

BEGIN_SHADER_PARAMETER_STRUCT(FResolveSceneDepthParameters, )
	RDG_TEXTURE_ACCESS(SceneDepth, ERHIAccess::SRVGraphics)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void AddResolveSceneDepthPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureMSAA SceneDepth)
{
	check(SceneDepth.IsValid());

	const uint32 NumSamples = SceneDepth.Target->Desc.NumSamples;
	const EShaderPlatform CurrentShaderPlatform = GetFeatureLevelShaderPlatform(View.FeatureLevel);

	if (NumSamples == 1 || !SceneDepth.IsSeparate())
	{
		return;
	}

	FResolveRect ResolveRect(View.ViewRect);
	if (View.IsInstancedStereoPass())
	{
		ResolveRect.X1 = 0;
		ResolveRect.X2 = View.InstancedStereoWidth;
	}

	if (!GAllowCustomMSAAResolves)
	{
		AddCopyToResolveTargetPass(GraphBuilder, SceneDepth.Target, SceneDepth.Resolve, ResolveRect);
	}
	else
	{
		const FIntPoint DepthExtent = SceneDepth.Resolve->Desc.Extent;

		FResolveSceneDepthParameters* PassParameters = GraphBuilder.AllocParameters<FResolveSceneDepthParameters>();
		PassParameters->SceneDepth = SceneDepth.Target;
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth.Resolve, ERenderTargetLoadAction::ENoAction, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilWrite);

		FRDGTextureRef SourceTexture = SceneDepth.Target;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ResolveSceneDepth"),
			PassParameters,
			ERDGPassFlags::Raster,
			[&View, SourceTexture, NumSamples, DepthExtent, ResolveRect](FRHICommandList& RHICmdList)
		{
			FRHITexture* SourceTextureRHI = SourceTexture->GetRHI();
			SourceTexture->MarkResourceAsUsed();

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Always>::GetRHI();

			RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, DepthExtent.X, DepthExtent.Y, 1.0f);

			TShaderMapRef<FResolveVS> ResolveVertexShader(View.ShaderMap);
			TShaderMapRef<FResolveDepthPS> ResolvePixelShaderAny(View.ShaderMap);
			TShaderMapRef<FResolveDepth2XPS> ResolvePixelShader2X(View.ShaderMap);
			TShaderMapRef<FResolveDepth4XPS> ResolvePixelShader4X(View.ShaderMap);
			TShaderMapRef<FResolveDepth8XPS> ResolvePixelShader8X(View.ShaderMap);

			int32 TextureIndex = -1;
			FRHIPixelShader* ResolvePixelShader;
			switch (NumSamples)
			{
			case 2:
				TextureIndex = ResolvePixelShader2X->UnresolvedSurface.GetBaseIndex();
				ResolvePixelShader = ResolvePixelShader2X.GetPixelShader();
				break;
			case 4:
				TextureIndex = ResolvePixelShader4X->UnresolvedSurface.GetBaseIndex();
				ResolvePixelShader = ResolvePixelShader4X.GetPixelShader();
				break;
			case 8:
				TextureIndex = ResolvePixelShader8X->UnresolvedSurface.GetBaseIndex();
				ResolvePixelShader = ResolvePixelShader8X.GetPixelShader();
				break;
			default:
				ensureMsgf(false, TEXT("Unsupported depth resolve for samples: %i.  Dynamic loop method isn't supported on all platforms.  Please add specific case."), NumSamples);
				TextureIndex = ResolvePixelShaderAny->UnresolvedSurface.GetBaseIndex();
				ResolvePixelShader = ResolvePixelShaderAny.GetPixelShader();
				break;
			}

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ResolveVertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = ResolvePixelShader;
			GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
			RHICmdList.SetBlendFactor(FLinearColor::White);
			RHICmdList.SetShaderTexture(ResolvePixelShader, TextureIndex, SourceTextureRHI);

			ResolveVertexShader->SetParameters(RHICmdList, ResolveRect, ResolveRect, DepthExtent.X, DepthExtent.Y);

			RHICmdList.SetStreamSource(0, nullptr, 0);
			RHICmdList.DrawPrimitive(0, 2, 1);
		});
	}
}

void AddResolveSceneDepthPass(FRDGBuilder& GraphBuilder, TArrayView<const FViewInfo> Views, FRDGTextureMSAA SceneDepth)
{
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		if (View.ShouldRenderView())
		{
			AddResolveSceneDepthPass(GraphBuilder, View, SceneDepth);
		}
	}
}

void VirtualTextureFeedbackBegin(FRDGBuilder& GraphBuilder, TArrayView<const FViewInfo> Views, FIntPoint SceneTextureExtent)
{
	TArray<FIntRect, TInlineAllocator<4>> ViewRects;
	ViewRects.AddUninitialized(Views.Num());
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		ViewRects[ViewIndex] = Views[ViewIndex].ViewRect;
	}

	FVirtualTextureFeedbackBufferDesc Desc;
	Desc.Init2D(SceneTextureExtent, ViewRects, GetVirtualTextureFeedbackScale());
	GVirtualTextureFeedbackBuffer.Begin(GraphBuilder, Desc);
}

void VirtualTextureFeedbackEnd(FRDGBuilder& GraphBuilder)
{
	GVirtualTextureFeedbackBuffer.End(GraphBuilder);
}

FRDGTextureRef CreateHalfResolutionDepthCheckerboardMinMax(FRDGBuilder& GraphBuilder, TArrayView<const FViewInfo> Views, FRDGTextureRef SceneDepthTexture)
{
	const uint32 DownscaleFactor = 2;
	const FIntPoint SmallDepthExtent = GetDownscaledExtent(SceneDepthTexture->Desc.Extent, DownscaleFactor);
	const FRDGTextureDesc SmallDepthDesc = FRDGTextureDesc::Create2D(SmallDepthExtent, PF_DepthStencil, FClearValueBinding::None, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
	FRDGTextureRef SmallDepthTexture = GraphBuilder.CreateTexture(SmallDepthDesc, TEXT("HalfResolutionDepthCheckerboardMinMax"));

	for (const FViewInfo& View : Views)
	{
		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

		const FScreenPassTexture SceneDepth(SceneDepthTexture, View.ViewRect);
		const FScreenPassRenderTarget SmallDepth(SmallDepthTexture, GetDownscaledRect(View.ViewRect, DownscaleFactor), View.DecayLoadAction(ERenderTargetLoadAction::ENoAction));
		AddDownsampleDepthPass(GraphBuilder, View, SceneDepth, SmallDepth, EDownsampleDepthFilter::Checkerboard);
	}

	return SmallDepthTexture;
}

void FSceneRenderer::UpdateSkyIrradianceGpuBuffer(FRHICommandListImmediate& RHICmdList)
{
	if (Scene == nullptr)
	{
		return;
	}

	if (!Scene->SkyIrradianceEnvironmentMap.Buffer)
	{
		Scene->SkyIrradianceEnvironmentMap.Initialize(TEXT("SkyIrradianceEnvironmentMap"), sizeof(FVector4f), 7);
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateSkyIrradianceGpuBuffer);

	FVector4f OutSkyIrradianceEnvironmentMap[7];
	// Make sure there's no padding since we're going to cast to FVector4f*
	checkSlow(sizeof(OutSkyIrradianceEnvironmentMap) == sizeof(FVector4f) * 7);
	check(Scene);
	const bool bUploadIrradiance = 
		Scene->SkyLight
		// Skylights with static lighting already had their diffuse contribution baked into lightmaps
		&& !Scene->SkyLight->bHasStaticLighting
		&& ViewFamily.EngineShowFlags.SkyLighting
		&& !Scene->SkyLight->bRealTimeCaptureEnabled; // When bRealTimeCaptureEnabled is tru, the buffer will be setup on GPU directly in this case

	if (bUploadIrradiance)
	{
		const FSHVectorRGB3& SkyIrradiance = Scene->SkyLight->IrradianceEnvironmentMap;
		SetupSkyIrradianceEnvironmentMapConstantsFromSkyIrradiance(OutSkyIrradianceEnvironmentMap, SkyIrradiance);

		// Set the captured environment map data
		void* DataPtr = RHICmdList.LockBuffer(Scene->SkyIrradianceEnvironmentMap.Buffer, 0, Scene->SkyIrradianceEnvironmentMap.NumBytes, RLM_WriteOnly);
		checkSlow(Scene->SkyIrradianceEnvironmentMap.NumBytes == sizeof(OutSkyIrradianceEnvironmentMap));
		FPlatformMemory::Memcpy(DataPtr, &OutSkyIrradianceEnvironmentMap, sizeof(OutSkyIrradianceEnvironmentMap));
		RHICmdList.UnlockBuffer(Scene->SkyIrradianceEnvironmentMap.Buffer);
	}
	else if (Scene->SkyIrradianceEnvironmentMap.NumBytes == 0)
	{
		// Ensure that sky irradiance SH buffer contains sensible initial values (zero init).
		// If there is no sky in the level, then nothing else may fill this buffer.
		void* DataPtr = RHICmdList.LockBuffer(Scene->SkyIrradianceEnvironmentMap.Buffer, 0, Scene->SkyIrradianceEnvironmentMap.NumBytes, RLM_WriteOnly);
		FPlatformMemory::Memset(DataPtr, 0, Scene->SkyIrradianceEnvironmentMap.NumBytes);
		RHICmdList.UnlockBuffer(Scene->SkyIrradianceEnvironmentMap.Buffer);
	}

	// This buffer is now going to be read for rendering.
	RHICmdList.Transition(FRHITransitionInfo(Scene->SkyIrradianceEnvironmentMap.UAV, ERHIAccess::Unknown, ERHIAccess::SRVMask));
}

void RunGPUSkinCacheTransition(FRHICommandList& RHICmdList, FScene* Scene, EGPUSkinCacheTransition Type)
{
	// * When hair strands is disabled, the skin cache sync point run later 
	//   during the deferred render pass
	// * When hair strands is enabled, the skin cache sync point is run earlier, during 
	//   the init views pass, as the output of the skin cached is used by Niagara
	const bool bHairStrandsEnabled = IsHairStrandsEnabled(EHairStrandsShaderType::All, Scene->GetShaderPlatform());
	const bool bRun = 
		(bHairStrandsEnabled && EGPUSkinCacheTransition::FrameSetup == Type) || 
		(!bHairStrandsEnabled && EGPUSkinCacheTransition::FrameSetup != Type);
	if (bRun)
	{
		if (FGPUSkinCache* GPUSkinCache = Scene->GetGPUSkinCache())
		{
			GPUSkinCache->TransitionAllToReadable(RHICmdList);
		}
	}
}
