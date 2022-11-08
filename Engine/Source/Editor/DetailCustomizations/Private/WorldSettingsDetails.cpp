// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldSettingsDetails.h"
#include "Framework/Commands/UIAction.h"
#include "Layout/WidgetPath.h"
#include "Framework/Application/MenuStack.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/MessageDialog.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Misc/MessageDialog.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "DetailCategoryBuilder.h"
#include "GameModeInfoCustomizer.h"
#include "Settings/EditorExperimentalSettings.h"
#include "GameFramework/WorldSettings.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "ScopedTransaction.h"
#include "ActorFolder.h"

#define LOCTEXT_NAMESPACE "WorldSettingsDetails"


FWorldSettingsDetails::~FWorldSettingsDetails()
{
}

/* IDetailCustomization overrides
 *****************************************************************************/

void FWorldSettingsDetails::CustomizeDetails( IDetailLayoutBuilder& DetailBuilder )
{
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory("GameMode");
	CustomizeGameInfoProperty("DefaultGameMode", DetailBuilder, Category);

	AddLightmapCustomization(DetailBuilder);

	AddWorldCustomization(DetailBuilder);
	
	DetailBuilder.HideProperty(AActor::GetHiddenPropertyName(), AActor::StaticClass());
}


/* FWorldSettingsDetails implementation
 *****************************************************************************/


void FWorldSettingsDetails::CustomizeGameInfoProperty( const FName& PropertyName, IDetailLayoutBuilder& DetailBuilder, IDetailCategoryBuilder& CategoryBuilder )
{
	// Get the object that we are viewing details of. Expect to only edit one WorldSettings object at a time!
	TArray< TWeakObjectPtr<UObject> > ObjectsCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsCustomized);
	UObject* ObjectCustomized = (ObjectsCustomized.Num() > 0) ? ObjectsCustomized[0].Get() : NULL;

	// Allocate customizer object
	GameInfoModeCustomizer = MakeShareable(new FGameModeInfoCustomizer(ObjectCustomized, PropertyName));

	// Then use it to customize
	GameInfoModeCustomizer->CustomizeGameModeSetting(DetailBuilder, CategoryBuilder);
}


void FWorldSettingsDetails::AddLightmapCustomization( IDetailLayoutBuilder& DetailBuilder )
{
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory("Lightmass");

	TSharedRef<FLightmapCustomNodeBuilder> LightMapGroupBuilder = MakeShareable(new FLightmapCustomNodeBuilder(DetailBuilder.GetThumbnailPool()));
	const bool bForAdvanced = true;
	Category.AddCustomBuilder(LightMapGroupBuilder, bForAdvanced);
}

void FWorldSettingsDetails::AddWorldCustomization(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> CustomizedObjects;
	DetailBuilder.GetObjectsBeingCustomized(CustomizedObjects);
	ULevel* CustomizedLevel = nullptr;
	if (CustomizedObjects.Num() > 0)
	{
		if (AWorldSettings* WorldSettings = Cast<AWorldSettings>(CustomizedObjects[0]))
		{
			CustomizedLevel = WorldSettings->GetLevel();
			SelectedWorldSettings = WorldSettings;
		}
	}

	// Hide some of the WorldPartition properties found in AActor
	TArray<TSharedRef<IPropertyHandle>> PropertiesToHide;
	PropertiesToHide.Add(DetailBuilder.GetProperty(AActor::GetRuntimeGridPropertyName(), AActor::StaticClass()));
	PropertiesToHide.Add(DetailBuilder.GetProperty(AActor::GetIsSpatiallyLoadedPropertyName(), AActor::StaticClass()));
	for (TSharedRef<IPropertyHandle> Property : PropertiesToHide)
	{
		DetailBuilder.HideProperty(Property);
	}

	if (CustomizedLevel)
	{
		const bool bIsPartitionedWorld = UWorld::HasSubsystem<UWorldPartitionSubsystem>(CustomizedLevel->GetWorld());

		IDetailCategoryBuilder& WorldCategory = DetailBuilder.EditCategory("World");
		if (GetDefault<UEditorExperimentalSettings>()->bEnableOneFilePerActorSupport)
		{
			WorldCategory.AddCustomRow(LOCTEXT("LevelUseExternalActorsRow", "LevelUseExternalActors"), true)
				.NameContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LevelUseExternalActors", "Use External Actors"))
					.ToolTipText(LOCTEXT("ActorPackagingMode_ToolTip", "Use external actors, new actor spawned in this level will be external and existing external actors will be loaded on load."))
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.IsEnabled(!bIsPartitionedWorld)
				]
				.ValueContent()
				[
					SNew(SCheckBox)
					.OnCheckStateChanged(this, &FWorldSettingsDetails::OnUseExternalActorsChanged, CustomizedLevel)
					.IsChecked(this, &FWorldSettingsDetails::IsUseExternalActorsChecked, CustomizedLevel)
					.IsEnabled(!bIsPartitionedWorld)
				];
		}

		const bool bIsUsingActorFolders = CustomizedLevel->IsUsingActorFolders();
		if (bIsUsingActorFolders || GetDefault<UEditorExperimentalSettings>()->bEnableActorFolderObjectSupport)
		{
			WorldCategory.AddCustomRow(LOCTEXT("LevelUseActorFoldersRow", "LevelUseActorFolders"), true)
				.NameContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LevelUseActorFolders", "Use Actor Folder Objects"))
					.ToolTipText(LOCTEXT("LevelUseActorFolders_ToolTip", "Use actor folder objects, actor folders of this level will be persistent in their own object."))
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.IsEnabled(!bIsUsingActorFolders)
				]
				.ValueContent()
				[
					SNew(SCheckBox)
					.OnCheckStateChanged(this, &FWorldSettingsDetails::OnUseActorFoldersChanged, CustomizedLevel)
					.IsChecked(this, &FWorldSettingsDetails::IsUsingActorFoldersChecked, CustomizedLevel)
					.IsEnabled(!bIsUsingActorFolders)
				];
		}

		if (bIsPartitionedWorld)
		{
			IDetailCategoryBuilder& WorldPartitionCategory = DetailBuilder.EditCategory("WorldPartition");

			WorldPartitionCategory.AddCustomRow(LOCTEXT("DefaultWorldPartitionSettingsRow", "DefaultWorldPartitionSettings"), true)
				.NameContent()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DefaultWorldPartitionSettings", "Default World Partition Settings"))
					.ToolTipText(LOCTEXT("DefaultWorldPartitionSettings_ToolTip", "Save or Reset the current World Partition default editor state"))
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.IsEnabled(bIsPartitionedWorld)
				]
				.ValueContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					[
						SNew(SButton)
						.OnClicked_Lambda([CustomizedLevel]()
							{
								FScopedTransaction Transaction(LOCTEXT("ResetDefaultWorldPartitionSettings", "Reset Default World Partition Settings"));
								CustomizedLevel->GetWorldSettings()->ResetDefaultWorldPartitionSettings();
								return FReply::Handled();
							})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ResetButtonText", "Reset"))
							.ToolTipText(LOCTEXT("ResetButtonToolTip", "Reset World Partition default editor state"))
							.Font(IDetailLayoutBuilder::GetDetailFont())
						]
					]
					+ SHorizontalBox::Slot()
					[
						SNew(SButton)
						.OnClicked_Lambda([CustomizedLevel]()
							{
								FScopedTransaction Transaction(LOCTEXT("SaveDefaultWorldPartitionSettings", "Save Default World Partition Settings"));
								CustomizedLevel->GetWorldSettings()->SaveDefaultWorldPartitionSettings();
								return FReply::Handled();
							})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("SaveButtonText", "Save"))
							.ToolTipText(LOCTEXT("SaveButtonToolTip", "Save current World Partition editor state as map default"))
							.Font(IDetailLayoutBuilder::GetDetailFont())
							.IsEnabled(bIsPartitionedWorld)
						]
					]
				];
		}
	}
}

void FWorldSettingsDetails::OnUseActorFoldersChanged(ECheckBoxState BoxState, ULevel* Level)
{
	if (Level && (BoxState == ECheckBoxState::Checked))
	{
		Level->SetUseActorFolders(true, /*bInteractiveMode*/ true);
	}
}

ECheckBoxState FWorldSettingsDetails::IsUsingActorFoldersChecked(ULevel* Level) const
{
	return Level->IsUsingActorFolders() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FWorldSettingsDetails::OnUseExternalActorsChanged(ECheckBoxState BoxState, ULevel* Level)
{
	if (Level != nullptr)
	{
		// Validate we have a saved map
		UPackage* LevelPackage = Level->GetOutermost();
		if (LevelPackage == GetTransientPackage()
			|| LevelPackage->HasAnyFlags(RF_Transient)
			|| !FPackageName::IsValidLongPackageName(LevelPackage->GetName()))
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("UseExternalActorsSaveMap", "You need to save the level before enabling the `Use External Actors` option."));
			return;
		}

		FScopedTransaction Transaction(LOCTEXT("WorldUseExternalActors", "Change World Use External Actors"));

		Level->Modify();
		Level->SetUseExternalActors(BoxState == ECheckBoxState::Checked);
		
		FText MessageTitle(LOCTEXT("ConvertActorPackagingDialog", "Convert Actors Packaging"));
		FText PackagingMode = Level->IsUsingExternalActors() ? LOCTEXT("ExternalActors", "External") : LOCTEXT("InternalActors", "Internal");
		FText Message = FText::Format(LOCTEXT("ConvertActorPackagingMsg", "Do you want to convert all actors to {0} packaging as well?"), PackagingMode);
		EAppReturnType::Type ConvertAnswer = FMessageDialog::Open(EAppMsgType::YesNo, Message, &MessageTitle);

		// if the user accepts, convert all actors to what the new packaging mode will be
		if (ConvertAnswer == EAppReturnType::Yes)
		{
			Level->ConvertAllActorsToPackaging(Level->IsUsingExternalActors());
		}
	}
}

ECheckBoxState FWorldSettingsDetails::IsUseExternalActorsChecked(ULevel* Level) const
{
	return Level->IsUsingExternalActors() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

FLightmapCustomNodeBuilder::FLightmapCustomNodeBuilder(const TSharedPtr<FAssetThumbnailPool>& InThumbnailPool)
{
	ThumbnailPool = InThumbnailPool;
}


FLightmapCustomNodeBuilder::~FLightmapCustomNodeBuilder()
{
	FEditorDelegates::OnLightingBuildKept.RemoveAll(this);
	FEditorDelegates::NewCurrentLevel.RemoveAll(this);
}


void FLightmapCustomNodeBuilder::SetOnRebuildChildren(FSimpleDelegate InOnRegenerateChildren)
{
	OnRegenerateChildren = InOnRegenerateChildren;

	FEditorDelegates::OnLightingBuildKept.AddSP(this, &FLightmapCustomNodeBuilder::HandleLightingBuildKept);
	FEditorDelegates::NewCurrentLevel.AddSP(this, &FLightmapCustomNodeBuilder::HandleNewCurrentLevel);
}


void FLightmapCustomNodeBuilder::GenerateHeaderRowContent(FDetailWidgetRow& NodeRow)
{
	NodeRow.NameContent()
	[
		SNew( STextBlock )
		.Text( LOCTEXT("LightmapHeaderRowContent", "Lightmaps") )
		.Font( IDetailLayoutBuilder::GetDetailFont() )
	];

	NodeRow.ValueContent()
	[
		SNew( STextBlock )
		.Text( this, &FLightmapCustomNodeBuilder::GetLightmapCountText )
		.Font( IDetailLayoutBuilder::GetDetailFont() )
	];
}


void FLightmapCustomNodeBuilder::GenerateChildContent(IDetailChildrenBuilder& ChildrenBuilder)
{
	RefreshLightmapItems();

	for(TSharedPtr<FLightmapItem>& Item : LightmapItems)
	{
		ChildrenBuilder.AddCustomRow(LOCTEXT("LightMapsFilter", "Lightmaps"))
		.WholeRowContent()
		.HAlign(HAlign_Fill)
		[
			MakeLightMapList(Item)
		];
	}
}


FText FLightmapCustomNodeBuilder::GetLightmapCountText() const
{
	return FText::Format( LOCTEXT("LightmapHeaderRowCount", "{0} Lightmap(s)"), FText::AsNumber(LightmapItems.Num()) );
}


void FLightmapCustomNodeBuilder::HandleLightingBuildKept()
{
	OnRegenerateChildren.ExecuteIfBound();
}

void FLightmapCustomNodeBuilder::HandleNewCurrentLevel()
{
	OnRegenerateChildren.ExecuteIfBound();
}


TSharedRef<SWidget> FLightmapCustomNodeBuilder::MakeLightMapList(TSharedPtr<FLightmapItem> LightMapItem)
{
	if ( !ensure(LightMapItem.IsValid()) )
	{
		return SNullWidget::NullWidget;
	}

	const uint32 ThumbnailResolution = 64;
	const uint32 ThumbnailBoxPadding = 4;
	UObject* LightMapObject = FindObject<UObject>(NULL, *LightMapItem->ObjectPath);
	FAssetData LightMapAssetData(LightMapObject);

	FAssetThumbnailConfig ThumbnailConfig;
	ThumbnailConfig.bAllowFadeIn = true;

	TWeakPtr<FLightmapItem> LightmapWeakPtr = LightMapItem;
	return
		SNew(SBorder)
		.BorderImage(nullptr)
		.Padding(0.0f)
		.OnMouseButtonUp(this, &FLightmapCustomNodeBuilder::OnMouseButtonUp, LightmapWeakPtr)
		.OnMouseDoubleClick(this, &FLightmapCustomNodeBuilder::OnLightMapListMouseButtonDoubleClick, LightmapWeakPtr)
		[
			SNew(SHorizontalBox)
			// Viewport
			+SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew( SBox )
				.WidthOverride( ThumbnailResolution + ThumbnailBoxPadding * 2 )
				.HeightOverride( ThumbnailResolution + ThumbnailBoxPadding * 2 )
				[
					// Drop shadow border
					SNew(SBorder)
					.Padding(ThumbnailBoxPadding)
					.BorderImage( FEditorStyle::GetBrush("ContentBrowser.ThumbnailShadow") )
					[
						LightMapItem->Thumbnail->MakeThumbnailWidget(ThumbnailConfig)
					]
				]
			]

			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6, 0, 0, 0)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 1)
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(FText::FromName(LightMapAssetData.AssetName) )
				]

				+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 1)
				[
					// Class
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(FText::FromName(LightMapAssetData.AssetClass))
				]
			]
		];
}


TSharedPtr<SWidget> FLightmapCustomNodeBuilder::OnGetLightMapContextMenuContent(TSharedPtr<FLightmapItem> Lightmap)
{
	if (Lightmap.IsValid() )
	{
		FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, NULL);

		MenuBuilder.BeginSection("LightMapsContextMenuSection", LOCTEXT("LightMapsContextMenuHeading", "Options") );
		{
			MenuBuilder.AddMenuEntry(
				LOCTEXT("ViewLightmapLabel", "View Lightmap"),
				LOCTEXT("ViewLightmapTooltip", "Opens the texture editor with this lightmap."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &FLightmapCustomNodeBuilder::ExecuteViewLightmap, Lightmap->ObjectPath))
				);
		}
		MenuBuilder.EndSection(); //LightMapsContextMenuSection

		return MenuBuilder.MakeWidget();
	}

	return nullptr;
}


FReply FLightmapCustomNodeBuilder::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent, TWeakPtr<FLightmapItem> Lightmap)
{
	if(MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		TSharedPtr<SWidget> MenuContent = OnGetLightMapContextMenuContent(Lightmap.Pin());

		if(MenuContent.IsValid() && MouseEvent.GetEventPath() != nullptr)
		{
			const FVector2D& SummonLocation = MouseEvent.GetScreenSpacePosition();
			FWidgetPath WidgetPath = *MouseEvent.GetEventPath();
			FSlateApplication::Get().PushMenu(WidgetPath.Widgets.Last().Widget, WidgetPath, MenuContent.ToSharedRef(), SummonLocation, FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
		}

		return FReply::Handled();
	}

	return FReply::Unhandled();
}

FReply FLightmapCustomNodeBuilder::OnLightMapListMouseButtonDoubleClick(const FGeometry& MyGeom, const FPointerEvent& PointerEvent, TWeakPtr<FLightmapItem> SelectedLightmap)
{
	if ( ensure(SelectedLightmap.IsValid()) )
	{
		ExecuteViewLightmap(SelectedLightmap.Pin()->ObjectPath);
	}

	return FReply::Handled();
}


void FLightmapCustomNodeBuilder::ExecuteViewLightmap(FString SelectedLightmapPath)
{
	UObject* LightMapObject = FindObject<UObject>(NULL, *SelectedLightmapPath);
	if ( LightMapObject )
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(LightMapObject);
	}
}

void FLightmapCustomNodeBuilder::RefreshLightmapItems()
{
	LightmapItems.Empty();

	FWorldContext& Context = GEditor->GetEditorWorldContext();
	UWorld* World = Context.World();
	if ( World )
	{
		TArray<UTexture2D*> LightMapsAndShadowMaps;
		World->GetLightMapsAndShadowMaps(World->GetCurrentLevel(), LightMapsAndShadowMaps, false);

		for ( auto ObjIt = LightMapsAndShadowMaps.CreateConstIterator(); ObjIt; ++ObjIt )
		{
			UTexture2D* CurrentObject = *ObjIt;
			if (CurrentObject)
			{
				FAssetData AssetData = FAssetData(CurrentObject);
				const uint32 ThumbnailResolution = 64;
				TSharedPtr<FAssetThumbnail> LightMapThumbnail = MakeShareable( new FAssetThumbnail( AssetData, ThumbnailResolution, ThumbnailResolution, ThumbnailPool ) );
				TSharedPtr<FLightmapItem> NewItem = MakeShareable( new FLightmapItem(CurrentObject->GetPathName(), LightMapThumbnail) );
				LightmapItems.Add(NewItem);
			}
		}
	}
}


#undef LOCTEXT_NAMESPACE
