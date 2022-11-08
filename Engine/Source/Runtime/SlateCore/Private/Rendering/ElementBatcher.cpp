// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/ElementBatcher.h"
#include "Fonts/SlateFontInfo.h"
#include "Fonts/FontCache.h"
#include "Rendering/DrawElements.h"
#include "Rendering/RenderingPolicy.h"
#include "Widgets/SWindow.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "Debugging/SlateDebugging.h"
#include "Widgets/SWidgetUtils.h"

CSV_DECLARE_CATEGORY_MODULE_EXTERN(SLATECORE_API, Slate);

DECLARE_CYCLE_STAT(TEXT("Slate RT: Create Batches"), STAT_SlateRTCreateBatches, STATGROUP_Slate);

DEFINE_STAT(STAT_SlateAddElements);

DEFINE_STAT(STAT_SlateElements);
DEFINE_STAT(STAT_SlateElements_Box);
DEFINE_STAT(STAT_SlateElements_Border);
DEFINE_STAT(STAT_SlateElements_Text);
DEFINE_STAT(STAT_SlateElements_ShapedText);
DEFINE_STAT(STAT_SlateElements_Line);
DEFINE_STAT(STAT_SlateElements_Other);
DEFINE_STAT(STAT_SlateInvalidation_RecachedElements);

int32 GSlateFeathering = 0;


FSlateElementBatch::FSlateElementBatch(const FSlateShaderResource* InShaderResource, const FShaderParams& InShaderParams, ESlateShader ShaderType, ESlateDrawPrimitive PrimitiveType, ESlateDrawEffect InDrawEffects, ESlateBatchDrawFlag InBatchFlags, const FSlateDrawElement& InDrawElement, int32 InstanceCount, uint32 InstanceOffset, ISlateUpdatableInstanceBuffer* InstanceData)
	: BatchKey(InShaderParams, ShaderType, PrimitiveType, InDrawEffects, InBatchFlags, InDrawElement.GetClippingHandle(), InstanceCount, InstanceOffset, InstanceData, InDrawElement.GetSceneIndex())
	, ShaderResource(InShaderResource)
	, NumElementsInBatch(0)
	, VertexArrayIndex(INDEX_NONE)
	, IndexArrayIndex(INDEX_NONE)
{
}

FSlateElementBatch::FSlateElementBatch(TWeakPtr<ICustomSlateElement, ESPMode::ThreadSafe> InCustomDrawer, const FSlateDrawElement& InDrawElement)
	: BatchKey(InCustomDrawer, InDrawElement.GetClippingHandle())
	, ShaderResource(nullptr)
	, NumElementsInBatch(0)
	, VertexArrayIndex(INDEX_NONE)
	, IndexArrayIndex(INDEX_NONE)
{
}

void FSlateElementBatch::SaveClippingState(const TArray<FSlateClippingState>& PrecachedClipStates)
{
	/*// Do cached first
	if (BatchKey.ClipStateHandle.GetCachedClipState().IsSet())
	{
		const TSharedPtr<FSlateClippingState>& CachedState = BatchKey.ClipStateHandle.GetCachedClipState().GetValue();
		if (CachedState.IsValid())
		{
			ClippingState = *CachedState;
		}
	}
	else if (PrecachedClipStates.IsValidIndex(BatchKey.ClipStateHandle.GetPrecachedClipIndex()))
	{
		// Store the clipping state so we can use it later for rendering.
		ClippingState = PrecachedClipStates[BatchKey.ClipStateHandle.GetPrecachedClipIndex()];
	}*/
}

void FSlateBatchData::ResetData()
{
	RenderBatches.Reset();
	UncachedSourceBatchIndices.Reset();
	UncachedSourceBatchVertices.Reset();
	FinalIndexData.Reset();
	FinalVertexData.Reset();

	FirstRenderBatchIndex = INDEX_NONE;

	NumBatches = 0;
	NumLayers = 0;

	bIsStencilBufferRequired = false;
}

#define MAX_VERT_ARRAY_RECYCLE (200)
#define MAX_INDEX_ARRAY_RECYCLE (500)

bool FSlateBatchData::IsStencilClippingRequired() const
{
	return bIsStencilBufferRequired;
}

FSlateRenderBatch& FSlateBatchData::AddRenderBatch(int32 InLayer, const FShaderParams& InShaderParams, const FSlateShaderResource* InResource, ESlateDrawPrimitive InPrimitiveType, ESlateShader InShaderType, ESlateDrawEffect InDrawEffects, ESlateBatchDrawFlag InDrawFlags, int8 SceneIndex)
{
	return RenderBatches.Emplace_GetRef(InLayer, InShaderParams, InResource, InPrimitiveType, InShaderType, InDrawEffects, InDrawFlags, SceneIndex, &UncachedSourceBatchVertices, &UncachedSourceBatchIndices, UncachedSourceBatchVertices.Num(), UncachedSourceBatchIndices.Num());
}

void FSlateBatchData::AddCachedBatches(const TSparseArray<FSlateRenderBatch>& InCachedBatches)
{
	RenderBatches.Reserve(RenderBatches.Num() + InCachedBatches.Num());

	for (const FSlateRenderBatch& CachedBatch : InCachedBatches)
	{
		RenderBatches.Add(CachedBatch);
	}
}

void FSlateBatchData::FillBuffersFromNewBatch(FSlateRenderBatch& Batch, FSlateVertexArray& FinalVertices, FSlateIndexArray& FinalIndices)
{
	if(Batch.HasVertexData())
	{
		const int32 SourceVertexOffset = Batch.VertexOffset;
		const int32 SourceIndexOffset = Batch.IndexOffset;

		// At the start of a new batch, just direct copy the verts
		// todo: May need to change this to use absolute indices
		Batch.VertexOffset = FinalVertices.Num();
		Batch.IndexOffset = FinalIndices.Num();
		
		FinalVertices.Append(&(*Batch.SourceVertices)[SourceVertexOffset], Batch.NumVertices);
		FinalIndices.Append(&(*Batch.SourceIndices)[SourceIndexOffset], Batch.NumIndices);
	}
}

void FSlateBatchData::CombineBatches(FSlateRenderBatch& FirstBatch, FSlateRenderBatch& SecondBatch, FSlateVertexArray& FinalVertices, FSlateIndexArray& FinalIndices)
{
	check(!SecondBatch.bIsMerged);
	if (FirstBatch.HasVertexData() || SecondBatch.HasVertexData())
	{
		// when merging verts we have to offset the indices in the second batch based on the first batches existing number of verts
		const int32 BatchOffset = FirstBatch.NumVertices;

		// Final vertices is assumed to have the first batch already in it
		FirstBatch.NumVertices += SecondBatch.NumVertices;
		FirstBatch.NumIndices += SecondBatch.NumIndices;

		FinalVertices.Append(&(*SecondBatch.SourceVertices)[SecondBatch.VertexOffset], SecondBatch.NumVertices);

		FinalIndices.Reserve(FinalIndices.Num()+SecondBatch.NumIndices);
		
		// Get source indices at the source index offset and shift each index by the batches current offset
		for (int32 i = 0; i < SecondBatch.NumIndices; ++i)
		{
			const int32 FinalIndex = (*SecondBatch.SourceIndices)[i + SecondBatch.IndexOffset] + BatchOffset;
			FinalIndices.Add(FinalIndex);
		}
	}

	SecondBatch.bIsMerged = true;
}


void FSlateBatchData::MergeRenderBatches()
{
	SCOPE_CYCLE_COUNTER(STAT_SlateRTCreateBatches);

	if(RenderBatches.Num())
	{
		TArray<TPair<int32, int32>, TInlineAllocator<100, TMemStackAllocator<>>> BatchIndices;

		{
			SCOPED_NAMED_EVENT_TEXT("Slate::SortRenderBatches", FColor::Magenta);

			// Sort an index array instead of the render batches since they are large and not trivially relocatable 
			BatchIndices.AddUninitialized(RenderBatches.Num());
			for (int32 Index = 0; Index < RenderBatches.Num(); ++Index)
			{
				BatchIndices[Index].Key = Index;
				BatchIndices[Index].Value = RenderBatches[Index].GetLayer();
			}

			// Stable sort because order in the same layer should be preserved
			BatchIndices.StableSort
			(
				[](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
				{
					return A.Value < B.Value;
				}
			);
		}


		NumBatches = 0;
		NumLayers = 0;

#if STATS
		int32 CurLayerId = INDEX_NONE;
		int32 PrevLayerId = INDEX_NONE;
#endif

		FirstRenderBatchIndex = BatchIndices[0].Key;

		FSlateRenderBatch* PrevBatch = nullptr;
		for (int32 BatchIndex = 0; BatchIndex < BatchIndices.Num(); ++BatchIndex)
		{
			const TPair<int32, int32>& BatchIndexPair = BatchIndices[BatchIndex];

			FSlateRenderBatch& CurBatch = RenderBatches[BatchIndexPair.Key];


			if (CurBatch.bIsMerged || !CurBatch.IsValidForRendering())
			{
				// skip already merged batches or batches with invalid data (e.g text with pure whitespace)
				continue;
			}

#if STATS
			CurLayerId = CurBatch.GetLayer();
			if (PrevLayerId != CurLayerId)
			{
				++NumLayers;
			}
			CurLayerId = PrevLayerId;
#endif

			if (PrevBatch != nullptr)
			{
				PrevBatch->NextBatchIndex = BatchIndexPair.Key;
			}

			++NumBatches;

			FillBuffersFromNewBatch(CurBatch, FinalVertexData, FinalIndexData);

			if (CurBatch.ClippingState)
			{
				bIsStencilBufferRequired |= CurBatch.ClippingState->GetClippingMethod() == EClippingMethod::Stencil;
			}

#if 1  // Do batching at all?

			if (CurBatch.bIsMergable)
			{
				for (int32 TestIndex = BatchIndex + 1; TestIndex < BatchIndices.Num(); ++TestIndex)
				{
					const TPair<int32, int32>& NextBatchIndexPair = BatchIndices[TestIndex];
					FSlateRenderBatch& TestBatch = RenderBatches[NextBatchIndexPair.Key];
					if (TestBatch.GetLayer() != CurBatch.GetLayer())
					{
						// none of the batches will be compatible since we encountered an incompatible layer
						break;
					}
					else if (!TestBatch.bIsMerged && CurBatch.IsBatchableWith(TestBatch))
					{
						CombineBatches(CurBatch, TestBatch, FinalVertexData, FinalIndexData);

						check(TestBatch.NextBatchIndex == INDEX_NONE);

					}
				}
			}
#endif
			PrevBatch = &CurBatch;
		}
	}
}

FSlateElementBatcher::FSlateElementBatcher( TSharedRef<FSlateRenderingPolicy> InRenderingPolicy )
	: BatchData( nullptr )
	, CurrentCachedElementList( nullptr )
	, PrecachedClippingStates(nullptr)
	, RenderingPolicy( &InRenderingPolicy.Get() )
	, NumPostProcessPasses(0)
	, PixelCenterOffset( InRenderingPolicy->GetPixelCenterOffset() )
	, bSRGBVertexColor( !InRenderingPolicy->IsVertexColorInLinearSpace() )
	, bRequiresVsync(false)
{
}

FSlateElementBatcher::~FSlateElementBatcher()
{
}

void FSlateElementBatcher::AddElements(FSlateWindowElementList& WindowElementList)
{
	SCOPED_NAMED_EVENT_TEXT("Slate::AddElements", FColor::Magenta);

	SCOPE_CYCLE_COUNTER(STAT_SlateAddElements);

#if STATS
	ElementStat_Other = 0;
	ElementStat_Boxes = 0;
	ElementStat_Borders = 0;
	ElementStat_Text = 0;
	ElementStat_ShapedText = 0;
	ElementStat_Line = 0;
	ElementStat_RecachedElements = 0;
#endif

	BatchData = &WindowElementList.GetBatchData();
	check(BatchData->GetRenderBatches().Num() == 0);


	FVector2D ViewportSize = WindowElementList.GetPaintWindow()->GetViewportSize();

	PrecachedClippingStates = &WindowElementList.ClippingManager.GetClippingStates();

	AddElementsInternal(WindowElementList.GetUncachedDrawElements(), ViewportSize);

	const TArrayView<FSlateCachedElementData* const> CachedElementDataList = WindowElementList.GetCachedElementDataList();


	if(CachedElementDataList.Num())
	{
		SCOPED_NAMED_EVENT_TEXT("Slate::AddCachedElements", FColor::Magenta);

		for (FSlateCachedElementData* CachedElementData : CachedElementDataList)
		{
			if (CachedElementData)
			{
				AddCachedElements(*CachedElementData, ViewportSize);
			}
		}
	}

	// Done with the element list
	BatchData = nullptr;
	PrecachedClippingStates = nullptr;

#if STATS
	const int32 ElementStat_All =
		ElementStat_Boxes +
		ElementStat_Borders +
		ElementStat_Text +
		ElementStat_ShapedText +
		ElementStat_Line +
		ElementStat_Other;

	INC_DWORD_STAT_BY(STAT_SlateElements, ElementStat_All);
	INC_DWORD_STAT_BY(STAT_SlateElements_Box, ElementStat_Boxes);
	INC_DWORD_STAT_BY(STAT_SlateElements_Border, ElementStat_Borders);
	INC_DWORD_STAT_BY(STAT_SlateElements_Text, ElementStat_Text);
	INC_DWORD_STAT_BY(STAT_SlateElements_ShapedText, ElementStat_ShapedText);
	INC_DWORD_STAT_BY(STAT_SlateElements_Line, ElementStat_Line);
	INC_DWORD_STAT_BY(STAT_SlateElements_Other, ElementStat_Other);
	INC_DWORD_STAT_BY(STAT_SlateInvalidation_RecachedElements, ElementStat_RecachedElements);
#endif
}


void FSlateElementBatcher::AddElementsInternal(const FSlateDrawElementArray& DrawElements, const FVector2D& ViewportSize)
{
	for (const FSlateDrawElement& DrawElement : DrawElements)
	{
		// Determine what type of element to add
		switch ( DrawElement.GetElementType() )
		{
		case EElementType::ET_Box:
		case EElementType::ET_RoundedBox:	
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddBoxElement", FColor::Magenta);
			STAT(ElementStat_Boxes++);
			DrawElement.IsPixelSnapped() ? AddBoxElement<ESlateVertexRounding::Enabled>(DrawElement) : AddBoxElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Border:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddBorderElement", FColor::Magenta);
			STAT(ElementStat_Borders++);
			DrawElement.IsPixelSnapped() ? AddBorderElement<ESlateVertexRounding::Enabled>(DrawElement) : AddBorderElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Text:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddTextElement", FColor::Magenta);
			STAT(ElementStat_Text++);
			DrawElement.IsPixelSnapped() ? AddTextElement<ESlateVertexRounding::Enabled>(DrawElement) : AddTextElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_ShapedText:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddShapedTextElement", FColor::Magenta);
			STAT(ElementStat_ShapedText++);
			DrawElement.IsPixelSnapped() ? AddShapedTextElement<ESlateVertexRounding::Enabled>(DrawElement) : AddShapedTextElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Line:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddLineElement", FColor::Magenta);
			STAT(ElementStat_Line++);
			DrawElement.IsPixelSnapped() ? AddLineElement<ESlateVertexRounding::Enabled>(DrawElement) : AddLineElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_DebugQuad:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddDebugQuadElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			DrawElement.IsPixelSnapped() ? AddDebugQuadElement<ESlateVertexRounding::Enabled>(DrawElement) : AddDebugQuadElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Spline:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddSplineElement", FColor::Magenta);
			// Note that we ignore pixel snapping here; see implementation for more info.
			STAT(ElementStat_Other++);
			AddSplineElement(DrawElement);
		}
			break;
		case EElementType::ET_Gradient:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddGradientElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			DrawElement.IsPixelSnapped() ? AddGradientElement<ESlateVertexRounding::Enabled>(DrawElement) : AddGradientElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Viewport:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddViewportElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			DrawElement.IsPixelSnapped() ? AddViewportElement<ESlateVertexRounding::Enabled>(DrawElement) : AddViewportElement<ESlateVertexRounding::Disabled>(DrawElement);
		}
			break;
		case EElementType::ET_Custom:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddCustomElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			AddCustomElement(DrawElement);
		}
			break;
		case EElementType::ET_CustomVerts:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddCustomVertsElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			AddCustomVerts(DrawElement);
		}
			break;
	
		case EElementType::ET_PostProcessPass:
		{
			SCOPED_NAMED_EVENT_TEXT("Slate::AddPostProcessElement", FColor::Magenta);
			STAT(ElementStat_Other++);
			AddPostProcessPass(DrawElement, ViewportSize);
		}
			break;
		default:
			checkf(0, TEXT("Invalid element type"));
			break;
		}
	}
}

void FSlateElementBatcher::AddCachedElements(FSlateCachedElementData& CachedElementData, const FVector2D& ViewportSize)
{
	CSV_SCOPED_TIMING_STAT(Slate, AddCachedElements);
	SCOPED_NAMED_EVENT_TEXT("Slate::AddCachedElements", FColor::Magenta);

#if SLATE_CSV_TRACKER
	FCsvProfiler::RecordCustomStat("Paint/CacheListsWithNewData", CSV_CATEGORY_INDEX(Slate), CachedElementData.ListsWithNewData.Num(), ECsvCustomStatOp::Set);
	int32 RecachedDrawElements = 0;
	int32 RecachedEmptyDrawLists = 0;
#endif

	for (FSlateCachedElementList* List : CachedElementData.ListsWithNewData)
	{
		if (List->DrawElements.Num() > 0)
		{
			STAT(ElementStat_RecachedElements += List->DrawElements.Num());

#if SLATE_CSV_TRACKER
			RecachedDrawElements += List->DrawElements.Num();
#endif

			CurrentCachedElementList = List;
			{
				SCOPE_CYCLE_SWIDGET(List->OwningWidget);
				AddElementsInternal(List->DrawElements, ViewportSize);
			}
			CurrentCachedElementList = nullptr;
		}
#if SLATE_CSV_TRACKER
		else
		{
			RecachedEmptyDrawLists++;
		}
#endif
	}
	CachedElementData.ListsWithNewData.Empty();

	// Add the existing and new cached batches.
	BatchData->AddCachedBatches(CachedElementData.GetCachedBatches());

	CachedElementData.CleanupUnusedClipStates();

#if SLATE_CSV_TRACKER
	FCsvProfiler::RecordCustomStat("Paint/RecachedElements", CSV_CATEGORY_INDEX(Slate), RecachedDrawElements, ECsvCustomStatOp::Accumulate);
	FCsvProfiler::RecordCustomStat("Paint/RecachedEmptyDrawLists", CSV_CATEGORY_INDEX(Slate), RecachedEmptyDrawLists, ECsvCustomStatOp::Accumulate);
#endif
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddDebugQuadElement(const FSlateDrawElement& DrawElement)
{
	const FSlateBoxPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateBoxPayload>();

	const FColor Tint = PackVertexColor(DrawElementPayload.GetTint());
	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FVector2D& LocalSize = DrawElement.GetLocalSize();
	ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();

	FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, FShaderParams(), nullptr, ESlateDrawPrimitive::TriangleList, ESlateShader::Default, ESlateDrawEffect::None, ESlateBatchDrawFlag::None, DrawElement);
	
	const FColor Color = PackVertexColor(DrawElementPayload.GetTint());

	// Determine the four corners of the quad
	FVector2D TopLeft = FVector2D::ZeroVector;
	FVector2D TopRight = FVector2D(LocalSize.X, 0);
	FVector2D BotLeft = FVector2D(0, LocalSize.Y);
	FVector2D BotRight = FVector2D(LocalSize.X, LocalSize.Y);

	// The start index of these vertices in the index buffer
	//const uint32 IndexStart = BatchVertices.Num();
	const uint32 IndexStart = 0;

	// Add four vertices to the list of verts to be added to the vertex buffer
	RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopLeft), FVector2f(0.0f,0.0f),  Tint));
	RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopRight), FVector2f(1.0f,0.0f), Tint));
	RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotLeft), FVector2f(0.0f,1.0f),  Tint));
	RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotRight), FVector2f(1.0f,1.0f), Tint));

	// Add 6 indices to the vertex buffer.  (2 tri's per quad, 3 indices per tri)
	RenderBatch.AddIndex(IndexStart + 0);
	RenderBatch.AddIndex(IndexStart + 1);
	RenderBatch.AddIndex(IndexStart + 2);

	RenderBatch.AddIndex(IndexStart + 2);
	RenderBatch.AddIndex(IndexStart + 1);
	RenderBatch.AddIndex(IndexStart + 3);
}

FORCEINLINE void IndexQuad(FSlateRenderBatch& RenderBatch, int32 TopLeft, int32 TopRight, int32 BottomRight, int32 BottomLeft)
{
	RenderBatch.AddIndex(TopLeft);
	RenderBatch.AddIndex(TopRight);
	RenderBatch.AddIndex(BottomRight);

	RenderBatch.AddIndex(BottomRight);
	RenderBatch.AddIndex(BottomLeft);
	RenderBatch.AddIndex(TopLeft);
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddBoxElement(const FSlateDrawElement& DrawElement)
{
	const FSlateBoxPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateBoxPayload>();

	const FColor Tint = PackVertexColor(DrawElementPayload.GetTint());
	const FSlateRenderTransform& ElementRenderTransform = DrawElement.GetRenderTransform();
	const FSlateRenderTransform RenderTransform = DrawElement.GetRenderTransform();// GetBoxRenderTransform(DrawElement);
	const FVector2D LocalSize = DrawElement.GetLocalSize();

	const ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();

	const float DrawScale = DrawElement.GetScale();

	// Do pixel snapping
	FVector2D TopLeft(0,0);
	FVector2D BotRight(LocalSize);

	uint32 TextureWidth = 1;
	uint32 TextureHeight = 1;

	// Get the default start and end UV.  If the texture is atlased this value will be a subset of this
	FVector2D StartUV = FVector2D(0.0f,0.0f);
	FVector2D EndUV = FVector2D(1.0f,1.0f);
	FVector2D SizeUV;

	FVector2D HalfTexel;

	const FSlateShaderResourceProxy* ResourceProxy = DrawElementPayload.GetResourceProxy();
	FSlateShaderResource* Resource = nullptr;
	if( ResourceProxy )
	{
		// The actual texture for rendering.  If the texture is atlased this is the atlas
		Resource = ResourceProxy->Resource;
		// The width and height of the texture (non-atlased size)
		TextureWidth = ResourceProxy->ActualSize.X != 0 ? ResourceProxy->ActualSize.X : 1;
		TextureHeight = ResourceProxy->ActualSize.Y != 0 ? ResourceProxy->ActualSize.Y : 1;

		// Texel offset
		HalfTexel = FVector2D( PixelCenterOffset/TextureWidth, PixelCenterOffset/TextureHeight );

		const FBox2D& BrushUV = DrawElementPayload.GetBrushUVRegion();
		//In case brush has valid UV region - use it instead of proxy UV
		if (BrushUV.bIsValid)
		{
			SizeUV = BrushUV.GetSize();
			StartUV = BrushUV.Min + HalfTexel;
			EndUV = StartUV + SizeUV;
		}
		else
		{
			SizeUV = FVector2D(ResourceProxy->SizeUV);
			StartUV = FVector2D(ResourceProxy->StartUV) + HalfTexel;
			EndUV = StartUV + FVector2D(ResourceProxy->SizeUV);
		}
	}
	else
	{
		// no texture
		SizeUV = FVector2D(1.0f,1.0f);
		HalfTexel = FVector2D( PixelCenterOffset, PixelCenterOffset );
	}


	const ESlateBrushTileType::Type TilingRule = DrawElementPayload.GetBrushTiling();
	const bool bTileHorizontal = (TilingRule == ESlateBrushTileType::Both || TilingRule == ESlateBrushTileType::Horizontal);
	const bool bTileVertical = (TilingRule == ESlateBrushTileType::Both || TilingRule == ESlateBrushTileType::Vertical);

	const ESlateBrushMirrorType::Type MirroringRule = DrawElementPayload.GetBrushMirroring();
	const bool bMirrorHorizontal = (MirroringRule == ESlateBrushMirrorType::Both || MirroringRule == ESlateBrushMirrorType::Horizontal);
	const bool bMirrorVertical = (MirroringRule == ESlateBrushMirrorType::Both || MirroringRule == ESlateBrushMirrorType::Vertical);

	// Pass the tiling information as a flag so we can pick the correct texture addressing mode
	ESlateBatchDrawFlag DrawFlags = DrawElement.GetBatchFlags();
	DrawFlags |= ( ( bTileHorizontal ? ESlateBatchDrawFlag::TileU : ESlateBatchDrawFlag::None ) | ( bTileVertical ? ESlateBatchDrawFlag::TileV : ESlateBatchDrawFlag::None ) );

	// Add Shader Parameters for extra RoundedBox parameters
	ESlateShader ShaderType = ESlateShader::Default;
	FShaderParams ShaderParams;
	FColor SecondaryColor;
	if (DrawElement.GetElementType() == EElementType::ET_RoundedBox)
	{
	
		ShaderType = ESlateShader::RoundedBox;
		const FSlateRoundedBoxPayload& RoundedPayload = DrawElement.GetDataPayload<FSlateRoundedBoxPayload>();

		ShaderParams.PixelParams = FVector4f(0, RoundedPayload.GetOutlineWeight(), LocalSize.X, LocalSize.Y);//RadiusWeight;
		ShaderParams.PixelParams2 = RoundedPayload.GetRadius();

		SecondaryColor = PackVertexColor(RoundedPayload.OutlineColor);
	}

	FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, ShaderParams, Resource, ESlateDrawPrimitive::TriangleList, ShaderType, InDrawEffects, DrawFlags, DrawElement);
	 
	float HorizontalTiling = bTileHorizontal ? LocalSize.X / TextureWidth : 1.0f;
	float VerticalTiling = bTileVertical ? LocalSize.Y / TextureHeight : 1.0f;

	const FVector2D Tiling( HorizontalTiling, VerticalTiling );

	// The start index of these vertices in the index buffer
	const uint32 IndexStart = 0;// BatchVertices.Num();


	const FMargin& Margin = DrawElementPayload.GetBrushMargin();

	const FVector2D TopRight = FVector2D(BotRight.X, TopLeft.Y);
	const FVector2D BotLeft = FVector2D(TopLeft.X, BotRight.Y);

	const FColor FeatherColor(0, 0, 0, 0);

	if (DrawElementPayload.GetBrushDrawType() != ESlateBrushDrawType::Image &&
		( Margin.Left != 0.0f || Margin.Top != 0.0f || Margin.Right != 0.0f || Margin.Bottom != 0.0f ) )
	{
		// Create 9 quads for the box element based on the following diagram
		//     ___LeftMargin    ___RightMargin
		//    /                /
		//  +--+-------------+--+
		//  |  |c1           |c2| ___TopMargin
		//  +--o-------------o--+
		//  |  |             |  |
		//  |  |c3           |c4|
		//  +--o-------------o--+
		//  |  |             |  | ___BottomMargin
		//  +--+-------------+--+


		// Determine the texture coordinates for each quad
		// These are not scaled.
		float LeftMarginU = (Margin.Left > 0.0f)
			? StartUV.X + Margin.Left * SizeUV.X + HalfTexel.X
			: StartUV.X;
		float TopMarginV = (Margin.Top > 0.0f)
			? StartUV.Y + Margin.Top * SizeUV.Y + HalfTexel.Y
			: StartUV.Y;
		float RightMarginU = (Margin.Right > 0.0f)
			? EndUV.X - Margin.Right * SizeUV.X + HalfTexel.X
			: EndUV.X;
		float BottomMarginV = (Margin.Bottom > 0.0f)
			? EndUV.Y - Margin.Bottom * SizeUV.Y + HalfTexel.Y
			: EndUV.Y;

		if( bMirrorHorizontal || bMirrorVertical )
		{
			const FVector2D UVMin = StartUV;
			const FVector2D UVMax = EndUV;

			if( bMirrorHorizontal )
			{
				StartUV.X = UVMax.X - ( StartUV.X - UVMin.X );
				EndUV.X = UVMax.X - ( EndUV.X - UVMin.X );
				LeftMarginU = UVMax.X - ( LeftMarginU - UVMin.X );
				RightMarginU = UVMax.X - ( RightMarginU - UVMin.X );
			}
			if( bMirrorVertical )
			{
				StartUV.Y = UVMax.Y - ( StartUV.Y - UVMin.Y );
				EndUV.Y = UVMax.Y - ( EndUV.Y - UVMin.Y );
				TopMarginV = UVMax.Y - ( TopMarginV - UVMin.Y );
				BottomMarginV = UVMax.Y - ( BottomMarginV - UVMin.Y );
			}
		}

		// Determine the margins for each quad

		float LeftMarginX = TextureWidth * Margin.Left;
		float TopMarginY = TextureHeight * Margin.Top;
		float RightMarginX = LocalSize.X - TextureWidth * Margin.Right;
		float BottomMarginY = LocalSize.Y - TextureHeight * Margin.Bottom;

		// If the margins are overlapping the margins are too big or the button is too small
		// so clamp margins to half of the box size
		if( RightMarginX < LeftMarginX )
		{
			LeftMarginX = LocalSize.X / 2;
			RightMarginX = LeftMarginX;
		}

		if( BottomMarginY < TopMarginY )
		{
			TopMarginY = LocalSize.Y / 2;
			BottomMarginY = TopMarginY;
		}

		FVector2D Position = TopLeft;
		FVector2D EndPos = BotRight;

		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( StartUV ),						FVector2f(Tiling)),	Tint, SecondaryColor) ); //0
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( StartUV.X, TopMarginV ),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //1
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( LeftMarginU, StartUV.Y ),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //2
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( LeftMarginU, TopMarginV ),		FVector2f(Tiling)),	Tint, SecondaryColor) ); //3
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, Position.Y ),	FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( RightMarginU, StartUV.Y ),		FVector2f(Tiling)),	Tint, SecondaryColor) ); //4
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, TopMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( RightMarginU,TopMarginV),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //5
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( EndUV.X, StartUV.Y ),				FVector2f(Tiling)),	Tint, SecondaryColor) ); //6
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( EndUV.X, TopMarginV),				FVector2f(Tiling)),	Tint, SecondaryColor) ); //7

		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( StartUV.X, BottomMarginV ),		FVector2f(Tiling)),	Tint, SecondaryColor) ); //8
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( LeftMarginU, BottomMarginV ),		FVector2f(Tiling)),	Tint, SecondaryColor) ); //9
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, BottomMarginY ), FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( RightMarginU, BottomMarginV ),	FVector2f(Tiling)),	Tint, SecondaryColor) ); //10
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, BottomMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( EndUV.X, BottomMarginV ),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //11
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( StartUV.X, EndUV.Y ),				FVector2f(Tiling)),	Tint, SecondaryColor) ); //12
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( LeftMarginU, EndUV.Y ),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //13
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( RightMarginU, EndUV.Y ),			FVector2f(Tiling)),	Tint, SecondaryColor) ); //14
		RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, EndPos.Y ),			FVector2f(LocalSize), DrawScale, FVector4f(FVector2f( EndUV ),							FVector2f(Tiling)),	Tint, SecondaryColor) ); //15

		// Top
		RenderBatch.AddIndex( IndexStart + 0 );
		RenderBatch.AddIndex( IndexStart + 1 );
		RenderBatch.AddIndex( IndexStart + 2 );
		RenderBatch.AddIndex( IndexStart + 2 );
		RenderBatch.AddIndex( IndexStart + 1 );
		RenderBatch.AddIndex( IndexStart + 3 );

		RenderBatch.AddIndex( IndexStart + 2 );
		RenderBatch.AddIndex( IndexStart + 3 );
		RenderBatch.AddIndex( IndexStart + 4 );
		RenderBatch.AddIndex( IndexStart + 4 );
		RenderBatch.AddIndex( IndexStart + 3 );
		RenderBatch.AddIndex( IndexStart + 5 );

		RenderBatch.AddIndex( IndexStart + 4 );
		RenderBatch.AddIndex( IndexStart + 5 );
		RenderBatch.AddIndex( IndexStart + 6 );
		RenderBatch.AddIndex( IndexStart + 6 );
		RenderBatch.AddIndex( IndexStart + 5 );
		RenderBatch.AddIndex( IndexStart + 7 );

		// Middle
		RenderBatch.AddIndex( IndexStart + 1 );
		RenderBatch.AddIndex( IndexStart + 8 );
		RenderBatch.AddIndex( IndexStart + 3 );
		RenderBatch.AddIndex( IndexStart + 3 );
		RenderBatch.AddIndex( IndexStart + 8 );
		RenderBatch.AddIndex( IndexStart + 9 );

		RenderBatch.AddIndex( IndexStart + 3 );
		RenderBatch.AddIndex( IndexStart + 9 );
		RenderBatch.AddIndex( IndexStart + 5 );
		RenderBatch.AddIndex( IndexStart + 5 );
		RenderBatch.AddIndex( IndexStart + 9 );
		RenderBatch.AddIndex( IndexStart + 10 );

		RenderBatch.AddIndex( IndexStart + 5 );
		RenderBatch.AddIndex( IndexStart + 10 );
		RenderBatch.AddIndex( IndexStart + 7 );
		RenderBatch.AddIndex( IndexStart + 7 );
		RenderBatch.AddIndex( IndexStart + 10 );
		RenderBatch.AddIndex( IndexStart + 11 );

		// Bottom
		RenderBatch.AddIndex( IndexStart + 8 );
		RenderBatch.AddIndex( IndexStart + 12 );
		RenderBatch.AddIndex( IndexStart + 9 );
		RenderBatch.AddIndex( IndexStart + 9 );
		RenderBatch.AddIndex( IndexStart + 12 );
		RenderBatch.AddIndex( IndexStart + 13 );

		RenderBatch.AddIndex( IndexStart + 9 );
		RenderBatch.AddIndex( IndexStart + 13 );
		RenderBatch.AddIndex( IndexStart + 10 );
		RenderBatch.AddIndex( IndexStart + 10 );
		RenderBatch.AddIndex( IndexStart + 13 );
		RenderBatch.AddIndex( IndexStart + 14 );

		RenderBatch.AddIndex( IndexStart + 10 );
		RenderBatch.AddIndex( IndexStart + 14 );
		RenderBatch.AddIndex( IndexStart + 11 );
		RenderBatch.AddIndex( IndexStart + 11 );
		RenderBatch.AddIndex( IndexStart + 14 );
		RenderBatch.AddIndex( IndexStart + 15 );

		if ( GSlateFeathering && Rounding == ESlateVertexRounding::Disabled )
		{
			const int32 FeatherStart = RenderBatch.GetNumVertices();

			// Top
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(Position.X, Position.Y) + FVector2f(-1, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //0
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(LeftMarginX, Position.Y) + FVector2f(0, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(LeftMarginU, StartUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //1
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(RightMarginX, Position.Y) + FVector2f(0, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(RightMarginU, StartUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //2
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos.X, Position.Y) + FVector2f(1, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV.X, StartUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //3

			// Left
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(Position.X, TopMarginY) + FVector2f(-1, 0) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV.X, TopMarginV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //4
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(Position.X, BottomMarginY) + FVector2f(-1, 0) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV.X, BottomMarginV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //5

			// Right
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos.X, TopMarginY) + FVector2f(1, 0) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV.X, TopMarginV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //6
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos.X, BottomMarginY) + FVector2f(1, 0) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV.X, BottomMarginV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //7

			// Bottom
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(Position.X, EndPos.Y) + FVector2f(-1, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV.X, EndUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //8
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(LeftMarginX, EndPos.Y) + FVector2f(0, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(LeftMarginU, EndUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //9
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(RightMarginX, EndPos.Y) + FVector2f(0, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(RightMarginU, EndUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //10
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos.X, EndPos.Y) + FVector2f(1, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV), FVector2f(Tiling)), FeatherColor, SecondaryColor)); //11

			// Top Left
			IndexQuad(RenderBatch, FeatherStart + 0, FeatherStart + 1, IndexStart + 2, IndexStart + 0);
			// Top Middle
			IndexQuad(RenderBatch, FeatherStart + 1, FeatherStart + 2, IndexStart + 4, IndexStart + 2);
			// Top Right
			IndexQuad(RenderBatch, FeatherStart + 2, FeatherStart + 3, IndexStart + 6, IndexStart + 4);

			//-----------------------------------------------------------

			// Left Top
			IndexQuad(RenderBatch, FeatherStart + 0, IndexStart + 0, IndexStart + 1, FeatherStart + 4);
			// Left Middle
			IndexQuad(RenderBatch, FeatherStart + 4, IndexStart + 1, IndexStart + 8, FeatherStart + 5);
			// Left Bottom
			IndexQuad(RenderBatch, FeatherStart + 5, IndexStart + 8, IndexStart + 12, FeatherStart + 8);

			//-----------------------------------------------------------

			// Right Top
			IndexQuad(RenderBatch, IndexStart + 6, FeatherStart + 3, FeatherStart + 6, IndexStart + 7);
			// Right Middle
			IndexQuad(RenderBatch, IndexStart + 7, FeatherStart + 6, FeatherStart + 7, IndexStart + 11);
			// Right Bottom
			IndexQuad(RenderBatch, IndexStart + 11, FeatherStart + 7, FeatherStart + 11, IndexStart + 15);

			//-----------------------------------------------------------

			// Bottom Left
			IndexQuad(RenderBatch, IndexStart + 12, IndexStart + 13, FeatherStart + 9, FeatherStart + 8);
			// Bottom Middle
			IndexQuad(RenderBatch, IndexStart + 13, IndexStart + 14, FeatherStart + 10, FeatherStart + 9);
			// Bottom Right
			IndexQuad(RenderBatch, IndexStart + 14, IndexStart + 15, FeatherStart + 11, FeatherStart + 10);
		}
	}
	else
	{
		if( bMirrorHorizontal || bMirrorVertical )
		{
			const FVector2D UVMin = StartUV;
			const FVector2D UVMax = EndUV;

			if( bMirrorHorizontal )
			{
				StartUV.X = UVMax.X - ( StartUV.X - UVMin.X );
				EndUV.X = UVMax.X - ( EndUV.X - UVMin.X );
			}
			if( bMirrorVertical )
			{
				StartUV.Y = UVMax.Y - ( StartUV.Y - UVMin.Y );
				EndUV.Y = UVMax.Y - ( EndUV.Y - UVMin.Y );
			}
		}

		// Add four vertices to the list of verts to be added to the vertex buffer
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopLeft), FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV), FVector2f(Tiling)), Tint, SecondaryColor));
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopRight), FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV.X, StartUV.Y), FVector2f(Tiling)), Tint, SecondaryColor));
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotLeft), FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV.X, EndUV.Y), FVector2f(Tiling)), Tint, SecondaryColor));
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotRight), FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV), FVector2f(Tiling)), Tint, SecondaryColor));

		RenderBatch.AddIndex( IndexStart + 0 );
		RenderBatch.AddIndex( IndexStart + 1 );
		RenderBatch.AddIndex( IndexStart + 2 );

		RenderBatch.AddIndex( IndexStart + 2 );
		RenderBatch.AddIndex( IndexStart + 1 );
		RenderBatch.AddIndex( IndexStart + 3 );

		int32 TopLeftIndex = IndexStart + 0;
		int32 TopRightIndex = IndexStart + 1;
		int32 BottomLeftIndex = IndexStart + 2;
		int32 BottomRightIndex = IndexStart + 3;

		if ( GSlateFeathering && Rounding == ESlateVertexRounding::Disabled )
		{
			const int32 FeatherStart = RenderBatch.GetNumVertices();

			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopLeft) + FVector2f(-1, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV), FVector2f(Tiling)), FeatherColor, SecondaryColor));
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(TopRight) + FVector2f(1, -1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV.X, StartUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor));
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotLeft) + FVector2f(-1, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(StartUV.X, EndUV.Y), FVector2f(Tiling)), FeatherColor, SecondaryColor));
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(BotRight) + FVector2f(1, 1) / DrawScale, FVector2f(LocalSize), DrawScale, FVector4f(FVector2f(EndUV), FVector2f(Tiling)), FeatherColor, SecondaryColor));

			// Top-Top
			RenderBatch.AddIndex(FeatherStart + 0);
			RenderBatch.AddIndex(FeatherStart + 1);
			RenderBatch.AddIndex(TopRightIndex);

			// Top-Bottom
			RenderBatch.AddIndex(FeatherStart + 0);
			RenderBatch.AddIndex(TopRightIndex);
			RenderBatch.AddIndex(TopLeftIndex);

			// Left-Top
			RenderBatch.AddIndex(FeatherStart + 0);
			RenderBatch.AddIndex(BottomLeftIndex);
			RenderBatch.AddIndex(FeatherStart + 2);

			// Left-Bottom
			RenderBatch.AddIndex(FeatherStart + 0);
			RenderBatch.AddIndex(TopLeftIndex);
			RenderBatch.AddIndex(BottomLeftIndex);

			// Right-Top
			RenderBatch.AddIndex(TopRightIndex);
			RenderBatch.AddIndex(FeatherStart + 1);
			RenderBatch.AddIndex(FeatherStart + 3);

			// Right-Bottom
			RenderBatch.AddIndex(TopRightIndex);
			RenderBatch.AddIndex(FeatherStart + 3);
			RenderBatch.AddIndex(BottomRightIndex);

			// Bottom-Top
			RenderBatch.AddIndex(BottomLeftIndex);
			RenderBatch.AddIndex(BottomRightIndex);
			RenderBatch.AddIndex(FeatherStart + 3);

			// Bottom-Bottom
			RenderBatch.AddIndex(FeatherStart + 3);
			RenderBatch.AddIndex(FeatherStart + 2);
			RenderBatch.AddIndex(BottomLeftIndex);
		}
	}
}
namespace SlateElementBatcher
{
#if SLATE_CHECK_UOBJECT_RENDER_RESOURCES
	const FName MaterialInterfaceClassName = "MaterialInterface";

	void CheckUObject(const FSlateTextPayload& InDrawElementPayload, const UObject* InFontMaterial)
	{
		if (InFontMaterial && GSlateCheckUObjectRenderResources)
		{
			bool bIsValidLowLevel = InFontMaterial->IsValidLowLevelFast(false);
			if (!bIsValidLowLevel || !IsValid(InFontMaterial) || InFontMaterial->GetClass()->GetFName() == MaterialInterfaceClassName)
			{
				UE_LOG(LogSlate, Error, TEXT("We are rendering a string with an invalid font. The string is: '%s'")
					, InDrawElementPayload.GetText());
				// We expect to log more info in the SlateMaterialResource.
				//In case we crash before that, we also log some info here.
				UE_LOG(LogSlate, Error, TEXT("Material is not valid. PendingKill:'%d'. ValidLowLevelFast:'%d'. InvalidClass:'%d'")
					, (bIsValidLowLevel ? !IsValid(InFontMaterial) : false)
					, bIsValidLowLevel
					, (bIsValidLowLevel ? InFontMaterial->GetClass()->GetFName() == MaterialInterfaceClassName : false));
			}
		}
	}
#endif
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddTextElement(const FSlateDrawElement& DrawElement)
{
	const FSlateTextPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateTextPayload>();
	FColor BaseTint = PackVertexColor(DrawElementPayload.GetTint());

	const FFontOutlineSettings& OutlineSettings = DrawElementPayload.GetFontInfo().OutlineSettings;

	int32 Len = DrawElementPayload.GetTextLength();
	ensure(Len > 0);

	ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();

	const int32 Layer = DrawElement.GetLayer();

	// extract the layout transform from the draw element
	FSlateLayoutTransform LayoutTransform(DrawElement.GetScale(), DrawElement.GetPosition());

	// We don't just scale up fonts, we draw them in local space pre-scaled so we don't get scaling artifacts.
	// So we need to pull the layout scale out of the layout and render transform so we can apply them
	// in local space with pre-scaled fonts.
	const float FontScale = LayoutTransform.GetScale();
	FSlateLayoutTransform InverseLayoutTransform = Inverse(Concatenate(Inverse(FontScale), LayoutTransform));
	const FSlateRenderTransform RenderTransform = Concatenate(Inverse(FontScale), DrawElement.GetRenderTransform());

	FSlateFontCache& FontCache = *RenderingPolicy->GetFontCache();
	FSlateShaderResourceManager& ResourceManager = *RenderingPolicy->GetResourceManager();

	const UObject* BaseFontMaterial = DrawElementPayload.GetFontInfo().FontMaterial;
	const UObject* OutlineFontMaterial = OutlineSettings.OutlineMaterial;

#if SLATE_CHECK_UOBJECT_RENDER_RESOURCES
	SlateElementBatcher::CheckUObject(DrawElementPayload, BaseFontMaterial);
	SlateElementBatcher::CheckUObject(DrawElementPayload, OutlineFontMaterial);
#endif

	bool bOutlineFont = OutlineSettings.OutlineSize > 0;
	const int32 OutlineSize = OutlineSettings.OutlineSize;

	auto BuildFontGeometry = [&](const FFontOutlineSettings& InOutlineSettings, const FColor& InTint, const UObject* FontMaterial, int32 InLayer, float InOutlineHorizontalOffset)
	{
		FCharacterList& CharacterList = FontCache.GetCharacterList(DrawElementPayload.GetFontInfo(), FontScale, InOutlineSettings);

		float MaxHeight = CharacterList.GetMaxHeight();

		if (MaxHeight == 0)
		{
			// If the max text height is 0, we'll create NaN's further in the code, so avoid drawing text if this happens.
			return;
		}

		uint32 FontTextureIndex = 0;
		FSlateShaderResource* FontAtlasTexture = nullptr;
		FSlateShaderResource* FontShaderResource = nullptr;
		FColor FontTint = InTint;

		FSlateRenderBatch* RenderBatch = nullptr;
		FSlateVertexArray* BatchVertices = nullptr;
		FSlateIndexArray* BatchIndices = nullptr;

		uint32 VertexOffset = 0;
		uint32 IndexOffset = 0;

		float InvTextureSizeX = 0;
		float InvTextureSizeY = 0;

		float LineX = 0;

		FCharacterEntry PreviousCharEntry;

		int32 Kerning = 0;

		FVector2D TopLeft(0, 0);

		const float PosX = TopLeft.X;
		float PosY = TopLeft.Y;

		LineX = PosX;

		const bool bIsFontMaterial = FontMaterial != nullptr;
		const bool bEnableOutline = InOutlineSettings.OutlineSize > 0;

		uint32 NumChars = Len;

		uint32 NumLines = 1;
		for( uint32 CharIndex = 0; CharIndex < NumChars; ++CharIndex )
		{
			const TCHAR CurrentChar = DrawElementPayload.GetText()[ CharIndex ];

			ensure(CurrentChar != '\0');

			const bool IsNewline = (CurrentChar == '\n');

			if (IsNewline)
			{
				// Move down: we are drawing the next line.
				PosY += MaxHeight;
				// Carriage return 
				LineX = PosX;

				++NumLines;

			}
			else
			{
				const FCharacterEntry& Entry = CharacterList.GetCharacter(CurrentChar, DrawElementPayload.GetFontInfo().FontFallback);

				if( Entry.Valid && (FontAtlasTexture == nullptr || Entry.TextureIndex != FontTextureIndex) )
				{
					// Font has a new texture for this glyph. Refresh the batch we use and the index we are currently using
					FontTextureIndex = Entry.TextureIndex;

					ISlateFontTexture* SlateFontTexture = FontCache.GetFontTexture(FontTextureIndex);
					check(SlateFontTexture);

					FontAtlasTexture = SlateFontTexture->GetSlateTexture();
					check(FontAtlasTexture);

					FontShaderResource = ResourceManager.GetFontShaderResource( FontTextureIndex, FontAtlasTexture, DrawElementPayload.GetFontInfo().FontMaterial );
					check(FontShaderResource);

					const bool bIsGrayscale = SlateFontTexture->IsGrayscale();
					FontTint = bIsGrayscale ? InTint : FColor::White;

					RenderBatch = &CreateRenderBatch(InLayer, FShaderParams(), FontShaderResource, ESlateDrawPrimitive::TriangleList, bIsGrayscale ? ESlateShader::GrayscaleFont : ESlateShader::ColorFont, InDrawEffects, ESlateBatchDrawFlag::None, DrawElement);

					// Reserve memory for the glyphs.  This isn't perfect as the text could contain spaces and we might not render the rest of the text in this batch but its better than resizing constantly
					const int32 GlyphsLeft = NumChars - CharIndex;
					RenderBatch->ReserveVertices(GlyphsLeft * 4);
					RenderBatch->ReserveIndices(GlyphsLeft * 6);

					InvTextureSizeX = 1.0f / FontAtlasTexture->GetWidth();
					InvTextureSizeY = 1.0f / FontAtlasTexture->GetHeight();
				}

				const bool bIsWhitespace = !Entry.Valid || (bEnableOutline && !Entry.SupportsOutline) || FChar::IsWhitespace(CurrentChar);

				if( !bIsWhitespace && PreviousCharEntry.Valid )
				{
					Kerning = CharacterList.GetKerning( PreviousCharEntry, Entry );
				}
				else
				{
					Kerning = 0;
				}

				LineX += Kerning;
				PreviousCharEntry = Entry;

				if( !bIsWhitespace )
				{
					const float InvBitmapRenderScale = 1.0f / Entry.BitmapRenderScale;

					const float X = LineX + Entry.HorizontalOffset+InOutlineHorizontalOffset;
					// Note PosX,PosY is the upper left corner of the bounding box representing the string.  This computes the Y position of the baseline where text will sit

					const float Y = PosY - Entry.VerticalOffset + ((MaxHeight + Entry.GlobalDescender) * InvBitmapRenderScale);
					const float U = Entry.StartU * InvTextureSizeX;
					const float V = Entry.StartV * InvTextureSizeY;
					const float SizeX = Entry.USize * Entry.BitmapRenderScale;
					const float SizeY = Entry.VSize * Entry.BitmapRenderScale;
					const float SizeU = Entry.USize * InvTextureSizeX;
					const float SizeV = Entry.VSize * InvTextureSizeY;

					{
						FVector2D UpperLeft( X, Y );
						FVector2D UpperRight( X+SizeX, Y );
						FVector2D LowerLeft( X, Y+SizeY );
						FVector2D LowerRight( X+SizeX, Y+SizeY );

						// The start index of these vertices in the index buffer
						const uint32 IndexStart = RenderBatch->GetNumVertices();

						float Ut = 0.0f, Vt = 0.0f, UtMax = 0.0f, VtMax = 0.0f;
						if( bIsFontMaterial )
						{
							float DistAlpha = (float)CharIndex/NumChars;
							float DistAlphaNext = (float)(CharIndex+1)/NumChars;

							// This creates a set of UV's that goes from 0-1, from left to right of the string in U and 0-1 baseline to baseline top to bottom in V
							Ut = FMath::Lerp(0.0f, 1.0f, DistAlpha);
							Vt = FMath::Lerp(0.0f, 1.0f, UpperLeft.Y / (MaxHeight*NumLines));

							UtMax = FMath::Lerp(0.0f, 1.0f, DistAlphaNext);
							VtMax = FMath::Lerp(0.0f, 1.0f, LowerLeft.Y / (MaxHeight*NumLines));
						}

						// Add four vertices to the list of verts to be added to the vertex buffer
						RenderBatch->AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(UpperLeft),					FVector4f(U,V,Ut,Vt),						FVector2f(0.0f,0.0f), FontTint ));
						RenderBatch->AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(LowerRight.X,UpperLeft.Y),	FVector4f(U+SizeU, V, UtMax,Vt),			FVector2f(1.0f,0.0f), FontTint ));
						RenderBatch->AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(UpperLeft.X,LowerRight.Y),	FVector4f(U, V+SizeV, Ut,VtMax),			FVector2f(0.0f,1.0f), FontTint ));
						RenderBatch->AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(LowerRight),				FVector4f(U+SizeU, V+SizeV, UtMax,VtMax),	FVector2f(1.0f,1.0f), FontTint ));

						RenderBatch->AddIndex(IndexStart + 0);
						RenderBatch->AddIndex(IndexStart + 1);
						RenderBatch->AddIndex(IndexStart + 2);
						RenderBatch->AddIndex(IndexStart + 1);
						RenderBatch->AddIndex(IndexStart + 3);
						RenderBatch->AddIndex(IndexStart + 2);
					}
				}

				LineX += Entry.XAdvance;
			}
		}
	};

	if (bOutlineFont)
	{
		// Build geometry for the outline
		BuildFontGeometry(OutlineSettings, PackVertexColor(OutlineSettings.OutlineColor), OutlineFontMaterial, Layer, 0.f);

		//The fill area was measured without an outline so it must be shifted by the scaled outline size
		const float HorizontalOffset = FMath::RoundToFloat((float)OutlineSize * FontScale);

		// Build geometry for the base font which is always rendered on top of the outline
		BuildFontGeometry(FFontOutlineSettings::NoOutline, BaseTint, BaseFontMaterial, Layer + 1, HorizontalOffset);
	}
	else
	{
		// No outline, draw normally
		BuildFontGeometry(FFontOutlineSettings::NoOutline, BaseTint, BaseFontMaterial, Layer, 0.f);
	}
}

/**
 * Determines if the 2x2 matrix represents a rotation that will keep an axis-aligned rect
 * axis-aligned (i.e. a rotation of 90-degree increments). This allows both "proper rotations"
 * (those without a reflection) and "improper rotations" (rotations combined with a reflection
 * over a single axis).
 */
static bool IsAxisAlignedRotation(const FMatrix2x2& Matrix)
{
	constexpr float Tolerance = KINDA_SMALL_NUMBER;

	float A, B, C, D;
	Matrix.GetMatrix(A, B, C, D);

	// The 90- and 270-degree rotation matrices have zeroes on the main diagonal e.g.
	// [0 n]
	// [n 0] with n = 1 or -1
	if (FMath::IsNearlyZero(A, Tolerance) && FMath::IsNearlyZero(D, Tolerance))
	{
		return FMath::IsNearlyEqual(1.0f, FMath::Abs(B), Tolerance) && FMath::IsNearlyEqual(1.0f, FMath::Abs(C), Tolerance);
	}

	// The 0- and 180-degree rotation matrices have zeroes on the secondary diagonal e.g.
	// [n 0]
	// [0 n] with n = 1 or -1
	if (FMath::IsNearlyZero(B, Tolerance) && FMath::IsNearlyZero(C, Tolerance))
	{
		return FMath::IsNearlyEqual(1.0f, FMath::Abs(A), Tolerance) && FMath::IsNearlyEqual(1.0f, FMath::Abs(D), Tolerance);
	}

	return false;
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddShapedTextElement( const FSlateDrawElement& DrawElement )
{
	const FSlateShapedTextPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateShapedTextPayload>();
	const FShapedGlyphSequence* ShapedGlyphSequence = DrawElementPayload.GetShapedGlyphSequence().Get();

	const FShapedGlyphSequence* OverflowGlyphSequence = DrawElementPayload.OverflowArgs.OverflowTextPtr.Get();

	checkSlow(ShapedGlyphSequence);

	const FFontOutlineSettings& OutlineSettings = ShapedGlyphSequence->GetFontOutlineSettings();

	ensure(ShapedGlyphSequence->GetGlyphsToRender().Num() > 0);

	const FColor BaseTint = PackVertexColor(DrawElementPayload.GetTint());

	FSlateFontCache& FontCache = *RenderingPolicy->GetFontCache();


	const int16 TextBaseline = ShapedGlyphSequence->GetTextBaseline();
	const uint16 MaxHeight = ShapedGlyphSequence->GetMaxTextHeight();

	FShapedTextBuildContext BuildContext;

	BuildContext.DrawElement = &DrawElement;
	BuildContext.FontCache = &FontCache;
	BuildContext.ShapedGlyphSequence = ShapedGlyphSequence;
	BuildContext.OverflowGlyphSequence = OverflowGlyphSequence;
	BuildContext.TextBaseline = TextBaseline;
	BuildContext.MaxHeight = MaxHeight;

	if (MaxHeight == 0)
	{
		// If the max text height is 0, we'll create NaN's further in the code, so avoid drawing text if this happens.
		return;
	}

	const int32 Layer = DrawElement.GetLayer();

	// extract the layout transform from the draw element
	FSlateLayoutTransform LayoutTransform(DrawElement.GetScale(), DrawElement.GetPosition());

	// We don't just scale up fonts, we draw them in local space pre-scaled so we don't get scaling artifacts.
	// So we need to pull the layout scale out of the layout and render transform so we can apply them
	// in local space with pre-scaled fonts.
	const float FontScale = LayoutTransform.GetScale();

	const FSlateRenderTransform RenderTransform = Concatenate(Inverse(FontScale), DrawElement.GetRenderTransform());
	BuildContext.RenderTransform = &RenderTransform;

	const UObject* BaseFontMaterial = ShapedGlyphSequence->GetFontMaterial();
	const UObject* OutlineFontMaterial = OutlineSettings.OutlineMaterial;

	bool bOutlineFont = OutlineSettings.OutlineSize > 0;
	const int32 OutlineSize = OutlineSettings.OutlineSize;

	auto BuildFontGeometry = [&](const FFontOutlineSettings& InOutlineSettings, const FColor& InTint, const UObject* FontMaterial, int32 InLayer, float InHorizontalOffset)
	{
		FVector2D TopLeft(0, 0);

		const float PosX = TopLeft.X+InHorizontalOffset;
		float PosY = TopLeft.Y;

		BuildContext.FontMaterial = FontMaterial;
		BuildContext.OutlineFontMaterial = OutlineFontMaterial;
	
		BuildContext.OutlineSettings = &InOutlineSettings;
		BuildContext.StartLineX = PosX;
		BuildContext.StartLineY = PosY;
		BuildContext.LayerId = InLayer;
		BuildContext.FontTint = InTint;

		BuildContext.bEnableOutline = InOutlineSettings.OutlineSize > 0;

		// Optimize by culling
		// Todo: this doesn't work with cached clipping
		BuildContext.bEnableCulling = false;
		BuildContext.bForceEllipsis = DrawElementPayload.OverflowArgs.bForceEllipsisDueToClippedLine;
		BuildContext.OverflowDirection = DrawElementPayload.OverflowArgs.OverflowDirection;

		if (ShapedGlyphSequence->GetGlyphsToRender().Num() > 200 || (OverflowGlyphSequence && BuildContext.OverflowDirection != ETextOverflowDirection::NoOverflow))
		{
			const FSlateClippingState* ClippingState = ResolveClippingState(DrawElement);

			if (ClippingState && ClippingState->ScissorRect.IsSet() && ClippingState->ScissorRect->IsAxisAligned() && IsAxisAlignedRotation(RenderTransform.GetMatrix()))
			{
				// Non-render transformed box or rotation is axis-aligned at 90-degree increments
				const FSlateRect ScissorRectBox = ClippingState->ScissorRect->GetBoundingBox();

				// We know that this will be axis-aligned because the scissor rect is axis-aligned and we already
				// checked that the render transform is axis-aligned as well
				const FSlateRect LocalClipBoundingBox = TransformRect(RenderTransform.Inverse(), ScissorRectBox);
				BuildContext.LocalClipBoundingBoxLeft = LocalClipBoundingBox.Left;
				BuildContext.LocalClipBoundingBoxRight = LocalClipBoundingBox.Right - (BuildContext.bForceEllipsis ? OverflowGlyphSequence->GetMeasuredWidth() : 0);

				// In checks below, ignore floating-point differences caused by transforming and untransforming the clip rect
				if (OverflowGlyphSequence && FMath::FloorToInt(BuildContext.LocalClipBoundingBoxLeft) <= 0 && FMath::CeilToInt(BuildContext.LocalClipBoundingBoxRight) >= ShapedGlyphSequence->GetMeasuredWidth())
				{
					// Override overflow if the text is smaller than (or is the same size as) the clipping rect and wont be clipped
					BuildContext.OverflowDirection = ETextOverflowDirection::NoOverflow;
				}
				else if(!OverflowGlyphSequence)
				{
					BuildContext.bEnableCulling = true;
				}
			}
			else
			{
				// Overflow not supported on non-identity transforms (except for 90-degree rotations)
				BuildContext.OverflowDirection = ETextOverflowDirection::NoOverflow;
			}
		}

		BuildShapedTextSequence<Rounding>(BuildContext);
	};

	if (bOutlineFont)
	{
		// Build geometry for the outline
		BuildFontGeometry(OutlineSettings, PackVertexColor(DrawElementPayload.GetOutlineTint()), OutlineFontMaterial, Layer, 0.f);
		
		//The fill area was measured without an outline so it must be shifted by the scaled outline size
		const float HorizontalOffset = FMath::RoundToFloat((float)OutlineSize * FontScale);

		// Build geometry for the base font which is always rendered on top of the outline 
		BuildFontGeometry(FFontOutlineSettings::NoOutline, BaseTint, BaseFontMaterial, Layer+1, HorizontalOffset);
	}
	else
	{
		// No outline
		BuildFontGeometry(FFontOutlineSettings::NoOutline, BaseTint, BaseFontMaterial, Layer, 0.f);
	}
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddGradientElement( const FSlateDrawElement& DrawElement )
{
	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FVector2D LocalSize = DrawElement.GetLocalSize();
	const FSlateGradientPayload& InPayload = DrawElement.GetDataPayload<FSlateGradientPayload>();
	const ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();
	const float DrawScale = DrawElement.GetScale();

	// There must be at least one gradient stop
	check( InPayload.GradientStops.Num() > 0 );

	FShaderParams ShaderParams;

	ESlateShader ShaderType = ESlateShader::Default;
	if (InPayload.CornerRadius != FVector4f(0))
	{
		ShaderType = ESlateShader::RoundedBox;
		ShaderParams.PixelParams = FVector4f(0.0f, 0.0f, LocalSize.X, LocalSize.Y);
		ShaderParams.PixelParams2 = InPayload.CornerRadius;
	}

	FSlateRenderBatch& RenderBatch = 
		CreateRenderBatch( 
			Layer,
			ShaderParams,
			nullptr,
			ESlateDrawPrimitive::TriangleList,
			ShaderType,
			InDrawEffects,
			DrawElement.GetBatchFlags(),
			DrawElement);

	// Determine the four corners of the quad containing the gradient
	FVector2D TopLeft = FVector2D::ZeroVector;
	FVector2D TopRight = FVector2D(LocalSize.X, 0);
	FVector2D BotLeft = FVector2D(0, LocalSize.Y);
	FVector2D BotRight = FVector2D(LocalSize.X, LocalSize.Y);

	// Copy the gradient stops.. We may need to add more
	TArray<FSlateGradientStop> GradientStops = InPayload.GradientStops;

	const FSlateGradientStop& FirstStop = InPayload.GradientStops[0];
	const FSlateGradientStop& LastStop = InPayload.GradientStops[ InPayload.GradientStops.Num() - 1 ];
		
	// Determine if the first and last stops are not at the start and end of the quad
	// If they are not add a gradient stop with the same color as the first and/or last stop
	if( InPayload.GradientType == Orient_Vertical )
	{
		if( 0.0f < FirstStop.Position.X )
		{
			// The first stop is after the left side of the quad.  Add a stop at the left side of the quad using the same color as the first stop
			GradientStops.Insert( FSlateGradientStop( FVector2D(0.0, 0.0), FirstStop.Color ), 0 );
		}

		if( LocalSize.X > LastStop.Position.X )
		{
			// The last stop is before the right side of the quad.  Add a stop at the right side of the quad using the same color as the last stop
			GradientStops.Add( FSlateGradientStop( LocalSize, LastStop.Color ) ); 
		}
	}
	else
	{
		if( 0.0f < FirstStop.Position.Y )
		{
			// The first stop is after the top side of the quad.  Add a stop at the top side of the quad using the same color as the first stop
			GradientStops.Insert( FSlateGradientStop( FVector2D(0.0, 0.0), FirstStop.Color ), 0 );
		}

		if( LocalSize.Y > LastStop.Position.Y )
		{
			// The last stop is before the bottom side of the quad.  Add a stop at the bottom side of the quad using the same color as the last stop
			GradientStops.Add( FSlateGradientStop( LocalSize, LastStop.Color ) ); 
		}
	}

	// Add a pair of vertices for each gradient stop. Connecting them to the previous stop if necessary
	// Assumes gradient stops are sorted by position left to right or top to bottom
	for( int32 StopIndex = 0; StopIndex < GradientStops.Num(); ++StopIndex )
	{
		const uint32 IndexStart = RenderBatch.GetNumVertices();

		const FSlateGradientStop& CurStop = GradientStops[StopIndex];

		// The start vertex at this stop
		FVector2D StartPt;
		// The end vertex at this stop
		FVector2D EndPt;

		FVector2D StartUV;
		FVector2D EndUV;

		if( InPayload.GradientType == Orient_Vertical )
		{
			// Gradient stop is vertical so gradients to left to right
			StartPt = TopLeft;
			EndPt = BotLeft;
			// Gradient stops are interpreted in local space.
			StartPt.X += CurStop.Position.X;
			EndPt.X += CurStop.Position.X;

			StartUV.X = StartPt.X / TopRight.X;
			StartUV.Y = 0;

			EndUV.X = EndPt.X / TopRight.X;
			EndUV.Y = 1;
		}
		else
		{

			// Gradient stop is horizontal so gradients to top to bottom
			StartPt = TopLeft;
			EndPt = TopRight;
			// Gradient stops are interpreted in local space.
			StartPt.Y += CurStop.Position.Y;
			EndPt.Y += CurStop.Position.Y;

			StartUV.X = 0;
			StartUV.Y = StartPt.Y / BotLeft.Y;

			EndUV.X = 1;
			EndUV.Y = StartPt.Y / BotLeft.Y;
		}

		if( StopIndex == 0 )
		{
			// First stop does not have a full quad yet so do not create indices
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(StartPt), FVector2f(LocalSize), DrawScale, FVector4f(StartUV.X, StartUV.Y, 0,0), PackVertexColor(CurStop.Color), FColor::Transparent));
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPt), FVector2f(LocalSize), DrawScale, FVector4f(EndUV.X, EndUV.Y, 0, 0), PackVertexColor(CurStop.Color), FColor::Transparent));
		}
		else
		{
			// All stops after the first have indices and generate quads
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(StartPt), FVector2f(LocalSize), DrawScale, FVector4f(StartUV.X, StartUV.Y, 0, 0), PackVertexColor(CurStop.Color), FColor::Transparent));
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPt), FVector2f(LocalSize), DrawScale, FVector4f(EndUV.X, EndUV.Y, 0, 0), PackVertexColor(CurStop.Color), FColor::Transparent));

			// Connect the indices to the previous vertices
			RenderBatch.AddIndex(IndexStart - 2);
			RenderBatch.AddIndex(IndexStart - 1);
			RenderBatch.AddIndex(IndexStart + 0);

			RenderBatch.AddIndex(IndexStart + 0);
			RenderBatch.AddIndex(IndexStart - 1);
			RenderBatch.AddIndex(IndexStart + 1);
		}
	}
}

/**
* Calculates the intersection of two line segments P1->P2, P3->P4
* The tolerance setting is used when the lines aren't currently intersecting but will intersect in the future
* The higher the tolerance the greater the distance that the intersection point can be.
*
* @return true if the line intersects.  Populates Intersection
*/
static bool LineIntersect(const FVector2D& P1, const FVector2D& P2, const FVector2D& P3, const FVector2D& P4, FVector2D& Intersect, float Tolerance = .1f)
{
	float NumA = ((P4.X - P3.X)*(P1.Y - P3.Y) - (P4.Y - P3.Y)*(P1.X - P3.X));
	float NumB = ((P2.X - P1.X)*(P1.Y - P3.Y) - (P2.Y - P1.Y)*(P1.X - P3.X));

	float Denom = (P4.Y - P3.Y)*(P2.X - P1.X) - (P4.X - P3.X)*(P2.Y - P1.Y);

	if (FMath::IsNearlyZero(NumA) && FMath::IsNearlyZero(NumB))
	{
		// Lines are the same
		Intersect = (P1 + P2) / 2;
		return true;
	}

	if (FMath::IsNearlyZero(Denom))
	{
		// Lines are parallel
		return false;
	}

	float B = NumB / Denom;
	float A = NumA / Denom;

	// Note that this is a "tweaked" intersection test for the purpose of joining line segments.  We don't just want to know if the line segments
	// Intersect, but where they would if they don't currently. Except that we don't care in the case that where the segments 
	// intersection is so far away that its infeasible to use the intersection point later.
	if (A >= -Tolerance && A <= (1.0f + Tolerance) && B >= -Tolerance && B <= (1.0f + Tolerance))
	{
		Intersect = P1 + A*(P2 - P1);
		return true;
	}

	return false;
}


/** Utility class for building a strip of lines. */
struct FLineBuilder
{
	// Will append 5 vertexes to OutBatchVertices and 9 indexes to
	// OutBatchIndices. Creates the following cap geometry:
	//
	// Cap Vertex Indexes              Cap Measurements
	//
	//     U == 0
	//   2-4----                        2-------4-------....
	//   |\| 							|       |     ^ 
	//   | 0  <-- U==0.5				|<- d ->o    2h  
	//   |/|							|       |     v
	//   1-3----						1-------3-------....
	//     U == 0
	//                                 d is CapDirection
	//                                 h is Up
	//                                 o is CapOrigin

	static void MakeCap(
		FSlateRenderBatch& RenderBatch,
		const FSlateRenderTransform& RenderTransform,
		const FVector2D& CapOrigin,
		const FVector2D& CapDirection,
		const FVector2D& Up,
		const FColor& Color
	)
	{
		const uint32 FirstVertIndex = RenderBatch.GetNumVertices();

		RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(CapOrigin), FVector2f(0.5f, 0.0f), FVector2f::ZeroVector, Color));
		RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(CapOrigin + CapDirection + Up), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, Color));
		RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(CapOrigin + CapDirection - Up), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, Color));
		RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(CapOrigin + Up), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, Color));
		RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(CapOrigin - Up), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, Color));

		RenderBatch.AddIndex(FirstVertIndex + 0);
		RenderBatch.AddIndex(FirstVertIndex + 3);
		RenderBatch.AddIndex(FirstVertIndex + 1);

		RenderBatch.AddIndex(FirstVertIndex + 0);
		RenderBatch.AddIndex(FirstVertIndex + 1);
		RenderBatch.AddIndex(FirstVertIndex + 2);

		RenderBatch.AddIndex(FirstVertIndex + 0);
		RenderBatch.AddIndex(FirstVertIndex + 2);
		RenderBatch.AddIndex(FirstVertIndex + 4);
	}

	FLineBuilder(FSlateRenderBatch& InRenderBatch, const FVector2D StartPoint, float HalfThickness, const FSlateRenderTransform& InRenderTransform, const FColor& InColor)
		: RenderBatch(InRenderBatch)
		, RenderTransform(InRenderTransform)
		, LastPointAdded()
		, LastNormal(FVector2D::ZeroVector)
		, HalfLineThickness(HalfThickness)
		, NumPointsAdded(1)
		, SingleColor(InColor)
	{
		LastPointAdded[0] = LastPointAdded[1] = StartPoint;
	}

	
	void BuildBezierGeometry_WithColorGradient(const TArray<FSlateGradientStop>& GradientStops, int32 GradientStopIndex, const FVector2D& P0, const FVector2D& P1, const FVector2D& P2, const FVector2D& P3, const FSlateElementBatcher& InBatcher)
	{
		const int32 NumGradientStops = GradientStops.Num();
		const float SubdivisionPoint = 1.0f / (NumGradientStops - GradientStopIndex);
		
		if (GradientStopIndex < NumGradientStops - 1)
		{
			FVector2D TwoCurves[7];
			deCasteljauSplit_WithColorGradient(P0, P1, P2, P3, TwoCurves, SubdivisionPoint);
			Subdivide_WithColorGradient(GradientStops[GradientStopIndex - 1].Color, GradientStops[GradientStopIndex].Color, InBatcher, TwoCurves[0], TwoCurves[1], TwoCurves[2], TwoCurves[3], *this, 1.0f);
			BuildBezierGeometry_WithColorGradient(GradientStops, GradientStopIndex + 1, TwoCurves[3], TwoCurves[4], TwoCurves[5], TwoCurves[6], InBatcher);
		}
		else
		{
			// We have reached the last gradient stop, so we can finish this spline.
			Subdivide_WithColorGradient(GradientStops[GradientStopIndex - 1].Color, GradientStops[GradientStopIndex].Color, InBatcher, P0, P1, P2, P3, *this, 1.0f);
			Finish(P3, InBatcher.PackVertexColor(GradientStops[GradientStopIndex].Color));
		}	
		
	}

	void BuildBezierGeometry(const FVector2D& P0, const FVector2D& P1, const FVector2D& P2, const FVector2D& P3)
	{
		Subdivide(P0, P1, P2, P3, *this, 1.0f);
		Finish(P3, SingleColor);
	}
	
private:
	void AppendPoint(const FVector2D NewPoint, const FColor& InColor)
	{
		// We only add vertexes for the previous line segment.
		// This is because we want to average the previous and new normals
		// In order to prevent overlapping line segments on the spline.
		// These occur especially when curvature is high.

		const FVector2D NewNormal = FVector2D(LastPointAdded[0].Y - NewPoint.Y, NewPoint.X - LastPointAdded[0].X).GetSafeNormal();

		if (NumPointsAdded == 2)
		{
			// Once we have two points, we have a normal, so we can generate the first bit of geometry.
			const FVector2D LastUp = LastNormal*HalfLineThickness;

			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[1] + LastUp), FVector2f(1.0f, 0.0f), FVector2f::ZeroVector, InColor));
			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[1] - LastUp), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, InColor));
		}

		if (NumPointsAdded >= 2)
		{
			const FVector2D AveragedUp = (0.5f*(NewNormal + LastNormal)).GetSafeNormal()*HalfLineThickness;

			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[0] + AveragedUp), FVector2f(1.0f, 0.0f), FVector2f::ZeroVector, InColor));
			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[0] - AveragedUp), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, InColor));

			const int32 NumVerts = RenderBatch.GetNumVertices();

			// Counterclockwise winding on triangles
			RenderBatch.AddIndex(NumVerts - 3);
			RenderBatch.AddIndex(NumVerts - 4);
			RenderBatch.AddIndex(NumVerts - 2);

			RenderBatch.AddIndex(NumVerts - 3);
			RenderBatch.AddIndex(NumVerts - 2);
			RenderBatch.AddIndex(NumVerts - 1);
		}

		LastPointAdded[1] = LastPointAdded[0];
		LastPointAdded[0] = NewPoint;
		LastNormal = NewNormal;

		++NumPointsAdded;
	}

	void Finish(const FVector2D& LastPoint, const FColor& InColor)
	{
		if (NumPointsAdded < 3)
		{
			// Line builder needs at least two line segments (3 points) to
			// complete building its geometry.
			// This will only happen in the case when we have a straight line.
			AppendPoint(LastPoint, InColor);
		}
		else
		{
			// We have added the last point, but the line builder only builds
			// geometry for the previous line segment. Build geometry for the
			// last line segment.
			const FVector2D LastUp = LastNormal*HalfLineThickness;

			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[0] + LastUp), FVector2f(1.0f, 0.0f), FVector2f::ZeroVector, InColor));
			RenderBatch.AddVertex(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RenderTransform, FVector2f(LastPointAdded[0] - LastUp), FVector2f(0.0f, 0.0f), FVector2f::ZeroVector, InColor));

			const int32 NumVerts = RenderBatch.GetNumVertices();

			// Counterclockwise winding on triangles
			RenderBatch.AddIndex(NumVerts - 3);
			RenderBatch.AddIndex(NumVerts - 4);
			RenderBatch.AddIndex(NumVerts - 2);

			RenderBatch.AddIndex(NumVerts - 3);
			RenderBatch.AddIndex(NumVerts - 2);
			RenderBatch.AddIndex(NumVerts - 1);
		}
	}

	/**
	* Based on comp.graphics.algorithms: Adaptive Subdivision of Bezier Curves.
	*
	*   P1 + - - - - + P2
	*     /           \
	* P0 *             * P3
	*
	* In a perfectly flat curve P1 is the midpoint of (P0, P2) and P2 is the midpoint of (P1,P3).
	* Computing the deviation of points P1 and P2 from the midpoints of P0,P2 and P1,P3 provides
	* a simple and reliable measure of flatness.
	*
	* P1Deviation = (P0 + P2)/2 - P1
	* P2Deviation = (P1 + P3)/2 - P2
	*
	* Eliminate divides: same expression but gets us twice the allowable error
	* P1Deviation*2 = P0 + P2 - 2*P1
	* P2Deviation*2 = P1 + P3 - 2*P2
	*
	* Use manhattan distance: 2*Deviation = |P1Deviation.x| + |P1Deviation.y| + |P2Deviation.x| + |P2Deviation.y|
	*
	*/
	static float ComputeCurviness(const FVector2D&  P0, const FVector2D&  P1, const FVector2D&  P2, const FVector2D&  P3)
	{
		FVector2D TwoP1Deviations = P0 + P2 - 2 * P1;
		FVector2D TwoP2Deviations = P1 + P3 - 2 * P2;
		float TwoDeviations = FMath::Abs(TwoP1Deviations.X) + FMath::Abs(TwoP1Deviations.Y) + FMath::Abs(TwoP2Deviations.X) + FMath::Abs(TwoP2Deviations.Y);
		return TwoDeviations;
	}


	/**
	* deCasteljau subdivision of Bezier Curves based on reading of Gernot Hoffmann's Bezier Curves.
	*
	*       P1 + - - - - + P2                P1 +
	*         /           \                    / \
	*     P0 *             * P3            P0 *   \   * P3
	*                                              \ /
	*                                               + P2
	*
	*
	* Split the curve defined by P0,P1,P2,P3 into two new curves L0..L3 and R0..R3 that define the same shape.
	*
	* Points L0 and R3 are P0 and P3.
	* First find points L1, M, R2  as the midpoints of (P0,P1), (P1,P2), (P2,P3).
	* Find two more points: L2, R1 defined by midpoints of (L1,M) and (M,R2) respectively.
	* The final points L3 and R0 are both the midpoint of (L2,R1)
	*
	*/
	static void deCasteljauSplit(const FVector2D&  P0, const FVector2D&  P1, const FVector2D&  P2, const FVector2D& P3, FVector2D OutCurveParams[7])
	{
		FVector2D L1 = (P0 + P1) * 0.5f;
		FVector2D M = (P1 + P2) * 0.5f;
		FVector2D R2 = (P2 + P3) * 0.5f;

		FVector2D L2 = (L1 + M) * 0.5f;
		FVector2D R1 = (M + R2) * 0.5f;

		FVector2D L3R0 = (L2 + R1) * 0.5f;

		OutCurveParams[0] = P0;
		OutCurveParams[1] = L1;
		OutCurveParams[2] = L2;
		OutCurveParams[3] = L3R0;
		OutCurveParams[4] = R1;
		OutCurveParams[5] = R2;
		OutCurveParams[6] = P3;
	}

	/** More general form of the deCasteljauSplit splits the curve into two parts at a point between 0 and 1 along the curve's length. */
	static void deCasteljauSplit_WithColorGradient(const FVector2D&  P0, const FVector2D&  P1, const FVector2D&  P2, const FVector2D& P3, FVector2D OutCurveParams[7], float SplitPoint = 0.5f)
	{
		FVector2D L1 = FMath::Lerp(P0,P1,SplitPoint);
		FVector2D M = FMath::Lerp(P1,P2,SplitPoint);
		FVector2D R2 = FMath::Lerp(P2,P3,SplitPoint);

		FVector2D L2 = FMath::Lerp(L1,M,SplitPoint);
		FVector2D R1 = FMath::Lerp(M,R2,SplitPoint);

		FVector2D L3R0 = FMath::Lerp(L2,R1,SplitPoint);

		OutCurveParams[0] = P0;
		OutCurveParams[1] = L1;
		OutCurveParams[2] = L2;
		OutCurveParams[3] = L3R0;
		OutCurveParams[4] = R1;
		OutCurveParams[5] = R2;
		OutCurveParams[6] = P3;
	}

	static void Subdivide(const FVector2D&  P0, const FVector2D&  P1, const FVector2D&  P2, const FVector2D&  P3, FLineBuilder& LineBuilder, float MaxBiasTimesTwo = 2.0f)
	{
		const float Curviness = ComputeCurviness(P0, P1, P2, P3);
		if (Curviness > MaxBiasTimesTwo)
		{
			// Split the Bezier into two curves.
			FVector2D TwoCurves[7];
			deCasteljauSplit(P0, P1, P2, P3, TwoCurves);
			// Subdivide left, then right
			Subdivide(TwoCurves[0], TwoCurves[1], TwoCurves[2], TwoCurves[3], LineBuilder, MaxBiasTimesTwo);
			Subdivide(TwoCurves[3], TwoCurves[4], TwoCurves[5], TwoCurves[6], LineBuilder, MaxBiasTimesTwo);
		}
		else
		{
			LineBuilder.AppendPoint(P3, LineBuilder.SingleColor);
		}
	}

	static void Subdivide_WithColorGradient(const FLinearColor& StartColor, const FLinearColor& EndColor, const FSlateElementBatcher& InBatcher, const FVector2D&  P0, const FVector2D&  P1, const FVector2D&  P2, const FVector2D&  P3, FLineBuilder& LineBuilder, float MaxBiasTimesTwo = 2.0f)
	{
		const float Curviness = ComputeCurviness(P0, P1, P2, P3);
		if (Curviness > MaxBiasTimesTwo)
		{
			// Split the Bezier into two curves.
			FVector2D TwoCurves[7];
			deCasteljauSplit(P0, P1, P2, P3, TwoCurves);
			const FLinearColor MidpointColor = FLinearColor::LerpUsingHSV(StartColor, EndColor, 0.5f);
			// Subdivide left, then right
			Subdivide_WithColorGradient(StartColor, MidpointColor, InBatcher, TwoCurves[0], TwoCurves[1], TwoCurves[2], TwoCurves[3], LineBuilder, MaxBiasTimesTwo);
			Subdivide_WithColorGradient(MidpointColor, EndColor, InBatcher, TwoCurves[3], TwoCurves[4], TwoCurves[5], TwoCurves[6], LineBuilder, MaxBiasTimesTwo);
		}
		else
		{
			LineBuilder.AppendPoint(P3, InBatcher.PackVertexColor(EndColor));
		}
	}
	
private:
	FSlateRenderBatch& RenderBatch;
	const FSlateRenderTransform& RenderTransform;
	FVector2D LastPointAdded[2];
	FVector2D LastNormal;
	float HalfLineThickness;
	int32 NumPointsAdded;
	FColor SingleColor;

};


void FSlateElementBatcher::AddSplineElement(const FSlateDrawElement& DrawElement)
{
	// WHY NO PIXEL SNAPPING?
	//
	// Pixel snapping with splines does not make sense.
	// If any of the segments are snapped to pixels, the line will
	// not appear continuous. It is possible to snap the first and
	// last points to pixels, but unclear what that means given
	// a floating point line width.

	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FSlateSplinePayload& InPayload = DrawElement.GetDataPayload<FSlateSplinePayload>();
	ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();

	// 1 is the minimum thickness we support for generating geometry.
	// The shader takes care of sub-pixel line widths.
	// Thickness is given in screenspace, so convert it to local space before proceeding.
	float InThickness = FMath::Max(1.0f, DrawElement.GetInverseLayoutTransform().GetScale() * InPayload.GetThickness());

	// Width of the filter size to use for anti-aliasing.
	// Increasing this value will increase the fuzziness of line edges.
	const float FilterScale = 1.0f;

	static const float TwoRootTwo = 2 * UE_SQRT_2;
	// Compute the actual size of the line we need based on thickness.
	// Each line segment will be a bit thicker than the line to account
	// for the size of the filter.
	const float LineThickness = (TwoRootTwo + InThickness);

	// The amount we increase each side of the line to generate enough pixels
	const float HalfThickness = LineThickness * .5f + FilterScale;

	const FColor SplineColor = (InPayload.GradientStops.Num()==1) ? PackVertexColor(InPayload.GradientStops[0].Color) : PackVertexColor(InPayload.GetTint());

	FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, FShaderParams::MakePixelShaderParams(FVector4f(InPayload.GetThickness(), FilterScale, 0, 0)), nullptr, ESlateDrawPrimitive::TriangleList, ESlateShader::LineSegment, InDrawEffects, ESlateBatchDrawFlag::None, DrawElement);

	FLineBuilder LineBuilder(
		RenderBatch,
		InPayload.P0,
		HalfThickness,
		RenderTransform,
		SplineColor
	);

	if (/*const bool bNoGradient = */InPayload.GradientStops.Num() <= 1)
	{
		// Normal scenario where there is no color gradient.
		LineBuilder.BuildBezierGeometry(InPayload.P0, InPayload.P1, InPayload.P2, InPayload.P3);
	}
	else
	{
		// Deprecated scenario _WithColorGradient
		LineBuilder.BuildBezierGeometry_WithColorGradient( InPayload.GradientStops, 1, InPayload.P0, InPayload.P1, InPayload.P2, InPayload.P3, *this);
	}
}


template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddLineElement( const FSlateDrawElement& DrawElement )
{
	const FSlateLinePayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateLinePayload>();
	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	ESlateDrawEffect DrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();

	const TArray<FVector2D>& Points = DrawElementPayload.GetPoints();
	const TArray<FLinearColor>& PointColors = DrawElementPayload.GetPointColors();

	const int32 NumPoints = Points.Num();
	if (NumPoints < 2)
	{
		return;
	}
	
	const FColor FinalTint = PackVertexColor(DrawElementPayload.GetTint());

	if(DrawElementPayload.IsAntialiased() )
	{
		//
		//  The true center of the line is represented by o---o---o
		//
		//
		//           Two triangles make up each trapezoidal line segment
		//                /        |  |   
		//               v         |  |   
		//    +-+---------------+  |  | 
		//    |\|              / \ v  | 
		//    | o-------------o   \   |  +--------- U==0
		//    |/|            / \   \  |  | 
		//    +-+-----------+   \   \ v  v  
		//                   \   \   +------+-+
		//     ^              \   \ /       |/| 
		//     |               \   o--------o | <-- Endcap
		//     Endcap           \ /         |\|
		//                       +----------+-+
		//                               ^
		//                               |
		//                               +--------- U==1
		//
		// Each trapezoidal section has a Vertex.U==1 on the bottom and Vertex.U==0 on top.
		// Endcaps have Vertex.U==0.5 in the middle and Vertex.U==0 on the outside.
		// This enables easy distance calculations to the "true center" of the line for
		// anti-aliasing calculations performed in the pixels shader.




		// Half of the width of the filter size to use for anti-aliasing.
		// Increasing this value will increase the fuzziness of line edges.
		const float FilterScale = 1.0f;

		// Thickness is given in screen space, so convert it to local space before proceeding.
		float RequestedThickness = DrawElementPayload.GetThickness();
		
		static const float TwoRootTwo = 2 * UE_SQRT_2;
		// Compute the actual size of the line we need based on thickness.
		// Each line segment will be a bit thicker than the line to account
		// for the size of the filter.
		const float LineThickness = (TwoRootTwo + RequestedThickness );

		// The amount we increase each side of the line to generate enough pixels
		const float HalfThickness = LineThickness * .5f + FilterScale;

		// Find a batch for the element
		FSlateRenderBatch& RenderBatch = CreateRenderBatch( Layer, FShaderParams::MakePixelShaderParams(FVector4f(RequestedThickness, FilterScale,0,0) ), nullptr, ESlateDrawPrimitive::TriangleList, ESlateShader::LineSegment, DrawEffects, ESlateBatchDrawFlag::None, DrawElement);

		FVector2D StartPos = Points[0];
		FVector2D EndPos = Points[1];

		FVector2D Normal = FVector2D( StartPos.Y - EndPos.Y, EndPos.X - StartPos.X ).GetSafeNormal();
		FVector2D Up = Normal * HalfThickness;

		FColor StartColor = PointColors.Num() ? PackVertexColor(PointColors[0] * DrawElementPayload.GetTint()) : FinalTint;
		FColor EndColor = PointColors.Num() ? PackVertexColor(PointColors[1] * DrawElementPayload.GetTint()) : FinalTint;
	
		const FVector2D StartCapDirection = HalfThickness*((StartPos - EndPos).GetSafeNormal());
		FLineBuilder::MakeCap(RenderBatch, RenderTransform, StartPos, StartCapDirection, Up, StartColor);
		const uint32 IndexStart = RenderBatch.GetNumVertices();

		// First two points in the line.
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(StartPos + Up), FVector2f(1.0, 0.0f), FVector2f::ZeroVector, StartColor ) );
		RenderBatch.AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(StartPos - Up), FVector2f(0.0, 0.0f), FVector2f::ZeroVector, StartColor) );

		// Generate the rest of the segments
		for( int32 Point = 1; Point < NumPoints; ++Point )
		{
			EndPos = Points[Point];
			// Determine if we should check the intersection point with the next line segment.
			// We will adjust were this line ends to the intersection
			bool bCheckIntersection = (Point + 1) < NumPoints;

			// Compute the normal to the line
			Normal = FVector2D( StartPos.Y - EndPos.Y, EndPos.X - StartPos.X ).GetSafeNormal();

			// Create the new vertices for the thick line segment
			Up = Normal * HalfThickness;

			FColor PointColor = PointColors.Num() ? PackVertexColor(PointColors[Point] * DrawElementPayload.GetTint()) : FinalTint;

			FVector2D IntersectUpper = EndPos + Up;
			FVector2D IntersectLower = EndPos - Up;

			if( bCheckIntersection )
			{
				// The end point of the next segment
				const FVector2D NextEndPos = Points[Point+1];

				// The normal of the next segment
				const FVector2D NextNormal = FVector2D( EndPos.Y - NextEndPos.Y, NextEndPos.X - EndPos.X ).GetSafeNormal();

				// The next amount to adjust the vertices by 
				FVector2D NextUp = NextNormal * HalfThickness;

				FVector2D IntersectionPoint;
				if( LineIntersect( StartPos + Up, EndPos + Up, EndPos + NextUp, NextEndPos + NextUp, IntersectionPoint ) )
				{
					// If the lines intersect adjust where the line starts
					IntersectUpper = IntersectionPoint;

					// visualizes the intersection
					//AddQuadElement( IntersectUpper-FVector2D(1,1), FVector2D(2,2), 1, InClippingRect, Layer+1, FColor::Orange);
				}

				if( LineIntersect( StartPos - Up, EndPos - Up, EndPos - NextUp, NextEndPos - NextUp, IntersectionPoint ) )
				{
					// If the lines intersect adjust where the line starts
					IntersectLower = IntersectionPoint;

					// visualizes the intersection
					//AddQuadElement( IntersectLower-FVector2D(1,1), FVector2D(2,2), 1, InClippingRect, Layer+1, FColor::Yellow);
				}
			}

			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(IntersectUpper), FVector2f(1.0, 0.0f), FVector2f::ZeroVector, PointColor ) );
			RenderBatch.AddVertex(FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(IntersectLower), FVector2f(0.0, 0.0f), FVector2f::ZeroVector, PointColor ) );
			
			// Counterclockwise winding on triangles
			RenderBatch.AddIndex(IndexStart + 2 * Point - 1);
			RenderBatch.AddIndex(IndexStart + 2 * Point - 2);
			RenderBatch.AddIndex(IndexStart + 2 * Point + 0);

			RenderBatch.AddIndex(IndexStart + 2 * Point - 1);
			RenderBatch.AddIndex(IndexStart + 2 * Point + 0);
			RenderBatch.AddIndex(IndexStart + 2 * Point + 1);

			StartPos = EndPos;
		}

		EndPos = Points[NumPoints - 1];
		StartPos = Points[NumPoints - 2];
		const FVector2D EndCapDirection = HalfThickness*((EndPos-StartPos).GetSafeNormal());
		FLineBuilder::MakeCap(RenderBatch, RenderTransform, EndPos, EndCapDirection, Up, EndColor);
	}
	else
	{
		if (DrawElementPayload.GetThickness() == 1)
		{
			// Find a batch for the element
			FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, FShaderParams(), nullptr, ESlateDrawPrimitive::LineList, ESlateShader::Default, DrawEffects, ESlateBatchDrawFlag::None, DrawElement);

			// Generate the line segments using the native line rendering of the platform.
			for (int32 Point = 0; Point < NumPoints - 1; ++Point)
			{
				const uint32 IndexStart = RenderBatch.GetNumVertices();
				FVector2D StartPos = Points[Point];
				FVector2D EndPos = Points[Point + 1];

				FColor StartColor = PointColors.Num() ? PackVertexColor(PointColors[Point]  * DrawElementPayload.GetTint()) : FinalTint;
				FColor EndColor = PointColors.Num() ? PackVertexColor(PointColors[Point+1] * DrawElementPayload.GetTint()) : FinalTint;

				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(StartPos), FVector2f::ZeroVector, StartColor));
				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos), FVector2f::ZeroVector, EndColor));

				RenderBatch.AddIndex(IndexStart);
				RenderBatch.AddIndex(IndexStart + 1);
			}
		}
		else
		{
			// Find a batch for the element
			FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, FShaderParams(), nullptr, ESlateDrawPrimitive::TriangleList, ESlateShader::Default, DrawEffects, ESlateBatchDrawFlag::None, DrawElement);


			// Generate the line segments using non-aa'ed polylines.
			for (int32 Point = 0; Point < NumPoints - 1; ++Point)
			{
				const uint32 IndexStart = RenderBatch.GetNumVertices();
				const FVector2D StartPos = Points[Point];
				const FVector2D EndPos = Points[Point + 1];

				FColor StartColor	= PointColors.Num() ? PackVertexColor(PointColors[Point]   * DrawElementPayload.GetTint()) : FinalTint;
				FColor EndColor		= PointColors.Num() ? PackVertexColor(PointColors[Point+1] * DrawElementPayload.GetTint()) : FinalTint;
	
				const FVector2D SegmentNormal = (EndPos - StartPos).GetSafeNormal();
				const FVector2D HalfThickNormal = SegmentNormal * (DrawElementPayload.GetThickness() * 0.5f);

				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(StartPos + FVector2D(HalfThickNormal.Y, -HalfThickNormal.X)), FVector2f::ZeroVector, FVector2f::ZeroVector, StartColor));
				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(StartPos + FVector2D(-HalfThickNormal.Y, HalfThickNormal.X)), FVector2f::ZeroVector, FVector2f::ZeroVector, StartColor));
				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos + FVector2D(HalfThickNormal.Y, -HalfThickNormal.X)), FVector2f::ZeroVector, FVector2f::ZeroVector, EndColor));
				RenderBatch.AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(EndPos + FVector2D(-HalfThickNormal.Y, HalfThickNormal.X)), FVector2f::ZeroVector, FVector2f::ZeroVector, EndColor));

				RenderBatch.AddIndex(IndexStart + 0);
				RenderBatch.AddIndex(IndexStart + 1);
				RenderBatch.AddIndex(IndexStart + 2);

				RenderBatch.AddIndex(IndexStart + 2);
				RenderBatch.AddIndex(IndexStart + 1);
				RenderBatch.AddIndex(IndexStart + 3);
			}
		}
	}
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddViewportElement( const FSlateDrawElement& DrawElement )
{
	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FVector2D& LocalSize = DrawElement.GetLocalSize();
	const FSlateViewportPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateViewportPayload>();
	ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();
	const int32 Layer = DrawElement.GetLayer();

	const FColor FinalColor = PackVertexColor(DrawElementPayload.GetTint());

	ESlateBatchDrawFlag DrawFlags = DrawElement.GetBatchFlags();

	FSlateShaderResource* ViewportResource = DrawElementPayload.RenderTargetResource;
	ESlateShader ShaderType = ESlateShader::Default;

	if(DrawElementPayload.bViewportTextureAlphaOnly )
	{
		// This is a slight hack, but the grayscale font shader is the same as the general shader except it reads alpha only textures and doesn't support tiling
		ShaderType = ESlateShader::GrayscaleFont;
	}

	FSlateRenderBatch& RenderBatch = CreateRenderBatch( Layer, FShaderParams(), ViewportResource, ESlateDrawPrimitive::TriangleList, ShaderType, InDrawEffects, DrawFlags, DrawElement);

	// Tag this batch as requiring vsync if the viewport requires it.
	if( ViewportResource != nullptr && !DrawElementPayload.bAllowViewportScaling )
	{
		bRequiresVsync |= DrawElementPayload.bRequiresVSync;
	}

	// Do pixel snapping
	FVector2D TopLeft(0,0);
	FVector2D BotRight(LocalSize);

	// If the viewport disallows scaling, force size to current texture size.
	if (ViewportResource != nullptr && !DrawElementPayload.bAllowViewportScaling)
	{
		const float ElementScale = DrawElement.GetScale();
		BotRight = FVector2D(ViewportResource->GetWidth() / ElementScale, ViewportResource->GetHeight() / ElementScale);
	}

	FVector2D TopRight = FVector2D( BotRight.X, TopLeft.Y);
	FVector2D BotLeft =	 FVector2D( TopLeft.X, BotRight.Y);

	// The start index of these vertices in the index buffer
	const uint32 IndexStart = 0;

	// Add four vertices to the list of verts to be added to the vertex buffer
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(TopLeft),	FVector2f(0.0f,0.0f),	FinalColor ) );
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(TopRight),	FVector2f(1.0f,0.0f),	FinalColor ) );
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(BotLeft),	FVector2f(0.0f,1.0f),	FinalColor ) );
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f(BotRight),	FVector2f(1.0f,1.0f),	FinalColor ) );

	// Add 6 indices to the vertex buffer.  (2 tri's per quad, 3 indices per tri)
	RenderBatch.AddIndex( IndexStart + 0 );
	RenderBatch.AddIndex( IndexStart + 1 );
	RenderBatch.AddIndex( IndexStart + 2 );

	RenderBatch.AddIndex( IndexStart + 2 );
	RenderBatch.AddIndex( IndexStart + 1 );
	RenderBatch.AddIndex( IndexStart + 3 );
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::AddBorderElement( const FSlateDrawElement& DrawElement )
{
	const FSlateBoxPayload& DrawElementPayload = DrawElement.GetDataPayload<FSlateBoxPayload>();
	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FVector2D& LocalSize = DrawElement.GetLocalSize();
	const ESlateDrawEffect InDrawEffects = DrawElement.GetDrawEffects();

	const int32 Layer = DrawElement.GetLayer();

	const float DrawScale = DrawElement.GetScale();

	uint32 TextureWidth = 1;
	uint32 TextureHeight = 1;

	// Currently borders are not atlased because they are tiled.  So we just assume the texture proxy holds the actual texture
	const FSlateShaderResourceProxy* ResourceProxy = DrawElementPayload.GetResourceProxy();
	FSlateShaderResource* Resource = ResourceProxy ? ResourceProxy->Resource : nullptr;
	if( Resource )
	{
		TextureWidth = Resource->GetWidth();
		TextureHeight = Resource->GetHeight();
	}
	FVector2D TextureSizeLocalSpace = TransformVector(DrawElement.GetInverseLayoutTransform(), FVector2D((float)TextureWidth, (float)TextureHeight));
 
	// Texel offset
	const FVector2D HalfTexel( PixelCenterOffset/TextureWidth, PixelCenterOffset/TextureHeight );

	const FVector2D StartUV = HalfTexel;
	const FVector2D EndUV = FVector2D( 1.0f, 1.0f ) + HalfTexel;

	FMargin Margin = DrawElementPayload.GetBrushMargin();

	// Do pixel snapping
	FVector2D TopLeft(0,0);
	FVector2D BotRight(LocalSize);

	// Account for negative sizes
	bool bIsFlippedX = TopLeft.X > BotRight.X;
	bool bIsFlippedY = TopLeft.Y > BotRight.Y;
	Margin.Left = bIsFlippedX ? -Margin.Left : Margin.Left;
	Margin.Top = bIsFlippedY ? -Margin.Top : Margin.Top;
	Margin.Right = bIsFlippedX ? -Margin.Right : Margin.Right;
	Margin.Bottom = bIsFlippedY ? -Margin.Bottom : Margin.Bottom;

	// Determine the margins for each quad
	FVector2D TopLeftMargin(TextureSizeLocalSpace * FVector2D(Margin.Left, Margin.Top));
	FVector2D BotRightMargin(LocalSize - TextureSizeLocalSpace * FVector2D(Margin.Right, Margin.Bottom));

	float LeftMarginX = TopLeftMargin.X;
	float TopMarginY = TopLeftMargin.Y;
	float RightMarginX = BotRightMargin.X;
	float BottomMarginY = BotRightMargin.Y;

	// If the margins are overlapping the margins are too big or the button is too small
	// so clamp margins to half of the box size
	if (FMath::Abs(RightMarginX) < FMath::Abs(LeftMarginX))
	{
		LeftMarginX = LocalSize.X / 2;
		RightMarginX = LeftMarginX;
	}

	if (FMath::Abs(BottomMarginY) < FMath::Abs(TopMarginY))
	{
		TopMarginY = LocalSize.Y / 2;
		BottomMarginY = TopMarginY;
	}

	// Determine the texture coordinates for each quad
	float LeftMarginU = FMath::Abs(Margin.Left);
	float TopMarginV = FMath::Abs(Margin.Top);
	float RightMarginU = 1.0f - FMath::Abs(Margin.Right);
	float BottomMarginV = 1.0f - FMath::Abs(Margin.Bottom);

	LeftMarginU += HalfTexel.X;
	TopMarginV += HalfTexel.Y;
	BottomMarginV += HalfTexel.Y;
	RightMarginU += HalfTexel.X;

	// Determine the amount of tiling needed for the texture in this element.  The formula is number of pixels covered by the tiling portion of the texture / the number number of texels corresponding to the tiled portion of the texture.
	float TopTiling = 1.0f;
	float LeftTiling = 1.0f;
	float Denom = TextureSizeLocalSpace.X * (1.0f - Margin.GetTotalSpaceAlong<Orient_Horizontal>());
	if (!FMath::IsNearlyZero(Denom))
	{
		TopTiling = (RightMarginX - LeftMarginX) / Denom;
	}
	Denom = TextureSizeLocalSpace.Y * (1.0f - Margin.GetTotalSpaceAlong<Orient_Vertical>());
	if (!FMath::IsNearlyZero(Denom))
	{
		LeftTiling = (BottomMarginY - TopMarginY) / Denom;
	}
	
	FShaderParams ShaderParams = FShaderParams::MakePixelShaderParams(FVector4f(LeftMarginU,RightMarginU,TopMarginV,BottomMarginV) );

	// The tint color applies to all brushes and is passed per vertex
	const FColor Tint = PackVertexColor(DrawElementPayload.GetTint());

	// Pass the tiling information as a flag so we can pick the correct texture addressing mode
	ESlateBatchDrawFlag DrawFlags = (ESlateBatchDrawFlag::TileU|ESlateBatchDrawFlag::TileV);

	FSlateRenderBatch& RenderBatch = CreateRenderBatch( Layer, ShaderParams, Resource, ESlateDrawPrimitive::TriangleList, ESlateShader::Border, InDrawEffects, DrawFlags, DrawElement);

	// Ensure tiling of at least 1.  
	TopTiling = TopTiling >= 1.0f ? TopTiling : 1.0f;
	LeftTiling = LeftTiling >= 1.0f ? LeftTiling : 1.0f;
	float RightTiling = LeftTiling;
	float BottomTiling = TopTiling;

	FVector2D Position = TopLeft;
	FVector2D EndPos = BotRight;

	// The start index of these vertices in the index buffer
	const uint32 IndexStart = RenderBatch.GetNumVertices();

	// Zero for second UV indicates no tiling and to just pass the UV though (for the corner sections)
	FVector2D Zero(0,0);

	// Add all the vertices needed for this element.  Vertices are duplicated so that we can have some sections with no tiling and some with tiling.
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position ),					FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, StartUV.Y, 0.0f, 0.0f),				Tint ) ); //0
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, TopMarginV, 0.0f, 0.0f),				Tint ) ); //1
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, StartUV.Y, 0.0f, 0.0f),			Tint ) ); //2
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, TopMarginV, 0.0f, 0.0f),			Tint ) ); //3

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, StartUV.Y, TopTiling, 0.0f),			Tint ) ); //4
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, TopMarginV, TopTiling, 0.0f),		Tint ) ); //5
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, Position.Y ),	FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, StartUV.Y, TopTiling, 0.0f),			Tint ) ); //6
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, TopMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, TopMarginV, TopTiling, 0.0f),			Tint ) ); //7

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, Position.Y ),	FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, StartUV.Y, 0.0f, 0.0f),			Tint ) ); //8
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, TopMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, TopMarginV, 0.0f, 0.0f),			Tint ) ); //9
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, Position.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, StartUV.Y, 0.0f, 0.0f),				Tint ) ); //10
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, TopMarginV, 0.0f, 0.0f),				Tint ) ); //11

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, StartUV.Y, 0.0f, LeftTiling),		Tint ) ); //12
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, EndUV.Y, 0.0f, LeftTiling),			Tint ) ); //13
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, StartUV.Y, 0.0f, LeftTiling),		Tint ) ); //14
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, EndUV.Y, 0.0f, LeftTiling),		Tint ) ); //15

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, TopMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, StartUV.Y, 0.0f, RightTiling),	Tint ) ); //16
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, BottomMarginY ), FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, EndUV.Y, 0.0f, RightTiling),		Tint ) ); //17
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, TopMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, StartUV.Y, 0.0f, RightTiling),			Tint ) ); //18
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, BottomMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, EndUV.Y, 0.0f, RightTiling),			Tint ) ); //19

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, BottomMarginV, 0.0f, 0.0f),			Tint ) ); //20
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( Position.X, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, EndUV.Y, 0.0f, 0.0f),				Tint ) ); //21
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, BottomMarginV, 0.0f, 0.0f),		Tint ) ); //22
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( LeftMarginU, EndUV.Y, 0.0f, 0.0f),				Tint ) ); //23

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, BottomMarginV, BottomTiling, 0.0f),	Tint ) ); //24
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( LeftMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( StartUV.X, EndUV.Y, BottomTiling, 0.0f),		Tint ) ); //25
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX,BottomMarginY ),	FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, BottomMarginV, BottomTiling, 0.0f),	Tint ) ); //26
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, EndUV.Y, BottomTiling, 0.0f),			Tint ) ); //27

	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, BottomMarginY ), FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, BottomMarginV, 0.0f, 0.0f),		Tint ) ); //29
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( RightMarginX, EndPos.Y ),		FVector2f(LocalSize), DrawScale, FVector4f( RightMarginU, EndUV.Y, 0.0f, 0.0f),				Tint ) ); //30
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, BottomMarginY ),		FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, BottomMarginV, 0.0f, 0.0f),			Tint ) ); //31
	RenderBatch.AddVertex( FSlateVertex::Make<Rounding>( RenderTransform, FVector2f( EndPos.X, EndPos.Y ),			FVector2f(LocalSize), DrawScale, FVector4f( EndUV.X, EndUV.Y, 0.0f, 0.0f),					Tint ) ); //32

	// Top
	RenderBatch.AddIndex( IndexStart + 0 );
	RenderBatch.AddIndex( IndexStart + 1 );
	RenderBatch.AddIndex( IndexStart + 2 );
	RenderBatch.AddIndex( IndexStart + 2 );
	RenderBatch.AddIndex( IndexStart + 1 );
	RenderBatch.AddIndex( IndexStart + 3 );

	RenderBatch.AddIndex( IndexStart + 4 );
	RenderBatch.AddIndex( IndexStart + 5 );
	RenderBatch.AddIndex( IndexStart + 6 );
	RenderBatch.AddIndex( IndexStart + 6 );
	RenderBatch.AddIndex( IndexStart + 5 );
	RenderBatch.AddIndex( IndexStart + 7 );

	RenderBatch.AddIndex( IndexStart + 8 );
	RenderBatch.AddIndex( IndexStart + 9 );
	RenderBatch.AddIndex( IndexStart + 10 );
	RenderBatch.AddIndex( IndexStart + 10 );
	RenderBatch.AddIndex( IndexStart + 9 );
	RenderBatch.AddIndex( IndexStart + 11 );

	// Middle
	RenderBatch.AddIndex( IndexStart + 12 );
	RenderBatch.AddIndex( IndexStart + 13 );
	RenderBatch.AddIndex( IndexStart + 14 );
	RenderBatch.AddIndex( IndexStart + 14 );
	RenderBatch.AddIndex( IndexStart + 13 );
	RenderBatch.AddIndex( IndexStart + 15 );

	RenderBatch.AddIndex( IndexStart + 16 );
	RenderBatch.AddIndex( IndexStart + 17 );
	RenderBatch.AddIndex( IndexStart + 18 );
	RenderBatch.AddIndex( IndexStart + 18 );
	RenderBatch.AddIndex( IndexStart + 17 );
	RenderBatch.AddIndex( IndexStart + 19 );

	// Bottom
	RenderBatch.AddIndex( IndexStart + 20 );
	RenderBatch.AddIndex( IndexStart + 21 );
	RenderBatch.AddIndex( IndexStart + 22 );
	RenderBatch.AddIndex( IndexStart + 22 );
	RenderBatch.AddIndex( IndexStart + 21 );
	RenderBatch.AddIndex( IndexStart + 23 );

	RenderBatch.AddIndex( IndexStart + 24 );
	RenderBatch.AddIndex( IndexStart + 25 );
	RenderBatch.AddIndex( IndexStart + 26 );
	RenderBatch.AddIndex( IndexStart + 26 );
	RenderBatch.AddIndex( IndexStart + 25 );
	RenderBatch.AddIndex( IndexStart + 27 );

	RenderBatch.AddIndex( IndexStart + 28 );
	RenderBatch.AddIndex( IndexStart + 29 );
	RenderBatch.AddIndex( IndexStart + 30 );
	RenderBatch.AddIndex( IndexStart + 30 );
	RenderBatch.AddIndex( IndexStart + 29 );
	RenderBatch.AddIndex( IndexStart + 31 );
}

void FSlateElementBatcher::AddCustomElement( const FSlateDrawElement& DrawElement )
{
	const int32 Layer = DrawElement.GetLayer();

	FSlateRenderBatch& RenderBatch = CreateRenderBatch(Layer, FShaderParams(), nullptr, ESlateDrawPrimitive::None, ESlateShader::Default, ESlateDrawEffect::None, ESlateBatchDrawFlag::None, DrawElement);
	RenderBatch.CustomDrawer = DrawElement.GetDataPayload<FSlateCustomDrawerPayload>().CustomDrawer.Pin().Get();
	RenderBatch.bIsMergable = false;
}

void FSlateElementBatcher::AddCustomVerts(const FSlateDrawElement& DrawElement)
{
	const FSlateCustomVertsPayload& InPayload = DrawElement.GetDataPayload<FSlateCustomVertsPayload>();
	const int32 Layer = DrawElement.GetLayer();

	if (InPayload.Vertices.Num() > 0)
	{
		FSlateRenderBatch& RenderBatch = CreateRenderBatch(
			Layer, 
			FShaderParams(), 
			InPayload.ResourceProxy != nullptr ? InPayload.ResourceProxy->Resource : nullptr, 
			ESlateDrawPrimitive::TriangleList,
			ESlateShader::Custom, 
			DrawElement.GetDrawEffects(), 
			DrawElement.GetBatchFlags(),
			DrawElement);

		RenderBatch.bIsMergable = false;
		RenderBatch.InstanceCount = InPayload.NumInstances;
		RenderBatch.InstanceOffset = InPayload.InstanceOffset;
		RenderBatch.InstanceData = InPayload.InstanceData;

		RenderBatch.AddVertices(InPayload.Vertices);
		RenderBatch.AddIndices(InPayload.Indices);

	}
	/*FElementBatchMap& LayerToElementBatches = CurrentDrawLayer->GetElementBatchMap();

	const FSlateCustomVertsPayload& InPayload = DrawElement.GetDataPayload<FSlateCustomVertsPayload>();
	uint32 Layer = DrawElement.GetAbsoluteLayer();

	if (InPayload.Vertices.Num() >0)
	{
		// See if the layer already exists.
		TUniqueObj<FElementBatchArray>* ElementBatches = LayerToElementBatches.Find(Layer);
		if (!ElementBatches)
		{
			// The layer doesn't exist so make it now
			ElementBatches = &LayerToElementBatches.Add( Layer );
		}
		check(ElementBatches);

		FSlateElementBatch NewBatch(
			InPayload.ResourceProxy != nullptr ? InPayload.ResourceProxy->Resource : nullptr,
			FShaderParams(),
			ESlateShader::Custom,
			ESlateDrawPrimitive::TriangleList,
			DrawElement.GetDrawEffects(),
			DrawElement.GetBatchFlags(),
			DrawElement,
			InPayload.NumInstances,
			InPayload.InstanceOffset,
			InPayload.InstanceData
		);

		NewBatch.SaveClippingState(*PrecachedClippingStates);

		int32 Index = (*ElementBatches)->Add(NewBatch);
		FSlateElementBatch* ElementBatch = &(**ElementBatches)[Index];

		BatchData->AssignVertexArrayToBatch(*ElementBatch);
		BatchData->AssignIndexArrayToBatch(*ElementBatch);

		FSlateVertexArray& BatchVertices = BatchData->GetBatchVertexList(*ElementBatch);
		FSlateIndexArray& BatchIndices = BatchData->GetBatchIndexList(*ElementBatch);

		// Vertex Buffer since  it is already in slate format it is a straight copy
		BatchVertices = InPayload.Vertices;
		BatchIndices = InPayload.Indices;
	}*/
}


void FSlateElementBatcher::AddPostProcessPass(const FSlateDrawElement& DrawElement, const FVector2D& WindowSize)
{
	++NumPostProcessPasses;

	const FSlateRenderTransform& RenderTransform = DrawElement.GetRenderTransform();
	const FVector2D& LocalSize = DrawElement.GetLocalSize();
	
	const FSlatePostProcessPayload& Payload = DrawElement.GetDataPayload<FSlatePostProcessPayload>();

	//@todo doesn't work with rotated or skewed objects yet
	const FVector2D& Position = DrawElement.GetPosition();

	const int32 Layer = DrawElement.GetLayer();

	// Determine the four corners of the quad
	FVector2D TopLeft = FVector2D::ZeroVector;
	FVector2D TopRight = FVector2D(LocalSize.X, 0);
	FVector2D BotLeft = FVector2D(0, LocalSize.Y);
	FVector2D BotRight = FVector2D(LocalSize.X, LocalSize.Y);


	// Offset by half a texel if the platform requires it for pixel perfect sampling
	//FVector2D HalfTexel = FVector2D(PixelCenterOffset / WindowSize.X, PixelCenterOffset / WindowSize.Y);

	FVector2D WorldTopLeft = TransformPoint(RenderTransform, TopLeft).RoundToVector();
	FVector2D WorldBotRight = TransformPoint(RenderTransform, BotRight).RoundToVector();

	FVector2D SizeUV = (WorldBotRight - WorldTopLeft) / WindowSize;

	// These could be negative with rotation or negative scales.  This is not supported yet
	if(SizeUV.X > 0 && SizeUV.Y > 0)
	{
		FShaderParams Params = FShaderParams::MakePixelShaderParams(
			FVector4f(FVector2f(WorldTopLeft), FVector2f(WorldBotRight)),
			FVector4f(Payload.PostProcessData.X, Payload.PostProcessData.Y, (float)Payload.DownsampleAmount, 0.f),
			FVector4f(Payload.CornerRadius));

		CreateRenderBatch(Layer, Params, nullptr, ESlateDrawPrimitive::TriangleList, ESlateShader::PostProcess, ESlateDrawEffect::None, ESlateBatchDrawFlag::None, DrawElement);
	}
}

FSlateRenderBatch& FSlateElementBatcher::CreateRenderBatch(
	int32 Layer, 
	const FShaderParams& ShaderParams,
	const FSlateShaderResource* InResource,
	ESlateDrawPrimitive PrimitiveType,
	ESlateShader ShaderType,
	ESlateDrawEffect DrawEffects,
	ESlateBatchDrawFlag DrawFlags,
	const FSlateDrawElement& DrawElement)
{
	FSlateRenderBatch& NewBatch = CurrentCachedElementList
		? CurrentCachedElementList->AddRenderBatch(Layer, ShaderParams, InResource, PrimitiveType, ShaderType, DrawEffects, DrawFlags, DrawElement.GetSceneIndex())
		: BatchData->AddRenderBatch(Layer, ShaderParams, InResource, PrimitiveType, ShaderType, DrawEffects, DrawFlags, DrawElement.GetSceneIndex());

	NewBatch.ClippingState = ResolveClippingState(DrawElement);

	return NewBatch;
}

const FSlateClippingState* FSlateElementBatcher::ResolveClippingState(const FSlateDrawElement& DrawElement) const
{
	const FClipStateHandle& ClipHandle = DrawElement.GetClippingHandle();
	// Do cached first
	if (ClipHandle.GetCachedClipState())
	{
		// We should be working with cached elements if we have a cached clip state
		check(CurrentCachedElementList);
		return ClipHandle.GetCachedClipState();
	}
	else if (PrecachedClippingStates->IsValidIndex(ClipHandle.GetPrecachedClipIndex()))
	{
		// Store the clipping state so we can use it later for rendering.
		return &(*PrecachedClippingStates)[ClipHandle.GetPrecachedClipIndex()];
	}

	return nullptr;
}

template<ESlateVertexRounding Rounding>
void FSlateElementBatcher::BuildShapedTextSequence(const FShapedTextBuildContext& Context)
{
	const FShapedGlyphSequence* GlyphSequenceToRender = Context.ShapedGlyphSequence;

	FSlateShaderResourceManager& ResourceManager = *RenderingPolicy->GetResourceManager();

	float InvTextureSizeX = 0;
	float InvTextureSizeY = 0;

	FSlateRenderBatch* RenderBatch = nullptr;

	int32 FontTextureIndex = -1;
	FSlateShaderResource* FontAtlasTexture = nullptr;
	FSlateShaderResource* FontShaderResource = nullptr;

	float LineX = Context.StartLineX;
	float LineY = Context.StartLineY;

	FSlateRenderTransform RenderTransform = *Context.RenderTransform;

	FColor Tint = FColor::White;

	ETextOverflowDirection OverflowDirection = Context.OverflowDirection;

	float EllipsisLineX = 0;
	float EllipsisLineY = 0;
	bool bNeedEllipsis = false;
	bool bChararcterWasClipped = false;

	// For left to right overflow direction - Sum of total whitespace we're currently advancing through. Once a non-whitespace glyph is detected this will return to 0
	// For right to left this value is unused. We just skip all leading whitespace
	float PreviousWhitespaceAdvance = 0;

	int32 NumGlyphs = GlyphSequenceToRender->GetGlyphsToRender().Num();
	const TArray<FShapedGlyphEntry>& GlyphsToRender = GlyphSequenceToRender->GetGlyphsToRender();
	for (int32 GlyphIndex = 0; GlyphIndex < NumGlyphs; ++GlyphIndex)
	{
		const FShapedGlyphEntry& GlyphToRender = GlyphsToRender[GlyphIndex];

		const float BitmapRenderScale = GlyphToRender.GetBitmapRenderScale();
		const float InvBitmapRenderScale = 1.0f / BitmapRenderScale;

		float X = 0;
		float SizeX = 0;
		float Y = 0;
		float U = 0;
		float V = 0;
		float SizeY = 0;
		float SizeU = 0;
		float SizeV = 0;

		bool bCanRenderGlyph = GlyphToRender.bIsVisible;
		if (bCanRenderGlyph)
		{
			// Get Sizing and atlas info
			const FShapedGlyphFontAtlasData GlyphAtlasData = Context.FontCache->GetShapedGlyphFontAtlasData(GlyphToRender, *Context.OutlineSettings);
			if (GlyphAtlasData.Valid && (!Context.bEnableOutline || GlyphAtlasData.SupportsOutline))
			{
				X = LineX + GlyphAtlasData.HorizontalOffset + GlyphToRender.XOffset;
				// Note PosX,PosY is the upper left corner of the bounding box representing the string.  This computes the Y position of the baseline where text will sit

				if (Context.bEnableCulling)
				{
					if (X + GlyphAtlasData.USize < Context.LocalClipBoundingBoxLeft)
					{
						LineX += GlyphToRender.XAdvance;
						LineY += GlyphToRender.YAdvance;
						continue;
					}
					else if (X > Context.LocalClipBoundingBoxRight)
					{
						break;
					}
				}

				if (FontAtlasTexture == nullptr || GlyphAtlasData.TextureIndex != FontTextureIndex)
				{
					// Font has a new texture for this glyph. Refresh the batch we use and the index we are currently using
					FontTextureIndex = GlyphAtlasData.TextureIndex;

					ISlateFontTexture* SlateFontTexture = Context.FontCache->GetFontTexture(FontTextureIndex);
					check(SlateFontTexture);

					FontAtlasTexture = SlateFontTexture->GetSlateTexture();
					check(FontAtlasTexture);

					FontShaderResource = ResourceManager.GetFontShaderResource(FontTextureIndex, FontAtlasTexture, Context.FontMaterial);
					check(FontShaderResource);

					const bool bIsGrayscale = SlateFontTexture->IsGrayscale();
					Tint = bIsGrayscale ? Context.FontTint : FColor::White;

					RenderBatch = &CreateRenderBatch(Context.LayerId, FShaderParams(), FontShaderResource, ESlateDrawPrimitive::TriangleList, bIsGrayscale ? ESlateShader::GrayscaleFont : ESlateShader::ColorFont, Context.DrawElement->GetDrawEffects(), ESlateBatchDrawFlag::None, *Context.DrawElement);

					// Reserve memory for the glyphs.  This isn't perfect as the text could contain spaces and we might not render the rest of the text in this batch but its better than resizing constantly
					const int32 GlyphsLeft = NumGlyphs - GlyphIndex;
					RenderBatch->ReserveVertices(GlyphsLeft * 4);
					RenderBatch->ReserveIndices(GlyphsLeft * 6);

					InvTextureSizeX = 1.0f / FontAtlasTexture->GetWidth();
					InvTextureSizeY = 1.0f / FontAtlasTexture->GetHeight();
				}


				Y = LineY - GlyphAtlasData.VerticalOffset + GlyphToRender.YOffset + ((Context.MaxHeight + Context.TextBaseline) * InvBitmapRenderScale);
				U = GlyphAtlasData.StartU * InvTextureSizeX;
				V = GlyphAtlasData.StartV * InvTextureSizeY;
				SizeX = GlyphAtlasData.USize * BitmapRenderScale;
				SizeY = GlyphAtlasData.VSize * BitmapRenderScale;
				SizeU = GlyphAtlasData.USize * InvTextureSizeX;
				SizeV = GlyphAtlasData.VSize * InvTextureSizeY;
			}
			else
			{
				bCanRenderGlyph = false;
			}

		}
		else
		{
			X = LineX;
			SizeX = GlyphToRender.XAdvance;
		}

		// Overflow Detection
		// First figure out the size of the glyph. If the glyph contains multiple characters we have to measure all of them and if clicked, omit them all. This is common in complex languages with lots of diacritics
		float OverflowTestWidth = SizeX;
		if (OverflowDirection != ETextOverflowDirection::NoOverflow && (GlyphToRender.NumGraphemeClustersInGlyph > 1 || GlyphToRender.NumCharactersInGlyph > 1))
		{
			const int32 StartIndex = GlyphIndex;
			int32 EndIndex = GlyphIndex;
			int32 NextIndex = GlyphIndex + 1;
			const int32 SourceIndex = GlyphToRender.SourceIndex;
			while (NextIndex < NumGlyphs && GlyphsToRender[NextIndex].SourceIndex == SourceIndex)
			{
				++EndIndex;
				++NextIndex;
			}
			if (StartIndex < EndIndex)
			{
				OverflowTestWidth = GlyphSequenceToRender->GetMeasuredWidth(StartIndex, EndIndex).Get(SizeX);
			}
		}

		// Left to right overflow - If the current pen position + the ellipsis cannot fit, we have reached the end of the possible area for drawing this text. 
		if (OverflowDirection == ETextOverflowDirection::LeftToRight)
		{
			// If we are on the last glyph dont bother checking if the ellipsis can fit. If the last glyph can fit there is no need for ellipsis
			float OverflowSequenceNeededSize = GlyphIndex < NumGlyphs - 1 ? Context.OverflowGlyphSequence->GetMeasuredWidth() : 0;
			if(X + OverflowTestWidth + OverflowSequenceNeededSize >= Context.LocalClipBoundingBoxRight)
			{
				bNeedEllipsis = true;
				// We subtract out any whitespace advance. This avoids the ellipsis from ever floating out in the middle of a block of whitespace.
				// e.g without this something like "The quick brown		fox jumps over the lazy dog" could be clipped to "The quick brown	..." but we want it to be "The quick brown..."
				EllipsisLineX = LineX - PreviousWhitespaceAdvance;
				EllipsisLineY = LineY;
				// No characters to render after the ellipsis on the right side
				break;
			}
		}
		else if(OverflowDirection == ETextOverflowDirection::RightToLeft)
		{
			bool bClipped = false;
			// Right to left overflow
			if (X < Context.LocalClipBoundingBoxLeft)
			{
				// This glyph is in the clipped region or is not visible so just advance. It cannot be shown
				bClipped = true;
				bChararcterWasClipped = true;
			}
			else if(bChararcterWasClipped)
			{
				// Can the ellipsis fit in the free spot by skipping the previous glyph(s)
				float EllipsisWidth = Context.OverflowGlyphSequence->GetMeasuredWidth();
				float AvailableX = X-Context.LocalClipBoundingBoxLeft;
				if (AvailableX >= EllipsisWidth)
				{
					// The available area can fit the ellipsis. Mark that we need an ellipsis and stop checking for overflow. The rest of the text can be built normally
					bNeedEllipsis = true;
					EllipsisLineX = (LineX - EllipsisWidth);
					EllipsisLineY = LineY;
					OverflowDirection = ETextOverflowDirection::NoOverflow;
				}
				else
				{
					bClipped = true;
					bChararcterWasClipped = true;
				}

			}
			// If we just clipped a glyph omit all characters in said glyph. Otherwise floating diacritics would be visible above the ellipsis. This is common in complex languages.
			if (bClipped && GlyphToRender.NumCharactersInGlyph > 1)
			{
				GlyphIndex += GlyphToRender.NumCharactersInGlyph - 1;
				LineX += OverflowTestWidth;
				continue;
			}

			bCanRenderGlyph = !bClipped;
		}

		if(bCanRenderGlyph && RenderBatch)
		{
			const FVector2D UpperLeft(X, Y);
			const FVector2D UpperRight(X + SizeX, Y);
			const FVector2D LowerLeft(X, Y + SizeY);
			const FVector2D LowerRight(X + SizeX, Y + SizeY);

			// The start index of these vertices in the index buffer
			const uint32 IndexStart = RenderBatch->GetNumVertices();

			float Ut = 0.0f, Vt = 0.0f, UtMax = 0.0f, VtMax = 0.0f;
			if (Context.FontMaterial)
			{
				float DistAlpha = (float)GlyphIndex / NumGlyphs;
				float DistAlphaNext = (float)(GlyphIndex + 1) / NumGlyphs;

				// This creates a set of UV's that goes from 0-1, from left to right of the string in U and 0-1 baseline to baseline top to bottom in V
				Ut = FMath::Lerp(0.0f, 1.0f, DistAlpha);
				Vt = FMath::Lerp(0.0f, 1.0f, UpperLeft.Y / Context.MaxHeight);

				UtMax = FMath::Lerp(0.0f, 1.0f, DistAlphaNext);
				VtMax = FMath::Lerp(0.0f, 1.0f, LowerLeft.Y / Context.MaxHeight);
			}

			// Add four vertices to the list of verts to be added to the vertex buffer
			RenderBatch->AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(UpperLeft), FVector4f(U, V, Ut, Vt), FVector2f(0.0f, 0.0f), Tint));
			RenderBatch->AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(LowerRight.X, UpperLeft.Y), FVector4f(U + SizeU, V, UtMax, Vt), FVector2f(1.0f, 0.0f), Tint));
			RenderBatch->AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(UpperLeft.X, LowerRight.Y), FVector4f(U, V + SizeV, Ut, VtMax), FVector2f(0.0f, 1.0f), Tint));
			RenderBatch->AddVertex(FSlateVertex::Make<Rounding>(RenderTransform, FVector2f(LowerRight), FVector4f(U + SizeU, V + SizeV, UtMax, VtMax), FVector2f(1.0f, 1.0f), Tint));

			RenderBatch->AddIndex(IndexStart + 0);
			RenderBatch->AddIndex(IndexStart + 1);
			RenderBatch->AddIndex(IndexStart + 2);
			RenderBatch->AddIndex(IndexStart + 1);
			RenderBatch->AddIndex(IndexStart + 3);
			RenderBatch->AddIndex(IndexStart + 2);

			// Reset whitespace advance to 0, this is a visible character
			PreviousWhitespaceAdvance = 0;
		}
		else if(!GlyphToRender.bIsVisible)
		{
			// How much whitespace we are currently walking through
			PreviousWhitespaceAdvance += GlyphToRender.XAdvance;
		}

		LineX += GlyphToRender.XAdvance;
		LineY += GlyphToRender.YAdvance;
	}

	if (!bNeedEllipsis && Context.bForceEllipsis)
	{
		bNeedEllipsis = true;
		EllipsisLineX = LineX;
		EllipsisLineY = LineY;
	}

	if (bNeedEllipsis)
	{
		// Ellipsis can fit, place it at the current lineX
		FShapedTextBuildContext EllipsisContext = Context;
		EllipsisContext.bForceEllipsis = false;
		EllipsisContext.ShapedGlyphSequence = Context.OverflowGlyphSequence;
		EllipsisContext.OverflowGlyphSequence = nullptr;
		EllipsisContext.bEnableCulling = false;
		EllipsisContext.OverflowDirection = ETextOverflowDirection::NoOverflow;
		EllipsisContext.StartLineX = EllipsisLineX;
		EllipsisContext.StartLineY = EllipsisLineY;

		BuildShapedTextSequence<Rounding>(EllipsisContext);
	}
}

void FSlateElementBatcher::ResetBatches()
{
	bRequiresVsync = false;
	NumPostProcessPasses = 0;
}

