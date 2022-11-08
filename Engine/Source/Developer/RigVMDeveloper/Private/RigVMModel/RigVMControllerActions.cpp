// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigVMModel/RigVMControllerActions.h"
#include "UObject/Package.h"

#if WITH_EDITOR
#include "Misc/ITransaction.h"
#endif

FRigVMActionWrapper::FRigVMActionWrapper(const FRigVMActionKey& Key)
{
	ScriptStruct = FindObjectChecked<UScriptStruct>(ANY_PACKAGE, *Key.ScriptStructPath);
	Data.SetNumUninitialized(ScriptStruct->GetStructureSize());
	ScriptStruct->InitializeStruct(Data.GetData(), 1);
	ScriptStruct->ImportText(*Key.ExportedText, Data.GetData(), nullptr, PPF_None, nullptr, ScriptStruct->GetName());
}
FRigVMActionWrapper::~FRigVMActionWrapper()
{
	if (Data.Num() > 0 && ScriptStruct != nullptr)
	{
		ScriptStruct->DestroyStruct(Data.GetData(), 1);
	}
}

FRigVMBaseAction* FRigVMActionWrapper::GetAction()
{
	return (FRigVMBaseAction*)Data.GetData();
}

FString FRigVMActionWrapper::ExportText()
{
	FString ExportedText;
	if (Data.Num() > 0 && ScriptStruct != nullptr)
	{
		TArray<uint8, TAlignedHeapAllocator<16>> DefaultStructData;
		DefaultStructData.AddZeroed(ScriptStruct->GetStructureSize());
		ScriptStruct->InitializeDefaultValue(DefaultStructData.GetData());
		
		ScriptStruct->ExportText(ExportedText, GetAction(), DefaultStructData.GetData(), nullptr, PPF_None, nullptr);
	}
	return ExportedText;
}

bool URigVMActionStack::OpenUndoBracket(const FString& InTitle)
{
	FRigVMBaseAction* Action = new FRigVMBaseAction;
	Action->Title = InTitle;
	BracketActions.Add(Action);
	BeginAction(*Action);
	return true;
}

bool URigVMActionStack::CloseUndoBracket()
{
	ensure(BracketActions.Num() > 0);
	FRigVMBaseAction* Action = BracketActions.Pop();
	EndAction(*Action);
	delete(Action);
	return true;
}

bool URigVMActionStack::CancelUndoBracket()
{
	ensure(BracketActions.Num() > 0);
	FRigVMBaseAction* Action = BracketActions.Pop();
	CancelAction(*Action);
	delete(Action);
	return true;
}

bool URigVMActionStack::Undo(URigVMController* InController)
{
	check(InController)

	if (UndoActions.Num() == 0)
	{
		InController->ReportWarning(TEXT("Nothing to undo."));
		return false;
	}

	FRigVMActionKey KeyToUndo = UndoActions.Pop();
	ActionIndex = UndoActions.Num();
	
	FRigVMActionWrapper Wrapper(KeyToUndo);
	if (Wrapper.GetAction()->Undo(InController))
	{
		RedoActions.Add(KeyToUndo);
		return true;
	}

	InController->ReportAndNotifyErrorf(TEXT("Error while undoing action %s."), *Wrapper.GetAction()->Title);
	return false;
}

bool URigVMActionStack::Redo(URigVMController* InController)
{
	check(InController)

	if (RedoActions.Num() == 0)
	{
		InController->ReportWarning(TEXT("Nothing to redo."));
		return false;
	}

	FRigVMActionKey KeyToRedo = RedoActions.Pop();

	FRigVMActionWrapper Wrapper(KeyToRedo);
	if (Wrapper.GetAction()->Redo(InController))
	{
		UndoActions.Add(KeyToRedo);
		ActionIndex = UndoActions.Num();
		return true;
	}

	InController->ReportAndNotifyErrorf(TEXT("Error while undoing action %s."), *Wrapper.GetAction()->Title);
	return false;
}

#if WITH_EDITOR

void URigVMActionStack::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	if (TransactionEvent.GetEventType() == ETransactionObjectEventType::UndoRedo)
	{
		URigVMController* Controller = Cast< URigVMController>(GetOuter());
		if (Controller == nullptr)
		{
			return;
		}

		int32 DesiredActionIndex = ActionIndex;
		ActionIndex = UndoActions.Num();

		if (DesiredActionIndex == ActionIndex)
		{
			return;
		}

		ModifiedEvent.Broadcast(ERigVMGraphNotifType::InteractionBracketOpened, nullptr, nullptr);

		while (DesiredActionIndex < ActionIndex)
		{
			if (UndoActions.Num() == 0)
			{
				break;
			}
			if (!Undo(Controller))
			{
				ModifiedEvent.Broadcast(ERigVMGraphNotifType::InteractionBracketCanceled, nullptr, nullptr);
				return;
			}
		}
		while (DesiredActionIndex > ActionIndex)
		{
			if (RedoActions.Num() == 0)
			{
				break;
			}
			if (!Redo(Controller))
			{
				ModifiedEvent.Broadcast(ERigVMGraphNotifType::InteractionBracketCanceled, nullptr, nullptr);
				return;
			}
		}

		ModifiedEvent.Broadcast(ERigVMGraphNotifType::InteractionBracketClosed, nullptr, nullptr);
	}
}

#endif


bool FRigVMBaseAction::Merge(const FRigVMBaseAction* Other)
{
	return SubActions.Num() == 0 && Other->SubActions.Num() == 0;
}

bool FRigVMBaseAction::Undo(URigVMController* InController)
{
	bool Result = true;
	for (int32 KeyIndex = SubActions.Num() - 1; KeyIndex >= 0; KeyIndex--)
	{
		FRigVMActionWrapper Wrapper(SubActions[KeyIndex]);
		if(!Wrapper.GetAction()->Undo(InController))
		{
			InController->ReportAndNotifyErrorf(TEXT("Error while undoing action '%s'."), *Wrapper.GetAction()->Title);
			Result = false;
		}
	}
	return Result;
}

bool FRigVMBaseAction::Redo(URigVMController* InController)
{
	bool Result = true;
	for (int32 KeyIndex = 0; KeyIndex < SubActions.Num(); KeyIndex++)
	{
		FRigVMActionWrapper Wrapper(SubActions[KeyIndex]);
		if (!Wrapper.GetAction()->Redo(InController))
		{
			InController->ReportAndNotifyErrorf(TEXT("Error while redoing action '%s'."), *Wrapper.GetAction()->Title);
			Result = false;
		}
	}
	return Result;
}

bool FRigVMInverseAction::Undo(URigVMController* InController)
{
	return FRigVMBaseAction::Redo(InController);
}

bool FRigVMInverseAction::Redo(URigVMController* InController)
{
	return FRigVMBaseAction::Undo(InController);
}

FRigVMAddUnitNodeAction::FRigVMAddUnitNodeAction()
	: ScriptStructPath()
	, MethodName(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddUnitNodeAction::FRigVMAddUnitNodeAction(URigVMUnitNode* InNode)
	: ScriptStructPath(InNode->GetScriptStruct()->GetPathName())
	, MethodName(InNode->GetMethodName())
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
}

bool FRigVMAddUnitNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddUnitNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMUnitNode* Node = InController->AddUnitNodeFromStructPath(ScriptStructPath, MethodName, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddVariableNodeAction::FRigVMAddVariableNodeAction()
	: VariableName(NAME_None)
	, CPPType()
	, CPPTypeObjectPath()
	, bIsGetter(false)
	, DefaultValue()
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddVariableNodeAction::FRigVMAddVariableNodeAction(URigVMVariableNode* InNode)
	: VariableName(InNode->GetVariableName())
	, CPPType(InNode->GetCPPType())
	, CPPTypeObjectPath()
	, bIsGetter(InNode->IsGetter())
	, DefaultValue(InNode->GetDefaultValue())
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if (InNode->GetCPPTypeObject())
	{
		CPPTypeObjectPath = InNode->GetCPPTypeObject()->GetPathName();
	}
}

bool FRigVMAddVariableNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddVariableNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMVariableNode* Node = InController->AddVariableNodeFromObjectPath(VariableName, CPPType, CPPTypeObjectPath, bIsGetter, DefaultValue, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddParameterNodeAction::FRigVMAddParameterNodeAction()
	: ParameterName(NAME_None)
	, CPPType()
	, CPPTypeObjectPath()
	, bIsInput(false)
	, DefaultValue()
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddParameterNodeAction::FRigVMAddParameterNodeAction(URigVMParameterNode* InNode)
	: ParameterName(InNode->GetParameterName())
	, CPPType(InNode->GetCPPType())
	, CPPTypeObjectPath()
	, bIsInput(InNode->IsInput())
	, DefaultValue(InNode->GetDefaultValue())
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if (InNode->GetCPPTypeObject())
	{
		CPPTypeObjectPath = InNode->GetCPPTypeObject()->GetPathName();
	}
}

bool FRigVMAddParameterNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddParameterNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMParameterNode* Node = InController->AddParameterNodeFromObjectPath(ParameterName, CPPType, CPPTypeObjectPath, bIsInput, DefaultValue, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddCommentNodeAction::FRigVMAddCommentNodeAction()
	: CommentText()
	, Position(FVector2D::ZeroVector)
	, Size(FVector2D::ZeroVector)
	, Color(FLinearColor::Black)
	, NodePath()
{
}

FRigVMAddCommentNodeAction::FRigVMAddCommentNodeAction(URigVMCommentNode* InNode)
	: CommentText(InNode->GetCommentText())
	, Position(InNode->GetPosition())
	, Size(InNode->GetSize())
	, Color(InNode->GetNodeColor())
	, NodePath(InNode->GetNodePath())
{
}

bool FRigVMAddCommentNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddCommentNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMCommentNode* Node = InController->AddCommentNode(CommentText, Position, Size, Color, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddRerouteNodeAction::FRigVMAddRerouteNodeAction()
	: bShowAsFullNode(false)
	, CPPType()
	, CPPTypeObjectPath(NAME_None)
	, DefaultValue()
	, bIsConstant(false)
	, CustomWidgetName(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddRerouteNodeAction::FRigVMAddRerouteNodeAction(URigVMRerouteNode* InNode)
	: bShowAsFullNode(InNode->GetShowsAsFullNode())
	, CPPType(InNode->GetPins()[0]->GetCPPType())
	, CPPTypeObjectPath(NAME_None)
	, DefaultValue(InNode->GetPins()[0]->GetDefaultValue())
	, bIsConstant(InNode->GetPins()[0]->IsDefinedAsConstant())
	, CustomWidgetName(InNode->GetPins()[0]->GetCustomWidgetName())
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if (InNode->GetPins()[0]->GetCPPTypeObject())
	{
		CPPTypeObjectPath = *InNode->GetPins()[0]->GetCPPTypeObject()->GetPathName();
	}
}

bool FRigVMAddRerouteNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddRerouteNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMRerouteNode* Node = InController->AddFreeRerouteNode(bShowAsFullNode, CPPType, CPPTypeObjectPath, bIsConstant, CustomWidgetName, DefaultValue, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddBranchNodeAction::FRigVMAddBranchNodeAction()
	: Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddBranchNodeAction::FRigVMAddBranchNodeAction(URigVMBranchNode* InNode)
	: Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
}

bool FRigVMAddBranchNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddBranchNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMBranchNode* Node = InController->AddBranchNode(Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddIfNodeAction::FRigVMAddIfNodeAction()
	: CPPType()
	, CPPTypeObjectPath(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddIfNodeAction::FRigVMAddIfNodeAction(URigVMIfNode* InNode)
	: CPPType()
	, CPPTypeObjectPath(NAME_None)
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if(URigVMPin* ResultPin = InNode->FindPin(URigVMIfNode::ResultName))
	{
		CPPType = ResultPin->GetCPPType();
		if (ResultPin->GetCPPTypeObject())
		{
			CPPTypeObjectPath = *ResultPin->GetCPPTypeObject()->GetPathName();
		}
	}
}

bool FRigVMAddIfNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddIfNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMIfNode* Node = InController->AddIfNode(CPPType, CPPTypeObjectPath, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddSelectNodeAction::FRigVMAddSelectNodeAction()
	: CPPType()
	, CPPTypeObjectPath(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddSelectNodeAction::FRigVMAddSelectNodeAction(URigVMSelectNode* InNode)
	: CPPType()
	, CPPTypeObjectPath(NAME_None)
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if(URigVMPin* ResultPin = InNode->FindPin(URigVMSelectNode::ResultName))
	{
		CPPType = ResultPin->GetCPPType();
		if (ResultPin->GetCPPTypeObject())
		{
			CPPTypeObjectPath = *ResultPin->GetCPPTypeObject()->GetPathName();
		}
	}
}

bool FRigVMAddSelectNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddSelectNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMSelectNode* Node = InController->AddSelectNode(CPPType, CPPTypeObjectPath, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddEnumNodeAction::FRigVMAddEnumNodeAction()
	: CPPTypeObjectPath(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddEnumNodeAction::FRigVMAddEnumNodeAction(URigVMEnumNode* InNode)
	: CPPTypeObjectPath(NAME_None)
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	CPPTypeObjectPath = *InNode->GetCPPTypeObject()->GetPathName();
}

bool FRigVMAddEnumNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddEnumNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMEnumNode* Node = InController->AddEnumNode(CPPTypeObjectPath, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMAddPrototypeNodeAction::FRigVMAddPrototypeNodeAction()
	: PrototypeNotation(NAME_None)
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddPrototypeNodeAction::FRigVMAddPrototypeNodeAction(URigVMPrototypeNode* InNode)
	: PrototypeNotation(InNode->GetNotation())
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
}

bool FRigVMAddPrototypeNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddPrototypeNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMPrototypeNode* Node = InController->AddPrototypeNode(PrototypeNotation, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMInjectNodeIntoPinAction::FRigVMInjectNodeIntoPinAction()
	: PinPath()
	, bAsInput(false)
	, InputPinName(NAME_None)
	, OutputPinName(NAME_None)
	, NodePath()
{
}

FRigVMInjectNodeIntoPinAction::FRigVMInjectNodeIntoPinAction(URigVMInjectionInfo* InInjectionInfo)
	: PinPath(InInjectionInfo->GetPin()->GetPinPath())
	, bAsInput(InInjectionInfo->bInjectedAsInput)
	, NodePath(InInjectionInfo->Node->GetName())
{
	if (InInjectionInfo->InputPin)
	{
		InputPinName = InInjectionInfo->InputPin->GetFName();
	}
	if (InInjectionInfo->OutputPin)
	{
		OutputPinName = InInjectionInfo->OutputPin->GetFName();
	}
}

bool FRigVMInjectNodeIntoPinAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->EjectNodeFromPin(*PinPath, false) != nullptr;
}

bool FRigVMInjectNodeIntoPinAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMInjectionInfo* InjectionInfo = InController->InjectNodeIntoPin(PinPath, bAsInput, InputPinName, OutputPinName, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMRemoveNodeAction::FRigVMRemoveNodeAction(URigVMNode* InNode, URigVMController* InController)
{
	FRigVMInverseAction InverseAction;

	if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddUnitNodeAction(UnitNode));
		for (URigVMPin* Pin : UnitNode->GetPins())
		{
			if (Pin->GetDirection() == ERigVMPinDirection::Input ||
				Pin->GetDirection() == ERigVMPinDirection::Visible)
			{
				InverseAction.AddAction(FRigVMSetPinDefaultValueAction(Pin, Pin->GetDefaultValue()));
			}
		}
	}
	else if (URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddVariableNodeAction(VariableNode));
		URigVMPin* ValuePin = VariableNode->FindPin(TEXT("Value"));
		InverseAction.AddAction(FRigVMSetPinDefaultValueAction(ValuePin, ValuePin->GetDefaultValue()));
	}
	else if (URigVMParameterNode* ParameterNode = Cast<URigVMParameterNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddParameterNodeAction(ParameterNode));
		URigVMPin* ValuePin = ParameterNode->FindPin(TEXT("Value"));
		InverseAction.AddAction(FRigVMSetPinDefaultValueAction(ValuePin, ValuePin->GetDefaultValue()));
	}
	else if (URigVMCommentNode* CommentNode = Cast<URigVMCommentNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddCommentNodeAction(CommentNode));
	}
	else if (URigVMRerouteNode* RerouteNode = Cast<URigVMRerouteNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddRerouteNodeAction(RerouteNode));
	}
	else if (URigVMBranchNode* BranchNode = Cast<URigVMBranchNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddBranchNodeAction(BranchNode));
	}
	else if (URigVMIfNode* IfNode = Cast<URigVMIfNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddIfNodeAction(IfNode));
	}
	else if (URigVMSelectNode* SelectNode = Cast<URigVMSelectNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddSelectNodeAction(SelectNode));
	}
	else if (URigVMPrototypeNode* PrototypeNode = Cast<URigVMPrototypeNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddPrototypeNodeAction(PrototypeNode));
	}
	else if (URigVMEnumNode* EnumNode = Cast<URigVMEnumNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddEnumNodeAction(EnumNode));
	}
	else if (URigVMArrayNode* ArrayNode = Cast<URigVMArrayNode>(InNode))
	{
		InverseAction.AddAction(FRigVMAddArrayNodeAction(ArrayNode));
		for (URigVMPin* Pin : ArrayNode->GetPins())
		{
			if (Pin->GetDirection() == ERigVMPinDirection::Input ||
				Pin->GetDirection() == ERigVMPinDirection::Visible)
			{
				InverseAction.AddAction(FRigVMSetPinDefaultValueAction(Pin, Pin->GetDefaultValue()));
			}
		}
	}
	else if (URigVMLibraryNode* LibraryNode = Cast<URigVMLibraryNode>(InNode))
	{
		InverseAction.AddAction(FRigVMImportNodeFromTextAction(LibraryNode, InController));
	}
	else if (InNode->IsA<URigVMFunctionEntryNode>() || InNode->IsA<URigVMFunctionReturnNode>())
	{
		// Do nothing
	}
	else
	{
		ensure(false);
	}

	for (URigVMPin* Pin : InNode->GetPins())
	{
		if(Pin->IsExpanded() && Pin->GetSubPins().Num() > 0)
		{
			FRigVMSetPinExpansionAction ExpansionAction(Pin, true);
			ExpansionAction.OldIsExpanded = false;
			InverseAction.AddAction(ExpansionAction);
		}

		if (Pin->HasInjectedNodes())
		{
			if (URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Pin->GetInjectedNodes()[0]->Node))
			{
				FRigVMAddVariableNodeAction AddVariableNodeAction(VariableNode);
				FRigVMAddLinkAction AddLinkAction(VariableNode->GetValuePin(), Pin);
				FRigVMInjectNodeIntoPinAction InjectAction(Pin->GetInjectedNodes()[0]);
				InverseAction.AddAction(AddVariableNodeAction);
				InverseAction.AddAction(AddLinkAction);
				InverseAction.AddAction(InjectAction);
			}
		}
	}

	InverseActionKey.Set<FRigVMInverseAction>(InverseAction);
}

bool FRigVMRemoveNodeAction::Undo(URigVMController* InController)
{
	FRigVMActionWrapper InverseWrapper(InverseActionKey);
	if (!InverseWrapper.GetAction()->Undo(InController))
	{
		return false;
	}
	return FRigVMBaseAction::Undo(InController);
}

bool FRigVMRemoveNodeAction::Redo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Redo(InController))
	{
		return false;
	}
	FRigVMActionWrapper InverseWrapper(InverseActionKey);
	return InverseWrapper.GetAction()->Redo(InController);
}

FRigVMSetNodeSelectionAction::FRigVMSetNodeSelectionAction()
{

}

FRigVMSetNodeSelectionAction::FRigVMSetNodeSelectionAction(URigVMGraph* InGraph, TArray<FName> InNewSelection)
{
	OldSelection = InGraph->GetSelectNodes();
	NewSelection = InNewSelection;
}

bool FRigVMSetNodeSelectionAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeSelection(OldSelection, false);
}

bool FRigVMSetNodeSelectionAction::Redo(URigVMController* InController)
{
	if(!InController->SetNodeSelection(NewSelection, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodePositionAction::FRigVMSetNodePositionAction(URigVMNode* InNode, const FVector2D& InNewPosition)
: NodePath(InNode->GetNodePath())
, OldPosition(InNode->GetPosition())
, NewPosition(InNewPosition)
{
}

bool FRigVMSetNodePositionAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodePositionAction* Action = (const FRigVMSetNodePositionAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewPosition = Action->NewPosition;
	return true;
}

bool FRigVMSetNodePositionAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodePositionByName(*NodePath, OldPosition, false);
}

bool FRigVMSetNodePositionAction::Redo(URigVMController* InController)
{
	if(!InController->SetNodePositionByName(*NodePath, NewPosition, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodeSizeAction::FRigVMSetNodeSizeAction(URigVMNode* InNode, const FVector2D& InNewSize)
: NodePath(InNode->GetNodePath())
, OldSize(InNode->GetSize())
, NewSize(InNewSize)
{
}

bool FRigVMSetNodeSizeAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodeSizeAction* Action = (const FRigVMSetNodeSizeAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewSize = Action->NewSize;
	return true;
}

bool FRigVMSetNodeSizeAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeSizeByName(*NodePath, OldSize, false);
}

bool FRigVMSetNodeSizeAction::Redo(URigVMController* InController)
{
	if(!InController->SetNodeSizeByName(*NodePath, NewSize, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodeColorAction::FRigVMSetNodeColorAction(URigVMNode* InNode, const FLinearColor& InNewColor)
: NodePath(InNode->GetNodePath())
, OldColor(InNode->GetNodeColor())
, NewColor(InNewColor)
{
}

bool FRigVMSetNodeColorAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodeColorAction* Action = (const FRigVMSetNodeColorAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewColor = Action->NewColor;
	return true;
}

bool FRigVMSetNodeColorAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeColorByName(*NodePath, OldColor, false);
}

bool FRigVMSetNodeColorAction::Redo(URigVMController* InController)
{
	if(!InController->SetNodeColorByName(*NodePath, NewColor, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodeCategoryAction::FRigVMSetNodeCategoryAction(URigVMCollapseNode* InNode, const FString& InNewCategory)
	: NodePath(InNode->GetNodePath())
	, OldCategory(InNode->GetNodeCategory())
	, NewCategory(InNewCategory)
{
}

bool FRigVMSetNodeCategoryAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodeCategoryAction* Action = (const FRigVMSetNodeCategoryAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewCategory = Action->NewCategory;
	return true;
}

bool FRigVMSetNodeCategoryAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeCategoryByName(*NodePath, OldCategory, false);
}

bool FRigVMSetNodeCategoryAction::Redo(URigVMController* InController)
{
	if (!InController->SetNodeCategoryByName(*NodePath, NewCategory, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodeKeywordsAction::FRigVMSetNodeKeywordsAction(URigVMCollapseNode* InNode, const FString& InNewKeywords)
	: NodePath(InNode->GetNodePath())
	, OldKeywords(InNode->GetNodeKeywords())
	, NewKeywords(InNewKeywords)
{
}

bool FRigVMSetNodeKeywordsAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodeKeywordsAction* Action = (const FRigVMSetNodeKeywordsAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewKeywords = Action->NewKeywords;
	return true;
}

bool FRigVMSetNodeKeywordsAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeKeywordsByName(*NodePath, OldKeywords, false);
}

bool FRigVMSetNodeKeywordsAction::Redo(URigVMController* InController)
{
	if (!InController->SetNodeKeywordsByName(*NodePath, NewKeywords, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetNodeDescriptionAction::FRigVMSetNodeDescriptionAction(URigVMCollapseNode* InNode, const FString& InNewDescription)
	: NodePath(InNode->GetNodePath())
	, OldDescription(InNode->GetNodeDescription())
	, NewDescription(InNewDescription)
{
}

bool FRigVMSetNodeDescriptionAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetNodeDescriptionAction* Action = (const FRigVMSetNodeDescriptionAction*)Other;
	if (NodePath != Action->NodePath)
	{
		return false;
	}

	NewDescription = Action->NewDescription;
	return true;
}

bool FRigVMSetNodeDescriptionAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetNodeDescriptionByName(*NodePath, OldDescription, false);
}

bool FRigVMSetNodeDescriptionAction::Redo(URigVMController* InController)
{
	if (!InController->SetNodeDescriptionByName(*NodePath, NewDescription, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetCommentTextAction::FRigVMSetCommentTextAction()
: NodePath()
, OldText()
, NewText()
, OldFontSize(18)
, NewFontSize(18)
, bOldBubbleVisible(false)
, bNewBubbleVisible(false)
, bOldColorBubble(false)
, bNewColorBubble(false)
{
	
}


FRigVMSetCommentTextAction::FRigVMSetCommentTextAction(URigVMCommentNode* InNode, const FString& InNewText, const int32& InNewFontSize, const bool& bInNewBubbleVisible, const bool& bInNewColorBubble)
: NodePath(InNode->GetNodePath())
, OldText(InNode->GetCommentText())
, NewText(InNewText)
, OldFontSize(InNode->GetCommentFontSize())
, NewFontSize(InNewFontSize)
, bOldBubbleVisible(InNode->GetCommentBubbleVisible())
, bNewBubbleVisible(bInNewBubbleVisible)
, bOldColorBubble(InNode->GetCommentColorBubble())
, bNewColorBubble(bInNewColorBubble)
{
}

bool FRigVMSetCommentTextAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetCommentTextByName(*NodePath, OldText, OldFontSize, bOldBubbleVisible, bOldColorBubble, false);
}

bool FRigVMSetCommentTextAction::Redo(URigVMController* InController)
{
	if(!InController->SetCommentTextByName(*NodePath, NewText, NewFontSize, bNewBubbleVisible, bNewColorBubble, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetRerouteCompactnessAction::FRigVMSetRerouteCompactnessAction()
	: NodePath()
	, OldShowAsFullNode(false)
	, NewShowAsFullNode(false)
{

}
FRigVMSetRerouteCompactnessAction::FRigVMSetRerouteCompactnessAction(URigVMRerouteNode* InNode, bool InShowAsFullNode)
	: NodePath(InNode->GetNodePath())
	, OldShowAsFullNode(InNode->GetShowsAsFullNode())
	, NewShowAsFullNode(InShowAsFullNode)
{
}

bool FRigVMSetRerouteCompactnessAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetRerouteCompactnessByName(*NodePath, OldShowAsFullNode, false);
}

bool FRigVMSetRerouteCompactnessAction::Redo(URigVMController* InController)
{
	if(!InController->SetRerouteCompactnessByName(*NodePath, NewShowAsFullNode, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMRenameVariableAction::FRigVMRenameVariableAction(const FName& InOldVariableName, const FName& InNewVariableName)
: OldVariableName(InOldVariableName.ToString())
, NewVariableName(InNewVariableName.ToString())
{
}

bool FRigVMRenameVariableAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RenameVariable(*NewVariableName, *OldVariableName, false);
}

bool FRigVMRenameVariableAction::Redo(URigVMController* InController)
{
	if(!InController->RenameVariable(*OldVariableName, *NewVariableName, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMRenameParameterAction::FRigVMRenameParameterAction(const FName& InOldParameterName, const FName& InNewParameterName)
: OldParameterName(InOldParameterName.ToString())
, NewParameterName(InNewParameterName.ToString())
{
}

bool FRigVMRenameParameterAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RenameParameter(*NewParameterName, *OldParameterName, false);
}

bool FRigVMRenameParameterAction::Redo(URigVMController* InController)
{
	if(!InController->RenameParameter(*OldParameterName, *NewParameterName, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetPinExpansionAction::FRigVMSetPinExpansionAction(URigVMPin* InPin, bool bNewIsExpanded)
: PinPath(InPin->GetPinPath())
, OldIsExpanded(InPin->IsExpanded())
, NewIsExpanded(bNewIsExpanded)
{
}

bool FRigVMSetPinExpansionAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetPinExpansion(PinPath, OldIsExpanded, false);
}

bool FRigVMSetPinExpansionAction::Redo(URigVMController* InController)
{
	if(!InController->SetPinExpansion(PinPath, NewIsExpanded, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetPinWatchAction::FRigVMSetPinWatchAction(URigVMPin* InPin, bool bNewIsWatched)
: PinPath(InPin->GetPinPath())
, OldIsWatched(InPin->RequiresWatch())
, NewIsWatched(bNewIsWatched)
{
}

bool FRigVMSetPinWatchAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetPinIsWatched(PinPath, OldIsWatched, false);
}

bool FRigVMSetPinWatchAction::Redo(URigVMController* InController)
{
	if(!InController->SetPinIsWatched(PinPath, NewIsWatched, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetPinDefaultValueAction::FRigVMSetPinDefaultValueAction(URigVMPin* InPin, const FString& InNewDefaultValue)
: PinPath(InPin->GetPinPath())
, OldDefaultValue(InPin->GetDefaultValue())
, NewDefaultValue(InNewDefaultValue)
{
}

bool FRigVMSetPinDefaultValueAction::Merge(const FRigVMBaseAction* Other)
{
	if (!FRigVMBaseAction::Merge(Other))
	{
		return false;
	}

	const FRigVMSetPinDefaultValueAction* Action = (const FRigVMSetPinDefaultValueAction*)Other;
	if (PinPath != Action->PinPath)
	{
		return false;
	}

	NewDefaultValue = Action->NewDefaultValue;
	return true;
}

bool FRigVMSetPinDefaultValueAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	if (OldDefaultValue.IsEmpty())
	{
		return true;
	}
	return InController->SetPinDefaultValue(PinPath, OldDefaultValue, true, false);
}

bool FRigVMSetPinDefaultValueAction::Redo(URigVMController* InController)
{
	if (!NewDefaultValue.IsEmpty())
	{
		if (!InController->SetPinDefaultValue(PinPath, NewDefaultValue, true, false))
		{
			return false;
		}
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMInsertArrayPinAction::FRigVMInsertArrayPinAction(URigVMPin* InArrayPin, int32 InIndex, const FString& InNewDefaultValue)
: ArrayPinPath(InArrayPin->GetPinPath())
, Index(InIndex)
, NewDefaultValue(InNewDefaultValue)
{
}

bool FRigVMInsertArrayPinAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveArrayPin(FString::Printf(TEXT("%s.%d"), *ArrayPinPath, Index), false);
}

bool FRigVMInsertArrayPinAction::Redo(URigVMController* InController)
{
	if(InController->InsertArrayPin(ArrayPinPath, Index, NewDefaultValue, false).IsEmpty())
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMRemoveArrayPinAction::FRigVMRemoveArrayPinAction(URigVMPin* InArrayElementPin)
: ArrayPinPath(InArrayElementPin->GetParentPin()->GetPinPath())
, Index(InArrayElementPin->GetPinIndex())
, DefaultValue(InArrayElementPin->GetDefaultValue())
{
}

bool FRigVMRemoveArrayPinAction::Undo(URigVMController* InController)
{
	if (InController->InsertArrayPin(*ArrayPinPath, Index, DefaultValue, false).IsEmpty())
	{
		return false;
	}
	return FRigVMBaseAction::Undo(InController);
}

bool FRigVMRemoveArrayPinAction::Redo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Redo(InController))
	{
		return false;
	}
	return InController->RemoveArrayPin(FString::Printf(TEXT("%s.%d"), *ArrayPinPath, Index), false);
}

FRigVMAddLinkAction::FRigVMAddLinkAction(URigVMPin* InOutputPin, URigVMPin* InInputPin)
: OutputPinPath(InOutputPin->GetPinPath())
, InputPinPath(InInputPin->GetPinPath())
{
}

bool FRigVMAddLinkAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->BreakLink(OutputPinPath, InputPinPath, false);
}

bool FRigVMAddLinkAction::Redo(URigVMController* InController)
{
	if(!InController->AddLink(OutputPinPath, InputPinPath, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMBreakLinkAction::FRigVMBreakLinkAction(URigVMPin* InOutputPin, URigVMPin* InInputPin)
: OutputPinPath(InOutputPin->GetPinPath())
, InputPinPath(InInputPin->GetPinPath())
{
}

bool FRigVMBreakLinkAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->AddLink(OutputPinPath, InputPinPath, false);
}

bool FRigVMBreakLinkAction::Redo(URigVMController* InController)
{
	if(!InController->BreakLink(OutputPinPath, InputPinPath, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMChangePinTypeAction::FRigVMChangePinTypeAction()
: PinPath()
, OldCPPType()
, OldCPPTypeObjectPath(NAME_None)
, NewCPPType()
, NewCPPTypeObjectPath(NAME_None)
, bSetupOrphanPins(true)
, bBreakLinks(true)
, bRemoveSubPins(true)
{}

FRigVMChangePinTypeAction::FRigVMChangePinTypeAction(URigVMPin* InPin, const FString& InCppType, const FName& InCppTypeObjectPath, bool InSetupOrphanPins, bool InBreakLinks, bool InRemoveSubPins)
: PinPath(InPin->GetPinPath())
, OldCPPType(InPin->GetCPPType())
, OldCPPTypeObjectPath(NAME_None)
, NewCPPType(InCppType)
, NewCPPTypeObjectPath(InCppTypeObjectPath)
, bSetupOrphanPins(InSetupOrphanPins)
, bBreakLinks(InBreakLinks)
, bRemoveSubPins(InRemoveSubPins)
{
	if (UObject* CPPTypeObject = InPin->GetCPPTypeObject())
	{
		OldCPPTypeObjectPath = *CPPTypeObject->GetPathName();
	}
}

bool FRigVMChangePinTypeAction::Undo(URigVMController* InController)
{
	if(!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->ChangePinType(PinPath, OldCPPType, OldCPPTypeObjectPath, false, bSetupOrphanPins, bBreakLinks, bRemoveSubPins);
}

bool FRigVMChangePinTypeAction::Redo(URigVMController* InController)
{
	if(!InController->ChangePinType(PinPath, NewCPPType, NewCPPTypeObjectPath, false, bSetupOrphanPins, bBreakLinks, bRemoveSubPins))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMImportNodeFromTextAction::FRigVMImportNodeFromTextAction()
	: Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMImportNodeFromTextAction::FRigVMImportNodeFromTextAction(URigVMNode* InNode, URigVMController* InController)
	: Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
	, ExportedText()
{
	TArray<FName> NodeNamesToExport;
	NodeNamesToExport.Add(InNode->GetFName());
	ExportedText = InController->ExportNodesToText(NodeNamesToExport);
}

bool FRigVMImportNodeFromTextAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMImportNodeFromTextAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	TArray<FName> NodeNames = InController->ImportNodesFromText(ExportedText, false);
	if (NodeNames.Num() == 1)
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMCollapseNodesAction::FRigVMCollapseNodesAction()
	: LibraryNodePath()
{
}

FRigVMCollapseNodesAction::FRigVMCollapseNodesAction(URigVMController* InController, const TArray<URigVMNode*>& InNodes, const FString& InNodePath)
	: LibraryNodePath(InNodePath)
{
	TArray<FName> NodesToExport;
	for (URigVMNode* InNode : InNodes)
	{
		NodesToExport.Add(InNode->GetFName());
		CollapsedNodesPaths.Add(InNode->GetName());

		// find the links external to the nodes to be collapsed
		TArray<URigVMLink*> Links = InNode->GetLinks();
		for(URigVMLink* Link : Links)
		{
			if(InNodes.Contains(Link->GetSourcePin()->GetNode()) &&
				InNodes.Contains(Link->GetTargetPin()->GetNode()))
			{
				continue;
			}
			CollapsedNodesLinks.Add(Link->GetPinPathRepresentation());
		}
	}

	CollapsedNodesContent = InController->ExportNodesToText(NodesToExport);
}

bool FRigVMCollapseNodesAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}

	// remove the library node
	if(!InController->RemoveNodeByName(*LibraryNodePath, false, true, false))
	{
		return false;
	}

	const TArray<FName> RecoveredNodes = InController->ImportNodesFromText(CollapsedNodesContent, false, false);
	if(RecoveredNodes.Num() != CollapsedNodesPaths.Num())
	{
		return false;
	}

	for(const FString& CollapsedNodesLink : CollapsedNodesLinks)
	{
		FString Source, Target;
		if(URigVMLink::SplitPinPathRepresentation(CollapsedNodesLink, Source, Target))
		{
			InController->AddLink(Source, Target, false, false);
		}
	}

	return true;
}

bool FRigVMCollapseNodesAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	TArray<FName> NodeNames;
	for (const FString& NodePath : CollapsedNodesPaths)
	{
		NodeNames.Add(*NodePath);
	}

	URigVMLibraryNode* LibraryNode = InController->CollapseNodes(NodeNames, LibraryNodePath, false);
	if (LibraryNode)
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMExpandNodeAction::FRigVMExpandNodeAction()
	: LibraryNodePath()
{
}

FRigVMExpandNodeAction::FRigVMExpandNodeAction(URigVMController* InController, URigVMLibraryNode* InLibraryNode)
	: LibraryNodePath(InLibraryNode->GetName())
{
	TArray<FName> NodesToExport;
	NodesToExport.Add(InLibraryNode->GetFName());
	LibraryNodeContent = InController->ExportNodesToText(NodesToExport);

	TArray<URigVMLink*> Links = InLibraryNode->GetLinks();
	for(URigVMLink* Link : Links)
	{
		LibraryNodeLinks.Add(Link->GetPinPathRepresentation());
	}
}

bool FRigVMExpandNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}

	// remove the expanded nodes
	for (const FString& NodePath : ExpandedNodePaths)
	{
		if(!InController->RemoveNodeByName(*NodePath, false, true, false))
		{
			return false;
		}
	}

	const TArray<FName> RecoveredNodes = InController->ImportNodesFromText(LibraryNodeContent, false, false);
	if(RecoveredNodes.Num() != 1)
	{
		return false;
	}

	for(const FString& LibraryNodeLink : LibraryNodeLinks)
	{
		FString Source, Target;
		if(URigVMLink::SplitPinPathRepresentation(LibraryNodeLink, Source, Target))
		{
			InController->AddLink(Source, Target, false, false);
		}
	}

	return true;
}

bool FRigVMExpandNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	TArray<URigVMNode*> ExpandedNodes = InController->ExpandLibraryNode(*LibraryNodePath, false);
	if (ExpandedNodes.Num() == ExpandedNodePaths.Num())
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMRenameNodeAction::FRigVMRenameNodeAction(const FName& InOldNodeName, const FName& InNewNodeName)
	: OldNodeName(InOldNodeName.ToString())
	, NewNodeName(InNewNodeName.ToString())
{
}

bool FRigVMRenameNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	if (URigVMNode* Node = InController->GetGraph()->FindNode(NewNodeName))
	{
		return InController->RenameNode(Node, *OldNodeName, false);
	}
	return false;
}

bool FRigVMRenameNodeAction::Redo(URigVMController* InController)
{
	if (URigVMNode* Node = InController->GetGraph()->FindNode(OldNodeName))
	{
		return InController->RenameNode(Node, *NewNodeName, false);
	}
	else
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMPushGraphAction::FRigVMPushGraphAction(UObject* InGraph)
{
	GraphPath = TSoftObjectPtr<URigVMGraph>(Cast<URigVMGraph>(InGraph)).GetUniqueID();
}

bool FRigVMPushGraphAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->PopGraph(false) != nullptr;
}

bool FRigVMPushGraphAction::Redo(URigVMController* InController)
{
	TSoftObjectPtr<URigVMGraph> GraphPtr(GraphPath);
	if(URigVMGraph* Graph = GraphPtr.Get())
	{
		InController->PushGraph(Graph, false);
		return FRigVMBaseAction::Redo(InController);
	}
	return false;
}

FRigVMPopGraphAction::FRigVMPopGraphAction(UObject* InGraph)
{
	GraphPath = TSoftObjectPtr<URigVMGraph>(Cast<URigVMGraph>(InGraph)).GetUniqueID();
}

bool FRigVMPopGraphAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}

	TSoftObjectPtr<URigVMGraph> GraphPtr(GraphPath);
	if (URigVMGraph* Graph = GraphPtr.Get())
	{
		InController->PushGraph(Graph, false);
		return true;
	}
	return false;
}

bool FRigVMPopGraphAction::Redo(URigVMController* InController)
{
	if (InController->PopGraph(false) != nullptr)
	{
		return FRigVMBaseAction::Redo(InController);
	}
	return false;
}

FRigVMAddExposedPinAction::FRigVMAddExposedPinAction(URigVMPin* InPin)
	: PinName(InPin->GetName())
	, Direction(InPin->GetDirection())
	, CPPType(InPin->GetCPPType())
	, CPPTypeObjectPath()
	, DefaultValue(InPin->GetDefaultValue())
{
	if (UObject* CPPTypeObject = InPin->GetCPPTypeObject())
	{
		CPPTypeObjectPath = CPPTypeObject->GetPathName();
	}
}

bool FRigVMAddExposedPinAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveExposedPin(*PinName, false);
}

bool FRigVMAddExposedPinAction::Redo(URigVMController* InController)
{
	if (!InController->AddExposedPin(*PinName, Direction, CPPType, *CPPTypeObjectPath, DefaultValue, false).IsNone())
	{
		return FRigVMBaseAction::Redo(InController);
	}
	return false;
}

FRigVMRemoveExposedPinAction::FRigVMRemoveExposedPinAction(URigVMPin* InPin)
	: PinName(InPin->GetName())
	, Direction(InPin->GetDirection())
	, CPPType(InPin->GetCPPType())
	, CPPTypeObjectPath()
	, DefaultValue(InPin->GetDefaultValue())
{
	if (UObject* CPPTypeObject = InPin->GetCPPTypeObject())
	{
		CPPTypeObjectPath = CPPTypeObject->GetPathName();
	}
}

bool FRigVMRemoveExposedPinAction::Undo(URigVMController* InController)
{
	if(!InController->AddExposedPin(*PinName, Direction, CPPType, *CPPTypeObjectPath, DefaultValue, false).IsNone())
	{
		return FRigVMBaseAction::Undo(InController);
	}
	return false;
}

bool FRigVMRemoveExposedPinAction::Redo(URigVMController* InController)
{
	if(FRigVMBaseAction::Redo(InController))
	{
		return InController->RemoveExposedPin(*PinName, false);
	}
	return false;
}

FRigVMRenameExposedPinAction::FRigVMRenameExposedPinAction(const FName& InOldPinName, const FName& InNewPinName)
	: OldPinName(InOldPinName.ToString())
	, NewPinName(InNewPinName.ToString())
{
}

bool FRigVMRenameExposedPinAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RenameExposedPin(*NewPinName, *OldPinName, false);
}

bool FRigVMRenameExposedPinAction::Redo(URigVMController* InController)
{
	if(!InController->RenameExposedPin(*OldPinName, *NewPinName, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetPinIndexAction::FRigVMSetPinIndexAction()
	: PinPath()
	, OldIndex(INDEX_NONE)
	, NewIndex(INDEX_NONE)
{
}

FRigVMSetPinIndexAction::FRigVMSetPinIndexAction(URigVMPin* InPin, int32 InNewIndex)
	: PinPath(InPin->GetPinPath())
	, OldIndex(InPin->GetPinIndex())
	, NewIndex(InNewIndex)
{
}

bool FRigVMSetPinIndexAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetExposedPinIndex(*PinPath, OldIndex);
}

bool FRigVMSetPinIndexAction::Redo(URigVMController* InController)
{
	if (!InController->SetExposedPinIndex(*PinPath, NewIndex))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMSetRemappedVariableAction::FRigVMSetRemappedVariableAction(URigVMFunctionReferenceNode* InFunctionRefNode,
	const FName& InInnerVariableName, const FName& InOldOuterVariableName, const FName& InNewOuterVariableName)
	: NodePath()
	, InnerVariableName(InInnerVariableName)
	, OldOuterVariableName(InOldOuterVariableName)
	, NewOuterVariableName(InNewOuterVariableName)
{
	if(InFunctionRefNode)
	{
		NodePath = InFunctionRefNode->GetName();
	}
}

bool FRigVMSetRemappedVariableAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	if (URigVMFunctionReferenceNode* Node = Cast<URigVMFunctionReferenceNode>(InController->GetGraph()->FindNode(NodePath)))
	{
		return InController->SetRemappedVariable(Node, InnerVariableName, OldOuterVariableName, false);
	}
	return false;
}

bool FRigVMSetRemappedVariableAction::Redo(URigVMController* InController)
{
	if (URigVMFunctionReferenceNode* Node = Cast<URigVMFunctionReferenceNode>(InController->GetGraph()->FindNode(NodePath)))
	{
		return InController->SetRemappedVariable(Node, InnerVariableName, NewOuterVariableName, false);
	}
	else
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMAddLocalVariableAction::FRigVMAddLocalVariableAction(const FRigVMGraphVariableDescription& InLocalVariable)
	: LocalVariable(InLocalVariable)
{
	
}

bool FRigVMAddLocalVariableAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveLocalVariable(LocalVariable.Name, false);
}

bool FRigVMAddLocalVariableAction::Redo(URigVMController* InController)
{
	if (!LocalVariable.Name.IsNone())
	{
		return InController->AddLocalVariable(LocalVariable.Name, LocalVariable.CPPType, LocalVariable.CPPTypeObject, LocalVariable.DefaultValue, false).Name.IsNone() == false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMRemoveLocalVariableAction::FRigVMRemoveLocalVariableAction(const FRigVMGraphVariableDescription& InLocalVariable)
	: LocalVariable(InLocalVariable)
{
}

bool FRigVMRemoveLocalVariableAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return !InController->AddLocalVariable(LocalVariable.Name, LocalVariable.CPPType, LocalVariable.CPPTypeObject, LocalVariable.DefaultValue, false).Name.IsNone();
}

bool FRigVMRemoveLocalVariableAction::Redo(URigVMController* InController)
{
	if (!LocalVariable.Name.IsNone())
	{
		return InController->RemoveLocalVariable(LocalVariable.Name, false);
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMRenameLocalVariableAction::FRigVMRenameLocalVariableAction(const FName& InOldName, const FName& InNewName)
	: OldVariableName(InOldName), NewVariableName(InNewName)
{
	
}

bool FRigVMRenameLocalVariableAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RenameLocalVariable(NewVariableName, OldVariableName, false);
}

bool FRigVMRenameLocalVariableAction::Redo(URigVMController* InController)
{
	if (!InController->RenameLocalVariable(OldVariableName, NewVariableName, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMChangeLocalVariableTypeAction::FRigVMChangeLocalVariableTypeAction()
	: LocalVariable()
	, CPPType()
	, CPPTypeObject(nullptr)
{
}

FRigVMChangeLocalVariableTypeAction::FRigVMChangeLocalVariableTypeAction(
	const FRigVMGraphVariableDescription& InLocalVariable, const FString& InCPPType, UObject* InCPPTypeObject)
		: LocalVariable(InLocalVariable), CPPType(InCPPType), CPPTypeObject(InCPPTypeObject)
{
}

bool FRigVMChangeLocalVariableTypeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetLocalVariableType(LocalVariable.Name, LocalVariable.CPPType, LocalVariable.CPPTypeObject, false);
}

bool FRigVMChangeLocalVariableTypeAction::Redo(URigVMController* InController)
{
	if (!InController->SetLocalVariableType(LocalVariable.Name, CPPType, CPPTypeObject, false))
	{
		return false;
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMChangeLocalVariableDefaultValueAction::FRigVMChangeLocalVariableDefaultValueAction()
	: LocalVariable()
	, DefaultValue()
{
}

FRigVMChangeLocalVariableDefaultValueAction::FRigVMChangeLocalVariableDefaultValueAction(
	const FRigVMGraphVariableDescription& InLocalVariable, const FString& InDefaultValue)
		: LocalVariable(InLocalVariable), DefaultValue(InDefaultValue)
{
}

bool FRigVMChangeLocalVariableDefaultValueAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->SetLocalVariableDefaultValue(LocalVariable.Name, LocalVariable.DefaultValue, false);
}

bool FRigVMChangeLocalVariableDefaultValueAction::Redo(URigVMController* InController)
{
	if (!InController->SetLocalVariableDefaultValue(LocalVariable.Name, DefaultValue, false))
	{
		return false;		
	}
	return FRigVMBaseAction::Redo(InController);
}

FRigVMAddArrayNodeAction::FRigVMAddArrayNodeAction()
	: OpCode(ERigVMOpCode::Invalid)
	, CPPType()
	, CPPTypeObjectPath()
	, Position(FVector2D::ZeroVector)
	, NodePath()
{
}

FRigVMAddArrayNodeAction::FRigVMAddArrayNodeAction(URigVMArrayNode* InNode)
	: OpCode(InNode->GetOpCode())
	, CPPType(InNode->GetCPPType())
	, CPPTypeObjectPath()
	, Position(InNode->GetPosition())
	, NodePath(InNode->GetNodePath())
{
	if (InNode->GetCPPTypeObject())
	{
		CPPTypeObjectPath = InNode->GetCPPTypeObject()->GetPathName();
	}
}

bool FRigVMAddArrayNodeAction::Undo(URigVMController* InController)
{
	if (!FRigVMBaseAction::Undo(InController))
	{
		return false;
	}
	return InController->RemoveNodeByName(*NodePath, false);
}

bool FRigVMAddArrayNodeAction::Redo(URigVMController* InController)
{
#if WITH_EDITOR
	if (URigVMArrayNode* Node = InController->AddArrayNodeFromObjectPath(OpCode, CPPType, CPPTypeObjectPath, Position, NodePath, false))
	{
		return FRigVMBaseAction::Redo(InController);
	}
#endif
	return false;
}

FRigVMPromoteNodeAction::FRigVMPromoteNodeAction()
	: LibraryNodePath()
	, FunctionDefinitionPath()
	, bFromFunctionToCollapseNode(false)
{
}

FRigVMPromoteNodeAction::FRigVMPromoteNodeAction(const URigVMNode* InNodeToPromote, const FString& InNodePath, const FString& InFunctionDefinitionPath)
	: LibraryNodePath(InNodePath)
	, FunctionDefinitionPath(InFunctionDefinitionPath)
	, bFromFunctionToCollapseNode(InNodeToPromote->IsA<URigVMFunctionReferenceNode>())
{
}

bool FRigVMPromoteNodeAction::Undo(URigVMController* InController)
{
	if(bFromFunctionToCollapseNode)
	{
		const FName FunctionRefNodeName = InController->PromoteCollapseNodeToFunctionReferenceNode(*LibraryNodePath, false, false, FunctionDefinitionPath);
		return FunctionRefNodeName.ToString() == LibraryNodePath;
	}

	const FName CollapseNodeName = InController->PromoteFunctionReferenceNodeToCollapseNode(*LibraryNodePath, false, false, true);
	return CollapseNodeName.ToString() == LibraryNodePath;
}

bool FRigVMPromoteNodeAction::Redo(URigVMController* InController)
{
	if(bFromFunctionToCollapseNode)
	{
		const FName CollapseNodeName = InController->PromoteFunctionReferenceNodeToCollapseNode(*LibraryNodePath, false, false, false);
		return CollapseNodeName.ToString() == LibraryNodePath;
	}
	const FName FunctionRefNodeName = InController->PromoteCollapseNodeToFunctionReferenceNode(*LibraryNodePath, false, false);
	return FunctionRefNodeName.ToString() == LibraryNodePath;
}
