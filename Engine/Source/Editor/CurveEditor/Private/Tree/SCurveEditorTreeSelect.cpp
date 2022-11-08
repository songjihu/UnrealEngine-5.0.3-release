// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tree/SCurveEditorTreeSelect.h"
#include "CurveEditor.h"
#include "Algo/AllOf.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Views/STableRow.h"

#include "EditorStyleSet.h"
#include "Framework/Application/SlateApplication.h"


void SCurveEditorTreeSelect::Construct(const FArguments& InArgs, TWeakPtr<FCurveEditor> InCurveEditor, FCurveEditorTreeItemID InTreeItemID, const TSharedRef<ITableRow>& InTableRow)
{
	WeakCurveEditor = InCurveEditor;
	WeakTableRow = InTableRow;
	TreeItemID = InTreeItemID;

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FEditorStyle::Get(), "HoverHintOnly")
		.Visibility(this, &SCurveEditorTreeSelect::GetSelectVisibility)
		.OnClicked(this, &SCurveEditorTreeSelect::SelectAll)
		[
			SNew(SImage)
			.Image(this, &SCurveEditorTreeSelect::GetSelectBrush)
		]
	];
}

FReply SCurveEditorTreeSelect::SelectAll()
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (!CurveEditor)
	{
		return FReply::Handled();
	}
	
	FCurveEditorTreeItem* Item = CurveEditor->FindTreeItem(TreeItemID);
	if (!Item)
	{
		return FReply::Handled();
	}			
	
	const bool bIsShiftDown = FSlateApplication::Get().GetModifierKeys().IsShiftDown();
	const bool bIsControlDown = FSlateApplication::Get().GetModifierKeys().IsControlDown();

	if (!bIsShiftDown && !bIsControlDown)
	{
		CurveEditor->GetSelection().Clear();
	}
		
	for (FCurveModelID CurveID : Item->GetCurves())
	{
		FCurveModel* CurveModel = CurveEditor.Get()->FindCurve(CurveID);
		if (CurveModel)
		{
			TArray<FKeyHandle> KeyHandles;
			KeyHandles.Reserve(CurveModel->GetNumKeys());
			CurveModel->GetKeys(*CurveEditor.Get(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Max(), KeyHandles);
		
			if (bIsControlDown)
			{
				CurveEditor.Get()->GetSelection().Toggle(CurveID, ECurvePointType::Key, KeyHandles);
			}
			else
			{
				CurveEditor.Get()->GetSelection().Add(CurveID, ECurvePointType::Key, KeyHandles);
			}
		}
	}

	return FReply::Handled();
}


EVisibility SCurveEditorTreeSelect::GetSelectVisibility() const
{
	TSharedPtr<FCurveEditor> CurveEditor = WeakCurveEditor.Pin();
	if (!CurveEditor)
	{
		return EVisibility::Collapsed;
	}
	
	FCurveEditorTreeItem* Item = CurveEditor->FindTreeItem(TreeItemID);
	if (!Item)
	{
		return EVisibility::Collapsed;
	}
		
	for (FCurveModelID CurveID : Item->GetCurves())
	{
		if (CurveEditor.Get()->GetSelection().GetAll().Contains(CurveID))
		{
			return EVisibility::Visible;
		}
	}

	return EVisibility::Collapsed;
}

const FSlateBrush* SCurveEditorTreeSelect::GetSelectBrush() const
{
	return FEditorStyle::GetBrush("GenericCurveEditor.Select");
}
