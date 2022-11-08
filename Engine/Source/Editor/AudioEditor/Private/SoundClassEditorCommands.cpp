// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoundClassEditorCommands.h"

#define LOCTEXT_NAMESPACE "SoundClassEditorCommands"

void FSoundClassEditorCommands::RegisterCommands()
{
	UI_COMMAND(ToggleSolo, "Solo", "Toggles Soloing this sound class", EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::S));
	UI_COMMAND(ToggleMute, "Mute", "Toggles Muting this sound class", EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::M));
}

#undef LOCTEXT_NAMESPACE
