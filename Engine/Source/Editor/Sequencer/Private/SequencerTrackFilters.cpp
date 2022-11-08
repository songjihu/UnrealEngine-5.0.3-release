// Copyright Epic Games, Inc. All Rights Reserved.

#include "SequencerTrackFilters.h"
#include "EditorStyleSet.h"
#include "Engine/World.h"
#include "Framework/Commands/Commands.h"
#include "ISequencer.h"

#define LOCTEXT_NAMESPACE "Sequencer"

FSequencerTrackFilter_LevelFilter::~FSequencerTrackFilter_LevelFilter()
{
	if (CachedWorld.IsValid())
	{
		CachedWorld.Get()->OnLevelsChanged().RemoveAll(this);
		CachedWorld.Reset();
	}
}

bool FSequencerTrackFilter_LevelFilter::PassesFilter(FTrackFilterType InItem) const
{
	if (!InItem || !InItem->GetOutermost())
	{
		return false;
	}

	// For anything in a level, outermost should refer to the ULevel that contains it
	FString OutermostName = FPackageName::GetShortName(InItem->GetOutermost()->GetName());
	
	// Pass anything that is not in a hidden level
	return !HiddenLevels.Contains(OutermostName);
}

void FSequencerTrackFilter_LevelFilter::ResetFilter()
{
	HiddenLevels.Empty();

	BroadcastChangedEvent();
}

bool FSequencerTrackFilter_LevelFilter::IsLevelHidden(const FString& LevelName) const
{
	return HiddenLevels.Contains(LevelName);
}

void FSequencerTrackFilter_LevelFilter::HideLevel(const FString& LevelName)
{
	HiddenLevels.AddUnique(LevelName);

	BroadcastChangedEvent();
}

void FSequencerTrackFilter_LevelFilter::UnhideLevel(const FString& LevelName)
{
	HiddenLevels.Remove(LevelName);

	BroadcastChangedEvent();
}

void FSequencerTrackFilter_LevelFilter::UpdateWorld(UWorld* World)
{
	if (!CachedWorld.IsValid() || CachedWorld.Get() != World)
	{
		if (CachedWorld.IsValid())
		{
			CachedWorld.Get()->OnLevelsChanged().RemoveAll(this);
		}
		
		CachedWorld.Reset();
	
		if (IsValid(World))
		{
			CachedWorld = World;
			CachedWorld.Get()->OnLevelsChanged().AddRaw(this, &FSequencerTrackFilter_LevelFilter::HandleLevelsChanged);
		}

		HandleLevelsChanged();
	}
}

void FSequencerTrackFilter_LevelFilter::HandleLevelsChanged()
{
	if (!CachedWorld.IsValid())
	{
		ResetFilter();
		return;
	}

	const TArray<ULevel*>& WorldLevels = CachedWorld->GetLevels();
	
	if (WorldLevels.Num() < 2)
	{
		ResetFilter();
		return;
	}

	// Build a list of level names contained in the current world
	TArray<FString> WorldLevelNames;
	for (const ULevel* Level : WorldLevels)
	{
		if (IsValid(Level))
		{
			FString LevelName = FPackageName::GetShortName(Level->GetOutermost()->GetName());
			WorldLevelNames.Add(LevelName);
		}
	}

	// Rebuild our list of hidden level names to only include levels which are still in the world
	TArray<FString> OldHiddenLevels = HiddenLevels;
	HiddenLevels.Empty();
	for (FString LevelName : OldHiddenLevels)
	{
		if (WorldLevelNames.Contains(LevelName))
		{
			HiddenLevels.Add(LevelName);
		}
	}

	if (OldHiddenLevels.Num() != HiddenLevels.Num())
	{
		BroadcastChangedEvent();
	}
}

class FSequencerTrackFilter_AnimatedCommands
	: public TCommands<FSequencerTrackFilter_AnimatedCommands>
{
public:

	FSequencerTrackFilter_AnimatedCommands()
		: TCommands<FSequencerTrackFilter_AnimatedCommands>
	(
		"FSequencerTrackFilter_Animated",
		NSLOCTEXT("Contexts", "FSequencerTrackFilter_Animated", "FSequencerTrackFilter_Animated"),
		NAME_None,
		FEditorStyle::GetStyleSetName() // Icon Style Set
	)
	{ }
		
	/** Toggle the animated tracks filter */
	TSharedPtr< FUICommandInfo > ToggleAnimatedTracks;

	/** Initialize commands */
	virtual void RegisterCommands() override
	{
		UI_COMMAND(ToggleAnimatedTracks, "Animated Tracks", "Toggle the filter for Animated Tracks.", EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::U));
	}
};

FSequencerTrackFilter_Animated::FSequencerTrackFilter_Animated()
{
	FSequencerTrackFilter_AnimatedCommands::Register();	
}

FSequencerTrackFilter_Animated::~FSequencerTrackFilter_Animated()
{
	FSequencerTrackFilter_AnimatedCommands::Unregister();	
}

FText FSequencerTrackFilter_Animated::GetToolTipText() const 
{ 
	// When opening another sequence, FSequencer initializes the first sequence and then closes the previous sequence. 
	// This causes the track filter commands to be initialized for the first sequence and then destroyed when the subsequence 
	// sequence is opened. For now, register the commands before calling Get()
	if (!FSequencerTrackFilter_AnimatedCommands::IsRegistered())
	{
		FSequencerTrackFilter_AnimatedCommands::Register();	
	}

	const FSequencerTrackFilter_AnimatedCommands& Commands = FSequencerTrackFilter_AnimatedCommands::Get();

	const TSharedRef<const FInputChord> FirstActiveChord = Commands.ToggleAnimatedTracks->GetFirstValidChord();
	
	FText Tooltip = LOCTEXT("SequencerTrackFilter_AnimatedTip", "Show Only Animated Tracks."); 

	if (FirstActiveChord->IsValidChord())
	{
		return FText::Join(FText::FromString(TEXT(" ")), Tooltip, FirstActiveChord->GetInputText());
	}
	return Tooltip;
}

void FSequencerTrackFilter_Animated::BindCommands(TSharedRef<FUICommandList> CommandBindings, TWeakPtr<ISequencer> Sequencer)
{
	// See comment above
	if (!FSequencerTrackFilter_AnimatedCommands::IsRegistered())
	{
		FSequencerTrackFilter_AnimatedCommands::Register();	
	}

	const FSequencerTrackFilter_AnimatedCommands& Commands = FSequencerTrackFilter_AnimatedCommands::Get();

	CommandBindings->MapAction(
		Commands.ToggleAnimatedTracks,
		FExecuteAction::CreateLambda( [this, Sequencer]{ Sequencer.Pin()->SetTrackFilterEnabled(GetDisplayName(), !Sequencer.Pin()->IsTrackFilterEnabled(GetDisplayName())); } ),
		FCanExecuteAction::CreateLambda( [this, Sequencer]{ return true; } ),
		FIsActionChecked::CreateLambda( [this, Sequencer]{ return Sequencer.Pin()->IsTrackFilterEnabled(GetDisplayName()); } ) );
}

#undef LOCTEXT_NAMESPACE
