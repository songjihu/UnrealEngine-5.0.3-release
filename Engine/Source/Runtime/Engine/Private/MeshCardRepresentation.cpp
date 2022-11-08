// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MeshCardRepresentation.cpp
=============================================================================*/

#include "MeshCardRepresentation.h"
#include "HAL/RunnableThread.h"
#include "HAL/Runnable.h"
#include "Misc/App.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"
#include "ProfilingDebugging/CookStats.h"
#include "Templates/UniquePtr.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "DistanceFieldAtlas.h"
#include "Misc/QueuedThreadPoolWrapper.h"
#include "Async/Async.h"
#include "ObjectCacheContext.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "DerivedDataCacheInterface.h"
#include "MeshUtilities.h"
#include "StaticMeshCompiler.h"
#endif

#define LOCTEXT_NAMESPACE "MeshCardRepresentation"

#if WITH_EDITORONLY_DATA
#include "IMeshBuilderModule.h"
#endif

#if ENABLE_COOK_STATS
namespace CardRepresentationCookStats
{
	FCookStats::FDDCResourceUsageStats UsageStats;
	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
	{
		UsageStats.LogStats(AddStat, TEXT("CardRepresentation.Usage"), TEXT(""));
	});
}
#endif

static TAutoConsoleVariable<int32> CVarCardRepresentation(
	TEXT("r.MeshCardRepresentation"),
	1,
	TEXT(""),
	ECVF_ReadOnly);

static TAutoConsoleVariable<float> CVarCardRepresentationMinDensity(
	TEXT("r.MeshCardRepresentation.MinDensity"),
	0.2f,
	TEXT("How much of filled area needs to be there to spawn a card, [0;1] range."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<float> CVarCardRepresentationNormalTreshold(
	TEXT("r.MeshCardRepresentation.NormalTreshold"),
	0.25f,
	TEXT("Normal treshold when surface elements should be clustered together."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarCardRepresentationMaxSurfelDistanceXY(
	TEXT("r.MeshCardRepresentation.DistanceTresholdXY"),
	4,
	TEXT("Max distance (in surfels) when surface elements should be clustered together along XY."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarCardRepresentationMaxSurfelDistanceZ(
	TEXT("r.MeshCardRepresentation.DistanceTresholdZ"),
	16,
	TEXT("Max distance (in surfels) when surface elements should be clustered together along Z."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarCardRepresentationSeedIterations(
	TEXT("r.MeshCardRepresentation.SeedIterations"),
	3,
	TEXT("Max number of clustering iterations."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarCardRepresentationGrowIterations(
	TEXT("r.MeshCardRepresentation.GrowIterations"),
	3,
	TEXT("Max number of grow iterations."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarCardRepresentationDebugSurfelDirection(
	TEXT("r.MeshCardRepresentation.Debug.SurfelDirection"),
	-1,
	TEXT("Generate cards for only surfels pointing in a specific direction."),
	ECVF_Default);

float MeshCardRepresentation::GetMinDensity()
{
	return FMath::Clamp(CVarCardRepresentationMinDensity.GetValueOnAnyThread(), 0.0f, 1.0f);
}

float MeshCardRepresentation::GetNormalTreshold()
{
	return FMath::Clamp(CVarCardRepresentationNormalTreshold.GetValueOnAnyThread(), 0.0f, 1.0f);
}

int32 MeshCardRepresentation::GetMaxSurfelDistanceXY()
{
	return FMath::Max(CVarCardRepresentationMaxSurfelDistanceXY.GetValueOnAnyThread(), 0);
}

int32 MeshCardRepresentation::GetMaxSurfelDistanceZ()
{
	return FMath::Max(CVarCardRepresentationMaxSurfelDistanceZ.GetValueOnAnyThread(), 0);
}

int32 MeshCardRepresentation::GetDebugSurfelDirection()
{
	return FMath::Clamp(CVarCardRepresentationDebugSurfelDirection.GetValueOnAnyThread(), -1, 5);
}

int32 MeshCardRepresentation::GetSeedIterations()
{
	return FMath::Clamp(CVarCardRepresentationSeedIterations.GetValueOnAnyThread(), 1, 16);
}

int32 MeshCardRepresentation::GetGrowIterations()
{
	return FMath::Clamp(CVarCardRepresentationGrowIterations.GetValueOnAnyThread(), 0, 16);
}

FCardRepresentationAsyncQueue* GCardRepresentationAsyncQueue = NULL;

#if WITH_EDITOR

// DDC key for card representation data, must be changed when modifying the generation code or data format
#define CARDREPRESENTATION_DERIVEDDATA_VER TEXT("B7D0E3B0-440D-4C43-82C7-B2117F14A692")

FString BuildCardRepresentationDerivedDataKey(const FString& InMeshKey, int32 MaxLumenMeshCards)
{
	const float MinDensity = MeshCardRepresentation::GetMinDensity();
	const float NormalTreshold = MeshCardRepresentation::GetNormalTreshold();
	const float MaxSurfelDistanceXY = MeshCardRepresentation::GetMaxSurfelDistanceXY();
	const float MaxSurfelDistanceZ = MeshCardRepresentation::GetMaxSurfelDistanceZ();
	const int32 SeedIterations = MeshCardRepresentation::GetSeedIterations();
	const int32 GrowIterations = MeshCardRepresentation::GetGrowIterations();
	const int32 DebugSurfelDirection = MeshCardRepresentation::GetDebugSurfelDirection();

	return FDerivedDataCacheInterface::BuildCacheKey(
		TEXT("CARD"),
		*FString::Printf(TEXT("%s_%s_%.3f_%.3f_%.3f_%d_%d_%d_%d_%d"), *InMeshKey, CARDREPRESENTATION_DERIVEDDATA_VER, 
			MinDensity, NormalTreshold, MaxSurfelDistanceXY, MaxSurfelDistanceZ, SeedIterations, GrowIterations, MaxLumenMeshCards, DebugSurfelDirection),
		TEXT(""));
}

#endif

#if WITH_EDITORONLY_DATA

void BeginCacheMeshCardRepresentation(const ITargetPlatform* TargetPlatform, UStaticMesh* StaticMeshAsset, FStaticMeshRenderData& RenderData, const FString& DistanceFieldKey, FSourceMeshDataForDerivedDataTask* OptionalSourceMeshData)
{
	static const auto CVarCards = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MeshCardRepresentation"));

	if (CVarCards->GetValueOnAnyThread() != 0)
	{
		const FMeshBuildSettings& BuildSettings = StaticMeshAsset->GetSourceModel(0).BuildSettings;
		FString Key = BuildCardRepresentationDerivedDataKey(DistanceFieldKey, BuildSettings.MaxLumenMeshCards);
		if (RenderData.LODResources.IsValidIndex(0))
		{
			if (!RenderData.LODResources[0].CardRepresentationData)
			{
				RenderData.LODResources[0].CardRepresentationData = new FCardRepresentationData();
			}

			UStaticMesh* MeshToGenerateFrom = StaticMeshAsset;

			RenderData.LODResources[0].CardRepresentationData->CacheDerivedData(
				Key,
				TargetPlatform,
				StaticMeshAsset,
				MeshToGenerateFrom,
				BuildSettings.MaxLumenMeshCards,
				BuildSettings.bGenerateDistanceFieldAsIfTwoSided,
				OptionalSourceMeshData);
		}
	}
}

void FCardRepresentationData::CacheDerivedData(const FString& InDDCKey, const ITargetPlatform* TargetPlatform, UStaticMesh* Mesh, UStaticMesh* GenerateSource, int32 MaxLumenMeshCards, bool bGenerateDistanceFieldAsIfTwoSided, FSourceMeshDataForDerivedDataTask* OptionalSourceMeshData)
{
	TArray<uint8> DerivedData;

	COOK_STAT(auto Timer = CardRepresentationCookStats::UsageStats.TimeSyncWork());
	if (GetDerivedDataCacheRef().GetSynchronous(*InDDCKey, DerivedData, Mesh->GetPathName()))
	{
		COOK_STAT(Timer.AddHit(DerivedData.Num()));
		FMemoryReader Ar(DerivedData, /*bIsPersistent=*/ true);
		Ar << *this;
	}
	else
	{
		// We don't actually build the resource until later, so only track the cycles used here.
		COOK_STAT(Timer.TrackCyclesOnly());
		FAsyncCardRepresentationTask* NewTask = new FAsyncCardRepresentationTask;
		NewTask->DDCKey = InDDCKey;
		check(Mesh && GenerateSource);
		NewTask->StaticMesh = Mesh;
		NewTask->GenerateSource = GenerateSource;
		NewTask->GeneratedCardRepresentation = new FCardRepresentationData();
		NewTask->MaxLumenMeshCards = MaxLumenMeshCards;
		NewTask->bGenerateDistanceFieldAsIfTwoSided = bGenerateDistanceFieldAsIfTwoSided;

		for (int32 MaterialIndex = 0; MaterialIndex < Mesh->GetStaticMaterials().Num(); MaterialIndex++)
		{
			FSignedDistanceFieldBuildMaterialData MaterialData;
			// Default material blend mode
			MaterialData.BlendMode = BLEND_Opaque;
			MaterialData.bTwoSided = false;

			if (Mesh->GetStaticMaterials()[MaterialIndex].MaterialInterface)
			{
				MaterialData.BlendMode = Mesh->GetStaticMaterials()[MaterialIndex].MaterialInterface->GetBlendMode();
				MaterialData.bTwoSided = Mesh->GetStaticMaterials()[MaterialIndex].MaterialInterface->IsTwoSided();
			}

			NewTask->MaterialBlendModes.Add(MaterialData);
		}

		// Nanite overrides source static mesh with a coarse representation. Need to load original data before we build the mesh SDF.
		if (OptionalSourceMeshData)
		{
			NewTask->SourceMeshData = *OptionalSourceMeshData;
		}
		else if (Mesh->NaniteSettings.bEnabled)
		{
			IMeshBuilderModule& MeshBuilderModule = IMeshBuilderModule::GetForPlatform(TargetPlatform);
			if (!MeshBuilderModule.BuildMeshVertexPositions(Mesh, NewTask->SourceMeshData.TriangleIndices, NewTask->SourceMeshData.VertexPositions))
			{
				UE_LOG(LogStaticMesh, Error, TEXT("Failed to build static mesh. See previous line(s) for details."));
			}
		}

		GCardRepresentationAsyncQueue->AddTask(NewTask);
	}
}

#endif

int32 GUseAsyncCardRepresentationBuildQueue = 1;
static FAutoConsoleVariableRef CVarCardRepresentationAsyncBuildQueue(
	TEXT("r.MeshCardRepresentation.Async"),
	GUseAsyncCardRepresentationBuildQueue,
	TEXT("."),
	ECVF_Default | ECVF_ReadOnly
	);

FCardRepresentationAsyncQueue::FCardRepresentationAsyncQueue()
	: Notification(GetAssetNameFormat())
{
#if WITH_EDITOR
	MeshUtilities = NULL;

	const int32 MaxConcurrency = -1;
	// In Editor, we allow faster compilation by letting the asset compiler's scheduler organize work.
	FQueuedThreadPool* InnerThreadPool = FAssetCompilingManager::Get().GetThreadPool();
#else
	const int32 MaxConcurrency = 1;
	FQueuedThreadPool* InnerThreadPool = GThreadPool;
#endif

	if (InnerThreadPool != nullptr)
	{
		ThreadPool = MakeUnique<FQueuedThreadPoolWrapper>(InnerThreadPool, MaxConcurrency, [](EQueuedWorkPriority) { return EQueuedWorkPriority::Lowest; });
	}

	FAssetCompilingManager::Get().RegisterManager(this);

	PostReachabilityAnalysisHandle = FCoreUObjectDelegates::PostReachabilityAnalysis.AddRaw(this, &FCardRepresentationAsyncQueue::OnPostReachabilityAnalysis);
}

FCardRepresentationAsyncQueue::~FCardRepresentationAsyncQueue()
{
	FAssetCompilingManager::Get().UnregisterManager(this);
	FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(PostReachabilityAnalysisHandle);
}

void FCardRepresentationAsyncQueue::OnPostReachabilityAnalysis()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::CancelUnreachableMeshes);

	CancelAndDeleteTaskByPredicate([this](FAsyncCardRepresentationTask* Task) { return IsTaskInvalid(Task); });
}
void FAsyncCardRepresentationTaskWorker::DoWork()
{
	// Put on background thread to avoid interfering with game-thread bound tasks
	FQueuedThreadPoolTaskGraphWrapper TaskGraphWrapper(ENamedThreads::AnyBackgroundThreadNormalTask);
	GCardRepresentationAsyncQueue->Build(&Task, TaskGraphWrapper);
}

FName FCardRepresentationAsyncQueue::GetStaticAssetTypeName()
{
	return TEXT("UE-MeshCard");
}

FName FCardRepresentationAsyncQueue::GetAssetTypeName() const 
{
	return GetStaticAssetTypeName();
}

FTextFormat FCardRepresentationAsyncQueue::GetAssetNameFormat() const
{
	return LOCTEXT("MeshCardNameFormat", "{0}|plural(one=Mesh Card,other=Mesh Cards)");
}

TArrayView<FName> FCardRepresentationAsyncQueue::GetDependentTypeNames() const 
{
	static FName DependentTypeNames[] = { FDistanceFieldAsyncQueue::GetStaticAssetTypeName() };
	return TArrayView<FName>(DependentTypeNames);
}

int32 FCardRepresentationAsyncQueue::GetNumRemainingAssets() const
{
	return GetNumOutstandingTasks();
}

void FCardRepresentationAsyncQueue::FinishAllCompilation()
{
	BlockUntilAllBuildsComplete();
}

bool FCardRepresentationAsyncQueue::IsTaskInvalid(FAsyncCardRepresentationTask* Task) const
{
	return (Task->StaticMesh && Task->StaticMesh->IsUnreachable()) || (Task->GenerateSource && Task->GenerateSource->IsUnreachable());
}

void FCardRepresentationAsyncQueue::CancelAndDeleteTaskByPredicate(TFunctionRef<bool (FAsyncCardRepresentationTask*)> InShouldCancelPredicate)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::CancelAndDeleteTaskByPredicate);

	FScopeLock Lock(&CriticalSection);

	if (ReferencedTasks.Num() || PendingTasks.Num() || CompletedTasks.Num())
	{
		TSet<FAsyncCardRepresentationTask*> Removed;

		auto RemoveByPredicate =
			[&InShouldCancelPredicate, &Removed](TSet<FAsyncCardRepresentationTask*>& Tasks)
		{
			for (auto It = Tasks.CreateIterator(); It; ++It)
			{
				FAsyncCardRepresentationTask* Task = *It;
				if (InShouldCancelPredicate(Task))
				{
					Removed.Add(Task);
					It.RemoveCurrent();
				}
			}
		};

		RemoveByPredicate(PendingTasks);
		RemoveByPredicate(ReferencedTasks);
		RemoveByPredicate(CompletedTasks);

		Lock.Unlock();

		CancelAndDeleteTask(Removed);
	}
}

void FCardRepresentationAsyncQueue::CancelAndDeleteTask(const TSet<FAsyncCardRepresentationTask*>& Tasks)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::CancelAndDeleteTask);

	// Do all the cancellation first to make sure none of these tasks
	// get scheduled as we're waiting for completion.
	for (FAsyncCardRepresentationTask* Task : Tasks)
	{
		if (Task->AsyncTask)
		{
			Task->AsyncTask->Cancel();
		}
	}

	for (FAsyncCardRepresentationTask* Task : Tasks)
	{
		if (Task->AsyncTask)
		{
			Task->AsyncTask->EnsureCompletion();
			Task->AsyncTask.Reset();
		}
	}

	for (FAsyncCardRepresentationTask* Task : Tasks)
	{
		if (Task->GeneratedCardRepresentation != nullptr)
		{
			// Rendering thread may still be referencing the old one, use the deferred cleanup interface to delete it next frame when it is safe
			BeginCleanup(Task->GeneratedCardRepresentation);
		}

#if DO_GUARD_SLOW
		{
			FScopeLock Lock(&CriticalSection);
			check(!PendingTasks.Contains(Task));
			check(!ReferencedTasks.Contains(Task));
			check(!CompletedTasks.Contains(Task));
		}
#endif
		delete Task;
	}
}

void FCardRepresentationAsyncQueue::StartBackgroundTask(FAsyncCardRepresentationTask* Task)
{
	check(Task->AsyncTask == nullptr);
	Task->AsyncTask = MakeUnique<FAsyncTask<FAsyncCardRepresentationTaskWorker>>(*Task);
	Task->AsyncTask->StartBackgroundTask(ThreadPool.Get(), EQueuedWorkPriority::Lowest);
}

void FCardRepresentationAsyncQueue::ProcessPendingTasks()
{
	FScopeLock Lock(&CriticalSection);

	for (auto It = PendingTasks.CreateIterator(); It; ++It)
	{
		FAsyncCardRepresentationTask* Task = *It;
		if (Task->GenerateSource == nullptr || 
			Task->GenerateSource->IsCompiling() == false)
		{
			StartBackgroundTask(Task);
			It.RemoveCurrent();
		}
	}
}

void FCardRepresentationAsyncQueue::AddTask(FAsyncCardRepresentationTask* Task)
{
#if WITH_EDITOR
	// This could happen during the cancellation of async static mesh build
	// Simply delete the task if the static mesh are being garbage collected
	if (IsTaskInvalid(Task))
	{
		CancelAndDeleteTask({ Task });
		return;
	}

	if (!MeshUtilities)
	{
		MeshUtilities = &FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
	}

	const bool bUseAsyncBuild = GUseAsyncCardRepresentationBuildQueue || !IsInGameThread();
	const bool bIsCompiling = Task->GenerateSource->IsCompiling();
	{
		// Array protection when called from multiple threads
		FScopeLock Lock(&CriticalSection);
		check(!CompletedTasks.Contains(Task)); // reusing same pointer for a new task that is marked completed but has been canceled...
		ReferencedTasks.Add(Task);

		// The Source Mesh's RenderData is not ready yet, postpone the build
		if (bIsCompiling)
		{
			PendingTasks.Add(Task);
		}
		else if (bUseAsyncBuild)
		{
			// Make sure the Task is launched while we hold the lock to avoid
			// race with cancellation
			StartBackgroundTask(Task);
		}
	}

	if (!bIsCompiling && !bUseAsyncBuild)
	{
		// To avoid deadlocks, we must queue the inner build tasks on another thread pool, so use the task graph.
		// Put on background thread to avoid interfering with game-thread bound tasks
		FQueuedThreadPoolTaskGraphWrapper TaskGraphWrapper(ENamedThreads::AnyBackgroundThreadNormalTask);
		Build(Task, TaskGraphWrapper);
	}
#else
	UE_LOG(LogStaticMesh,Fatal,TEXT("Tried to build a card representation without editor support (this should have been done during cooking)"));
#endif
}

void FCardRepresentationAsyncQueue::CancelBuild(UStaticMesh* InStaticMesh)
{
	CancelBuilds({ InStaticMesh });
}

void FCardRepresentationAsyncQueue::CancelBuilds(const TSet<UStaticMesh*>& InStaticMeshes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::CancelBuilds);

	CancelAndDeleteTaskByPredicate(
		[&InStaticMeshes](FAsyncCardRepresentationTask* Task) 
		{
			return InStaticMeshes.Contains(Task->GenerateSource) || InStaticMeshes.Contains(Task->StaticMesh);
		}
	);
}

void FCardRepresentationAsyncQueue::CancelAllOutstandingBuilds()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::CancelAllOutstandingBuilds);

	TSet<FAsyncCardRepresentationTask*> OutstandingTasks;
	{
		FScopeLock Lock(&CriticalSection);
		PendingTasks.Reset();
		OutstandingTasks = MoveTemp(ReferencedTasks);
	}

	CancelAndDeleteTask(OutstandingTasks);
}

void FCardRepresentationAsyncQueue::RescheduleBackgroundTask(FAsyncCardRepresentationTask* InTask, EQueuedWorkPriority InPriority)
{
	if (InTask->AsyncTask && InTask->AsyncTask->GetPriority() != InPriority)
	{
		InTask->AsyncTask->Reschedule(GThreadPool, InPriority);
	}
}

void FCardRepresentationAsyncQueue::BlockUntilBuildComplete(UStaticMesh* StaticMesh, bool bWarnIfBlocked)
{
	// We will track the wait time here, but only the cycles used.
	// This function is called whether or not an async task is pending, 
	// so we have to look elsewhere to properly count how many resources have actually finished building.
	COOK_STAT(auto Timer = CardRepresentationCookStats::UsageStats.TimeAsyncWait());
	COOK_STAT(Timer.TrackCyclesOnly());
	bool bReferenced = false;
	bool bHadToBlock = false;
	double StartTime = 0;

#if WITH_EDITOR
	FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
#endif

	TSet<UStaticMesh*> RequiredFinishCompilation;
	do 
	{
		ProcessAsyncTasks();

		bReferenced = false;

		{
			FScopeLock Lock(&CriticalSection);
			for (FAsyncCardRepresentationTask* Task : ReferencedTasks)
			{
				if (Task->StaticMesh == StaticMesh ||
					Task->GenerateSource == StaticMesh)
				{
					bReferenced = true;

					// If the task we are waiting on depends on other static meshes
					// we need to force finish them too.
#if WITH_EDITOR
					
					if (Task->GenerateSource != nullptr &&
						Task->GenerateSource->IsCompiling())
					{
						RequiredFinishCompilation.Add(Task->GenerateSource);
					}

					if (Task->StaticMesh != nullptr &&
						Task->StaticMesh->IsCompiling())
					{
						RequiredFinishCompilation.Add(Task->StaticMesh);
					}
#endif

					RescheduleBackgroundTask(Task, EQueuedWorkPriority::Blocking);
				}
			}
		}

#if WITH_EDITOR
		// Call the finish compilation outside of the critical section since those compilations
		// might need to register new distance field tasks which also uses the critical section.
		if (RequiredFinishCompilation.Num())
		{
			FStaticMeshCompilingManager::Get().FinishCompilation(RequiredFinishCompilation.Array());
		}
#endif

		if (bReferenced)
		{
			if (!bHadToBlock)
			{
				StartTime = FPlatformTime::Seconds();
			}

			bHadToBlock = true;
			FPlatformProcess::Sleep(.01f);
		}
	} 
	while (bReferenced);

	if (bHadToBlock &&
		bWarnIfBlocked
#if WITH_EDITOR
		&& !FAutomationTestFramework::Get().GetCurrentTest() // HACK - Don't output this warning during automation test
#endif
		)
	{
		UE_LOG(LogStaticMesh, Display, TEXT("Main thread blocked for %.3fs for async card representation build of %s to complete!  This can happen if the mesh is rebuilt excessively."),
			(float)(FPlatformTime::Seconds() - StartTime), 
			*StaticMesh->GetName());
	}
}

void FCardRepresentationAsyncQueue::BlockUntilAllBuildsComplete()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::BlockUntilAllBuildsComplete)
	while (true)
	{
#if WITH_EDITOR
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
		// Reschedule as highest prio since we're explicitly waiting on them
		{
			FScopeLock Lock(&CriticalSection);
			for (FAsyncCardRepresentationTask* Task : ReferencedTasks)
			{
				RescheduleBackgroundTask(Task, EQueuedWorkPriority::Blocking);
			}
		}

		ProcessAsyncTasks();

		if (GetNumOutstandingTasks() <= 0)
		{
			break;
		}

		FPlatformProcess::Sleep(.01f);
	} 
}

void FCardRepresentationAsyncQueue::Build(FAsyncCardRepresentationTask* Task, FQueuedThreadPool& BuildThreadPool)
{
#if WITH_EDITOR
	
	// Editor 'force delete' can null any UObject pointers which are seen by reference collecting (eg UProperty or serialized)
	if (Task->StaticMesh && Task->GenerateSource)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::Build);

		const FStaticMeshLODResources& LODModel = Task->GenerateSource->GetRenderData()->LODResources[0];

		Task->bSuccess = MeshUtilities->GenerateCardRepresentationData(
			Task->StaticMesh->GetName(),
			Task->SourceMeshData,
			LODModel,
			BuildThreadPool,
			Task->MaterialBlendModes,
			Task->GenerateSource->GetRenderData()->Bounds,
			Task->GenerateSource->GetRenderData()->LODResources[0].DistanceFieldData,
			Task->MaxLumenMeshCards,
			Task->bGenerateDistanceFieldAsIfTwoSided,
			*Task->GeneratedCardRepresentation);
	}

	{
		FScopeLock Lock(&CriticalSection);
		// Avoid adding to the completed list if the task has been canceled
		if (ReferencedTasks.Contains(Task))
		{
			CompletedTasks.Add(Task);
		}
	}

#endif
}

void FCardRepresentationAsyncQueue::ProcessAsyncTasks(bool bLimitExecutionTime)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(FCardRepresentationAsyncQueue::ProcessAsyncTasks);

	ProcessPendingTasks();

	FObjectCacheContextScope ObjectCacheScope;
	const double MaxProcessingTime = 0.016f;
	double StartTime = FPlatformTime::Seconds();
	bool bMadeProgress = false;
	while (!bLimitExecutionTime || (FPlatformTime::Seconds() - StartTime) < MaxProcessingTime)
	{
		FAsyncCardRepresentationTask* Task = nullptr;
		{
			FScopeLock Lock(&CriticalSection);
			if (CompletedTasks.Num() > 0)
			{
				// Pop any element from the set
				auto Iterator = CompletedTasks.CreateIterator();
				Task = *Iterator;
				Iterator.RemoveCurrent();

				verify(ReferencedTasks.Remove(Task));
			}
		}

		if (Task == nullptr)
		{
			break;
		}
		bMadeProgress = true;

		// We want to count each resource built from a DDC miss, so count each iteration of the loop separately.
		COOK_STAT(auto Timer = CardRepresentationCookStats::UsageStats.TimeSyncWork());

		if (Task->AsyncTask)
		{
			Task->AsyncTask->EnsureCompletion();
			Task->AsyncTask.Reset();
		}

		// Editor 'force delete' can null any UObject pointers which are seen by reference collecting (eg UProperty or serialized)
		if (Task->StaticMesh && Task->bSuccess)
		{
			check(!Task->StaticMesh->IsCompiling());

			FStaticMeshRenderData* RenderData = Task->StaticMesh->GetRenderData();
			FCardRepresentationData* OldCardData = RenderData->LODResources[0].CardRepresentationData;

			// Assign the new data, this is safe because the render threads makes a copy of the pointer at scene proxy creation time.
			RenderData->LODResources[0].CardRepresentationData = Task->GeneratedCardRepresentation;

			// Any already created render state needs to be dirtied
			if (RenderData->IsInitialized())
			{
				for (UStaticMeshComponent* Component : ObjectCacheScope.GetContext().GetStaticMeshComponents(Task->StaticMesh))
				{
					if (Component->IsRegistered() && Component->IsRenderStateCreated())
					{
						Component->MarkRenderStateDirty();
					}
				}
			}

			// Rendering thread may still be referencing the old one, use the deferred cleanup interface to delete it next frame when it is safe
			BeginCleanup(OldCardData);

			// Need also to update platform render data if it's being cached
			FStaticMeshRenderData* PlatformRenderData = RenderData->NextCachedRenderData.Get();
			while (PlatformRenderData)
			{
				if (PlatformRenderData->LODResources[0].CardRepresentationData)
				{
					*PlatformRenderData->LODResources[0].CardRepresentationData = *Task->GeneratedCardRepresentation;
				}
				PlatformRenderData = PlatformRenderData->NextCachedRenderData.Get();
			}

			{
				TArray<uint8> DerivedData;
				// Save built data to DDC
				FMemoryWriter Ar(DerivedData, /*bIsPersistent=*/ true);
				Ar << *(Task->StaticMesh->GetRenderData()->LODResources[0].CardRepresentationData);
				GetDerivedDataCacheRef().Put(*Task->DDCKey, DerivedData, Task->StaticMesh->GetPathName());
				COOK_STAT(Timer.AddMiss(DerivedData.Num()));
			}
		}

		delete Task;
	}

	if (bMadeProgress)
	{
		Notification.Update(GetNumRemainingAssets());
	}
#endif
}

void FCardRepresentationAsyncQueue::Shutdown()
{
	CancelAllOutstandingBuilds();

	UE_LOG(LogStaticMesh, Log, TEXT("Abandoning remaining async card representation tasks for shutdown"));
	ThreadPool.Reset();
}

#undef LOCTEXT_NAMESPACE