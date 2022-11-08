// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Map.h"
#include "UObject/NameTypes.h"
#include "Templates/SharedPointer.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Commands/Commands.h"

class FEditorViewportClient;

class COMMONMENUEXTENSIONS_API FLumenVisualizationMenuCommands : public TCommands<FLumenVisualizationMenuCommands>
{
public:
	enum class FLumenVisualizationType : uint8
	{
		Overview,
		Standard
	};

	struct FLumenVisualizationRecord
	{
		FName Name;
		TSharedPtr<FUICommandInfo> Command;
		FLumenVisualizationType Type;

		FLumenVisualizationRecord()
		: Name()
		, Command()
		, Type(FLumenVisualizationType::Overview)
		{
		}
	};

	typedef TMultiMap<FName, FLumenVisualizationRecord> TLumenVisualizationModeCommandMap;
	typedef TLumenVisualizationModeCommandMap::TConstIterator TCommandConstIterator;

	FLumenVisualizationMenuCommands();

	TCommandConstIterator CreateCommandConstIterator() const;

	static void BuildVisualisationSubMenu(FMenuBuilder& Menu);

	virtual void RegisterCommands() override;

	void BindCommands(FUICommandList& CommandList, const TSharedPtr<FEditorViewportClient>& Client) const;
	
	inline bool IsPopulated() const
	{
		return CommandMap.Num() > 0;
	}

private:
	void BuildCommandMap();
	bool AddCommandTypeToMenu(FMenuBuilder& Menu, const FLumenVisualizationType Type) const;

	static void ChangeLumenVisualizationMode(const TSharedPtr<FEditorViewportClient>& Client, FName InName);
	static bool IsLumenVisualizationModeSelected(const TSharedPtr<FEditorViewportClient>& Client, FName InName);

private:
	TLumenVisualizationModeCommandMap CommandMap;
};