// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using HordeCommon.Rpc;
using System;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace HordeAgent
{
	/// <summary>
	/// The type of executor to use
	/// </summary>
	public enum ExecutorType
	{
		/// <summary>
		/// The test executor (predefined job, 
		/// </summary>
		Test,

		/// <summary>
		/// The local executor (run all steps locally)
		/// </summary>
		Local,

		/// <summary>
		/// Manage workspaces and run steps in a synced branch
		/// </summary>
		Perforce,
	}

	/// <summary>
	/// Describes a network share to mount
	/// </summary>
	public class MountNetworkShare
	{
		/// <summary>
		/// Where the share should be mounted on the local machine. Must be a drive letter for Windows.
		/// </summary>
		public string? MountPoint { get; set; }

		/// <summary>
		/// Path to the remote resource
		/// </summary>
		public string? RemotePath { get; set; }
	}

	/// <summary>
	/// Information about a server to use
	/// </summary>
	public class ServerProfile
	{
		/// <summary>
		/// Name of this server profile
		/// </summary>
		[Required]
		public string Name { get; set; } = null!;

		/// <summary>
		/// Name of the environment (currrently just used for tracing)
		/// </summary>
		[Required]
		public string Environment { get; set; } = "prod";

		/// <summary>
		/// Url of the server
		/// </summary>
		[Required]
		public string Url { get; set; } = null!;

		/// <summary>
		/// Bearer token to use to initiate the connection
		/// </summary>
		public string? Token { get; set; }

		/// <summary>
		/// Thumbprint of a certificate to trust. Allows using self-signed certs for the server.
		/// </summary>
		public string? Thumbprint { get; set; }

		/// <summary>
		/// Thumbprints of certificates to trust. Allows using self-signed certs for the server.
		/// </summary>
		public List<string> Thumbprints { get; set; } = new List<string>();

		/// <summary>
		/// Checks whether the given certificate thumbprint should be trusted
		/// </summary>
		/// <param name="CertificateThumbprint">The cert thumbprint</param>
		/// <returns>True if the cert should be trusted</returns>
		public bool IsTrustedCertificate(string CertificateThumbprint)
		{
			if (Thumbprint != null && Thumbprint.Equals(CertificateThumbprint, StringComparison.OrdinalIgnoreCase))
			{
				return true;
			}
			if (Thumbprints.Any(x => x.Equals(CertificateThumbprint, StringComparison.OrdinalIgnoreCase)))
			{
				return true;
			}
			return false;
		}
	}

	/// <summary>
	/// Settings for the local executor
	/// </summary>
	public class LocalExecutorSettings
	{
		/// <summary>
		/// Path to the local workspace to use with the local executor
		/// </summary>
		public string? WorkspaceDir { get; set; }

		/// <summary>
		/// Whether to actually execute steps, or just do job setup
		/// </summary>
		public bool RunSteps { get; set; } = true;
	}

	/// <summary>
	/// Setttings for the perforce executor
	/// </summary>
	public class PerforceExecutorSettings
	{
		/// <summary>
		/// Whether to run conform jobs
		/// </summary>
		public bool RunConform { get; set; } = true;
	}

	/// <summary>
	/// Global settings for the agent
	/// </summary>
	public class AgentSettings
	{
		/// <summary>
		/// Name of the section containing these settings
		/// </summary>
		public const string SectionName = "Horde";

		/// <summary>
		/// Known servers to connect to
		/// </summary>
		public List<ServerProfile> ServerProfiles { get; set; } = new List<ServerProfile>();

		/// <summary>
		/// The default server, unless overridden from the command line
		/// </summary>
		public string? Server { get; set; }
		
		/// <summary>
		/// Name of agent to report as when connecting to server.
		/// By default, the computer's hostname will be used.
		/// </summary>
		public string? Name { get; set; }

		/// <summary>
		/// The executor to use for jobs. Defaults to the Perforce executor.
		/// </summary>
		public ExecutorType Executor { get; set; } = ExecutorType.Perforce;

		/// <summary>
		/// Settings for the local executor
		/// </summary>
		public LocalExecutorSettings LocalExecutor { get; set; } = new LocalExecutorSettings();

		/// <summary>
		/// Settings for the perforce executor
		/// </summary>
		public PerforceExecutorSettings PerforceExecutor { get; set; } = new PerforceExecutorSettings();

		/// <summary>
		/// Working directory
		/// </summary>
		public string? WorkingDir { get; set; } = DirectoryReference.Combine(Program.DataDir, "Data").FullName;

		/// <summary>
		/// Whether to mount the specified list of network shares
		/// </summary>
		public bool ShareMountingEnabled { get; set; } = true;
		
		/// <summary>
		/// List of network shares to mount
		/// </summary>
		public List<MountNetworkShare> Shares { get; set; } = new List<MountNetworkShare>();

		/// <summary>
		/// List of process names to terminate after a job
		/// </summary>
		public List<string> ProcessNamesToTerminate { get; set; } = new List<string>();

		/// <summary>
		/// Whether to write step output to the logging device
		/// </summary>
		public bool WriteStepOutputToLogger { get; set; }

		/// <summary>
		/// Gets the current server settings
		/// </summary>
		/// <returns>The current server settings</returns>
		public ServerProfile GetServerProfile(string Name)
		{
			ServerProfile? ServerProfile = ServerProfiles.FirstOrDefault(x => x.Name.Equals(Name, StringComparison.OrdinalIgnoreCase));
			if (ServerProfile == null)
			{
				if (ServerProfiles.Count == 0)
				{
					throw new Exception("No server profiles are defined (missing configuration?)");
				}
				else
				{
					throw new Exception($"Unknown server profile name '{Name}' (valid profiles: {String.Join("/", ServerProfiles.Select(x => x.Name))})");
				}
			}
			return ServerProfile;
		}

		/// <summary>
		/// Gets the current server settings
		/// </summary>
		/// <returns>The current server settings</returns>
		public ServerProfile GetCurrentServerProfile()
		{
			if (Server == null)
			{
				throw new Exception("Server is not set");
			}

			return GetServerProfile(Server);
		}
		
		internal string GetAgentName()
		{
			return Name ?? Environment.MachineName;
		}
	}
}
