// Copyright Epic Games, Inc. All Rights Reserved.

#include "Channels/SCurveEditorKeyBarView.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"
#include "CurveEditor.h"
#include "EditorStyleSet.h"
#include "CurveModel.h"
#include "ICurveEditorBounds.h"
#include "CurveEditorScreenSpace.h"
#include "CurveDataAbstraction.h"
#include "CurveDrawInfo.h"
#include "KeyBarCurveModel.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "SCurveEditorPanel.h"

float SCurveEditorKeyBarView::TrackHeight = 24.f;

void SCurveEditorKeyBarView::Construct(const FArguments& InArgs, TWeakPtr<FCurveEditor> InCurveEditor)
{
	bFixedOutputBounds = true;
	OutputMin = -0.5;
	OutputMax =  0.5;
	WeakCurveEditor = InCurveEditor;
	SortBias = 25;

	SInteractiveCurveEditorView::Construct(InArgs, InCurveEditor);
}

FVector2D SCurveEditorKeyBarView::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(100.f, TrackHeight * (CurveInfoByID.Num()));
}

void SCurveEditorKeyBarView::GetGridLinesY(TSharedRef<const FCurveEditor> CurveEditor, TArray<float>& MajorGridLines, TArray<float>& MinorGridLines, TArray<FText>* MajorGridLabels) const
{}

void SCurveEditorKeyBarView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (!CurveEditor)
	{
		return;
	}

	double Count = 0.0;
	for (auto It = CurveInfoByID.CreateIterator(); It; ++It)
	{
		FCurveModel* Curve = CurveEditor->FindCurve(It.Key());
		if (ensureAlways(Curve))
		{
			It->Value.ViewToCurveTransform = FTransform2D(FVector2D(0.f, Count));
		}

		Count += 1.0;
	}

	OutputMin = OutputMax - FMath::Max(Count, 1e-10);
	SInteractiveCurveEditorView::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
}

void SCurveEditorKeyBarView::PaintView(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 BaseLayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (CurveEditor)
	{
		const ESlateDrawEffect DrawEffects = ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;

		DrawBackground(AllottedGeometry, OutDrawElements, BaseLayerId, DrawEffects);
		DrawLabels(AllottedGeometry, OutDrawElements, BaseLayerId, DrawEffects);
		DrawGridLines(CurveEditor.ToSharedRef(), AllottedGeometry, OutDrawElements, BaseLayerId, DrawEffects);
		DrawCurves(CurveEditor.ToSharedRef(), AllottedGeometry, MyCullingRect, OutDrawElements, BaseLayerId, InWidgetStyle, DrawEffects);
	}
}

void SCurveEditorKeyBarView::DrawLabels(const FGeometry& AllottedGeometry, FSlateWindowElementList& OutDrawElements, int32 BaseLayerId, ESlateDrawEffect DrawEffects) const
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (!CurveEditor)
	{
		return;
	}

	const FSlateBrush* WhiteBrush = FEditorStyle::GetBrush("WhiteBrush");

	// Draw some text telling the user how to get retime anchors.
	const FSlateFontInfo FontInfo = FCoreStyle::Get().GetFontStyle("ToolTip.LargerFont");

	// We have to measure the string so we can draw it centered on the window.
	const TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();

	const FCurveEditorScreenSpace ViewSpace = GetViewSpace();

	for (auto It = CurveInfoByID.CreateConstIterator(); It; ++It)
	{
		FCurveModel* Curve = CurveEditor->FindCurve(It.Key());
		if (!ensureAlways(Curve))
		{
			continue;
		}

		const int32 CurveIndex = static_cast<int32>(It->Value.ViewToCurveTransform.GetTranslation().Y);

		FCurveEditorScreenSpace CurveSpace = GetCurveSpace(It.Key());

		const float LaneTop = CurveSpace.ValueToScreen(0.0) - TrackHeight*.5f;


		double InputMin = 0, InputMax = 1;
		GetInputBounds(InputMin, InputMax);//in Sequencer Seconds

		// Draw the curve label and background
		FKeyBarCurveModel* KeyBarCurveModel = static_cast<FKeyBarCurveModel*>(Curve);
		if (KeyBarCurveModel)
		{
			TArray<FKeyBarCurveModel::FBarRange> Ranges = KeyBarCurveModel->FindRanges();
			int32 LastLabelPos = -1;
			for (int32 Index = 0;Index < Ranges.Num(); ++Index)
			{
				const FKeyBarCurveModel::FBarRange& Range = Ranges[Index];
				const double LowerSeconds = Range.Range.GetLowerBoundValue();
				const double UpperSeconds = Range.Range.GetUpperBoundValue();
				const bool bOutsideUpper = (Index != Ranges.Num() - 1) && UpperSeconds < InputMin;
				if (LowerSeconds > InputMax || bOutsideUpper) //out of range
				{
					continue;
				}
				
				FLinearColor CurveColor = Range.Color;

				// Alpha blend the zebra tint
				if (CurveIndex % 2)
				{
					static FLinearColor ZebraTint = FLinearColor::White.CopyWithNewOpacity(0.01f);
					if (CurveColor == FLinearColor::White)
					{
						CurveColor = ZebraTint;
					}
					else
					{
						CurveColor = CurveColor * (1.f - ZebraTint.A) + ZebraTint * ZebraTint.A;
					}
				}

				if (CurveColor != FLinearColor::White)
				{
					const double LowerSecondsForBox =(Index == 0 && LowerSeconds > InputMin )? InputMin : LowerSeconds;
					const double BoxPos = CurveSpace.SecondsToScreen(LowerSecondsForBox);

					const FPaintGeometry BoxGeometry = AllottedGeometry.ToPaintGeometry(
						FVector2D(AllottedGeometry.GetLocalSize().X, TrackHeight),
						FSlateLayoutTransform(FVector2D(BoxPos, LaneTop))
					);

					FSlateDrawElement::MakeBox(OutDrawElements, BaseLayerId + CurveViewConstants::ELayerOffset::Background, BoxGeometry, WhiteBrush, DrawEffects, CurveColor);
				}
				const double LowerSecondsForLabel = (InputMin > LowerSeconds) ? InputMin : LowerSeconds;
				double LabelPos = CurveSpace.SecondsToScreen(LowerSecondsForLabel) + 10;

				const FText Label = FText::FromName(Range.Name);
				const FVector2D TextSize = FontMeasure->Measure(Label, FontInfo);
				if (Index > 0)
				{
					LabelPos = (LabelPos < LastLabelPos) ? LastLabelPos + 5 : LabelPos;
				}
				LastLabelPos = LabelPos + TextSize.X + 15;
				const FVector2D Position(LabelPos, LaneTop + (TrackHeight - TextSize.Y) * .5f);

				const FPaintGeometry LabelGeometry = AllottedGeometry.ToPaintGeometry(
					FSlateLayoutTransform(Position)
				);

				FSlateDrawElement::MakeText(OutDrawElements, BaseLayerId + CurveViewConstants::ELayerOffset::Labels, LabelGeometry, Label, FontInfo, DrawEffects, FLinearColor::White);

			}
		}
	}
}

void SCurveEditorKeyBarView::BuildContextMenu(FMenuBuilder & MenuBuilder, TOptional<FCurvePointHandle> ClickedPoint, TOptional<FCurveModelID> HoveredCurveID)
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (!CurveEditor)
	{
		return;
	}
	int32 NumSelectedKeys = CurveEditor->GetSelection().Count();

	FCurveModel* Curve = HoveredCurveID.IsSet() ? CurveEditor->FindCurve(HoveredCurveID.GetValue()) : nullptr;
	FKeyBarCurveModel* KeyBarCurveModel = static_cast<FKeyBarCurveModel*>(Curve);
	if (KeyBarCurveModel)
	{
		KeyBarCurveModel->BuildContextMenu(*CurveEditor,MenuBuilder, ClickedPoint);
	}
}




