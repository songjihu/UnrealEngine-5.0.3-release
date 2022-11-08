// Copyright Epic Games, Inc. All Rights Reserved.

#include "ThumbnailRendering/SkeletalMeshThumbnailRenderer.h"
#include "Misc/App.h"
#include "ShowFlags.h"
#include "SceneView.h"
#include "Engine/SkeletalMesh.h"
#include "ThumbnailHelpers.h"

USkeletalMeshThumbnailRenderer::USkeletalMeshThumbnailRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void USkeletalMeshThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	TSharedRef<FSkeletalMeshThumbnailScene> ThumbnailScene = ThumbnailSceneCache.EnsureThumbnailScene(Object);

	if(SkeletalMesh)
	{
		ThumbnailScene->SetSkeletalMesh(SkeletalMesh);
	}
	AddAdditionalPreviewSceneContent(Object, ThumbnailScene->GetWorld());

	FSceneViewFamilyContext ViewFamily( FSceneViewFamily::ConstructionValues( RenderTarget, ThumbnailScene->GetScene(), FEngineShowFlags(ESFIM_Game) )
		.SetTime(UThumbnailRenderer::GetTime())
		.SetAdditionalViewFamily(bAdditionalViewFamily));

	ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
	ViewFamily.EngineShowFlags.MotionBlur = 0;
	ViewFamily.EngineShowFlags.LOD = 0;

	RenderViewFamily(Canvas, &ViewFamily, ThumbnailScene->CreateView(&ViewFamily, X, Y, Width, Height));
	ThumbnailScene->SetSkeletalMesh(nullptr);
}

bool USkeletalMeshThumbnailRenderer::AllowsRealtimeThumbnails(UObject* Object) const
{
	if (!Super::AllowsRealtimeThumbnails(Object))
	{
		return false;
	}
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	return SkeletalMesh && SkeletalMesh->GetResourceForRendering();
}

void USkeletalMeshThumbnailRenderer::BeginDestroy()
{
	ThumbnailSceneCache.Clear();

	Super::BeginDestroy();
}
