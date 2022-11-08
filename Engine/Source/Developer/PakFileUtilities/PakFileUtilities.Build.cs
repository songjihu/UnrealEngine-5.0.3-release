// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PakFileUtilities : ModuleRules
{
	public PakFileUtilities(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(new string[] {
            "Core",
            "PakFile",
            "Json",
            "Projects",
            "RSA",
            "IoStoreUtilities",
        });

        PrivateIncludePathModuleNames.AddRange(new string[] {
            "DerivedDataCache",
            "Json",
        });

		DynamicallyLoadedModuleNames.Add("DerivedDataCache");
		
	}
}
