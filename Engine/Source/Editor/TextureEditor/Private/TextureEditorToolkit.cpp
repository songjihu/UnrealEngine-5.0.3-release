// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureEditorToolkit.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Editor.h"
#include "Widgets/Layout/SBorder.h"
#include "Misc/FeedbackContext.h"
#include "Modules/ModuleManager.h"
#include "SlateOptMacros.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "EditorStyleSet.h"
#include "EditorReimportHandler.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "Engine/LightMapTexture2D.h"
#include "Engine/ShadowMapTexture2D.h"
#include "Engine/Texture2DDynamic.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/VolumeTexture.h"
#include "TextureEncodingSettings.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "Interfaces/ITextureEditorModule.h"
#include "TextureEditor.h"
#include "Slate/SceneViewport.h"
#include "PropertyEditorModule.h"
#include "TextureEditorConstants.h"
#include "Models/TextureEditorCommands.h"
#include "Widgets/STextureEditorViewport.h"
#include "ISettingsModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "TextureEditorSettings.h"
#include "TextureCompiler.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Menus/TextureEditorViewOptionsMenu.h"
#include "MediaTexture.h"
#include "TextureEncodingSettings.h"
#include "EditorWidgets/Public/SEnumCombo.h"
#include "Widgets/Layout/SHeader.h"
#include "DerivedDataCache/Public/DerivedDataCacheKey.h"
#include "Settings/ProjectPackagingSettings.h"
#include "Compression/OodleDataCompressionUtil.h"

#define LOCTEXT_NAMESPACE "FTextureEditorToolkit"

DEFINE_LOG_CATEGORY_STATIC(LogTextureEditor, Log, All);

#define MIPLEVEL_MIN 0
#define MIPLEVEL_MAX 15
#define EXPOSURE_MIN -10
#define EXPOSURE_MAX 10


const FName FTextureEditorToolkit::ViewportTabId(TEXT("TextureEditor_Viewport"));
const FName FTextureEditorToolkit::PropertiesTabId(TEXT("TextureEditor_Properties"));
const FName FTextureEditorToolkit::OodleTabId(TEXT("TextureEditor_Oodle"));

UNREALED_API void GetBestFitForNumberOfTiles(int32 InSize, int32& OutRatioX, int32& OutRatioY);

EPixelFormatChannelFlags GetPixelFormatChannelFlagForButton(ETextureChannelButton InButton)
{
	switch (InButton)
	{
		case ETextureChannelButton::Red:
		{
			return EPixelFormatChannelFlags::R;
		}
		case ETextureChannelButton::Green:
		{
			return EPixelFormatChannelFlags::G;
		}
		case ETextureChannelButton::Blue:
		{
			return EPixelFormatChannelFlags::B;
		}
		case ETextureChannelButton::Alpha:
		{
			return EPixelFormatChannelFlags::A;
		}
		default:
		{
			check(false);
		}
	}

	return EPixelFormatChannelFlags::None;
}

void FTextureEditorToolkit::PostTextureRecode()
{
	// Each time we change a custom encode setting we want to re-encode the texture
	// as though we changed a compression setting on the actual texture, so we just
	// post a CompressionSettings property changed event to handle all of that for
	// us.
	FProperty* Property = FindFProperty<FProperty>(UTexture::StaticClass(), "CompressionSettings");
	FPropertyChangedEvent PropertyChangedEvent(Property);
	Texture->PostEditChangeProperty(PropertyChangedEvent);

	// Clear the key we have so we know when we have new data
	OodleCompressedPreviewDDCKey.Set<FString>(FString());
}


/* FTextureEditorToolkit structors
 *****************************************************************************/

FTextureEditorToolkit::FTextureEditorToolkit()
	: Texture(nullptr)
	, VolumeOpacity(1.f)
	, VolumeOrientation(90, 0, -90)
{
}

FTextureEditorToolkit::~FTextureEditorToolkit( )
{
	// Release the VT page table allocation used to display this texture
	UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
	if (Texture2D && Texture2D->IsCurrentlyVirtualTextured())
	{
		FVirtualTexture2DResource* Resource = (FVirtualTexture2DResource*)Texture2D->GetResource();
		Resource->ReleaseAllocatedVT();
	}

	FReimportManager::Instance()->OnPreReimport().RemoveAll(this);
	FReimportManager::Instance()->OnPostReimport().RemoveAll(this);
	GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetPostImport.RemoveAll(this);

	GEditor->UnregisterForUndo(this);

	if (CustomEncoding->bUseCustomEncode)
	{
		// reencode the texture with normal settings.
		CustomEncoding->bUseCustomEncode = false;
		PostTextureRecode();
	}
}


/* FAssetEditorToolkit interface
 *****************************************************************************/

FString FTextureEditorToolkit::GetDocumentationLink( ) const 
{
	return FString(TEXT("Engine/Content/Types/Textures/Properties/Interface"));
}


void FTextureEditorToolkit::RegisterTabSpawners( const TSharedRef<class FTabManager>& InTabManager )
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_TextureEditor", "Texture Editor"));
	auto WorkspaceMenuCategoryRef = WorkspaceMenuCategory.ToSharedRef();

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FTextureEditorToolkit::HandleTabSpawnerSpawnViewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "Viewport"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Viewports"));

	InTabManager->RegisterTabSpawner(PropertiesTabId, FOnSpawnTab::CreateSP(this, &FTextureEditorToolkit::HandleTabSpawnerSpawnProperties))
		.SetDisplayName(LOCTEXT("PropertiesTab", "Details") )
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Details"));

	InTabManager->RegisterTabSpawner(OodleTabId, FOnSpawnTab::CreateSP(this, &FTextureEditorToolkit::HandleTabSpawnerSpawnOodle))
		.SetDisplayName(LOCTEXT("OodleTab", "Oodle"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Details"));
}


void FTextureEditorToolkit::UnregisterTabSpawners( const TSharedRef<class FTabManager>& InTabManager )
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(PropertiesTabId);
	InTabManager->UnregisterTabSpawner(OodleTabId);
}


void FTextureEditorToolkit::InitTextureEditor( const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UObject* ObjectToEdit )
{
	FReimportManager::Instance()->OnPreReimport().AddRaw(this, &FTextureEditorToolkit::HandleReimportManagerPreReimport);
	FReimportManager::Instance()->OnPostReimport().AddRaw(this, &FTextureEditorToolkit::HandleReimportManagerPostReimport);
	GEditor->GetEditorSubsystem<UImportSubsystem>()->OnAssetPostImport.AddRaw(this, &FTextureEditorToolkit::HandleAssetPostImport);

	Texture = CastChecked<UTexture>(ObjectToEdit);

	// The texture being edited might still be compiling, wait till it finishes then.
	// FinishCompilation is nice enough to provide a progress for us while we're waiting.
	FTextureCompilingManager::Get().FinishCompilation({Texture});

	// Support undo/redo
	Texture->SetFlags(RF_Transactional);
	GEditor->RegisterForUndo(this);

	CustomEncoding = MakeShared<FTextureEditorCustomEncode>(FTextureEditorCustomEncode());

	// initialize view options
	bIsRedChannel = true;
	bIsGreenChannel = true;
	bIsBlueChannel = true;
	bIsAlphaChannel = false;

	ExposureBias = 0;

	bIsVolumeTexture = IsVolumeTexture();

	switch (Texture->CompressionSettings)
	{
	default:
		bIsAlphaChannel = !Texture->CompressionNoAlpha;
		break;
	case TC_Normalmap:
	case TC_Grayscale:
	case TC_Displacementmap:
	case TC_VectorDisplacementmap:
	case TC_DistanceFieldFont:
		bIsAlphaChannel = false;
		break;
	}

	bIsDesaturation = false;

	SpecifiedMipLevel = 0;
	bUseSpecifiedMipLevel = false;

	SpecifiedLayer = 0;

	SavedCompressionSetting = false;

	// Start at whatever the last used zoom mode was
	const UTextureEditorSettings& Settings = *GetDefault<UTextureEditorSettings>();
	ZoomMode = Settings.ZoomMode;
	Zoom = 1.0f;

	// Register our commands. This will only register them if not previously registered
	FTextureEditorCommands::Register();

	BindCommands();
	CreateInternalWidgets();

	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_TextureEditor_Layout_v5")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
				->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewSplitter()
						->SetOrientation(Orient_Vertical)
						->Split
						(
							FTabManager::NewStack()
								->AddTab(ViewportTabId, ETabState::OpenedTab)
								->SetHideTabWell(true)
								->SetSizeCoefficient(0.9f)
						)
				)
				->Split
				(
					FTabManager::NewStack()
						->AddTab(PropertiesTabId, ETabState::OpenedTab)
						->AddTab(OodleTabId, ETabState::OpenedTab)
						->SetForegroundTab(PropertiesTabId)
						->SetSizeCoefficient(0.33f)
				)
		);

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;

	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, TextureEditorAppIdentifier, StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, ObjectToEdit);
	
	ITextureEditorModule* TextureEditorModule = &FModuleManager::LoadModuleChecked<ITextureEditorModule>("TextureEditor");
	AddMenuExtender(TextureEditorModule->GetMenuExtensibilityManager()->GetAllExtenders(GetToolkitCommands(), GetEditingObjects()));

	ExtendToolBar();

	RegenerateMenusAndToolbars();

	// @todo toolkit world centric editing
	/*if(IsWorldCentricAssetEditor())
	{
		SpawnToolkitTab(GetToolbarTabId(), FString(), EToolkitTabSpot::ToolBar);
		SpawnToolkitTab(ViewportTabId, FString(), EToolkitTabSpot::Viewport);
		SpawnToolkitTab(PropertiesTabId, FString(), EToolkitTabSpot::Details);
	}*/
}


/* ITextureEditorToolkit interface
 *****************************************************************************/

void FTextureEditorToolkit::CalculateTextureDimensions( uint32& Width, uint32& Height, uint32& Depth, uint32& ArraySize ) const
{
	const FIntPoint LogicalSize = Texture->Source.GetLogicalSize();
	Width = LogicalSize.X;
	Height = LogicalSize.Y;
	Depth = IsVolumeTexture() ? Texture->Source.GetNumLayers() : 0;
	ArraySize = (Is2DArrayTexture() || IsCubeTexture()) ? Texture->Source.GetNumLayers() : 0;

	if (!Width && !Height)
	{
		Width = (uint32)Texture->GetSurfaceWidth();
		Height = (uint32)Texture->GetSurfaceHeight();
		Depth = (uint32)Texture->GetSurfaceDepth();
		ArraySize = Texture->GetSurfaceArraySize();
	}

	// catch if the Width and Height are still zero for some reason
	if ((Width == 0) || (Height == 0))
	{
		Width = 0;
		Height= 0;
		Depth = 0;
		ArraySize = 0;
		return;
	}

	// See if we need to uniformly scale it to fit in viewport
	// Cap the size to effective dimensions
	uint32 ViewportW = TextureViewport->GetViewport()->GetSizeXY().X;
	uint32 ViewportH = TextureViewport->GetViewport()->GetSizeXY().Y;
	uint32 MaxWidth; 
	uint32 MaxHeight;

	// Fit is the same as fill, but doesn't scale up past 100%
	const ETextureEditorZoomMode CurrentZoomMode = GetZoomMode();
	if (CurrentZoomMode == ETextureEditorZoomMode::Fit || CurrentZoomMode == ETextureEditorZoomMode::Fill)
	{
		const UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture);
		const UTextureRenderTargetVolume* VolumeTextureRT = Cast< UTextureRenderTargetVolume>(Texture);

		// Subtract off the viewport space devoted to padding (2 * PreviewPadding)
		// so that the texture is padded on all sides
		MaxWidth = ViewportW;
		MaxHeight = ViewportH;

		if (IsCubeTexture())
		{
			// Cubes are displayed 2:1. 2x width if the source exists and is not an unwrapped image.
			const bool bMultipleSourceImages = Texture->Source.GetNumSlices() > 1;
			const bool bNoSourceImage = Texture->Source.GetNumSlices() == 0;
			Width *= (bNoSourceImage || bMultipleSourceImages) ? 2 : 1;
		}
		else if (VolumeTexture || VolumeTextureRT)
		{
			UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
			if (Settings.VolumeViewMode == ETextureEditorVolumeViewMode::TextureEditorVolumeViewMode_VolumeTrace)
			{
				Width  = Height;
			}
			else
			{
				Width = FMath::CeilToInt((float)Height * (float)PreviewEffectiveTextureWidth / (float)PreviewEffectiveTextureHeight);
			}
		}

		// First, scale up based on the size of the viewport
		if (MaxWidth > MaxHeight)
		{
			Height = Height * MaxWidth / Width;
			Width = MaxWidth;
		}
		else
		{
			Width = Width * MaxHeight / Height;
			Height = MaxHeight;
		}

		// then, scale again if our width and height is impacted by the scaling
		if (Width > MaxWidth)
		{
			Height = Height * MaxWidth / Width;
			Width = MaxWidth;
		}
		if (Height > MaxHeight)
		{
			Width = Width * MaxHeight / Height;
			Height = MaxHeight;
		}
		
		// If fit, then we only want to scale down
		// So if our natural dimensions are smaller than the viewport, we can just use those
		if (CurrentZoomMode == ETextureEditorZoomMode::Fit)
		{
			if (PreviewEffectiveTextureWidth < Width && PreviewEffectiveTextureHeight < Height)
			{
				Width = PreviewEffectiveTextureWidth;
				Height = PreviewEffectiveTextureHeight;
			}
		}
	}
	else
	{
		Width = PreviewEffectiveTextureWidth * Zoom;
		Height = PreviewEffectiveTextureHeight * Zoom;
	}
}


ESimpleElementBlendMode FTextureEditorToolkit::GetColourChannelBlendMode( ) const
{
	if (Texture && (Texture->CompressionSettings == TC_Grayscale || Texture->CompressionSettings == TC_Alpha))
	{
		return SE_BLEND_Opaque;
	}

	// Add the red, green, blue, alpha and desaturation flags to the enum to identify the chosen filters
	uint32 Result = (uint32)SE_BLEND_RGBA_MASK_START;
	Result += bIsRedChannel ? (1 << 0) : 0;
	Result += bIsGreenChannel ? (1 << 1) : 0;
	Result += bIsBlueChannel ? (1 << 2) : 0;
	Result += bIsAlphaChannel ? (1 << 3) : 0;
	
	// If we only have one color channel active, enable color desaturation by default
	const int32 NumColorChannelsActive = (bIsRedChannel ? 1 : 0) + (bIsGreenChannel ? 1 : 0) + (bIsBlueChannel ? 1 : 0);
	const bool bIsDesaturationLocal = bIsDesaturation ? true : (NumColorChannelsActive==1);
	Result += bIsDesaturationLocal ? (1 << 4) : 0;

	return (ESimpleElementBlendMode)Result;
}

bool FTextureEditorToolkit::IsFitToViewport() const
{
	return IsCurrentZoomMode(ETextureEditorZoomMode::Fit);
}

bool FTextureEditorToolkit::IsFillToViewport() const
{
	return IsCurrentZoomMode(ETextureEditorZoomMode::Fill);
}

int32 FTextureEditorToolkit::GetMipLevel( ) const
{
	return GetUseSpecifiedMip() ? SpecifiedMipLevel : -1;
}

int32 FTextureEditorToolkit::GetLayer() const
{
	return SpecifiedLayer;
}

UTexture* FTextureEditorToolkit::GetTexture( ) const
{
	return Texture;
}


bool FTextureEditorToolkit::HasValidTextureResource( ) const
{
	return Texture != nullptr && Texture->GetResource() != nullptr;
}


bool FTextureEditorToolkit::GetUseSpecifiedMip( ) const
{
	if (GetMaxMipLevel().Get(MIPLEVEL_MAX) > 0)
	{
		if (HandleMipLevelCheckBoxIsEnabled())
		{
			return bUseSpecifiedMipLevel;
		}

		// by default this is on
		return true; 
	}

	// disable the widgets if we have no mip maps
	return false;
}


double FTextureEditorToolkit::GetCustomZoomLevel( ) const
{
	return Zoom;
}


void FTextureEditorToolkit::PopulateQuickInfo( )
{
	if (Texture->IsDefaultTexture())
	{
		ImportedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Imported_NA", "Imported: Computing..."));
		CurrentText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Displayed_NA", "Displayed: Computing..."));
		MaxInGameText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_MaxInGame_NA", "Max In-Game: Computing..."));
		SizeText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_ResourceSize_NA", "Resource Size: Computing..."));
		MethodText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Method_NA", "Method: Computing..."));
		LODBiasText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_LODBias_NA", "Combined LOD Bias: Computing..."));
		FormatText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Format_NA", "Format: Computing..."));
		NumMipsText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_NumMips_NA", "Number of Mips: Computing..."));
		HasAlphaChannelText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_HasAlphaChannel_NA", "Has Alpha Channel: Computing..."));
		EncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_Computing", "Encode Speed: Computing..."));
		return;
	}

	FTexturePlatformData** PlatformDataPtr = Texture->GetRunningPlatformData();
	if (PlatformDataPtr && PlatformDataPtr[0]) // Can be null if we haven't had a chance to call CachePlatformData on the texture (brand new)
	{
		FTexturePlatformData::FTextureEncodeResultMetadata const& ResultMetadata = PlatformDataPtr[0]->ResultMetadata;
		if (ResultMetadata.bIsValid == false)
		{
			EncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_NA", "Encode Speed: N/A"));

			FText OodleInfoMissing = NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_Missing", "<Metadata Missing>");
			OodleEncoderText->SetText(OodleInfoMissing);
			OodleEncodeSpeedText->SetText(OodleInfoMissing);
			OodleRDOText->SetText(OodleInfoMissing);
			OodleEffortText->SetText(OodleInfoMissing);
			OodleTilingText->SetText(OodleInfoMissing);
			OodleRDOSourceText->SetText(OodleInfoMissing);

			OodleRDOText->SetVisibility(EVisibility::Hidden);
			OodleEffortText->SetVisibility(EVisibility::Hidden);
			OodleTilingText->SetVisibility(EVisibility::Hidden);
			OodleRDOSourceText->SetVisibility(EVisibility::Hidden);

			OodleRDOEnabledLabel->SetVisibility(EVisibility::Hidden);
			OodleRDOSourceLabel->SetVisibility(EVisibility::Hidden);
			OodleEffortLabel->SetVisibility(EVisibility::Hidden);
			OodleTilingLabel->SetVisibility(EVisibility::Hidden);
		}
		else
		{
			//
			// Check if we need to compress new Oodle preview once we know we have
			// valid results.
			//
			FTexturePlatformData* PlatformData = PlatformDataPtr[0];

			bool AlreadyHaveResults = false;
			if (PlatformData->DerivedDataKey.GetIndex() == OodleCompressedPreviewDDCKey.GetIndex())
			{
				if (PlatformData->DerivedDataKey.IsType<FString>())
				{
					if (PlatformData->DerivedDataKey.Get<FString>() == OodleCompressedPreviewDDCKey.Get<FString>())
					{
						AlreadyHaveResults = true;
					}
				}
				else
				{
					if (*PlatformData->DerivedDataKey.Get<UE::DerivedData::FCacheKeyProxy>().AsCacheKey() == *OodleCompressedPreviewDDCKey.Get<UE::DerivedData::FCacheKeyProxy>().AsCacheKey())
					{
						AlreadyHaveResults = true;
					}
				}
			}

			if (AlreadyHaveResults == false)
			{
				if (bEstimateCompressionEnabled)
				{
					OutstandingEstimation = PlatformData->LaunchEstimateOnDiskSizeTask(OodleCompressor, OodleCompressionLevel, CompressionBlockSize, Texture->GetPathName());
				}

				OodleCompressedPreviewDDCKey = PlatformDataPtr[0]->DerivedDataKey;
			}

			// If we have an outstanding estimation task, update UI when complete.
			if (OutstandingEstimation.IsValid())
			{
				if (OutstandingEstimation.IsReady())
				{
					TTuple<uint64, uint64> Result = OutstandingEstimation.Get();

					OodleEstimateRaw->SetText(FText::AsMemory(Result.Get<1>()));
					OodleEstimateCompressed->SetText(FText::AsMemory(Result.Get<0>()));

					OutstandingEstimation = TFuture<TTuple<uint64, uint64>>();
				}
				else
				{
					OodleEstimateRaw->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_Working", "Working..."));
					OodleEstimateCompressed->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_Working", "Working..."));
				}
			}

			OodleEncoderText->SetText(FText::FromName(ResultMetadata.Encoder));

			if (ResultMetadata.bSupportsEncodeSpeed == false)
			{
				EncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_Unsup", "Encode Speed: Unsupported"));
				OodleEncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_SpeedUnsup", "Unsupported"));

				FText OodleInfoNA = NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_NA", "N/A");
				OodleRDOText->SetText(OodleInfoNA);
				OodleEffortText->SetText(OodleInfoNA);
				OodleTilingText->SetText(OodleInfoNA);
				OodleRDOSourceText->SetText(OodleInfoNA);

				OodleRDOText->SetVisibility(EVisibility::Hidden);
				OodleEffortText->SetVisibility(EVisibility::Hidden);
				OodleTilingText->SetVisibility(EVisibility::Hidden);
				OodleRDOSourceText->SetVisibility(EVisibility::Hidden);

				OodleRDOEnabledLabel->SetVisibility(EVisibility::Hidden);
				OodleRDOSourceLabel->SetVisibility(EVisibility::Hidden);
				OodleEffortLabel->SetVisibility(EVisibility::Hidden);
				OodleTilingLabel->SetVisibility(EVisibility::Hidden);

			}
			else
			{
				OodleRDOText->SetVisibility(EVisibility::Visible);
				OodleEffortText->SetVisibility(EVisibility::Visible);
				OodleTilingText->SetVisibility(EVisibility::Visible);
				OodleRDOSourceText->SetVisibility(EVisibility::Visible);

				OodleRDOEnabledLabel->SetVisibility(EVisibility::Visible);
				OodleRDOSourceLabel->SetVisibility(EVisibility::Visible);
				OodleEffortLabel->SetVisibility(EVisibility::Visible);
				OodleTilingLabel->SetVisibility(EVisibility::Visible);

				if (ResultMetadata.bWasEditorCustomEncoding)
				{
					EncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_Custom", "Encode Speed: Custom"));
					OodleEncodeSpeedText->SetText(NSLOCTEXT("TextureEditor", "QuickInfoDetails_EncodeSpeed_Custom", "Custom"));
				}
				else
				{
					EncodeSpeedText->SetText(
						ResultMetadata.EncodeSpeed == (uint8)ETextureEncodeSpeed::Fast ?
							NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_Fast", "Encode Speed: Fast") : 
							NSLOCTEXT("TextureEditor", "QuickInfo_EncodeSpeed_Final", "Encode Speed: Final")
					);
					OodleEncodeSpeedText->SetText(
						ResultMetadata.EncodeSpeed == (uint8)ETextureEncodeSpeed::Fast ?
						NSLOCTEXT("TextureEditor", "QuickInfoDetails_EncodeSpeed_Fast", "Fast") :
						NSLOCTEXT("TextureEditor", "QuickInfoDetails_EncodeSpeed_Final", "Final")
					);
				}

				if (ResultMetadata.OodleRDO == 0)
				{
					const UTextureEncodingProjectSettings* Settings = GetDefault<UTextureEncodingProjectSettings>();
					const bool bDisabledGlobally = ResultMetadata.EncodeSpeed == (uint8)ETextureEncodeSpeed::Fast ? !Settings->bFastUsesRDO : !Settings->bFinalUsesRDO;

					OodleRDOText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDODisable", "Disabled"));
					if (ResultMetadata.bWasEditorCustomEncoding)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSource_Custom", "Custom"));
					}
					else if (bDisabledGlobally)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSourceDisableSettings", "Disabled By Project Settings"));
					}
					else
					{
						if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::Default)
						{
							OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSourceDisableLCA_Default", "Disabled By Project (Lossy Compression Amount)"));
						}
						else if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::Texture)
						{
							OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSourceDisableLCA_Texture", "Disabled By Texture (Lossy Compression Amount)"));
						}
						else if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::LODGroup)
						{
							OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSourceDisableLCA_LODGroup", "Disabled By LODGroup (Lossy Compression Amount)"));
						}
					}
				}
				else
				{
					OodleRDOText->SetText(FText::AsNumber(ResultMetadata.OodleRDO));

					if (ResultMetadata.bWasEditorCustomEncoding)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSource_Custom", "Custom"));
					}
					else if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::Default)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSource_Default", "Project (Lambda)"));
					}
					else if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::Texture)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSource_Texture", "Texture (Lossy Compression Amount)"));
					}
					else if (ResultMetadata.RDOSource == FTexturePlatformData::FTextureEncodeResultMetadata::OodleRDOSource::LODGroup)
					{
						OodleRDOSourceText->SetText(NSLOCTEXT("TextureEditor", "QuickInfo_Oodle_RDOSource_LODGroup", "LODGroup (Lossy Compression Amount)"));
					}
				}

				UEnum* EncodeEffortEnum = StaticEnum<ETextureEncodeEffort>();
				OodleEffortText->SetText(FText::AsCultureInvariant(EncodeEffortEnum->GetNameStringByValue(ResultMetadata.OodleEncodeEffort)));

				UEnum* UniversalTilingEnum = StaticEnum<ETextureUniversalTiling>();
				OodleTilingText->SetText(FText::AsCultureInvariant(UniversalTilingEnum->GetNameStringByValue(ResultMetadata.OodleUniversalTiling)));
			} // end if encode speed supported
		} // end if results metadata valid
	} // end if valid platform data

	UTexture2D* Texture2D = Cast<UTexture2D>(Texture);

	const bool bIsVolume = IsVolumeTexture();
	const bool bIsArray = IsArrayTexture();
	const bool bIsCube = IsCubeTexture();

	const uint32 SurfaceWidth = (uint32)Texture->GetSurfaceWidth();
	const uint32 SurfaceHeight = (uint32)Texture->GetSurfaceHeight();
	const uint32 SurfaceDepth = (uint32)Texture->GetSurfaceDepth();
	const uint32 NumSurfaces = (uint32)Texture->GetSurfaceArraySize();
	const uint32 ArraySize = bIsCube ? (NumSurfaces / 6u) : NumSurfaces;

	const uint32 ImportedWidth = FMath::Max<uint32>(SurfaceWidth, Texture->Source.GetSizeX());
	const uint32 ImportedHeight =  FMath::Max<uint32>(SurfaceHeight, Texture->Source.GetSizeY());
	const uint32 ImportedDepth = FMath::Max<uint32>(SurfaceDepth, bIsVolume ? Texture->Source.GetNumSlices() : 0);

	const FStreamableRenderResourceState SRRState = Texture->GetStreamableResourceState();
	const int32 ActualMipBias = SRRState.IsValid() ? (SRRState.ResidentFirstLODIdx() + SRRState.AssetLODBias) : Texture->GetCachedLODBias();
	const uint32 ActualWidth = FMath::Max<uint32>(SurfaceWidth >> ActualMipBias, 1);
	const uint32 ActualHeight = FMath::Max<uint32>(SurfaceHeight >> ActualMipBias, 1);
	const uint32 ActualDepth =  FMath::Max<uint32>(SurfaceDepth >> ActualMipBias, 1);

	// Editor dimensions (takes user specified mip setting into account)
	const int32 MipLevel = FMath::Max(GetMipLevel(), 0);
	PreviewEffectiveTextureWidth = FMath::Max<uint32>(ActualWidth >> MipLevel, 1);
	PreviewEffectiveTextureHeight = FMath::Max<uint32>(ActualHeight >> MipLevel, 1);;
	uint32 PreviewEffectiveTextureDepth = FMath::Max<uint32>(ActualDepth >> MipLevel, 1);

	// In game max bias and dimensions
	const int32 MaxResMipBias = Texture2D ? (Texture2D->GetNumMips() - Texture2D->GetNumMipsAllowed(true)) : Texture->GetCachedLODBias();
	const uint32 MaxInGameWidth = FMath::Max<uint32>(SurfaceWidth >> MaxResMipBias, 1);
	const uint32 MaxInGameHeight = FMath::Max<uint32>(SurfaceHeight >> MaxResMipBias, 1);
	const uint32 MaxInGameDepth = FMath::Max<uint32>(SurfaceDepth >> MaxResMipBias, 1);

	// Texture asset size
	const uint32 Size = (Texture->GetResourceSizeBytes(EResourceSizeMode::Exclusive) + 512) / 1024;

	FNumberFormattingOptions SizeOptions;
	SizeOptions.UseGrouping = false;
	SizeOptions.MaximumFractionalDigits = 0;

	// Cubes are previewed as unwrapped 2D textures.
	// These have 2x the width of a cube face.
	PreviewEffectiveTextureWidth *= IsCubeTexture() ? 2 : 1;

	FNumberFormattingOptions Options;
	Options.UseGrouping = false;

	FText CubemapAdd;
	if (bIsCube)
	{
		CubemapAdd = NSLOCTEXT("TextureEditor", "QuickInfo_PerCubeSide", "*6 (CubeMap)");
	}

	if (bIsVolume)
	{
		ImportedText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_Imported_3x", "Imported: {0}x{1}x{2}"), FText::AsNumber(ImportedWidth, &Options), FText::AsNumber(ImportedHeight, &Options), FText::AsNumber(ImportedDepth, &Options)));
		CurrentText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_Displayed_3x", "Displayed: {0}x{1}x{2}"), FText::AsNumber(PreviewEffectiveTextureWidth, &Options ), FText::AsNumber(PreviewEffectiveTextureHeight, &Options), FText::AsNumber(PreviewEffectiveTextureDepth, &Options)));
		MaxInGameText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_MaxInGame_3x_v1", "Max In-Game: {0}x{1}x{2}"), FText::AsNumber(MaxInGameWidth, &Options), FText::AsNumber(MaxInGameHeight, &Options), FText::AsNumber(MaxInGameDepth, &Options)));

		UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
		if (Settings.VolumeViewMode == ETextureEditorVolumeViewMode::TextureEditorVolumeViewMode_VolumeTrace)
		{
			PreviewEffectiveTextureWidth = PreviewEffectiveTextureHeight = FMath::Max(PreviewEffectiveTextureWidth, PreviewEffectiveTextureHeight);
		}
		else
		{
			int32 NumTilesX = 0;
			int32 NumTilesY = 0;
			GetBestFitForNumberOfTiles(PreviewEffectiveTextureDepth, NumTilesX, NumTilesY);
			PreviewEffectiveTextureWidth *= (uint32)NumTilesX;
			PreviewEffectiveTextureHeight *= (uint32)NumTilesY;
		}
	}
	else if (bIsArray)
	{
		ImportedText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_Imported_3x_v2", "Imported: {0}x{1}*{2}"), FText::AsNumber(ImportedWidth, &Options), FText::AsNumber(ImportedHeight, &Options), FText::AsNumber(ArraySize, &Options)));
		CurrentText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_Displayed_3x_v2", "Displayed: {0}x{1}{2}*{3}"), FText::AsNumber(PreviewEffectiveTextureWidth, &Options), FText::AsNumber(PreviewEffectiveTextureHeight, &Options), CubemapAdd, FText::AsNumber(ArraySize, &Options)));
		MaxInGameText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_MaxInGame_3x_v2", "Max In-Game: {0}x{1}{2}*{3}"), FText::AsNumber(MaxInGameWidth, &Options), FText::AsNumber(MaxInGameHeight, &Options), CubemapAdd, FText::AsNumber(ArraySize, &Options)));
	}
	else
	{
	    ImportedText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_Imported_2x", "Imported: {0}x{1}"), FText::AsNumber(ImportedWidth, &Options), FText::AsNumber(ImportedHeight, &Options)));
		CurrentText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_Displayed_2x", "Displayed: {0}x{1}{2}"), FText::AsNumber(PreviewEffectiveTextureWidth, &Options ), FText::AsNumber(PreviewEffectiveTextureHeight, &Options), CubemapAdd));
		MaxInGameText->SetText(FText::Format( NSLOCTEXT("TextureEditor", "QuickInfo_MaxInGame_2x", "Max In-Game: {0}x{1}{2}"), FText::AsNumber(MaxInGameWidth, &Options), FText::AsNumber(MaxInGameHeight, &Options), CubemapAdd));
	}

	SizeText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_ResourceSize", "Resource Size: {0} KB"), FText::AsNumber(Size, &SizeOptions)));

	FText Method = Texture->IsCurrentlyVirtualTextured() ? NSLOCTEXT("TextureEditor", "QuickInfo_MethodVirtualStreamed", "Virtual Streamed")
													: (!Texture->IsStreamable() ? NSLOCTEXT("TextureEditor", "QuickInfo_MethodNotStreamed", "Not Streamed") 
																			: NSLOCTEXT("TextureEditor", "QuickInfo_MethodStreamed", "Streamed") );

	MethodText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_Method", "Method: {0}"), Method));
	LODBiasText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_LODBias", "Combined LOD Bias: {0}"), FText::AsNumber(Texture->GetCachedLODBias())));

	EPixelFormat TextureFormat = GetPixelFormat();
	if (TextureFormat != PF_MAX)
	{
		FormatText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_Format", "Format: {0}"), FText::FromString(GPixelFormats[(uint8)TextureFormat].Name)));
	}

	EPixelFormatChannelFlags ValidTextureChannels = GetPixelFormatValidChannels(TextureFormat);
	HasAlphaChannelText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_HasAlphaChannel", "Has Alpha Channel: {0}"),
		EnumHasAnyFlags(ValidTextureChannels, EPixelFormatChannelFlags::A) ? NSLOCTEXT("TextureEditor", "True", "True") : NSLOCTEXT("TextureEditor", "False", "False")));
	HasAlphaChannelText->SetVisibility((ValidTextureChannels != EPixelFormatChannelFlags::None) ? EVisibility::Visible : EVisibility::Collapsed);

	int32 NumMips = GetNumMips();
	NumMipsText->SetText(FText::Format(NSLOCTEXT("TextureEditor", "QuickInfo_NumMips", "Number of Mips: {0}"), FText::AsNumber(NumMips)));
}


void FTextureEditorToolkit::SetZoomMode( const ETextureEditorZoomMode InZoomMode )
{
	// Update our own zoom mode
	ZoomMode = InZoomMode;
	
	// And also save it so it's used for new texture editors
	UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
	Settings.ZoomMode = ZoomMode;
	Settings.PostEditChange();
}

ETextureEditorZoomMode FTextureEditorToolkit::GetZoomMode() const
{
	// Each texture editors keeps a local zoom mode so that it can be changed without affecting other open editors
	return ZoomMode;
}

double FTextureEditorToolkit::CalculateDisplayedZoomLevel() const
{
	// Avoid calculating dimensions if we're custom anyway
	if (GetZoomMode() == ETextureEditorZoomMode::Custom)
	{
		return Zoom;
	}

	uint32 DisplayWidth, DisplayHeight, DisplayDepth, DisplayArraySize;
	CalculateTextureDimensions(DisplayWidth, DisplayHeight, DisplayDepth, DisplayArraySize);
	if (PreviewEffectiveTextureHeight != 0)
	{
		return (double)DisplayHeight / PreviewEffectiveTextureHeight;
	}
	else if (PreviewEffectiveTextureWidth != 0)
	{
		return (double)DisplayWidth / PreviewEffectiveTextureWidth;
	}
	else
	{
		return 0;
	}
}

void FTextureEditorToolkit::SetCustomZoomLevel( double ZoomValue )
{
	Zoom = FMath::Clamp(ZoomValue, MinZoom, MaxZoom);
	
	// For now we also want to be in custom mode whenever this is changed
	SetZoomMode(ETextureEditorZoomMode::Custom);
}


void FTextureEditorToolkit::OffsetZoom(double OffsetValue, bool bSnapToStepSize)
{
	// Offset from our current "visual" zoom level so that you can
	// smoothly transition from Fit/Fill mode into a custom zoom level
	const double CurrentZoom = CalculateDisplayedZoomLevel();

	if (bSnapToStepSize)
	{
		// Snap to the zoom step when offsetting to avoid zooming all the way to the min (0.01)
		// then back up (+0.1) causing your zoom level to be off by 0.01 (eg. 11%)
		// If we were in a fit view mode then our current zoom level could also be off the grid
		const double FinalZoom = FMath::GridSnap(CurrentZoom + OffsetValue, ZoomStep);
		SetCustomZoomLevel(FinalZoom);
	}
	else
	{
		SetCustomZoomLevel(CurrentZoom + OffsetValue);
	}
}

void FTextureEditorToolkit::ZoomIn( )
{
	OffsetZoom(ZoomStep);
}


void FTextureEditorToolkit::ZoomOut( )
{
	OffsetZoom(-ZoomStep);
}

float FTextureEditorToolkit::GetVolumeOpacity() const
{
	return VolumeOpacity;
}

void FTextureEditorToolkit::SetVolumeOpacity(float InVolumeOpacity)
{
	VolumeOpacity = FMath::Clamp(InVolumeOpacity, 0.f, 1.f);
}

const FRotator& FTextureEditorToolkit::GetVolumeOrientation() const
{
	return VolumeOrientation;
}

void FTextureEditorToolkit::SetVolumeOrientation(const FRotator& InOrientation)
{
	VolumeOrientation = InOrientation;
}

/* IToolkit interface
 *****************************************************************************/

FText FTextureEditorToolkit::GetBaseToolkitName( ) const
{
	return LOCTEXT("AppLabel", "Texture Editor");
}


FName FTextureEditorToolkit::GetToolkitFName( ) const
{
	return FName("TextureEditor");
}


FLinearColor FTextureEditorToolkit::GetWorldCentricTabColorScale( ) const
{
	return FLinearColor(0.3f, 0.2f, 0.5f, 0.5f);
}


FString FTextureEditorToolkit::GetWorldCentricTabPrefix( ) const
{
	return LOCTEXT("WorldCentricTabPrefix", "Texture ").ToString();
}


/* FGCObject interface
 *****************************************************************************/

void FTextureEditorToolkit::AddReferencedObjects( FReferenceCollector& Collector )
{
	Collector.AddReferencedObject(Texture);
	TextureViewport->AddReferencedObjects(Collector);
}


/* FEditorUndoClient interface
 *****************************************************************************/

void FTextureEditorToolkit::PostUndo( bool bSuccess )
{
}


void FTextureEditorToolkit::PostRedo( bool bSuccess )
{
	PostUndo(bSuccess);
}


/* FTextureEditorToolkit implementation
 *****************************************************************************/

void FTextureEditorToolkit::BindCommands( )
{
	const FTextureEditorCommands& Commands = FTextureEditorCommands::Get();

	ToolkitCommands->MapAction(
		Commands.RedChannel,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::OnChannelButtonCheckStateChanged, ETextureChannelButton::Red),
		FCanExecuteAction());

	ToolkitCommands->MapAction(
		Commands.GreenChannel,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::OnChannelButtonCheckStateChanged, ETextureChannelButton::Green),
		FCanExecuteAction());

	ToolkitCommands->MapAction(
		Commands.BlueChannel,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::OnChannelButtonCheckStateChanged, ETextureChannelButton::Blue),
		FCanExecuteAction());

	ToolkitCommands->MapAction(
		Commands.AlphaChannel,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::OnChannelButtonCheckStateChanged, ETextureChannelButton::Alpha),
		FCanExecuteAction());

	ToolkitCommands->MapAction(
		Commands.Desaturation,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleDesaturationChannelActionExecute),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleDesaturationChannelActionIsChecked));

	ToolkitCommands->MapAction(
		Commands.FillToViewport,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleFillToViewportActionExecute));

	ToolkitCommands->MapAction(
		Commands.FitToViewport,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleFitToViewportActionExecute));

	ToolkitCommands->MapAction(
		Commands.ZoomToNatural,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomToNaturalActionExecute));
	

	ToolkitCommands->MapAction(
		Commands.CheckeredBackground,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionExecute, TextureEditorBackground_Checkered),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionIsChecked, TextureEditorBackground_Checkered));

	ToolkitCommands->MapAction(
		Commands.CheckeredBackgroundFill,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionExecute, TextureEditorBackground_CheckeredFill),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionIsChecked, TextureEditorBackground_CheckeredFill));

	ToolkitCommands->MapAction(
		Commands.SolidBackground,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionExecute, TextureEditorBackground_SolidColor),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleCheckeredBackgroundActionIsChecked, TextureEditorBackground_SolidColor));

	// Begin - Volume Texture Specifics
	ToolkitCommands->MapAction(
		Commands.DepthSlices,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleVolumeViewModeActionExecute, TextureEditorVolumeViewMode_DepthSlices),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleVolumeViewModeActionIsChecked, TextureEditorVolumeViewMode_DepthSlices));

	ToolkitCommands->MapAction(
		Commands.TraceIntoVolume,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleVolumeViewModeActionExecute, TextureEditorVolumeViewMode_VolumeTrace),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleVolumeViewModeActionIsChecked, TextureEditorVolumeViewMode_VolumeTrace));
	// End - Volume Texture Specifics

	ToolkitCommands->MapAction(
		Commands.TextureBorder,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleTextureBorderActionExecute),
		FCanExecuteAction(),
		FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::HandleTextureBorderActionIsChecked));

	ToolkitCommands->MapAction(
		Commands.CompressNow,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleCompressNowActionExecute),
		FCanExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleCompressNowActionCanExecute));

	ToolkitCommands->MapAction(
		Commands.Reimport,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleReimportActionExecute),
		FCanExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleReimportActionCanExecute));

	ToolkitCommands->MapAction(
		Commands.Settings,
		FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleSettingsActionExecute));
}


TSharedRef<SWidget> FTextureEditorToolkit::BuildTexturePropertiesWidget( )
{
	FDetailsViewArgs Args;
	Args.bHideSelectionTip = true;

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	TexturePropertiesWidget = PropertyModule.CreateDetailView(Args);
	TexturePropertiesWidget->SetObject(Texture);

	return TexturePropertiesWidget.ToSharedRef();
}

void FTextureEditorToolkit::CreateInternalWidgets()
{
	//
	// Convert the packaging settings names into enums we can use.
	//
	UProjectPackagingSettings const* ProjectSettings = GetDefault<UProjectPackagingSettings>();
	
	PackagingSettingsNames.Add(MakeShared<FString>(TEXT("DebugDevelopment")));
	PackagingSettingsNames.Add(MakeShared<FString>(TEXT("TestShipping")));
	PackagingSettingsNames.Add(MakeShared<FString>(TEXT("Distribution")));
	
	// Default to Distribution
	TSharedPtr<FString> InitialPackagingSetting = PackagingSettingsNames[2];

	// Determine which oodle encoder they are using.
	const TCHAR* CompressorName = 0;
	{
		// Validity check the string by trying to convert to enum.
		const FString& LookupCompressor = ProjectSettings->PackageCompressionMethod;
		FOodleDataCompression::ECompressor PackageCompressor = FOodleDataCompression::ECompressor::Kraken;
		if (FOodleDataCompression::ECompressorFromString(LookupCompressor, PackageCompressor))
		{
			OodleCompressor = PackageCompressor;
		}
		else
		{
			UE_LOG(LogTextureEditor, Warning, TEXT("Project packaging settings not using Oodle? Failed to recognize compression: %s - using Kraken for estimation."), *LookupCompressor);
			OodleCompressor = FOodleDataCompression::ECompressor::Kraken;
		}

		FOodleDataCompression::ECompressorToString(OodleCompressor, &CompressorName);
	}

	OodleCompressionLevel = FOodleDataCompression::ECompressionLevel::Optimal3;
	const TCHAR* LevelName;
	{		 
		FOodleDataCompression::ECompressionLevelFromValue(ProjectSettings->PackageCompressionLevel_Distribution, OodleCompressionLevel);
		FOodleDataCompression::ECompressionLevelToString(OodleCompressionLevel, &LevelName);
	}

	// Grab the compression block size in the settings.
	{
		FString CompBlockSizeString;
		if (FParse::Value(*ProjectSettings->PackageAdditionalCompressionOptions, TEXT("-compressionblocksize="), CompBlockSizeString) &&
			FParse::Value(*ProjectSettings->PackageAdditionalCompressionOptions, TEXT("-compressionblocksize="), CompressionBlockSize))
		{
			if (CompBlockSizeString.EndsWith(TEXT("MB")))
			{
				CompressionBlockSize *= 1024 * 1024;
			}
			else if (CompBlockSizeString.EndsWith(TEXT("KB")))
			{
				CompressionBlockSize *= 1024;
			}
		}
		else
		{
			UE_LOG(LogTextureEditor, Warning, TEXT("No compression block size found in settings - using 256KB"));
			CompressionBlockSize = 256*1024;
		}
	}

	TextureViewport = SNew(STextureEditorViewport, SharedThis(this));

	OodleTabContainer = SNew(SVerticalBox)

	//
	// Oodle relevant details container
	//
	+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			//
			// Details label container
			//
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SNew(STextBlock)
								.Text(LOCTEXT("OodleTab_Label_Encoder", "Encoder:"))
								.ToolTipText(LOCTEXT("OodleTab_Tooltip_Encoder", "Which texture encoder was used to encode the texture."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SNew(STextBlock)
								.Text(LOCTEXT("OodleTab_Label_EncodeSpeed", "Encode Speed:"))
								.ToolTipText(LOCTEXT("OodleTab_Tooltip_EncodeSpeed", "Which of the encode speeds was used for this texture encode, if the encoder supports encode speed."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleRDOEnabledLabel, STextBlock)
								.Text(LOCTEXT("OodleTab_Label_RDOEnabled", "RDO Lambda:"))
								.ToolTipText(LOCTEXT("OodleTab_Tooltip_RDOEnabled", "Whether or not the texture was encoded with RDO enabled. If enabled, shows the lambda used to encode. Excludes any global ini specific adjustments (e.g. GlobalLambdaMultiplier)"))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleRDOSourceLabel, STextBlock)
								.Text(LOCTEXT("OodleTab_Label_RDOSource", "RDO Lambda Source:"))
								.ToolTipText(LOCTEXT("OodleTab_Tooltip_RDOSource", "This is where the build system found the lambda to use, due to defaults and fallbacks. (Lambda) means a direct lambda value (Lossy Compression Amount) means it was converted from that property."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleEffortLabel, STextBlock)
								.Text(LOCTEXT("OodleTab_Label_Effort", "Effort:"))
								.ToolTipText(LOCTEXT("OodleTab_ToolTip_Effort", "Which effort value was used when encoding this texture. Pulled from the encode speed options. Effort represents how much CPU time was spent finding better results."))
						]

					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleTilingLabel, STextBlock)
								.Text(LOCTEXT("OodleTab_Label_UniversalTiling", "Universal Tiling:"))
								.ToolTipText(LOCTEXT("OodleTab_ToolTip_UniversalTiling", "Which universal tiling setting was used when encoding this texture. Specified with encode speed. Universal Tiling is a technique to save on-disk space for platforms that expect tiled textures."))
						]
				]

			//
			// Details controls container
			//
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleEncoderText, STextBlock)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleEncodeSpeedText, STextBlock)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleRDOText, STextBlock)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleRDOSourceText, STextBlock)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleEffortText, STextBlock)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(4.0f)
						[
							SAssignNew(OodleTilingText, STextBlock)
						]
				]
		] // end oodle details.

	//
	// Header for oodle rdo experiments
	//
	+ SVerticalBox::Slot()
		.AutoHeight()
		.VAlign(VAlign_Center)
		[
			SNew(SHeader)
				.HAlign(EHorizontalAlignment::HAlign_Fill)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("OodleTab_Label_TryHeader", "Try Encodings"))
				]
		]

	//
	// Container for oodle rdo experiments labels/controls
	//
	+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			//
			// Labels for oodle rdo experiments.
			//
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_OverrideCompression", "Enabled:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_OverrideCompression", "If checked, allows you to experiment with Oodle RDO compression settings to visualize results."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_OverrideRDO", "RDO Lambda:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_OverrideRDO", "The RDO lambda to encode with for experimentation. 0 disables RDO entirely. 1 is largest filesize, 100 is smallest."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_OverrideEffort", "Effort:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_OverrideEffort", "The encoding effort to try. Effort controls how much CPU time spent on finding better results. See the Oodle Texture documentation for detailed information."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_OverrideTiling", "Universal Tiling:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_OverrideTiling", "The universal tiling to try. See the Oodle Texture documentation for detailed information."))
						]
				]

			//
			// Controls for oodle rdo experiments
			//
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SAssignNew(OodleOverrideCheck, SCheckBox)
							.OnCheckStateChanged(this, &FTextureEditorToolkit::OnUseEditorOodleSettingsChanged)
							.IsChecked(this, &FTextureEditorToolkit::UseEditorOodleSettingsChecked)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SNew(SNumericEntryBox<int32>)
							.Value(this, &FTextureEditorToolkit::GetEditorOodleSettingsRDO)
							.OnValueCommitted(this, &FTextureEditorToolkit::EditorOodleSettingsRDOCommitted)
							.IsEnabled(this, &FTextureEditorToolkit::EditorOodleSettingsEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SNew(SEnumComboBox, StaticEnum< ETextureEncodeEffort >())
							.CurrentValue(this, &FTextureEditorToolkit::GetEditorOodleSettingsEffort)
							.OnEnumSelectionChanged(this, &FTextureEditorToolkit::EditorOodleSettingsEffortChanged)
							.IsEnabled(this, &FTextureEditorToolkit::EditorOodleSettingsEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SNew(SEnumComboBox, StaticEnum< ETextureUniversalTiling >())
							.CurrentValue(this, &FTextureEditorToolkit::GetEditorOodleSettingsTiling)
							.OnEnumSelectionChanged(this, &FTextureEditorToolkit::EditorOodleSettingsTilingChanged)
							.IsEnabled(this, &FTextureEditorToolkit::EditorOodleSettingsEnabled)
						]
				] // end controls
		] // end oodle rdo experiment labels/controls

	//
	// Header for the on disk estimates
	//
	+ SVerticalBox::Slot()
		.AutoHeight()
		.VAlign(VAlign_Center)
		[
			SNew(SHeader)
			.HAlign(EHorizontalAlignment::HAlign_Fill)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OodleTab_Label_EstimatesHeader", "On-disk Sizes"))
				.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimatesHeader", "RDO encoding only helps on-disk texture sizes when package compression is enabled. It does not affect runtime memory usage."))
			]
		]
	//
	// Container for the on disk estimate labels/controls.
	//
	+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			//
			// Labels for the on disk estimates
			//
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EstimatesEnabled", "Enabled:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimatesEnabled", "If checked, texture data will be compressed in the same manner as project packaging in order to estimate the benefits of RDO encoding of the texture."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EncoderSettings", "Packaging Configuration:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EncoderSettings", "Which packaging configuration to pull from for determining which Oodle encoder and compression level to use."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EstimateEncoder", "Oodle Encoder:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimateEncoder", "The oodle encoder to use for estimating. Pulled from the packaging configuration specified above."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EstimateLevel", "Oodle Compression Level:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimateLevel", "The compression level. Pulled from the packaging configuration specified above."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_BlockSize", "Compression Block Size:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_BlockSize", "The size of chunks used when compressing. Pulled from the packaging configuration 'Package Compression Commandline Options'."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EstimateRaw", "Uncompressed size:"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimateRaw", "The size of the mip or virtual texture data for the texture."))
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OodleTab_Label_EstimateCompressed", "Compressed size (estimate):"))
							.ToolTipText(LOCTEXT("OodleTab_ToolTip_EstimateCompressed", "The size of the compressed mip or virtual texture data for the texture."))
						]
				]

			//
			// Controls for the on disk estimates
			//
			+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SAssignNew(OodleEstimateCheck, SCheckBox)
							.OnCheckStateChanged(this, &FTextureEditorToolkit::OnEstimateCompressionChanged)
							.IsChecked(this, &FTextureEditorToolkit::EstimateCompressionChecked)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(2)
						[
							SNew(STextComboBox)
							.OptionsSource(&PackagingSettingsNames)
							.OnSelectionChanged(this, &FTextureEditorToolkit::PackagingSettingsChanged)
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
							.InitiallySelectedItem(InitialPackagingSetting)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(8)
						[
							SAssignNew(OodleEncoderUsed, STextBlock)
							.Text(FText::AsCultureInvariant(CompressorName))
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SAssignNew(OodleLevelUsed, STextBlock)
							.Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), LevelName, (int8)OodleCompressionLevel)))
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SAssignNew(OodleCompressionBlockUsed, STextBlock)
							.Text(FText::AsMemory(CompressionBlockSize))
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SAssignNew(OodleEstimateRaw, STextBlock)
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
						]
					+ SVerticalBox::Slot()
						.AutoHeight()
						.VAlign(VAlign_Center)
						.Padding(6)
						[
							SAssignNew(OodleEstimateCompressed, STextBlock)
							.IsEnabled(this, &FTextureEditorToolkit::EstimateCompressionEnabled)
						]
				]
		]; // end on disk estimates controls


	TextureProperties = SNew(SVerticalBox)

	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(2.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(0.5f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(ImportedText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(CurrentText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(MaxInGameText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(SizeText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(HasAlphaChannelText, STextBlock)
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(0.5f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(MethodText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(FormatText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(LODBiasText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(NumMipsText, STextBlock)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SAssignNew(EncodeSpeedText, STextBlock)
			]

		]
	]

	+ SVerticalBox::Slot()
	.FillHeight(1.0f)
	.Padding(2.0f)
	[
		BuildTexturePropertiesWidget()
	];
}

void FTextureEditorToolkit::ExtendToolBar()
{
	TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);

	ToolbarExtender->AddToolBarExtension(
		"Asset",
		EExtensionHook::After,
		GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FTextureEditorToolkit::FillToolbar)
	);

	AddToolbarExtender(ToolbarExtender);

	ITextureEditorModule* TextureEditorModule = &FModuleManager::LoadModuleChecked<ITextureEditorModule>("TextureEditor");
	AddToolbarExtender(TextureEditorModule->GetToolBarExtensibilityManager()->GetAllExtenders(GetToolkitCommands(), GetEditingObjects()));
}

void FTextureEditorToolkit::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	TSharedRef<SWidget> ChannelControl = MakeChannelControlWidget();
	TSharedRef<SWidget> LODControl = MakeLODControlWidget();
	TSharedRef<SWidget> LayerControl = MakeLayerControlWidget();
	TSharedRef<SWidget> ExposureControl = MakeExposureContolWidget();
	TSharedPtr<SWidget> OptionalOpacityControl = IsVolumeTexture() ? TSharedPtr<SWidget>(MakeOpacityControlWidget()) : nullptr;
	TSharedRef<SWidget> ZoomControl = MakeZoomControlWidget();

	UCurveLinearColorAtlas* Atlas = Cast<UCurveLinearColorAtlas>(GetTexture());
	if (!Atlas)
	{
		ToolbarBuilder.BeginSection("TextureMisc");
		{
			ToolbarBuilder.AddToolBarButton(FTextureEditorCommands::Get().CompressNow);
			ToolbarBuilder.AddToolBarButton(FTextureEditorCommands::Get().Reimport);
		}
		ToolbarBuilder.EndSection();

		ToolbarBuilder.BeginSection("Channels");
		{
			ToolbarBuilder.AddWidget(ChannelControl);
		}
		ToolbarBuilder.EndSection();

		ToolbarBuilder.BeginSection("TextureMipAndExposure");
		{
			ToolbarBuilder.AddWidget(LODControl);
			ToolbarBuilder.AddWidget(ExposureControl);
		}	
		ToolbarBuilder.EndSection();

		if (HasLayers())
		{
			ToolbarBuilder.BeginSection("Layers");
			{
				ToolbarBuilder.AddWidget(LayerControl);
			}
			ToolbarBuilder.EndSection();
		}

		if (OptionalOpacityControl.IsValid())
		{
			ToolbarBuilder.BeginSection("Opacity");
			{
				ToolbarBuilder.AddWidget(OptionalOpacityControl.ToSharedRef());
			}
			ToolbarBuilder.EndSection();
		}

		ToolbarBuilder.BeginSection("Zoom");
		{
			ToolbarBuilder.AddWidget(ZoomControl);
		}
		ToolbarBuilder.EndSection();
		ToolbarBuilder.BeginSection("Settings");
		ToolbarBuilder.BeginStyleOverride("CalloutToolbar");
		{
			ToolbarBuilder.AddWidget(SNew(SSpacer), NAME_None, false, HAlign_Right);
			ToolbarBuilder.AddComboButton(
				FUIAction(),
				FOnGetContent::CreateSP(this, &FTextureEditorToolkit::OnGenerateSettingsMenu),
				LOCTEXT("SettingsMenu", "View Settings"),
				FText::GetEmpty(),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings")
			);
		}
		ToolbarBuilder.EndStyleOverride();
		ToolbarBuilder.EndSection();
	}
}

TOptional<int32> FTextureEditorToolkit::GetMaxMipLevel() const
{
	TOptional<int32> MaxMipLevel;
	int32 NumMips = GetNumMips();
	if (NumMips > 0)
	{
		MaxMipLevel = NumMips - 1;
	}
	return MaxMipLevel;
}

int32 FTextureEditorToolkit::GetNumMips( ) const
{
	if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
	{
		return Texture2D->GetNumMips();
	}
	else if (const UTextureCube* TextureCube = Cast<UTextureCube>(Texture))
	{
		return TextureCube->GetNumMips();
	}
	else if (const UTextureCubeArray* TextureCubeArray = Cast<UTextureCubeArray>(Texture))
	{
		return TextureCubeArray->GetNumMips();
	}
	else if (const UTexture2DArray* Texture2DArray = Cast<UTexture2DArray>(Texture))
	{
		return Texture2DArray->GetNumMips();
	}
	else if (const UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture))
	{
		return VolumeTexture->GetNumMips();
	}
	else if (const UTextureRenderTarget2D* Texture2DRT = Cast<UTextureRenderTarget2D>(Texture))
	{
		return Texture2DRT->GetNumMips();
	}
	else if (const UTextureRenderTargetCube* TextureCubeRT = Cast<UTextureRenderTargetCube>(Texture))
	{
		return TextureCubeRT->GetNumMips();
	}
	else if (const UTextureRenderTarget2DArray* Texture2DArrayRT = Cast<UTextureRenderTarget2DArray>(Texture))
	{
		return Texture2DArrayRT->GetNumMips();
	}
	else if (const UTextureRenderTargetVolume* VolumeTextureRT = Cast<UTextureRenderTargetVolume>(Texture))
	{
		return VolumeTextureRT->GetNumMips();
	}
	else if (const UTexture2DDynamic* Texture2DDynamic = Cast<UTexture2DDynamic>(Texture))
	{
		return Texture2DDynamic->NumMips;
	}
	else if (const UMediaTexture* MediaTexture = Cast<UMediaTexture>(Texture))
	{
		return MediaTexture->GetTextureNumMips();
	}

	return MIPLEVEL_MAX;
}

EPixelFormat FTextureEditorToolkit::GetPixelFormat() const
{
	if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
	{
		return Texture2D->GetPixelFormat(SpecifiedLayer);
	}
	else if (const UTextureCube* TextureCube = Cast<UTextureCube>(Texture))
	{
		return TextureCube->GetPixelFormat();
	}
	else if (const UTexture2DArray* Texture2DArray = Cast<UTexture2DArray>(Texture))
	{
		return Texture2DArray->GetPixelFormat();
	}
	else if (const UTextureCubeArray* TextureCubeArray = Cast<UTextureCubeArray>(Texture))
	{
		return TextureCubeArray->GetPixelFormat();
	}
	else if (const UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture))
	{
		return VolumeTexture->GetPixelFormat();
	}
	else if (const UTextureRenderTarget2D* Texture2DRT = Cast<UTextureRenderTarget2D>(Texture))
	{
		return Texture2DRT->GetFormat();
	}
	else if (const UTextureRenderTargetCube* TextureCubeRT = Cast<UTextureRenderTargetCube>(Texture))
	{
		return TextureCubeRT->GetFormat();
	}
	else if (const UTextureRenderTarget2DArray* Texture2DArrayRT = Cast<UTextureRenderTarget2DArray>(Texture))
	{
		return Texture2DArrayRT->GetFormat();
	}
	else if (const UTextureRenderTargetVolume* VolumeTextureRT = Cast<UTextureRenderTargetVolume>(Texture))
	{
		return VolumeTextureRT->GetFormat();
	}
	else if (const UTexture2DDynamic* Texture2DDynamic = Cast<UTexture2DDynamic>(Texture))
	{
		return Texture2DDynamic->Format;
	}
	//else if (const UMediaTexture* MediaTexture = Cast<UMediaTexture>(Texture))
	//{
	//	UMediaTexture::GetDesc() suggests PF_B8G8R8A8, maybe?
	//}

	return PF_MAX;
}

TOptional<int32> FTextureEditorToolkit::GetMaxLayer() const
{
	return FMath::Max(Texture->Source.GetNumLayers() - 1, 1);
}

bool FTextureEditorToolkit::IsCubeTexture( ) const
{
	return (Texture->IsA(UTextureCube::StaticClass()) || Texture->IsA(UTextureCubeArray::StaticClass()) || Texture->IsA(UTextureRenderTargetCube::StaticClass()));
}

bool FTextureEditorToolkit::IsVolumeTexture() const
{
	return (Texture->IsA(UVolumeTexture::StaticClass()) || Texture->IsA(UTextureRenderTargetVolume::StaticClass()));
}

bool FTextureEditorToolkit::Is2DArrayTexture() const
{
	return (Texture->IsA(UTexture2DArray::StaticClass()) || Texture->IsA(UTextureRenderTarget2DArray::StaticClass()));
}

bool FTextureEditorToolkit::IsArrayTexture() const
{
	return Is2DArrayTexture() || Texture->IsA(UTextureCubeArray::StaticClass());
}

TSharedRef<SWidget> FTextureEditorToolkit::OnGenerateMipMapLevelMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);

	for (int32 MipLevel = MIPLEVEL_MIN; MipLevel <= GetMaxMipLevel().Get(MIPLEVEL_MAX); ++MipLevel)
	{
		FText MipNumberText = FText::AsNumber(MipLevel);

		MenuBuilder.AddMenuEntry(
			FText::Format(LOCTEXT("MipLevel", "Mip Level {0}"), MipNumberText),
			FText::Format(LOCTEXT("MipLevel_Tooltip", "Display Mip Level {0}"), MipNumberText),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleMipLevelChanged, MipLevel),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, MipLevel]() {return SpecifiedMipLevel == MipLevel; })
			)
		);
	}

	return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> FTextureEditorToolkit::OnGenerateSettingsMenu()
{
	FMenuBuilder MenuBuilder(true, ToolkitCommands);
	FTextureEditorViewOptionsMenu::MakeMenu(MenuBuilder, IsVolumeTexture());

	return MenuBuilder.MakeWidget();
}

/* FTextureEditorToolkit callbacks
 *****************************************************************************/
bool FTextureEditorToolkit::IsChannelButtonEnabled(ETextureChannelButton Button) const
{
	EPixelFormatChannelFlags ValidTextureChannels = GetPixelFormatValidChannels(GetPixelFormat());
	return EnumHasAnyFlags(ValidTextureChannels, GetPixelFormatChannelFlagForButton(Button));
}

FSlateColor FTextureEditorToolkit::GetChannelButtonBackgroundColor(ETextureChannelButton Button) const
{
	FSlateColor Dropdown = FAppStyle::Get().GetSlateColor("Colors.Dropdown");

	switch (Button)
	{
	case ETextureChannelButton::Red:
		return bIsRedChannel ? FLinearColor::Red : FLinearColor::White;
	case ETextureChannelButton::Green:
		return bIsGreenChannel ? FLinearColor::Green : FLinearColor::White;
	case ETextureChannelButton::Blue:
		return bIsBlueChannel ? FLinearColor::Blue : FLinearColor::White;
	case ETextureChannelButton::Alpha:
		return FLinearColor::White;
	default:
		check(false);
		return FSlateColor();
	}
}

FSlateColor FTextureEditorToolkit::GetChannelButtonForegroundColor(ETextureChannelButton Button) const
{
	FSlateColor DefaultForeground = FAppStyle::Get().GetSlateColor("Colors.Foreground");

	switch (Button)
	{
	case ETextureChannelButton::Red:
		return bIsRedChannel ? FLinearColor::Black : DefaultForeground;
	case ETextureChannelButton::Green:
		return bIsGreenChannel ? FLinearColor::Black : DefaultForeground;
	case ETextureChannelButton::Blue:
		return bIsBlueChannel ? FLinearColor::Black : DefaultForeground;
	case ETextureChannelButton::Alpha:
		return bIsAlphaChannel ? FLinearColor::Black : DefaultForeground;
	default:
		check(false);
		return FSlateColor::UseForeground();
	}
}

void FTextureEditorToolkit::OnChannelButtonCheckStateChanged(ETextureChannelButton Button)
{
	switch (Button)
	{
	case ETextureChannelButton::Red:
		bIsRedChannel = !bIsRedChannel;
		break;
	case ETextureChannelButton::Green:
		bIsGreenChannel = !bIsGreenChannel;
		break;
	case ETextureChannelButton::Blue:
		bIsBlueChannel = !bIsBlueChannel;
		break;
	case ETextureChannelButton::Alpha:
		bIsAlphaChannel = !bIsAlphaChannel;
		break;
	default:
		check(false);
		break;
	}
}

ECheckBoxState FTextureEditorToolkit::OnGetChannelButtonCheckState(ETextureChannelButton Button) const
{
	switch (Button)
	{
	case ETextureChannelButton::Red:
		return bIsRedChannel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		break;
	case ETextureChannelButton::Green:
		return bIsGreenChannel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		break;
	case ETextureChannelButton::Blue:
		return bIsBlueChannel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		break;
	case ETextureChannelButton::Alpha:
		return bIsAlphaChannel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		break;
	default:
		check(false);
		return ECheckBoxState::Unchecked;
		break;
	}
}


void FTextureEditorToolkit::HandleCheckeredBackgroundActionExecute( ETextureEditorBackgrounds Background )
{
	UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
	Settings.Background = Background;
	Settings.PostEditChange();
}


bool FTextureEditorToolkit::HandleCheckeredBackgroundActionIsChecked( ETextureEditorBackgrounds Background )
{
	const UTextureEditorSettings& Settings = *GetDefault<UTextureEditorSettings>();

	return (Background == Settings.Background);
}

// Callback for toggling the volume display action.
void FTextureEditorToolkit::HandleVolumeViewModeActionExecute(ETextureEditorVolumeViewMode InViewMode)
{
	UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
	Settings.VolumeViewMode = InViewMode;
	Settings.PostEditChange();
}

// Callback for getting the checked state of the volume display action.
bool FTextureEditorToolkit::HandleVolumeViewModeActionIsChecked(ETextureEditorVolumeViewMode InViewMode)
{
	const UTextureEditorSettings& Settings = *GetDefault<UTextureEditorSettings>();

	return (InViewMode == Settings.VolumeViewMode);
}


void FTextureEditorToolkit::HandleCompressNowActionExecute( )
{
	GWarn->BeginSlowTask(NSLOCTEXT("TextureEditor", "CompressNow", "Compressing 1 Textures that have Defer Compression set"), true);

	if (Texture->DeferCompression)
	{
		// turn off deferred compression and compress the texture
		Texture->DeferCompression = false;
		Texture->Source.Compress();
		Texture->PostEditChange();

		PopulateQuickInfo();
	}

	GWarn->EndSlowTask();
}


bool FTextureEditorToolkit::HandleCompressNowActionCanExecute( ) const
{
	return (Texture->DeferCompression != 0);
}


void FTextureEditorToolkit::HandleFitToViewportActionExecute( )
{
	SetZoomMode(ETextureEditorZoomMode::Fit);
}


void FTextureEditorToolkit::HandleFillToViewportActionExecute()
{
	SetZoomMode(ETextureEditorZoomMode::Fill);
}

void FTextureEditorToolkit::HandleZoomToNaturalActionExecute()
{
	SetCustomZoomLevel(1);
}

void FTextureEditorToolkit::HandleMipLevelCheckBoxCheckedStateChanged( ECheckBoxState InNewState )
{
	bUseSpecifiedMipLevel = InNewState == ECheckBoxState::Checked;
}


ECheckBoxState FTextureEditorToolkit::HandleMipLevelCheckBoxIsChecked( ) const
{
	return GetUseSpecifiedMip() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}


bool FTextureEditorToolkit::HandleMipLevelCheckBoxIsEnabled( ) const
{
	UTextureCube* TextureCube = Cast<UTextureCube>(Texture);

	if (GetMaxMipLevel().Get(MIPLEVEL_MAX) <= 0 || TextureCube)
	{
		return false;
	}

	return true;
}

void FTextureEditorToolkit::HandleMipLevelChanged(int32 NewMipLevel)
{
	SpecifiedMipLevel = FMath::Clamp<int32>(NewMipLevel, MIPLEVEL_MIN, GetMaxMipLevel().Get(MIPLEVEL_MAX));

	MipLevelTextBlock->SetText(FText::Format(LOCTEXT("MipLevel", "Mip Level {0}"), SpecifiedMipLevel));
}

TOptional<int32> FTextureEditorToolkit::HandleMipLevelEntryBoxValue( ) const
{
	return SpecifiedMipLevel;
}

FReply FTextureEditorToolkit::HandleMipMapMinusButtonClicked()
{
	HandleMipLevelChanged(--SpecifiedMipLevel);

	return FReply::Handled();
}

FReply FTextureEditorToolkit::HandleMipMapPlusButtonClicked()
{
	HandleMipLevelChanged(++SpecifiedMipLevel);

	return FReply::Handled();
}

void FTextureEditorToolkit::HandleLayerEntryBoxChanged(int32 NewLayer)
{
	SpecifiedLayer = FMath::Clamp<int32>(NewLayer, 0, Texture->Source.GetNumLayers() - 1);
	PopulateQuickInfo();
}


TOptional<int32> FTextureEditorToolkit::HandleLayerEntryBoxValue() const
{
	return SpecifiedLayer;
}

bool FTextureEditorToolkit::HasLayers() const
{
	return Texture->Source.GetNumLayers() > 1;
}

bool FTextureEditorToolkit::HandleReimportActionCanExecute( ) const
{
	if (Texture->IsA<ULightMapTexture2D>() || Texture->IsA<UShadowMapTexture2D>() || Texture->IsA<UTexture2DDynamic>() || Texture->IsA<UTextureRenderTarget>() || Texture->IsA<UCurveLinearColorAtlas>())
	{
		return false;
	}

	return true;
}


void FTextureEditorToolkit::HandleReimportActionExecute( )
{
	FReimportManager::Instance()->Reimport(Texture, /*bAskForNewFileIfMissing=*/true);
}


void FTextureEditorToolkit::HandleReimportManagerPostReimport( UObject* InObject, bool bSuccess )
{
	// Ignore if this is regarding a different object
	if (InObject != Texture)
	{
		return;
	}

	if (!bSuccess)
	{
		// Failed, restore the compression flag
		Texture->DeferCompression = SavedCompressionSetting;
	}

	// Re-enable viewport rendering now that the texture should be in a known state again
	TextureViewport->EnableRendering();
}


void FTextureEditorToolkit::HandleReimportManagerPreReimport( UObject* InObject )
{
	// Ignore if this is regarding a different object
	if (InObject != Texture)
	{
		return;
	}

	// Prevent the texture from being compressed immediately, so the user can see the results
	SavedCompressionSetting = Texture->DeferCompression;
	Texture->DeferCompression = true;

	// Disable viewport rendering until the texture has finished re-importing
	TextureViewport->DisableRendering();
}

void FTextureEditorToolkit::HandleAssetPostImport(UFactory* InFactory, UObject* InObject)
{
	if (Cast<UTexture>(InObject) != nullptr && InObject == Texture)
	{
		// Refresh this object within the details panel
		TexturePropertiesWidget->SetObject(InObject);
	}
}

void FTextureEditorToolkit::HandleDesaturationChannelActionExecute( )
{
	bIsDesaturation = !bIsDesaturation;
}


bool FTextureEditorToolkit::HandleDesaturationChannelActionIsChecked( ) const
{
	return bIsDesaturation;
}


void FTextureEditorToolkit::HandleSettingsActionExecute( )
{
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Editor", "ContentEditors", "TextureEditor");
}

TSharedRef<SDockTab> FTextureEditorToolkit::HandleTabSpawnerSpawnOodle(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == OodleTabId);

	TSharedRef<SDockTab> SpawnedTab = SNew(SDockTab)
		.Label(LOCTEXT("TextureOodleTitle", "Oodle"))
		[
			OodleTabContainer.ToSharedRef()
		];

	return SpawnedTab;
}

TSharedRef<SDockTab> FTextureEditorToolkit::HandleTabSpawnerSpawnProperties( const FSpawnTabArgs& Args )
{
	check(Args.GetTabId() == PropertiesTabId);

	TSharedRef<SDockTab> SpawnedTab = SNew(SDockTab)
		.Label(LOCTEXT("TexturePropertiesTitle", "Details"))
		[
			TextureProperties.ToSharedRef()
		];

	PopulateQuickInfo();

	return SpawnedTab;
}


TSharedRef<SDockTab> FTextureEditorToolkit::HandleTabSpawnerSpawnViewport( const FSpawnTabArgs& Args )
{
	check(Args.GetTabId() == ViewportTabId);

	return SNew(SDockTab)
		.Label(LOCTEXT("TextureViewportTitle", "Viewport"))
		[
			TextureViewport.ToSharedRef()
		];
}


void FTextureEditorToolkit::HandleTextureBorderActionExecute( )
{
	UTextureEditorSettings& Settings = *GetMutableDefault<UTextureEditorSettings>();
	Settings.TextureBorderEnabled = !Settings.TextureBorderEnabled;
	Settings.PostEditChange();
}


bool FTextureEditorToolkit::HandleTextureBorderActionIsChecked( ) const
{
	const UTextureEditorSettings& Settings = *GetDefault<UTextureEditorSettings>();

	return Settings.TextureBorderEnabled;
}

EVisibility FTextureEditorToolkit::HandleExposureBiasWidgetVisibility() const
{
	if ((Texture != nullptr) && (Texture->CompressionSettings == TC_HDR || Texture->CompressionSettings == TC_HDR_Compressed))
	{
		return EVisibility::Visible;
	}

	return EVisibility::Collapsed;
}


TOptional<int32> FTextureEditorToolkit::HandleExposureBiasBoxValue() const
{
	return GetExposureBias();
}

void FTextureEditorToolkit::HandleExposureBiasBoxValueChanged(int32 NewExposure)
{
	ExposureBias = NewExposure;
}

void FTextureEditorToolkit::HandleOpacitySliderChanged(float NewValue)
{
	SetVolumeOpacity(NewValue);
}

TOptional<float> FTextureEditorToolkit::HandleOpacitySliderValue() const
{
	return GetVolumeOpacity();
}


FReply FTextureEditorToolkit::HandleViewOptionsMenuButtonClicked()
{
	if (ViewOptionsMenuAnchor->ShouldOpenDueToClick())
	{
		ViewOptionsMenuAnchor->SetIsOpen(true);
	}
	else
	{
		ViewOptionsMenuAnchor->SetIsOpen(false);
	}

	return FReply::Handled();
}

void FTextureEditorToolkit::HandleZoomMenuEntryClicked(double ZoomValue)
{
	SetCustomZoomLevel(ZoomValue);
}

void FTextureEditorToolkit::HandleZoomMenuFillClicked()
{
	SetZoomMode(ETextureEditorZoomMode::Fill);
}

void FTextureEditorToolkit::HandleZoomMenuFitClicked()
{
	SetZoomMode(ETextureEditorZoomMode::Fit);
}

bool FTextureEditorToolkit::IsZoomMenuFillChecked() const
{
	return IsCurrentZoomMode(ETextureEditorZoomMode::Fill);
}

bool FTextureEditorToolkit::IsZoomMenuFitChecked() const
{
	return IsCurrentZoomMode(ETextureEditorZoomMode::Fit);
}

FText FTextureEditorToolkit::HandleZoomPercentageText() const
{
	double DisplayedZoomLevel = CalculateDisplayedZoomLevel();
	FText ZoomLevelPercent = FText::AsPercent(DisplayedZoomLevel);

	// For fit and fill, show the effective zoom level in parenthesis - eg. "Fill (220%)"
	static const FText ZoomModeWithPercentFormat = LOCTEXT("ZoomModeWithPercentFormat", "{ZoomMode} ({ZoomPercent})");
	if (GetZoomMode() == ETextureEditorZoomMode::Fit)
	{
		static const FText ZoomModeFit = LOCTEXT("ZoomModeFit", "Fit");
		return FText::FormatNamed(ZoomModeWithPercentFormat, TEXT("ZoomMode"), ZoomModeFit, TEXT("ZoomPercent"), ZoomLevelPercent);
	}

	if (GetZoomMode() == ETextureEditorZoomMode::Fill)
	{
		static const FText ZoomModeFill = LOCTEXT("ZoomModeFill", "Fill");
		return FText::FormatNamed(ZoomModeWithPercentFormat, TEXT("ZoomMode"), ZoomModeFill, TEXT("ZoomPercent"), ZoomLevelPercent);
	}

	// If custom, then just the percent is enough
	return ZoomLevelPercent;
}

void FTextureEditorToolkit::HandleZoomSliderChanged(float NewValue)
{
	SetCustomZoomLevel(NewValue * MaxZoom);
}

float FTextureEditorToolkit::HandleZoomSliderValue() const
{
	return (CalculateDisplayedZoomLevel() / MaxZoom);
}

int32 FTextureEditorToolkit::GetEditorOodleSettingsEffort() const
{
	return CustomEncoding->OodleEncodeEffort;
}

void FTextureEditorToolkit::EditorOodleSettingsEffortChanged(int32 NewValue, ESelectInfo::Type SelectionType)
{
	bool bChanged = CustomEncoding->OodleEncodeEffort != NewValue;

	CustomEncoding->OodleEncodeEffort = NewValue;

	if (CustomEncoding->bUseCustomEncode || bChanged)
	{
		PostTextureRecode();
	}
}

int32 FTextureEditorToolkit::GetEditorOodleSettingsTiling() const
{
	return CustomEncoding->OodleUniversalTiling;
}

void FTextureEditorToolkit::EditorOodleSettingsTilingChanged(int32 NewValue, ESelectInfo::Type SelectionType)
{
	bool bChanged = CustomEncoding->OodleUniversalTiling != NewValue;
	CustomEncoding->OodleUniversalTiling = NewValue;

	if (CustomEncoding->bUseCustomEncode && bChanged)
	{
		PostTextureRecode();
	}
}

TOptional<int32> FTextureEditorToolkit::GetEditorOodleSettingsRDO() const
{
	return CustomEncoding->OodleRDOLambda;
}

void FTextureEditorToolkit::EditorOodleSettingsRDOCommitted(int32 NewValue, ETextCommit::Type CommitType)
{
	if (NewValue > 100)
	{
		NewValue = 100;
	}
	if (NewValue < 0)
	{
		NewValue = 0;
	}

	bool bChanged = CustomEncoding->OodleRDOLambda != (int8)NewValue;

	CustomEncoding->OodleRDOLambda = (int8)NewValue;

	if (CustomEncoding->bUseCustomEncode && bChanged)
	{
		PostTextureRecode();
	}
}


bool FTextureEditorToolkit::EditorOodleSettingsEnabled() const
{
	return CustomEncoding->bUseCustomEncode;
}

ECheckBoxState FTextureEditorToolkit::UseEditorOodleSettingsChecked() const
{
	return CustomEncoding->bUseCustomEncode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FTextureEditorToolkit::OnUseEditorOodleSettingsChanged(ECheckBoxState NewState)
{
	// We need to convince the texture to recompress and signal all its users
	// that they need to update, so we do this by faking a compression method property change.
	CustomEncoding->bUseCustomEncode = NewState == ECheckBoxState::Checked ? true : false;

	PostTextureRecode();
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeChannelControlWidget()
{
	auto OnChannelCheckStateChanged = [this](ECheckBoxState NewState, ETextureChannelButton Button)
	{
		OnChannelButtonCheckStateChanged(Button);
	};

	TSharedRef<SWidget> ChannelControl = 
		SNew(SHorizontalBox)
		+SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		.AutoWidth()
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "TextureEditor.ChannelButtonStyle")
			.BorderBackgroundColor(this, &FTextureEditorToolkit::GetChannelButtonBackgroundColor, ETextureChannelButton::Red)
			.ForegroundColor(this, &FTextureEditorToolkit::GetChannelButtonForegroundColor, ETextureChannelButton::Red)
			.OnCheckStateChanged_Lambda(OnChannelCheckStateChanged, ETextureChannelButton::Red)
			.IsChecked(this, &FTextureEditorToolkit::OnGetChannelButtonCheckState, ETextureChannelButton::Red)
			.IsEnabled(this, &FTextureEditorToolkit::IsChannelButtonEnabled, ETextureChannelButton::Red)
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle("TextureEditor.ChannelButtonFont"))
				.Text(FText::FromString("R"))
			]
		]
		+SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		.AutoWidth()
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "TextureEditor.ChannelButtonStyle")
			.BorderBackgroundColor(this, &FTextureEditorToolkit::GetChannelButtonBackgroundColor, ETextureChannelButton::Green)
			.ForegroundColor(this, &FTextureEditorToolkit::GetChannelButtonForegroundColor, ETextureChannelButton::Green)
			.OnCheckStateChanged_Lambda(OnChannelCheckStateChanged, ETextureChannelButton::Green)
			.IsChecked(this, &FTextureEditorToolkit::OnGetChannelButtonCheckState, ETextureChannelButton::Green)
			.IsEnabled(this, &FTextureEditorToolkit::IsChannelButtonEnabled, ETextureChannelButton::Green)
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle("TextureEditor.ChannelButtonFont"))
				.Text(FText::FromString("G"))
			]
		]

		+SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		.AutoWidth()
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "TextureEditor.ChannelButtonStyle")
			.BorderBackgroundColor(this, &FTextureEditorToolkit::GetChannelButtonBackgroundColor, ETextureChannelButton::Blue)
			.ForegroundColor(this, &FTextureEditorToolkit::GetChannelButtonForegroundColor, ETextureChannelButton::Blue)
			.OnCheckStateChanged_Lambda(OnChannelCheckStateChanged, ETextureChannelButton::Blue)
			.IsChecked(this, &FTextureEditorToolkit::OnGetChannelButtonCheckState, ETextureChannelButton::Blue)
			.IsEnabled(this, &FTextureEditorToolkit::IsChannelButtonEnabled, ETextureChannelButton::Blue)
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle("TextureEditor.ChannelButtonFont"))
				.Text(FText::FromString("B"))
			]
		]
		+SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(2.0f)
		.AutoWidth()
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "TextureEditor.ChannelButtonStyle")
			.BorderBackgroundColor(this, &FTextureEditorToolkit::GetChannelButtonBackgroundColor, ETextureChannelButton::Alpha)
			.ForegroundColor(this, &FTextureEditorToolkit::GetChannelButtonForegroundColor, ETextureChannelButton::Alpha)
			.OnCheckStateChanged_Lambda(OnChannelCheckStateChanged, ETextureChannelButton::Alpha)
			.IsChecked(this, &FTextureEditorToolkit::OnGetChannelButtonCheckState, ETextureChannelButton::Alpha)
			.IsEnabled(this, &FTextureEditorToolkit::IsChannelButtonEnabled, ETextureChannelButton::Alpha)
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle("TextureEditor.ChannelButtonFont"))
				.Text(FText::FromString("A"))
			]
		];

	return ChannelControl;
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeLODControlWidget()
{
	TSharedRef<SWidget> LODControl = SNew(SBox)
		.WidthOverride(212.0f)
		[
			SNew(SHorizontalBox)
			.IsEnabled(this, &FTextureEditorToolkit::HandleMipLevelCheckBoxIsEnabled)
			+ SHorizontalBox::Slot()
			.Padding(4.0f, 0.0f, 2.0f, 0.0f)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked(this, &FTextureEditorToolkit::HandleMipLevelCheckBoxIsChecked)
				.OnCheckStateChanged(this, &FTextureEditorToolkit::HandleMipLevelCheckBoxCheckedStateChanged)
			]
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SComboButton)
				.IsEnabled(this, &FTextureEditorToolkit::GetUseSpecifiedMip)
				.OnGetMenuContent(this, &FTextureEditorToolkit::OnGenerateMipMapLevelMenu)
				.ButtonContent()
				[
					SAssignNew(MipLevelTextBlock, STextBlock)
					.Text(FText::Format(LOCTEXT("MipLevel", "Mip Level {0}"), SpecifiedMipLevel))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "TextureEditor.MipmapButtonStyle")
				.OnClicked(this, &FTextureEditorToolkit::HandleMipMapPlusButtonClicked)
				.IsEnabled(this, &FTextureEditorToolkit::GetUseSpecifiedMip)
				[
					SNew(SImage)
					.Image(FAppStyle::Get().GetBrush("Icons.Plus"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "TextureEditor.MipmapButtonStyle")
				.OnClicked(this, &FTextureEditorToolkit::HandleMipMapMinusButtonClicked)
				.IsEnabled(this, &FTextureEditorToolkit::GetUseSpecifiedMip)
				[
					SNew(SImage)
					.Image(FAppStyle::Get().GetBrush("Icons.Minus"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];

	return LODControl;
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeLayerControlWidget()
{
	TSharedRef<SWidget> LayerControl = SNew(SBox)
		.WidthOverride(160.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.Padding(4.0f, 0.0f, 4.0f, 0.0f)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("TextureEditor", "Layer", "Layer"))
			]
			+ SHorizontalBox::Slot()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				.VAlign(VAlign_Center)
			[
				SNew(SNumericEntryBox<int32>)
				.AllowSpin(true)
				.MinSliderValue(0)
				.MaxSliderValue(this, &FTextureEditorToolkit::GetMaxLayer)
				.Value(this, &FTextureEditorToolkit::HandleLayerEntryBoxValue)
				.OnValueChanged(this, &FTextureEditorToolkit::HandleLayerEntryBoxChanged)
			]
		];

	return LayerControl;
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeExposureContolWidget()
{
	TSharedRef<SWidget> ExposureControl = SNew(SBox)
		.WidthOverride(160.0f)
		.Visibility(this, &FTextureEditorToolkit::HandleExposureBiasWidgetVisibility)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.Padding(8.0f, 0.0f, 4.0f, 0.0f)
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExposureBiasLabel", "Exposure Bias"))
				]
				+ SHorizontalBox::Slot()
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(true)
					.MinSliderValue(MinExposure)
					.MaxSliderValue(MaxExposure)
					.Value(this, &FTextureEditorToolkit::HandleExposureBiasBoxValue)
					.OnValueChanged(this, &FTextureEditorToolkit::HandleExposureBiasBoxValueChanged)
				]
			]
		];
	return ExposureControl;
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeOpacityControlWidget()
{
	TSharedRef<SWidget> OpacityControl = SNew(SBox)
		.WidthOverride(160.0f)
		[
			// opacity slider
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OpacityLabel", "Opacity"))
			]
			+ SHorizontalBox::Slot()
			.Padding(4.0f, 0.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SNumericEntryBox<float>)
				.AllowSpin(true)
				.MinSliderValue(0.0f)
				.MaxSliderValue(1.0f)
				.OnValueChanged(this, &FTextureEditorToolkit::HandleOpacitySliderChanged)
				.Value(this, &FTextureEditorToolkit::HandleOpacitySliderValue)
			]
		];

	return OpacityControl;
}

TSharedRef<SWidget> FTextureEditorToolkit::MakeZoomControlWidget()
{
	const FMargin ToolbarSlotPadding(4.0f, 1.0f);
	const FMargin ToolbarButtonPadding(4.0f, 0.0f);

	FMenuBuilder ZoomMenuBuilder(true, NULL);
	{
		FUIAction Zoom25Action(FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuEntryClicked, 0.25));
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("Zoom25Action", "25%"), LOCTEXT("Zoom25ActionHint", "Show the texture at a quarter of its size."), FSlateIcon(), Zoom25Action);

		FUIAction Zoom50Action(FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuEntryClicked, 0.5));
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("Zoom50Action", "50%"), LOCTEXT("Zoom50ActionHint", "Show the texture at half its size."), FSlateIcon(), Zoom50Action);

		FUIAction Zoom100Action(FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuEntryClicked, 1.0));
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("Zoom100Action", "100%"), LOCTEXT("Zoom100ActionHint", "Show the texture in its original size."), FSlateIcon(), Zoom100Action);

		FUIAction Zoom200Action(FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuEntryClicked, 2.0));
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("Zoom200Action", "200%"), LOCTEXT("Zoom200ActionHint", "Show the texture at twice its size."), FSlateIcon(), Zoom200Action);

		FUIAction Zoom400Action(FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuEntryClicked, 4.0));
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("Zoom400Action", "400%"), LOCTEXT("Zoom400ActionHint", "Show the texture at four times its size."), FSlateIcon(), Zoom400Action);

		ZoomMenuBuilder.AddMenuSeparator();

		FUIAction ZoomFitAction(
			FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuFitClicked),
			FCanExecuteAction(),
			FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::IsZoomMenuFitChecked)
		);
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("ZoomFitAction", "Scale To Fit"), LOCTEXT("ZoomFitActionHint", "Scales the texture down to fit within the viewport if needed."), FSlateIcon(), ZoomFitAction, NAME_None, EUserInterfaceActionType::RadioButton);

		FUIAction ZoomFillAction(
			FExecuteAction::CreateSP(this, &FTextureEditorToolkit::HandleZoomMenuFillClicked),
			FCanExecuteAction(),
			FIsActionChecked::CreateSP(this, &FTextureEditorToolkit::IsZoomMenuFillChecked)
		);
		ZoomMenuBuilder.AddMenuEntry(LOCTEXT("ZoomFillAction", "Scale To Fill"), LOCTEXT("ZoomFillActionHint", "Scales the texture up and down to fill the viewport."), FSlateIcon(), ZoomFillAction, NAME_None, EUserInterfaceActionType::RadioButton);
	}

	// zoom slider
	TSharedRef<SWidget> ZoomControl = 
		SNew(SBox)
		.WidthOverride(250.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ZoomLabel", "Zoom"))
			]
			+ SHorizontalBox::Slot()
			.Padding(4.0f, 0.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(200.f)
				[
					SNew(SSlider)
					.OnValueChanged(this, &FTextureEditorToolkit::HandleZoomSliderChanged)
					.Value(this, &FTextureEditorToolkit::HandleZoomSliderValue)
				]
			]
			+ SHorizontalBox::Slot()
			.VAlign(VAlign_Center)
			.AutoWidth()
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[

					SNew(SComboButton)
					.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
					.ButtonContent()
					[
						SNew(STextBlock)
						.Text(this, &FTextureEditorToolkit::HandleZoomPercentageText)
					]
					.MenuContent()
					[
						ZoomMenuBuilder.MakeWidget()
					]
			]
		];

	return ZoomControl;
}

void FTextureEditorToolkit::OnEstimateCompressionChanged(ECheckBoxState NewState)
{
	OodleCompressedPreviewDDCKey.Set<FString>(FString());
	bEstimateCompressionEnabled = NewState == ECheckBoxState::Checked;
}
ECheckBoxState FTextureEditorToolkit::EstimateCompressionChecked() const
{
	return bEstimateCompressionEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FTextureEditorToolkit::PackagingSettingsChanged(TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		UProjectPackagingSettings const* ProjectSettings = GetDefault<UProjectPackagingSettings>();
		int8 CompressionLevelFromSettings = (int8)FOodleDataCompression::ECompressionLevel::Optimal3;
		if (*Selection == TEXT("DebugDevelopment"))
		{
			CompressionLevelFromSettings = ProjectSettings->PackageCompressionLevel_DebugDevelopment;
		}
		else if (*Selection == TEXT("TestShipping"))
		{
			CompressionLevelFromSettings = ProjectSettings->PackageCompressionLevel_TestShipping;
		}
		else if (*Selection == TEXT("Distribution"))
		{
			CompressionLevelFromSettings = ProjectSettings->PackageCompressionLevel_Distribution;
		}

		FOodleDataCompression::ECompressionLevel OldLevel = OodleCompressionLevel;
		FOodleDataCompression::ECompressionLevelFromValue(CompressionLevelFromSettings, OodleCompressionLevel);

		const TCHAR* LevelName;
		FOodleDataCompression::ECompressionLevelToString(OodleCompressionLevel, &LevelName);
		OodleLevelUsed->SetText(FText::FromString(FString::Printf(TEXT("%s (%d)"), LevelName, CompressionLevelFromSettings)));

		if (OldLevel != OodleCompressionLevel)
		{
			OodleCompressedPreviewDDCKey.Set<FString>(FString());
		}
	}
}

#undef LOCTEXT_NAMESPACE
