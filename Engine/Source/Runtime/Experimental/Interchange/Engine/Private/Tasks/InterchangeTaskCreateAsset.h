// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/TaskGraphInterfaces.h"
#include "CoreMinimal.h"
#include "InterchangeFactoryBase.h"
#include "InterchangeManager.h"
#include "Stats/Stats.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Nodes/InterchangeBaseNode.h"

namespace UE
{
	namespace Interchange
	{

		/**
		 * This task create package, Cook::PackageTracker::NotifyUObjectCreated is not thread safe, so we need to create the packages on the main thread
		 */
		class FTaskCreatePackage
		{
		private:
			FString PackageBasePath;
			int32 SourceIndex;
			TWeakPtr<FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper;
			UInterchangeBaseNode* Node;
			const UClass* FactoryClass;

		public:
			FTaskCreatePackage(const FString& InPackageBasePath, const int32 InSourceIndex, TWeakPtr<FImportAsyncHelper, ESPMode::ThreadSafe> InAsyncHelper, UInterchangeBaseNode* InNode, const UClass* InFactoryClass)
				: PackageBasePath(InPackageBasePath)
				, SourceIndex(InSourceIndex)
				, WeakAsyncHelper(InAsyncHelper)
				, Node(InNode)
				, FactoryClass(InFactoryClass)
			{
				check(Node);
				check(FactoryClass);
			}

			ENamedThreads::Type GetDesiredThread() const
			{
				// This is no longer true now that we have to construct a factory here
				//if (WeakAsyncHelper.IsValid() && WeakAsyncHelper.Pin()->TaskData.ReimportObject)
				//{
				//	//When doing a reimport the package already exist, so we can get it outside of the main thread
				//	return ENamedThreads::AnyBackgroundThreadNormalTask;
				//}
				return ENamedThreads::GameThread;
			}

			static ESubsequentsMode::Type GetSubsequentsMode()
			{
				return ESubsequentsMode::TrackSubsequents;
			}

			TStatId GetStatId() const
			{
				RETURN_QUICK_DECLARE_CYCLE_STAT(FTaskCreatePackage, STATGROUP_TaskGraphTasks);
			}

			void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent);
		};

		class FTaskCreateAsset
		{
		private:
			FString PackageBasePath;
			int32 SourceIndex;
			TWeakPtr<FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper;
			UInterchangeBaseNode* Node;
			bool bCanRunOnAnyThread;

		public:
			FTaskCreateAsset(const FString& InPackageBasePath, const int32 InSourceIndex, TWeakPtr<FImportAsyncHelper, ESPMode::ThreadSafe> InAsyncHelper, UInterchangeBaseNode* InNode, bool bInCanRunOnAnyThread)
				: PackageBasePath(InPackageBasePath)
				, SourceIndex(InSourceIndex)
				, WeakAsyncHelper(InAsyncHelper)
				, Node(InNode)
				, bCanRunOnAnyThread(bInCanRunOnAnyThread)
			{
				check(Node);
			}

			ENamedThreads::Type GetDesiredThread() const
			{
				return bCanRunOnAnyThread ? ENamedThreads::AnyBackgroundThreadNormalTask : ENamedThreads::GameThread;
			}

			static ESubsequentsMode::Type GetSubsequentsMode()
			{
				return ESubsequentsMode::TrackSubsequents;
			}

			TStatId GetStatId() const
			{
				RETURN_QUICK_DECLARE_CYCLE_STAT(FTaskCreateAsset, STATGROUP_TaskGraphTasks);
			}

			void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent);
		};


	} //ns Interchange
}//ns UE
