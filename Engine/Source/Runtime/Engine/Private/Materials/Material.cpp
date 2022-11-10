// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	UnMaterial.cpp: Shader implementation.
=============================================================================*/

#include "Materials/Material.h"

#include "Stats/StatsMisc.h"
#include "Misc/FeedbackContext.h"
#include "UObject/RenderingObjectVersion.h"
#include "Misc/App.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/UObjectAnnotation.h"
#include "UObject/LinkerLoad.h"
#include "EngineGlobals.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UnrealEngine.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionQualitySwitch.h"
#include "Materials/MaterialExpressionFeatureLevelSwitch.h"
#include "Materials/MaterialExpressionShadingPathSwitch.h"
#include "Materials/MaterialExpressionShaderStageSwitch.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionRuntimeVirtualTextureOutput.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Materials/MaterialExpressionShadingModel.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionExecBegin.h"
#include "Materials/MaterialExpressionExecEnd.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionPower.h"
#include "Engine/Font.h"
#include "SceneManagement.h"
#include "Materials/MaterialUniformExpressions.h"
#include "Engine/SubsurfaceProfile.h"
#include "EditorSupportDelegates.h"
#include "ComponentRecreateRenderStateContext.h"
#include "ShaderCompiler.h"
#include "Materials/MaterialParameterCollection.h"
#include "ShaderPlatformQualitySettings.h"
#include "MaterialShaderQualitySettings.h"
#include "Engine/RendererSettings.h"
#include "ProfilingDebugging/LoadTimeTracker.h"
#include "ProfilingDebugging/CookStats.h"
#include "MaterialCompiler.h"
#include "MaterialShaderType.h"
#include "Materials/MaterialInstanceSupport.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "Materials/MaterialExpressionComment.h"
#include "UObject/EditorObjectVersion.h"
#include "UObject/ReleaseObjectVersion.h"
#include "RenderCore/Public/RenderUtils.h"
#include "Engine/Font.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "UObject/UE5ReleaseStreamObjectVersion.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionStrata.h"
#include "Materials/StrataMaterial.h"
#include "Materials/MaterialExpressionThinTranslucentMaterialOutput.h"
#include "Materials/MaterialExpressionClearCoatNormalCustomOutput.h"
#include "Materials/MaterialExpressionTangentOutput.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"


#if WITH_EDITOR
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#include "ObjectCacheEventSink.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "ThumbnailRendering/SceneThumbnailInfoWithPrimitive.h"
#endif
#include "ShaderCodeLibrary.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "HAL/ThreadHeartBeat.h"
#include "Misc/ScopedSlowTask.h"

#if ENABLE_COOK_STATS
#include "ProfilingDebugging/ScopedTimers.h"
namespace MaterialCookStats
{
	static double UpdateCachedExpressionDataSec = 0.0;

	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
		{
			AddStat(TEXT("Material"), FCookStatsManager::CreateKeyValueArray(
				TEXT("UpdateCachedExpressionDataSec"), UpdateCachedExpressionDataSec
			));
		});
}
#endif

#define LOCTEXT_NAMESPACE "Material"

static TAutoConsoleVariable<int32> CVarMaterialParameterLegacyChecks(
	TEXT("r.MaterialParameterLegacyChecks"),
	0,
	TEXT("When enabled, sanity check new material parameter logic against legacy path.\n")
	TEXT("Note that this can be slow"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMaterialEnableControlFlow(
	TEXT("r.MaterialEnableControlFlow"),
	0,
	TEXT("Allows experemental control flow to be used in the material editor.\n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarMaterialEnableNewHLSLGenerator(
	TEXT("r.MaterialEnableNewHLSLGenerator"),
	0,
	TEXT("Enables the new (WIP) material HLSL generator.\n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

#if WITH_EDITOR
const FMaterialsWithDirtyUsageFlags FMaterialsWithDirtyUsageFlags::DefaultAnnotation;

void FMaterialsWithDirtyUsageFlags::MarkUsageFlagDirty(EMaterialUsage UsageFlag)
{
	MaterialFlagsThatHaveChanged |= (1 << UsageFlag);
}

bool FMaterialsWithDirtyUsageFlags::IsUsageFlagDirty(EMaterialUsage UsageFlag)
{
	return (MaterialFlagsThatHaveChanged & (1 << UsageFlag)) != 0;
}

FUObjectAnnotationSparseBool GMaterialsThatNeedSamplerFixup;
FUObjectAnnotationSparse<FMaterialsWithDirtyUsageFlags,true> GMaterialsWithDirtyUsageFlags;
FUObjectAnnotationSparseBool GMaterialsThatNeedExpressionsFlipped;
FUObjectAnnotationSparseBool GMaterialsThatNeedCoordinateCheck;
FUObjectAnnotationSparseBool GMaterialsThatNeedCommentFix;
FUObjectAnnotationSparseBool GMaterialsThatNeedDecalFix;

#endif // #if WITH_EDITOR

FMaterialResource::FMaterialResource()
	: FMaterial()
	, Material(NULL)
	, MaterialInstance(NULL)
{
}

FMaterialResource::~FMaterialResource()
{
}

int32 FMaterialResource::CompilePropertyAndSetMaterialProperty(EMaterialProperty Property, FMaterialCompiler* Compiler, EShaderFrequency OverrideShaderFrequency, bool bUsePreviousFrameTime) const
{
#if WITH_EDITOR
	// needs to be called in this function!!
	// sets CurrentShaderFrequency
	Compiler->SetMaterialProperty(Property, OverrideShaderFrequency, bUsePreviousFrameTime);

	EShaderFrequency ShaderFrequency = Compiler->GetCurrentShaderFrequency();
	
	int32 SelectionColorIndex = INDEX_NONE;
	int32 SelectionColorToggle = INDEX_NONE;

	if (ShaderFrequency == SF_Pixel &&
		GetMaterialDomain() != MD_Volume &&
		Compiler->IsDevelopmentFeatureEnabled(NAME_SelectionColor))
	{
		// RGB stores SelectionColor value, A is toggle on/off switch for SelectionColor
		int32 SelectionColorVector = Compiler->VectorParameter(NAME_SelectionColor, FLinearColor::Transparent);
		SelectionColorIndex = Compiler->ComponentMask(SelectionColorVector, 1, 1, 1, 0);
		SelectionColorToggle = Compiler->ComponentMask(SelectionColorVector, 0, 0, 0, 1);
	}

	//Compile the material instance if we have one.
	UMaterialInterface* MaterialInterface = MaterialInstance ? static_cast<UMaterialInterface*>(MaterialInstance) : Material;

	int32 Ret = INDEX_NONE;

	switch(Property)
	{
		case MP_EmissiveColor:
			if (SelectionColorIndex != INDEX_NONE)
			{
				// Alpha channel is used to as toggle between EmissiveColor and SelectionColor
				Ret = Compiler->Lerp(MaterialInterface->CompileProperty(Compiler, MP_EmissiveColor, MFCF_ForceCast), SelectionColorIndex, SelectionColorToggle);
			}
			else
			{
				Ret = MaterialInterface->CompileProperty(Compiler, MP_EmissiveColor);
			}
			break;

		case MP_DiffuseColor: 
			Ret = MaterialInterface->CompileProperty(Compiler, MP_DiffuseColor, MFCF_ForceCast);
			break;

		case MP_BaseColor: 
			Ret = MaterialInterface->CompileProperty(Compiler, MP_BaseColor, MFCF_ForceCast);
			break;

		case MP_Opacity:
		case MP_OpacityMask:
			// Force basic opaque surfaces to skip masked/translucent-only attributes.
			// Some features can force the material to create a masked variant which unintentionally runs this dormant code
			if (GetMaterialDomain() != MD_Surface || GetBlendMode() != BLEND_Opaque || (GetShadingModels().IsLit() && !GetShadingModels().HasOnlyShadingModel(MSM_DefaultLit)))
			{
				Ret = MaterialInterface->CompileProperty(Compiler, Property);
			}
			else
			{
				Ret = FMaterialAttributeDefinitionMap::CompileDefaultExpression(Compiler, Property);
			}
			break;
		case MP_ShadingModel:
			if (AllowPerPixelShadingModels(Compiler->GetShaderPlatform()))
			{
				Ret = MaterialInterface->CompileProperty(Compiler, Property);
			}
			else
			{
				FMaterialShadingModelField ShadingModels = Compiler->GetMaterialShadingModels();
				Ret = Compiler->ShadingModel(ShadingModels.GetFirstShadingModel());
			}
			break;
		case MP_MaterialAttributes:
			Ret = MaterialInterface->CompileProperty(Compiler, Property);
			break;

		default:
			Ret = MaterialInterface->CompileProperty(Compiler, Property);
	};
	
	EMaterialValueType AttributeType = FMaterialAttributeDefinitionMap::GetValueType(Property);

	if (Ret != INDEX_NONE)
	{
		FMaterialUniformExpression* Expression = Compiler->GetParameterUniformExpression(Ret);

		if (Expression && Expression->IsConstant())
		{
			// Where possible we want to preserve constant expressions allowing default value checks
			EMaterialValueType ResultType = Compiler->GetParameterType(Ret);
			EMaterialValueType ExactAttributeType = (AttributeType == MCT_Float) ? MCT_Float1 : AttributeType;
			EMaterialValueType ExactResultType = (ResultType == MCT_Float) ? MCT_Float1 : ResultType;

			if (ExactAttributeType == ExactResultType)
			{
				return Ret;
			}
			else if (ResultType == MCT_Float || (ExactAttributeType == MCT_Float1 && ResultType & MCT_Float))
			{
				return Compiler->ComponentMask(Ret, true, ExactAttributeType >= MCT_Float2, ExactAttributeType >= MCT_Float3, ExactAttributeType >= MCT_Float4);
			}
		}
	}

	// MaterialAttributes are expected to give a void statement, don't need to cast that
	if (Property != MP_MaterialAttributes)
	{
		// Output should always be the right type for this property
		Ret = Compiler->ForceCast(Ret, AttributeType);
	}
	return Ret;

#else // WITH_EDITOR
	check(0); // This is editor-only function
	return INDEX_NONE;
#endif // WITH_EDITOR
}

#if HANDLE_CUSTOM_OUTPUTS_AS_MATERIAL_ATTRIBUTES
int32 FMaterialResource::CompileCustomAttribute(const FGuid& AttributeID, FMaterialCompiler* Compiler) const
{
	return Material->CompilePropertyEx(Compiler, AttributeID);
}
#endif

#if WITH_EDITORONLY_DATA
void FMaterialResource::GatherCustomOutputExpressions(TArray<UMaterialExpressionCustomOutput*>& OutCustomOutputs) const
{
	Material->GetAllCustomOutputExpressions(OutCustomOutputs);
}

void FMaterialResource::GatherExpressionsForCustomInterpolators(TArray<UMaterialExpression*>& OutExpressions) const
{
	Material->GetAllExpressionsForCustomInterpolators(OutExpressions);
}
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR
void FMaterialResource::BeginAllowCachingStaticParameterValues()
{
	if (MaterialInstance)
	{
		MaterialInstance->BeginAllowCachingStaticParameterValues();
	}
}

void FMaterialResource::EndAllowCachingStaticParameterValues()
{
	if (MaterialInstance)
	{
		MaterialInstance->EndAllowCachingStaticParameterValues();
	}
}
#endif // WITH_EDITOR

void FMaterialResource::GetShaderMapId(EShaderPlatform Platform, const ITargetPlatform* TargetPlatform, FMaterialShaderMapId& OutId) const
{
	FMaterial::GetShaderMapId(Platform, TargetPlatform, OutId);
#if WITH_EDITOR
	const FMaterialCachedExpressionData& CachedData = GetCachedExpressionData();
	CachedData.AppendReferencedFunctionIdsTo(OutId.ReferencedFunctions);
	CachedData.AppendReferencedParameterCollectionIdsTo(OutId.ReferencedParameterCollections);

	Material->GetForceRecompileTextureIdsHash(OutId.TextureReferencesHash);

	if(MaterialInstance)
	{
		MaterialInstance->GetBasePropertyOverridesHash(OutId.BasePropertyOverridesHash);

		FStaticParameterSet CompositedStaticParameters;
		MaterialInstance->GetStaticParameterValues(CompositedStaticParameters);
		OutId.UpdateFromParameterSet(CompositedStaticParameters);
	}
#endif // WITH_EDITOR
}

#if WITH_EDITORONLY_DATA
void FMaterialResource::GetStaticParameterSet(EShaderPlatform Platform, FStaticParameterSet& OutSet) const
{
	FMaterial::GetStaticParameterSet(Platform, OutSet);

	// Get the set from instance
	if (MaterialInstance)
	{
		MaterialInstance->GetStaticParameterValues(OutSet);
	}
}
#endif // WITH_EDITORONLY_DATA

/**
 * A resource which represents the default instance of a UMaterial to the renderer.
 * Note that default parameter values are stored in the FMaterialUniformExpressionXxxParameter objects now.
 * This resource is only responsible for the selection color.
 */
class FDefaultMaterialInstance : public FMaterialRenderProxy
{
public:

	/**
	 * Called from the game thread to destroy the material instance on the rendering thread.
	 */
	void GameThread_Destroy()
	{
		FDefaultMaterialInstance* Resource = this;
		ENQUEUE_RENDER_COMMAND(FDestroyDefaultMaterialInstanceCommand)(
			[Resource](FRHICommandList& RHICmdList)
		{
			delete Resource;
		});
	}

	virtual const FMaterialRenderProxy* GetFallback(ERHIFeatureLevel::Type InFeatureLevel) const override
	{
		const FMaterialRenderProxy* Fallback = &GetFallbackRenderProxy();
		if (Fallback == this)
		{ 
			// If we are the default material, must not try to fall back to the default material in an error state as that will be infinite recursion
			return nullptr;
		}
		return Fallback;
	}

	virtual const FMaterial* GetMaterialNoFallback(ERHIFeatureLevel::Type InFeatureLevel) const override
	{
		checkSlow(IsInParallelRenderingThread());
		const FMaterial* MaterialResource = Material->GetMaterialResource(InFeatureLevel);
		if (MaterialResource && MaterialResource->GetRenderingThreadShaderMap())
		{
			return MaterialResource;
		}
		return nullptr;
	}

	virtual UMaterialInterface* GetMaterialInterface() const override
	{
		return Material;
	}

	virtual bool GetParameterValue(EMaterialParameterType Type, const FHashedMaterialParameterInfo& ParameterInfo, FMaterialParameterValue& OutValue, const FMaterialRenderContext& Context) const override
	{
		const FMaterialResource* MaterialResource = Material->GetMaterialResource(Context.Material.GetFeatureLevel());
		if (MaterialResource && MaterialResource->GetRenderingThreadShaderMap())
		{
			if (Type == EMaterialParameterType::Scalar && ParameterInfo.Name == GetSubsurfaceProfileParameterName())
			{
				OutValue = GetSubsurfaceProfileId(GetSubsurfaceProfileRT());
				return true;
			}

			return false;
		}
		else
		{
			return GetFallbackRenderProxy().GetParameterValue(Type, ParameterInfo, OutValue, Context);
		}
	}

	// FRenderResource interface.
	virtual FString GetFriendlyName() const { return Material->GetName(); }

	// Constructor.
	FDefaultMaterialInstance(UMaterial* InMaterial)
		: FMaterialRenderProxy(GetPathNameSafe(InMaterial))
		, Material(InMaterial)
	{}

private:

	/** Get the fallback material. */
	FMaterialRenderProxy& GetFallbackRenderProxy() const
	{
		return *(UMaterial::GetDefaultMaterial(Material->MaterialDomain)->GetRenderProxy());
	}

	UMaterial* Material;
};

#if WITH_EDITOR
static bool GAllowCompilationInPostLoad=true;
#else
#define GAllowCompilationInPostLoad true
#endif

void UMaterial::ForceNoCompilationInPostLoad(bool bForceNoCompilation)
{
#if WITH_EDITOR
	GAllowCompilationInPostLoad = !bForceNoCompilation;
#endif
}

static UMaterialFunction* GPowerToRoughnessMaterialFunction = NULL;
static UMaterialFunction* GConvertFromDiffSpecMaterialFunction = NULL;

static UMaterial* GDefaultMaterials[MD_MAX] = {0};

static const TCHAR* GDefaultMaterialNames[MD_MAX] =
{
	// Surface
	TEXT("engine-ini:/Script/Engine.Engine.DefaultMaterialName"),
	// Deferred Decal
	TEXT("engine-ini:/Script/Engine.Engine.DefaultDeferredDecalMaterialName"),
	// Light Function
	TEXT("engine-ini:/Script/Engine.Engine.DefaultLightFunctionMaterialName"),
	// Volume
	//@todo - get a real MD_Volume default material
	TEXT("engine-ini:/Script/Engine.Engine.DefaultMaterialName"),
	// Post Process
	TEXT("engine-ini:/Script/Engine.Engine.DefaultPostProcessMaterialName"),
	// User Interface 
	TEXT("engine-ini:/Script/Engine.Engine.DefaultMaterialName"),
	// Virtual Texture
	TEXT("engine-ini:/Script/Engine.Engine.DefaultMaterialName")
};

void UMaterialInterface::InitDefaultMaterials()
{
	// Note that this function will (in fact must!) be called recursively. This
	// guarantees that the default materials will have been loaded and pointers
	// set before any other material interface has been instantiated -- even
	// one of the default materials! It is actually possible to assert that
	// these materials exist in the UMaterial or UMaterialInstance constructor.
	// 
	// The check for initialization is purely an optimization as initializing
	// the default materials is only done very early in the boot process.
	static bool bInitialized = false;
	if (!bInitialized)
	{
		SCOPED_BOOT_TIMING("UMaterialInterface::InitDefaultMaterials");
		check(IsInGameThread());
		if (!IsInGameThread())
		{
			return;
		}
		static int32 RecursionLevel = 0;
		RecursionLevel++;

		
#if WITH_EDITOR
		GPowerToRoughnessMaterialFunction = LoadObject< UMaterialFunction >(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions01/Shading/PowerToRoughness.PowerToRoughness"), nullptr, LOAD_None, nullptr);
		checkf( GPowerToRoughnessMaterialFunction, TEXT("Cannot load PowerToRoughness") );
		GPowerToRoughnessMaterialFunction->AddToRoot();

		GConvertFromDiffSpecMaterialFunction = LoadObject< UMaterialFunction >(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions01/Shading/ConvertFromDiffSpec.ConvertFromDiffSpec"), nullptr, LOAD_None, nullptr);
		checkf( GConvertFromDiffSpecMaterialFunction, TEXT("Cannot load ConvertFromDiffSpec") );
		GConvertFromDiffSpecMaterialFunction->AddToRoot();
#endif

		for (int32 Domain = 0; Domain < MD_MAX; ++Domain)
		{
			if (GDefaultMaterials[Domain] == nullptr)
			{
				FString ResolvedPath = ResolveIniObjectsReference(GDefaultMaterialNames[Domain]);

				GDefaultMaterials[Domain] = FindObject<UMaterial>(nullptr, *ResolvedPath);
				if (GDefaultMaterials[Domain] == nullptr
#if USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME
					&& (RecursionLevel == 1 || !GEventDrivenLoaderEnabled)
#endif
					)
				{
					GDefaultMaterials[Domain] = LoadObject<UMaterial>(nullptr, *ResolvedPath, nullptr, LOAD_DisableDependencyPreloading, nullptr);
					checkf(GDefaultMaterials[Domain] != nullptr, TEXT("Cannot load default material '%s' from path '%s'"), GDefaultMaterialNames[Domain], *ResolvedPath);
				}
				if (GDefaultMaterials[Domain])
				{
					GDefaultMaterials[Domain]->AddToRoot();
				}
			}
		}
		
		RecursionLevel--;
#if USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME
		bInitialized = !GEventDrivenLoaderEnabled || RecursionLevel == 0;
#else
		bInitialized = true;
#endif
	}
}

void UMaterialInterface::PostCDOContruct()
{
	if (GEventDrivenLoaderEnabled && EVENT_DRIVEN_ASYNC_LOAD_ACTIVE_AT_RUNTIME)
	{
		UMaterial::StaticClass()->GetDefaultObject();
		UMaterialInterface::InitDefaultMaterials();
	}
}

// We can save time if instead of blocking after compilation of each synchronous material we block after scheduling all of them
bool GPoolSpecialMaterialsCompileJobs = true;
bool PoolSpecialMaterialsCompileJobs()
{
	return GPoolSpecialMaterialsCompileJobs;
}

void UMaterialInterface::PostLoadDefaultMaterials()
{
	LLM_SCOPE(ELLMTag::Materials);

	// Here we prevent this function from being called recursively. Mostly this
	// is an optimization and guarantees that default materials are post loaded
	// in the order material domains are defined. Surface -> deferred decal -> etc.
	static bool bPostLoaded = false;
	if (!bPostLoaded)
	{
		check(IsInGameThread());
		bPostLoaded = true;

#if WITH_EDITOR
		GPowerToRoughnessMaterialFunction->ConditionalPostLoad();
		GConvertFromDiffSpecMaterialFunction->ConditionalPostLoad();
#endif

		for (int32 Domain = 0; Domain < MD_MAX; ++Domain)
		{
			UMaterial* Material = GDefaultMaterials[Domain];
#if USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME
			check(Material || (GIsInitialLoad && GEventDrivenLoaderEnabled));
			if (Material && !Material->HasAnyFlags(RF_NeedLoad))
#else
			check(Material);
			if (Material)
#endif
			{
				Material->ConditionalPostLoad();
				// Sometimes the above will get called before the material has been fully serialized
				// in this case its NeedPostLoad flag will not be cleared.
				if (Material->HasAnyFlags(RF_NeedPostLoad))
				{
					bPostLoaded = false;
				}
			}
			else
			{
				bPostLoaded = false;
			}
		}

		// Block after scheduling for compilation all (hopefully) default materials.
		// Even if not all of them ended up being post-loaded, block here just out of extra caution
		if (GPoolSpecialMaterialsCompileJobs == true)
		{
			GPoolSpecialMaterialsCompileJobs = false;
			GShaderCompilingManager->FinishAllCompilation();
		}
	}
}

void UMaterialInterface::AssertDefaultMaterialsExist()
{
#if (USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME)
	if (!GIsInitialLoad || !GEventDrivenLoaderEnabled)
#endif
	{
		for (int32 Domain = 0; Domain < MD_MAX; ++Domain)
		{
			check(GDefaultMaterials[Domain] != NULL);
		}
	}
}

void UMaterialInterface::AssertDefaultMaterialsPostLoaded()
{
#if (USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME)
	if (!GIsInitialLoad || !GEventDrivenLoaderEnabled)
#endif
	{
		for (int32 Domain = 0; Domain < MD_MAX; ++Domain)
		{
			check(GDefaultMaterials[Domain] != NULL);
			check(!GDefaultMaterials[Domain]->HasAnyFlags(RF_NeedPostLoad));
		}
	}
}

FString MaterialDomainString(EMaterialDomain MaterialDomain)
{
	static const UEnum* Enum = StaticEnum<EMaterialDomain>();
	check(Enum);
	return Enum->GetNameStringByValue(int64(MaterialDomain));
}

static TAutoConsoleVariable<int32> CVarDiscardUnusedQualityLevels(
	TEXT("r.DiscardUnusedQuality"),
	0,
	TEXT("Whether to keep or discard unused quality level shadermaps in memory.\n")
	TEXT("0: keep all quality levels in memory. (default)\n")
	TEXT("1: Discard unused quality levels on load."),
	ECVF_ReadOnly);

void SerializeInlineShaderMaps(
	const TMap<const ITargetPlatform*, TArray<FMaterialResource*>>* PlatformMaterialResourcesToSavePtr,
	FArchive& Ar,
	TArray<FMaterialResource>& OutLoadedResources,
	uint32* OutOffsetToFirstResource)
{
	LLM_SCOPE(ELLMTag::Shaders);
	SCOPED_LOADTIMER(SerializeInlineShaderMaps);

	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);
	Ar.UsingCustomVersion(FReleaseObjectVersion::GUID);

	if (Ar.IsSaving())
	{
		int32 NumResourcesToSave = 0;
		const TArray<FMaterialResource*> *MaterialResourcesToSavePtr = NULL;
		if (Ar.IsCooking() && Ar.IsPersistent() && !Ar.IsObjectReferenceCollector() && !Ar.ShouldSkipBulkData())
		{
			check( PlatformMaterialResourcesToSavePtr );
			auto& PlatformMaterialResourcesToSave = *PlatformMaterialResourcesToSavePtr;

			MaterialResourcesToSavePtr = PlatformMaterialResourcesToSave.Find( Ar.CookingTarget() );
			check( MaterialResourcesToSavePtr != NULL || (Ar.GetLinker()==NULL) );
			if (MaterialResourcesToSavePtr!= NULL )
			{
				NumResourcesToSave = MaterialResourcesToSavePtr->Num();
			}
		}

		Ar << NumResourcesToSave;

		if (MaterialResourcesToSavePtr
			&& NumResourcesToSave > 0)
		{
			FMaterialResourceMemoryWriter ResourceAr(Ar);
			const TArray<FMaterialResource*> &MaterialResourcesToSave = *MaterialResourcesToSavePtr;
			for (int32 ResourceIndex = 0; ResourceIndex < NumResourcesToSave; ResourceIndex++)
			{
				FMaterialResourceWriteScope Scope(&ResourceAr, *MaterialResourcesToSave[ResourceIndex]);
				MaterialResourcesToSave[ResourceIndex]->SerializeInlineShaderMap(ResourceAr);
			}
		}
	}
	else if (Ar.IsLoading())
	{
		int32 NumLoadedResources = 0;
		Ar << NumLoadedResources;

		if (OutOffsetToFirstResource)
		{
			const FLinker* Linker = Ar.GetLinker();
			int64 Tmp = Ar.Tell() - (Linker ? Linker->Summary.TotalHeaderSize : 0);
			check(Tmp >= 0 && Tmp <= 0xffffffffLL);
			*OutOffsetToFirstResource = uint32(Tmp);
		}

		if (NumLoadedResources > 0)
		{
#if STORE_ONLY_ACTIVE_SHADERMAPS
			ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
			EMaterialQualityLevel::Type QualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
			FMaterialResourceProxyReader ResourceAr(Ar, FeatureLevel, QualityLevel);
			OutLoadedResources.Empty(1);
			OutLoadedResources[OutLoadedResources.AddDefaulted()].SerializeInlineShaderMap(ResourceAr);
#else
			ERHIFeatureLevel::Type FeatureLevel = ERHIFeatureLevel::Num;
			EMaterialQualityLevel::Type QualityLevel = EMaterialQualityLevel::Num;
			OutLoadedResources.Empty(NumLoadedResources);
			FMaterialResourceProxyReader ResourceAr(Ar, FeatureLevel, QualityLevel);
			for (int32 ResourceIndex = 0; ResourceIndex < NumLoadedResources; ++ResourceIndex)
			{
				FMaterialResource& LoadedResource = OutLoadedResources[OutLoadedResources.AddDefaulted()];
				LoadedResource.SerializeInlineShaderMap(ResourceAr);
			}
#endif
		}
	}
}

void ProcessSerializedInlineShaderMaps(UMaterialInterface* Owner, TArray<FMaterialResource>& LoadedResources, TArray<FMaterialResource*>& OutMaterialResourcesLoaded)
{
	LLM_SCOPE(ELLMTag::Shaders);
	check(IsInGameThread());

	if (LoadedResources.Num() == 0)
	{
		// Nothing to process
		return;
	}
	UMaterialInstance* OwnerMaterialInstance = Cast<UMaterialInstance>(Owner);
	UMaterial* OwnerMaterial = nullptr;
	if (OwnerMaterialInstance)
	{
		OwnerMaterial = OwnerMaterialInstance->GetBaseMaterial();
	}
	else
	{
		OwnerMaterial = CastChecked<UMaterial>(Owner);
	}

#if WITH_EDITORONLY_DATA
	const bool bLoadedByCookedMaterial = FPlatformProperties::RequiresCookedData() || Owner->GetOutermost()->bIsCookedForEditor;
#else
	const bool bLoadedByCookedMaterial = FPlatformProperties::RequiresCookedData();
#endif
	for (FMaterialResource& Resource : LoadedResources)
	{
		Resource.RegisterInlineShaderMap(bLoadedByCookedMaterial);
	}

	const bool bDiscardUnusedQualityLevels = CVarDiscardUnusedQualityLevels.GetValueOnAnyThread() != 0;
	const EMaterialQualityLevel::Type ActiveQualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;

	checkf(!(STORE_ONLY_ACTIVE_SHADERMAPS && LoadedResources.Num() > 1),
		TEXT("STORE_ONLY_ACTIVE_SHADERMAPS is set, but %d shader maps were loaded, expected at most 1"), LoadedResources.Num());

	for (int32 ResourceIndex = 0; ResourceIndex < LoadedResources.Num(); ResourceIndex++)
	{
		FMaterialResource& LoadedResource = LoadedResources[ResourceIndex];
		FMaterialShaderMap* LoadedShaderMap = LoadedResource.GetGameThreadShaderMap();

		if (LoadedShaderMap && LoadedShaderMap->GetShaderPlatform() == GMaxRHIShaderPlatform)
		{
			const EMaterialQualityLevel::Type LoadedQualityLevel = LoadedShaderMap->GetShaderMapId().QualityLevel;
			const ERHIFeatureLevel::Type LoadedFeatureLevel = LoadedShaderMap->GetShaderMapId().FeatureLevel;
			check(GShaderPlatformForFeatureLevel[LoadedFeatureLevel] == GMaxRHIShaderPlatform);

			bool bIncludeShaderMap = true;
			if (bDiscardUnusedQualityLevels)
			{
				// Only include shader map if QL matches, or doesn't depend on QL
				bIncludeShaderMap = (LoadedQualityLevel == ActiveQualityLevel) || (LoadedQualityLevel == EMaterialQualityLevel::Num);
			}

			if (bIncludeShaderMap)
			{
				FMaterialResource* CurrentResource = FindOrCreateMaterialResource(OutMaterialResourcesLoaded, OwnerMaterial, OwnerMaterialInstance, LoadedFeatureLevel, LoadedQualityLevel);
				CurrentResource->SetInlineShaderMap(LoadedShaderMap);
			}
		}
	}
}

extern FMaterialResource* FindMaterialResource(const TArray<FMaterialResource*>& MaterialResources, ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type QualityLevel, bool bAllowDefaultQuality)
{
	return FindMaterialResource(const_cast<TArray<FMaterialResource*>&>(MaterialResources), InFeatureLevel, QualityLevel, bAllowDefaultQuality);
}

FMaterialResource* FindMaterialResource(TArray<FMaterialResource*>& MaterialResources, ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type QualityLevel, bool bAllowDefaultQuality)
{
	FMaterialResource* DefaultResource = nullptr;
	for (int32 ResourceIndex = 0; ResourceIndex < MaterialResources.Num(); ++ResourceIndex)
	{
		FMaterialResource* CurrentResource = MaterialResources[ResourceIndex];
		if (CurrentResource->GetFeatureLevel() == InFeatureLevel)
		{
			const EMaterialQualityLevel::Type CurrentQualityLevel = CurrentResource->GetQualityLevel();
			if (CurrentQualityLevel == QualityLevel)
			{
				// exact match
				return CurrentResource;
			}
			else if (bAllowDefaultQuality && CurrentQualityLevel == EMaterialQualityLevel::Num)
			{
				// return the default resource, if we don't find a resource for the requested quality level
				DefaultResource = CurrentResource;
			}
		}
	}
	return DefaultResource;
}

FMaterialResource* FindOrCreateMaterialResource(TArray<FMaterialResource*>& MaterialResources,
	UMaterial* OwnerMaterial,
	UMaterialInstance* OwnerMaterialInstance,
	ERHIFeatureLevel::Type InFeatureLevel,
	EMaterialQualityLevel::Type InQualityLevel)
{
	check(OwnerMaterial);
	
	EMaterialQualityLevel::Type QualityLevelForResource = InQualityLevel;
	if (InQualityLevel != EMaterialQualityLevel::Num)
	{
		// See if we have an explicit resource for the requested quality
		TArray<bool, TInlineAllocator<EMaterialQualityLevel::Num>> QualityLevelsUsed;
		if (OwnerMaterialInstance)
		{
			OwnerMaterialInstance->GetQualityLevelUsage(QualityLevelsUsed, GShaderPlatformForFeatureLevel[InFeatureLevel]);
		}
		else
		{
			OwnerMaterial->GetQualityLevelUsage(QualityLevelsUsed, GShaderPlatformForFeatureLevel[InFeatureLevel]);
		}
		if (!QualityLevelsUsed[InQualityLevel])
		{
			// No explicit resource, just use the default
			QualityLevelForResource = EMaterialQualityLevel::Num;
		}
	}
	
	FMaterialResource* CurrentResource = FindMaterialResource(MaterialResources, InFeatureLevel, QualityLevelForResource, false);
	if (!CurrentResource)
	{
		CurrentResource = OwnerMaterialInstance ? OwnerMaterialInstance->AllocatePermutationResource() : OwnerMaterial->AllocateResource();
		CurrentResource->SetMaterial(OwnerMaterial, OwnerMaterialInstance, InFeatureLevel, QualityLevelForResource);
		MaterialResources.Add(CurrentResource);
	}
	else
	{
		// Make sure the resource we found still has the correct owner
		// This needs to be updated for various complicated reasons...
		// * Since these pointers are passed to reference collector, the GC may null them out
		// * Landscape does lots of complicated material reparenting under the hood, which can cause these pointers to get stale
		CurrentResource->SetMaterial(OwnerMaterial);
		CurrentResource->SetMaterialInstance(OwnerMaterialInstance);
	}

	return CurrentResource;
}

UMaterial* UMaterial::GetDefaultMaterial(EMaterialDomain Domain)
{
	InitDefaultMaterials();
	check(Domain >= MD_Surface && Domain < MD_MAX);
	check(GDefaultMaterials[Domain] != NULL);
	UMaterial* Default = GDefaultMaterials[Domain];
	return Default;
}

bool UMaterial::IsDefaultMaterial() const
{
	bool bDefault = false;
	for (int32 Domain = MD_Surface; !bDefault && Domain < MD_MAX; ++Domain)
	{
		bDefault = (this == GDefaultMaterials[Domain]);
	}
	return bDefault;
}

UMaterial::UMaterial(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, ReleasedByRT(true)
{
	BlendMode = BLEND_Opaque;
	ShadingModel = MSM_DefaultLit;
	ShadingModels = FMaterialShadingModelField(ShadingModel); 
	TranslucencyLightingMode = TLM_VolumetricNonDirectional;
	TranslucencyDirectionalLightingIntensity = 1.0f;
	TranslucentShadowDensityScale = 0.5f;
	TranslucentSelfShadowDensityScale = 2.0f;
	TranslucentSelfShadowSecondDensityScale = 10.0f;
	TranslucentSelfShadowSecondOpacity = 0.0f;
	TranslucentBackscatteringExponent = 30.0f;
	TranslucentMultipleScatteringExtinction = FLinearColor(1.0f, 0.833f, 0.588f, 1.0f);
	TranslucentShadowStartOffset = 100.0f;

#if WITH_EDITORONLY_DATA
	DiffuseColor_DEPRECATED.Constant = FColor(128,128,128);
	SpecularColor_DEPRECATED.Constant = FColor(128,128,128);
	BaseColor.Constant = FColor(128,128,128);	
	Metallic.Constant = 0.0f;
	Specular.Constant = 0.5f;
	Roughness.Constant = 0.5f;
	
	Opacity.Constant = 1.0f;
	OpacityMask.Constant = 1.0f;
#endif
	OpacityMaskClipValue = 0.3333f;
	bCastDynamicShadowAsMasked = false;
	bUsedWithStaticLighting = false;
	bEnableSeparateTranslucency_DEPRECATED = true;
	bEnableMobileSeparateTranslucency = false;
	TranslucencyPass = MTP_AfterDOF;
	bEnableResponsiveAA = false;
	bScreenSpaceReflections = false;
	bContactShadows = false;
	bTangentSpaceNormal = true;
	bUseLightmapDirectionality = true;
	bAutomaticallySetUsageInEditor = true;

	bUseMaterialAttributes = false;
	bCastRayTracedShadows = true;
	bUseTranslucencyVertexFog = true;
	bApplyCloudFogging = false;
	bIsSky = false;
	bUsedWithWater = false;
	BlendableLocation = BL_AfterTonemapping;
	BlendablePriority = 0;
	BlendableOutputAlpha = false;
	bIsBlendable = true;
	bEnableStencilTest = false;

	bUseEmissiveForDynamicAreaLighting = false;
	RefractionDepthBias = 0.0f;
	MaterialDecalResponse = MDR_ColorNormalRoughness;

	bAllowDevelopmentShaderCompile = true;
	bIsMaterialEditorStatsMaterial = false;

#if WITH_EDITORONLY_DATA
	MaterialGraph = NULL;
#endif //WITH_EDITORONLY_DATA

	bIsPreviewMaterial = false;
	bIsFunctionPreviewMaterial = false;

	PhysMaterial = nullptr;
	PhysMaterialMask = nullptr;

	FloatPrecisionMode = EMaterialFloatPrecisionMode::MFPM_Default;
}

void UMaterial::PreSave(const class ITargetPlatform* TargetPlatform)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::PreSave(TargetPlatform);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void UMaterial::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	Super::PreSave(ObjectSaveContext);
#if WITH_EDITOR
	GMaterialsWithDirtyUsageFlags.RemoveAnnotation(this);
#endif
}

void UMaterial::PostInitProperties()
{
	LLM_SCOPE(ELLMTag::Materials);

	Super::PostInitProperties();
	if(!HasAnyFlags(RF_ClassDefaultObject))
	{
		DefaultMaterialInstance = new FDefaultMaterialInstance(this);
	}

	// Initialize StateId to something unique, in case this is a new material
	FPlatformMisc::CreateGuid(StateId);
}

FMaterialResource* UMaterial::AllocateResource()
{
	LLM_SCOPE(ELLMTag::Materials);

	return new FMaterialResource();
}

void UMaterial::GetUsedTextures(TArray<UTexture*>& OutTextures, EMaterialQualityLevel::Type QualityLevel, bool bAllQualityLevels, ERHIFeatureLevel::Type FeatureLevel, bool bAllFeatureLevels) const
{
	OutTextures.Empty();

	if (!FPlatformProperties::IsServerOnly())
	{
		const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(this);

		FInt32Range QualityLevelRange(0, EMaterialQualityLevel::Num - 1);
		if (!bAllQualityLevels)
		{
			if (QualityLevel == EMaterialQualityLevel::Num)
			{
				QualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
			}
			QualityLevelRange = FInt32Range(QualityLevel, QualityLevel);
		}

		FInt32Range FeatureLevelRange(0, ERHIFeatureLevel::Num - 1);
		if (!bAllFeatureLevels)
		{
			if (FeatureLevel == ERHIFeatureLevel::Num)
			{
				FeatureLevel = GMaxRHIFeatureLevel;
			}
			FeatureLevelRange = FInt32Range(FeatureLevel, FeatureLevel);
		}

		TArray<const FMaterialResource*, TInlineAllocator<4>> MatchedResources;
		// Parse all relevant quality and feature levels.
		for (int32 QualityLevelIndex = QualityLevelRange.GetLowerBoundValue(); QualityLevelIndex <= QualityLevelRange.GetUpperBoundValue(); ++QualityLevelIndex)
		{
			for (int32 FeatureLevelIndex = FeatureLevelRange.GetLowerBoundValue(); FeatureLevelIndex <= FeatureLevelRange.GetUpperBoundValue(); ++FeatureLevelIndex)
			{
				const FMaterialResource* CurrentResource = FindMaterialResource(MaterialResources, (ERHIFeatureLevel::Type)FeatureLevelIndex, (EMaterialQualityLevel::Type)QualityLevelIndex, true);
				if (CurrentResource)
				{
					MatchedResources.AddUnique(CurrentResource);
				}
			}
		}

		for (const FMaterialResource* CurrentResource : MatchedResources)
		{
			for (int32 TypeIndex = 0; TypeIndex < NumMaterialTextureParameterTypes; TypeIndex++)
			{
				// Iterate over each of the material's texture expressions.
				for (const FMaterialTextureParameterInfo& Parameter : CurrentResource->GetUniformTextureExpressions((EMaterialTextureParameterType)TypeIndex))
				{
					UTexture* Texture = NULL;
					Parameter.GetGameThreadTextureValue(this, *CurrentResource, Texture);
					if (Texture)
					{
						OutTextures.AddUnique(Texture);
					}
				}
			}

#if WITH_EDITOR
			if (MaterialInstance)
			{
				// Also look for any scalar parameters that are acting as lookups for an atlas texture, and store the atlas texture
				const TArrayView<const FMaterialNumericParameterInfo> AtlasExpressions[1] =
				{
					CurrentResource->GetUniformNumericParameterExpressions()
				};

				for (int32 TypeIndex = 0; TypeIndex < UE_ARRAY_COUNT(AtlasExpressions); TypeIndex++)
				{
					// Iterate over each of the material's texture expressions.
					for (const FMaterialNumericParameterInfo& Parameter : AtlasExpressions[TypeIndex])
					{
						if (Parameter.ParameterType == EMaterialParameterType::Scalar)
						{
							bool bIsUsedAsAtlasPosition;
							TSoftObjectPtr<class UCurveLinearColor> Curve;
							TSoftObjectPtr<class UCurveLinearColorAtlas> Atlas;
							MaterialInstance->IsScalarParameterUsedAsAtlasPosition(Parameter.ParameterInfo, bIsUsedAsAtlasPosition, Curve, Atlas);
							if (Atlas)
							{
								OutTextures.AddUnique(Atlas.Get());
							}
						}
					}
				}
			}
#endif // WITH_EDITOR
		}
	}
}

void UMaterial::GetUsedTexturesAndIndices(TArray<UTexture*>& OutTextures, TArray< TArray<int32> >& OutIndices, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel) const
{
	OutTextures.Empty();
	OutIndices.Empty();

	check(QualityLevel != EMaterialQualityLevel::Num && FeatureLevel != ERHIFeatureLevel::Num);

	if (!FPlatformProperties::IsServerOnly())
	{
		const FMaterialResource* CurrentResource = FindMaterialResource(MaterialResources, FeatureLevel, QualityLevel, true);
		if (CurrentResource)
		{
			TArrayView<const FMaterialTextureParameterInfo> ExpressionsByType[NumMaterialTextureParameterTypes];
			uint32 NumTextures = 0u;
			for (uint32 TypeIndex = 0u; TypeIndex < NumMaterialTextureParameterTypes; ++TypeIndex)
			{
				ExpressionsByType[TypeIndex] = CurrentResource->GetUniformTextureExpressions((EMaterialTextureParameterType)TypeIndex);
				NumTextures += ExpressionsByType[TypeIndex].Num();
			}

			// Try to prevent resizing since this would be expensive.
			OutIndices.Empty(NumTextures);

			for (int32 TypeIndex = 0; TypeIndex < UE_ARRAY_COUNT(ExpressionsByType); TypeIndex++)
			{
				// Iterate over each of the material's texture expressions.
				for (const FMaterialTextureParameterInfo& Parameter : ExpressionsByType[TypeIndex])
				{
					UTexture* Texture = NULL;
					Parameter.GetGameThreadTextureValue(this, *CurrentResource, Texture);

					if (Texture)
					{
						int32 InsertIndex = OutTextures.AddUnique(Texture);
						if (InsertIndex >= OutIndices.Num())
						{
							OutIndices.AddDefaulted(InsertIndex - OutIndices.Num() + 1);
						}
						OutIndices[InsertIndex].Add(Parameter.TextureIndex);
					}
				}
			}
		}
	}
}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
void UMaterial::LogMaterialsAndTextures(FOutputDevice& Ar, int32 Indent) const
{
	auto World = GetWorld();
	const EMaterialQualityLevel::Type QualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
	const ERHIFeatureLevel::Type FeatureLevel = World ? World->FeatureLevel.GetValue() : GMaxRHIFeatureLevel;

	Ar.Logf(TEXT("%sMaterial: %s"), FCString::Tab(Indent), *GetName());

	if (FPlatformProperties::IsServerOnly())
	{
		Ar.Logf(TEXT("%sNo Textures: IsServerOnly"), FCString::Tab(Indent + 1));
	}
	else
	{
		const FMaterialResource* MaterialResource = FindMaterialResource(MaterialResources, FeatureLevel, QualityLevel, false);
		if (MaterialResource)
		{
			if (MaterialResource->HasValidGameThreadShaderMap())
			{
				TArray<UTexture*> Textures;
				// GetTextureExpressionValues(MaterialResource, Textures);
				{
					for (int32 TypeIndex = 0; TypeIndex < NumMaterialTextureParameterTypes; TypeIndex++)
					{
						for (const FMaterialTextureParameterInfo& Parameter : MaterialResource->GetUniformTextureExpressions((EMaterialTextureParameterType)TypeIndex))
						{
							UTexture* Texture = NULL;
							Parameter.GetGameThreadTextureValue(this, *MaterialResource, Texture);
							if (Texture)
							{
								Textures.AddUnique(Texture);
							}
						}
					}
				}

				for (UTexture* Texture : Textures)
				{
					if (Texture)
					{
						Ar.Logf(TEXT("%s%s"), FCString::Tab(Indent + 1), *Texture->GetName());
					}
				}
			}
			else
			{
				Ar.Logf(TEXT("%sNo Textures : Invalid GameThread ShaderMap"), FCString::Tab(Indent + 1));
			}
		}
		else
		{
			Ar.Logf(TEXT("%sNo Textures : Invalid MaterialResource"), FCString::Tab(Indent + 1));
		}
	}
}
#endif

void UMaterial::OverrideTexture(const UTexture* InTextureToOverride, UTexture* OverrideTexture, ERHIFeatureLevel::Type InFeatureLevel)
{
#if WITH_EDITOR
	bool bShouldRecacheMaterialExpressions = false;
	ERHIFeatureLevel::Type FeatureLevelsToUpdate[1] = { InFeatureLevel };
	int32 NumFeatureLevelsToUpdate = 1;
	
	for (int32 FeatureLevelIndex = 0; FeatureLevelIndex < NumFeatureLevelsToUpdate; ++FeatureLevelIndex)
	{
		FMaterialResource* Resource = GetMaterialResource(FeatureLevelsToUpdate[FeatureLevelIndex]);
		if (Resource)
		{
			// Iterate over both the 2D textures and cube texture expressions.
			for (int32 TypeIndex = 0; TypeIndex < NumMaterialTextureParameterTypes; TypeIndex++)
			{
				const TArrayView<const FMaterialTextureParameterInfo> Parameters = Resource->GetUniformTextureExpressions((EMaterialTextureParameterType)TypeIndex);
				// Iterate over each of the material's texture expressions.
				for (int32 ParameterIndex = 0; ParameterIndex < Parameters.Num(); ++ParameterIndex)
				{
					const FMaterialTextureParameterInfo& Parameter = Parameters[ParameterIndex];

					// Evaluate the expression in terms of this material instance.
					UTexture* Texture = NULL;
					Parameter.GetGameThreadTextureValue(this, *Resource, Texture);
					if (Texture != NULL && Texture == InTextureToOverride)
					{
						// Override this texture!
						Resource->TransientOverrides.SetTextureOverride((EMaterialTextureParameterType)TypeIndex, Parameter.ParameterInfo, OverrideTexture);
						bShouldRecacheMaterialExpressions = true;
					}
				}
			}
		}
	}

	if (bShouldRecacheMaterialExpressions)
	{
		RecacheUniformExpressions(false);
	}
#endif // WITH_EDITOR
}

void UMaterial::OverrideNumericParameterDefault(EMaterialParameterType Type, const FHashedMaterialParameterInfo& ParameterInfo, const UE::Shader::FValue& Value, bool bOverride, ERHIFeatureLevel::Type InFeatureLevel)
{
#if WITH_EDITOR
	FMaterialResource* Resource = GetMaterialResource(InFeatureLevel);
	if (Resource)
	{
		Resource->TransientOverrides.SetNumericOverride(Type, ParameterInfo, Value, bOverride);

		const TArrayView<const FMaterialNumericParameterInfo> Parameters = Resource->GetUniformNumericParameterExpressions();
		bool bShouldRecacheMaterialExpressions = false;
		// Iterate over each of the material's vector expressions.
		for (int32 i = 0; i < Parameters.Num(); ++i)
		{
			const FMaterialNumericParameterInfo& Parameter = Parameters[i];
			if (Parameter.ParameterInfo == ParameterInfo)
			{
				bShouldRecacheMaterialExpressions = true;
			}
		}

		if (bShouldRecacheMaterialExpressions)
		{
			RecacheUniformExpressions(false);
		}
	}
#endif // #if WITH_EDITOR
}

void UMaterial::RecacheUniformExpressions(bool bRecreateUniformBuffer) const
{
	bool bUsingNewLoader = EVENT_DRIVEN_ASYNC_LOAD_ACTIVE_AT_RUNTIME && GEventDrivenLoaderEnabled;

	// Ensure that default material is available before caching expressions.
	if (!bUsingNewLoader)
	{
		UMaterial::GetDefaultMaterial(MD_Surface);
	}

	if (DefaultMaterialInstance)
	{
		DefaultMaterialInstance->CacheUniformExpressions_GameThread(bRecreateUniformBuffer);
	}

#if WITH_EDITOR
	// Need to invalidate all child material instances as well.
	RecacheMaterialInstanceUniformExpressions(this, bRecreateUniformBuffer);
#endif
}

bool UMaterial::GetUsageByFlag(EMaterialUsage Usage) const
{
	bool UsageValue = false;
	switch(Usage)
	{
		case MATUSAGE_SkeletalMesh: UsageValue = bUsedWithSkeletalMesh; break;
		case MATUSAGE_ParticleSprites: UsageValue = bUsedWithParticleSprites; break;
		case MATUSAGE_BeamTrails: UsageValue = bUsedWithBeamTrails; break;
		case MATUSAGE_MeshParticles: UsageValue = bUsedWithMeshParticles; break;
		case MATUSAGE_NiagaraSprites: UsageValue = bUsedWithNiagaraSprites; break;
		case MATUSAGE_NiagaraRibbons: UsageValue = bUsedWithNiagaraRibbons; break;
		case MATUSAGE_NiagaraMeshParticles: UsageValue = bUsedWithNiagaraMeshParticles; break;
		case MATUSAGE_StaticLighting: UsageValue = bUsedWithStaticLighting; break;
		case MATUSAGE_MorphTargets: UsageValue = bUsedWithMorphTargets; break;
		case MATUSAGE_SplineMesh: UsageValue = bUsedWithSplineMeshes; break;
		case MATUSAGE_InstancedStaticMeshes: UsageValue = bUsedWithInstancedStaticMeshes; break;
		case MATUSAGE_GeometryCollections: UsageValue = bUsedWithGeometryCollections; break;
		case MATUSAGE_Clothing: UsageValue = bUsedWithClothing; break;
		case MATUSAGE_GeometryCache: UsageValue = bUsedWithGeometryCache; break;
		case MATUSAGE_Water: UsageValue = bUsedWithWater; break;
		case MATUSAGE_HairStrands: UsageValue = bUsedWithHairStrands; break;
		case MATUSAGE_LidarPointCloud: UsageValue = bUsedWithLidarPointCloud; break;
		case MATUSAGE_VirtualHeightfieldMesh: UsageValue = bUsedWithVirtualHeightfieldMesh; break;
		case MATUSAGE_Nanite: UsageValue = bUsedWithNanite; break;
		default: UE_LOG(LogMaterial, Fatal,TEXT("Unknown material usage: %u"), (int32)Usage);
	};
	return UsageValue;
}

bool UMaterial::IsUsageFlagDirty(EMaterialUsage Usage)
{
#if WITH_EDITOR
	return GMaterialsWithDirtyUsageFlags.GetAnnotation(this).IsUsageFlagDirty(Usage);
#endif
	return false;
}

bool UMaterial::IsCompilingOrHadCompileError(ERHIFeatureLevel::Type InFeatureLevel)
{
	const FMaterialResource* Res = GetMaterialResource(InFeatureLevel);
	return Res == nullptr || Res->GetGameThreadShaderMap() == nullptr;
}

#if WITH_EDITOR
bool UMaterial::SetParameterValueEditorOnly(const FName& ParameterName, const FMaterialParameterMetadata& Meta)
{
	bool bResult = false;
	for (TObjectPtr<UMaterialExpression>& Expression : Expressions)
	{
		if (Expression && Expression->SetParameterValue(ParameterName, Meta, EMaterialExpressionSetParameterValueFlags::SendPostEditChangeProperty))
		{
			bResult = true;
		}
		else if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			if (FunctionCall->MaterialFunction)
			{
				TArray<UMaterialFunctionInterface*> Functions;
				Functions.Add(FunctionCall->MaterialFunction);
				FunctionCall->MaterialFunction->GetDependentFunctions(Functions);

				for (UMaterialFunctionInterface* Function : Functions)
				{
					const TArray<TObjectPtr<UMaterialExpression>>* ExpressionPtr = Function->GetFunctionExpressions();
					if (ExpressionPtr)
					{
						for (const TObjectPtr<UMaterialExpression>& FunctionExpression : *ExpressionPtr)
						{
							if (FunctionExpression && FunctionExpression->SetParameterValue(ParameterName, Meta, EMaterialExpressionSetParameterValueFlags::SendPostEditChangeProperty))
							{
								bResult = true;
							}
						}
					}
				}
			}
		}
	}

	return bResult;
}

bool UMaterial::SetVectorParameterValueEditorOnly(FName ParameterName, FLinearColor InValue)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = InValue;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetScalarParameterValueEditorOnly(FName ParameterName, float InValue)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = InValue;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetTextureParameterValueEditorOnly(FName ParameterName, class UTexture* InValue)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = InValue;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetRuntimeVirtualTextureParameterValueEditorOnly(FName ParameterName, class URuntimeVirtualTexture* InValue)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = InValue;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetFontParameterValueEditorOnly(FName ParameterName, class UFont* InFontValue, int32 InFontPage)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = FMaterialParameterValue(InFontValue, InFontPage);
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetStaticSwitchParameterValueEditorOnly(FName ParameterName, bool InValue, FGuid InExpressionGuid)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = InValue;
	Meta.ExpressionGuid = InExpressionGuid;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}

bool UMaterial::SetStaticComponentMaskParameterValueEditorOnly(FName ParameterName, bool R, bool G, bool B, bool A, FGuid InExpressionGuid)
{
	FMaterialParameterMetadata Meta;
	Meta.Value = FMaterialParameterValue(R, G, B, A);
	Meta.ExpressionGuid = InExpressionGuid;
	return SetParameterValueEditorOnly(ParameterName, Meta);
}
#endif

void UMaterial::MarkUsageFlagDirty(EMaterialUsage Usage, bool CurrentValue, bool NewValue)
{
#if WITH_EDITOR
	if(CurrentValue != NewValue)
	{
		FMaterialsWithDirtyUsageFlags Annotation = GMaterialsWithDirtyUsageFlags.GetAnnotation(this);
		Annotation.MarkUsageFlagDirty(Usage);
		GMaterialsWithDirtyUsageFlags.AddAnnotation(this, MoveTemp(Annotation));
	}
#endif
}

void UMaterial::SetUsageByFlag(EMaterialUsage Usage, bool NewValue)
{
	bool bOldValue = GetUsageByFlag(Usage);
	MarkUsageFlagDirty(Usage, bOldValue, NewValue);

	switch(Usage)
	{
		case MATUSAGE_SkeletalMesh:
		{
			bUsedWithSkeletalMesh = NewValue; break;
		}
		case MATUSAGE_ParticleSprites:
		{
			bUsedWithParticleSprites = NewValue; break;
		}
		case MATUSAGE_BeamTrails:
		{
			bUsedWithBeamTrails = NewValue; break;
		}
		case MATUSAGE_MeshParticles:
		{
			bUsedWithMeshParticles = NewValue; break;
		}
		case MATUSAGE_NiagaraSprites:
		{
			bUsedWithNiagaraSprites = NewValue; break;
		}
		case MATUSAGE_NiagaraRibbons:
		{
			bUsedWithNiagaraRibbons = NewValue; break;
		}
		case MATUSAGE_NiagaraMeshParticles:
		{
			bUsedWithNiagaraMeshParticles = NewValue; break;
		}
		case MATUSAGE_StaticLighting:
		{
			bUsedWithStaticLighting = NewValue; break;
		}
		case MATUSAGE_MorphTargets:
		{
			bUsedWithMorphTargets = NewValue; break;
		}
		case MATUSAGE_SplineMesh:
		{
			bUsedWithSplineMeshes = NewValue; break;
		}
		case MATUSAGE_InstancedStaticMeshes:
		{
			bUsedWithInstancedStaticMeshes = NewValue; break;
		}
		case MATUSAGE_GeometryCollections:
		{
			bUsedWithGeometryCollections = NewValue; break;
		}
		case MATUSAGE_Clothing:
		{
			bUsedWithClothing = NewValue; break;
		}
		case MATUSAGE_GeometryCache:
		{
			bUsedWithGeometryCache = NewValue; break;
		}
		case MATUSAGE_Water:
		{
			bUsedWithWater = NewValue; break;
		}
		case MATUSAGE_HairStrands:
		{
			bUsedWithHairStrands = NewValue; break;
		}
		case MATUSAGE_LidarPointCloud:
		{
			bUsedWithLidarPointCloud = NewValue; break;
		}
		case MATUSAGE_VirtualHeightfieldMesh:
		{
			bUsedWithVirtualHeightfieldMesh = NewValue; break;
		}
		case MATUSAGE_Nanite:
		{
			bUsedWithNanite = NewValue; break;
		}
		default: UE_LOG(LogMaterial, Fatal, TEXT("Unknown material usage: %u"), (int32)Usage);
	};
#if WITH_EDITOR
	FEditorSupportDelegates::MaterialUsageFlagsChanged.Broadcast(this, Usage);
#endif
}


FString UMaterial::GetUsageName(EMaterialUsage Usage) const
{
	FString UsageName = TEXT("");
	switch(Usage)
	{
		case MATUSAGE_SkeletalMesh: UsageName = TEXT("bUsedWithSkeletalMesh"); break;
		case MATUSAGE_ParticleSprites: UsageName = TEXT("bUsedWithParticleSprites"); break;
		case MATUSAGE_BeamTrails: UsageName = TEXT("bUsedWithBeamTrails"); break;
		case MATUSAGE_MeshParticles: UsageName = TEXT("bUsedWithMeshParticles"); break;
		case MATUSAGE_NiagaraSprites: UsageName = TEXT("bUsedWithNiagaraSprites"); break;
		case MATUSAGE_NiagaraRibbons: UsageName = TEXT("bUsedWithNiagaraRibbons"); break;
		case MATUSAGE_NiagaraMeshParticles: UsageName = TEXT("bUsedWithNiagaraMeshParticles"); break;
		case MATUSAGE_StaticLighting: UsageName = TEXT("bUsedWithStaticLighting"); break;
		case MATUSAGE_MorphTargets: UsageName = TEXT("bUsedWithMorphTargets"); break;
		case MATUSAGE_SplineMesh: UsageName = TEXT("bUsedWithSplineMeshes"); break;
		case MATUSAGE_InstancedStaticMeshes: UsageName = TEXT("bUsedWithInstancedStaticMeshes"); break;
		case MATUSAGE_GeometryCollections: UsageName = TEXT("bUsedWithGeometryCollections"); break;
		case MATUSAGE_Clothing: UsageName = TEXT("bUsedWithClothing"); break;
		case MATUSAGE_GeometryCache: UsageName = TEXT("bUsedWithGeometryCache"); break;
		case MATUSAGE_Water: UsageName = TEXT("bUsedWithWater"); break;
		case MATUSAGE_HairStrands: UsageName = TEXT("bUsedWithHairStrands"); break;
		case MATUSAGE_LidarPointCloud: UsageName = TEXT("bUsedWithLidarPointCloud"); break;
		case MATUSAGE_VirtualHeightfieldMesh: UsageName = TEXT("bUsedWithVirtualHeightfieldMesh"); break;
		case MATUSAGE_Nanite: UsageName = TEXT("bUsedWithNanite"); break;
		default: UE_LOG(LogMaterial, Fatal,TEXT("Unknown material usage: %u"), (int32)Usage);
	};
	return UsageName;
}


bool UMaterial::CheckMaterialUsage(EMaterialUsage Usage)
{
	check(IsInGameThread());
	bool bNeedsRecompile = false;
	return SetMaterialUsage(bNeedsRecompile, Usage);
}

bool UMaterial::CheckMaterialUsage_Concurrent(EMaterialUsage Usage) const 
{
	bool bUsageSetSuccessfully = false;
	if (NeedsSetMaterialUsage_Concurrent(bUsageSetSuccessfully, Usage))
	{
		if (IsInGameThread())
		{
			bUsageSetSuccessfully = const_cast<UMaterial*>(this)->CheckMaterialUsage(Usage);
		}	
		else
		{
			struct FCallSMU
			{
				UMaterial* Material;
				EMaterialUsage Usage;

				FCallSMU(UMaterial* InMaterial, EMaterialUsage InUsage)
					: Material(InMaterial)
					, Usage(InUsage)
				{
				}

				void Task()
				{
					Material->CheckMaterialUsage(Usage);
				}
			};
			UE_LOG(LogMaterial, Log, TEXT("Had to pass SMU back to game thread. Please ensure correct material usage flags."));

			TSharedRef<FCallSMU, ESPMode::ThreadSafe> CallSMU = MakeShareable(new FCallSMU(const_cast<UMaterial*>(this), Usage));
			bUsageSetSuccessfully = false;

			DECLARE_CYCLE_STAT(TEXT("FSimpleDelegateGraphTask.CheckMaterialUsage"),
				STAT_FSimpleDelegateGraphTask_CheckMaterialUsage,
				STATGROUP_TaskGraphTasks);

			FSimpleDelegateGraphTask::CreateAndDispatchWhenReady(
				FSimpleDelegateGraphTask::FDelegate::CreateThreadSafeSP(CallSMU, &FCallSMU::Task),
				GET_STATID(STAT_FSimpleDelegateGraphTask_CheckMaterialUsage), NULL, ENamedThreads::GameThread_Local
			);
		}
	}
	return bUsageSetSuccessfully;
}

bool UMaterial::NeedsSetMaterialUsage_Concurrent(bool &bOutHasUsage, EMaterialUsage Usage) const
{
	bOutHasUsage = true;
	// Material usage is only relevant for materials that can be applied onto a mesh / use with different vertex factories.
	if (MaterialDomain != MD_Surface && MaterialDomain != MD_DeferredDecal && MaterialDomain != MD_Volume)
	{
		bOutHasUsage = false;
		return false;
	}
	// Check that the material has been flagged for use with the given usage flag.
	if(!GetUsageByFlag(Usage) && !bUsedAsSpecialEngineMaterial)
	{
		uint32 UsageFlagBit = (1 << (uint32)Usage);
		if ((UsageFlagWarnings & UsageFlagBit) == 0)
		{
			// This will be overwritten later by SetMaterialUsage, since we are saying that it needs to be called with the return value
			bOutHasUsage = false;
			return true;
		}
		else
		{
			// We have already warned about this, so we aren't going to warn or compile or set anything this time
			bOutHasUsage = false;
			return false;
		}
	}
	return false;
}

bool UMaterial::SetMaterialUsage(bool &bNeedsRecompile, EMaterialUsage Usage)
{
	bNeedsRecompile = false;

	// Material usage is only relevant for materials that can be applied onto a mesh / use with different vertex factories.
	if (MaterialDomain != MD_Surface && MaterialDomain != MD_DeferredDecal && MaterialDomain != MD_Volume)
	{
		return false;
	}

	// Check that the material has been flagged for use with the given usage flag.
	if(!GetUsageByFlag(Usage) && !bUsedAsSpecialEngineMaterial)
	{
		// For materials which do not have their bUsedWith____ correctly set the DefaultMaterial<type> should be used in game
		// Leaving this GIsEditor ensures that in game on PC will not look different than on the Consoles as we will not be compiling shaders on the fly
		if( GIsEditor && !FApp::IsGame() && bAutomaticallySetUsageInEditor )
		{
			check(IsInGameThread());
			//Do not warn the user during automation testing
			if (!GIsAutomationTesting)
			{
				UE_LOG(LogMaterial, Display, TEXT("Material %s needed to have new flag set %s !"), *GetPathName(), *GetUsageName(Usage));
			}

			// Open a material update context so this material can be modified safely.
			FMaterialUpdateContext UpdateContext(
				// We need to sync with the rendering thread but don't reregister components
				// because SetMaterialUsage may be called during registration!
				FMaterialUpdateContext::EOptions::SyncWithRenderingThread
				);
			UpdateContext.AddMaterial(this);

			// If the flag is missing in the editor, set it, and recompile shaders.
			SetUsageByFlag(Usage, true);
			bNeedsRecompile = true;

			// Compile and force the Id to be regenerated, since we changed the material in a way that changes compilation
			CacheResourceShadersForRendering(true);

			// Mark the package dirty so that hopefully it will be saved with the new usage flag.
			// This is important because the only way an artist can fix an infinite 'compile on load' scenario is by saving with the new usage flag
			if (!MarkPackageDirty())
			{
#if WITH_EDITOR
				// The package could not be marked as dirty as we're loading content in the editor. Add a Map Check error to notify the user.
				FFormatNamedArguments Arguments;
				Arguments.Add(TEXT("Material"), FText::FromString(*GetPathName()));
				Arguments.Add(TEXT("Usage"), FText::FromString(*GetUsageName(Usage)));
				FMessageLog("MapCheck").Warning()
					->AddToken(FUObjectToken::Create(this))
					->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_SetMaterialUsage", "Material {Material} was missing the usage flag {Usage}. If the material asset is not re-saved, it may not render correctly when run outside the editor."), Arguments)))
					->AddToken(FActionToken::Create(LOCTEXT("MapCheck_FixMaterialUsage", "Fix"), LOCTEXT("MapCheck_FixMaterialUsage_Desc", "Click to set the usage flag correctly and mark the asset file as needing to be saved."), FOnActionTokenExecuted::CreateUObject(this, &UMaterial::FixupMaterialUsageAfterLoad), true));
				FMessageLog("MapCheck").Open(EMessageSeverity::Warning);
#endif
			}
		}
		else
		{
			uint32 UsageFlagBit = (1 << (uint32)Usage);
			if ((UsageFlagWarnings & UsageFlagBit) == 0)
			{
				UE_LOG(LogMaterial, Warning, TEXT("Material %s missing %s=True! Default Material will be used in game."), *GetPathName(), *GetUsageName(Usage));
				
				if (bAutomaticallySetUsageInEditor)
				{
					UE_LOG(LogMaterial, Warning, TEXT("     The material will recompile every editor launch until resaved."));
				}
				else if (GIsEditor && !FApp::IsGame())
				{
#if WITH_EDITOR
					FFormatNamedArguments Args;
					Args.Add(TEXT("UsageName"), FText::FromString(GetUsageName(Usage)));
					FNotificationInfo Info(FText::Format(LOCTEXT("CouldntSetMaterialUsage","Material didn't allow automatic setting of usage flag {UsageName} needed to render on this component, using Default Material instead."), Args));
					Info.ExpireDuration = 5.0f;
					Info.bUseSuccessFailIcons = true;

					// Give the user feedback as to why they are seeing the default material
					FSlateNotificationManager::Get().AddNotification(Info);
#endif
				}

				UsageFlagWarnings |= UsageFlagBit;
			}

			// Return failure if the flag is missing in game, since compiling shaders in game is not supported on some platforms.
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
void UMaterial::FixupMaterialUsageAfterLoad()
{
	// All we need to do here is mark the package dirty as the usage itself was set on load.
	MarkPackageDirty();
}
#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA
bool UMaterial::IterateDependentFunctions(TFunctionRef<bool(UMaterialFunctionInterface*)> Predicate) const
{
	for (UMaterialExpression* Expression : Expressions)
	{
		if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			if (!FunctionCall->IterateDependentFunctions(Predicate))
			{
				return false;
			}
		}
		else if (UMaterialExpressionMaterialAttributeLayers* Layers = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
		{
			if (!Layers->IterateDependentFunctions(Predicate))
			{
				return false;
			}
		}
	}
	return true;
}

void UMaterial::GetDependentFunctions(TArray<UMaterialFunctionInterface*>& DependentFunctions) const
{
	IterateDependentFunctions([&DependentFunctions](UMaterialFunctionInterface* MaterialFunction) -> bool
	{
		DependentFunctions.AddUnique(MaterialFunction);
		return true;
	});
}
#endif // WITH_EDITORONLY_DATA

extern FPostProcessMaterialNode* IteratePostProcessMaterialNodes(const FFinalPostProcessSettings& Dest, const UMaterial* Material, FBlendableEntry*& Iterator);

void UMaterialInterface::OverrideBlendableSettings(class FSceneView& View, float Weight) const
{
	check(Weight > 0.0f && Weight <= 1.0f);

	FFinalPostProcessSettings& Dest = View.FinalPostProcessSettings;

	const UMaterial* Base = GetMaterial();

	//	should we use UMaterial::GetDefaultMaterial(Domain) instead of skipping the material

	if(!Base || Base->MaterialDomain != MD_PostProcess || !View.State)
	{
		return;
	}

	FBlendableEntry* Iterator = 0;

	FPostProcessMaterialNode* DestNode = IteratePostProcessMaterialNodes(Dest, Base, Iterator);

	// is this the first one of this material?
	if(!DestNode)
	{
		UMaterialInstanceDynamic* InitialMID = View.State->GetReusableMID((UMaterialInterface*)this);

		if(InitialMID)
		{
			// If the initial node is faded in partly we add the base material (it's assumed to be the neutral state, see docs)
			// and then blend in the material instance (it it's the base there is no need for that)
			const UMaterialInterface* SourceData = (Weight < 1.0f) ? Base : this;

			InitialMID->CopyScalarAndVectorParameters(*SourceData, View.FeatureLevel);

			FPostProcessMaterialNode InitialNode(InitialMID, Base->BlendableLocation, Base->BlendablePriority, Base->bIsBlendable);

			// no blending needed on this one
			FPostProcessMaterialNode* InitialDestNode = Dest.BlendableManager.PushBlendableData(1.0f, InitialNode);

			if(Weight < 1.0f && this != Base)
			{
				// We are not done, we still need to fade with SrcMID
				DestNode = InitialDestNode;
			}
		}
	}

	if(DestNode)
	{
		// we apply this material on top of an existing one
		UMaterialInstanceDynamic* DestMID = DestNode->GetMID();
		check(DestMID);

		UMaterialInstance* SrcMID = (UMaterialInstance*)this;
		check(SrcMID);

		// Here we could check for Weight=1.0 and use copy instead of interpolate but that case quite likely not intended anyway.

		// a material already exists, blend (Scalar and Vector parameters) with existing ones
		DestMID->K2_InterpolateMaterialInstanceParams(DestMID, SrcMID, Weight);
	}
}

UMaterial* UMaterial::GetMaterial()
{
	return this;
}

const UMaterial* UMaterial::GetMaterial() const
{
	return this;
}

const UMaterial* UMaterial::GetMaterial_Concurrent(TMicRecursionGuard) const
{
	return this;
}

void UMaterial::GetMaterialInheritanceChain(FMaterialInheritanceChain& OutChain) const
{
	check(!OutChain.BaseMaterial);
	OutChain.BaseMaterial = this;
	if (!OutChain.CachedExpressionData)
	{
		const FMaterialCachedExpressionData* LocalData = CachedExpressionData.Get();
		OutChain.CachedExpressionData = LocalData ? LocalData : &FMaterialCachedExpressionData::EmptyData;
	}
}

#if WITH_EDITOR
void UMaterial::UpdateCachedExpressionData()
{
	COOK_STAT(FScopedDurationTimer BlockingTimer(MaterialCookStats::UpdateCachedExpressionDataSec));

	if (bLoadedCachedExpressionData)
	{
		// Don't need to rebuild cached data if it was serialized
		return;
	}

	if (!CachedExpressionData)
	{
		CachedExpressionData.Reset(new FMaterialCachedExpressionData());
	}

	CachedExpressionData->Reset();
	FMaterialCachedExpressionContext Context;
	CachedExpressionData->UpdateForExpressions(Context, Expressions, EMaterialParameterAssociation::GlobalParameter, -1);
	if (CachedExpressionData->bHasMaterialLayers)
	{
		// Set all layers as linked to parent (there is no parent for base UMaterials)
		CachedExpressionData->MaterialLayers.LinkAllLayersToParent();
	}

	FObjectCacheEventSink::NotifyReferencedTextureChanged_Concurrent(this);
}
#endif // WITH_EDITOR

bool UMaterial::GetParameterValue(EMaterialParameterType Type, const FMemoryImageMaterialParameterInfo& ParameterInfo, FMaterialParameterMetadata& OutResult, EMaterialGetParameterValueFlags Flags) const
{
	if (EnumHasAnyFlags(Flags, EMaterialGetParameterValueFlags::CheckNonOverrides) && CachedExpressionData)
	{
		return CachedExpressionData->Parameters.GetParameterValue(Type, ParameterInfo, OutResult);
	}

	return false;
}

bool UMaterial::GetMaterialLayers(FMaterialLayersFunctions& OutLayers, TMicRecursionGuard) const
{
	if (CachedExpressionData && CachedExpressionData->bHasMaterialLayers)
	{
		OutLayers = CachedExpressionData->MaterialLayers;
		return true;
	}
	return false;
}

bool UMaterial::GetRefractionSettings(float& OutBiasValue) const
{
	OutBiasValue = RefractionDepthBias;
	return true;
}

void UMaterial::GetDependencies(TSet<UMaterialInterface*>& Dependencies) 
{
	Dependencies.Add(this);
}

FMaterialRenderProxy* UMaterial::GetRenderProxy() const
{
	return DefaultMaterialInstance;
}

UPhysicalMaterial* UMaterial::GetPhysicalMaterial() const
{
	if (GEngine)
	{
		return (PhysMaterial != nullptr) ? PhysMaterial : GEngine->DefaultPhysMaterial;
	}
	return nullptr;
}

UPhysicalMaterialMask* UMaterial::GetPhysicalMaterialMask() const
{
	return PhysMaterialMask;
}

UPhysicalMaterial* UMaterial::GetPhysicalMaterialFromMap(int32 Index) const
{
	if (Index >= 0 && Index < EPhysicalMaterialMaskColor::MAX)
	{
		return PhysicalMaterialMap[Index];
	}
	return nullptr;
}

/** Helper functions for text output of properties... */
#ifndef CASE_ENUM_TO_TEXT
#define CASE_ENUM_TO_TEXT(txt) case txt: return TEXT(#txt);
#endif

#ifndef TEXT_TO_ENUM
#define TEXT_TO_ENUM(eVal, txt)		if (FCString::Stricmp(TEXT(#eVal), txt) == 0)	return eVal;
#endif

const TCHAR* UMaterial::GetMaterialShadingModelString(EMaterialShadingModel InMaterialShadingModel)
{
	switch (InMaterialShadingModel)
	{
		FOREACH_ENUM_EMATERIALSHADINGMODEL(CASE_ENUM_TO_TEXT)
	}
	return TEXT("MSM_DefaultLit");
}

EMaterialShadingModel UMaterial::GetMaterialShadingModelFromString(const TCHAR* InMaterialShadingModelStr)
{
	#define TEXT_TO_SHADINGMODEL(m) TEXT_TO_ENUM(m, InMaterialShadingModelStr);
	FOREACH_ENUM_EMATERIALSHADINGMODEL(TEXT_TO_SHADINGMODEL)
	#undef TEXT_TO_SHADINGMODEL
	return MSM_DefaultLit;
}

const TCHAR* UMaterial::GetBlendModeString(EBlendMode InBlendMode)
{
	switch (InBlendMode)
	{
		FOREACH_ENUM_EBLENDMODE(CASE_ENUM_TO_TEXT)
	}
	return TEXT("BLEND_Opaque");
}

EBlendMode UMaterial::GetBlendModeFromString(const TCHAR* InBlendModeStr)
{
	#define TEXT_TO_BLENDMODE(b) TEXT_TO_ENUM(b, InBlendModeStr);
	FOREACH_ENUM_EBLENDMODE(TEXT_TO_BLENDMODE)
	#undef TEXT_TO_BLENDMODE
	return BLEND_Opaque;
}

static FAutoConsoleVariable GCompileMaterialsForShaderFormatCVar(
	TEXT("r.CompileMaterialsForShaderFormat"),
	TEXT(""),
	TEXT("When enabled, compile materials for this shader format in addition to those for the running platform.\n")
	TEXT("Note that these shaders are compiled and immediately tossed. This is only useful when directly inspecting output via r.DebugDumpShaderInfo.")
	);

#if WITH_EDITOR
void UMaterial::GetForceRecompileTextureIdsHash(FSHAHash &TextureReferencesHash)
{
	TArray<UTexture*> ForceRecompileTextures;
	for (const UMaterialExpression *MaterialExpression : Expressions)
	{
		if (MaterialExpression == nullptr)
		{
			continue;
		}
		TArray<UTexture*> ExpressionForceRecompileTextures;
		MaterialExpression->GetTexturesForceMaterialRecompile(ExpressionForceRecompileTextures);
		for (UTexture *ForceRecompileTexture : ExpressionForceRecompileTextures)
		{
			ForceRecompileTextures.AddUnique(ForceRecompileTexture);
		}
	}
	if (ForceRecompileTextures.Num() <= 0)
	{
		//There is no Texture that trig a recompile of the material, nothing to add to the hash
		return;
	}

	FSHA1 TextureCompileDependencies;
	FString OriginalHash = TextureReferencesHash.ToString();
	TextureCompileDependencies.UpdateWithString(*OriginalHash, OriginalHash.Len());

	for (UTexture *ForceRecompileTexture : ForceRecompileTextures)
	{
		FString TextureGuidString = ForceRecompileTexture->GetLightingGuid().ToString();
		TextureCompileDependencies.UpdateWithString(*TextureGuidString, TextureGuidString.Len());
	}

	TextureCompileDependencies.Final();
	TextureCompileDependencies.GetHash(&TextureReferencesHash.Hash[0]);
}

bool UMaterial::IsTextureForceRecompileCacheRessource(UTexture *Texture)
{
	for (const UMaterialExpression *MaterialExpression : Expressions)
	{
		if (MaterialExpression == nullptr)
		{
			continue;
		}
		TArray<UTexture*> ExpressionForceRecompileTextures;
		MaterialExpression->GetTexturesForceMaterialRecompile(ExpressionForceRecompileTextures);
		for (UTexture *ForceRecompileTexture : ExpressionForceRecompileTextures)
		{
			if (Texture == ForceRecompileTexture)
			{
				return true;
			}
		}
	}
	return false;
}

void UMaterial::UpdateMaterialShaderCacheAndTextureReferences()
{
	//Cancel any current compilation jobs that are in flight for this material.
	CancelOutstandingCompilation();

	//Force a recompute of the DDC key
	CacheResourceShadersForRendering(true);
	
	// Ensure that the ReferencedTextureGuids array is up to date.
	if (GIsEditor)
	{
		UpdateLightmassTextureTracking();
	}

	// Ensure that any components with static elements using this material have their render state recreated
	// so changes are propagated to them. The preview material is only applied to the preview mesh component,
	// and that reregister is handled by the material editor.
	if (!bIsPreviewMaterial && !bIsFunctionPreviewMaterial && !bIsMaterialEditorStatsMaterial)
	{
		FGlobalComponentRecreateRenderStateContext RecreateComponentsRenderState;
	}
	// needed for UMaterial as it doesn't have the InitResources() override where this is called
	PropagateDataToMaterialProxy();
}

#endif //WITH_EDITOR

UE_TRACE_EVENT_BEGIN(Cpu, CacheResourceShadersForRendering, NoSync)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, MaterialName)
UE_TRACE_EVENT_END()

void UMaterial::CacheResourceShadersForRendering(bool bRegenerateId, EMaterialShaderPrecompileMode PrecompileMode)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UMaterial::CacheResourceShadersForRendering);

#if CPUPROFILERTRACE_ENABLED
	FString TraceMaterialName;
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(CpuChannel))
	{
		TraceMaterialName = GetFullName();
	}
	UE_TRACE_LOG_SCOPED_T(Cpu, CacheResourceShadersForRendering, CpuChannel)
		<< CacheResourceShadersForRendering.MaterialName(*TraceMaterialName);
#endif

#if WITH_EDITOR
	// Always rebuild the shading model field on recompile
	RebuildShadingModelField();
#endif //WITH_EDITOR

	if (bRegenerateId)
	{
		// Regenerate this material's Id if requested
		// Since we can't provide an explanation for why we've been asked to change the guid.
		// We can't give this function a unique transformation id, let it generate a new one.
		ReleaseResourcesAndMutateDDCKey();
	}

	// Resources cannot be deleted before uniform expressions are recached because
	// UB layouts will be accessed and they are owned by material resources
	FMaterialResourceDeferredDeletionArray ResourcesToFree;
#if STORE_ONLY_ACTIVE_SHADERMAPS
	ResourcesToFree = MoveTemp(MaterialResources);
	MaterialResources.Reset();
#endif

	if (FApp::CanEverRender())
	{
		const EMaterialQualityLevel::Type ActiveQualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
		uint32 FeatureLevelsToCompile = GetFeatureLevelsToCompileForRendering();

		TArray<FMaterialResource*> ResourcesToCache;
		while (FeatureLevelsToCompile != 0)
		{
			const ERHIFeatureLevel::Type FeatureLevel = (ERHIFeatureLevel::Type)FBitSet::GetAndClearNextBit(FeatureLevelsToCompile);
			const EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[FeatureLevel];

			// Only cache shaders for the quality level that will actually be used to render
			// In cooked build, there is no shader compilation but this is still needed
			// to register the loaded shadermap
			FMaterialResource* CurrentResource = FindOrCreateMaterialResource(MaterialResources, this, nullptr, FeatureLevel, ActiveQualityLevel);
			check(CurrentResource);

#if STORE_ONLY_ACTIVE_SHADERMAPS
			if (CurrentResource && !CurrentResource->GetGameThreadShaderMap())
			{
				// Load the shader map for this resource, if needed
				FMaterialResource Tmp;
				FName PackageFileName = GetOutermost()->FileName;
				UE_CLOG(PackageFileName.IsNone(), LogMaterial, Warning,
					TEXT("UMaterial::CacheResourceShadersForRendering - Can't reload material resource '%s'. File system based reload is unsupported in this build."),
					*GetFullName());
				if (!PackageFileName.IsNone() && ReloadMaterialResource(&Tmp, PackageFileName.ToString(), OffsetToFirstResource, FeatureLevel, ActiveQualityLevel))
				{
					CurrentResource->SetInlineShaderMap(Tmp.GetGameThreadShaderMap());
					CurrentResource->UpdateInlineShaderMapIsComplete();
				}
			}
#endif // STORE_ONLY_ACTIVE_SHADERMAPS

			ResourcesToCache.Reset();
			ResourcesToCache.Add(CurrentResource);
			CacheShadersForResources(ShaderPlatform, ResourcesToCache, PrecompileMode);
		}

		FString AdditionalFormatToCache = GCompileMaterialsForShaderFormatCVar->GetString();
		if (!AdditionalFormatToCache.IsEmpty())
		{
			EShaderPlatform AdditionalPlatform = ShaderFormatToLegacyShaderPlatform(FName(*AdditionalFormatToCache));
			if (AdditionalPlatform != SP_NumPlatforms)
			{
				ResourcesToCache.Reset();
				CacheResourceShadersForCooking(AdditionalPlatform,ResourcesToCache);
				for (int32 i = 0; i < ResourcesToCache.Num(); ++i)
				{
					FMaterialResource* Resource = ResourcesToCache[i];
					delete Resource;
				}
				ResourcesToCache.Reset();
			}
		}

		RecacheUniformExpressions(true);
	}

	FMaterial::DeferredDeleteArray(ResourcesToFree);
}

void UMaterial::CacheResourceShadersForCooking(EShaderPlatform ShaderPlatform, TArray<FMaterialResource*>& OutCachedMaterialResources, const ITargetPlatform* TargetPlatform)
{
	TArray<FMaterialResource*> NewResourcesToCache;	// only new resources need to have CacheShaders() called on them, whereas OutCachedMaterialResources may already contain resources for another shader platform
	GetNewResources(ShaderPlatform, NewResourcesToCache);

	CacheShadersForResources(ShaderPlatform, NewResourcesToCache, EMaterialShaderPrecompileMode::Background, TargetPlatform);

	OutCachedMaterialResources.Append(NewResourcesToCache);
}

void UMaterial::GetNewResources(EShaderPlatform ShaderPlatform, TArray<FMaterialResource*>& NewResourcesToCache)
{
	ERHIFeatureLevel::Type TargetFeatureLevel = GetMaxSupportedFeatureLevel(ShaderPlatform);

	TArray<bool, TInlineAllocator<EMaterialQualityLevel::Num> > QualityLevelsUsed;
	GetQualityLevelUsageForCooking(QualityLevelsUsed, ShaderPlatform);

	const UShaderPlatformQualitySettings* MaterialQualitySettings = UMaterialShaderQualitySettings::Get()->GetShaderPlatformQualitySettings(ShaderPlatform);
	bool bNeedDefaultQuality = false;

	for (int32 QualityLevelIndex = 0; QualityLevelIndex < EMaterialQualityLevel::Num; QualityLevelIndex++)
	{
		// Add all quality levels actually used
		if (QualityLevelsUsed[QualityLevelIndex])
		{
			FMaterialResource* NewResource = AllocateResource();
			NewResource->SetMaterial(this, nullptr, (ERHIFeatureLevel::Type)TargetFeatureLevel, (EMaterialQualityLevel::Type)QualityLevelIndex);
			NewResourcesToCache.Add(NewResource);
		}
		else
		{
			const FMaterialQualityOverrides& QualityOverrides = MaterialQualitySettings->GetQualityOverrides((EMaterialQualityLevel::Type)QualityLevelIndex);
			if (!QualityOverrides.bDiscardQualityDuringCook)
			{
				// don't have an explicit resource for this quality level, but still need to support it, so make sure we include a default quality resource
				bNeedDefaultQuality = true;
			}
		}
	}

	if (bNeedDefaultQuality)
	{
		FMaterialResource* NewResource = AllocateResource();
		NewResource->SetMaterial(this, nullptr, (ERHIFeatureLevel::Type)TargetFeatureLevel);
		NewResourcesToCache.Add(NewResource);
	}
}

void UMaterial::CacheShadersForResources(EShaderPlatform ShaderPlatform, const TArray<FMaterialResource*>& ResourcesToCache, EMaterialShaderPrecompileMode PrecompileMode, const ITargetPlatform* TargetPlatform)
{
#if WITH_EDITOR
	check(!HasAnyFlags(RF_NeedPostLoad));
#endif
	for (int32 ResourceIndex = 0; ResourceIndex < ResourcesToCache.Num(); ResourceIndex++)
	{
		FMaterialResource* CurrentResource = ResourcesToCache[ResourceIndex];
		const bool bSuccess = CurrentResource->CacheShaders(ShaderPlatform, PrecompileMode, TargetPlatform);

		if (!bSuccess)
		{
			const bool bIsDefaultMaterial = IsDefaultMaterial();
			FString ErrorString;
			if (bIsDefaultMaterial)
			{
				ErrorString += FString::Printf(TEXT("Failed to compile Default Material for platform %s!\n"),
					*LegacyShaderPlatformToShaderFormat(ShaderPlatform).ToString());
			}
			else
			{
				ErrorString += FString::Printf(TEXT("Failed to compile Material for platform %s, Default Material will be used in game.\n"),
					*LegacyShaderPlatformToShaderFormat(ShaderPlatform).ToString());
			}

#if WITH_EDITOR
			const TArray<FString>& CompileErrors = CurrentResource->GetCompileErrors();
			for (int32 ErrorIndex = 0; ErrorIndex < CompileErrors.Num(); ErrorIndex++)
			{
				ErrorString += FString::Printf(TEXT("	%s\n"), *CompileErrors[ErrorIndex]);
			}
#endif
			
			if (bIsDefaultMaterial)
			{
				UE_ASSET_LOG(LogMaterial, Fatal, this, TEXT("%s"), *ErrorString);
			}
			else
			{
				UE_ASSET_LOG(LogMaterial, Warning, this, TEXT("%s"), *ErrorString);
			}
		}
	}
}

void UMaterial::ReleaseResourcesAndMutateDDCKey(const FGuid& TransformationId)
{
	if (TransformationId.IsValid())
	{
		// Combine current guid with the transformation applied.
		StateId.A ^= TransformationId.A;
		StateId.B ^= TransformationId.B;
		StateId.C ^= TransformationId.C;
		StateId.D ^= TransformationId.D;
		
	}
	else
	{
		FPlatformMisc::CreateGuid(StateId);
	}

	if(FApp::CanEverRender())
	{
		for (FMaterialResource* CurrentResource : MaterialResources)
		{
			CurrentResource->ReleaseShaderMap();
		}

		// Release all resources because we could have changed the quality levels (e.g. in material editor).
		FMaterialResourceDeferredDeletionArray ResourcesToFree = MoveTemp(MaterialResources);
		MaterialResources.Reset();
		FMaterial::DeferredDeleteArray(ResourcesToFree);
	}
}

bool UMaterial::AttemptInsertNewGroupName(const FString & InNewName)
{
#if WITH_EDITOR
	FParameterGroupData* ParameterGroupDataElement = ParameterGroupData.FindByPredicate([&InNewName](const FParameterGroupData& DataElement)
	{
		return InNewName == DataElement.GroupName;
	});

	if (ParameterGroupDataElement == nullptr)
	{
		FParameterGroupData NewGroupData;
		NewGroupData.GroupName = InNewName;
		NewGroupData.GroupSortPriority = 0;
		ParameterGroupData.Add(NewGroupData);
		return true;
	}
#endif
	return false;
}

FMaterialResource* UMaterial::GetMaterialResource(ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type QualityLevel)
{
	if (QualityLevel == EMaterialQualityLevel::Num)
	{
		QualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
	}
	return FindMaterialResource(MaterialResources, InFeatureLevel, QualityLevel, true);
}

const FMaterialResource* UMaterial::GetMaterialResource(ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type QualityLevel) const
{
	if (QualityLevel == EMaterialQualityLevel::Num)
	{
		QualityLevel = GetCachedScalabilityCVars().MaterialQualityLevel;
	}
	return FindMaterialResource(MaterialResources, InFeatureLevel, QualityLevel, true);
}

void UMaterial::Serialize(FArchive& Ar)
{
	LLM_SCOPE(ELLMTag::Materials);
	SCOPED_LOADTIMER(MaterialSerializeTime);

	Ar.UsingCustomVersion(FRenderingObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5ReleaseStreamObjectVersion::GUID);

	Super::Serialize(Ar);

	if (Ar.UEVer() >= VER_UE4_PURGED_FMATERIAL_COMPILE_OUTPUTS)
	{
#if WITH_EDITOR
		static_assert(!STORE_ONLY_ACTIVE_SHADERMAPS, "Only discard unused SMs in cooked build");
		SerializeInlineShaderMaps(&CachedMaterialResourcesForCooking, Ar, LoadedMaterialResources);
#else
		SerializeInlineShaderMaps(
			NULL,
			Ar,
			LoadedMaterialResources
#if STORE_ONLY_ACTIVE_SHADERMAPS
			, &OffsetToFirstResource
#endif
		);
#endif
	}
	else
	{
#if WITH_EDITOR
		FMaterialResource* LegacyResource = AllocateResource();
		LegacyResource->LegacySerialize(Ar);
		StateId = LegacyResource->GetLegacyId();
		delete LegacyResource;
#endif
	}

#if WITH_EDITOR
	// CachedExpressionData is moved to UMaterialInterface
	// Actual data will be regenerated on load in editor, so here we just need to handle skipping over any legacy data that might be in the archive
	{
		bool bLocalSavedCachedExpressionData_DEPRECATED = false;
		if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::MaterialSavedCachedData &&
			Ar.CustomVer(FUE5ReleaseStreamObjectVersion::GUID) < FUE5ReleaseStreamObjectVersion::MaterialInterfaceSavedCachedData)
		{
			Ar << bLocalSavedCachedExpressionData_DEPRECATED;
		}

		if (Ar.IsLoading() && bSavedCachedExpressionData_DEPRECATED)
		{
			bSavedCachedExpressionData_DEPRECATED = false;
			bLocalSavedCachedExpressionData_DEPRECATED = true;
		}

		if (bLocalSavedCachedExpressionData_DEPRECATED)
		{
			FMaterialCachedExpressionData LocalCachedExpressionData;
			UScriptStruct* Struct = FMaterialCachedExpressionData::StaticStruct();
			Struct->SerializeTaggedProperties(Ar, (uint8*)&LocalCachedExpressionData, Struct, nullptr);
		}
	}
#endif // WITH_EDITOR

#if WITH_EDITOR
	if (Ar.UEVer() < VER_UE4_FLIP_MATERIAL_COORDS)
	{
		GMaterialsThatNeedExpressionsFlipped.Set(this);
	}
	else if (Ar.UEVer() < VER_UE4_FIX_MATERIAL_COORDS)
	{
		GMaterialsThatNeedCoordinateCheck.Set(this);
	}
	else if (Ar.UEVer() < VER_UE4_FIX_MATERIAL_COMMENTS)
	{
		GMaterialsThatNeedCommentFix.Set(this);
	}

	if (Ar.UEVer() < VER_UE4_ADD_LINEAR_COLOR_SAMPLER)
	{
		GMaterialsThatNeedSamplerFixup.Set(this);
	}
#endif // #if WITH_EDITOR

	static_assert(MP_MAX == 33, "New material properties must have DoMaterialAttributeReorder called on them to ensure that any future reordering of property pins is correctly applied.");

	if (Ar.UEVer() < VER_UE4_MATERIAL_MASKED_BLENDMODE_TIDY)
	{
		//Set based on old value. Real check may not be possible here in cooked builds?
		//Cached using acutal check in PostEditChangProperty().
		if (BlendMode == BLEND_Masked && !bIsMasked_DEPRECATED)
		{
			bCanMaskedBeAssumedOpaque = true;
		}
		else
		{
			bCanMaskedBeAssumedOpaque = false;
		}
	}

	if(Ar.IsLoading() && Ar.CustomVer(FRenderingObjectVersion::GUID) < FRenderingObjectVersion::IntroducedMeshDecals)
	{
		if(MaterialDomain == MD_DeferredDecal)
		{
			BlendMode = BLEND_Translucent;
		}
	}
	
#if WITH_EDITOR
	if (Ar.IsLoading() && Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::RemoveDecalBlendMode)
	{
		if (MaterialDomain == MD_DeferredDecal)
		{
			GMaterialsThatNeedDecalFix.Set(this);
		}
	}
#endif

#if WITH_EDITOR
	if (Ar.IsSaving() && Ar.IsCooking() && Ar.IsPersistent() && !Ar.IsObjectReferenceCollector() && FShaderLibraryCooker::NeedsShaderStableKeys(EShaderPlatform::SP_NumPlatforms))
	{
		SaveShaderStableKeys(Ar.CookingTarget());
	}
#endif

#if WITH_EDITORONLY_DATA
	if (MaterialDomain == MD_Volume && Ar.IsLoading() && Ar.CustomVer(FRenderingObjectVersion::GUID) < FRenderingObjectVersion::VolumeExtinctionBecomesRGB)
	{
		if (Opacity.IsConnected()) // Base material input cannot have default values so we only deal with connected expression
		{
			// Change expression output from the Opacity to SubSurfaceColor that is now representing RGB extinction. Leave opacity connected as it is unused now anyway
			SubsurfaceColor.Connect(Opacity.OutputIndex, Opacity.Expression);
			// Now disconnect Opacity
			Opacity.Expression = nullptr;

			// Now force the material to recompile and we use a hash of the original StateId.
			// This is to avoid having different StateId each time we load the material and to not forever recompile it,i.e. use a cached version.
			uint32 HashBuffer[5];
			FSHA1::HashBuffer(&StateId, sizeof(FGuid), reinterpret_cast<uint8*>(HashBuffer));
			StateId.A = HashBuffer[0];
			StateId.B = HashBuffer[1];
			StateId.C = HashBuffer[2];
			StateId.D = HashBuffer[3];
		}
	}
#endif // WITH_EDITORONLY_DATA

	if (Ar.IsLoading() && Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::MaterialTranslucencyPass)
	{
		if (bEnableSeparateTranslucency_DEPRECATED == false)
		{
			TranslucencyPass = MTP_BeforeDOF;
		}
	}
}

void UMaterial::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	// Reset the StateId on duplication since it needs to be unique for each material.
	FPlatformMisc::CreateGuid(StateId);
}

void UMaterial::BackwardsCompatibilityInputConversion()
{
#if WITH_EDITOR
	if( ShadingModel != MSM_Unlit )
	{
		bool bIsDS = DiffuseColor_DEPRECATED.IsConnected() || SpecularColor_DEPRECATED.IsConnected();
		bool bIsBMS = BaseColor.IsConnected() || Metallic.IsConnected() || Specular.IsConnected();

		if( bIsDS && !bIsBMS )
		{
			// ConvertFromDiffSpec

			check( GConvertFromDiffSpecMaterialFunction );

			UMaterialExpressionMaterialFunctionCall* FunctionExpression = NewObject<UMaterialExpressionMaterialFunctionCall>(this);
			Expressions.Add( FunctionExpression );

			FunctionExpression->MaterialExpressionEditorX += 200;

			FunctionExpression->MaterialFunction = GConvertFromDiffSpecMaterialFunction;
			FunctionExpression->UpdateFromFunctionResource();

			if( DiffuseColor_DEPRECATED.IsConnected() )
			{
				FunctionExpression->GetInput(0)->Connect( DiffuseColor_DEPRECATED.OutputIndex, DiffuseColor_DEPRECATED.Expression );
			}

			if( SpecularColor_DEPRECATED.IsConnected() )
			{
				FunctionExpression->GetInput(1)->Connect( SpecularColor_DEPRECATED.OutputIndex, SpecularColor_DEPRECATED.Expression );
			}

			BaseColor.Connect( 0, FunctionExpression );
			Metallic.Connect( 1, FunctionExpression );
			Specular.Connect( 2, FunctionExpression );
		}
	}
#endif // WITH_EDITOR
}

void UMaterial::BackwardsCompatibilityVirtualTextureOutputConversion()
{
#if WITH_EDITOR
	// Remove MD_RuntimeVirtualTexture support and replace with an explicit UMaterialExpressionRuntimeVirtualTextureOutput.
	if (MaterialDomain == MD_RuntimeVirtualTexture)
	{
		// Change this guid if you change the conversion code below
		static FGuid BackwardsCompatibilityVirtualTextureOutputConversionGuid(TEXT("BABD7074-001F-4FC2-BDE5-3A0C436F4414"));

		MaterialDomain = MD_Surface;

		if (!bUseMaterialAttributes)
		{
			// Create a new UMaterialExpressionRuntimeVirtualTextureOutput node and route the old material attribute output to it.
			UMaterialExpressionRuntimeVirtualTextureOutput* OutputExpression = NewObject<UMaterialExpressionRuntimeVirtualTextureOutput>(this);
			Expressions.Add(OutputExpression);

			OutputExpression->MaterialExpressionEditorX = EditorX;
			OutputExpression->MaterialExpressionEditorY = EditorY - 300;

			if (BaseColor.IsConnected())
			{
				OutputExpression->GetInput(0)->Connect(BaseColor.OutputIndex, BaseColor.Expression);
			}
			if (Specular.IsConnected())
			{
				OutputExpression->GetInput(1)->Connect(Specular.OutputIndex, Specular.Expression);
			}
			if (Roughness.IsConnected())
			{
				OutputExpression->GetInput(2)->Connect(Roughness.OutputIndex, Roughness.Expression);
			}
			if (Normal.IsConnected())
			{
				if (bTangentSpaceNormal)
				{
					OutputExpression->GetInput(3)->Connect(Normal.OutputIndex, Normal.Expression);
				}
				else
				{
					// Apply the tangent space to world transform that would be applied in the material output.
					UMaterialExpressionTransform* TransformExpression = NewObject<UMaterialExpressionTransform>(this);
					Expressions.Add(TransformExpression);

					TransformExpression->MaterialExpressionEditorX = EditorX - 300;
					TransformExpression->MaterialExpressionEditorY = EditorY - 300;
					TransformExpression->TransformSourceType = TRANSFORMSOURCE_Tangent;
					TransformExpression->TransformType = TRANSFORM_World;
					TransformExpression->Input.Connect(Normal.OutputIndex, Normal.Expression);

					OutputExpression->GetInput(3)->Connect(0, TransformExpression);
				}
			}
			if (Opacity.IsConnected())
			{
				OutputExpression->GetInput(5)->Connect(Opacity.OutputIndex, Opacity.Expression);
			}
			if (BlendMode != BLEND_Opaque)
			{
				// Full alpha blend modes were mostly/always used with MD_RuntimeVirtualTexture to allow pin connections.
				// But we will assume the intention for any associated MD_Surface output is opaque or alpha mask and force convert here.
				if (Opacity.IsConnected())
				{
					OpacityMask.Connect(Opacity.OutputIndex, Opacity.Expression);
					Opacity.Expression = nullptr;
				}
				BlendMode = OpacityMask.IsConnected() ? BLEND_Masked : BLEND_Opaque;
				bCanMaskedBeAssumedOpaque = !OpacityMask.Expression && !(OpacityMask.UseConstant && OpacityMask.Constant < 0.999f);
			}
		}

		// Recompile after changes with a guid representing the conversion applied here.
		ReleaseResourcesAndMutateDDCKey(BackwardsCompatibilityVirtualTextureOutputConversionGuid);
	}
#endif // WITH_EDITOR
}

void UMaterial::BackwardsCompatibilityDecalConversion()
{
#if WITH_EDITOR
	if (GMaterialsThatNeedDecalFix.Get(this))
	{
		// Change this guid if you change the conversion code below
		static FGuid BackwardsCompatibilityDecalConversionGuid(TEXT("352069F8-1B8C-406A-9B88-6946BCDF2C10"));

		GMaterialsThatNeedDecalFix.Clear(this);

		// Move stain and alpha composite setting into material blend mode.
		if (DecalBlendMode == DBM_AlphaComposite)
		{
			BlendMode = BLEND_AlphaComposite;
		}
		else if (DecalBlendMode == DBM_Stain)
		{
			BlendMode = BLEND_Modulate;
		}
		else
		{
			BlendMode = BLEND_Translucent;
		}

		// Disconnect outputs according to old DBuffer blend mode.
		if (DecalBlendMode == DBM_DBuffer_Normal || DecalBlendMode == DBM_DBuffer_Roughness || DecalBlendMode == DBM_DBuffer_NormalRoughness)
		{
			BaseColor.Expression = nullptr;
		}
		if (DecalBlendMode == DBM_DBuffer_Color || DecalBlendMode == DBM_DBuffer_Roughness || DecalBlendMode == DBM_DBuffer_ColorRoughness || DecalBlendMode == DBM_AlphaComposite)
		{
			Normal.Expression = nullptr;
		}
		if (DecalBlendMode == DBM_DBuffer_Color || DecalBlendMode == DBM_DBuffer_Normal || DecalBlendMode == DBM_DBuffer_ColorNormal)
		{
			Roughness.Expression = Specular.Expression = Metallic.Expression = nullptr;
		}

		// Previously translucent decals used default values in all unconnected attributes (except for normal).
		// For backwards compatibility we connect those attributes with defaults.
		if (DecalBlendMode == DBM_Translucent || DecalBlendMode == DBM_AlphaComposite || DecalBlendMode == DBM_Stain)
		{
			if (!BaseColor.IsConnected() || !Metallic.IsConnected())
			{
				UMaterialExpressionConstant* Expression = NewObject<UMaterialExpressionConstant>(this);
				Expressions.Add(Expression);

				Expression->MaterialExpressionEditorX = EditorX - 100;
				Expression->MaterialExpressionEditorY = EditorY - 120;
				Expression->R = 0.f;

				if (!BaseColor.IsConnected())
				{
					BaseColor.Connect(0, Expression);
				}
				if (!Metallic.IsConnected())
				{
					Metallic.Connect(0, Expression);
				}
			}

			if (!Roughness.IsConnected() || !Specular.IsConnected())
			{
				UMaterialExpressionConstant* Expression = NewObject<UMaterialExpressionConstant>(this);
				Expressions.Add(Expression);

				Expression->MaterialExpressionEditorX = EditorX - 100;
				Expression->MaterialExpressionEditorY = EditorY - 60;
				Expression->R = .5f;

				if (!Roughness.IsConnected())
				{
					Roughness.Connect(0, Expression);
				}
				if (!Specular.IsConnected())
				{
					Specular.Connect(0, Expression);
				}
			}
		}

		// Recompile after changes with a guid representing the conversion applied here.
		ReleaseResourcesAndMutateDDCKey(BackwardsCompatibilityDecalConversionGuid);
	}
#endif // WITH_EDITOR
}

static void AddStrataShadingModelFromMaterialShadingModel(FStrataMaterialInfo& OutInfo, const FMaterialShadingModelField& InShadingModels)
{
	if (InShadingModels.HasShadingModel(MSM_Unlit))				{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_Unlit); }
	if (InShadingModels.HasShadingModel(MSM_DefaultLit))		{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_DefaultLit); }
	if (InShadingModels.HasShadingModel(MSM_Subsurface))		{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_SubsurfaceLit); }
	if (InShadingModels.HasShadingModel(MSM_PreintegratedSkin))	{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_SubsurfaceLit); }
	if (InShadingModels.HasShadingModel(MSM_ClearCoat))			{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_DefaultLit); }
	if (InShadingModels.HasShadingModel(MSM_SubsurfaceProfile))	{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_SubsurfaceLit); }
	if (InShadingModels.HasShadingModel(MSM_TwoSidedFoliage))	{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_SubsurfaceLit); }
	if (InShadingModels.HasShadingModel(MSM_Hair))				{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_Hair); }
	if (InShadingModels.HasShadingModel(MSM_Cloth))				{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_DefaultLit); }
	if (InShadingModels.HasShadingModel(MSM_Eye))				{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_SubsurfaceLit); }
	if (InShadingModels.HasShadingModel(MSM_SingleLayerWater)) { OutInfo.AddShadingModel(EStrataShadingModel::SSM_SingleLayerWater); }
	if (InShadingModels.HasShadingModel(MSM_DoubleLayerWater))	{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_DoubleLayerWater); }
	if (InShadingModels.HasShadingModel(MSM_ThinTranslucent))	{ OutInfo.AddShadingModel(EStrataShadingModel::SSM_DefaultLit); }
}

void UMaterial::ConvertMaterialToStrataMaterial()
{
#if WITH_EDITOR
	static const auto CVarStrata = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Strata"));
	const bool bStrataEnabled = CVarStrata ? CVarStrata->GetValueOnAnyThread() > 0 : false;

	if (!bStrataEnabled)
	{
		return;
	}

	auto MoveConnectionTo = [](auto& OldNodeInput, UMaterialExpression* NewNode, uint32 NewInputIndex)
	{
		if (OldNodeInput.IsConnected())
		{
			NewNode->GetInput(NewInputIndex)->Connect(OldNodeInput.OutputIndex, OldNodeInput.Expression);
			OldNodeInput.Expression = nullptr;
		}
	};
	auto CopyConnectionTo = [](auto& OldNodeInput, UMaterialExpression* NewNode, uint32 NewInputIndex)
	{
		if (OldNodeInput.IsConnected())
		{
			NewNode->GetInput(NewInputIndex)->Connect(OldNodeInput.OutputIndex, OldNodeInput.Expression);
		}
	};

	// SSS Profile
	const bool bHasShadingModelMixture		= ShadingModels.CountShadingModels() > 1;
	const bool bRequireSubsurfacePasses		= ShadingModels.HasShadingModel(MSM_SubsurfaceProfile) || ShadingModels.HasShadingModel(MSM_Subsurface) || ShadingModels.HasShadingModel(MSM_PreintegratedSkin) || ShadingModels.HasShadingModel(MSM_Eye);
	const bool bRequireNoSubsurfaceProfile	= !bHasShadingModelMixture && (ShadingModel == MSM_Subsurface || ShadingModel == MSM_PreintegratedSkin); // Insure there is no profile, as this would take priority otherwise

	bool bInvalidateShader = false;
	// Connect all the legacy pin into the conversion node
	if (bUseMaterialAttributes && MaterialAttributes.Expression && !MaterialAttributes.Expression->IsResultStrataMaterial(MaterialAttributes.OutputIndex)) // M_Rifle cause issues there
	{
		UMaterialExpressionBreakMaterialAttributes* BreakMatAtt = NewObject<UMaterialExpressionBreakMaterialAttributes>(this);
		MoveConnectionTo(MaterialAttributes, BreakMatAtt, 0);

		UMaterialExpressionStrataLegacyConversion* ConvertNode = NewObject<UMaterialExpressionStrataLegacyConversion>(this);
		ConvertNode->BaseColor.Connect(0, BreakMatAtt);
		ConvertNode->Metallic.Connect(1, BreakMatAtt);
		ConvertNode->Specular.Connect(2, BreakMatAtt);
		ConvertNode->Roughness.Connect(3, BreakMatAtt);
		ConvertNode->Anisotropy.Connect(4, BreakMatAtt);
		ConvertNode->EmissiveColor.Connect(5, BreakMatAtt);
		ConvertNode->Normal.Connect(8, BreakMatAtt);
		ConvertNode->Tangent.Connect(9, BreakMatAtt);
		ConvertNode->SubSurfaceColor.Connect(11, BreakMatAtt);
		ConvertNode->ClearCoat.Connect(12, BreakMatAtt);
		ConvertNode->ClearCoatRoughness.Connect(13, BreakMatAtt);
		ConvertNode->Opacity.Connect(6, BreakMatAtt);
		ConvertNode->ShadingModel.Connect(25, BreakMatAtt);
		ConvertNode->SubsurfaceProfile = bRequireNoSubsurfaceProfile ? nullptr : SubsurfaceProfile;

		// * Remove support for material attribute
		// * explicitely connect the Strata node to the root node
		// * Forward inputs to the root node (Do not reconnect the Opacity as we handle the opacity by internally within the conversion node)
		bUseMaterialAttributes = false;
		FrontMaterial.Connect(0, ConvertNode);
		OpacityMask.Connect(7, BreakMatAtt);
		WorldPositionOffset.Connect(10, BreakMatAtt);
		AmbientOcclusion.Connect(14, BreakMatAtt);
		PixelDepthOffset.Connect(24, BreakMatAtt);

		if (ShadingModel == MSM_FromMaterialExpression)
		{
			ConvertNode->ConvertedStrataMaterialInfo.SetShadingModelFromExpression(true);
			AddStrataShadingModelFromMaterialShadingModel(ConvertNode->ConvertedStrataMaterialInfo, ShadingModels);
			check(ConvertNode->ConvertedStrataMaterialInfo.CountShadingModels() >= 1);
		}
		else
		{
			check(!bHasShadingModelMixture);

			// Add constant for the shading model
			UMaterialExpressionConstant* ShadingModelNode = NewObject<UMaterialExpressionConstant>(this);
			ShadingModelNode->SetParameterName(FName(TEXT("ConstantShadingModel")));
			ShadingModelNode->R = ShadingModel;
			ConvertNode->ShadingModel.Connect(0, ShadingModelNode);

			// Store strata shading model of the converted material. 
			AddStrataShadingModelFromMaterialShadingModel(ConvertNode->ConvertedStrataMaterialInfo, ShadingModels);
			check(ConvertNode->ConvertedStrataMaterialInfo.CountShadingModels() == 1);
		}

		bInvalidateShader = true;
	}
	else if (!bUseMaterialAttributes && !FrontMaterial.IsConnected())
	{
		// STRATA_TODO for material conversion
		//  - WorldPositionOffset can remain on the end point node
		//  - Refraction
		//  - PixelDepthOffset

		if (MaterialDomain == MD_Surface)
		{
			TArray<class UMaterialExpressionCustomOutput*> CustomOutputExpressions;
			GetAllCustomOutputExpressions(CustomOutputExpressions);

			UMaterialExpressionThinTranslucentMaterialOutput* ThinTranslucentOutput = nullptr;
			UMaterialExpressionSingleLayerWaterMaterialOutput* SingleLayerWaterOutput = nullptr;
			UMaterialExpressionClearCoatNormalCustomOutput* ClearCoatOutput = nullptr;
			UMaterialExpressionTangentOutput* TangentOutput = nullptr;

			for (UMaterialExpressionCustomOutput* Expression : CustomOutputExpressions)
			{
				// Gather custom output for thin translucency
				if (ThinTranslucentOutput == nullptr && Cast<UMaterialExpressionThinTranslucentMaterialOutput>(Expression))
				{
					ThinTranslucentOutput = Cast<UMaterialExpressionThinTranslucentMaterialOutput>(Expression);
				}

				// Gather custom output for single layer water
				if (SingleLayerWaterOutput == nullptr && Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(Expression))
				{
					SingleLayerWaterOutput = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(Expression);
				}

				// Gather custom output for clear coat
				if (ClearCoatOutput == nullptr && Cast<UMaterialExpressionClearCoatNormalCustomOutput>(Expression))
				{
					ClearCoatOutput = Cast<UMaterialExpressionClearCoatNormalCustomOutput>(Expression);
				}

				// Gather custom output for tangent (unused atm)
				if (TangentOutput == nullptr && Cast<UMaterialExpressionTangentOutput>(Expression))
				{
					TangentOutput = Cast<UMaterialExpressionTangentOutput>(Expression);
				}

				if (ThinTranslucentOutput && SingleLayerWaterOutput && ClearCoatOutput && TangentOutput)
				{
					break;
				}
			}

			UMaterialExpressionStrataLegacyConversion* ConvertNode = NewObject<UMaterialExpressionStrataLegacyConversion>(this);
			ConvertNode->SubsurfaceProfile = bRequireNoSubsurfaceProfile ? nullptr : SubsurfaceProfile;
			MoveConnectionTo(BaseColor, ConvertNode, 0);	
			MoveConnectionTo(Metallic, ConvertNode, 1);		
			MoveConnectionTo(Specular, ConvertNode, 2);		 
			MoveConnectionTo(Roughness, ConvertNode, 3);
			MoveConnectionTo(Anisotropy, ConvertNode, 4);	
			MoveConnectionTo(EmissiveColor, ConvertNode, 5);
			CopyConnectionTo(Normal, ConvertNode, 6);
			MoveConnectionTo(Tangent, ConvertNode, 7);
			MoveConnectionTo(SubsurfaceColor, ConvertNode, 8);	
			MoveConnectionTo(ClearCoat, ConvertNode, 9);		
			MoveConnectionTo(ClearCoatRoughness,ConvertNode,10);
			MoveConnectionTo(Opacity, ConvertNode, 11);
			if (ThinTranslucentOutput)
			{
				MoveConnectionTo(*ThinTranslucentOutput->GetInput(0), ConvertNode, 12);	 // TransmittanceColor
			}
			if (SingleLayerWaterOutput)
			{
				MoveConnectionTo(*SingleLayerWaterOutput->GetInput(0), ConvertNode, 13); // WaterScatteringCoefficients
				MoveConnectionTo(*SingleLayerWaterOutput->GetInput(1), ConvertNode, 14); // WaterAbsorptionCoefficients
				MoveConnectionTo(*SingleLayerWaterOutput->GetInput(2), ConvertNode, 15); // WaterPhaseG
				MoveConnectionTo(*SingleLayerWaterOutput->GetInput(3), ConvertNode, 16); // ColorScaleBehindWater
			}
			if (ClearCoatOutput)
			{
				MoveConnectionTo(*ClearCoatOutput->GetInput(0), ConvertNode, 17);		 // ClearCoatNormal
			}
			
			// Shading Model
			// * either use the shader graph expression 
			// * or add a constant shading model
			// 
			// Note: store this conversion type(s) into ConvertedStrataMaterialInfo for having more context when 
			// rebuilding the final Shading model (see RebuildShadingModelField())
			if (ShadingModel == MSM_FromMaterialExpression)
			{
				check(ShadingModelFromMaterialExpression.IsConnected());

				// Reconnect the shading model expression
				MoveConnectionTo(ShadingModelFromMaterialExpression, ConvertNode, 18);

				// Store strata shading model of the converted material. 
				if (SingleLayerWaterOutput)
				{
					ConvertNode->ConvertedStrataMaterialInfo.AddShadingModel(SSM_SingleLayerWater);
				}

				ConvertNode->ConvertedStrataMaterialInfo.SetShadingModelFromExpression(true);
				AddStrataShadingModelFromMaterialShadingModel(ConvertNode->ConvertedStrataMaterialInfo, ShadingModels);
				check(ConvertNode->ConvertedStrataMaterialInfo.CountShadingModels() >= 1);
			}
			else
			{
				check(!bHasShadingModelMixture);

				// Add constant for the shading model
				UMaterialExpressionConstant* ShadingModelNode = NewObject<UMaterialExpressionConstant>(this);
				ShadingModelNode->SetParameterName(FName(TEXT("ConstantShadingModel")));
				ShadingModelNode->R = ShadingModel;
				ConvertNode->ShadingModel.Connect(0, ShadingModelNode);

				AddStrataShadingModelFromMaterialShadingModel(ConvertNode->ConvertedStrataMaterialInfo, ShadingModels);
				check(ConvertNode->ConvertedStrataMaterialInfo.CountShadingModels() == 1);
			}

			FrontMaterial.Connect(0, ConvertNode);
			bInvalidateShader = true;
		}
		else if (MaterialDomain == MD_Volume)
		{
			UMaterialExpressionStrataVolumetricFogCloudBSDF* VolBSDF = NewObject<UMaterialExpressionStrataVolumetricFogCloudBSDF>(this);
			MoveConnectionTo(BaseColor, VolBSDF, 0);		// Albedo
			MoveConnectionTo(SubsurfaceColor, VolBSDF, 1);	// Extinction
			MoveConnectionTo(EmissiveColor, VolBSDF, 2);	// EmissiveColor
			MoveConnectionTo(AmbientOcclusion, VolBSDF, 3);	// AmbientOcclusion

			// STRATA_TODO remove the VolumetricAdvancedOutput node and add the input onto FogCloudBSDF even if only used by the cloud renderer?
			FrontMaterial.Connect(0, VolBSDF);
			bInvalidateShader = true;
		}
	}

	if (bInvalidateShader)
	{
		// Now force the material to recompile and we use a hash of the original StateId.
		// This is to avoid having different StateId each time we load the material and to not forever recompile it, i.e. use a cached version.
		uint32 HashBuffer[5];
		FSHA1::HashBuffer(&StateId, sizeof(FGuid), reinterpret_cast<uint8*>(HashBuffer));
		StateId.A = HashBuffer[0];
		StateId.B = HashBuffer[1];
		StateId.C = HashBuffer[2];
		StateId.D = HashBuffer[3];
	}

	// For rebuild the shading mode since we have change it
	RebuildShadingModelField();
#endif
}

TMap<FGuid, UMaterialInterface*> LightingGuidFixupMap;

void UMaterial::PostLoad()
{
	LLM_SCOPE(ELLMTag::Materials);

	SCOPED_LOADTIMER(MaterialPostLoad);

	Super::PostLoad();

	if (FApp::CanEverRender())
	{
		// Resources can be processed / registered now that we're back on the main thread
		ProcessSerializedInlineShaderMaps(this, LoadedMaterialResources, MaterialResources);
	}
	else
	{
		// Discard all loaded material resources
		for (FMaterialResource& Resource : LoadedMaterialResources)
		{
			Resource.DiscardShaderMap();
		}		
	}
	// Empty the list of loaded resources, we don't need it anymore
	LoadedMaterialResources.Empty();

#if WITH_EDITORONLY_DATA
	const FPackageFileVersion UEVer = GetLinkerUEVersion();
	const int32 RenderObjVer = GetLinkerCustomVersion(FRenderingObjectVersion::GUID);
	const int32 UE5MainVer = GetLinkerCustomVersion(FUE5MainStreamObjectVersion::GUID);

	DoMaterialAttributeReorder(&DiffuseColor_DEPRECATED, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&SpecularColor_DEPRECATED, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&BaseColor, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Metallic, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Specular, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Roughness, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Anisotropy, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Normal, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Tangent, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&EmissiveColor, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Opacity, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&OpacityMask, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&WorldPositionOffset, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&SubsurfaceColor, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&ClearCoat, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&ClearCoatRoughness, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&AmbientOcclusion, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&Refraction, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[0], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[1], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[2], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[3], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[4], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[5], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[6], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&CustomizedUVs[7], UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&PixelDepthOffset, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&ShadingModelFromMaterialExpression, UEVer, RenderObjVer, UE5MainVer);
	DoMaterialAttributeReorder(&FrontMaterial, UEVer, RenderObjVer, UE5MainVer);
#endif // WITH_EDITORONLY_DATA

	if (!IsDefaultMaterial())
	{
		AssertDefaultMaterialsPostLoaded();
	}	

	if ( GIsEditor && GetOuter() == GetTransientPackage() && FCString::Strstr(*GetName(), TEXT("MEStatsMaterial_")))
	{
		bIsMaterialEditorStatsMaterial = true;
	}


	if( GetLinkerUEVersion() < VER_UE4_REMOVED_MATERIAL_USED_WITH_UI_FLAG && bUsedWithUI_DEPRECATED == true )
	{
		MaterialDomain = MD_UI;
	}

#if WITH_EDITORONLY_DATA
	// Ensure expressions have been postloaded before we use them for compiling
	// Any UObjects used by material compilation must be postloaded here
	for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ExpressionIndex++)
	{
		if (Expressions[ExpressionIndex])
		{
			Expressions[ExpressionIndex]->ConditionalPostLoad();
		}
	}
#endif // WITH_EDITORONLY_DATA

	// Fixup for legacy materials which didn't recreate the lighting guid properly on duplication
	if (GetLinker() && GetLinker()->UEVer() < VER_UE4_BUMPED_MATERIAL_EXPORT_GUIDS)
	{
		UMaterialInterface** ExistingMaterial = LightingGuidFixupMap.Find(GetLightingGuid());

		if (ExistingMaterial)
		{
			SetLightingGuid();
		}

		LightingGuidFixupMap.Add(GetLightingGuid(), this);
	}

	// Fix the shading model to be valid.  Loading a material saved with a shading model that has been removed will yield a MSM_MAX.
	if(ShadingModel == MSM_MAX)
	{
		ShadingModel = MSM_DefaultLit;
	}

	// Take care of loading materials that were not compiled when the shading model field existed
	if (ShadingModel != MSM_FromMaterialExpression)
	{
		ShadingModels = FMaterialShadingModelField(ShadingModel);
	}

	if(DecalBlendMode == DBM_MAX)
	{
		DecalBlendMode = DBM_Translucent;
	}

	if(bUseFullPrecision_DEPRECATED && FloatPrecisionMode == EMaterialFloatPrecisionMode::MFPM_Half)
	{
		FloatPrecisionMode = EMaterialFloatPrecisionMode::MFPM_Full;
		bUseFullPrecision_DEPRECATED = false;
	}

#if WITH_EDITOR
	// Create exec flow expressions, if needed
	CreateExecutionFlowExpressions();
	if (GIsEditor)
	{
		// Clean up any removed material expression classes	
		if (Expressions.Remove(NULL) != 0)
		{
			// Force this material to recompile because its expressions have changed
			// We're not providing a deterministic transformation guid because there could be many different ways expression
			// could change. Each conversion code removing such expression would need its own guid.
			ReleaseResourcesAndMutateDDCKey();
		}
	}
#endif // WITH_EDITOR

	if (!StateId.IsValid())
	{
		// Fixup for some legacy content
		// This path means recompiling every time the material is loaded until it is saved
		FPlatformMisc::CreateGuid(StateId);
	}

	BackwardsCompatibilityInputConversion();
	BackwardsCompatibilityVirtualTextureOutputConversion();
	BackwardsCompatibilityDecalConversion();
	ConvertMaterialToStrataMaterial();

#if WITH_EDITOR
	if ( GMaterialsThatNeedSamplerFixup.Get( this ) )
	{
		GMaterialsThatNeedSamplerFixup.Clear( this );
		const int32 ExpressionCount = Expressions.Num();
		for ( int32 ExpressionIndex = 0; ExpressionIndex < ExpressionCount; ++ExpressionIndex )
		{
			UMaterialExpressionTextureBase* TextureExpression = Cast<UMaterialExpressionTextureBase>(Expressions[ExpressionIndex]);
			if ( TextureExpression && TextureExpression->Texture )
			{
				switch( TextureExpression->Texture->CompressionSettings )
				{
				case TC_Normalmap:
					TextureExpression->SamplerType = SAMPLERTYPE_Normal;
					break;
					
				case TC_Grayscale:
					TextureExpression->SamplerType = TextureExpression->Texture->SRGB ? SAMPLERTYPE_Grayscale : SAMPLERTYPE_LinearGrayscale;
					break;

				case TC_Masks:
					TextureExpression->SamplerType = SAMPLERTYPE_Masks;
					break;

				case TC_Alpha:
					TextureExpression->SamplerType = SAMPLERTYPE_Alpha;
					break;
				default:
					TextureExpression->SamplerType = TextureExpression->Texture->SRGB ? SAMPLERTYPE_Color : SAMPLERTYPE_LinearColor;
					break;
				}
			}
		}
	}
#endif // #if WITH_EDITOR

	// needed for UMaterial as it doesn't have the InitResources() override where this is called
	PropagateDataToMaterialProxy();

#if WITH_EDITOR
	// cooked materials will not have any expressions in them, so this will obliterate the saved cached expression data
	if (!GetOutermost()->bIsCookedForEditor)
	{
		UpdateCachedExpressionData();
	}
#endif // WITH_EDITOR

	checkf(CachedExpressionData, TEXT("Missing cached expression data for material, should have been either serialized or created during PostLoad"));

	for (const FMaterialParameterCollectionInfo& CollectionInfo : CachedExpressionData->ParameterCollectionInfos)
	{
		if (CollectionInfo.ParameterCollection)
		{
			CollectionInfo.ParameterCollection->ConditionalPostLoad();
		}
	}

	STAT(double MaterialLoadTime = 0);
	{
		SCOPE_SECONDS_COUNTER(MaterialLoadTime);
// Daniel: Disable compiling shaders for cooked platforms as the cooker will manually call the BeginCacheForCookedPlatformData function and load balence
#if 0 && WITH_EDITOR
		// enable caching in postload for derived data cache commandlet and cook by the book
		ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
		if (TPM && (TPM->RestrictFormatsToRuntimeOnly() == false))
		{
			TArray<ITargetPlatform*> Platforms = TPM->GetActiveTargetPlatforms();
			// Cache for all the shader formats that the cooking target requires
			for (int32 FormatIndex = 0; FormatIndex < Platforms.Num(); FormatIndex++)
			{
				BeginCacheForCookedPlatformData(Platforms[FormatIndex]);
			}
		}
#endif
		//Don't compile shaders in post load for dev overhead materials.
		if (FApp::CanEverRender() && !bIsMaterialEditorStatsMaterial && GAllowCompilationInPostLoad)
		{
			// Before caching shader resources we have to make sure all referenced textures have been post loaded
			// as we depend on their resources being valid.
			for (UObject* Texture : CachedExpressionData->ReferencedTextures)
			{
				if (Texture)
				{
					Texture->ConditionalPostLoad();
				}
			}

			const bool bSkipCompilationOnPostLoad = IsShaderJobCacheDDCEnabled();
			if (bSkipCompilationOnPostLoad)
			{
				CacheResourceShadersForRendering(false, EMaterialShaderPrecompileMode::None);
			}
			else
			{
				CacheResourceShadersForRendering(false);
			}
		}
	}
	INC_FLOAT_STAT_BY(STAT_ShaderCompiling_MaterialLoading,(float)MaterialLoadTime);

	if( GIsEditor && !IsTemplate() )
	{
		// Ensure that the ReferencedTextureGuids array is up to date.
		UpdateLightmassTextureTracking();
	}


#if WITH_EDITOR
	if (GMaterialsThatNeedExpressionsFlipped.Get(this))
	{
		GMaterialsThatNeedExpressionsFlipped.Clear(this);
		FlipExpressionPositions(Expressions, EditorComments, true, this);
	}
	else if (GMaterialsThatNeedCoordinateCheck.Get(this))
	{
		GMaterialsThatNeedCoordinateCheck.Clear(this);
		if (HasFlippedCoordinates())
		{
			FlipExpressionPositions(Expressions, EditorComments, false, this);
		}
		FixCommentPositions(EditorComments);
	}
	else if (GMaterialsThatNeedCommentFix.Get(this))
	{
		GMaterialsThatNeedCommentFix.Clear(this);
		FixCommentPositions(EditorComments);
	}
#endif // #if WITH_EDITOR

	//DumpDebugInfo();
}

void UMaterial::DumpDebugInfo() const
{
	UE_LOG(LogConsoleResponse, Display, TEXT("----------------------------- %s"), *GetFullName());

	{
		static const UEnum* Enum = StaticEnum<EMaterialDomain>();
		check(Enum);
		UE_LOG(LogConsoleResponse, Display, TEXT("  MaterialDomain %s"), *Enum->GetNameStringByValue(int64(MaterialDomain)));
	}

	for (FMaterialResource* Resource : MaterialResources)
	{
		Resource->DumpDebugInfo();
	}
}

void UMaterial::SaveShaderStableKeys(const class ITargetPlatform* TP)
{
#if WITH_EDITOR
	FStableShaderKeyAndValue SaveKeyVal;
	SaveKeyVal.ClassNameAndObjectPath.SetCompactFullNameFromObject(this);
	SaveShaderStableKeysInner(TP, SaveKeyVal);
#endif
}

void UMaterial::SaveShaderStableKeysInner(const class ITargetPlatform* TP, const FStableShaderKeyAndValue& InSaveKeyVal)
{
#if WITH_EDITOR
	FStableShaderKeyAndValue SaveKeyVal(InSaveKeyVal);
	SaveKeyVal.MaterialDomain = FName(*MaterialDomainString(MaterialDomain));
	TArray<FMaterialResource*>* MatRes = CachedMaterialResourcesForCooking.Find(TP);
	if (MatRes)
	{
		for (FMaterialResource* Mat : *MatRes)
		{
			if (Mat)
			{
				Mat->SaveShaderStableKeys(EShaderPlatform::SP_NumPlatforms, SaveKeyVal);
			}
		}
	}
#endif
}

#if WITH_EDITOR
void UMaterial::GetShaderTypes(EShaderPlatform ShaderPlatform, const ITargetPlatform* TargetPlatform, TArray<FDebugShaderTypeInfo>& OutShaderInfo)
{
	TArray<FMaterialResource*> NewResourcesToCache;
	GetNewResources(ShaderPlatform, NewResourcesToCache);

	FPlatformTypeLayoutParameters LayoutParams;
	LayoutParams.InitializeForPlatform(TargetPlatform);

	for (FMaterialResource* Resource : NewResourcesToCache)
	{
		Resource->GetShaderTypes(ShaderPlatform, LayoutParams, OutShaderInfo);
		delete Resource;
	}

	NewResourcesToCache.Empty();
}
#endif // WITH_EDITOR

void UMaterial::PropagateDataToMaterialProxy()
{
	UpdateMaterialRenderProxy(*DefaultMaterialInstance);
}

bool UMaterial::IsCompiledWithExecutionFlow() const
{
	if (bEnableExecWire)
	{
		return CVarMaterialEnableControlFlow.GetValueOnAnyThread() != 0;
	}
	return false;
}

bool UMaterial::IsUsingNewHLSLGenerator() const
{
	if (bEnableNewHLSLGenerator)
	{
		return CVarMaterialEnableNewHLSLGenerator.GetValueOnAnyThread() != 0;
	}
	return false;
}

#if WITH_EDITOR
void UMaterial::BeginCacheForCookedPlatformData( const ITargetPlatform *TargetPlatform )
{
	TArray<FName> DesiredShaderFormats;
	TargetPlatform->GetAllTargetedShaderFormats(DesiredShaderFormats);

	TArray<FMaterialResource*> *CachedMaterialResourcesForPlatform = CachedMaterialResourcesForCooking.Find( TargetPlatform );

	if ( CachedMaterialResourcesForPlatform == NULL )
	{
		CachedMaterialResourcesForCooking.Add( TargetPlatform );
		CachedMaterialResourcesForPlatform = CachedMaterialResourcesForCooking.Find( TargetPlatform );

		check( CachedMaterialResourcesForPlatform != NULL );

		if (DesiredShaderFormats.Num())
		{
			// Cache for all the shader formats that the cooking target requires
			for (int32 FormatIndex = 0; FormatIndex < DesiredShaderFormats.Num(); FormatIndex++)
			{
				const EShaderPlatform LegacyShaderPlatform = ShaderFormatToLegacyShaderPlatform(DesiredShaderFormats[FormatIndex]);

				// Begin caching shaders for the target platform and store the material resource being compiled into CachedMaterialResourcesForCooking
				CacheResourceShadersForCooking(LegacyShaderPlatform, *CachedMaterialResourcesForPlatform, TargetPlatform);
			}
		}
	}
}

bool UMaterial::IsCachedCookedPlatformDataLoaded( const ITargetPlatform* TargetPlatform ) 
{
	const TArray<FMaterialResource*>* CachedMaterialResourcesForPlatform = CachedMaterialResourcesForCooking.Find( TargetPlatform );

	if ( CachedMaterialResourcesForPlatform != NULL ) // this should always succeed if begincacheforcookedcplatformdata is called first
	{
		for ( const auto& MaterialResource : *CachedMaterialResourcesForPlatform )
		{
			if ( MaterialResource->IsCompilationFinished() == false )
			{
				return false;
			}
		}
		return true;
	}
	return false;
}

void UMaterial::ClearCachedCookedPlatformData( const ITargetPlatform *TargetPlatform )
{
	TArray<FMaterialResource*>* CachedMaterialResourcesForPlatform = CachedMaterialResourcesForCooking.Find( TargetPlatform );
	if ( CachedMaterialResourcesForPlatform != nullptr)
	{
		FMaterial::DeferredDeleteArray(*CachedMaterialResourcesForPlatform);
	}
	CachedMaterialResourcesForCooking.Remove( TargetPlatform );
}

void UMaterial::ClearAllCachedCookedPlatformData()
{
	for ( auto& It : CachedMaterialResourcesForCooking )
	{
		TArray<FMaterialResource*>& CachedMaterialResourcesForPlatform = It.Value;
		FMaterial::DeferredDeleteArray(CachedMaterialResourcesForPlatform);
	}
	CachedMaterialResourcesForCooking.Empty();
}
#endif // WITH_EDITOR

#if WITH_EDITOR
bool UMaterial::CanEditChange(const FProperty* InProperty) const
{
	if (InProperty)
	{
		FString PropertyName = InProperty->GetName();

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, PhysMaterial) || PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, PhysMaterialMask))
		{
			return MaterialDomain == MD_Surface;
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, OpacityMaskClipValue) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, DitherOpacityMask)
			)
		{
			return BlendMode == BLEND_Masked ||
			bCastDynamicShadowAsMasked ||
			IsTranslucencyWritingCustomDepth() ||
			IsTranslucencyWritingVelocity();
		}

		if ( PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bCastDynamicShadowAsMasked) )
		{
			return BlendMode == BLEND_Translucent;
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, MaterialDecalResponse))
		{
			static auto* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DBuffer"));

			return MaterialDomain == MD_Surface && CVar->GetValueOnGameThread() > 0;
		}		

		if(MaterialDomain == MD_PostProcess)
		{
			// some settings don't make sense for postprocess materials

			if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bTangentSpaceNormal) ||
				PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bDisableDepthTest) ||
				PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUseMaterialAttributes)
				)
			{
				return false;
			}
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bFullyRough) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bNormalCurvatureToRoughness) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TwoSided) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUseLightmapDirectionality) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUseHQForwardReflections) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bForwardBlendsSkyLightCubemaps) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bMobileEnableHighQualityBRDF) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUsePlanarForwardReflections)
			)
		{
			return MaterialDomain == MD_Surface;
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, BlendableLocation) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, BlendablePriority) || 
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, BlendableOutputAlpha) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bIsBlendable) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bEnableStencilTest) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, StencilCompare) ||
			PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, StencilRefValue)
			)
		{
			return MaterialDomain == MD_PostProcess;
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, BlendMode))
		{
			return MaterialDomain == MD_DeferredDecal || MaterialDomain == MD_Surface || MaterialDomain == MD_Volume || MaterialDomain == MD_UI || (MaterialDomain == MD_PostProcess && BlendableOutputAlpha);
		}
	
		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, ShadingModel))
		{
			return MaterialDomain == MD_Surface;
		}

		if (FCString::Strncmp(*PropertyName, TEXT("bUsedWith"), 9) == 0)
		{
			return MaterialDomain == MD_DeferredDecal || MaterialDomain == MD_Surface;
		}
		else if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUsesDistortion))
		{
			return MaterialDomain == MD_DeferredDecal || MaterialDomain == MD_Surface;
		}
		else if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, RefractionDepthBias))
		{
			return Refraction.IsConnected();
		}
	
		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucencyPass)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bEnableResponsiveAA)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bScreenSpaceReflections)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bContactShadows)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bDisableDepthTest)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bUseTranslucencyVertexFog)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bComputeFogPerPixel)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bOutputTranslucentVelocity))
		{
			return MaterialDomain != MD_DeferredDecal && IsTranslucentBlendMode(BlendMode);
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bApplyCloudFogging))
		{
			const bool bApplyFogging = bUseTranslucencyVertexFog;
			return bApplyFogging && MaterialDomain != MD_DeferredDecal && IsTranslucentBlendMode(BlendMode);
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, bIsSky))
		{
			return MaterialDomain != MD_DeferredDecal && GetShadingModels().IsUnlit() && (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked);
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucencyLightingMode)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucencyDirectionalLightingIntensity)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentShadowDensityScale)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentSelfShadowDensityScale)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentSelfShadowSecondDensityScale)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentSelfShadowSecondOpacity)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentBackscatteringExponent)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentMultipleScatteringExtinction)
			|| PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, TranslucentShadowStartOffset))
		{
			return MaterialDomain != MD_DeferredDecal && IsTranslucentBlendMode(BlendMode) && GetShadingModels().IsLit();
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UMaterial, SubsurfaceProfile))
		{
			return MaterialDomain == MD_Surface && UseSubsurfaceProfile(ShadingModels) && (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked);
		}

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FLightmassMaterialInterfaceSettings, bCastShadowAsMasked))
		{
			return BlendMode != BLEND_Opaque && BlendMode != BLEND_Modulate;
		}
	}

	return true;
}

void UMaterial::CreateExecutionFlowExpressions()
{
	if (IsCompiledWithExecutionFlow())
	{
		if (!ExpressionExecBegin)
		{
			ExpressionExecBegin = NewObject<UMaterialExpressionExecBegin>(this);
			ExpressionExecBegin->Material = this;
			Expressions.Add(ExpressionExecBegin);
		}

		if (!ExpressionExecEnd)
		{
			ExpressionExecEnd = NewObject<UMaterialExpressionExecEnd>(this);
			ExpressionExecEnd->Material = this;
			Expressions.Add(ExpressionExecEnd);
		}
	}
}

void UMaterial::PreEditChange(FProperty* PropertyThatChanged)
{
	Super::PreEditChange(PropertyThatChanged);
}

void UMaterial::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	return PostEditChangePropertyInternal(PropertyChangedEvent, EPostEditChangeEffectOnShaders::Default);
}

void UMaterial::PostEditChangePropertyInternal(FPropertyChangedEvent& PropertyChangedEvent, const EPostEditChangeEffectOnShaders EffectOnShaders)
{
	// PreEditChange is not enforced to be called before PostEditChange.
	// CacheResourceShadersForRendering if called will cause a rendering thread race condition with a debug mechanism (bDeletedThroughDeferredCleanup) if there is no flush or
	// FMaterialUpdateContext present.
	FlushRenderingCommands();

	Super::PostEditChangeProperty(PropertyChangedEvent);

	FProperty* PropertyThatChanged = PropertyChangedEvent.Property;

	//Cancel any current compilation jobs that are in flight for this material.
	CancelOutstandingCompilation();

	// check for distortion in material 
	{
		bUsesDistortion = false;
		// check for a distortion value
		if (Refraction.Expression
			|| (Refraction.UseConstant && FMath::Abs(Refraction.Constant - 1.0f) >= KINDA_SMALL_NUMBER))
		{
			bUsesDistortion = true;
		}

		// check the material attributes for refraction expressions as well
		if (MaterialAttributes.Expression)
		{
			// handle make attribute expressions
			UMaterialExpressionMakeMaterialAttributes * MakeAttributeExpression = Cast<UMaterialExpressionMakeMaterialAttributes>(MaterialAttributes.Expression);
			if (MakeAttributeExpression && MakeAttributeExpression->Refraction.Expression)
			{
				bUsesDistortion = true;
			}

			// handle set attribute expressions
			UMaterialExpressionSetMaterialAttributes * SetAttributeExpression = Cast<UMaterialExpressionSetMaterialAttributes>(MaterialAttributes.Expression);
			if (SetAttributeExpression)
			{
				for (int32 Index = 0; Index < SetAttributeExpression->Inputs.Num(); Index++)
				{
					FExpressionInput & Input = SetAttributeExpression->Inputs[Index];
					FName InputName = SetAttributeExpression->GetInputName(Index);
					if (InputName == TEXT("Refraction"))
					{
						bUsesDistortion = true;
					}
				}
			}
		}
	}

	//If we can be sure this material would be the same opaque as it is masked then allow it to be assumed opaque.
	bCanMaskedBeAssumedOpaque = !OpacityMask.Expression && !(OpacityMask.UseConstant && OpacityMask.Constant < 0.999f) && !bUseMaterialAttributes;

	bool bRequiresCompilation = true;
	if( PropertyThatChanged ) 
	{
		// Don't recompile the material if we only changed the PhysMaterial property.
		if (PropertyThatChanged->GetName() == TEXT("PhysMaterial") || PropertyThatChanged->GetName() == TEXT("PhysMaterialMask") || PropertyThatChanged->GetName() == TEXT("PhysicalMaterialMap"))
		{
			bRequiresCompilation = false;
		}
	}

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMaterial, bEnableExecWire))
	{
		CreateExecutionFlowExpressions();
	}

	TranslucencyDirectionalLightingIntensity = FMath::Clamp(TranslucencyDirectionalLightingIntensity, .1f, 10.0f);

	// Don't want to recompile after a duplicate because it's just been done by PostLoad, nor during interactive changes to prevent constant recompilation while spinning properties.
	if( PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate || PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive )
	{
		bRequiresCompilation = false;
	}
	
	if (bRequiresCompilation)
	{
		UpdateCachedExpressionData();

		// When redirecting an object pointer, we trust that the DDC hash will detect the change and that we don't need to force a recompile.
		const bool bRegenerateId = PropertyChangedEvent.ChangeType != EPropertyChangeType::Redirected && EffectOnShaders != EPostEditChangeEffectOnShaders::DoesNotInvalidate;
		CacheResourceShadersForRendering(bRegenerateId, EMaterialShaderPrecompileMode::None);

		// Ensure that the ReferencedTextureGuids array is up to date.
		if (GIsEditor)
		{
			UpdateLightmassTextureTracking();
		}

		// Ensure that any components with static elements using this material have their render state recreated
		// so changes are propagated to them. The preview material is only applied to the preview mesh component,
		// and that reregister is handled by the material editor.
		if (!bIsPreviewMaterial && !bIsFunctionPreviewMaterial && !bIsMaterialEditorStatsMaterial)
		{
			FGlobalComponentRecreateRenderStateContext RecreateComponentsRenderState;
		}
	}

	// needed for UMaterial as it doesn't have the InitResources() override where this is called
	PropagateDataToMaterialProxy();

	// many property changes can require rebuild of graph so always mark as changed
	// not interested in PostEditChange calls though as the graph may have instigated it
	if (PropertyThatChanged && MaterialGraph)
	{
		MaterialGraph->NotifyGraphChanged();
	}
} 

bool UMaterial::AddExpressionParameter(UMaterialExpression* Expression, TMap<FName, TArray<UMaterialExpression*> >& ParameterTypeMap)
{
	if(Expression && Expression->HasAParameterName())
	{
		const FName ParameterName = Expression->GetParameterName();
		TArray<UMaterialExpression*>* ExpressionList = ParameterTypeMap.Find(ParameterName);
		if (!ExpressionList)
		{
			ExpressionList = &ParameterTypeMap.Add(ParameterName, TArray<UMaterialExpression*>());
		}
		ExpressionList->Add(Expression);
		return true;
	}
	return false;
}

bool UMaterial::RemoveExpressionParameter(UMaterialExpression* Expression)
{
	if (Expression && Expression->HasAParameterName())
	{
		const FName ParameterName = Expression->GetParameterName();
		TArray<UMaterialExpression*>* ExpressionList = EditorParameters.Find(ParameterName);
		if (ExpressionList)
		{
			return ExpressionList->Remove(Expression) > 0;
		}
	}
	return false;
}

bool UMaterial::IsParameter(const UMaterialExpression* Expression)
{
	return Expression->HasAParameterName();
}

bool UMaterial::IsDynamicParameter(const UMaterialExpression* Expression)
{
	if (Expression->IsA(UMaterialExpressionDynamicParameter::StaticClass()))
	{
		return true;
	}

	return false;
}

void UMaterial::BuildEditorParameterList()
{
	EditorParameters.Empty();

	for(int32 MaterialExpressionIndex = 0 ; MaterialExpressionIndex < Expressions.Num() ; ++MaterialExpressionIndex)
	{
		AddExpressionParameter(Expressions[MaterialExpressionIndex], EditorParameters);
	}
}

bool UMaterial::HasDuplicateParameters(const UMaterialExpression* Expression)
{
	FName ExpressionName;
	if(GetExpressionParameterName(Expression, ExpressionName))
	{
		const TArray<UMaterialExpression*>* ExpressionList = EditorParameters.Find(ExpressionName);
		if(ExpressionList)
		{
			const EMaterialParameterType ParameterType = Expression->GetParameterType();
			for (UMaterialExpression* CurNode : *ExpressionList)
			{
				if(CurNode != Expression && CurNode->GetParameterType() == ParameterType)
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool UMaterial::HasDuplicateDynamicParameters(const UMaterialExpression* Expression)
{
	const UMaterialExpressionDynamicParameter* DynParam = Cast<UMaterialExpressionDynamicParameter>(Expression);
	if (DynParam)
	{
		for (int32 ExpIndex = 0; ExpIndex < Expressions.Num(); ExpIndex++)
		{
			UMaterialExpressionDynamicParameter* CheckDynParam = Cast<UMaterialExpressionDynamicParameter>(Expressions[ExpIndex]);
			if (CheckDynParam != Expression)
			{
				return true;
			}
		}
	}
	return false;
}

void UMaterial::UpdateExpressionDynamicParameters(const UMaterialExpression* Expression)
{
	const UMaterialExpressionDynamicParameter* DynParam = Cast<UMaterialExpressionDynamicParameter>(Expression);
	if (DynParam)
	{
		for (int32 ExpIndex = 0; ExpIndex < Expressions.Num(); ExpIndex++)
		{
			UMaterialExpressionDynamicParameter* CheckParam = Cast<UMaterialExpressionDynamicParameter>(Expressions[ExpIndex]);
			if (CheckParam && CheckParam->CopyDynamicParameterProperties(DynParam))
			{
				CheckParam->GraphNode->ReconstructNode();
			}
		}
	}
}

void UMaterial::PropagateExpressionParameterChanges(const UMaterialExpression* Parameter)
{
	FMaterialParameterMetadata Meta;
	if (Parameter->GetParameterValue(Meta))
	{
		PropagateExpressionParameterChanges(Parameter->GetParameterName(), Meta);
	}
}

void UMaterial::PropagateExpressionParameterChanges(const FName& ParameterName, const FMaterialParameterMetadata& Meta)
{
	TArray<UMaterialExpression*>* ExpressionList = EditorParameters.Find(ParameterName);
	if (ExpressionList && ExpressionList->Num() > 1)
	{
		for (UMaterialExpression* Expression : *ExpressionList)
		{
			const EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::NoUpdateExpressionGuid | EMaterialExpressionSetParameterValueFlags::AssignGroupAndSortPriority;
			if (Expression->SetParameterValue(ParameterName, Meta, Flags))
			{
				Expression->Modify();
				Expression->Desc = Meta.Description;
				Expression->GraphNode->OnUpdateCommentText(Meta.Description);
			}
		}
	}
}

void UMaterial::UpdateExpressionParameterName(UMaterialExpression* Expression)
{
	FName ExpressionName;

	for(TMap<FName, TArray<UMaterialExpression*> >::TIterator Iter(EditorParameters); Iter; ++Iter)
	{
		if(Iter.Value().Remove(Expression) > 0)
		{
			if(Iter.Value().Num() == 0)
			{
				EditorParameters.Remove(Iter.Key());
			}

			AddExpressionParameter(Expression, EditorParameters);
			break;
		}
	}
}

void UMaterial::RebuildShadingModelField()
{
	ShadingModels.ClearShadingModels();

	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Strata"));
	const bool bStrataEnabled = CVar && CVar->GetValueOnAnyThread() > 0;
	if (bStrataEnabled && FrontMaterial.IsConnected())
	{
		FStrataMaterialInfo StrataMaterialInfo;
		check(this->FrontMaterial.Expression);
		if (this->FrontMaterial.Expression->IsResultStrataMaterial(this->FrontMaterial.OutputIndex))
		{
			this->FrontMaterial.Expression->GatherStrataMaterialInfo(StrataMaterialInfo, this->FrontMaterial.OutputIndex);
		}

		bool bSanitizeMaterial = false;
		if (!StrataMaterialInfo.IsValid())
		{
			bSanitizeMaterial = true;
			UE_LOG(LogMaterial, Error, TEXT("%s: Material information is invalid."), *GetName());
		}
		if (StrataMaterialInfo.CountShadingModels() > 1)
		{
			if (StrataMaterialInfo.HasShadingModelFromExpression())
			{
				if (BlendMode == EBlendMode::BLEND_Opaque || BlendMode == EBlendMode::BLEND_Masked)
				{
					// Nothing
				}
				else
				{
					// For transparent, we will fall back to use DefaultLit with simple volumetric
					bSanitizeMaterial = true;
				}
			}
			else if (StrataMaterialInfo.CountShadingModels() == 2 && StrataMaterialInfo.HasShadingModel(EStrataShadingModel::SSM_DefaultLit) && StrataMaterialInfo.HasShadingModel(EStrataShadingModel::SSM_SubsurfaceLit))
			{
				if (BlendMode == EBlendMode::BLEND_Opaque || BlendMode == EBlendMode::BLEND_Masked) 
				{
					// We only consider SSS subsurface post process for opaque materials)
					StrataMaterialInfo = FStrataMaterialInfo();
					StrataMaterialInfo.AddShadingModel(SSM_SubsurfaceLit);
				}
				else
				{
					// For transparent, we will fall back to use DefaultLit with simple volumetric
					bSanitizeMaterial = true;
				}
			}
			else
			{
				// Clear the material to default Lit
				bSanitizeMaterial = true;
				UE_LOG(LogMaterial, Error, TEXT("%s: Material has more than a single material represented."), *GetName());
			}
		}
		if (StrataMaterialInfo.HasOnlyShadingModel(SSM_SubsurfaceLit) && (BlendMode != EBlendMode::BLEND_Opaque && BlendMode != EBlendMode::BLEND_Masked))
		{
			// For transparent, we will fall back to use DefaultLit with simple volumetric
			bSanitizeMaterial = true;
		}
		if (bSanitizeMaterial)
		{
			StrataMaterialInfo = FStrataMaterialInfo();
			StrataMaterialInfo.AddShadingModel(SSM_DefaultLit);
		}
		
		if (StrataMaterialInfo.HasShadingModelFromExpression())
		{
			MaterialDomain = EMaterialDomain::MD_Surface;

			check(ShadingModel == MSM_FromMaterialExpression);
			{
				TArray<UMaterialExpressionShadingModel*> ShadingModelExpressions;
				GetAllExpressionsInMaterialAndFunctionsOfType(ShadingModelExpressions);

				for (UMaterialExpressionShadingModel* MatExpr : ShadingModelExpressions)
				{
					ShadingModels.AddShadingModel(MatExpr->ShadingModel);
				}

				// If no expressions have been found, set a default
				if (!ShadingModels.IsValid())
				{
					ShadingModels.AddShadingModel(MSM_DefaultLit);
				}
			}

			// Blend mode
			// Unclear what best fallback it should be
			if (BlendMode != EBlendMode::BLEND_Opaque && BlendMode != EBlendMode::BLEND_Masked)
			{
				BlendMode = EBlendMode::BLEND_Translucent; // This is to be able to use dual-source blending
			}

			// Subsurface profil
			if (StrataMaterialInfo.HasShadingModel(SSM_SubsurfaceLit) && StrataMaterialInfo.CountSubsurfaceProfiles() > 0)
			{
				if (StrataMaterialInfo.CountSubsurfaceProfiles() > 1)
				{
					UE_LOG(LogMaterial, Error, TEXT("%s: Material has more than a single sub-surface profile used."), *GetName());
				}
				SubsurfaceProfile = StrataMaterialInfo.GetSubsurfaceProfile();
			}
		}
		else
		{
			// Now derive some properties from the material
			if (StrataMaterialInfo.HasOnlyShadingModel(SSM_Unlit))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_Unlit;
				if (BlendMode != EBlendMode::BLEND_Opaque && BlendMode != EBlendMode::BLEND_Masked)
				{
					BlendMode = EBlendMode::BLEND_Translucent; // This is to be able to use dual-source blending
				}
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_SubsurfaceLit))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_SubsurfaceProfile;
				if (BlendMode != EBlendMode::BLEND_Opaque && BlendMode != EBlendMode::BLEND_Masked)
				{
					UE_LOG(LogMaterial, Error, TEXT("%s: Material has subsurface data, and its blending mode is not set to Opaque or Masked. Forcing blend mode to Opaque."), *GetName());
					BlendMode = EBlendMode::BLEND_Opaque;
				}
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_DefaultLit))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_DefaultLit;
				if (BlendMode != EBlendMode::BLEND_Opaque && BlendMode != EBlendMode::BLEND_Masked)
				{
					BlendMode = EBlendMode::BLEND_Translucent; // This is to be able to use dual-source blending
				}
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_VolumetricFogCloud))
			{
				MaterialDomain = EMaterialDomain::MD_Volume;
				ShadingModel = MSM_DefaultLit;
				BlendMode = EBlendMode::BLEND_Additive;
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_Hair))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_Hair;
				BlendMode = EBlendMode::BLEND_Opaque;
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_SingleLayerWater))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_SingleLayerWater;
				BlendMode = EBlendMode::BLEND_Opaque; // STRATA_TODO water can also be masked: check Mask input from the main node to automatically enabled that?
			}
			else if (StrataMaterialInfo.HasOnlyShadingModel(SSM_DoubleLayerWater))
			{
				MaterialDomain = EMaterialDomain::MD_Surface;
				ShadingModel = MSM_DoubleLayerWater;
				BlendMode = EBlendMode::BLEND_Opaque; // STRATA_TODO water can also be masked: check Mask input from the main node to automatically enabled that?
			}

			// Also update the ShadingModels for remaining pipeline operation
			ShadingModels.AddShadingModel(ShadingModel);

			// Subsurface profil
			if (StrataMaterialInfo.HasOnlyShadingModel(SSM_SubsurfaceLit) && StrataMaterialInfo.CountSubsurfaceProfiles() > 0)
			{
				if (StrataMaterialInfo.CountSubsurfaceProfiles() > 1)
				{
					UE_LOG(LogMaterial, Error, TEXT("%s: Material has more than a single sub-surface profile used."), *GetName());
				}
				SubsurfaceProfile = StrataMaterialInfo.GetSubsurfaceProfile();
			}
		}
	}
	// If using shading model from material expression, go through the expressions and look for the ShadingModel expression to figure out what shading models need to be supported in this material.
	// This might not be the same as what is actually compiled in to the shader, since there might be feature switches, static switches etc. that skip certain shading models.
	else if (ShadingModel == MSM_FromMaterialExpression)
	{
		TArray<UMaterialExpressionShadingModel*> ShadingModelExpressions;
		GetAllExpressionsInMaterialAndFunctionsOfType(ShadingModelExpressions);

		for (UMaterialExpressionShadingModel* MatExpr : ShadingModelExpressions)
		{
			ShadingModels.AddShadingModel(MatExpr->ShadingModel);
		}

		// If no expressions have been found, set a default
		if (!ShadingModels.IsValid())
		{
			ShadingModels.AddShadingModel(MSM_DefaultLit);
		}
	}
	else 
	{
		// If a shading model has been selected directly for the material, set it here
		ShadingModels.AddShadingModel(ShadingModel);
	}

#if WITH_EDITORONLY_DATA
	// Build a string with all the shading models on this material. Used to display the used shading models in this material
	auto ShadingModelToStringLambda = 
	[](EMaterialShadingModel InShadingModel) -> FString
	{ 
		return StaticEnum<EMaterialShadingModel>()->GetDisplayNameTextByValue(InShadingModel).ToString();
	};
	UsedShadingModels = GetShadingModelFieldString(ShadingModels, FShadingModelToStringDelegate::CreateLambda(ShadingModelToStringLambda), " | ");
#endif
}

bool UMaterial::GetExpressionParameterName(const UMaterialExpression* Expression, FName& OutName)
{
	bool bRet = false;
	if (Expression->HasAParameterName())
	{
		OutName = Expression->GetParameterName();
		bRet = true;
	}
	return bRet;
}
#endif // WITH_EDITOR

void UMaterial::BeginDestroy()
{
	TArray<TRefCountPtr<FMaterialResource>> ResourcesToDestroy;
	for (FMaterialResource* Resource : MaterialResources)
	{
		Resource->SetOwnerBeginDestroyed();
		if (Resource->PrepareDestroy_GameThread())
		{
			ResourcesToDestroy.Add(Resource);
		}
	}

	Super::BeginDestroy();

	if (DefaultMaterialInstance || ResourcesToDestroy.Num() > 0)
	{
		ReleasedByRT = false;
		FMaterialRenderProxy* LocalResource = DefaultMaterialInstance;
		FThreadSafeBool* Released = &ReleasedByRT;
		ENQUEUE_RENDER_COMMAND(BeginDestroyCommand)(
			[ResourcesToDestroy = MoveTemp(ResourcesToDestroy), LocalResource, Released](FRHICommandListImmediate& RHICmdList) mutable
		{
			if (LocalResource)
			{
				LocalResource->MarkForGarbageCollection();
				LocalResource->ReleaseResource();
			}

			for (FMaterialResource* Resource : ResourcesToDestroy)
			{
				Resource->PrepareDestroy_RenderThread();
			}

			*Released = true;
		});
	}
}

bool UMaterial::IsReadyForFinishDestroy()
{
	bool bReady = Super::IsReadyForFinishDestroy();

	return bReady && ReleasedByRT;
}

void UMaterial::ReleaseResources()
{
	for (FMaterialResource* Resource : MaterialResources)
	{
		delete Resource;
	}
	MaterialResources.Empty();
	
#if WITH_EDITOR
	if (!GExitPurge)
	{
		ClearAllCachedCookedPlatformData();
	}
#endif
	if (DefaultMaterialInstance)
	{
		DefaultMaterialInstance->GameThread_Destroy();
		DefaultMaterialInstance = nullptr;
	}
}

void UMaterial::FinishDestroy()
{
	ReleaseResources();

	Super::FinishDestroy();
}

void UMaterial::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	if (DefaultMaterialInstance)
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(sizeof(FDefaultMaterialInstance));
	}

	for (FMaterialResource* CurrentResource : MaterialResources)
	{
		CurrentResource->GetResourceSizeEx(CumulativeResourceSize);
	}
}

void UMaterial::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UMaterial* This = CastChecked<UMaterial>(InThis);

	for (FMaterialResource* CurrentResource : This->MaterialResources)
	{
		CurrentResource->AddReferencedObjects(Collector);
	}
#if WITH_EDITORONLY_DATA
	Collector.AddReferencedObject(This->MaterialGraph, This);
#endif

	Super::AddReferencedObjects(This, Collector);
}

bool UMaterial::CanBeClusterRoot() const 
{
	return true;
}

void UMaterial::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	Super::GetAssetRegistryTags(OutTags);
}

#if WITH_EDITOR
void UMaterial::CancelOutstandingCompilation()
{
	for (int32 FeatureLevel = 0; FeatureLevel < ERHIFeatureLevel::Num; ++FeatureLevel)
	{
		if (FMaterialResource* Res = GetMaterialResource((ERHIFeatureLevel::Type)FeatureLevel))
		{
			Res->CancelCompilation();
		}
	}
}
#endif

void UMaterial::UpdateMaterialShaders(TArray<const FShaderType*>& ShaderTypesToFlush, TArray<const FShaderPipelineType*>& ShaderPipelineTypesToFlush, TArray<const FVertexFactoryType*>& VFTypesToFlush, EShaderPlatform ShaderPlatform)
{
	bool bAnyMaterialShaderTypes = VFTypesToFlush.Num() > 0 || ShaderPipelineTypesToFlush.Num() > 0;

	if (!bAnyMaterialShaderTypes)
	{
		for (int32 TypeIndex = 0; TypeIndex < ShaderTypesToFlush.Num(); TypeIndex++)
		{
			if (ShaderTypesToFlush[TypeIndex]->GetMaterialShaderType() || ShaderTypesToFlush[TypeIndex]->GetMeshMaterialShaderType())
			{
				bAnyMaterialShaderTypes = true;
				break;
			}
		}
	}

	if (bAnyMaterialShaderTypes)
	{
		// Create a material update context so we can safely update materials.
		{
			FMaterialUpdateContext UpdateContext(FMaterialUpdateContext::EOptions::Default, ShaderPlatform);

			int32 NumMaterials = 0;

			for( TObjectIterator<UMaterial> It; It; ++It )
			{
				NumMaterials++;
			}

			GWarn->StatusUpdate(0, NumMaterials, NSLOCTEXT("Material", "BeginAsyncMaterialShaderCompilesTask", "Kicking off async material shader compiles..."));

			int32 UpdateStatusDivisor = FMath::Max<int32>(NumMaterials / 20, 1);
			int32 MaterialIndex = 0;

			// Reinitialize the material shader maps
			for( TObjectIterator<UMaterial> It; It; ++It )
			{
				UMaterial* BaseMaterial = *It;
				UpdateContext.AddMaterial(BaseMaterial);
				BaseMaterial->CacheResourceShadersForRendering(false);

				// Limit the frequency of progress updates
				if (MaterialIndex % UpdateStatusDivisor == 0)
				{
					GWarn->UpdateProgress(MaterialIndex, NumMaterials);
				}
				MaterialIndex++;
			}

			// The material update context will safely update all dependent material instances when
			// it leaves scope.
		}

#if WITH_EDITOR
		// Update any FMaterials not belonging to a UMaterialInterface, for example FExpressionPreviews
		// If we did not do this, the editor would crash the next time it tried to render one of those previews
		// And didn't find a shader that had been flushed for the preview's shader map.
		FMaterial::UpdateEditorLoadedMaterialResources(ShaderPlatform);
#endif
	}
}

void UMaterial::BackupMaterialShadersToMemory(TMap<FMaterialShaderMap*, TUniquePtr<TArray<uint8> > >& ShaderMapToSerializedShaderData)
{
	// Process FMaterialShaderMap's referenced by UObjects (UMaterial, UMaterialInstance)
	for (TObjectIterator<UMaterialInterface> It; It; ++It)
	{
		UMaterialInterface* Material = *It;
		UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
		UMaterial* BaseMaterial = Cast<UMaterial>(Material);

		if (MaterialInstance)
		{
			if (MaterialInstance->bHasStaticPermutationResource)
			{
				TArray<FMaterialShaderMap*> MIShaderMaps;
				MaterialInstance->GetAllShaderMaps(MIShaderMaps);

				for (int32 ShaderMapIndex = 0; ShaderMapIndex < MIShaderMaps.Num(); ShaderMapIndex++)
				{
					FMaterialShaderMap* ShaderMap = MIShaderMaps[ShaderMapIndex];

					if (ShaderMap && !ShaderMapToSerializedShaderData.Contains(ShaderMap))
					{
						TArray<uint8>* ShaderData = ShaderMap->BackupShadersToMemory();
						ShaderMapToSerializedShaderData.Emplace(ShaderMap, ShaderData);
					}
				}
			}
		}
		else if (BaseMaterial)
		{
			for (FMaterialResource* CurrentResource : BaseMaterial->MaterialResources)
			{
				FMaterialShaderMap* ShaderMap = CurrentResource->GetGameThreadShaderMap();
				if (ShaderMap && !ShaderMapToSerializedShaderData.Contains(ShaderMap))
				{
					TArray<uint8>* ShaderData = ShaderMap->BackupShadersToMemory();
					ShaderMapToSerializedShaderData.Emplace(ShaderMap, ShaderData);
				}
			}
		}
	}

#if WITH_EDITOR
	// Process FMaterialShaderMap's referenced by the editor
	FMaterial::BackupEditorLoadedMaterialShadersToMemory(ShaderMapToSerializedShaderData);
#endif
}

void UMaterial::RestoreMaterialShadersFromMemory(const TMap<FMaterialShaderMap*, TUniquePtr<TArray<uint8> > >& ShaderMapToSerializedShaderData)
{
	// Process FMaterialShaderMap's referenced by UObjects (UMaterial, UMaterialInstance)
	for (TObjectIterator<UMaterialInterface> It; It; ++It)
	{
		UMaterialInterface* Material = *It;
		UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
		UMaterial* BaseMaterial = Cast<UMaterial>(Material);

		if (MaterialInstance)
		{
			if (MaterialInstance->bHasStaticPermutationResource)
			{
				TArray<FMaterialShaderMap*> MIShaderMaps;
				MaterialInstance->GetAllShaderMaps(MIShaderMaps);

				for (int32 ShaderMapIndex = 0; ShaderMapIndex < MIShaderMaps.Num(); ShaderMapIndex++)
				{
					FMaterialShaderMap* ShaderMap = MIShaderMaps[ShaderMapIndex];

					if (ShaderMap)
					{
						const TUniquePtr<TArray<uint8> >* ShaderData = ShaderMapToSerializedShaderData.Find(ShaderMap);

						if (ShaderData)
						{
							ShaderMap->RestoreShadersFromMemory(**ShaderData);
						}
					}
				}
			}
		}
		else if (BaseMaterial)
		{
			for(FMaterialResource* CurrentResource : BaseMaterial->MaterialResources)
			{
				FMaterialShaderMap* ShaderMap = CurrentResource->GetGameThreadShaderMap();
				if (ShaderMap)
				{
					const TUniquePtr<TArray<uint8>>* ShaderData = ShaderMapToSerializedShaderData.Find(ShaderMap);

					if (ShaderData)
					{
						ShaderMap->RestoreShadersFromMemory(**ShaderData);
					}
				}
			}
		}
	}

#if WITH_EDITOR
	// Process FMaterialShaderMap's referenced by the editor
	FMaterial::RestoreEditorLoadedMaterialShadersFromMemory(ShaderMapToSerializedShaderData);
#endif // WITH_EDITOR
}

void UMaterial::CompileMaterialsForRemoteRecompile(
	const TArray<UMaterialInterface*>& MaterialsToCompile,
	EShaderPlatform ShaderPlatform,
	ITargetPlatform* TargetPlatform,
	TMap<FString, TArray<TRefCountPtr<FMaterialShaderMap> > >& OutShaderMaps)
{
	// Build a map from UMaterial / UMaterialInstance to the resources which are being compiled
	TMap<FString, TArray<FMaterialResource*> > CompilingResources;

	// compile the requested materials
	for (int32 Index = 0; Index < MaterialsToCompile.Num(); Index++)
	{
		// get the material resource from the UMaterialInterface
		UMaterialInterface* Material = MaterialsToCompile[Index];
		UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(Material);
		UMaterial* BaseMaterial = Cast<UMaterial>(Material);

		if (MaterialInstance && MaterialInstance->bHasStaticPermutationResource)
		{
			TArray<FMaterialResource*>& ResourceArray = CompilingResources.Add(Material->GetPathName(), TArray<FMaterialResource*>());
			MaterialInstance->CacheResourceShadersForCooking(ShaderPlatform, ResourceArray, EMaterialShaderPrecompileMode::Default, TargetPlatform);
		}
		else if (BaseMaterial)
		{
			TArray<FMaterialResource*>& ResourceArray = CompilingResources.Add(Material->GetPathName(), TArray<FMaterialResource*>());
			BaseMaterial->CacheResourceShadersForCooking(ShaderPlatform, ResourceArray, TargetPlatform);
		}
	}

	// Wait until all compilation is finished and all of the gathered FMaterialResources have their GameThreadShaderMap up to date
	GShaderCompilingManager->FinishAllCompilation();

	// This is heavy handed, but wait until we've set the render thread shader map before proceeding to delete the FMaterialResource below.
	// This is code that should be run on the cooker so shouldn't be a big deal.
	FlushRenderingCommands();

	for(TMap<FString, TArray<FMaterialResource*> >::TIterator It(CompilingResources); It; ++It)
	{
		TArray<FMaterialResource*>& ResourceArray = It.Value();
		TArray<TRefCountPtr<FMaterialShaderMap> >& OutShaderMapArray = OutShaderMaps.Add(It.Key(), TArray<TRefCountPtr<FMaterialShaderMap> >());

		for (int32 Index = 0; Index < ResourceArray.Num(); Index++)
		{
			FMaterialResource* CurrentResource = ResourceArray[Index];
			OutShaderMapArray.Add(CurrentResource->GetGameThreadShaderMap());
			delete CurrentResource;
		}
	}
}

bool UMaterial::UpdateLightmassTextureTracking()
{
	bool bTexturesHaveChanged = false;
#if WITH_EDITORONLY_DATA
	TArray<UTexture*> UsedTextures;
	
	GetUsedTextures(UsedTextures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);
	if (UsedTextures.Num() != ReferencedTextureGuids.Num())
	{
		bTexturesHaveChanged = true;
		// Just clear out all the guids and the code below will
		// fill them back in...
		ReferencedTextureGuids.Empty(UsedTextures.Num());
		ReferencedTextureGuids.AddZeroed(UsedTextures.Num());
	}
	
	for (int32 CheckIdx = 0; CheckIdx < UsedTextures.Num(); CheckIdx++)
	{
		UTexture* Texture = UsedTextures[CheckIdx];
		if (Texture)
		{
			if (ReferencedTextureGuids[CheckIdx] != Texture->GetLightingGuid())
			{
				ReferencedTextureGuids[CheckIdx] = Texture->GetLightingGuid();
				bTexturesHaveChanged = true;
			}
		}
		else
		{
			if (ReferencedTextureGuids[CheckIdx] != FGuid(0,0,0,0))
			{
				ReferencedTextureGuids[CheckIdx] = FGuid(0,0,0,0);
				bTexturesHaveChanged = true;
			}
		}
	}
#endif // WITH_EDITORONLY_DATA

	return bTexturesHaveChanged;
}

#if WITH_EDITOR

FExpressionInput* UMaterial::GetExpressionInputForProperty(EMaterialProperty InProperty)
{
	FMaterialInputDescription Description;
	if (GetExpressionInputDescription(InProperty, Description))
	{
		if (!Description.bHidden)
		{
			return Description.Input;
		}
	}
	return nullptr;
}

static void SetMaterialInputDescription(FColorMaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Float3;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = Input.UseConstant;
	OutDescription.bHidden = bHidden;
	const FLinearColor ConstantColor(Input.Constant);
	OutDescription.ConstantValue = UE::Shader::FValue(ConstantColor.R, ConstantColor.G, ConstantColor.B);
}

static void SetMaterialInputDescription(FVectorMaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Float3;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = Input.UseConstant;
	OutDescription.bHidden = bHidden;
	OutDescription.ConstantValue = Input.Constant;
}

static void SetMaterialInputDescription(FVector2MaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Float2;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = Input.UseConstant;
	OutDescription.bHidden = bHidden;
	OutDescription.ConstantValue = Input.Constant;
}

static void SetMaterialInputDescription(FScalarMaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Float1;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = Input.UseConstant;
	OutDescription.bHidden = bHidden;
	OutDescription.ConstantValue = Input.Constant;
}

static void SetMaterialInputDescription(FShadingModelMaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Void;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = false;
	OutDescription.bHidden = bHidden;
}

static void SetMaterialInputDescription(FMaterialAttributesInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::MaterialAttributes;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = false;
	OutDescription.bHidden = bHidden;
}

static void SetMaterialInputDescription(FStrataMaterialInput& Input, bool bHidden, FMaterialInputDescription& OutDescription)
{
	OutDescription.Type = UE::Shader::EValueType::Void;
	OutDescription.Input = &Input;
	OutDescription.bUseConstant = false;
	OutDescription.bHidden = bHidden;
}

bool UMaterial::GetExpressionInputDescription(EMaterialProperty InProperty, FMaterialInputDescription& OutDescription)
{
	switch (InProperty)
	{
	case MP_EmissiveColor: SetMaterialInputDescription(EmissiveColor, false, OutDescription); return true;
	case MP_Opacity: SetMaterialInputDescription(Opacity, false, OutDescription); return true;
	case MP_OpacityMask: SetMaterialInputDescription(OpacityMask, false, OutDescription); return true;
	case MP_BaseColor: SetMaterialInputDescription(BaseColor, false, OutDescription); return true;
	case MP_Metallic: SetMaterialInputDescription(Metallic, false, OutDescription); return true;
	case MP_Specular: SetMaterialInputDescription(Specular, false, OutDescription); return true;
	case MP_Roughness: SetMaterialInputDescription(Roughness, false, OutDescription); return true;
	case MP_Anisotropy: SetMaterialInputDescription(Anisotropy, false, OutDescription); return true;
	case MP_Normal: SetMaterialInputDescription(Normal, false, OutDescription); return true;
	case MP_Tangent: SetMaterialInputDescription(Tangent, false, OutDescription); return true;
	case MP_WorldPositionOffset: SetMaterialInputDescription(WorldPositionOffset, false, OutDescription); return true;
	case MP_SubsurfaceColor: SetMaterialInputDescription(SubsurfaceColor, false, OutDescription); return true;
	case MP_CustomData0: SetMaterialInputDescription(ClearCoat, false, OutDescription); return true;
	case MP_CustomData1: SetMaterialInputDescription(ClearCoatRoughness, false, OutDescription); return true;
	case MP_AmbientOcclusion: SetMaterialInputDescription(AmbientOcclusion, false, OutDescription); return true;
	case MP_Refraction: SetMaterialInputDescription(Refraction, false, OutDescription); return true;
	case MP_MaterialAttributes: SetMaterialInputDescription(MaterialAttributes, false, OutDescription); return true;
	case MP_PixelDepthOffset: SetMaterialInputDescription(PixelDepthOffset, false, OutDescription); return true;
	case MP_ShadingModel: SetMaterialInputDescription(ShadingModelFromMaterialExpression, false, OutDescription); return true;
	case MP_FrontMaterial: SetMaterialInputDescription(FrontMaterial, false, OutDescription); return true;
	default:
		if (InProperty >= MP_CustomizedUVs0 && InProperty <= MP_CustomizedUVs7)
		{
			SetMaterialInputDescription(CustomizedUVs[InProperty - MP_CustomizedUVs0], false, OutDescription);
			return true;
		}
		return false;
	}
}

#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA
void UMaterial::GetAllFunctionOutputExpressions(TArray<class UMaterialExpressionFunctionOutput*>& OutFunctionOutputs) const
{
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(Expression);
		if (FunctionOutput)
		{
			OutFunctionOutputs.Add(FunctionOutput);
		}
	}
}

void UMaterial::GetAllCustomOutputExpressions(TArray<class UMaterialExpressionCustomOutput*>& OutCustomOutputs) const
{
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialExpressionCustomOutput* CustomOutput = Cast<UMaterialExpressionCustomOutput>(Expression);
		if (CustomOutput)
		{
			OutCustomOutputs.Add(CustomOutput);
		}
	}
}

void UMaterial::GetAllExpressionsForCustomInterpolators(TArray<class UMaterialExpression*>& OutExpressions) const
{
	for (UMaterialExpression* Expression : Expressions)
	{
		if (Expression &&
			(Expression->IsA(UMaterialExpressionVertexInterpolator::StaticClass()) ||
			Expression->IsA(UMaterialExpressionMaterialFunctionCall::StaticClass()) ||
			Expression->IsA(UMaterialExpressionMaterialAttributeLayers::StaticClass())) )
		{
				OutExpressions.Add(Expression);
		}
	}
}
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR
bool UMaterial::GetAllReferencedExpressions(TArray<UMaterialExpression*>& OutExpressions, struct FStaticParameterSet* InStaticParameterSet,
	ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type InQuality, ERHIShadingPath::Type InShadingPath)
{
	OutExpressions.Empty();

	// For mobile only consider nodes connected to material properties that affect mobile, and exclude any custom outputs
	if (InFeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		if (bUseMaterialAttributes)
		{
			TArray<UMaterialExpression*> MPRefdExpressions;
			if (GetExpressionsInPropertyChain(MP_MaterialAttributes, MPRefdExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath) == true)
			{
				for (int32 AddIdx = 0; AddIdx < MPRefdExpressions.Num(); AddIdx++)
				{
					OutExpressions.AddUnique(MPRefdExpressions[AddIdx]);
				}
			}
		}
		else
		{
			EMaterialProperty MobileRelevantMaterialProperties[] =
			{
				MP_EmissiveColor,
				MP_OpacityMask,
				MP_BaseColor,
				MP_Normal,
				MP_WorldPositionOffset,
			};


			for (EMaterialProperty MaterialProp : MobileRelevantMaterialProperties)
			{
				TArray<UMaterialExpression*> MPRefdExpressions;
				if (GetExpressionsInPropertyChain(MaterialProp, MPRefdExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath) == true)
				{
					for (int32 AddIdx = 0; AddIdx < MPRefdExpressions.Num(); AddIdx++)
					{
						OutExpressions.AddUnique(MPRefdExpressions[AddIdx]);
					}
				}
			}
		}

		bool bMobileUseVirtualTexturing = UseVirtualTexturing(ERHIFeatureLevel::ES3_1);
		if (bMobileUseVirtualTexturing)
		{
			TArray<class UMaterialExpressionCustomOutput*> CustomOutputExpressions;
			GetAllCustomOutputExpressions(CustomOutputExpressions);
			for (UMaterialExpressionCustomOutput* Expression : CustomOutputExpressions)
			{
				if (Expression->IsA<UMaterialExpressionRuntimeVirtualTextureOutput>())
				{
					TArray<FExpressionInput*> ProcessedInputs;
					RecursiveGetExpressionChain(Expression, ProcessedInputs, OutExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath);
				}
			}
		}
	}
	else
	{
	    for (int32 MPIdx = 0; MPIdx < MP_MAX; MPIdx++)
	    {
		    EMaterialProperty MaterialProp = EMaterialProperty(MPIdx);
		    TArray<UMaterialExpression*> MPRefdExpressions;
			if (GetExpressionsInPropertyChain(MaterialProp, MPRefdExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath) == true)
			{
			    for (int32 AddIdx = 0; AddIdx < MPRefdExpressions.Num(); AddIdx++)
			    {
				    OutExpressions.AddUnique(MPRefdExpressions[AddIdx]);
			    }
		    }
	    }
    
	    TArray<class UMaterialExpressionCustomOutput*> CustomOutputExpressions;
	    GetAllCustomOutputExpressions(CustomOutputExpressions);
	    for (UMaterialExpressionCustomOutput* Expression : CustomOutputExpressions)
	    {
		    TArray<FExpressionInput*> ProcessedInputs;
			RecursiveGetExpressionChain(Expression, ProcessedInputs, OutExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath);
		}

		// If this is a material function, we want to also trace function outputs
		TArray<class UMaterialExpressionFunctionOutput*> FunctionOutputExpressions;
		GetAllFunctionOutputExpressions(FunctionOutputExpressions);
		for (UMaterialExpressionFunctionOutput* Expression : FunctionOutputExpressions)
		{
			TArray<FExpressionInput*> ProcessedInputs;
			RecursiveGetExpressionChain(Expression, ProcessedInputs, OutExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath);
		}
	}

	return true;
}


bool UMaterial::GetExpressionsInPropertyChain(EMaterialProperty InProperty, 
	TArray<UMaterialExpression*>& OutExpressions, struct FStaticParameterSet* InStaticParameterSet,
	ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type InQuality, ERHIShadingPath::Type InShadingPath)
{
	OutExpressions.Empty();
	FExpressionInput* StartingExpression = GetExpressionInputForProperty(InProperty);

	if (StartingExpression == NULL)
	{
		// Failed to find the starting expression
		return false;
	}

	TArray<FExpressionInput*> ProcessedInputs;
	if (StartingExpression->Expression)
	{
		ProcessedInputs.AddUnique(StartingExpression);
		
		EShaderFrequency ShaderFrequency = SF_NumFrequencies;
		// These properties are "special", attempting to pass them to FMaterialAttributeDefinitionMap::GetShaderFrequency() will generate log spam
		if (!(InProperty == MP_MaterialAttributes || InProperty == MP_CustomOutput))
		{
			ShaderFrequency = FMaterialAttributeDefinitionMap::GetShaderFrequency(InProperty);
		}

		RecursiveGetExpressionChain(StartingExpression->Expression, ProcessedInputs, OutExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath, ShaderFrequency);
	}
	return true;
}

bool UMaterial::GetGroupSortPriority(const FString& InGroupName, int32& OutSortPriority) const
{
	const FParameterGroupData* ParameterGroupDataElement = ParameterGroupData.FindByPredicate([&InGroupName](const FParameterGroupData& DataElement)
	{
		return InGroupName == DataElement.GroupName;
	});
	if (ParameterGroupDataElement != nullptr)
	{
		OutSortPriority = ParameterGroupDataElement->GroupSortPriority;
		return true;
	}

	return false;
}

bool UMaterial::GetTexturesInPropertyChain(EMaterialProperty InProperty, TArray<UTexture*>& OutTextures,
	TArray<FName>* OutTextureParamNames, struct FStaticParameterSet* InStaticParameterSet,
	ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type InQuality)
{
	TArray<UMaterialExpression*> ChainExpressions;
	if (GetExpressionsInPropertyChain(InProperty, ChainExpressions, InStaticParameterSet, InFeatureLevel, InQuality) == true)
	{
		// Extract the texture and texture parameter expressions...
		for (int32 ExpressionIdx = 0; ExpressionIdx < ChainExpressions.Num(); ExpressionIdx++)
		{
			UMaterialExpression* MatExp = ChainExpressions[ExpressionIdx];
			if (MatExp != NULL)
			{
				// Is it a texture sample or texture parameter sample?
				UMaterialExpressionTextureSample* TextureSampleExp = Cast<UMaterialExpressionTextureSample>(MatExp);
				if (TextureSampleExp != NULL)
				{
					// Check the default texture...
					if (TextureSampleExp->Texture != NULL)
					{
						OutTextures.Add(TextureSampleExp->Texture);
					}

					if (OutTextureParamNames != NULL)
					{
						// If the expression is a parameter, add it's name to the texture names array
						UMaterialExpressionTextureSampleParameter* TextureSampleParamExp = Cast<UMaterialExpressionTextureSampleParameter>(MatExp);
						if (TextureSampleParamExp != NULL)
						{
							OutTextureParamNames->AddUnique(TextureSampleParamExp->ParameterName);
						}
					}
				}
			}
		}
	
		return true;
	}

	return false;
}

bool UMaterial::RecursiveGetExpressionChain(UMaterialExpression* InExpression, TArray<FExpressionInput*>& InOutProcessedInputs, 
	TArray<UMaterialExpression*>& OutExpressions, struct FStaticParameterSet* InStaticParameterSet,
	ERHIFeatureLevel::Type InFeatureLevel, EMaterialQualityLevel::Type InQuality, ERHIShadingPath::Type InShadingPath, EShaderFrequency InShaderFrequency)
{
	OutExpressions.AddUnique(InExpression);
	TArray<FExpressionInput*> Inputs;
	TArray<EShaderFrequency> InputsFrequency;
	
	UMaterialExpressionFeatureLevelSwitch* FeatureLevelSwitchExp;
	UMaterialExpressionQualitySwitch* QualitySwitchExp;
	UMaterialExpressionShadingPathSwitch* ShadingPathSwitchExp;
	UMaterialExpressionMakeMaterialAttributes* MakeMaterialAttributesExp;
	UMaterialExpressionShaderStageSwitch* ShaderStageSwitchExp;

	if (InFeatureLevel != ERHIFeatureLevel::Num && (FeatureLevelSwitchExp = Cast<UMaterialExpressionFeatureLevelSwitch>(InExpression)) != nullptr)
	{
		if (FeatureLevelSwitchExp->Inputs[InFeatureLevel].IsConnected())
		{
			Inputs.Add(&FeatureLevelSwitchExp->Inputs[InFeatureLevel]);
			InputsFrequency.Add(InShaderFrequency);
		}
		else
		{
			Inputs.Add(&FeatureLevelSwitchExp->Default);
			InputsFrequency.Add(InShaderFrequency);
		}
	}
	else if (InQuality != EMaterialQualityLevel::Num && (QualitySwitchExp = Cast<UMaterialExpressionQualitySwitch>(InExpression)) != nullptr)
	{
		if (QualitySwitchExp->Inputs[InQuality].IsConnected())
		{
			Inputs.Add(&QualitySwitchExp->Inputs[InQuality]);
			InputsFrequency.Add(InShaderFrequency);
		}
		else
		{
			Inputs.Add(&QualitySwitchExp->Default);
			InputsFrequency.Add(InShaderFrequency);
		}
	}
	else if (InShadingPath != ERHIShadingPath::Num && (ShadingPathSwitchExp = Cast<UMaterialExpressionShadingPathSwitch>(InExpression)) != nullptr)
	{
		if (ShadingPathSwitchExp->Inputs[InShadingPath].IsConnected())
		{
			Inputs.Add(&ShadingPathSwitchExp->Inputs[InShadingPath]);
			InputsFrequency.Add(InShaderFrequency);
		}
		else
		{
			Inputs.Add(&ShadingPathSwitchExp->Default);
			InputsFrequency.Add(InShaderFrequency);
		}
	}
	else if (InShaderFrequency != SF_NumFrequencies && (ShaderStageSwitchExp = Cast<UMaterialExpressionShaderStageSwitch>(InExpression)) != nullptr)
	{
		if (UMaterialExpressionShaderStageSwitch::ShouldUsePixelShaderInput(InShaderFrequency))
		{
			Inputs.Add(&ShaderStageSwitchExp->PixelShader);
			InputsFrequency.Add(InShaderFrequency);
		}
		else
		{
			Inputs.Add(&ShaderStageSwitchExp->VertexShader);
			InputsFrequency.Add(InShaderFrequency);
		}
	}
	else if (InFeatureLevel <= ERHIFeatureLevel::ES3_1 && (MakeMaterialAttributesExp = Cast<UMaterialExpressionMakeMaterialAttributes>(InExpression)) != nullptr)
	{
		// Follow only mobile-relevant inputs
		Inputs.Add(&MakeMaterialAttributesExp->EmissiveColor);
		InputsFrequency.Add(SF_Pixel);
		Inputs.Add(&MakeMaterialAttributesExp->OpacityMask);
		InputsFrequency.Add(SF_Pixel);
		Inputs.Add(&MakeMaterialAttributesExp->BaseColor);
		InputsFrequency.Add(SF_Pixel);
		Inputs.Add(&MakeMaterialAttributesExp->Normal);
		InputsFrequency.Add(SF_Pixel);
		Inputs.Add(&MakeMaterialAttributesExp->WorldPositionOffset);
		InputsFrequency.Add(SF_Vertex);
	}
	else
	{
		Inputs = InExpression->GetInputs();
		
		for (FExpressionInput* Input : Inputs)
		{
			InputsFrequency.Add(InShaderFrequency);
		}
	}

	check(Inputs.Num() == InputsFrequency.Num());

	for (int32 InputIdx = 0; InputIdx < Inputs.Num(); InputIdx++)
	{
		FExpressionInput* InnerInput = Inputs[InputIdx];
		if (InnerInput != NULL)
		{
			int32 DummyIdx;
			if (InOutProcessedInputs.Find(InnerInput,DummyIdx) == false)
			{
				if (InnerInput->Expression)
				{
					bool bProcessInput = true;
					if (InStaticParameterSet != NULL)
					{
						// By default, static switches use B...
						// Is this a static switch parameter?
						//@todo. Handle Terrain weight map layer expression here as well!
						UMaterialExpressionStaticSwitchParameter* StaticSwitchExp = Cast<UMaterialExpressionStaticSwitchParameter>(InExpression);
						if (StaticSwitchExp != NULL)
						{
							bool bUseInputA = StaticSwitchExp->DefaultValue;
							FName StaticSwitchExpName = StaticSwitchExp->ParameterName;
							for (int32 CheckIdx = 0; CheckIdx < InStaticParameterSet->StaticSwitchParameters.Num(); CheckIdx++)
							{
								FStaticSwitchParameter& SwitchParam = InStaticParameterSet->StaticSwitchParameters[CheckIdx];
								if (SwitchParam.ParameterInfo.Name == StaticSwitchExpName)
								{
									// Found it...
									if (SwitchParam.bOverride == true)
									{
										bUseInputA = SwitchParam.Value;
										break;
									}
								}
							}

							if (bUseInputA == true)
							{
								if (InnerInput->Expression != StaticSwitchExp->A.Expression)
								{
									bProcessInput = false;
								}
							}
							else
							{
								if (InnerInput->Expression != StaticSwitchExp->B.Expression)
								{
									bProcessInput = false;
								}
							}
						}
					}

					if (bProcessInput == true)
					{
						InOutProcessedInputs.Add(InnerInput);
						RecursiveGetExpressionChain(InnerInput->Expression, InOutProcessedInputs, OutExpressions, InStaticParameterSet, InFeatureLevel, InQuality, InShadingPath, InputsFrequency[InputIdx]);
					}
				}
			}
		}
	}

	return true;
}
#endif // WITH_EDITOR

#if WITH_EDITOR
void UMaterial::RecursiveUpdateRealtimePreview( UMaterialExpression* InExpression, TArray<UMaterialExpression*>& InOutExpressionsToProcess )
{
	// remove ourselves from the list to process
	InOutExpressionsToProcess.Remove(InExpression);

	bool bOldRealtimePreview = InExpression->bRealtimePreview;

	// See if we know ourselves if we need realtime preview or not.
	InExpression->bRealtimePreview = InExpression->NeedsRealtimePreview();

	if( InExpression->bRealtimePreview )
	{
		if( InExpression->bRealtimePreview != bOldRealtimePreview )
		{
			InExpression->bNeedToUpdatePreview = true;
		}

		return;		
	}

	// We need to examine our inputs. If any of them need realtime preview, so do we.
	TArray<FExpressionInput*> Inputs = InExpression->GetInputs();
	for (int32 InputIdx = 0; InputIdx < Inputs.Num(); InputIdx++)
	{
		FExpressionInput* InnerInput = Inputs[InputIdx];
		if (InnerInput != NULL && InnerInput->Expression != NULL)
		{
			// See if we still need to process this expression, and if so do that first.
			if (InOutExpressionsToProcess.Find(InnerInput->Expression) != INDEX_NONE)
			{
				RecursiveUpdateRealtimePreview(InnerInput->Expression, InOutExpressionsToProcess);
			}

			// If our input expression needed realtime preview, we do too.
			if( InnerInput->Expression->bRealtimePreview )
			{

				InExpression->bRealtimePreview = true;
				if( InExpression->bRealtimePreview != bOldRealtimePreview )
				{
					InExpression->bNeedToUpdatePreview = true;
				}
				return;		
			}
		}
	}

	if( InExpression->bRealtimePreview != bOldRealtimePreview )
	{
		InExpression->bNeedToUpdatePreview = true;
	}
}
#endif // WITH_EDITOR

#if WITH_EDITOR
int32 UMaterial::CompilePropertyEx( FMaterialCompiler* Compiler, const FGuid& AttributeID )
{
	const EMaterialProperty Property = FMaterialAttributeDefinitionMap::GetProperty(AttributeID);

	if (IsCompiledWithExecutionFlow())
	{
		check(ExpressionExecBegin);
		return ExpressionExecBegin->Compile(Compiler, UMaterialExpression::CompileExecutionOutputIndex);
	}

	if( bUseMaterialAttributes && MP_DiffuseColor != Property && MP_SpecularColor != Property )
	{
		return MaterialAttributes.CompileWithDefault(Compiler, AttributeID);
	}

	switch (Property)
	{
		case MP_Opacity:				return Opacity.CompileWithDefault(Compiler, Property);
		case MP_OpacityMask:			return OpacityMask.CompileWithDefault(Compiler, Property);
		case MP_Metallic:				return Metallic.CompileWithDefault(Compiler, Property);
		case MP_Specular:				return Specular.CompileWithDefault(Compiler, Property);
		case MP_Roughness:				return Roughness.CompileWithDefault(Compiler, Property);
		case MP_Anisotropy:				return Anisotropy.CompileWithDefault(Compiler, Property);
		case MP_CustomData0:			return ClearCoat.CompileWithDefault(Compiler, Property);
		case MP_CustomData1:			return ClearCoatRoughness.CompileWithDefault(Compiler, Property);
		case MP_AmbientOcclusion:		return AmbientOcclusion.CompileWithDefault(Compiler, Property);
		case MP_Refraction:				return Refraction.CompileWithDefault(Compiler, Property);
		case MP_EmissiveColor:			return EmissiveColor.CompileWithDefault(Compiler, Property);
		case MP_BaseColor:				return BaseColor.CompileWithDefault(Compiler, Property);
		case MP_SubsurfaceColor:		return SubsurfaceColor.CompileWithDefault(Compiler, Property);
		case MP_Normal:					return Normal.CompileWithDefault(Compiler, Property);
		case MP_Tangent:				return Tangent.CompileWithDefault(Compiler, Property);
		case MP_WorldPositionOffset:	return WorldPositionOffset.CompileWithDefault(Compiler, Property);
		case MP_PixelDepthOffset:		return PixelDepthOffset.CompileWithDefault(Compiler, Property);
		case MP_ShadingModel:			return ShadingModelFromMaterialExpression.CompileWithDefault(Compiler, Property);
		case MP_FrontMaterial:			return FrontMaterial.CompileWithDefault(Compiler, Property);

		default:
			if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
			{
				const int32 TextureCoordinateIndex = Property - MP_CustomizedUVs0;

				if (TextureCoordinateIndex < NumCustomizedUVs && CustomizedUVs[TextureCoordinateIndex].Expression)
				{
					return CustomizedUVs[TextureCoordinateIndex].CompileWithDefault(Compiler, Property);
				}
				else
				{
					// The user did not customize this UV, pass through the vertex texture coordinates
					return Compiler->TextureCoordinate(TextureCoordinateIndex, false, false);
				}
			}
		
	}

	check(0);
	return INDEX_NONE;
}

bool UMaterial::ShouldForcePlanePreview()
{
	const USceneThumbnailInfoWithPrimitive* MaterialThumbnailInfo = Cast<USceneThumbnailInfoWithPrimitive>(ThumbnailInfo);
	if (!MaterialThumbnailInfo)
	{
		MaterialThumbnailInfo = USceneThumbnailInfoWithPrimitive::StaticClass()->GetDefaultObject<USceneThumbnailInfoWithPrimitive>();
	}
	// UI and particle sprite material thumbnails always get a 2D plane centered at the camera which is a better representation of the what the material will look like
	const bool bUsedWithNiagara = bUsedWithNiagaraSprites || bUsedWithNiagaraRibbons || bUsedWithNiagaraMeshParticles; 
	return Super::ShouldForcePlanePreview() || IsUIMaterial() || (bUsedWithParticleSprites && !MaterialThumbnailInfo->bUserModifiedShape) || (bUsedWithNiagara && !MaterialThumbnailInfo->bUserModifiedShape);
}

void UMaterial::NotifyCompilationFinished(UMaterialInterface* Material)
{
	UMaterial::OnMaterialCompilationFinished().Broadcast(Material);
}

void UMaterial::ForceRecompileForRendering()
{
	UpdateCachedExpressionData();
	CacheResourceShadersForRendering( false );
}

UMaterial::FMaterialCompilationFinished UMaterial::MaterialCompilationFinishedEvent;
UMaterial::FMaterialCompilationFinished& UMaterial::OnMaterialCompilationFinished()
{
	return MaterialCompilationFinishedEvent;
}
#endif // WITH_EDITOR

void UMaterial::AllMaterialsCacheResourceShadersForRendering(bool bUpdateProgressDialog)
{
#if STORE_ONLY_ACTIVE_SHADERMAPS
	TArray<UMaterial*> Materials;
	for (TObjectIterator<UMaterial> It; It; ++It)
	{
		Materials.Add(*It);
	}
	Materials.Sort([](const UMaterial& A, const UMaterial& B) { return A.OffsetToFirstResource < B.OffsetToFirstResource; });
	for (UMaterial* Material : Materials)
	{
		Material->CacheResourceShadersForRendering(false);
		FThreadHeartBeat::Get().HeartBeat();
	}
#else
#if WITH_EDITOR
	FScopedSlowTask SlowTask(100.f, NSLOCTEXT("Engine", "CacheMaterialShadersMessage", "Caching material shaders"), true);
	if (bUpdateProgressDialog)
	{
		SlowTask.Visibility = ESlowTaskVisibility::ForceVisible;
		SlowTask.MakeDialog();
	}
#endif // WITH_EDITOR

	TArray<UObject*> MaterialArray;
	GetObjectsOfClass(UMaterial::StaticClass(), MaterialArray, true, RF_ClassDefaultObject, EInternalObjectFlags::None);
	float TaskIncrement = (float)100.0f / MaterialArray.Num();

	// ensure default materials are cached first. Default materials must be available to fallback to during async compile.
 	MaterialArray.Sort([](const UObject& L, const UObject& R)
 	{
 		return ((const UMaterial&)L).IsDefaultMaterial() > ((const UMaterial&)R).IsDefaultMaterial();
	});

	for (UObject* MaterialObj : MaterialArray)
	{
		UMaterial* Material = (UMaterial*)MaterialObj;

		Material->CacheResourceShadersForRendering(false);

#if WITH_EDITOR
		if (bUpdateProgressDialog)
		{
			SlowTask.EnterProgressFrame(TaskIncrement);
		}
#endif // WITH_EDITOR
	}
#endif // STORE_ONLY_ACTIVE_SHADERMAPS
}

/**
 * Lists all materials that read from scene color.
 */
static void ListSceneColorMaterials()
{
	int32 NumSceneColorMaterials = 0;

	UMaterialInterface::IterateOverActiveFeatureLevels([&](ERHIFeatureLevel::Type FeatureLevel) 
	{
		FString FeatureLevelName;
		GetFeatureLevelName(FeatureLevel, FeatureLevelName);

		for (TObjectIterator<UMaterialInterface> It; It; ++It)
		{
			UMaterialInterface* Mat = *It;
			const FMaterial* MatRes = Mat->GetRenderProxy()->GetMaterialNoFallback(FeatureLevel);
			if (MatRes && MatRes->RequiresSceneColorCopy_GameThread())
			{
				UMaterial* BaseMat = Mat->GetMaterial();
				UE_LOG(LogConsoleResponse, Display, TEXT("[TransPass=%d][FeatureLevel=%s] %s"),
					BaseMat ? (int32)BaseMat->TranslucencyPass : (int32)MTP_MAX,
					*FeatureLevelName,
					*Mat->GetPathName()
					);
				NumSceneColorMaterials++;
			}
		}
	});
	UE_LOG(LogConsoleResponse,Display,TEXT("%d loaded materials read from scene color."),NumSceneColorMaterials);
}

static FAutoConsoleCommand CmdListSceneColorMaterials(
	TEXT("r.ListSceneColorMaterials"),
	TEXT("Lists all materials that read from scene color."),
	FConsoleCommandDelegate::CreateStatic(ListSceneColorMaterials)
	);

float UMaterial::GetOpacityMaskClipValue() const
{
	return OpacityMaskClipValue;
}

bool UMaterial::GetCastDynamicShadowAsMasked() const
{
	return bCastDynamicShadowAsMasked;
}

EBlendMode UMaterial::GetBlendMode() const
{
	if (EBlendMode(BlendMode) == BLEND_Masked)
	{
		if (bCanMaskedBeAssumedOpaque)
		{
			return BLEND_Opaque;
		}
		else
		{
			return BLEND_Masked;
		}
	}
	else
	{
		return BlendMode;
	}
}

FMaterialShadingModelField UMaterial::GetShadingModels() const
{
	switch (MaterialDomain)
	{
		case MD_Surface:
		case MD_Volume:
			return ShadingModels;
		case MD_DeferredDecal:
		case MD_RuntimeVirtualTexture:
			return MSM_DefaultLit;

		// Post process and light function materials must be rendered with the unlit model.
		case MD_PostProcess:
		case MD_LightFunction:
		case MD_UI:
			return MSM_Unlit;

		default:
			checkNoEntry();
			return MSM_Unlit;
	}
}

bool UMaterial::IsShadingModelFromMaterialExpression() const
{
	return ShadingModel == MSM_FromMaterialExpression;
}

bool UMaterial::IsTwoSided() const
{
	return TwoSided != 0;
}

bool UMaterial::IsDitheredLODTransition() const
{
	return DitheredLODTransition != 0;
}

bool UMaterial::IsTranslucencyWritingCustomDepth() const
{
	return AllowTranslucentCustomDepthWrites != 0 && IsTranslucentBlendMode(GetBlendMode());
}

bool UMaterial::IsTranslucencyWritingVelocity() const
{
	return bOutputTranslucentVelocity && IsTranslucentBlendMode(GetBlendMode());
}

bool UMaterial::IsMasked() const
{
	return GetBlendMode() == BLEND_Masked || (GetBlendMode() == BLEND_Translucent && GetCastDynamicShadowAsMasked());
}

USubsurfaceProfile* UMaterial::GetSubsurfaceProfile_Internal() const
{
	checkSlow(IsInGameThread());
	return SubsurfaceProfile; 
}

bool UMaterial::CastsRayTracedShadows() const
{
	return bCastRayTracedShadows;
}

static bool IsPropertyActive_Internal(EMaterialProperty InProperty,
	EMaterialDomain Domain,
	EBlendMode BlendMode,
	FMaterialShadingModelField ShadingModels,
	ETranslucencyLightingMode TranslucencyLightingMode,
	bool bBlendableOutputAlpha,
	bool bHasRefraction,
	bool bUsesShadingModelFromMaterialExpression,
	bool bIsTranslucencyWritingVelocity)
{
	if (Domain == MD_PostProcess)
	{
		return InProperty == MP_EmissiveColor || (bBlendableOutputAlpha && InProperty == MP_Opacity);
	}
	else if (Domain == MD_LightFunction)
	{
		// light functions should already use MSM_Unlit but we also we don't want WorldPosOffset
		return InProperty == MP_EmissiveColor;
	}
	else if (Domain == MD_DeferredDecal)
	{
		if (InProperty >= MP_CustomizedUVs0 && InProperty <= MP_CustomizedUVs7)
		{
			return true;
		}
		else if (InProperty == MP_MaterialAttributes)
		{
			// todo: MaterialAttruibutes would not return true, should it? Why we don't check for the checkbox in the material
			return true;
		}
		else if (InProperty == MP_WorldPositionOffset)
		{
			// Note: DeferredDecals don't support this but MeshDecals do
			return true;
		}

		if (BlendMode == BLEND_Translucent)
		{
			return InProperty == MP_EmissiveColor
				|| InProperty == MP_Normal
				|| InProperty == MP_Metallic
				|| InProperty == MP_Specular
				|| InProperty == MP_BaseColor
				|| InProperty == MP_Roughness
				|| InProperty == MP_Opacity
				|| InProperty == MP_AmbientOcclusion;
		}
		else if (BlendMode == BLEND_AlphaComposite)
		{
			// AlphaComposite decals never write normal.
			return InProperty == MP_EmissiveColor
				|| InProperty == MP_Metallic
				|| InProperty == MP_Specular
				|| InProperty == MP_BaseColor
				|| InProperty == MP_Roughness
				|| InProperty == MP_Opacity;
		}
		else if (BlendMode == BLEND_Modulate)
		{
			return InProperty == MP_EmissiveColor
				|| InProperty == MP_Normal
				|| InProperty == MP_Metallic
				|| InProperty == MP_Specular
				|| InProperty == MP_BaseColor
				|| InProperty == MP_Roughness
				|| InProperty == MP_Opacity;
		}
		else
		{
			return false;
		}
	}
	else if (Domain == MD_Volume)
	{
		return InProperty == MP_EmissiveColor
			|| InProperty == MP_SubsurfaceColor
			|| InProperty == MP_BaseColor
			|| InProperty == MP_AmbientOcclusion
			|| InProperty == MP_FrontMaterial;
	}
	else if (Domain == MD_UI)
	{
		return InProperty == MP_EmissiveColor
			|| (InProperty == MP_WorldPositionOffset)
			|| (InProperty == MP_OpacityMask && BlendMode == BLEND_Masked)
			|| (InProperty == MP_Opacity && IsTranslucentBlendMode(BlendMode) && BlendMode != BLEND_Modulate)
			|| (InProperty >= MP_CustomizedUVs0 && InProperty <= MP_CustomizedUVs7);
		{
			return true;
		}
	}

	const bool bIsTranslucentBlendMode = IsTranslucentBlendMode(BlendMode);
	const bool bIsNonDirectionalTranslucencyLightingMode = TranslucencyLightingMode == TLM_VolumetricNonDirectional || TranslucencyLightingMode == TLM_VolumetricPerVertexNonDirectional;
	const bool bIsVolumetricTranslucencyLightingMode = TranslucencyLightingMode == TLM_VolumetricNonDirectional
		|| TranslucencyLightingMode == TLM_VolumetricDirectional
		|| TranslucencyLightingMode == TLM_VolumetricPerVertexNonDirectional
		|| TranslucencyLightingMode == TLM_VolumetricPerVertexDirectional;
	
	bool Active = true;

	switch (InProperty)
	{
	case MP_DiffuseColor:
	case MP_SpecularColor:
		Active = false;
		break;
	case MP_Refraction:
		Active = (bIsTranslucentBlendMode && BlendMode != BLEND_AlphaHoldout && BlendMode != BLEND_Modulate) || ShadingModels.HasShadingModel(MSM_SingleLayerWater) || ShadingModels.HasShadingModel(MSM_DoubleLayerWater);
		break;
	case MP_Opacity:
		Active = (bIsTranslucentBlendMode && BlendMode != BLEND_Modulate) || ShadingModels.HasShadingModel(MSM_SingleLayerWater) || ShadingModels.HasShadingModel(MSM_DoubleLayerWater);
		if (IsSubsurfaceShadingModel(ShadingModels))
		{
			Active = true;
		}
		break;
	case MP_OpacityMask:
		Active = BlendMode == BLEND_Masked;
		break;
	case MP_BaseColor:
	case MP_AmbientOcclusion:
		Active = ShadingModels.IsLit();
		break;
	case MP_Specular:
	case MP_Roughness:
		Active = ShadingModels.IsLit() && (!bIsTranslucentBlendMode || !bIsVolumetricTranslucencyLightingMode);
		break;
	case MP_Anisotropy:
		Active = ShadingModels.HasAnyShadingModel({ MSM_DefaultLit, MSM_ClearCoat }) && (!bIsTranslucentBlendMode || !bIsVolumetricTranslucencyLightingMode);
		break;
	case MP_Metallic:
		// Subsurface models store opacity in place of Metallic in the GBuffer
		Active = ShadingModels.IsLit() && (!bIsTranslucentBlendMode || !bIsVolumetricTranslucencyLightingMode);
		break;
	case MP_Normal:
		Active = (ShadingModels.IsLit() && (!bIsTranslucentBlendMode || !bIsNonDirectionalTranslucencyLightingMode)) || bHasRefraction;
		break;
	case MP_Tangent:
		Active = ShadingModels.HasAnyShadingModel({ MSM_DefaultLit, MSM_ClearCoat }) && (!bIsTranslucentBlendMode || !bIsVolumetricTranslucencyLightingMode);
		break;
	case MP_SubsurfaceColor:
		Active = ShadingModels.HasAnyShadingModel({ MSM_Subsurface, MSM_PreintegratedSkin, MSM_TwoSidedFoliage, MSM_Cloth });
		break;
	case MP_CustomData0:
		Active = ShadingModels.HasAnyShadingModel({ MSM_ClearCoat, MSM_Hair, MSM_Cloth, MSM_Eye, MSM_SubsurfaceProfile ,MSM_DoubleLayerWater});
		break;
	case MP_CustomData1:
		Active = ShadingModels.HasAnyShadingModel({ MSM_ClearCoat, MSM_Eye ,MSM_DoubleLayerWater });
		break;
	case MP_EmissiveColor:
		// Emissive is always active, even for light functions and post process materials, 
		// but not for AlphaHoldout
		Active = BlendMode != BLEND_AlphaHoldout;
		break;
	case MP_WorldPositionOffset:
		Active = true;
		break;
	case MP_PixelDepthOffset:
		Active = (!bIsTranslucentBlendMode) || (bIsTranslucencyWritingVelocity);
		break;
	case MP_ShadingModel:
		Active = bUsesShadingModelFromMaterialExpression;
		break;
	case MP_FrontMaterial:
		{
			static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Strata"));
			const bool bStrataEnabled = CVar && CVar->GetValueOnAnyThread() > 0;
			Active = bStrataEnabled;
			break;
		}
	case MP_MaterialAttributes:
	default:
		Active = true;
		break;
	}
	return Active;
}

bool UMaterial::IsPropertyActive(EMaterialProperty InProperty) const
{
	return IsPropertyActiveInDerived(InProperty, this);
}

#if WITH_EDITOR
bool UMaterial::IsPropertyActiveInEditor(EMaterialProperty InProperty) const
{
	// explicitly DON'T use getters for BlendMode/ShadingModel...these getters may return an optimized value
	// we want the actual value that's been set by the user in the material editor
	return IsPropertyActive_Internal(InProperty,
		MaterialDomain,
		BlendMode,
		ShadingModels,
		TranslucencyLightingMode,
		BlendableOutputAlpha,
		Refraction.IsConnected(),
		IsShadingModelFromMaterialExpression(),
		IsTranslucencyWritingVelocity());
}
#endif // WITH_EDITOR

bool UMaterial::IsPropertyActiveInDerived(EMaterialProperty InProperty, const UMaterialInterface* DerivedMaterial) const
{
	return IsPropertyActive_Internal(InProperty,
		MaterialDomain,
		DerivedMaterial->GetBlendMode(),
		DerivedMaterial->GetShadingModels(),
		TranslucencyLightingMode,
		BlendableOutputAlpha,
		Refraction.IsConnected(),
		DerivedMaterial->IsShadingModelFromMaterialExpression(),
		IsTranslucencyWritingVelocity());
}

#if WITH_EDITORONLY_DATA
void UMaterial::FlipExpressionPositions(const TArray<UMaterialExpression*>& Expressions, const TArray<UMaterialExpressionComment*>& Comments, bool bScaleCoords, UMaterial* InMaterial)
{
	// Rough estimate of average increase in node size for the new editor
	const float PosScaling = bScaleCoords ? 1.25f : 1.0f;

	if (InMaterial)
	{
		InMaterial->EditorX = -InMaterial->EditorX;
	}
	for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ExpressionIndex++)
	{
		UMaterialExpression* Expression = Expressions[ExpressionIndex];
		if (Expression)
		{
			Expression->MaterialExpressionEditorX = -Expression->MaterialExpressionEditorX * PosScaling;
			Expression->MaterialExpressionEditorY *= PosScaling;
		}
	}
	for (int32 ExpressionIndex = 0; ExpressionIndex < Comments.Num(); ExpressionIndex++)
	{
		UMaterialExpressionComment* Comment = Comments[ExpressionIndex];
		if (Comment)
		{
			Comment->MaterialExpressionEditorX = (-Comment->MaterialExpressionEditorX - Comment->SizeX) * PosScaling;
			Comment->MaterialExpressionEditorY *= PosScaling;
			Comment->SizeX *= PosScaling;
			Comment->SizeY *= PosScaling;
		}
	}
}

void UMaterial::FixCommentPositions(const TArray<UMaterialExpressionComment*>& Comments)
{
	// equivalent to 1/1.25 * 0.25 to get the amount that should have been used when first flipping
	const float SizeScaling = 0.2f;

	for (int32 Index = 0; Index < Comments.Num(); Index++)
	{
		UMaterialExpressionComment* Comment = Comments[Index];
		Comment->MaterialExpressionEditorX -= Comment->SizeX * SizeScaling;
	}
}

bool UMaterial::HasFlippedCoordinates()
{
	uint32 ReversedInputCount = 0;
	uint32 StandardInputCount = 0;

	// Check inputs to see if they are right of the root node
	for (int32 InputIndex = 0; InputIndex < MP_MAX; InputIndex++)
	{
		FExpressionInput* Input = GetExpressionInputForProperty((EMaterialProperty)InputIndex);
		if (Input && Input->Expression)
		{
			if (Input->Expression->MaterialExpressionEditorX > EditorX)
			{
				++ReversedInputCount;
			}
			else
			{
				++StandardInputCount;
			}
		}
	}

	// Can't be sure coords are flipped if most are set out correctly
	return ReversedInputCount > StandardInputCount;
}
#endif //WITH_EDITORONLY_DATA

void UMaterial::GetLightingGuidChain(bool bIncludeTextures, TArray<FGuid>& OutGuids) const
{
#if WITH_EDITORONLY_DATA
	if (bIncludeTextures)
	{
		OutGuids.Append(ReferencedTextureGuids);
	}

	OutGuids.Add(StateId);
	Super::GetLightingGuidChain(bIncludeTextures, OutGuids);
#endif
}

#undef LOCTEXT_NAMESPACE
