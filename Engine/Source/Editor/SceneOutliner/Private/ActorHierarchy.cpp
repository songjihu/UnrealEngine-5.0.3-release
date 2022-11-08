// Copyright Epic Games, Inc. All Rights Reserved.

#include "ActorHierarchy.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "WorldTreeItem.h"
#include "LevelTreeItem.h"
#include "ActorTreeItem.h"
#include "ActorDescTreeItem.h"
#include "ComponentTreeItem.h"
#include "ActorFolderTreeItem.h"
#include "ISceneOutlinerMode.h"
#include "ActorEditorUtils.h"
#include "LevelUtils.h"
#include "GameFramework/WorldSettings.h"
#include "EditorActorFolders.h"
#include "EditorFolderUtils.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Modules/ModuleManager.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/IWorldPartitionEditorModule.h"
#include "ActorFolder.h"

TUniquePtr<FActorHierarchy> FActorHierarchy::Create(ISceneOutlinerMode* Mode, const TWeakObjectPtr<UWorld>& World)
{
	FActorHierarchy* Hierarchy = new FActorHierarchy(Mode, World);

	GEngine->OnLevelActorAdded().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorAdded);
	GEngine->OnLevelActorDeleted().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorDeleted);
	GEngine->OnLevelActorDetached().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorDetached);
	GEngine->OnLevelActorAttached().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorAttached);
	GEngine->OnLevelActorFolderChanged().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorFolderChanged);
	GEngine->OnLevelActorListChanged().AddRaw(Hierarchy, &FActorHierarchy::OnLevelActorListChanged);
	GEngine->OnActorFolderAdded().AddRaw(Hierarchy, &FActorHierarchy::OnActorFolderAdded);
	GEngine->OnActorFoldersUpdatedEvent().AddRaw(Hierarchy, &FActorHierarchy::OnActorFoldersUpdatedEvent);

	IWorldPartitionEditorModule& WorldPartitionEditorModule = FModuleManager::LoadModuleChecked<IWorldPartitionEditorModule>("WorldPartitionEditor");
	WorldPartitionEditorModule.OnWorldPartitionCreated().AddRaw(Hierarchy, &FActorHierarchy::OnWorldPartitionCreated);

	if (World.IsValid())
	{
		if (World->PersistentLevel)
		{
			World->PersistentLevel->OnLoadedActorAddedToLevelEvent.AddRaw(Hierarchy, &FActorHierarchy::OnLoadedActorAdded);
			World->PersistentLevel->OnLoadedActorRemovedFromLevelEvent.AddRaw(Hierarchy, &FActorHierarchy::OnLoadedActorRemoved);
		}

		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			WorldPartition->OnActorDescAddedEvent.AddRaw(Hierarchy, &FActorHierarchy::OnActorDescAdded);
			WorldPartition->OnActorDescRemovedEvent.AddRaw(Hierarchy, &FActorHierarchy::OnActorDescRemoved);
		}
	}

	FWorldDelegates::LevelAddedToWorld.AddRaw(Hierarchy, &FActorHierarchy::OnLevelAdded);
	FWorldDelegates::LevelRemovedFromWorld.AddRaw(Hierarchy, &FActorHierarchy::OnLevelRemoved);

	auto& Folders = FActorFolders::Get();
	Folders.OnFolderCreated.AddRaw(Hierarchy, &FActorHierarchy::OnBroadcastFolderCreate);
	Folders.OnFolderMoved.AddRaw(Hierarchy, &FActorHierarchy::OnBroadcastFolderMove);
	Folders.OnFolderDeleted.AddRaw(Hierarchy, &FActorHierarchy::OnBroadcastFolderDelete);

	return TUniquePtr<FActorHierarchy>(Hierarchy);
}

FActorHierarchy::FActorHierarchy(ISceneOutlinerMode* Mode, const TWeakObjectPtr<UWorld>& World)
	: ISceneOutlinerHierarchy(Mode)
	, RepresentingWorld(World)
{
}

FActorHierarchy::~FActorHierarchy()
{
	if (GEngine)
	{
		GEngine->OnLevelActorAdded().RemoveAll(this);
		GEngine->OnLevelActorDeleted().RemoveAll(this);
		GEngine->OnLevelActorDetached().RemoveAll(this);
		GEngine->OnLevelActorAttached().RemoveAll(this);
		GEngine->OnLevelActorFolderChanged().RemoveAll(this);
		GEngine->OnLevelActorListChanged().RemoveAll(this);
		GEngine->OnActorFolderAdded().RemoveAll(this);
		GEngine->OnActorFoldersUpdatedEvent().RemoveAll(this);
	}

	IWorldPartitionEditorModule& WorldPartitionEditorModule = FModuleManager::LoadModuleChecked<IWorldPartitionEditorModule>("WorldPartitionEditor");
	WorldPartitionEditorModule.OnWorldPartitionCreated().RemoveAll(this);

	if (RepresentingWorld.IsValid())
	{
		if (RepresentingWorld->PersistentLevel)
		{
			RepresentingWorld->PersistentLevel->OnLoadedActorAddedToLevelEvent.RemoveAll(this);
			RepresentingWorld->PersistentLevel->OnLoadedActorRemovedFromLevelEvent.RemoveAll(this);
		}

		if (UWorldPartition* WorldPartition = RepresentingWorld->GetWorldPartition())
		{
			WorldPartition->OnActorDescAddedEvent.RemoveAll(this);
			WorldPartition->OnActorDescRemovedEvent.RemoveAll(this);
		}
	}

	FWorldDelegates::LevelAddedToWorld.RemoveAll(this);
	FWorldDelegates::LevelRemovedFromWorld.RemoveAll(this);


	if (FActorFolders::IsAvailable())
	{
		auto& Folders = FActorFolders::Get();
		Folders.OnFolderCreated.RemoveAll(this);
		Folders.OnFolderMoved.RemoveAll(this);
		Folders.OnFolderDeleted.RemoveAll(this);
	}
}

static FName GetFolderPathFromActorDesc(UWorld* InWorld, const FWorldPartitionActorDesc* InActorDesc)
{
	if (InWorld && InWorld->PersistentLevel->IsUsingActorFolders())
	{
		UActorFolder* ActorFolder = InWorld->PersistentLevel->GetActorFolder(InActorDesc->GetFolderGuid());
		return ActorFolder ? ActorFolder->GetPath() : FFolder::GetEmptyPath();
	}
	return InActorDesc->GetFolderPath();
}

FSceneOutlinerTreeItemPtr FActorHierarchy::FindOrCreateParentItem(const ISceneOutlinerTreeItem& Item, const TMap<FSceneOutlinerTreeItemID, FSceneOutlinerTreeItemPtr>& Items, bool bCreate)
{
	if (Item.IsA<FWorldTreeItem>())
	{
		return nullptr;
	}
	else if (const FActorTreeItem* ActorTreeItem = Item.CastTo<FActorTreeItem>())
	{
		if (AActor* Actor = ActorTreeItem->Actor.Get())
		{
			// Parent Actor (Actor attachement / parenting)
			if (AActor* ParentActor = Actor->GetSceneOutlinerParent())
			{
				if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(ParentActor))
				{
					return *ParentItem;
				}
				// If Parent can be listed in SceneOutliner return nullptr so it gets created
				else if (ParentActor->IsListedInSceneOutliner())
				{
					return bCreate ? Mode->CreateItemFor<FActorTreeItem>(ParentActor, true) : nullptr;
				}
			}

			// Parent Folder
			FFolder Folder = Actor->GetFolder();
			if (Mode->ShouldShowFolders() && !Folder.IsNone())
			{
				if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(Folder))
				{
					return *ParentItem;
				}
				else
				{
					return bCreate ? Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(Folder, ActorTreeItem->Actor->GetWorld()), true) : nullptr;
				}
			}

			// Parent Level Instance
			if (ALevelInstance* OwningLevelInstance = Cast<ALevelInstance>(Folder.GetRootObjectPtr()))
			{
				const ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(Actor);
				const bool bIsAnEditingLevelInstance = LevelInstanceActor ? LevelInstanceActor->IsEditing() : false;
				// Parent this to a LevelInstance if the parent LevelInstance is being edited or if this is a sub LevelInstance which is being edited
				if (bShowingLevelInstances || (OwningLevelInstance->IsEditing() || bIsAnEditingLevelInstance))
				{
					if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(OwningLevelInstance))
					{
						return *ParentItem;
					}
					else
					{
						return bCreate ? Mode->CreateItemFor<FActorTreeItem>(OwningLevelInstance, true) : nullptr;
					}
				}
			}

			// Parent Level Using Actor Folders
			if (ULevel* OwningLevel = Cast<ULevel>(Folder.GetRootObjectPtr()))
			{
				if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(OwningLevel))
				{
					return *ParentItem;
				}
				else
				{
					return bCreate ? Mode->CreateItemFor<FLevelTreeItem>(OwningLevel, true) : nullptr;
				}
			}
		}
	}
	else if (const FActorFolderTreeItem* FolderItem = Item.CastTo<FActorFolderTreeItem>())
	{
		// We should never call FindParents on a folder item if folders are not being shown
		check(Mode->ShouldShowFolders());

		const FFolder ParentPath = FolderItem->GetFolder().GetParent();

		// Parent Folder
		if (!ParentPath.IsNone())
		{
			if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(ParentPath))
			{
				return *ParentItem;
			}
			else
			{
				return bCreate ? Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(ParentPath, FolderItem->World), true) : nullptr;
			}
		}
		// Parent Level Instance
		else if (ALevelInstance* OwningLevelInstance = Cast<ALevelInstance>(ParentPath.GetRootObjectPtr()))
		{
			if (bShowingLevelInstances || OwningLevelInstance->IsEditing())
			{
				if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(OwningLevelInstance))
				{
					return *ParentItem;
				}
				else
				{
					return bCreate ? Mode->CreateItemFor<FActorTreeItem>(OwningLevelInstance, true) : nullptr;
				}
			}
		}
		// Parent Level Using Actor Folders
		else if (ULevel* OwningLevel = Cast<ULevel>(ParentPath.GetRootObjectPtr()))
		{
			if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(OwningLevel))
			{
				return *ParentItem;
			}
			else
			{
				return bCreate ? Mode->CreateItemFor<FLevelTreeItem>(OwningLevel, true) : nullptr;
			}
		}
	}
	else if (const FComponentTreeItem* ComponentTreeItem = Item.CastTo<FComponentTreeItem>())
	{
		if (AActor* Owner = ComponentTreeItem->Component->GetOwner())
		{
			if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(Owner))
			{
				return *ParentItem;
			}
			else
			{
				return bCreate ? Mode->CreateItemFor<FActorTreeItem>(Owner, true) : nullptr;
			}
		}
		// do not default to world on Component items
		return nullptr;
	}
	else if (const FActorDescTreeItem* ActorDescItem = Item.CastTo<FActorDescTreeItem>())
	{
		if (const FWorldPartitionActorDesc* ActorDesc = ActorDescItem->ActorDescHandle.Get())
		{
			const FName FolderPath = GetFolderPathFromActorDesc(RepresentingWorld.Get(), ActorDesc);
			if (!FolderPath.IsNone())
			{
				if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(FFolder(FolderPath)))
				{
					return *ParentItem;
				}
				else
				{
					return bCreate ? Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(FFolder(FolderPath), RepresentingWorld.Get()), true) : nullptr;
				}
			}

			// Parent Actor (Actor attachement / parenting)
			const FGuid& ParentActorGuid = ActorDesc->GetSceneOutlinerParent();
			if (ParentActorGuid.IsValid())
			{
				if (UWorldPartition* WorldPartition = RepresentingWorld->GetWorldPartition())
				{
					if (const FWorldPartitionActorDesc* ParentActorDesc = WorldPartition->GetActorDesc(ParentActorGuid))
					{
						// If parent actor is loaded
						if (AActor* ParentActor = ParentActorDesc->GetActor())
						{
							// Find loaded parent actor node (from the object ptr)
							if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(ParentActor))
							{
								return *ParentItem;
							}
							else
							{
								return bCreate ? Mode->CreateItemFor<FActorTreeItem>(ParentActor, true) : nullptr;
							}
						}

						// Find unloaded parent actor node (from the guid)
						if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(ParentActorGuid))
						{
							return *ParentItem;
						}
						else
						{
							return bCreate ? Mode->CreateItemFor<FActorDescTreeItem>(FActorDescTreeItem(ParentActorGuid, WorldPartition)) : nullptr;
						}
					}
				}
			}
		}
	}

	// If we get here return world item
	if (const FSceneOutlinerTreeItemPtr* ParentItem = Items.Find(RepresentingWorld.Get()))
	{
		return *ParentItem;
	}
	else
	{
		return bCreate ? Mode->CreateItemFor<FWorldTreeItem>(RepresentingWorld.Get(), true) : nullptr;
	}
}

void FActorHierarchy::CreateComponentItems(const AActor* Actor, TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	check(Actor);
	// Add all this actors components if showing components and the owning actor was created
	if (bShowingComponents)
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (Component != nullptr)
			{
				if (FSceneOutlinerTreeItemPtr ComponentItem = Mode->CreateItemFor<FComponentTreeItem>(Component))
				{
					OutItems.Add(ComponentItem);
				}
			}
		}
	}
}

void FActorHierarchy::CreateWorldChildren(UWorld* World, TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	check(World);

	if (Mode->ShouldShowFolders())
	{
		// Add any folders which might match the current search terms
		FActorFolders::Get().ForEachFolder(*World, [this, &World, &OutItems](const FFolder& Folder)
		{
			if (FSceneOutlinerTreeItemPtr FolderItem = Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(Folder, World)))
			{
				OutItems.Add(FolderItem);
			}
			return true;
		});
	}
	
	const ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>();
	// Create all actor items
	for (FActorIterator ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		// If we are not showing LevelInstances, LevelInstance sub actor items should not be created unless they belong to a LevelInstance which is being edited
		if (LevelInstanceSubsystem)
		{
			if (const ALevelInstance* ParentLevelInstance = LevelInstanceSubsystem->GetParentLevelInstance(Actor))
			{
				if (!bShowingLevelInstances && !ParentLevelInstance->IsEditing())
				{
					continue;
				}
			}
		}
		
		if (FSceneOutlinerTreeItemPtr ActorItem = Mode->CreateItemFor<FActorTreeItem>(Actor))
		{
			if (bShowingOnlyActorWithValidComponents)
			{
				int32 InsertLocation = OutItems.Num();

				// Create all component items
				CreateComponentItems(Actor, OutItems);

				if (OutItems.Num() != InsertLocation)
				{
					// Add the actor before the components
					OutItems.Insert(ActorItem, InsertLocation);
				}
			}
			else
			{
				OutItems.Add(ActorItem);

				// Create all component items
				CreateComponentItems(Actor, OutItems);
			}

		}
	}

	if (bShowingUnloadedActors)
	{
		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			FWorldPartitionHelpers::ForEachActorDesc(WorldPartition, [this, WorldPartition, &OutItems](const FWorldPartitionActorDesc* ActorDesc)
			{
				if (ActorDesc != nullptr && !ActorDesc->IsLoaded(true))
				{
					if (const FSceneOutlinerTreeItemPtr ActorDescItem = Mode->CreateItemFor<FActorDescTreeItem>(FActorDescTreeItem(ActorDesc->GetGuid(), WorldPartition)))
					{
						OutItems.Add(ActorDescItem);
					}
				}
				return true;
			});
		}
	}
}

void FActorHierarchy::CreateItems(TArray<FSceneOutlinerTreeItemPtr>& OutItems) const
{
	if (RepresentingWorld.IsValid())
	{
		UWorld* RepresentingWorldPtr = RepresentingWorld.Get();
		check(RepresentingWorldPtr);
		if (FSceneOutlinerTreeItemPtr WorldItem = Mode->CreateItemFor<FWorldTreeItem>(RepresentingWorldPtr))
		{
			OutItems.Add(WorldItem);
		}
		// Create world children regardless of if a world item was created
		CreateWorldChildren(RepresentingWorldPtr, OutItems);
	}
}

void FActorHierarchy::CreateChildren(const FSceneOutlinerTreeItemPtr& Item, TArray<FSceneOutlinerTreeItemPtr>& OutChildren) const
{
	auto CreateChildrenFolders = [this](UWorld* InWorld, const FFolder& InParentFolder, const FFolder::FRootObject& InFolderRootObject, TArray<FSceneOutlinerTreeItemPtr>& OutChildren)
	{
		FActorFolders::Get().ForEachFolderWithRootObject(*InWorld, InFolderRootObject, [this, InWorld, &InParentFolder, &OutChildren](const FFolder& Folder)
		{
			if (Folder.IsChildOf(InParentFolder))
			{
				if (FSceneOutlinerTreeItemPtr NewFolderItem = Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(Folder, InWorld)))
				{
					OutChildren.Add(NewFolderItem);
				}
			}
			return true;
		});
	};

	UWorld* World = RepresentingWorld.Get();
	if (FWorldTreeItem* WorldItem = Item->CastTo<FWorldTreeItem>())
	{
		check(WorldItem->World == RepresentingWorld);
		CreateWorldChildren(WorldItem->World.Get(), OutChildren);
	}
	else if (const FActorTreeItem* ParentActorItem = Item->CastTo<FActorTreeItem>())
	{
		AActor* ParentActor = ParentActorItem->Actor.Get();
		check(ParentActor->GetWorld() == RepresentingWorld);

		CreateComponentItems(ParentActor, OutChildren);

		TArray<AActor*> ChildActors;

		if (const ALevelInstance* LevelInstanceParentActor = Cast<ALevelInstance>(ParentActor))
		{
			const ULevelInstanceSubsystem* LevelInstanceSubsystem = RepresentingWorld->GetSubsystem<ULevelInstanceSubsystem>();
			check(LevelInstanceSubsystem);

			LevelInstanceSubsystem->ForEachActorInLevelInstance(LevelInstanceParentActor, [this, LevelInstanceParentActor, LevelInstanceSubsystem, &ChildActors](AActor* SubActor)
			{
				const ALevelInstance* LevelInstanceActor = Cast<ALevelInstance>(SubActor);
				const bool bIsAnEditingLevelInstance = LevelInstanceActor ? LevelInstanceSubsystem->IsEditingLevelInstance(LevelInstanceActor) : false;
				if (bShowingLevelInstances || (LevelInstanceSubsystem->IsEditingLevelInstance(LevelInstanceParentActor) || bIsAnEditingLevelInstance))
				{
					ChildActors.Add(SubActor);
				}
				return true;
			});

			check(World == LevelInstanceParentActor->GetWorld());
			FFolder ParentFolder = LevelInstanceParentActor->GetFolder();
			CreateChildrenFolders(World, ParentFolder, LevelInstanceParentActor, OutChildren);
		}
		else
		{
			TFunction<bool(AActor*)> GetAttachedActors = [&ChildActors, &GetAttachedActors](AActor* Child)
			{
				ChildActors.Add(Child);
				Child->ForEachAttachedActors(GetAttachedActors);

				// Always continue
				return true;
			};

			// Grab all direct/indirect children of an actor
			ParentActor->ForEachAttachedActors(GetAttachedActors);
		}

		for (auto ChildActor : ChildActors)
		{
			if (FSceneOutlinerTreeItemPtr ChildActorItem = Mode->CreateItemFor<FActorTreeItem>(ChildActor))
			{
				OutChildren.Add(ChildActorItem);

				CreateComponentItems(ChildActor, OutChildren);
			}
		}
	}
	else if (FActorFolderTreeItem* FolderItem = Item->CastTo<FActorFolderTreeItem>())
	{
		check(Mode->ShouldShowFolders());
		
		check(World == FolderItem->World.Get());
		FFolder ParentFolder = FolderItem->GetFolder();
		check(!ParentFolder.IsNone());
		CreateChildrenFolders(World, ParentFolder, ParentFolder.GetRootObject(), OutChildren);
	}
}

void FActorHierarchy::FullRefreshEvent()
{
	FSceneOutlinerHierarchyChangedData EventData;
	EventData.Type = FSceneOutlinerHierarchyChangedData::FullRefresh;

	HierarchyChangedEvent.Broadcast(EventData);
}

void FActorHierarchy::OnWorldPartitionCreated(UWorld* InWorld)
{
	if (RepresentingWorld.Get() == InWorld)
	{
		FullRefreshEvent();
	}
}

void FActorHierarchy::OnLevelActorAdded(AActor* InActor)
{
	if (InActor != nullptr && RepresentingWorld.Get() == InActor->GetWorld())
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Added;
		EventData.Items.Add(Mode->CreateItemFor<FActorTreeItem>(InActor));
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelActorDeleted(AActor* InActor)
{
	if (InActor != nullptr && RepresentingWorld.Get() == InActor->GetWorld())
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
		EventData.ItemIDs.Add(InActor);
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelActorAttached(AActor* InActor, const AActor* InParent)
{
	if (InActor != nullptr && RepresentingWorld.Get() == InActor->GetWorld())
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Moved;
		EventData.ItemIDs.Add(InActor);
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelActorDetached(AActor* InActor, const AActor* InParent)
{
	if (InActor != nullptr && RepresentingWorld.Get() == InActor->GetWorld())
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Moved;
		EventData.ItemIDs.Add(InActor);
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLoadedActorAdded(AActor& InActor)
{
	OnLevelActorAdded(&InActor);

	FSceneOutlinerHierarchyChangedData EventData;
	EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
	EventData.ItemIDs.Add(InActor.GetActorGuid());
	HierarchyChangedEvent.Broadcast(EventData);
}

void FActorHierarchy::OnLoadedActorRemoved(AActor& InActor)
{
	OnLevelActorDeleted(&InActor);

	if (bShowingUnloadedActors)
	{
		if (UWorldPartition* WorldPartition = RepresentingWorld->GetWorldPartition())
		{
			const FGuid& ActorGuid = InActor.GetActorGuid();
			if (WorldPartition->GetActorDesc(ActorGuid) != nullptr)
			{
				FSceneOutlinerHierarchyChangedData EventData;
				EventData.Type = FSceneOutlinerHierarchyChangedData::Added;
				EventData.Items.Add(Mode->CreateItemFor<FActorDescTreeItem>(FActorDescTreeItem(ActorGuid, WorldPartition)));
				HierarchyChangedEvent.Broadcast(EventData);
			}
		}
	}
}

void FActorHierarchy::OnActorDescAdded(FWorldPartitionActorDesc* ActorDesc)
{
	if (bShowingUnloadedActors && ActorDesc && !ActorDesc->IsLoaded(true))
	{
		if (UWorldPartition* WorldPartition = RepresentingWorld->GetWorldPartition())
		{
			FSceneOutlinerHierarchyChangedData EventData;
			EventData.Type = FSceneOutlinerHierarchyChangedData::Added;
			EventData.Items.Add(Mode->CreateItemFor<FActorDescTreeItem>(FActorDescTreeItem(ActorDesc->GetGuid(), WorldPartition)));
			HierarchyChangedEvent.Broadcast(EventData);
		}
	}
}

void FActorHierarchy::OnActorDescRemoved(FWorldPartitionActorDesc* ActorDesc)
{
	if (bShowingUnloadedActors && ActorDesc)
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
		EventData.ItemIDs.Add(ActorDesc->GetGuid());
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnComponentsUpdated()
{
	FullRefreshEvent();
}

void FActorHierarchy::OnLevelActorListChanged()
{
	FullRefreshEvent();
}

void FActorHierarchy::OnActorFoldersUpdatedEvent(ULevel* InLevel)
{
	FullRefreshEvent();
}

void FActorHierarchy::OnActorFolderAdded(UActorFolder* InActorFolder)
{
	check(InActorFolder);
	ULevel* Level = InActorFolder->GetOuterULevel();
	if (Level && Mode->ShouldShowFolders() && (RepresentingWorld.Get() == Level->GetWorld()))
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Added;
		EventData.Items.Add(Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(InActorFolder->GetFolder(), RepresentingWorld)));
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelAdded(ULevel* InLevel, UWorld* InWorld)
{
	if (InLevel != nullptr && RepresentingWorld.Get() == InWorld)
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Added;

		EventData.Items.Reserve(InLevel->Actors.Num());
		for (AActor* Actor : InLevel->Actors)
		{
			if (Actor != nullptr)
			{
				EventData.Items.Add(Mode->CreateItemFor<FActorTreeItem>(Actor));
			}
		}
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelRemoved(ULevel* InLevel, UWorld* InWorld)
{
	if (InLevel != nullptr && RepresentingWorld.Get() == InWorld)
	{
		{
			FSceneOutlinerHierarchyChangedData EventData;
			EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
			EventData.ItemIDs.Reserve(InLevel->Actors.Num());
			for (AActor* Actor : InLevel->Actors)
			{
				if (Actor != nullptr)
				{
					EventData.ItemIDs.Add(Actor);
				}
			}
			HierarchyChangedEvent.Broadcast(EventData);
		}

		if (InLevel->IsUsingActorFolders())
		{
			FSceneOutlinerHierarchyChangedData EventData;
			EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
			EventData.ItemIDs.Add(InLevel);
			HierarchyChangedEvent.Broadcast(EventData);
		}
	}
}

/** Called when a folder is to be created */
void FActorHierarchy::OnBroadcastFolderCreate(UWorld& InWorld, const FFolder& InNewFolder)
{
	if (Mode->ShouldShowFolders() && RepresentingWorld.Get() == &InWorld)
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Added;
		EventData.Items.Add(Mode->CreateItemFor<FActorFolderTreeItem>(FActorFolderTreeItem(InNewFolder, &InWorld)));
		EventData.ItemActions = SceneOutliner::ENewItemAction::Select | SceneOutliner::ENewItemAction::Rename;
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

/** Called when a folder is to be moved */
void FActorHierarchy::OnBroadcastFolderMove(UWorld& InWorld, const FFolder& InOldFolder, const FFolder& InNewFolder)
{
	if (Mode->ShouldShowFolders() && RepresentingWorld.Get() == &InWorld)
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::FolderMoved;
		EventData.ItemIDs.Add(InOldFolder);
		EventData.NewPaths.Add(InNewFolder);
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

/** Called when a folder is to be deleted */
void FActorHierarchy::OnBroadcastFolderDelete(UWorld& InWorld, const FFolder& InFolder)
{
	if (Mode->ShouldShowFolders() && RepresentingWorld.Get() == &InWorld)
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Removed;
		EventData.ItemIDs.Add(InFolder);
		HierarchyChangedEvent.Broadcast(EventData);
	}
}

void FActorHierarchy::OnLevelActorFolderChanged(const AActor* InActor, FName OldPath)
{
	if (Mode->ShouldShowFolders() && RepresentingWorld.Get() == InActor->GetWorld())
	{
		FSceneOutlinerHierarchyChangedData EventData;
		EventData.Type = FSceneOutlinerHierarchyChangedData::Moved;
		EventData.ItemIDs.Add(FSceneOutlinerTreeItemID(InActor));
		HierarchyChangedEvent.Broadcast(EventData);
	}
}