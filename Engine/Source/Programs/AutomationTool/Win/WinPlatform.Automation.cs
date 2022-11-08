// Copyright Epic Games, Inc. All Rights Reserved.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.IO;
using System.Text.RegularExpressions;
using AutomationTool;
using UnrealBuildTool;
using Microsoft.Win32;
using System.Diagnostics;
using EpicGames.Core;
using UnrealBuildBase;

public static class SteamDeckSupport
{
	public static string RSyncPath = Path.Combine(Unreal.RootDirectory.FullName, "Engine\\Extras\\ThirdPartyNotUE\\cwrsync\\bin\\rsync.exe");
	public static string SSHPath   = Path.Combine(Unreal.RootDirectory.FullName, "Engine\\Extras\\ThirdPartyNotUE\\cwrsync\\bin\\ssh.exe");

	public static string GetRegisterGameScript(string GameId, string GameExePath, string GameFolderPath, string GameRunArgs)
	{
		// create a script that will be copied over to the SteamDeck and ran to register the game
		// TODO make these easier to customize, vs hard coding the settings. Assume debugging for now, requires the user to have uploaded the required msvsmom/remote debugging stuff
		// which is done through uploading any game with debugging enabled through the SteamOS Devkit Client
		string GameIdArgs    = String.Format("\"gameid\":\"{0}\"", GameId);
		string DirectoryArgs = String.Format("\"directory\":\"{0}\"", GameFolderPath);
		string ArgvArgs      = String.Format("\"argv\":[\"{0} {1}\"]", GameExePath, GameRunArgs);
		string SettingsArgs  = String.Format("\"settings\": {{\"steam_play\": \"1\", \"steam_play_debug\": \"1\", \"steam_play_debug_version\": \"2019\"}}");

		return String.Format("#!/bin/bash\npython3 ~/devkit-utils/steam-client-create-shortcut --parms '{{{0}, {1}, {2}, {3}}}'", GameIdArgs, DirectoryArgs, ArgvArgs, SettingsArgs).Replace("\r\n", "\n");
	}

	// This is a bit nasty, due to rsync needing to use cygdrive path for its local location over Windows paths.
	// This will not work with UNC paths
	public static string ConvertWindowsPathToCygdrive(string WindowsPath)
	{
		string CygdrivePath = "";

		if (!string.IsNullOrEmpty(WindowsPath))
		{
			string FullPath = Path.GetFullPath(WindowsPath);
			string RootPath = Path.GetPathRoot(FullPath);

			System.Console.WriteLine("{0}", RootPath);
			CygdrivePath = Path.Combine("/cygdrive", Char.ToLower(FullPath[0]).ToString(), FullPath.Substring(RootPath.Length));

			return CygdrivePath.Replace('\\','/');
		}

		return CygdrivePath;
	}

	public static List<DeviceInfo> GetDevices()
	{
		List<DeviceInfo> Devices = new List<DeviceInfo>();

		// Look for any Steam Deck devices that are in the Engine ini files. If so lets add them as valid devices
		// This will be required for matching devices passed into the BuildCookRun command
		ConfigHierarchy EngineConfig = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, null, UnrealTargetPlatform.Win64);

		List<string> SteamDeckDevices;
		if (EngineConfig.GetArray("/Script/WindowsTargetPlatform.WindowsTargetSettings", "SteamDeckDevice", out SteamDeckDevices))
		{
			// Expected ini format: +SteamDeckDevice=(IpAddr=10.1.33.19,Name=MySteamDeck,UserName=deck)
			foreach (string DeckDevice in SteamDeckDevices)
			{
				string IpAddr     = GetStructEntry(DeckDevice, "IpAddr", false);
				string DeviceName = GetStructEntry(DeckDevice, "Name", false);
				// Skipping the UserName as its unused here

				// Name is optional, if its empty/not found lets just use the IpAddr for the Name
				if (string.IsNullOrEmpty(DeviceName))
				{
					DeviceName = IpAddr;
				}

				if (!string.IsNullOrEmpty(IpAddr))
				{
					// TODO Fix the usage of OSVersion here. We are abusing this and using MS OSVersion to allow Turnkey to be happy
					DeviceInfo SteamDeck = new DeviceInfo(UnrealTargetPlatform.Win64, DeviceName, IpAddr,
						Environment.OSVersion.Version.ToString(), "SteamDeck", true, true);

					Devices.Add(SteamDeck);
				}
			}
		}

		return Devices;
	}

	// GetStructEntry copied from ExecuteBuild.cs TODO move to a better to share this
	private static string GetStructEntry(string Input, string Property, bool bIsArrayProperty)
	{
		string PrimaryRegex;
		string AltRegex = null;
		if (bIsArrayProperty)
		{
			PrimaryRegex = string.Format("{0}\\s*=\\s*\\((.*?)\\)", Property);
		}
		else
		{
			// handle quoted strings, allowing for escaped quotation marks (basically doing " followed by whatever, until we see a quote that was not proceeded by a \, and gather the whole mess in an outer group)
			PrimaryRegex = string.Format("{0}\\s*=\\s*\"((.*?)[^\\\\])\"", Property);
			// when no quotes, we skip over whitespace, and we end when we see whitespace, a comma or a ). This will handle (Ip = 192.168.0.1 , Name=....) , and return only '192.168.0.1'
			AltRegex = string.Format("{0}\\s*=\\s*(.*?)[\\s,\\)]", Property);
		}

		// attempt to match it!
		Match Result = Regex.Match(Input, PrimaryRegex);
		if (!Result.Success && AltRegex != null)
		{
			Result = Regex.Match(Input, AltRegex);
		}

		// if we got a success, return the main match value
		if (Result.Success)
		{
			return Result.Groups[1].Value.ToString();
		}

		return null;
	}

	/* Deploying to a steam deck currently does 2 things
	 *
	 * 1) Generates a script CreateShortcutHelper.sh that will register the game on the SteamDeck once uploaded
	 * 2) Uploads the build using rsync to the devkit-game location. Once uploaded it runs the CreateShortcutHelper.sh generated before.
	 */
	public static void Deploy(ProjectParams Params, DeploymentContext SC)
	{
		string DevKitRSAPath  = Path.Combine(CommandUtils.GetEnvVar("LOCALAPPDATA"), "steamos-devkit\\steamos-devkit\\devkit_rsa");
		string SSHArgs        = String.Format("-i {0} {1}@{2}", DevKitRSAPath, Params.DeviceUsername, Params.DeviceNames[0]);
		string GameFolderPath = Path.Combine("/home", Params.DeviceUsername, "devkit-game", Params.ShortProjectName).Replace('\\', '/');
		string GameRunArgs    = String.Format("{0} {1} {2}", SC.ProjectArgForCommandLines, Params.StageCommandline, Params.RunCommandline).Replace("\"", "\\\"");

		FileReference ExePath = Params.GetProjectExeForPlatform(UnrealTargetPlatform.Win64);
		string RelGameExePath = ExePath.MakeRelativeTo(DirectoryReference.Combine(ExePath.Directory, "../../..")).Replace('\\', '/');

		if (!File.Exists(DevKitRSAPath))
		{
			CommandUtils.LogWarning("Unable to find '{0}' rsa key needed to deploy to the steam deck. Make sure you've installed the SteamOS Devkit client", DevKitRSAPath);
			return;
		}

		string ScriptFileName = "CreateShortcutHelper.sh";
		string ScriptFile = Path.Combine(SC.StageDirectory.FullName, ScriptFileName);
		File.WriteAllText(ScriptFile, SteamDeckSupport.GetRegisterGameScript(Params.ShortProjectName, RelGameExePath, GameFolderPath, GameRunArgs));

		// Exclude removing the Saved folders to preserve logs and crash data. Though note these will keep filling up with data
		IProcessResult Result = CommandUtils.Run(SteamDeckSupport.RSyncPath,
			String.Format("-avh --delete --exclude=\"Saved/\" --rsync-path=\"mkdir -p {5} && rsync\" --chmod=Du=rwx,Dgo=rx,Fu=rwx,Fog=rx -e \"{0} -o StrictHostKeyChecking=no -i '{1}'\" --update \"{2}/\" {3}@{4}:{5}",
			SteamDeckSupport.SSHPath, DevKitRSAPath, SteamDeckSupport.ConvertWindowsPathToCygdrive(SC.StageDirectory.FullName), Params.DeviceUsername, Params.DeviceNames[0], GameFolderPath), "");

		if (Result.ExitCode > 0)
		{
			CommandUtils.LogWarning("Failed to rsync files to the SteamDeck. Check connection on ip {0}@{1}", Params.DeviceUsername, Params.DeviceNames[0]);
			return;
		}

		// Run the script to register the game with the Deck
		Result = CommandUtils.Run(SteamDeckSupport.SSHPath, String.Format("{0} \"chmod +x {1}/{2} && {1}/{2}\"", SSHArgs, GameFolderPath, ScriptFileName), "");

		if (Result.ExitCode > 0)
		{
			CommandUtils.LogWarning("Failed to run the {0}.sh script. Check connection on ip {1}@{2}", ScriptFileName, Params.DeviceUsername, Params.DeviceNames[0]);
			return;
		}
	}
}

public class Win64Platform : Platform
{
	public Win64Platform()
		: base(UnrealTargetPlatform.Win64)
	{
	}

	public override DeviceInfo[] GetDevices()
	{
		List<DeviceInfo> Devices = new List<DeviceInfo>();

		if (HostPlatform.Current.HostEditorPlatform == UnrealTargetPlatform.Win64)
		{
			DeviceInfo LocalMachine = new DeviceInfo(UnrealTargetPlatform.Win64, Environment.MachineName, Environment.MachineName,
				Environment.OSVersion.Version.ToString(), "Computer", true, true);

			Devices.Add(LocalMachine);

			Devices.AddRange(SteamDeckSupport.GetDevices());
		}

		return Devices.ToArray();
	}

	public override void Deploy(ProjectParams Params, DeploymentContext SC)
	{
		// We only care about deploying for SteamDeck
		if (Params.Devices.Count == 1 && GetDevices().FirstOrDefault(x => x.Id == Params.DeviceNames[0])?.Type == "SteamDeck")
		{
			SteamDeckSupport.Deploy(Params, SC);
		}
	}

	public override IProcessResult RunClient(ERunOptions ClientRunFlags, string ClientApp, string ClientCmdLine, ProjectParams Params)
	{
		if (Params.Devices.Count == 1 && GetDevices().FirstOrDefault(x => x.Id == Params.DeviceNames[0])?.Type == "SteamDeck")
		{
			// TODO figure out how to get the steam app num id. Then can run like this:
			// steam steam://rungameid/<GameNumId>
			// TODO would be great if we could tail the log while running. Figure out how to cancel/exit app
			return null;
		}

		return base.RunClient(ClientRunFlags, ClientApp, ClientCmdLine, Params);
	}

	protected override string GetPlatformExeExtension()
	{
		return ".exe";
	}

	public override bool IsSupported { get { return true; } }

	public override void GetFilesToDeployOrStage(ProjectParams Params, DeploymentContext SC)
	{
		// Engine non-ufs (binaries)

		if (SC.bStageCrashReporter)
		{
			FileReference ReceiptFileName = TargetReceipt.GetDefaultPath(Unreal.EngineDirectory, "CrashReportClient", SC.StageTargetPlatform.PlatformType, UnrealTargetConfiguration.Shipping, null);
			if(FileReference.Exists(ReceiptFileName))
			{
				TargetReceipt Receipt = TargetReceipt.Read(ReceiptFileName);
				SC.StageBuildProductsFromReceipt(Receipt, true, false);
			}
		}

		// Stage all the build products
		foreach(StageTarget Target in SC.StageTargets)
		{
			SC.StageBuildProductsFromReceipt(Target.Receipt, Target.RequireFilesExist, Params.bTreatNonShippingBinariesAsDebugFiles);
		}

		// Copy the splash screen, windows specific
		FileReference SplashImage = FileReference.Combine(SC.ProjectRoot, "Content", "Splash", "Splash.bmp");
		if(FileReference.Exists(SplashImage))
		{
			SC.StageFile(StagedFileType.NonUFS, SplashImage);
		}

		// Stage cloud metadata
		DirectoryReference ProjectCloudPath = DirectoryReference.Combine(SC.ProjectRoot, "Platforms/Windows/Build/Cloud");
		if (DirectoryReference.Exists(ProjectCloudPath))
		{
			SC.StageFiles(StagedFileType.SystemNonUFS, ProjectCloudPath, StageFilesSearch.AllDirectories, new StagedDirectoryReference("Cloud"));
		}
		else
		{
			CommandUtils.LogLog("Can't find cloud directory {0}", ProjectCloudPath.FullName);
		}

		// Stage the bootstrap executable
		if (!Params.NoBootstrapExe)
		{
			foreach(StageTarget Target in SC.StageTargets)
			{
				BuildProduct Executable = Target.Receipt.BuildProducts.FirstOrDefault(x => x.Type == BuildProductType.Executable);
				if(Executable != null)
				{
					// only create bootstraps for executables
					List<StagedFileReference> StagedFiles = SC.FilesToStage.NonUFSFiles.Where(x => x.Value == Executable.Path).Select(x => x.Key).ToList();
					if (StagedFiles.Count > 0 && Executable.Path.HasExtension(".exe"))
					{
						string BootstrapArguments = "";
						if (!ShouldStageCommandLine(Params, SC))
						{
							if (!SC.IsCodeBasedProject)
							{
								BootstrapArguments = String.Format("..\\..\\..\\{0}\\{0}.uproject", SC.ShortProjectName);
							}
							else
							{
								BootstrapArguments = SC.ShortProjectName;
							}
						}

						string BootstrapExeName;
						if(SC.StageTargetConfigurations.Count > 1)
						{
							BootstrapExeName = Executable.Path.GetFileName();
						}
						else if(Params.IsCodeBasedProject)
						{
							BootstrapExeName = Target.Receipt.TargetName + ".exe";
						}
						else
						{
							BootstrapExeName = SC.ShortProjectName + ".exe";
						}

						foreach (StagedFileReference StagePath in StagedFiles)
						{
							StageBootstrapExecutable(SC, BootstrapExeName, Executable.Path, StagePath, BootstrapArguments);
						}
					}
				}
			}
		}

		if (Params.Prereqs)
		{
			SC.StageFile(StagedFileType.NonUFS, FileReference.Combine(SC.EngineRoot, "Extras", "Redist", "en-us", "UEPrereqSetup_x64.exe"));
		}

		if (!string.IsNullOrWhiteSpace(Params.AppLocalDirectory))
		{
			StageAppLocalDependencies(Params, SC, "Win64");
		}
	}

	public override void ExtractPackage(ProjectParams Params, string SourcePath, string DestinationPath)
    {
    }

	public override void GetTargetFile(string RemoteFilePath, string LocalFile, ProjectParams Params)
	{
		var SourceFile = FileReference.Combine(new DirectoryReference(Params.BaseStageDirectory), GetCookPlatform(Params.HasServerCookedTargets, Params.HasClientTargetDetected), RemoteFilePath);
		CommandUtils.CopyFile(SourceFile.FullName, LocalFile);
	}

	void StageBootstrapExecutable(DeploymentContext SC, string ExeName, FileReference TargetFile, StagedFileReference StagedRelativeTargetPath, string StagedArguments)
	{
		FileReference InputFile = FileReference.Combine(SC.LocalRoot, "Engine", "Binaries", SC.PlatformDir, String.Format("BootstrapPackagedGame-{0}-Shipping.exe", SC.PlatformDir));
		if(FileReference.Exists(InputFile))
		{
			// Create the new bootstrap program
			DirectoryReference IntermediateDir = DirectoryReference.Combine(SC.ProjectRoot, "Intermediate", "Staging");
			DirectoryReference.CreateDirectory(IntermediateDir);

			FileReference IntermediateFile = FileReference.Combine(IntermediateDir, ExeName);
			CommandUtils.CopyFile(InputFile.FullName, IntermediateFile.FullName);
			CommandUtils.SetFileAttributes(IntermediateFile.FullName, ReadOnly: false);
	
			// currently the icon updating doesn't run under mono
			if (UnrealBuildTool.BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64)
			{
				// Get the icon from the build directory if possible
				GroupIconResource GroupIcon = null;
				if(FileReference.Exists(FileReference.Combine(SC.ProjectRoot, "Build/Windows/Application.ico")))
				{
					GroupIcon = GroupIconResource.FromIco(FileReference.Combine(SC.ProjectRoot, "Build/Windows/Application.ico").FullName);
				}
				if(GroupIcon == null)
				{
					GroupIcon = GroupIconResource.FromExe(TargetFile.FullName);
				}

				// Update the resources in the new file
				using(ModuleResourceUpdate Update = new ModuleResourceUpdate(IntermediateFile.FullName, false))
				{
					const int IconResourceId = 101;
					if(GroupIcon != null) Update.SetIcons(IconResourceId, GroupIcon);

					const int ExecFileResourceId = 201;
					Update.SetData(ExecFileResourceId, ResourceType.RawData, Encoding.Unicode.GetBytes(StagedRelativeTargetPath.ToString().Replace('/', '\\') + "\0"));

					const int ExecArgsResourceId = 202;
					Update.SetData(ExecArgsResourceId, ResourceType.RawData, Encoding.Unicode.GetBytes(StagedArguments + "\0"));
				}
			}

			// Copy it to the staging directory
			SC.StageFile(StagedFileType.SystemNonUFS, IntermediateFile, new StagedFileReference(ExeName));
		}
	}

	public override string GetCookPlatform(bool bDedicatedServer, bool bIsClientOnly)
	{
		const string NoEditorCookPlatform = "Windows";
		const string ServerCookPlatform = "WindowsServer";
		const string ClientCookPlatform = "WindowsClient";

		if (bDedicatedServer)
		{
			return ServerCookPlatform;
		}
		else if (bIsClientOnly)
		{
			return ClientCookPlatform;
		}
		else
		{
			return NoEditorCookPlatform;
		}
	}

	public override string GetEditorCookPlatform()
	{
		return "WindowsEditor";
	}
	
	public override string GetPlatformPakCommandLine(ProjectParams Params, DeploymentContext SC)
	{
		string PakParams = " -patchpaddingalign=2048";
		if (!SC.DedicatedServer)
		{
			string OodleDllPath = DirectoryReference.Combine(SC.ProjectRoot, "Binaries/ThirdParty/Oodle/Win64/UnrealPakPlugin.dll").FullName;
			if (File.Exists(OodleDllPath))
			{
				PakParams += String.Format(" -customcompressor=\"{0}\"", OodleDllPath);
			}
		}
		return PakParams;
	}

	public override void Package(ProjectParams Params, DeploymentContext SC, int WorkingCL)
	{
		// If this is a content-only project and there's a custom icon, update the executable
		if (!Params.HasDLCName && !Params.IsCodeBasedProject)
		{
			FileReference IconFile = FileReference.Combine(Params.RawProjectPath.Directory, "Build", "Windows", "Application.ico");
			if(FileReference.Exists(IconFile))
			{
				CommandUtils.LogInformation("Updating executable with custom icon from {0}", IconFile);

				GroupIconResource GroupIcon = GroupIconResource.FromIco(IconFile.FullName);

				List<FileReference> ExecutablePaths = GetExecutableNames(SC);
				foreach (FileReference ExecutablePath in ExecutablePaths)
				{
					using (ModuleResourceUpdate Update = new ModuleResourceUpdate(ExecutablePath.FullName, false))
					{
						const int IconResourceId = 123; // As defined in Engine\Source\Runtime\Launch\Resources\Windows\resource.h
						if (GroupIcon != null)
						{
							Update.SetIcons(IconResourceId, GroupIcon);
						}
					}
				}
			}
		}

		PrintRunTime();
	}

	public override bool UseAbsLog
	{
		get { return BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64; }
	}

	public override bool CanHostPlatform(UnrealTargetPlatform Platform)
	{
		if (Platform == UnrealTargetPlatform.Mac)
		{
			return false;
		}
		return true;
	}

    public override bool ShouldStageCommandLine(ProjectParams Params, DeploymentContext SC)
	{
		return false; // !String.IsNullOrEmpty(Params.StageCommandline) || !String.IsNullOrEmpty(Params.RunCommandline) || (!Params.IsCodeBasedProject && Params.NoBootstrapExe);
	}

	public override List<string> GetDebugFileExtensions()
	{
		return new List<string> { ".pdb", ".map" };
	}

	public override bool SignExecutables(DeploymentContext SC, ProjectParams Params)
	{
		// Sign everything we built
		List<FileReference> FilesToSign = GetExecutableNames(SC);
		CodeSign.SignMultipleFilesIfEXEOrDLL(FilesToSign);

		return true;
	}

	public void StageAppLocalDependencies(ProjectParams Params, DeploymentContext SC, string PlatformDir)
	{
		Dictionary<string, string> PathVariables = new Dictionary<string, string>();
		PathVariables["EngineDir"] = SC.EngineRoot.FullName;
		PathVariables["ProjectDir"] = SC.ProjectRoot.FullName;

		// support multiple comma-separated paths
		string[] AppLocalDirectories = Params.AppLocalDirectory.Split(';');
		foreach (string AppLocalDirectory in AppLocalDirectories)
		{
			string ExpandedAppLocalDir = Utils.ExpandVariables(AppLocalDirectory, PathVariables);

			DirectoryReference BaseAppLocalDependenciesPath = Path.IsPathRooted(ExpandedAppLocalDir) ? new DirectoryReference(CombinePaths(ExpandedAppLocalDir, PlatformDir)) : DirectoryReference.Combine(SC.ProjectRoot, ExpandedAppLocalDir, PlatformDir);
			if (DirectoryReference.Exists(BaseAppLocalDependenciesPath))
			{
				StageAppLocalDependenciesToDir(SC, BaseAppLocalDependenciesPath, StagedDirectoryReference.Combine("Engine", "Binaries", PlatformDir));
				StageAppLocalDependenciesToDir(SC, BaseAppLocalDependenciesPath, StagedDirectoryReference.Combine(SC.RelativeProjectRootForStage, "Binaries", PlatformDir));
			}
			else
			{
				LogWarning("Unable to deploy AppLocalDirectory dependencies. No such path: {0}", BaseAppLocalDependenciesPath);
			}
		}
	}

	static void StageAppLocalDependenciesToDir(DeploymentContext SC, DirectoryReference BaseAppLocalDependenciesPath, StagedDirectoryReference StagedBinariesDir)
	{
		// Check if there are any executables being staged in this directory. Usually we only need to stage runtime dependencies next to the executable, but we may be staging
		// other engine executables too (eg. CEF)
		List<StagedFileReference> FilesInTargetDir = SC.FilesToStage.NonUFSFiles.Keys.Where(x => x.IsUnderDirectory(StagedBinariesDir) && (x.HasExtension(".exe") || x.HasExtension(".dll"))).ToList();
		if(FilesInTargetDir.Count > 0)
		{
			LogInformation("Copying AppLocal dependencies from {0} to {1}", BaseAppLocalDependenciesPath, StagedBinariesDir);

			// Stage files in subdirs
			foreach (DirectoryReference DependencyDirectory in DirectoryReference.EnumerateDirectories(BaseAppLocalDependenciesPath))
			{	
				SC.StageFiles(StagedFileType.NonUFS, DependencyDirectory, StageFilesSearch.AllDirectories, StagedBinariesDir);
			}
		}
	}

    /// <summary>
    /// Try to get the SYMSTORE.EXE path from the given Windows SDK version
    /// </summary>
    /// <returns>Path to SYMSTORE.EXE</returns>
    private static FileReference GetSymStoreExe()
    {
		List<KeyValuePair<string, DirectoryReference>> WindowsSdkDirs = WindowsExports.GetWindowsSdkDirs();
		foreach (DirectoryReference WindowsSdkDir in WindowsSdkDirs.Select(x => x.Value))
		{
			FileReference SymStoreExe64 = FileReference.Combine(WindowsSdkDir, "Debuggers", "x64", "SymStore.exe");
			if (FileReference.Exists(SymStoreExe64))
			{
				return SymStoreExe64;
			}

			FileReference SymStoreExe32 = FileReference.Combine(WindowsSdkDir, "Debuggers", "x86", "SymStore.exe");
			if (FileReference.Exists(SymStoreExe32))
			{
				return SymStoreExe32;
			}
		}
		throw new AutomationException("Unable to find a Windows SDK installation containing PDBSTR.EXE");
    }

	public static bool TryGetPdbCopyLocation(out FileReference OutLocation)
	{
		// Try to find an installation of the Windows 10 SDK
		List<KeyValuePair<string, DirectoryReference>> WindowsSdkDirs = WindowsExports.GetWindowsSdkDirs();
		foreach (DirectoryReference WindowsSdkDir in WindowsSdkDirs.Select(x => x.Value))
		{
			FileReference PdbCopyExe = FileReference.Combine(WindowsSdkDir, "Debuggers", "x64", "PdbCopy.exe");
			if (FileReference.Exists(PdbCopyExe))
			{
				OutLocation = PdbCopyExe;
				return true;
			}
		}

		// Look for an installation of the MSBuild 14
		FileReference LocationMsBuild14 = FileReference.Combine(DirectoryReference.GetSpecialFolder(Environment.SpecialFolder.ProgramFilesX86), "MSBuild", "Microsoft", "VisualStudio", "v14.0", "AppxPackage", "PDBCopy.exe");
		if(FileReference.Exists(LocationMsBuild14))
		{
			OutLocation = LocationMsBuild14;
			return true;
		}

		// Look for an installation of the MSBuild 12
		FileReference LocationMsBuild12 = FileReference.Combine(DirectoryReference.GetSpecialFolder(Environment.SpecialFolder.ProgramFilesX86), "MSBuild", "Microsoft", "VisualStudio", "v12.0", "AppxPackage", "PDBCopy.exe");
		if(FileReference.Exists(LocationMsBuild12))
		{
			OutLocation = LocationMsBuild12;
			return true;
		}

		// Otherwise fail
		OutLocation = null;
		return false;
	}

	public override void StripSymbols(FileReference SourceFile, FileReference TargetFile)
	{
		bool bStripInPlace = false;

		if (SourceFile == TargetFile)
		{
			// PDBCopy only supports creation of a brand new stripped file so we have to create a temporary filename
			TargetFile = new FileReference(Path.Combine(TargetFile.Directory.FullName, Guid.NewGuid().ToString() + TargetFile.GetExtension()));
			bStripInPlace = true;
		}

		FileReference PdbCopyLocation;
		if(!TryGetPdbCopyLocation(out PdbCopyLocation))
		{
			throw new AutomationException("Unable to find installation of PDBCOPY.EXE, which is required to strip symbols. This tool is included as part of the 'Windows Debugging Tools' component of the Windows 10 SDK (https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk).");
		}

		ProcessStartInfo StartInfo = new ProcessStartInfo();
		StartInfo.FileName = PdbCopyLocation.FullName;
		StartInfo.Arguments = String.Format("\"{0}\" \"{1}\" -p", SourceFile.FullName, TargetFile.FullName);
		StartInfo.UseShellExecute = false;
		StartInfo.CreateNoWindow = true;
		Utils.RunLocalProcessAndLogOutput(StartInfo);

		if (bStripInPlace)
		{
			// Copy stripped file to original location and delete the temporary file
			File.Copy(TargetFile.FullName, SourceFile.FullName, true);
			FileReference.Delete(TargetFile);
		}
	}

	public override bool PublishSymbols(DirectoryReference SymbolStoreDirectory, List<FileReference> Files, string Product, string BuildVersion = null)
    {
        // Get the SYMSTORE.EXE path, using the latest SDK version we can find.
        FileReference SymStoreExe = GetSymStoreExe();

		List<FileReference> FilesToAdd = Files.Where(x => x.HasExtension(".pdb") || x.HasExtension(".exe") || x.HasExtension(".dll")).ToList();
		if(FilesToAdd.Count > 0)
		{
			DateTime Start = DateTime.Now;
			DirectoryReference TempSymStoreDir = DirectoryReference.Combine(Unreal.RootDirectory, "Saved", "SymStore");

			if (DirectoryReference.Exists(TempSymStoreDir))
			{
				CommandUtils.DeleteDirectory(TempSymStoreDir);
				DirectoryReference.CreateDirectory(TempSymStoreDir);
			}

			string TempFileName = Path.GetTempFileName();
			try
			{
				File.WriteAllLines(TempFileName, FilesToAdd.Select(x => x.FullName), Encoding.ASCII);

				// Copy everything to the temp symstore
				ProcessStartInfo StartInfo = new ProcessStartInfo();
				StartInfo.FileName = SymStoreExe.FullName;
				StartInfo.Arguments = string.Format("add /f \"@{0}\" /s \"{1}\" /t \"{2}\"", TempFileName, TempSymStoreDir, Product);
				StartInfo.UseShellExecute = false;
				StartInfo.CreateNoWindow = true;
				if (Utils.RunLocalProcessAndLogOutput(StartInfo) != 0)
				{
					return false;
				}
			}
			finally
			{
				File.Delete(TempFileName);
			}
			DateTime CompressDone = DateTime.Now;
			LogInformation("Took {0}s to compress the symbol files to temp path {1}", (CompressDone - Start).TotalSeconds, TempSymStoreDir);

			int CopiedCount = 0;

			// Take each new compressed file made and try and copy it to the real symstore.  Exclude any symstore admin files
			foreach(FileReference File in DirectoryReference.EnumerateFiles(TempSymStoreDir, "*.*", SearchOption.AllDirectories).Where(File => IsSymbolFile(File)))
			{
				string RelativePath = File.MakeRelativeTo(DirectoryReference.Combine(TempSymStoreDir));
				FileReference ActualDestinationFile = FileReference.Combine(SymbolStoreDirectory, RelativePath);

				// Try and add a version file.  Do this before checking to see if the symbol is there already in the case of exact matches (multiple builds could use the same pdb, for example)
				if (!string.IsNullOrWhiteSpace(BuildVersion))
				{
					FileReference BuildVersionFile = FileReference.Combine(ActualDestinationFile.Directory, string.Format("{0}.version", BuildVersion));
					// Attempt to create the file. Just continue if it fails.
					try
					{
						DirectoryReference.CreateDirectory(BuildVersionFile.Directory);
						FileReference.WriteAllText(BuildVersionFile, string.Empty);
					}
					catch (Exception Ex)
					{
						LogWarning("Failed to write the version file, reason {0}", Ex.ToString());
					}
				}

				// Don't bother copying the temp file if the destination file is there already.
				if (FileReference.Exists(ActualDestinationFile))
				{
					LogInformation("Destination file {0} already exists, skipping", ActualDestinationFile.FullName);
					continue;
				}

				FileReference TempDestinationFile = new FileReference(ActualDestinationFile.FullName + Guid.NewGuid().ToString());
				try
				{
					CommandUtils.CopyFile(File.FullName, TempDestinationFile.FullName);
				}
				catch(Exception Ex)
				{
					throw new AutomationException("Couldn't copy the symbol file to the temp store! Reason: {0}", Ex.ToString());
				}
				// Move the file in the temp store over.
				try
				{
					FileReference.Move(TempDestinationFile, ActualDestinationFile);
					//LogVerbose("Moved {0} to {1}", TempDestinationFile, ActualDestinationFile);
					CopiedCount++;
				}
				catch (Exception Ex)
				{
					// If the file is there already, it was likely either copied elsewhere (and this is an ioexception) or it had a file handle open already.
					// Either way, it's fine to just continue on.
					if (FileReference.Exists(ActualDestinationFile))
					{
						LogInformation("Destination file {0} already exists or was in use, skipping.", ActualDestinationFile.FullName);
						continue;
					}
					// If it doesn't exist, we actually failed to copy it entirely.
					else
					{
						LogWarning("Couldn't move temp file {0} to the symbol store at location {1}! Reason: {2}", TempDestinationFile.FullName, ActualDestinationFile.FullName, Ex.ToString());
					}
				}
				// Delete the temp one no matter what, don't want them hanging around in the symstore
				finally
				{
					FileReference.Delete(TempDestinationFile);
				}
			}
			LogInformation("Took {0}s to copy {1} symbol files to the store at {2}", (DateTime.Now - CompressDone).TotalSeconds, CopiedCount, SymbolStoreDirectory);

			FileReference PingmeFile = FileReference.Combine(SymbolStoreDirectory, "pingme.txt");
			if (!FileReference.Exists(PingmeFile))
			{
				LogInformation("Creating {0} to mark path as three-tiered symbol location", PingmeFile);
				File.WriteAllText(PingmeFile.FullName, "Exists to mark this as a three-tiered symbol location");
			}
		}
			
		return true;
    }

	bool IsSymbolFile(FileReference File)
	{
		if (File.HasExtension(".dll") || File.HasExtension(".exe") || File.HasExtension(".pdb"))
		{
			return true;
		}
		if (File.HasExtension(".dl_") || File.HasExtension(".ex_") || File.HasExtension(".pd_"))
		{
			return true;
		}
		return false;
	}

	public override string[] SymbolServerDirectoryStructure
    {
        get
        {
            return new string[]
            {
                "{0}*.pdb;{0}*.exe;{0}*.dll", // Binary File Directory (e.g. QAGameClient-Win64-Test.exe --- .pdb, .dll and .exe are allowed extensions)
                "*",                          // Hash Directory        (e.g. A92F5744D99F416EB0CCFD58CCE719CD1)
            };
        }
    }
	
	// Lock file no longer needed since files are moved over the top from the temp symstore
	public override bool SymbolServerRequiresLock
	{
		get
		{
			return false;
		}
	}
}
