// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	DynamicRHI.cpp: Dynamically bound Render Hardware Interface implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "Misc/MessageDialog.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Modules/ModuleManager.h"
#include "GenericPlatform/GenericPlatformDriver.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "PipelineStateCache.h"
#include "TextureProfiler.h"

#if defined(NV_GEFORCENOW) && NV_GEFORCENOW
#include "GeForceNOWWrapper.h"
#endif

IMPLEMENT_TYPE_LAYOUT(FRayTracingGeometryInitializer);
IMPLEMENT_TYPE_LAYOUT(FRayTracingGeometrySegment);

static_assert(sizeof(FRayTracingGeometryInstance) <= 96,
	"Ray tracing instance descriptor is expected to be no more than 96 bytes, "
	"as there may be a very large number of them.");

#ifndef PLATFORM_ALLOW_NULL_RHI
	#define PLATFORM_ALLOW_NULL_RHI		0
#endif

// Globals.
FDynamicRHI* GDynamicRHI = NULL;

static TAutoConsoleVariable<int32> CVarWarnOfBadDrivers(
	TEXT("r.WarnOfBadDrivers"),
	1,
	TEXT("On engine startup we can check the current GPU driver and warn the user about issues and suggest a specific version\n")
	TEXT("The test is fast so this should not cost any performance.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: a message on startup might appear (default)\n")
	TEXT(" 2: Simulating the system has a NVIDIA driver on the deny list (UI should appear)\n")
	TEXT(" 3: Simulating the system has a AMD driver on the deny list (UI should appear)\n")
	TEXT(" 4: Simulating the system has an allowed AMD driver (no UI should appear)\n")
	TEXT(" 5: Simulating the system has a Intel driver (no UI should appear)"),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarDisableDriverWarningPopupIfGFN(
	TEXT("r.DisableDriverWarningPopupIfGFN"),
	1,
	TEXT("If non-zero, disable driver version warning popup if running on a GFN cloud machine."),
	ECVF_RenderThreadSafe);

void InitNullRHI()
{
	// Use the null RHI if it was specified on the command line, or if a commandlet is running.
	IDynamicRHIModule* DynamicRHIModule = &FModuleManager::LoadModuleChecked<IDynamicRHIModule>(TEXT("NullDrv"));
	// Create the dynamic RHI.
	if ((DynamicRHIModule == 0) || !DynamicRHIModule->IsSupported())
	{
		FMessageDialog::Open( EAppMsgType::Ok, NSLOCTEXT("DynamicRHI", "NullDrvFailure", "NullDrv failure?"));
		FPlatformMisc::RequestExit(1);
	}

	GDynamicRHI = DynamicRHIModule->CreateRHI();
	GDynamicRHI->Init();

	// Command lists need the validation RHI context if enabled, so call the global scope version of RHIGetDefaultContext() and RHIGetDefaultAsyncComputeContext().
	GRHICommandList.GetImmediateCommandList().SetContext(::RHIGetDefaultContext());
	GRHICommandList.GetImmediateAsyncComputeCommandList().SetComputeContext(::RHIGetDefaultAsyncComputeContext());

	GUsingNullRHI = true;
	GRHISupportsTextureStreaming = false;

	// Update the crash context analytics
	FGenericCrashContext::SetEngineData(TEXT("RHI.RHIName"), TEXT("NullRHI"));
}

#if PLATFORM_WINDOWS || PLATFORM_UNIX
static void RHIDetectAndWarnOfBadDrivers(bool bHasEditorToken)
{
	if (!GIsRHIInitialized || (GRHIVendorId == 0))
	{
		UE_LOG(LogRHI, Log, TEXT("Skipping Driver Check: RHI%s initialized, VendorId=0x%x"), GIsRHIInitialized ? TEXT("") : TEXT(" NOT"), GRHIVendorId);
		return;
	}

	int32 WarnMode = CVarWarnOfBadDrivers.GetValueOnGameThread();

	FGPUDriverInfo DriverInfo;

	// later we should make the globals use the struct directly
	DriverInfo.VendorId = GRHIVendorId;
	DriverInfo.DeviceDescription = GRHIAdapterName;
	DriverInfo.ProviderName = TEXT("Unknown");
	DriverInfo.InternalDriverVersion = GRHIAdapterInternalDriverVersion;
	DriverInfo.UserDriverVersion = GRHIAdapterUserDriverVersion;
	DriverInfo.DriverDate = GRHIAdapterDriverDate;
	DriverInfo.RHIName = GDynamicRHI ? GDynamicRHI->GetName() : FString();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// for testing
	if(WarnMode == 2)
	{
		DriverInfo.SetNVIDIA();
		DriverInfo.DeviceDescription = TEXT("Test NVIDIA (bad)");
		DriverInfo.UserDriverVersion = TEXT("346.43");
		DriverInfo.InternalDriverVersion = TEXT("9.18.134.643");
		DriverInfo.DriverDate = TEXT("01-01-1900");
	}
	else if(WarnMode == 3)
	{
		DriverInfo.SetAMD();
		DriverInfo.DeviceDescription = TEXT("Test AMD (bad)");
		DriverInfo.UserDriverVersion = TEXT("Test Catalyst Version");
		DriverInfo.InternalDriverVersion = TEXT("13.152.1.1000");
		DriverInfo.DriverDate = TEXT("09-10-13");
	}
	else if(WarnMode == 4)
	{
		DriverInfo.SetAMD();
		DriverInfo.DeviceDescription = TEXT("Test AMD (good)");
		DriverInfo.UserDriverVersion = TEXT("Test Catalyst Version");
		DriverInfo.InternalDriverVersion = TEXT("15.30.1025.1001");
		DriverInfo.DriverDate = TEXT("01-01-16");
	}
	else if(WarnMode == 5)
	{
		DriverInfo.SetIntel();
		DriverInfo.DeviceDescription = TEXT("Test Intel (good)");
		DriverInfo.UserDriverVersion = TEXT("Test Intel Version");
		DriverInfo.InternalDriverVersion = TEXT("8.15.10.2302");
		DriverInfo.DriverDate = TEXT("01-01-15");
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	FGPUHardware DetectedGPUHardware(DriverInfo);

	// Pre-GCN GPUs usually don't support updating to latest driver
	// But it is unclear what is the latest version supported as it varies from card to card
	// So just don't complain if pre-gcn
	if (DriverInfo.IsValid() && !GRHIDeviceIsAMDPreGCNArchitecture)
	{
		FDriverDenyListEntry DenyListEntry = DetectedGPUHardware.FindDriverDenyListEntry();

		GRHIAdapterDriverOnDenyList = DenyListEntry.IsValid();
		FGenericCrashContext::SetEngineData(TEXT("RHI.DriverBlacklisted"), DenyListEntry.IsValid() ? TEXT("true") : TEXT("false"));

		if (GRHIAdapterDriverOnDenyList)
		{
			if (!FApp::IsUnattended() && WarnMode != 0)
			{
				bool bLatestDenied = DetectedGPUHardware.IsLatestDenied();

				// Note: we don't localize the vendor's name.
				FString VendorString = DriverInfo.ProviderName;
				FText HyperlinkText;
				if (DriverInfo.IsNVIDIA())
				{
					VendorString = TEXT("NVIDIA");
					HyperlinkText = NSLOCTEXT("MessageDialog", "DriverDownloadLinkNVIDIA", "https://www.nvidia.com/en-us/geforce/drivers/");
				}
				else if (DriverInfo.IsAMD())
				{
					VendorString = TEXT("AMD");
					HyperlinkText = NSLOCTEXT("MessageDialog", "DriverDownloadLinkAMD", "https://www.amd.com/en/support");
				}
				else if (DriverInfo.IsIntel())
				{
					VendorString = TEXT("Intel");
					HyperlinkText = NSLOCTEXT("MessageDialog", "DriverDownloadLinkIntel", "https://downloadcenter.intel.com/product/80939/Graphics");
				}

				// format message box UI
				FFormatNamedArguments Args;
				Args.Add(TEXT("AdapterName"), FText::FromString(DriverInfo.DeviceDescription));
				Args.Add(TEXT("Vendor"), FText::FromString(VendorString));
				Args.Add(TEXT("RHI"), FText::FromString(DenyListEntry.RHIName));
				Args.Add(TEXT("Hyperlink"), HyperlinkText);
				Args.Add(TEXT("RecommendedVer"), FText::FromString(DetectedGPUHardware.GetSuggestedDriverVersion(DriverInfo.RHIName)));
				Args.Add(TEXT("InstalledVer"), FText::FromString(DriverInfo.UserDriverVersion));

				FText LocalizedMsg;
				if (bLatestDenied)
				{
					if (!DenyListEntry.RHIName.IsEmpty())
					{
						LocalizedMsg = FText::Format(NSLOCTEXT("MessageDialog", "LatestVideoCardDriverRHIIssueReport", "The latest version of the {Vendor} graphics driver has known issues in {RHI}.\nPlease install the recommended driver version or switch to a different rendering API.\n\n{Hyperlink}\n\n{AdapterName}\nInstalled: {InstalledVer}\nRecommended: {RecommendedVer}"), Args);
					}
					else
					{
						LocalizedMsg = FText::Format(NSLOCTEXT("MessageDialog", "LatestVideoCardDriverIssueReport", "The latest version of the {Vendor} graphics driver has known issues.\nPlease install the recommended driver version.\n\n{Hyperlink}\n\n{AdapterName}\nInstalled: {InstalledVer}\nRecommended: {RecommendedVer}"), Args);
					}
				}
				else
				{
					if (!DenyListEntry.RHIName.IsEmpty())
					{
						LocalizedMsg = FText::Format(NSLOCTEXT("MessageDialog", "VideoCardDriverRHIIssueReport", "The installed version of the {Vendor} graphics driver has known issues in {RHI}.\nPlease install either the latest or the recommended driver version or switch to a different rendering API.\n\n{Hyperlink}\n\n{AdapterName}\nInstalled: {InstalledVer}\nRecommended: {RecommendedVer}"), Args);
					}
					else
					{
						LocalizedMsg = FText::Format(NSLOCTEXT("MessageDialog", "VideoCardDriverIssueReport", "The installed version of the {Vendor} graphics driver has known issues.\nPlease install either the latest or the recommended driver version.\n\n{Hyperlink}\n\n{AdapterName}\nInstalled: {InstalledVer}\nRecommended: {RecommendedVer}"), Args);
					}
				}

				FText Title = NSLOCTEXT("MessageDialog", "TitleVideoCardDriverIssue", "WARNING: Known issues with graphics driver");
				FMessageDialog::Open(EAppMsgType::Ok, LocalizedMsg, &Title);
			}
			else
			{
				UE_LOG(LogRHI, Warning, TEXT("Running with bad GPU drivers but warning dialog will not be shown: IsUnattended=%d, r.WarnOfBadDrivers=%d"), FApp::IsUnattended(), WarnMode);
			}
		}
	}
}
#elif PLATFORM_MAC
static void RHIDetectAndWarnOfBadDrivers(bool bHasEditorToken)
{
	int32 CVarValue = CVarWarnOfBadDrivers.GetValueOnGameThread();

	if (!GIsRHIInitialized || !CVarValue || GRHIVendorId == 0 || bHasEditorToken || FApp::IsUnattended())
	{
		return;
	}

	if (FPlatformMisc::MacOSXVersionCompare(10,15,5) < 0)
	{
		// this message can be suppressed with r.WarnOfBadDrivers=0
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok,
									 *NSLOCTEXT("MessageDialog", "UpdateMacOSX_Body", "Please update to the latest version of macOS for best performance and stability.").ToString(),
									 *NSLOCTEXT("MessageDialog", "UpdateMacOSX_Title", "Update macOS").ToString());
	}
}
#endif // PLATFORM_WINDOWS

void RHIInit(bool bHasEditorToken)
{
	if (!GDynamicRHI)
	{
#if RHI_ENABLE_RESOURCE_INFO
		FRHIResource::StartTrackingAllResources();
#endif

		// read in any data driven shader platform info structures we can find
		FGenericDataDrivenShaderPlatformInfo::Initialize();

		GRHICommandList.LatchBypass(); // read commandline for bypass flag

		if (!FApp::CanEverRender())
		{
			InitNullRHI();
		}
		else
		{
			LLM_SCOPE(ELLMTag::RHIMisc);

			GDynamicRHI = PlatformCreateDynamicRHI();
			if (GDynamicRHI)
			{
				GDynamicRHI->Init();

#if WITH_MGPU
				AFRUtils::StaticInitialize();
#endif

				// Validation of contexts.
				GRHICommandList.GetImmediateCommandList().GetContext();
				GRHICommandList.GetImmediateAsyncComputeCommandList().GetComputeContext();
				check(GIsRHIInitialized);

				// Set default GPU mask to all GPUs. This is necessary to ensure that any commands
				// that create and initialize resources are executed on all GPUs. Scene rendering
				// will restrict itself to a subset of GPUs as needed.
				GRHICommandList.GetImmediateCommandList().SetGPUMask(FRHIGPUMask::All());
				GRHICommandList.GetImmediateAsyncComputeCommandList().SetGPUMask(FRHIGPUMask::All());

				FString FeatureLevelString;
				GetFeatureLevelName(GMaxRHIFeatureLevel, FeatureLevelString);

				if (bHasEditorToken && GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
				{
					FString ShaderPlatformString = LegacyShaderPlatformToShaderFormat(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel)).ToString();
					FString Error = FString::Printf(TEXT("A Feature Level 5 video card is required to run the editor.\nAvailableFeatureLevel = %s, ShaderPlatform = %s"), *FeatureLevelString, *ShaderPlatformString);
					FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
					FPlatformMisc::RequestExit(1);
				}

				// Update the crash context analytics
				FGenericCrashContext::SetEngineData(TEXT("RHI.RHIName"), GDynamicRHI ? (GMaxRHIFeatureLevel == ERHIFeatureLevel::ES3_1 ? FString(GDynamicRHI->GetName()) + TEXT("_ES31") : GDynamicRHI->GetName()) : TEXT("Unknown"));
				FGenericCrashContext::SetEngineData(TEXT("RHI.AdapterName"), GRHIAdapterName);
				FGenericCrashContext::SetEngineData(TEXT("RHI.UserDriverVersion"), GRHIAdapterUserDriverVersion);
				FGenericCrashContext::SetEngineData(TEXT("RHI.InternalDriverVersion"), GRHIAdapterInternalDriverVersion);
				FGenericCrashContext::SetEngineData(TEXT("RHI.DriverDate"), GRHIAdapterDriverDate);
				FGenericCrashContext::SetEngineData(TEXT("RHI.FeatureLevel"), FeatureLevelString);
				FGenericCrashContext::SetEngineData(TEXT("RHI.GPUVendor"), RHIVendorIdToString());

#if TEXTURE_PROFILER_ENABLED
				FTextureProfiler::Get()->Init();
#endif
			}
#if PLATFORM_ALLOW_NULL_RHI
			else
			{
				// If the platform supports doing so, fall back to the NULL RHI on failure
				InitNullRHI();
			}
#endif
		}

		check(GDynamicRHI);
	}

#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_UNIX
#if defined(NV_GEFORCENOW) && NV_GEFORCENOW
	bool bDetectAndWarnBadDrivers = true;
	if (IsRHIDeviceNVIDIA() && !!CVarDisableDriverWarningPopupIfGFN.GetValueOnAnyThread())
	{
		const GfnRuntimeError GfnResult = GeForceNOWWrapper::Get().Initialize();
		const bool bGfnRuntimeSDKInitialized = GfnResult == gfnSuccess || GfnResult == gfnInitSuccessClientOnly;
		if (bGfnRuntimeSDKInitialized)
		{
			UE_LOG(LogRHI, Log, TEXT("GeForceNow SDK initialized: %d"), (int32)GfnResult);
		}
		else
		{
			UE_LOG(LogRHI, Log, TEXT("GeForceNow SDK initialization failed: %d"), (int32)GfnResult);
		}

		// Don't pop up a driver version warning window when running on a cloud machine
		bDetectAndWarnBadDrivers = !bGfnRuntimeSDKInitialized || !GeForceNOWWrapper::Get().IsRunningInCloud();

		if (GeForceNOWWrapper::Get().IsRunningInGFN())
		{
			FGenericCrashContext::SetEngineData(TEXT("RHI.CloudInstance"), TEXT("GeForceNow"));
		}
	}

	if (bDetectAndWarnBadDrivers)
	{
		RHIDetectAndWarnOfBadDrivers(bHasEditorToken);
	}
#else
	RHIDetectAndWarnOfBadDrivers(bHasEditorToken);
#endif
#endif
}

void RHIPostInit(const TArray<uint32>& InPixelFormatByteWidth)
{
	check(GDynamicRHI);
	GDynamicRHI->InitPixelFormatInfo(InPixelFormatByteWidth);
	GDynamicRHI->PostInit();
}

void RHIExit()
{
	if (!GUsingNullRHI && GDynamicRHI != NULL)
	{
		// Clean up all cached pipelines
		PipelineStateCache::Shutdown();

		// Flush any potential commands queued before we shut things down.
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);

		// Destruct the dynamic RHI.
		GDynamicRHI->Shutdown();
		delete GDynamicRHI;
		GDynamicRHI = NULL;

#if RHI_ENABLE_RESOURCE_INFO
		FRHIResource::StopTrackingAllResources();
#endif
	}
	else if (GUsingNullRHI)
	{
		// If we are using NullRHI flush the command list here in case somethings has been added to the command list during exit calls
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	}
}


static void BaseRHISetGPUCaptureOptions(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() > 0)
	{
		const bool bEnabled = Args[0].ToBool();
		GDynamicRHI->EnableIdealGPUCaptureOptions(bEnabled);
	}
	else
	{
		UE_LOG(LogRHI, Display, TEXT("Usage: r.RHISetGPUCaptureOptions 0 or r.RHISetGPUCaptureOptions 1"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GBaseRHISetGPUCaptureOptions(
	TEXT("r.RHISetGPUCaptureOptions"),
	TEXT("Utility function to change multiple CVARs useful when profiling or debugging GPU rendering. Setting to 1 or 0 will guarantee all options are in the appropriate state.\n")
	TEXT("r.rhithread.enable, r.rhicmdbypass, r.showmaterialdrawevents, toggledrawevents\n")
	TEXT("Platform RHI's may implement more feature toggles."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&BaseRHISetGPUCaptureOptions)
	);

void FDynamicRHI::RHIReadSurfaceFloatData(FRHITexture* Texture, FIntRect Rect, TArray<FFloat16Color>& OutData, FReadSurfaceDataFlags InFlags)
{
#if WITH_MGPU
	if (InFlags.GetGPUIndex() != 0)
	{
		unimplemented();
	}
	else
#endif
	{
		RHIReadSurfaceFloatData(Texture, Rect, OutData, InFlags.GetCubeFace(), InFlags.GetArrayIndex(), InFlags.GetMip());
	}
}

void FDynamicRHI::RHIRead3DSurfaceFloatData(FRHITexture* Texture, FIntRect Rect, FIntPoint ZMinMax, TArray<FFloat16Color>& OutData, FReadSurfaceDataFlags InFlags)
{
#if WITH_MGPU
	if (InFlags.GetGPUIndex() != 0)
	{
		unimplemented();
	}
	else
#endif
	{
		RHIRead3DSurfaceFloatData(Texture, Rect, ZMinMax, OutData);
	}
}

void FDynamicRHI::EnableIdealGPUCaptureOptions(bool bEnabled)
{
	static IConsoleVariable* RHICmdBypassVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.rhicmdbypass"));
	static IConsoleVariable* ShowMaterialDrawEventVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShowMaterialDrawEvents"));	
	static IConsoleObject* RHIThreadEnableObj = IConsoleManager::Get().FindConsoleObject(TEXT("r.RHIThread.Enable"));
	static IConsoleCommand* RHIThreadEnableCommand = RHIThreadEnableObj ? RHIThreadEnableObj->AsCommand() : nullptr;

	const bool bShouldEnableDrawEvents = bEnabled;
	const bool bShouldEnableMaterialDrawEvents = bEnabled;
	const bool bShouldEnableRHIThread = !bEnabled;
	const bool bShouldRHICmdBypass = bEnabled;	

	const bool bDrawEvents = GetEmitDrawEvents() != 0;
	const bool bMaterialDrawEvents = ShowMaterialDrawEventVar ? ShowMaterialDrawEventVar->GetInt() != 0 : false;
	const bool bRHIThread = IsRunningRHIInSeparateThread();
	const bool bRHIBypass = RHICmdBypassVar ? RHICmdBypassVar->GetInt() != 0 : false;

	UE_LOG(LogRHI, Display, TEXT("Setting GPU Capture Options: %i"), bEnabled ? 1 : 0);
	if (bShouldEnableDrawEvents != bDrawEvents)
	{
		UE_LOG(LogRHI, Display, TEXT("Toggling draw events: %i"), bShouldEnableDrawEvents ? 1 : 0);
		SetEmitDrawEvents(bShouldEnableDrawEvents);
	}
	if (bShouldEnableMaterialDrawEvents != bMaterialDrawEvents && ShowMaterialDrawEventVar)
	{
		UE_LOG(LogRHI, Display, TEXT("Toggling showmaterialdrawevents: %i"), bShouldEnableDrawEvents ? 1 : 0);
		ShowMaterialDrawEventVar->Set(bShouldEnableDrawEvents ? -1 : 0);		
	}
	if (bRHIThread != bShouldEnableRHIThread && RHIThreadEnableCommand)
	{
		UE_LOG(LogRHI, Display, TEXT("Toggling rhi thread: %i"), bShouldEnableRHIThread ? 1 : 0);
		TArray<FString> Args;
		Args.Add(FString::Printf(TEXT("%i"), bShouldEnableRHIThread ? 1 : 0));
		RHIThreadEnableCommand->Execute(Args, nullptr, *GLog);
	}
	if (bRHIBypass != bShouldRHICmdBypass && RHICmdBypassVar)
	{
		UE_LOG(LogRHI, Display, TEXT("Toggling rhi bypass: %i"), bEnabled ? 1 : 0);
		RHICmdBypassVar->Set(bShouldRHICmdBypass ? 1 : 0, ECVF_SetByConsole);		
	}	
}

void FDynamicRHI::RHITransferBufferUnderlyingResource(FRHIBuffer* DestBuffer, FRHIBuffer* SrcBuffer)
{
	UE_LOG(LogRHI, Fatal, TEXT("RHITransferBufferUnderlyingResource isn't implemented for the current RHI"));
}

FUnorderedAccessViewRHIRef FDynamicRHI::RHICreateUnorderedAccessView(FRHITexture* Texture, uint32 MipLevel)
{
	return RHICreateUnorderedAccessView(Texture, MipLevel, 0, 0);
}

FUnorderedAccessViewRHIRef FDynamicRHI::RHICreateUnorderedAccessView(FRHITexture* Texture, uint32 MipLevel, uint8 Format)
{
	return RHICreateUnorderedAccessView(Texture, MipLevel, Format, 0, 0);
}

FUnorderedAccessViewRHIRef FDynamicRHI::RHICreateUnorderedAccessView(FRHITexture* Texture, uint32 MipLevel, uint8 Format, uint16 FirstArraySlice, uint16 NumArraySlices)
{
	UE_LOG(LogRHI, Fatal, TEXT("RHICreateUnorderedAccessView with Format parameter isn't implemented for the current RHI"));
	return RHICreateUnorderedAccessView(Texture, MipLevel, FirstArraySlice, NumArraySlices);
}

void FDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* Buffer, uint32 Stride, uint8 Format)
{
	UE_LOG(LogRHI, Fatal, TEXT("RHIUpdateShaderResourceView isn't implemented for the current RHI"));
}

void FDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* Buffer)
{
	UE_LOG(LogRHI, Fatal, TEXT("RHIUpdateShaderResourceView isn't implemented for the current RHI"));
}

uint64 FDynamicRHI::RHIGetMinimumAlignmentForBufferBackedSRV(EPixelFormat Format)
{
	return 1;
}

uint64 FDynamicRHI::RHICalcTexture2DArrayPlatformSize(uint32 SizeX, uint32 SizeY, uint32 ArraySize, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	// ensureMsgf(false, TEXT("RHICalcTexture2DArrayPlatformSize isn't implemented for the current RHI"));
	return ArraySize * RHICalcTexture2DPlatformSize(SizeX, SizeY, Format, NumMips, NumSamples, Flags, CreateInfo, OutAlign);
}

uint64 FDynamicRHI::RHICalcVMTexture2DPlatformSize(uint32 Mip0Width, uint32 Mip0Height, uint8 Format, uint32 NumMips, uint32 FirstMipIdx, uint32 NumSamples, ETextureCreateFlags Flags, uint32& OutAlign)
{
	UE_LOG(LogRHI, Fatal, TEXT("RHICalcVMTexture2DPlatformSize isn't implemented for the current RHI"));
	return -1;
}

FDefaultRHIRenderQueryPool::FDefaultRHIRenderQueryPool(ERenderQueryType InQueryType, FDynamicRHI* InDynamicRHI, uint32 InNumQueries)
	: DynamicRHI(InDynamicRHI)
	, QueryType(InQueryType)
	, NumQueries(InNumQueries)
{
	if (NumQueries != UINT32_MAX && (GSupportsTimestampRenderQueries || InQueryType != RQT_AbsoluteTime))
	{
		Queries.Reserve(NumQueries);
		for (uint32 i = 0; i < NumQueries; i++)
		{
			Queries.Push(DynamicRHI->RHICreateRenderQuery(QueryType));
			check(Queries.Last().IsValid());
			++AllocatedQueries;
		}
	}
}

FDefaultRHIRenderQueryPool::~FDefaultRHIRenderQueryPool()
{
	check(IsInRHIThread() || IsInRenderingThread());
	checkf(AllocatedQueries == Queries.Num(), TEXT("Querypool deleted before all Queries have been released"));
}

FRHIPooledRenderQuery FDefaultRHIRenderQueryPool::AllocateQuery()
{
	check(IsInParallelRenderingThread());
	if (Queries.Num() > 0)
	{
		return FRHIPooledRenderQuery(this, Queries.Pop());
	}
	else
	{
		FRHIPooledRenderQuery Query = FRHIPooledRenderQuery(this, DynamicRHI->RHICreateRenderQuery(QueryType));
		if (Query.IsValid())
		{
			++AllocatedQueries;
		}
		ensure(AllocatedQueries <= NumQueries);
		return Query;
	}
}

void FDefaultRHIRenderQueryPool::ReleaseQuery(TRefCountPtr<FRHIRenderQuery>&& Query)
{
	if (QueryType == ERenderQueryType::RQT_Occlusion)
	{
		static int dbg = 0;
		dbg++;
	}
	check(IsInParallelRenderingThread());
	//Hard to validate because of Resource resurrection, better to remove GetQueryRef entirely
	//checkf(Query.IsValid() && Query.GetRefCount() <= 2, TEXT("Query has been released but reference still held: use FRHIPooledRenderQuery::GetQueryRef() with extreme caution"));
	
	checkf(Query.IsValid(), TEXT("Only release valid queries"));
	checkf((uint32)Queries.Num() < NumQueries, TEXT("Pool contains more queries than it started with, double release somewhere?"));

	Queries.Push(MoveTemp(Query));
	check(!Query.IsValid());
}

FRenderQueryPoolRHIRef RHICreateRenderQueryPool(ERenderQueryType QueryType, uint32 NumQueries)
{
	return GDynamicRHI->RHICreateRenderQueryPool(QueryType, NumQueries);
}

EColorSpaceAndEOTF FDynamicRHI::RHIGetColorSpace(FRHIViewport* Viewport)
{
	return EColorSpaceAndEOTF::ERec709_sRGB;
}

void FDynamicRHI::RHICheckViewportHDRStatus(FRHIViewport* Viewport)
{
}

void* FDynamicRHI::RHILockBufferMGPU(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 GPUIndex, uint32 Offset, uint32 Size, EResourceLockMode LockMode)
{
	// Fall through to single GPU case
	check(GPUIndex == 0);
	return RHILockBuffer(RHICmdList, Buffer, Offset, Size, LockMode);
}

void FDynamicRHI::RHIUnlockBufferMGPU(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 GPUIndex)
{
	// Fall through to single GPU case
	check(GPUIndex == 0);
	RHIUnlockBuffer(RHICmdList, Buffer);
}

FShaderResourceViewInitializer::FShaderResourceViewInitializer(FRHIBuffer* InBuffer, EPixelFormat InFormat, uint32 InStartOffsetBytes, uint32 InNumElements)
	: BufferInitializer({ InBuffer, InStartOffsetBytes, InNumElements, InFormat }), Type(EType::VertexBufferSRV)
{
	check(InStartOffsetBytes % RHIGetMinimumAlignmentForBufferBackedSRV(InFormat) == 0);
	/*if (!BufferInitializer.IsWholeResource())
	{
		const uint32 Stride = GPixelFormats[InFormat].BlockBytes;
		check((BufferInitializer.NumElements * Stride + BufferInitializer.StartOffsetBytes) <= BufferInitializer.Buffer->GetSize());
	}*/

	InitType();
}

FShaderResourceViewInitializer::FShaderResourceViewInitializer(FRHIBuffer* InBuffer, EPixelFormat InFormat)
	: BufferInitializer({ InBuffer, 0, UINT32_MAX, InFormat }), Type(EType::VertexBufferSRV) 
{
	InitType();
}

FShaderResourceViewInitializer::FShaderResourceViewInitializer(FRHIBuffer* InBuffer, uint32 InStartOffsetBytes, uint32 InNumElements)
	: BufferInitializer({ InBuffer, InStartOffsetBytes, InNumElements, PF_Unknown }), Type(EType::StructuredBufferSRV)
{
	const uint32 Stride = EnumHasAnyFlags(InBuffer->GetUsage(), BUF_AccelerationStructure) 
		? 1 // Acceleration structure buffers don't have a stride as they are opaque and not indexable
		: InBuffer->GetStride();

	check(InStartOffsetBytes % Stride == 0);
	if (!BufferInitializer.IsWholeResource())
	{
		check((BufferInitializer.NumElements * Stride + BufferInitializer.StartOffsetBytes) <= BufferInitializer.Buffer->GetSize());
	}

	InitType();
}

FShaderResourceViewInitializer::FShaderResourceViewInitializer(FRHIBuffer* InBuffer)
	: BufferInitializer({ InBuffer, 0, UINT32_MAX }), Type(EType::StructuredBufferSRV)
{
	InitType();
}

void FShaderResourceViewInitializer::InitType()
{
	if (BufferInitializer.Buffer)
	{
		EBufferUsageFlags Usage = BufferInitializer.Buffer->GetUsage();
		if (EnumHasAnyFlags(Usage, BUF_VertexBuffer))
		{
			Type = EType::VertexBufferSRV;
		}
		else if (EnumHasAnyFlags(Usage, BUF_IndexBuffer))
		{
			Type = EType::IndexBufferSRV;
		}
		else if (EnumHasAnyFlags(Usage, BUF_AccelerationStructure))
		{
			Type = EType::AccelerationStructureSRV;
		}
		else
		{
			Type = EType::StructuredBufferSRV;
		}
	}
}
