// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothingPaintEditMode.h"
#include "IPersonaPreviewScene.h"
#include "AssetEditorModeManager.h"
#include "EngineUtils.h"

#include "Animation/DebugSkelMeshComponent.h"

#include "ClothPainter.h"
#include "ComponentReregisterContext.h"
#include "ClothingAssetBase.h"
#include "ComponentRecreateRenderStateContext.h"
#include "IPersonaToolkit.h"
#include "ClothingAsset.h"
#include "EditorViewportClient.h"
#include "AssetViewerSettings.h"
#include "Editor/EditorPerProjectUserSettings.h"

FClothingPaintEditMode::FClothingPaintEditMode()
{
	
}

FClothingPaintEditMode::~FClothingPaintEditMode()
{
	if(ClothPainter.IsValid())
	{
		// Drop the reference
		ClothPainter = nullptr;
	}
}

class IPersonaPreviewScene* FClothingPaintEditMode::GetAnimPreviewScene() const
{
	return static_cast<IPersonaPreviewScene*>(static_cast<FAssetEditorModeManager*>(Owner)->GetPreviewScene());
}

void FClothingPaintEditMode::Initialize()
{
	ClothPainter = MakeShared<FClothPainter>();
	MeshPainter = ClothPainter.Get();

	ClothPainter->Init();
}

TSharedPtr<class FModeToolkit> FClothingPaintEditMode::GetToolkit()
{
	return nullptr;
}

void FClothingPaintEditMode::SetPersonaToolKit(class TSharedPtr<IPersonaToolkit> InToolkit)
{
	PersonaToolkit = InToolkit;
}

void FClothingPaintEditMode::Enter()
{
	IMeshPaintEdMode::Enter();

	for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
	{
		if (!ViewportClient || ViewportClient->GetModeTools() != GetModeManager() )
		{
			continue;
		}

		ViewportClient->EngineShowFlags.DisableAdvancedFeatures();
	}

	IPersonaPreviewScene* Scene = GetAnimPreviewScene();
	if (Scene)
	{
		ClothPainter->SetSkeletalMeshComponent(Scene->GetPreviewMeshComponent());
	}
	
	ClothPainter->EnterPaintMode();
}

void FClothingPaintEditMode::Exit()
{
	IPersonaPreviewScene* Scene = GetAnimPreviewScene();
	if (Scene)
	{
		UDebugSkelMeshComponent* MeshComponent = Scene->GetPreviewMeshComponent();

		if(MeshComponent)
		{
			MeshComponent->bDisableClothSimulation = false;

			if(USkeletalMesh* SkelMesh = MeshComponent->SkeletalMesh)
			{
				for(UClothingAssetBase* AssetBase : SkelMesh->GetMeshClothingAssets())
				{
					UClothingAssetCommon* ConcreteAsset = CastChecked<UClothingAssetCommon>(AssetBase);
					constexpr bool bUpdateFixedVertData = true;
					constexpr bool bInvalidateDerivedDataCache = true;
					ConcreteAsset->ApplyParameterMasks(bUpdateFixedVertData, bInvalidateDerivedDataCache);
				}
			}

			MeshComponent->ResetMeshSectionVisibility();
			MeshComponent->SelectedClothingGuidForPainting = FGuid();
			MeshComponent->SelectedClothingLodForPainting = INDEX_NONE;
			MeshComponent->SelectedClothingLodMaskForPainting = INDEX_NONE;
		}
	}

	if(PersonaToolkit.IsValid())
	{
		if(USkeletalMesh* SkelMesh = PersonaToolkit.Pin()->GetPreviewMesh())
		{
			for(TObjectIterator<USkeletalMeshComponent> It; It; ++It)
			{
				USkeletalMeshComponent* Component = *It;
				if(Component && !Component->IsTemplate() && Component->SkeletalMesh == SkelMesh)
				{
					Component->ReregisterComponent();
				}
			}
		}
	}

	for (FEditorViewportClient* ViewportClient : GEditor->GetAllViewportClients())
	{
		if (!ViewportClient || ViewportClient->GetModeTools() != GetModeManager() )
		{
			continue;
		}

		const bool bEnablePostProcessing = UAssetViewerSettings::Get()->Profiles[GetMutableDefault<UEditorPerProjectUserSettings>()->AssetViewerProfileIndex].bPostProcessingEnabled;

		if ( bEnablePostProcessing )
		{
			ViewportClient->EngineShowFlags.EnableAdvancedFeatures();
		}
		else
		{
			ViewportClient->EngineShowFlags.DisableAdvancedFeatures();
		}
	}

	ClothPainter->ExitPaintMode();

	IMeshPaintEdMode::Exit();
}
