// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include COMPILED_PLATFORM_HEADER_WITH_PREFIX(Platform,TestRunner.h)

#include "CommandLineUtil.h"
#include "Logging/LogScopedVerbosityOverride.h"
#include "Logging/LogSuppressionInterface.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#endif

static bool bCatchIsRunning = false;
static bool bGAllowLogging = true;
static bool bMultithreaded = true;

/**
 * Setup phase before all tests.
 */
void Setup();

/**
 * Teardown phase after all tests.
 */
void Teardown();

#if PLATFORM_SWITCH
	extern "C" const char* GetProcessExecutablePath();
#else
	const char* GetProcessExecutablePath();
#endif
const char* GetCacheDirectory();

int RunTests(int argc, const char* argv[]);