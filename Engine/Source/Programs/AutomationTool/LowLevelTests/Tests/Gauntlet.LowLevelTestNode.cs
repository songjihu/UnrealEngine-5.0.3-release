// Copyright Epic Games, Inc. All Rights Reserved.

using AutomationTool;
using Gauntlet;
using System;
using System.IO;
using System.Linq;
using UnrealBuildTool;

namespace LowLevelTests
{
	public class LowLevelTests : BaseTest
	{
		private LowLevelTestContext Context;
		
		private IAppInstance TestInstance;

		private DateTime SessionStartTime = DateTime.MinValue;

		private TestResult LowLevelTestResult;

		public LowLevelTestsSession LowLevelTestsApp { get; private set; }

		public LowLevelTests(LowLevelTestContext InContext)
		{
			Context = InContext;

			MaxDuration = 60 * 5;
			LowLevelTestResult = TestResult.Invalid;
		}

		public string DefaultCommandLine;
		private string ArtifactPath;

		public override string Name { get { return "LowLevelTest"; } }

		public override float MaxDuration { protected set; get; }

		public override bool IsReadyToStart()
		{
			if (LowLevelTestsApp == null)
			{
				LowLevelTestsApp = new LowLevelTestsSession(Context.BuildInfo);
			}

			return LowLevelTestsApp.TryReserveDevices();
		}

		public override string GetTestSummary()
		{
			return "Low Level Test";
		}

		public override TestResult GetTestResult()
		{
			return LowLevelTestResult;
		}

		public override void SetTestResult(TestResult testResult)
		{
			LowLevelTestResult = testResult;
		}

		public override bool StartTest(int Pass, int NumPasses)
		{
			if (LowLevelTestsApp == null)
			{
				throw new AutomationException("Node already has a null LowLevelTestsApp, was IsReadyToStart called?");
			}

			ArtifactPath = Path.Join(Context.Options.LogDir, Context.Options.TestApp);
			Log.Info("LowLevelTestNode.StartTest Creating artifacts path at {0}", ArtifactPath);
			Directory.CreateDirectory(ArtifactPath);

			TestInstance = LowLevelTestsApp.InstallAndRunNativeTestApp();
			if (TestInstance != null)
			{
				IDeviceUsageReporter.RecordComment(TestInstance.Device.Name, (UnrealTargetPlatform)TestInstance.Device.Platform, IDeviceUsageReporter.EventType.Device, Context.Options.JobDetails);
				IDeviceUsageReporter.RecordComment(TestInstance.Device.Name, (UnrealTargetPlatform)TestInstance.Device.Platform, IDeviceUsageReporter.EventType.Test, this.GetType().Name);
			}

			if (SessionStartTime == DateTime.MinValue)
			{
				SessionStartTime = DateTime.Now;
			}

			if (TestInstance != null)
			{
				MarkTestStarted();
			}

			return TestInstance != null;
		}

		public override void TickTest()
		{
			if (TestInstance != null && TestInstance.HasExited)
			{
				if (TestInstance.WasKilled)
				{
					LowLevelTestResult = TestResult.Failed;
				}
				MarkTestComplete();
			}
		}

		public override void StopTest(StopReason InReason)
		{
			base.StopTest(InReason);

			if (TestInstance != null && !TestInstance.HasExited)
			{
				TestInstance.Kill();
			}

			string StdOut;
			if (TestInstance is IWithUnfilteredStdOut)
			{
				StdOut = ((IWithUnfilteredStdOut)TestInstance).UnfilteredStdOut;
			}
			else
			{
				StdOut = TestInstance.StdOut;
			}

			if (StdOut == null || string.IsNullOrEmpty(StdOut.Trim()))
			{
				Log.Warning("No StdOut returned from low level test app.");
			}

			LowLevelTestsLogParser LowLevelTestsLogParser = new LowLevelTestsLogParser(StdOut);

			UnrealProcessResult Result = GetExitCodeAndReason(
				InReason,
				LowLevelTestsLogParser.GetSummary(),
				LowLevelTestsLogParser.GetCatchTestResults(),
				out string ExitReason,
				out int ExitCode);

			if (ExitCode == 0)
			{
				LowLevelTestResult = TestResult.Passed;
			}
			else
			{
				if (Result == UnrealProcessResult.TimeOut)
				{
					LowLevelTestResult = TestResult.TimedOut;
				}
				else
				{
					LowLevelTestResult = TestResult.Failed;
				}
				Log.Info("Low level test exited with code {0} and reason: {1}", ExitCode, ExitReason);
			}
		}

		public override void CleanupTest()
		{
			if (LowLevelTestsApp != null)
			{
				LowLevelTestsApp.Dispose();
				LowLevelTestsApp = null;
			}
		}

		protected virtual UnrealProcessResult GetExitCodeAndReason(StopReason InReason, UnrealLog InLog, LowLevelTestsLogParser.CatchTestResults InCatchResults, out string ExitReason, out int ExitCode)
		{
			if (TestInstance.WasKilled)
			{
				if (InReason == StopReason.MaxDuration)
				{
					ExitReason = "Process was killed by Gauntlet due to a timeout.";
					ExitCode = -1;
					return UnrealProcessResult.TimeOut;
				}
				else
				{
					ExitReason = "Process was killed by Gauntlet.";
					ExitCode = 0;
					return UnrealProcessResult.ExitOk;
				}
			}

			// First we check for unreal specific issues.
			// A successful test run must be free of these.
			if (InLog.FatalError != null)
			{
				ExitReason = "Process encountered fatal error";
				ExitCode = -1;
				return UnrealProcessResult.EncounteredFatalError;
			}

			if (InLog.Ensures.Count() > 0)
			{
				ExitReason = string.Format("Process encountered {0} Ensures", InLog.Ensures.Count());
				ExitCode = -1;
				return UnrealProcessResult.EncounteredEnsure;
			}

			if (InLog.RequestedExit)
			{
				ExitReason = string.Format("Exit was requested: {0}", InLog.RequestedExitReason);
				ExitCode = 0;
				return UnrealProcessResult.ExitOk;
			}

			// Then we check for actual test results.
			if (InCatchResults.Passed)
			{
				ExitReason = "All Catch2 tests passed.";
				ExitCode = 0;
				return UnrealProcessResult.ExitOk;
			}
			else
			{
				ExitReason = "Catch2 tests failed.";
				ExitCode = -1;
				return UnrealProcessResult.TestFailure;
			}
		}
	}
}
