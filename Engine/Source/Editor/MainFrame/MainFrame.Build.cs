// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MainFrame : ModuleRules
{
	public MainFrame(ReadOnlyTargetRules Target) : base(Target)
	{

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Documentation",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"Engine",
				"EngineSettings",
				"InputCore",
				"RHI",
				"RenderCore",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"SourceControl",
				"SourceControlWindows",
				"TargetPlatform",
				"DesktopPlatform",
				"EditorFramework",
				"UnrealEd",
				"WorkspaceMenuStructure",
				"MessageLog",
				"UATHelper",
				"TranslationEditor",
				"Projects",
				"DeviceProfileEditor",
				"UndoHistory",
				"Analytics",
				"ToolMenus",
				"LauncherServices",
				"InterchangeCore",
				"InterchangeEngine",
				"ToolWidgets",
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
				"AssetTools",
				"DerivedDataCache",
				"DesktopPlatform",
				"LauncherPlatform",
				"GameProjectGeneration",
				"ProjectTargetPlatformEditor",
				"LevelEditor",
				"Settings",
				"SourceCodeAccess",
				"LocalizationDashboard", // not required but causes circular depends issues on Linux/Mac
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				"Editor/MainFrame/Private",
				"Editor/MainFrame/Private/Frame",
				"Editor/MainFrame/Private/Menus",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[] {
				"AssetTools",
				"GameProjectGeneration",
				"ProjectTargetPlatformEditor",
				"LevelEditor",
				"SourceCodeAccess",
				"HotReload",
				"LocalizationDashboard", // not required but causes circular depends issues on Linux/Mac
			}
		);
	}
}
