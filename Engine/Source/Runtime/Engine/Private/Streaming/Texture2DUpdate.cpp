// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
Texture2DUpdate.cpp: Helpers to stream in and out mips.
=============================================================================*/

#include "Streaming/Texture2DUpdate.h"
#include "RenderUtils.h"
#include "Containers/ResourceArray.h"
#include "Streaming/RenderAssetUpdate.inl"

// Instantiate TRenderAssetUpdate for FTexture2DUpdateContext
template class TRenderAssetUpdate<FTexture2DUpdateContext>;

#if STATS
extern volatile int64 GPending2DUpdateCount;
volatile int64 GPending2DUpdateCount = 0;
#endif

FTexture2DUpdateContext::FTexture2DUpdateContext(const UTexture2D* InTexture, EThreadType InCurrentThread)
	: Texture(InTexture)
	, CurrentThread(InCurrentThread)
{
	check(InTexture);
	checkSlow(InCurrentThread != FTexture2DUpdate::TT_Render || IsInRenderingThread());
	Resource = Texture && Texture->GetResource() ? const_cast<UTexture2D*>(Texture)->GetResource()->GetTexture2DResource() : nullptr;
	if (Resource)
	{
		MipsView = Resource->GetPlatformMipsView();
	}
}

FTexture2DUpdateContext::FTexture2DUpdateContext(const UStreamableRenderAsset* InTexture, EThreadType InCurrentThread)
	: FTexture2DUpdateContext(CastChecked<UTexture2D>(InTexture), InCurrentThread)
{}

FTexture2DUpdate::FTexture2DUpdate(UTexture2D* InTexture) 
	: TRenderAssetUpdate<FTexture2DUpdateContext>(InTexture)
{
	if (!InTexture->GetResource())
	{
		bIsCancelled = true;
	}

	STAT(FPlatformAtomics::InterlockedIncrement(&GPending2DUpdateCount));
}

FTexture2DUpdate::~FTexture2DUpdate()
{
	ensure(!IntermediateTextureRHI);

	STAT(FPlatformAtomics::InterlockedDecrement(&GPending2DUpdateCount));
}


// ****************************
// ********* Helpers **********
// ****************************

void FTexture2DUpdate::DoAsyncReallocate(const FContext& Context)
{
	check(Context.CurrentThread == TT_Render);

	if (!IsCancelled() && Context.Texture && Context.Resource)
	{
		const FTexture2DMipMap& RequestedMipMap = *Context.MipsView[PendingFirstLODIdx];

		TaskSynchronization.Set(1);

		ensure(!IntermediateTextureRHI);

		IntermediateTextureRHI = RHIAsyncReallocateTexture2D(
			Context.Resource->GetTexture2DRHI(),
			ResourceState.NumRequestedLODs,
			RequestedMipMap.SizeX,
			RequestedMipMap.SizeY,
			&TaskSynchronization);
	}
}


//  Transform the texture into a virtual texture.
void FTexture2DUpdate::DoConvertToVirtualWithNewMips(const FContext& Context)
{
	check(Context.CurrentThread == TT_Render);

	if (!IsCancelled() && Context.Resource)
	{
		// If the texture is not virtual, then make it virtual immediately.
		if (!Context.Resource->IsTextureRHIPartiallyResident())
		{
			const FTexture2DMipMap& MipMap0 = *Context.MipsView[0];

			ensure(!IntermediateTextureRHI);

			// Create a copy of the texture that is a virtual texture.
			FRHIResourceCreateInfo CreateInfo(TEXT("FTexture2DUpdate"), Context.Resource->ResourceMem);
			IntermediateTextureRHI = RHICreateTexture2D(
				MipMap0.SizeX, 
				MipMap0.SizeY, 
				Context.Resource->GetPixelFormat(), 
				ResourceState.MaxNumLODs, 
				1, 
				Context.Resource->GetCreationFlags() | TexCreate_Virtual, 
				CreateInfo);
			RHIVirtualTextureSetFirstMipInMemory(IntermediateTextureRHI, CurrentFirstLODIdx);
			RHIVirtualTextureSetFirstMipVisible(IntermediateTextureRHI, CurrentFirstLODIdx);
			RHICopySharedMips(IntermediateTextureRHI, Context.Resource->GetTexture2DRHI());
		}
		else
		{
			// Otherwise the current texture is already virtual and we can update it directly.
			IntermediateTextureRHI = Context.Resource->GetTexture2DRHI();
		}
		RHIVirtualTextureSetFirstMipInMemory(IntermediateTextureRHI, PendingFirstLODIdx);
	}
}

bool FTexture2DUpdate::DoConvertToNonVirtual(const FContext& Context)
{
	check(Context.CurrentThread == TT_Render);

	// If the texture is virtual, then create a new copy of the texture.
	if (!IsCancelled() && !IntermediateTextureRHI && Context.Texture && Context.Resource)
	{
		if (Context.Resource->IsTextureRHIPartiallyResident())
		{
			const FTexture2DMipMap& PendingFirstMipMap = *Context.MipsView[PendingFirstLODIdx];

			ensure(!IntermediateTextureRHI);
			FRHIResourceCreateInfo CreateInfo(TEXT("FTexture2DUpdate"), Context.Resource->ResourceMem);
			IntermediateTextureRHI = RHICreateTexture2D(
				PendingFirstMipMap.SizeX, 
				PendingFirstMipMap.SizeY, 
				Context.Resource->GetPixelFormat(), 
				ResourceState.NumRequestedLODs,
				1, 
				Context.Resource->GetCreationFlags(), 
				CreateInfo);
			RHICopySharedMips(IntermediateTextureRHI, Context.Resource->GetTexture2DRHI());

			return true;
		}
	}
	return false;
}

void FTexture2DUpdate::DoFinishUpdate(const FContext& Context)
{
	check(Context.CurrentThread == TT_Render);

	if (IntermediateTextureRHI && Context.Resource)
	{
		if (!IsCancelled())
		{
			Context.Resource->FinalizeStreaming(IntermediateTextureRHI);
			MarkAsSuccessfullyFinished();
		}
		IntermediateTextureRHI.SafeRelease();

	}
}
