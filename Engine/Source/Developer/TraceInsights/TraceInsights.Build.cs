// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TraceInsights : ModuleRules
{
	public TraceInsights(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePaths.AddRange
		(
			new string[] {
				"Developer/TraceInsights/Private",
			}
		);

		PublicDependencyModuleNames.AddRange
		(
			new string[] {
				"AppFramework", // for SColorPicker
				"ApplicationCore",
				"Cbor",
				"Core",
				"CoreUObject",
				"DesktopPlatform",
				"EditorStyle",
				"InputCore",
				"RenderCore",
				"RHI",
				"Slate",
				"Sockets",
				"SourceCodeAccess",
				"TraceAnalysis",
				"TraceLog",
				"TraceServices",
				"WorkspaceMenuStructure",
				"XmlParser",
			}
		);

		//Modules required for running automation in stand alone Insights
		if (Target.Configuration != UnrealTargetConfiguration.Shipping && !Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"AutomationWorker",
					"AutomationController",
					"AutomationWindow",
					"SessionServices",
				}
			);
		}

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"Engine",
				}
			);
		}

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"SlateCore",
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
				"Messaging",
			}
		);
	}
}
