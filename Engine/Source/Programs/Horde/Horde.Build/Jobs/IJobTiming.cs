// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Linq;
using System.Threading.Tasks;

namespace HordeServer.Models
{
	/// <summary>
	/// Information about the timing for an individual step
	/// </summary>
	public interface IJobStepTiming
	{
		/// <summary>
		/// Wait time before executing the group containing this node
		/// </summary>
		public float? AverageWaitTime { get; }

		/// <summary>
		/// Time taken for the group containing this node to initialize
		/// </summary>
		public float? AverageInitTime { get; }

		/// <summary>
		/// Time spent executing this node
		/// </summary>
		public float? AverageDuration { get; }
	}

	/// <summary>
	/// Timing information for a particular job
	/// </summary>
	public interface IJobTiming
	{
		/// <summary>
		/// Gets timing information for a particular step
		/// </summary>
		/// <param name="Name">Name of the node being executed</param>
		/// <param name="Timing">Receives the timing information for the given step</param>
		/// <returns>True if the timing was found</returns>
		public bool TryGetStepTiming(string Name, [NotNullWhen(true)] out IJobStepTiming? Timing);
	}
}
