// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "PersistenceManager.h"

#include "PersistenceComponent.h"
#include "PersistenceContainer.h"
#include "PersistenceUtils.h"
#include "SaveGameArchive.h"
#include "SaveGameProfile.h"
#include "SaveGameWorld.h"

#include "LevelScriptActorGunfire.h"

#include "Async/Async.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreaming.h"
#include "HAL/RunnableThread.h"
#include "Kismet/GameplayStatics.h"
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"
#include "Serialization/ArchiveLoadCompressedProxy.h"
#include "Serialization/ArchiveSaveCompressedProxy.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PersistenceManager)

DECLARE_CYCLE_STAT(TEXT("Commit Save"), STAT_PersistenceGunfire_CommitSave, STATGROUP_Persistence);
DECLARE_CYCLE_STAT(TEXT("Spawn Dynamic Actors"), STAT_PersistenceGunfire_SpawnDynamicActors, STATGROUP_Persistence);
DECLARE_CYCLE_STAT(TEXT("Process Cached Loads"), STAT_PersistenceGunfire_ProcessCachedLoads, STATGROUP_Persistence);
DECLARE_CYCLE_STAT(TEXT("Compress Save"), STAT_PersistenceGunfire_CompressSave, STATGROUP_Persistence);
DECLARE_CYCLE_STAT(TEXT("Decompress Save"), STAT_PersistenceGunfire_DecompressSave, STATGROUP_Persistence);

// Use different save names in PIE vs game, since on PC dev builds they'll output to the same spot
#if WITH_EDITOR
#define SAVE_PROFILE_NAME TEXT("editorprofile")
#define SAVE_SLOT_NAME TEXT("editorsave")
#else
#define SAVE_PROFILE_NAME TEXT("profile")
#define SAVE_SLOT_NAME TEXT("save")
#endif

// For debugging latency issues that only affect platforms with slow save systems
TAutoConsoleVariable<float> CVarPersistenceJobDelay(TEXT("SaveSystem.JobDelay"), 0.f, TEXT("If this is greater than zero, all async persistence jobs will be delayed for that many seconds"), ECVF_Cheat);
TAutoConsoleVariable<int32> CVarPersistenceDebug(TEXT("SaveSystem.Debug"), 0, TEXT(""), ECVF_Cheat);

// This version number is for changes to the persistence format at the top level.  The
// persistence containers have their own version, since they aren't guaranteed to be
// resaved each time the save game is (they may not be unpacked and repacked).  Bumping
// this doesn't cause savegames to be invalidated, it can be used for backwards compatible
// changes.
//
// Version History
// 1: Initial version
// 2: Reworked container format
// 3: Added build number
// 4: Added checksum (not backwards compatible)
// 5: Optimized the persistence archive by not always writing the full path to objects
// 6: Updated saved UE version from an int32 (UE4) to the FPackageFileVersion struct (UE5)
// 7: Switched to FTopLevelAssetPath for save game class reference
// 8: Stripped UE version from containers
// 9: Added compression to the final blob
static const int32 GUNFIRE_PERSISTENCE_VERSION = 9;

//////////////////////////////////////////////////////////////////////////////////////////

AActor* FPersistentReference::GetReference(UWorld* World)
{
	if (CachedActor)
	{
		return CachedActor;
	}

	if (Key.IsValid())
	{
		if (UPersistenceManager* Manager = UPersistenceManager::GetInstance(World))
		{
			return Manager->FindActorByKey(Key);
		}
	}

	return nullptr;
}

void FPersistentReference::SetReference(AActor* InActor)
{
	CachedActor = InActor;

	if (InActor)
	{
		if (UPersistenceManager* Manager = UPersistenceManager::GetInstance(InActor->GetWorld()))
		{
			Key = Manager->GetActorKey(InActor);
			return;
		}
	}

	// If we fall through, invalidate our key. This probably was a null reference, and so we should clear our data!
	Key = FPersistenceKey();
}

void FPersistentReference::CopyReferenceFrom(const FPersistentReference& OtherReference)
{
	CachedActor = OtherReference.CachedActor;
	Key = OtherReference.Key;
}

void FPersistentReference::ClearReference()
{
	CachedActor = nullptr;
	Key = FPersistenceKey();
}

///////////////////////////////////////////////////////////////////////////

FGetBuildNumber UPersistenceManager::GetBuildNumber;
FPersistenceUserMessage UPersistenceManager::UserMessage;

void UPersistenceManager::OutputUserMessage(const FString& Message, const UObject* ContextObject)
{
	if (UserMessage.IsBound())
	{
		UserMessage.Execute(Message, ContextObject);
	}
	else
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("%s"), *Message);
	}
}

UPersistenceManager::UPersistenceManager()
{
	ResetPersistence();

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ThisClass::OnLevelAddedToWorld);
		FWorldDelegates::OnPreWorldInitialization.AddUObject(this, &ThisClass::OnPreWorldInitialization);
		FWorldDelegates::PreLevelRemovedFromWorld.AddUObject(this, &ThisClass::OnLevelPreRemoveFromWorld);
		FWorldDelegates::LevelRemovedFromWorld.AddUObject(this, &ThisClass::OnLevelRemovedFromWorld);
		FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::OnWorldCleanup);
		FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &ThisClass::OnPreLoadMap);
		FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::OnPostLoadMap);

		// Special delegates that aren't part of the stock engine, see comment above
		FWorldDelegates::LevelPostLoad.AddUObject(this, &ThisClass::OnLevelPostLoad);
		FWorldDelegates::CanLevelActorsInitialize.AddUObject(this, &ThisClass::OnCanLevelActorsInitialize);
		FWorldDelegates::LevelActorsInitialized.AddUObject(this, &ThisClass::OnLevelActorsInitialized);

#if PLATFORM_XSX
		// On Xbox we should get the background event when the app is suspended, which is a
		// good time to save.
		FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &ThisClass::OnSuspend);
#elif PLATFORM_PS5
		// PlayStation doesn't have an event for when the app is suspended, so trigger a
		// save any time it goes into the background (ie, they hit the PS button).
		// If they hold the PS button and pick close application from the quick menu
		// there's nothing we can do though, they'll lose any unsaved progress.
		FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &ThisClass::OnSuspend);
#endif

		ThreadHasWork = FGenericPlatformProcess::GetSynchEventFromPool();
		Thread = FRunnableThread::Create(this, TEXT("PersistenceManager"), 128 * 1024);

		// Add any levels that loaded before we were initialized (should just be the persistent level)
		for (ULevel* Level : GetWorld()->GetLevels())
		{
			CachedLoads.Add(Level);
		}

		// If our save profile or game classes aren't loaded put in a high priority
		// request for them, since we'll need them soon.
		const UGunfireSaveSystemSettings* Settings = GetDefault<UGunfireSaveSystemSettings>();

		TArray<FSoftObjectPath> SaveClasses;
		SaveClasses.Reserve(2);

		if (!Settings->SaveProfileClass.IsNull())
		{
			SaveClasses.Add(Settings->SaveProfileClass.ToSoftObjectPath());
		}

		if (!Settings->SaveGameClass.IsNull())
		{
			SaveClasses.Add(Settings->SaveGameClass.ToSoftObjectPath());
		}

		if (SaveClasses.Num() > 0)
		{
			UAssetManager::GetStreamableManager().RequestAsyncLoad(SaveClasses, FStreamableDelegate(), FStreamableManager::AsyncLoadHighPriority);
		}

#if WITH_EDITOR
		EditorInit();
#endif
	}
}

void UPersistenceManager::BeginDestroy()
{
	Super::BeginDestroy();

	for (FThreadJob* Job : QueuedJobs)
	{
		Job->AsyncLoad->CancelHandle();
	}
	QueuedJobs.Empty();

	if (Thread)
	{
		ThreadShouldStop = true;
		ThreadHasWork->Trigger();
		Thread->WaitForCompletion();

		FScopeLock Lock(&ThreadJobsLock);
		for (FThreadJob* Job : ThreadJobs)
		{
			delete Job;
		}
		ThreadJobs.Empty();

		delete Thread;
		Thread = nullptr;
	}

	if (ThreadHasWork)
	{
		FGenericPlatformProcess::ReturnSynchEventToPool(ThreadHasWork);
		ThreadHasWork = nullptr;
	}
}

void UPersistenceManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Process cached loads immediately to ensure we are ready as soon as possible for loading objects (i.e. PIE)
	ProcessCachedLoads();
}

void UPersistenceManager::ResetPersistence()
{
	UE_LOG(LogGunfireSaveSystem, Display, TEXT("Resetting persistence, there is no active save now"));

	CurrentSlot = -1;
	CurrentData = nullptr;
}

void UPersistenceManager::LoadProfileSave(FLoadSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::LoadProfile;
	Job->LoadCallback = Callback;
	QueueJob(Job);
}

void UPersistenceManager::LoadProfileSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result)
{
	if (Result == EPersistenceLoadResult::Success)
	{
		UserProfile = Cast<USaveGameProfile>(ReadSave(Job.ProfileData, &Result));
	}
	else if (Result == EPersistenceLoadResult::DoesNotExist)
	{
		UserProfile = CreateSaveProfile();
	}

	Job.LoadCallback.ExecuteIfBound(Result, UserProfile);
	OnLoadProfile.Broadcast(Result, UserProfile);
}

void UPersistenceManager::DeleteProfileSave(FDeleteSaveComplete Callback)
{
	UserProfile = nullptr;

	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::DeleteProfile;
	Job->DeleteCallback = Callback;
	QueueJob(Job);
}

void UPersistenceManager::DeleteProfileSaveDone(const FThreadJob& Job, bool Result)
{
	Job.DeleteCallback.ExecuteIfBound(Result);
}

void UPersistenceManager::LoadSave(int32 Slot, FLoadSaveComplete Callback)
{
	if (CurrentSlot != Slot)
	{
		UE_LOG(LogGunfireSaveSystem, Display, TEXT("Loading save in slot %d"), Slot);
	}

	CurrentData = nullptr;
	CurrentSlot = Slot;

	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::LoadSlot;
	Job->LoadCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::LoadSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result)
{
	if (Result == EPersistenceLoadResult::Success)
	{
		CurrentData = Cast<USaveGameWorld>(ReadSave(Job.WorldData, &Result));
	}
	else if (Result == EPersistenceLoadResult::DoesNotExist)
	{
		CurrentData = CreateSaveGame();
	}

	Job.LoadCallback.ExecuteIfBound(Result, CurrentData);
	OnLoadGame.Broadcast(Result, CurrentData);
}

void UPersistenceManager::ReadSave(int32 Slot, FLoadSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::ReadSlot;
	Job->LoadCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::ReadSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result)
{
	USaveGameWorld* SaveGame = nullptr;

	if (Result == EPersistenceLoadResult::Success)
	{
		SaveGame = Cast<USaveGameWorld>(ReadSave(Job.WorldData, &Result));
	}

	Job.LoadCallback.ExecuteIfBound(Result, SaveGame);
}

void UPersistenceManager::HasSave(int32 Slot, FHasSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::HasSlot;
	Job->HasCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::HasSaveDone(const FThreadJob& Job, EPersistenceHasResult Result)
{
	Job.HasCallback.ExecuteIfBound(Result);
}

void UPersistenceManager::DeleteSave(int32 Slot, FDeleteSaveComplete Callback)
{
	// If this is the save game we are working with, reset
	if (CurrentSlot == Slot)
	{
		ResetPersistence();
	}

	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::DeleteSlot;
	Job->DeleteCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::DeleteSaveDone(const FThreadJob& Job, bool Result)
{
	Job.DeleteCallback.ExecuteIfBound(Result);
	OnDeleteGame.Broadcast(Result);
}

void UPersistenceManager::CommitSave(const FString& Reason, FCommitSaveComplete Callback)
{
	check(IsInGameThread());

	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_CommitSave);

	if (bNeverCommit)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Committing saves is disabled for this play session.  Restart the application to reset."));
		Callback.ExecuteIfBound(EPersistenceSaveResult::Disabled);
		return;
	}

	if (bDisableCommit)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Committing save while saving is disabled, ignoring latest commit"));
		Callback.ExecuteIfBound(EPersistenceSaveResult::Disabled);
		return;
	}

	UE_CLOG(NumSavesPending > 0, LogGunfireSaveSystem, Warning, TEXT("Committing save while %d save(s) still pending, queueing"), NumSavesPending);

	// If IsEngineExitRequested returns true we've already had EndPlay called on actors,
	// so we can potentially get incomplete save data.  Ignore the save in this case,
	// something should have already committed a save earlier than this.
	if (IsEngineExitRequested())
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Ignoring CommitSave during shutdown"));
		Callback.ExecuteIfBound(EPersistenceSaveResult::Disabled);
		return;
	}

	++NumSavesPending;

	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::Commit;
	Job->SaveCallback = Callback;

	OnPreSaveGame.Broadcast();

	if (CVarPersistenceDebug.GetValueOnGameThread() > 0)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::White, FString::Printf(TEXT("Commit save: %s"), *Reason));
	}

	UE_LOG(LogGunfireSaveSystem, Log, TEXT("Beginning commit save: %s"), *Reason);

	if (CurrentData != nullptr)
	{
		// Let blueprint do any pre-commit updates to the data
		CurrentData->PreCommit(this);
		CurrentData->PreCommitNative(this);

		TArray<FName, TInlineAllocator<16>> EmptyContainers;

		// Go through all currently in use containers and have their actors write their
		// latest save data.
		for (auto It = RegisteredActors.CreateIterator(); It; ++It)
		{
			const FName& ContainerName = It.Key();
			TArray<TWeakObjectPtr<UPersistenceComponent>>& Components = It.Value();

			UPersistenceContainer* Container = GetContainer(ContainerName, false);

			// If we've got registered components for this container, write them out
			// now.  It's possible to have a container with nothing to save if all the
			// actors using it were moved to another container, or they were deleted
			// and don't persist being destroyed.
			if (Components.Num() > 0 || (Container && Container->HasDestroyed()))
			{
				if (!Container)
				{
					Container = GetContainer(ContainerName, true);
				}

				Container->WriteData(Components, *this);
			}
			// If a container is unused, don't bother writing anything for it and flag
			// it for deletion.
			else
			{
				EmptyContainers.Add(ContainerName);
			}
		}

		// Now that we're done writing, remove any empty containers.
		for (const FName& ContainerName : EmptyContainers)
		{
			if (DeleteContainer(ContainerName, false))
			{
				UE_LOG(LogGunfireSaveSystem, Log, TEXT("Deleting container '%s' because it's unused"), *ContainerName.ToString());
			}
		}

		// Only allow the server to write out save games.
		UWorld* World = GetGameInstance()->GetWorld();
		if (CurrentData && World != nullptr && !World->IsNetMode(NM_Client))
		{
			if (CurrentSlot >= 0)
			{
				Job->Slot = CurrentSlot;
				WriteSave(CurrentData, Job->WorldData);
			}
			else
			{
				UE_LOG(LogGunfireSaveSystem, Log, TEXT("CommitSave CurrentSlot == -1  Bypassing WriteSave()"));
			}
		}
	}

	if (UserProfile)
	{
		UserProfile->PreCommit(this);
		UserProfile->PreCommitNative(this);

		// If we have profile data, always save it along with the world, 
		// even if you are a client connected to a servers game.
		WriteSave(UserProfile, Job->ProfileData);
	}

	UE_LOG(LogGunfireSaveSystem, Log, TEXT("Commit save done, pushing to thread"));

	QueueJob(Job);
}

void UPersistenceManager::CommitSaveDone(const FThreadJob& Job, EPersistenceSaveResult Result)
{
	check(IsInGameThread());

	--NumSavesPending;

	Job.SaveCallback.ExecuteIfBound(Result);
	OnSaveGame.Broadcast(Result);
}

void UPersistenceManager::CommitSaveToSlot(int32 Slot, FCommitSaveComplete Callback)
{
	if (CurrentSlot != Slot)
	{
		UE_LOG(LogGunfireSaveSystem, Display, TEXT("Changing current slot from %d to %d on commit"), CurrentSlot, Slot);
	}

	CurrentSlot = Slot;

	CommitSave(TEXT("Setting Slot"), Callback);
}

void UPersistenceManager::HasProfileBackup(FDeleteSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::HasProfileBackup;
	Job->DeleteCallback = Callback;
	QueueJob(Job);
}

void UPersistenceManager::RestoreProfileBackup(FDeleteSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::RestoreProfileBackup;
	Job->DeleteCallback = Callback;
	QueueJob(Job);
}

void UPersistenceManager::HasSlotBackup(int32 Slot, FDeleteSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::HasSlotBackup;
	Job->DeleteCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::RestoreSlotBackup(int32 Slot, FDeleteSaveComplete Callback)
{
	FThreadJob* Job = new FThreadJob;
	Job->Type = EJobType::RestoreSlotBackup;
	Job->DeleteCallback = Callback;
	Job->Slot = Slot;
	QueueJob(Job);
}

void UPersistenceManager::BackupOperationDone(const FThreadJob& Job, bool Result)
{
	Job.DeleteCallback.ExecuteIfBound(Result);
}

void UPersistenceManager::SetDisableCommit(bool DisableCommit)
{
	UE_LOG(LogGunfireSaveSystem, Display, TEXT("%s commit"), DisableCommit ? TEXT("Disabling") : TEXT("Enabling"));
	bDisableCommit = DisableCommit;
}

USaveGameWorld* UPersistenceManager::CreateSaveGame()
{
	const UGunfireSaveSystemSettings* Settings = GetDefault<UGunfireSaveSystemSettings>();

	TSubclassOf<USaveGameWorld> SaveGameClass = Settings->SaveGameClass.Get();

	// If the class wasn't already loaded, do so now
	// TODO: this could be streamed on subsystem initialize, and only load synchronously if it didn't have enough time to finish.
	if (SaveGameClass == nullptr && !Settings->SaveGameClass.IsNull())
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save Game Class not loaded during save game creation, loading it synchronously!"));
		SaveGameClass = LoadObject<UClass>(nullptr, *Settings->SaveGameClass.ToString());
	}

	// A class wasn't specified, just use the base
	if (SaveGameClass == nullptr)
	{
		SaveGameClass = USaveGameWorld::StaticClass();
	}

	return NewObject<USaveGameWorld>(GetTransientPackage(), SaveGameClass);
}

USaveGameProfile* UPersistenceManager::CreateSaveProfile()
{
	const UGunfireSaveSystemSettings* Settings = GetDefault<UGunfireSaveSystemSettings>();

	TSubclassOf<USaveGameProfile> SaveProfileClass = Settings->SaveProfileClass.Get();

	// If the class wasn't already loaded, do so now
	if (SaveProfileClass == nullptr && !Settings->SaveProfileClass.IsNull())
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save Profile Class not loaded during profile creation, loading it synchronously!"));
		SaveProfileClass = Settings->SaveProfileClass.LoadSynchronous();
	}

	// A save profile has no purpose if there isn't a derived class that has added data,
	// so log a warning and return null if it's requested in that case.
	if (SaveProfileClass == nullptr)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save Profile Class not specified, profile will be null"));
		return nullptr;
	}

	return NewObject<USaveGameProfile>(GetTransientPackage(), SaveProfileClass);
}

void UPersistenceManager::DeleteContainers(const FString& ContainerName, bool SubstringMatch)
{
	if (CurrentData != nullptr)
	{
		if (SubstringMatch)
		{
			FString CurContainerName;

			for (int i = CurrentData->Containers.Num() - 1; i >= 0; i--)
			{
				UPersistenceContainer* Container = CurrentData->Containers[i];

				Container->GetKey().ToString(CurContainerName);

				if (SubstringMatch ? CurContainerName.Contains(ContainerName) : ContainerName == CurContainerName)
				{
					UE_LOG(LogGunfireSaveSystem, Log, TEXT("Deleting container '%s' based on request '%s'"),
						*Container->GetKey().ToString(), *ContainerName);

					DeleteContainer(Container->GetKey(), true);
				}
			}
		}
		else
		{
			UE_LOG(LogGunfireSaveSystem, Log, TEXT("Deleting container '%s'"), *ContainerName);

			DeleteContainer(FName(*ContainerName), true);
		}
	}
}

FPersistenceKey UPersistenceManager::GetActorKey(AActor* Actor) const
{
	FPersistenceKey Key;
	Key.PersistentID = UPersistenceComponent::INVALID_UID;

	if (Actor)
	{
		if (UPersistenceComponent* Component = Actor->FindComponentByClass<UPersistenceComponent>())
		{
			Key.ContainerKey = GetContainerKey(Component);
			Key.PersistentID = Component->UniqueID;
		}
	}

	return Key;
}

AActor* UPersistenceManager::FindActorByKey(FPersistenceKey Key) const
{
	if (const TArray<TWeakObjectPtr<UPersistenceComponent>>* Components = RegisteredActors.Find(Key.ContainerKey))
	{
		for (const TWeakObjectPtr<UPersistenceComponent>& Component : *Components)
		{
			const UPersistenceComponent* RawComponent = Component.Get();
			if (RawComponent->UniqueID == Key.PersistentID)
			{
				return RawComponent->GetOwner();
			}
		}
	}

	return nullptr;
}

void UPersistenceManager::Register(UPersistenceComponent* pComponent)
{
#if !UE_BUILD_SHIPPING
	TInlineComponentArray<UPersistenceComponent*> PersistenceComponents(pComponent->GetOwner());

	if (PersistenceComponents.Num() > 1)
	{
		OutputUserMessage(
			TEXT("Persistent actor has more than one persistence component."),
			pComponent->GetOwner());
	}
#endif

	const FName& ContainerKey = GetContainerKey(pComponent);

	TArray<TWeakObjectPtr<UPersistenceComponent>>& Container = RegisteredActors.FindOrAdd(ContainerKey);
	
	Container.AddUnique(pComponent);

#if !UE_BUILD_SHIPPING
	if (!pComponent->SaveKey.IsNone() && Container.Num() > 1)
	{
		OutputUserMessage(
			FString::Printf(TEXT("More than one actor is using the save key '%s' (other is %s)"),
				*pComponent->SaveKey.ToString(),
				*Container[0]->GetOwner()->GetFullName()),
			pComponent->GetOwner());
	}
#endif
}

void UPersistenceManager::Unregister(UPersistenceComponent* pComponent, ULevel* OverrideLevel)
{
	FName ContainerKey;

	if (OverrideLevel)
	{
		const FName* OverrideContainerKey = LoadedLevels.Find(OverrideLevel);
		if (ensure(OverrideContainerKey))
			ContainerKey = *OverrideContainerKey;
	}
	else
	{
		ContainerKey = GetContainerKey(pComponent);
	}

	if (TArray<TWeakObjectPtr<UPersistenceComponent>>* Components = RegisteredActors.Find(ContainerKey))
	{
		int NumRemoved = Components->Remove(pComponent);

		if (!pComponent->SaveKey.IsNone())
		{
			// If this component uses a save key there will never be a level unload to
			// clear the registered actor and pack the container, so go ahead and do it now.
			PackContainer(ContainerKey);
		}

#if UE_BUILD_DEBUGGAME
		if (NumRemoved == 0)
		{
			bool FoundInOtherContainer = false;

			for (TPair<FName, TArray<TWeakObjectPtr<UPersistenceComponent>>>& Iter : RegisteredActors)
			{
				if (Iter.Value.Contains(pComponent))
				{
					FoundInOtherContainer = true;

					UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Unregistering persistence component on actor %s from container %s when it's actually in %s"),
						*pComponent->GetOwner()->GetName(),
						*ContainerKey.ToString(),
						*Iter.Key.ToString());

					break;
				}
			}

			if (!FoundInOtherContainer)
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Unregistering persistence component for actor %s that wasn't registered (container %s)"),
					*pComponent->GetOwner()->GetName(),
					*ContainerKey.ToString());
			}
		}
#endif
	}
}

FString UPersistenceManager::GetSlotName(int32 Slot)
{
	return FString::Printf(TEXT("%s_%d"), SAVE_SLOT_NAME, Slot);
}

UPersistenceContainer* UPersistenceManager::GetContainer(const UPersistenceComponent* Component)
{
	UPersistenceContainer* Container = nullptr;

	if (!Component->GetWorld()->IsNetMode(NM_Client))
	{
		FName ContainerKey = GetContainerKey(Component);
		if (ContainerKey == NAME_None)
		{
			// Due to a ULevelStreaming living longer than its child ULevel, 
			// we can encounter timing issues in World Composition levels when a 
			// level is rapidly toggled to be loaded and then unloaded. 
			ULevel* NewLevel = Component->GetComponentLevel();
			if (IsValid(Component) && NewLevel != nullptr)
			{
				UE_LOG(LogGunfireSaveSystem, Warning,
					TEXT("UPersistenceManager - Encountering level '%s', containing actor '%s', not previously hit by ULevel::OnLevelLoaded"),
					*Component->GetComponentLevel()->GetLevelScriptActor()->GetName(),
					*Component->GetOwner()->GetName());

				LoadedLevels.Emplace(NewLevel, FName(*Component->GetComponentLevel()->GetPathName()));
			}
		}

		Container = GetContainer(ContainerKey, false);

		// If this is a save key container we won't have gotten a level load event to unpack it,
		// so go ahead and do it now.
		if (Container && !Component->SaveKey.IsNone() && Container->IsPacked())
		{
			UE_LOG(LogGunfireSaveSystem, Log, TEXT("UPersistenceManager - Forcing unpack for container '%s'"), *Container->GetKey().ToString());

			Container->Unpack();
		}
	}

	return Container;
}

void UPersistenceManager::SetComponentDestroyed(UPersistenceComponent* Component)
{
	if (UPersistenceContainer* Container = GetContainer(GetContainerKey(Component), true))
	{
		Container->SetDestroyed(Component);
	}
}

void UPersistenceManager::WriteComponent(UPersistenceComponent* Component)
{
	if (!Component->GetWorld()->IsNetMode(NM_Client))
	{
		if (ensure(!Component->SaveKey.IsNone()))
		{
			if (UPersistenceContainer* Container = GetContainer(GetContainerKey(Component), true))
			{
				TArray<TWeakObjectPtr<UPersistenceComponent>, TInlineAllocator<1>> Array;
				Array.Emplace(Component);

				Container->WriteData(Array, *this);
			}
		}
	}
}

void UPersistenceManager::OnLevelChanged(UPersistenceComponent* pComponent, ULevel* OldLevel)
{
	// First, check if we should persist and don't have a save key.  If we've got a save
	// key we go into a special container, so we don't care what level we're in.
	if (pComponent->ShouldPersist() && pComponent->SaveKey.IsNone())
	{
		// Dynamic unique ID's are unique to the entire savegame, so we don't need to
		// generate a new one.  We don't support moving static persistent data though.
		if (!pComponent->IsDynamic)
		{
			OutputUserMessage(
				*FString::Printf(TEXT("Persistent actor in level %s is static, it needs to be dynamically spawned."), *OldLevel->GetLevelScriptActor()->GetName()),
				pComponent->GetOwner());

			return;
		}

		// If the container is packed it's not going to be saved again after this actor
		// has been removed, which means it'll be duped next time we load that level.
		// If this ever gets hit the solution is probably to get the actor moved over to
		// the new level sooner.
		if (const FName* LevelKey = LoadedLevels.Find(OldLevel))
		{
			if (UPersistenceContainer* Container = GetContainer(*LevelKey, false))
			{
				ensure(!Container->IsPacked());
			}
		}

		// Unregister the component from the old container
		Unregister(pComponent, OldLevel);

		// Re-register the component to add it to the new container
		Register(pComponent);
	}
}

bool UPersistenceManager::HasSpawnedDynamicActorsForContainer(const FName& Name)
{
	if (UPersistenceContainer* Container = GetContainer(Name, false))
	{
		return Container->HasSpawnedDynamicActors();
	}

	return false;
}

void UPersistenceManager::ToBinary(UObject* Object, TArray<uint8>& ObjectBytes)
{
	FMemoryWriter MemoryWriter(ObjectBytes, true);

	FPackageFileVersion PackageFileUEVersion = GPackageFileUEVersion;
	MemoryWriter << PackageFileUEVersion;

	FSaveGameArchive Ar(MemoryWriter);
	Ar.WriteBaseObject(Object, ClassCache);
}

void UPersistenceManager::FromBinary(UObject* Object, const TArray<uint8>& ObjectBytes)
{
	if (ObjectBytes.Num() == 0)
		return;

	FMemoryReader MemoryReader(ObjectBytes, true);

	FPackageFileVersion PackageFileUEVersion;
	MemoryReader << PackageFileUEVersion;

	// If the UE5 version is incorrect then assume we have saved data from the deprecated
	// UE4 version type which was just an int32. Repair the state of the reader by backing
	// up 4 bytes before continuing.
	if ((uint32)PackageFileUEVersion.FileVersionUE5 < (uint32)EUnrealEngineObjectUE5Version::INITIAL_VERSION ||
		(uint32)PackageFileUEVersion.FileVersionUE5 > (uint32)EUnrealEngineObjectUE5Version::AUTOMATIC_VERSION)
	{
		PackageFileUEVersion.FileVersionUE5 = 0;
		int64 FixedReaderPos = MemoryReader.Tell() - 4;
		MemoryReader.Seek(FixedReaderPos);
	}

	FSaveGameArchive Ar(MemoryReader);
	Ar.ReadBaseObject(Object);
}

// Convert the save game to a binary format. 
void UPersistenceManager::WriteSave(USaveGame* SaveGame, TArray<uint8>& SaveBlob)
{
	SaveBlob.Reset();

	FMemoryWriter MemoryWriter(SaveBlob, true);

	// The CRC and size are the first thing in the save, stub out the space for them.
	uint32 CRC = 0;
	int32 Size = 0;
	MemoryWriter << CRC;
	MemoryWriter << Size;

	// Write version for this file format
	int32 SavegameFileVersion = GUNFIRE_PERSISTENCE_VERSION;
	MemoryWriter << SavegameFileVersion;

	// Write out the build number
	int32 BuildNumber = 0;
	if (GetBuildNumber.IsBound())
	{
		BuildNumber = GetBuildNumber.Execute();
	}
	MemoryWriter << BuildNumber;

	// Write out engine and UE version information
	FPackageFileVersion PackageFileUEVersion = GPackageFileUEVersion;
	MemoryWriter << PackageFileUEVersion;

	// Write the class path so we know what class to load
	FTopLevelAssetPath SaveGameClassPath = SaveGame->GetClass()->GetClassPathName();
	MemoryWriter << SaveGameClassPath;

	FSaveGameArchive Ar(MemoryWriter);
	Ar.WriteBaseObject(SaveGame, ClassCache);

	// Write out the actual size before we calculate the checksum
	MemoryWriter.Seek(4);
	Size = SaveBlob.Num();

	// Seek back to the start and rewrite the checksum with the actual value
	MemoryWriter << Size;
	MemoryWriter.Seek(0);
	CRC = FCrc::MemCrc32(SaveBlob.GetData() + 4, SaveBlob.Num() - 4);
	MemoryWriter << CRC;
}

bool UPersistenceManager::InitSaveArchive(FArchive& Archive, const TArray<uint8>& SaveBlob, FTopLevelAssetPath& SaveGameClassPath, EPersistenceLoadResult* Result)
{
	// Read the CRC from the start of the save
	uint32 SavedCRC = 0;
	Archive << SavedCRC;

	int32 SavedSize = 0;
	Archive << SavedSize;

	// Some platforms will return extra padding bytes on load, so we write out the actual
	// size we wrote.  If it's greater than the amount of data read in or less than the
	// minimum size it must be corrupt though.
	if (SavedSize > SaveBlob.Num() || SavedSize <= 8)
	{
		if (Result)
		{
			*Result = EPersistenceLoadResult::Corrupt;
		}

		return false;
	}

	uint32 CalculatedCRC = FCrc::MemCrc32(SaveBlob.GetData() + 4, SavedSize - 4);

	if (CalculatedCRC != SavedCRC)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save CRC didn't match (saved: 0x%x, calculated: 0x%x), refusing to load"), SavedCRC, CalculatedCRC);

		if (Result)
		{
			*Result = EPersistenceLoadResult::Corrupt;
		}

		return false;
	}

	int32 SavegameFileVersion;
	Archive << SavegameFileVersion;

	if (SavegameFileVersion > GUNFIRE_PERSISTENCE_VERSION)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save version is %d, ours is %d, refusing to load"), SavegameFileVersion, GUNFIRE_PERSISTENCE_VERSION);

		if (Result)
		{
			*Result = EPersistenceLoadResult::TooNew;
		}

		return false;
	}

	int32 BuildNumber;
	Archive << BuildNumber;

	int32 CurrentBuildNumber = 0;
	if (GetBuildNumber.IsBound())
	{
		CurrentBuildNumber = GetBuildNumber.Execute();
	}

	// Just because the build number is newer doesn't mean anything in the save format has
	// changed, but to be safe we won't load it.
	if (CurrentBuildNumber != 0 && BuildNumber > CurrentBuildNumber)
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save build number is %d, ours is %d, refusing to load"), BuildNumber, CurrentBuildNumber);

		if (Result)
		{
			*Result = EPersistenceLoadResult::TooNew;
		}

		return false;
	}

	// Unreal 5 has changed the archive version to be a struct. For previous saves, we
	// convert the old integer to this new struct.
	FPackageFileVersion SavedUEVersion;
	if (SavegameFileVersion < 6)
	{
		int32 SavedUE4Version;
		Archive << SavedUE4Version;
		SavedUEVersion = FPackageFileVersion::CreateUE4Version((EUnrealEngineObjectUE4Version)SavedUE4Version);
	}
	else
	{
		Archive << SavedUEVersion;
	}

	Archive.SetUEVer(SavedUEVersion);

	// Get the class path
	if (SavegameFileVersion >= 7)
	{
		Archive << SaveGameClassPath;
	}
	else
	{
		FString OldClassPath;
		Archive << OldClassPath;
		SaveGameClassPath.TrySetPath(OldClassPath);
	}

	return true;
}

bool UPersistenceManager::PreloadSave(FThreadJob& Job, const TArray<uint8>& SaveBlob)
{
	if (SaveBlob.Num() == 0)
	{
		return false;
	}

	// Load raw data from memory
	FMemoryReader MemoryReader(SaveBlob, true);

	FTopLevelAssetPath SaveGameClassPath;

	if (!InitSaveArchive(MemoryReader, SaveBlob, SaveGameClassPath, nullptr))
	{
		return false;
	}

	TArray<FSoftObjectPath> ClassesToLoad;

	UClass* SaveGameClass = FindObject<UClass>(SaveGameClassPath);
	if (SaveGameClass == nullptr)
	{
		ClassesToLoad.Add(FSoftObjectPath(SaveGameClassPath));
	}

	FSaveGameArchive Ar(MemoryReader);
	Ar.GetClassesToLoad(ClassesToLoad);

	if (ClassesToLoad.Num() > 0)
	{
		Job.AsyncLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(
			ClassesToLoad,
			FStreamableDelegate::CreateUObject(this, &ThisClass::OnSaveClassesLoaded, &Job),
			FStreamableManager::AsyncLoadHighPriority);
	}

	return true;
}

void UPersistenceManager::OnSaveClassesLoaded(FThreadJob* Job)
{
	QueuedJobs.Remove(Job);

	// We finished loading the classes for this save, so requeue the job so it can finish.
	AsyncTask(ENamedThreads::GameThread, [Job]()
	{
		if (UPersistenceManager* ThisPtr = Job->Manager.Get())
		{
			if (Job->Type == EJobType::LoadSlot)
			{
				ThisPtr->LoadSaveDone(*Job, EPersistenceLoadResult::Success);
			}
			else if (Job->Type == EJobType::LoadProfile)
			{
				ThisPtr->LoadProfileSaveDone(*Job, EPersistenceLoadResult::Success);
			}
			else if (Job->Type == EJobType::ReadSlot)
			{
				ThisPtr->ReadSaveDone(*Job, EPersistenceLoadResult::Success);
			}
		}

		FreeThreadJob(Job);
	});
}

void UPersistenceManager::CompressData(TArray<uint8>& SaveBlob)
{
	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_CompressSave);

	// Copy header data (up to savegame version so we can determine if its compressed)
	TArray<uint8> HeaderBlob;
	HeaderBlob.Append(SaveBlob.GetData(), 12);

	// Remove the header so that our buffer is 100% compressed
	TArray<uint8> UncompressedBlob = SaveBlob;
	UncompressedBlob.RemoveAt(0, 12);

	// Apply Zlib decompression
	SaveBlob.Reset();
	FArchiveSaveCompressedProxy Compressor = FArchiveSaveCompressedProxy(SaveBlob, NAME_Zlib);

	int32 UncompressedSize = UncompressedBlob.Num();
	Compressor.Serialize(&UncompressedSize, 4);

	Compressor.Serialize(UncompressedBlob.GetData(), UncompressedSize);
	Compressor.Flush();

	// Add the header data back
	SaveBlob.Insert(HeaderBlob, 0);
	SaveBlob.Shrink();
}

bool UPersistenceManager::DecompressData(TArray<uint8>& SaveBlob)
{
	if (SaveBlob.Num() < 12)
	{
		return false;
	}

	FMemoryReader MemoryReader(SaveBlob, true);
	MemoryReader.Seek(8);

	int32 SavegameFileVersion;
	MemoryReader << SavegameFileVersion;

	// Is this a compressed save?
	if (SavegameFileVersion <= 8)
	{
		return true;
	}

	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_DecompressSave);

	// Copy header data (up to savegame version so we can determine if its compressed)
	TArray<uint8> HeaderBlob;
	HeaderBlob.Append(SaveBlob.GetData(), 12);

	// Remove the header so that our buffer is 100% compressed
	TArray<uint8> CompressedBlob = SaveBlob;
	CompressedBlob.RemoveAt(0, 12);

	// Apply Zlib compression
	SaveBlob.Reset();
	FArchiveLoadCompressedProxy Decompresser(CompressedBlob, NAME_Zlib);

	if (Decompresser.IsError())
	{
		return false;
	}

	int32 UncompressedSize = 0;
	Decompresser.Serialize(&UncompressedSize, 4);

	SaveBlob.Reserve(UncompressedSize + HeaderBlob.Num());
	SaveBlob.SetNum(UncompressedSize);

	Decompresser.Serialize(SaveBlob.GetData(), UncompressedSize);
	Decompresser.Flush();

	// Add the header data back
	SaveBlob.Insert(HeaderBlob, 0);
	SaveBlob.Shrink();

	return true;
}

USaveGame* UPersistenceManager::ReadSave(const TArray<uint8>& SaveBlob, EPersistenceLoadResult* Result)
{
	if (SaveBlob.Num() == 0)
	{
		return nullptr;
	}

	// Load raw data from memory
	FMemoryReader MemoryReader(SaveBlob, true);

	FTopLevelAssetPath SaveGameClassPath;

	if (!InitSaveArchive(MemoryReader, SaveBlob, SaveGameClassPath, Result))
	{
		return nullptr;
	}

	// Try to find it, and failing that, load it
	UClass* SaveGameClass = FindObject<UClass>(SaveGameClassPath);
	if (SaveGameClass == nullptr)
	{
		SaveGameClass = LoadObject<UClass>(nullptr, *SaveGameClassPath.ToString());
	}

	// If we have a class, try to load it.
	if (SaveGameClass != nullptr)
	{
		USaveGame* SaveGame = NewObject<USaveGame>(GetTransientPackage(), SaveGameClass);

		FSaveGameArchive Ar(MemoryReader);
		Ar.ReadBaseObject(SaveGame);

		if (Result)
		{
			*Result = EPersistenceLoadResult::Success;
		}

		return SaveGame;
	}
	else
	{
		UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Save game class couldn't be found: %s"), *SaveGameClassPath.ToString());

		if (Result)
		{
			*Result = EPersistenceLoadResult::Corrupt;
		}

		return nullptr;
	}
}

UPersistenceContainer* UPersistenceManager::GetContainer(const FName& Name, bool CreateIfMissing)
{
	if (CurrentData != nullptr)
	{
		for (UPersistenceContainer* Container : CurrentData->Containers)
		{
			// Encountering a null container originating from an old / invalid save.  
			if (Container == nullptr)
			{
				UE_LOG(LogGunfireSaveSystem, Error, TEXT("Null container encountered in current save data: '%s'"), *Name.ToString());
				continue;
			}

			if (Container->GetKey() == Name)
			{
				return Container;
			}
		}

		if (CreateIfMissing)
		{
			UE_LOG(LogGunfireSaveSystem, Log, TEXT("Creating container '%s'"), *Name.ToString());

			UPersistenceContainer* Container = NewObject<UPersistenceContainer>(CurrentData);
			Container->SetKey(Name);
			CurrentData->Containers.Emplace(Container);
			return Container;
		}
	}

	return nullptr;
}

const FName& UPersistenceManager::GetContainerKey(const UPersistenceComponent* Component) const
{
	if (ensure(Component))
	{
		if (Component->SaveKey.IsNone())
		{
			ULevel* Level = Component->GetComponentLevel();

			// The level should always be valid
			ensureMsgf(Level, TEXT("Invalid level for actor '%s' in outer '%s'"),
				(Component->GetOwner() != nullptr ? *Component->GetOwner()->GetName() : TEXT("Invalid Actor")),
				(Component->GetOutermost() != nullptr ? *Component->GetOutermost()->GetName() : TEXT("Invalid Outer")));

			const FName* LevelKey = LoadedLevels.Find(Level);

			if (LevelKey != nullptr)
			{
				return *LevelKey;
			}
		}
		else
		{
			return Component->SaveKey;
		}
	}

	static const FName None;
	return None;
}

bool UPersistenceManager::DeleteContainer(const FName& ContainerName, bool BlockLoadedLevel)
{
	if (CurrentData != nullptr)
	{
		for (int i = CurrentData->Containers.Num() - 1; i >= 0; i--)
		{
			UPersistenceContainer* Container = CurrentData->Containers[i];

			if (Container->GetKey() == ContainerName)
			{
				RegisteredActors.Remove(ContainerName);

				CurrentData->Containers.RemoveAt(i);

				Container->MarkAsGarbage();

				// If we have a level loaded for this container, remove it from our list.
				// That way we won't recreate the container we just deleted if a save is
				// triggered before the level unloads.  If the level is unloaded and then
				// loaded again it will save though.
				if (BlockLoadedLevel)
				{
					for (auto It = LoadedLevels.CreateIterator(); It; ++It)
					{
						if (It.Value() == ContainerName)
						{
							It.RemoveCurrent();
							break;
						}
					}
				}

				return true;
			}
		}
	}

	return false;
}

void UPersistenceManager::PackContainer(const FName& LevelKey)
{
	// This container should be done being used at this point, so pack it until it's
	// needed again.
	if (UPersistenceContainer* Container = GetContainer(LevelKey, false))
	{
		Container->Pack();
	}

#if UE_BUILD_DEBUGGAME
	const TArray<TWeakObjectPtr<UPersistenceComponent>>* Actors = RegisteredActors.Find(LevelKey);
	if (Actors && Actors->Num() > 0)
	{
		ensureMsgf(0, TEXT("Actors weren't unregistered"));
	}
#endif

	// Remove the registered actors array for this container (should be empty at this point)
	RegisteredActors.Remove(LevelKey);
}

void UPersistenceManager::OnLevelAddedToWorld(ULevel* Level, UWorld* World)
{
	if (Level != nullptr && !LoadedLevels.Contains(Level))
	{
		// Safely get the 'key'
		FName Key = NAME_None;
		if (ALevelScriptActor* LevelScript = Level->GetLevelScriptActor())
		{
			Key = *LevelScript->GetName();
		}

		UPersistenceContainer* Container = GetContainer(Key, false);
		if (Container == nullptr)
		{
			// Only allow the OnLevelPostLoad to be called if we know it is not yet tracked by the PersistenceManager.
			OnLevelPostLoad(Level, World);
		}
	}

	if (!LoadedLevels.Contains(Level))
	{
		// Only allow the OnLevelPostLoad to be called if it is untracked by the PersistenceManager.
		OnLevelPostLoad(Level, World);
	}
}

void UPersistenceManager::OnLevelPostLoad(ULevel* Level, UWorld* World)
{
	UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("UPersistenceManager - Level Loaded '%s'"), *Level->GetPathName());

	// During initial world load we'll get this callback before the game instance is set,
	// so we can't tell if it's our world or not.  Cache it off until later.
	if (World->GetGameInstance() == nullptr)
	{
		CachedLoads.Add(Level);
	}
	else
	{
		// Always check if this is actually our world.  In PIE this callback will come in
		// for all instances.
		if (GetInstance(World) == this)
		{
			ensure(LoadedLevels.Find(Level) == nullptr);

			// Cache off the level key so we don't have to keep recomputing it every time an
			// actor from this level is used.
			const FName LevelKey(*Level->GetPathName());
			LoadedLevels.Add(Level) = LevelKey;

			if (UPersistenceContainer* Container = GetContainer(LevelKey, false))
			{
				if (!Container->IsUnPacked())
				{
					Container->Unpack();
					Container->PreloadDynamicActors(Level, *this);
				}
			}
		}
	}
}

void UPersistenceManager::OnPreWorldInitialization(UWorld* World, const UWorld::InitializationValues IVS)
{
	ProcessCachedLoads();
}

void UPersistenceManager::OnCanLevelActorsInitialize(ULevel* Level, UWorld* World, bool& CanInitialize)
{
	// The engine is ready to finish loading this level.  If we aren't done loading
	// dynamic actors for the level, ask it to wait.
	if (GetInstance(World) == this && !World->IsNetMode(NM_Client))
	{
		// We can get into this case if the world is torn down before it finishes loading up (network error or
		// something). In that case just let it go.
		if (World->bIsTearingDown)
		{
			return;
		}

		// If something has triggered a block load no delegates will be triggered until after it's done. So to avoid an
		// infinite load, tell the container to check if their delegate is done but just not called yet, and continue in
		// that case.
		const bool bCheckDelegates = World->GetIsInBlockTillLevelStreamingCompleted();

		if (const FName* LevelKey = LoadedLevels.Find(Level))
		{
			if (UPersistenceContainer* Container = GetContainer(*LevelKey, false))
			{
				if (Container->IsPreloadingDynamicActors(bCheckDelegates))
				{
					CanInitialize = false;
				}
			}
		}
	}
}

void UPersistenceManager::OnLevelActorsInitialized(ULevel* Level, UWorld* World)
{
	ProcessCachedLoads();

	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_SpawnDynamicActors);

	if (GetInstance(World) == this && !World->IsNetMode(NM_Client))
	{
		bool SpawnedActors = true;

		const FName* LevelKey = LoadedLevels.Find(Level);
		if (LevelKey)
		{
			if (UPersistenceContainer* Container = GetContainer(*LevelKey, false))
			{
				SpawnedActors = Container->SpawnDynamicActors(Level, *this);
			}
		}

		if (SpawnedActors)
		{
			// Regardless of whether we spawned actors or not, send the notification
			OnDynamicSpawned.Broadcast(Level);
		}
	}
}

void UPersistenceManager::OnLevelPreRemoveFromWorld(ULevel* Level, UWorld* World)
{
	UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("Level pre-remove from world '%s'"), Level ? *Level->GetLevelScriptActor()->GetName() : *World->GetName());

	ProcessCachedLoads();

	if (GetInstance(World) == this && !World->IsNetMode(NM_Client))
	{
		// It's possible to not have a save in the editor so account for that here.
		if (CurrentData != nullptr)
		{
			// We're about to remove a level from the world.  Before anything gets removed force a
			// save for all persistent actors.
			const FName* LevelKey = LoadedLevels.Find(Level);
			if (LevelKey)
			{
				if (auto Components = RegisteredActors.Find(*LevelKey))
				{
					bool WriteContainer = true;

					// If we don't have any actors registered anymore and we don't have any
					// destroyed actors to persist we don't need this container anymore
					// and can remove it.
					if (Components->Num() == 0)
					{
						UPersistenceContainer* Container = GetContainer(*LevelKey, false);

						if (!Container || !Container->HasDestroyed())
						{
							WriteContainer = false;

							if (DeleteContainer(*LevelKey, true))
							{
								UE_LOG(LogGunfireSaveSystem, Log, TEXT("Deleting container '%s' on level unload because it's unused"), *LevelKey->ToString());
							}
						}
					}

					if (WriteContainer)
					{
						UPersistenceContainer* Container = GetContainer(*LevelKey, true);
						Container->WriteData(*Components, *this);
						Container->Pack();
					}
				}
			}
		}
	}
}

void UPersistenceManager::OnLevelRemovedFromWorld(ULevel* Level, UWorld* World)
{
	ProcessCachedLoads();

	UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("Level removed from world '%s'"),
		(Level && Level->GetLevelScriptActor()) ? *Level->GetLevelScriptActor()->GetName() : *World->GetName());

	if (GetInstance(World) == this)
	{
		// If Level is null the entire world is getting removed, so pack all containers for
		// that world.
		if (Level == nullptr)
		{
			for (auto It = LoadedLevels.CreateIterator(); It; ++It)
			{
				if (It.Key() && It.Key()->OwningWorld == World)
				{
					if (IsCachingUnloads)
					{
						CachedUnloads.Add(It.Key());
					}
					else
					{
						PackContainer(It.Value());
						It.RemoveCurrent();
					}
				}
			}

			LevelOffsets.RemoveAllSwap([World](const FLevelOffset& LevelOffset)
			{
				ULevelStreaming* LevelPtr = LevelOffset.Level.Get();
				return LevelPtr == nullptr || LevelPtr->GetWorld() == World;
			}, false);
		}
		// Otherwise, just pack the container for the specified level (if it has one)
		else
		{
			if (const FName* LevelKey = LoadedLevels.Find(Level))
			{
				if (IsCachingUnloads)
				{
					CachedUnloads.Add(Level);
				}
				else
				{
					PackContainer(*LevelKey);
					LoadedLevels.Remove(Level);
				}
			}

			LevelOffsets.RemoveAllSwap([Level](const FLevelOffset& LevelOffset)
			{
				ULevelStreaming* LevelPtr = LevelOffset.Level.Get();
				return LevelPtr == nullptr || LevelPtr->GetLoadedLevel() == Level;
			}, false);
		}
	}
}

void UPersistenceManager::OnPreLoadMap(const FString& MapURL)
{
	ProcessCachedLoads();

	check(!IsCachingUnloads);

	// In UEngine::LoadMap the LevelRemovedFromWorld notification is sent before all the
	// actors have had EndPlay called on them.  To work around that, when a map is loading
	// we cache all the unloads until the cleanup phase.
	IsCachingUnloads = true;
}

void UPersistenceManager::OnPostLoadMap(UWorld* LoadedWorld)
{
	ProcessCachedLoads();

	// There should have been a cleanup call before we hit this point.
	check(!IsCachingUnloads);
}

void UPersistenceManager::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	ProcessCachedLoads();

	// At this point all the actors from the world being unloaded should have written
	// their info and been destroyed, so cleanup the level containers now.
	if (IsCachingUnloads)
	{
		IsCachingUnloads = false;

		if (GetInstance(World) == this)
		{
			for (ULevel* Level : CachedUnloads)
			{
				OnLevelRemovedFromWorld(Level, World);
			}
		}
		else
		{
			// Assume that there won't be a case where we'll run world cleanup for
			// multiple instances at the same time.  If this triggers something is
			// probably wrong.
			check(CachedUnloads.Num() == 0);
		}

		CachedUnloads.SetNum(0);
	}
}

void UPersistenceManager::OnSuspend()
{
	// If we're getting suspended, trigger a save now.  We may be terminated at any point
	// after this.
	UE_LOG(LogGunfireSaveSystem, Log, TEXT("Forcing save during suspend"));
	CommitSave(TEXT("Suspend"));
}

void UPersistenceManager::ProcessCachedLoads()
{
	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_ProcessCachedLoads);

	for (int32 i = 0; i < CachedLoads.Num(); ++i)
	{
		ULevel* CachedLevel = CachedLoads[i];

		if (CachedLevel->OwningWorld->GetGameInstance() != nullptr)
		{
			// It's possible a load snuck in and registered this before we processed the
			// cache, so if it's already there just skip it.
			if (LoadedLevels.Find(CachedLevel) == nullptr)
			{
				OnLevelPostLoad(CachedLevel, CachedLevel->OwningWorld);
			}

			CachedLoads.RemoveAt(i);
			i--;
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////

UPersistenceManager* UPersistenceManager::GetInstance(const UObject* WorldContextObject)
{
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
	{
		if (World->IsGameWorld())
		{
			if (UGameInstance* GameInstance = World->GetGameInstance())
			{
				return GameInstance->GetSubsystem<UPersistenceManager>();
			}
		}
	}

	return nullptr;
}

uint64 UPersistenceManager::GeneratePID(ULevel* pLevel)
{
	uint64 uid = UPersistenceComponent::INVALID_UID;

	if (pLevel != nullptr)
	{
		UWorld* pWorld = pLevel->GetWorld();
		check(pWorld);

#if WITH_EDITOR
		// If we're in the editor and not in PIE, generate a persistent id from the level
		if (!pWorld->IsGameWorld())
		{
			if (ALevelScriptActorGunfire* pLevelActor = Cast<ALevelScriptActorGunfire>(pLevel->GetLevelScriptActor()))
			{
				uid = pLevelActor->GenerateUniqueID();
			}
		}
		else
#endif
		{
			// At runtime, generate a persistent id off of the save game
			if (UPersistenceManager* Persistence = GetInstance(pWorld))
			{
				if (USaveGameWorld* pWorkingSave = Persistence->GetCurrentSave())
				{
					uid = pWorkingSave->GenerateUniqueID();
				}
			}
		}

		if (uid == UPersistenceComponent::INVALID_UID)
		{
			OutputUserMessage(
				TEXT("Failed to find unique ID generator.  Check that the game instance inherits from ")
				TEXT("UGameInstanceGunfire and that the LevelScriptActor inherits from ALevelScriptActorGunfire."),
				pLevel);
		}
	}

	return uid;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

void UPersistenceManager::SetLevelOffset(class ULevelStreaming* Level, const FVector& Offset)
{
	FLevelOffset LevelOffset;
	LevelOffset.Level = Level;
	LevelOffset.Offset = Offset;
	LevelOffsets.Add(LevelOffset);
}

void UPersistenceManager::RemoveLevelOffset(ULevel* Level, FTransform& Transform)
{
	for (const FLevelOffset& LevelOffset : LevelOffsets)
	{
		if (ULevelStreaming* LevelStreaming = LevelOffset.Level.Get())
		{
			if (LevelStreaming->GetLoadedLevel() == Level)
			{
				Transform.SetLocation(Transform.GetLocation() - LevelOffset.Offset);
				break;
			}
		}
	}
}

void UPersistenceManager::AddLevelOffset(ULevel* Level, FTransform& Transform)
{
	for (const FLevelOffset& LevelOffset : LevelOffsets)
	{
		if (ULevelStreaming* LevelStreaming = LevelOffset.Level.Get())
		{
			if (LevelStreaming->GetLoadedLevel() == Level)
			{
				Transform.SetLocation(Transform.GetLocation() + LevelOffset.Offset);
				break;
			}
		}
	}
}

void UPersistenceManager::QueueJob(FThreadJob* Job)
{
	check(IsInGameThread());

	// If a bunch of background work is being queued, only send out a begin for the
	// first one.
	if (++NumBackgroundJobs == 1)
	{
		OnBackgroundWorkBegin.Broadcast();
	}

	{
		FScopeLock Lock(&ThreadJobsLock);
		Job->Manager = this;
		ThreadJobs.Add(Job);
	}
	ThreadHasWork->Trigger();
}

void UPersistenceManager::FreeThreadJob(FThreadJob* Job)
{
	check(IsInGameThread());

	if (UPersistenceManager* ThisPtr = Job->Manager.Get())
	{
		// Don't send out the work end notification until all jobs are done.
		if (--ThisPtr->NumBackgroundJobs == 0)
		{
			ThisPtr->OnBackgroundWorkEnd.Broadcast();
		}

		{
			FScopeLock Lock(&ThisPtr->ThreadJobsLock);
			delete Job;
			ThisPtr->HasRunningThreadJob = false;
		}

		ThisPtr->ThreadHasWork->Trigger();
	}
	else
	{
		// In the middle of shutting down, just delete the job
		delete Job;
	}
}

uint32 UPersistenceManager::Run()
{
	ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem();

	while (!ThreadShouldStop)
	{
		FThreadJob* Job = nullptr;

		// Pop a job off the thread queue, if we don't have one already.  If we're waiting
		// for an async task to push results on the main thread HasRunningThreadJob will
		// be non-null, in which case we want to sleep, so we leave Job null.
		{
			FScopeLock Lock(&ThreadJobsLock);
			if (!HasRunningThreadJob && ThreadJobs.Num() > 0)
			{
				HasRunningThreadJob = true;
				Job = ThreadJobs[0];
				ThreadJobs.RemoveAt(0);
			}
		}

		if (!Job)
		{
			ThreadHasWork->Wait();
			continue;
		}

		// For debugging we support delaying the persistence jobs, to flush out any issues
		// where game code isn't waiting for a job to finish.
		float JobDelay = CVarPersistenceJobDelay.GetValueOnAnyThread();
		if (JobDelay > 0.f)
		{
			FPlatformProcess::Sleep(JobDelay);
		}

		// Process the job on the thread, then queue an async task to dispatch the results
		// on the game thread.  If the game shuts down while the async task is waiting to
		// be dispatched the persistence manager pointer will be null, so we don't bother
		// with callbacks in that case and just delete the job.
		switch (Job->Type)
		{
		case EJobType::Commit:
			{
				bool Ret = true;

				if (Job->WorldData.Num() > 0)
				{
					CompressData(Job->WorldData);

					const FString SlotName = GetSlotName(Job->Slot);
					Ret = SaveSystem->SaveGame(false, *SlotName, UserIndex, Job->WorldData);
				}

				if (Ret && Job->ProfileData.Num() > 0)
				{
					CompressData(Job->ProfileData);
					Ret = SaveSystem->SaveGame(false, SAVE_PROFILE_NAME, UserIndex, Job->ProfileData);
				}

				AsyncTask(ENamedThreads::GameThread, [Job, Ret]()
				{
					if (UPersistenceManager* ThisPtr = Job->Manager.Get())
					{
						ThisPtr->CommitSaveDone(*Job, Ret ? EPersistenceSaveResult::Success : EPersistenceSaveResult::Unknown);
					}
					FreeThreadJob(Job);
				});
			}
			break;

		case EJobType::LoadSlot:
		case EJobType::LoadProfile:
		case EJobType::ReadSlot:
		case EJobType::HasSlot:
			{
				const bool IsProfile = (Job->Type == EJobType::LoadProfile);
				const FString SlotName = IsProfile ? SAVE_PROFILE_NAME : GetSlotName(Job->Slot);

				ISaveGameSystem::ESaveExistsResult Exists;

				Exists = SaveSystem->DoesSaveGameExistWithResult(*SlotName, UserIndex);

				if (Job->Type == EJobType::HasSlot)
				{
					EPersistenceHasResult Result;

					switch (Exists)
					{
					case ISaveGameSystem::ESaveExistsResult::OK:
						Result = EPersistenceHasResult::Exists;
						break;
					case ISaveGameSystem::ESaveExistsResult::DoesNotExist:
						Result = EPersistenceHasResult::Empty;
						break;
					case ISaveGameSystem::ESaveExistsResult::Corrupt:
						Result = EPersistenceHasResult::Corrupt;
						break;
					case ISaveGameSystem::ESaveExistsResult::UnspecifiedError:
					default:
						Result = EPersistenceHasResult::Unknown;
						break;
					}

					AsyncTask(ENamedThreads::GameThread, [Job, Result]()
					{
						if (UPersistenceManager* ThisPtr = Job->Manager.Get())
						{
							ThisPtr->HasSaveDone(*Job, Result);
						}
						FreeThreadJob(Job);
					});
				}
				else
				{
					EPersistenceLoadResult Result;

					if (Exists == ISaveGameSystem::ESaveExistsResult::Corrupt)
					{
						Result = EPersistenceLoadResult::Corrupt;
					}
					else if (Exists == ISaveGameSystem::ESaveExistsResult::UnspecifiedError)
					{
						Result = EPersistenceLoadResult::Unknown;
					}
					else if (Exists == ISaveGameSystem::ESaveExistsResult::DoesNotExist)
					{
						Result = EPersistenceLoadResult::DoesNotExist;
					}
					else
					{
						TArray<uint8>& Data = IsProfile ? Job->ProfileData : Job->WorldData;
						if (SaveSystem->LoadGame(false, *SlotName, UserIndex, Data))
						{
							if (DecompressData(Data))
							{
								Result = EPersistenceLoadResult::Success;
							}
							else
							{
								Result = EPersistenceLoadResult::Corrupt;
							}
						}
						else
						{
							Result = EPersistenceLoadResult::Unknown;
						}
					}

					AsyncTask(ENamedThreads::GameThread, [Job, Result]()
					{
						bool JobDone = true;

						if (UPersistenceManager* ThisPtr = Job->Manager.Get())
						{
							if (Result == EPersistenceLoadResult::Success)
							{
								const TArray<uint8>& Data = (Job->Type == EJobType::LoadProfile) ? Job->ProfileData : Job->WorldData;
								if (ThisPtr->PreloadSave(*Job, Data))
								{
									if (Job->AsyncLoad.IsValid())
									{
										ThisPtr->QueuedJobs.Add(Job);
										JobDone = false;
									}
								}
							}

							if (JobDone)
							{
								if (Job->Type == EJobType::LoadSlot)
								{
									ThisPtr->LoadSaveDone(*Job, Result);
								}
								else if (Job->Type == EJobType::LoadProfile)
								{
									ThisPtr->LoadProfileSaveDone(*Job, Result);
								}
								else if (Job->Type == EJobType::ReadSlot)
								{
									ThisPtr->ReadSaveDone(*Job, Result);
								}
							}
						}

						if (JobDone)
						{
							FreeThreadJob(Job);
						}
					});
				}
			}
			break;

		case EJobType::DeleteSlot:
		case EJobType::DeleteProfile:
			{
				bool Result = false;
				const bool IsProfile = (Job->Type == EJobType::DeleteProfile);
				const FString SlotName = IsProfile ? SAVE_PROFILE_NAME : GetSlotName(Job->Slot);

				if (SaveSystem->DeleteGame(false, *SlotName, UserIndex))
				{
					Result = true;
				}

				AsyncTask(ENamedThreads::GameThread, [Job, Result]()
				{
					if (UPersistenceManager* ThisPtr = Job->Manager.Get())
					{
						ThisPtr->DeleteSaveDone(*Job, Result);
					}
					FreeThreadJob(Job);
				});
			}
			break;

		case EJobType::HasSlotBackup:
		case EJobType::HasProfileBackup:
		case EJobType::RestoreSlotBackup:
		case EJobType::RestoreProfileBackup:
			{
				// TODO: No longer have backup/restore since it's not a native operation
				// on any platform we support. If we want to reimplement it it would just
				// need to be save slot rotation based.
				AsyncTask(ENamedThreads::GameThread, [Job]()
				{
					if (UPersistenceManager* ThisPtr = Job->Manager.Get())
					{
						ThisPtr->BackupOperationDone(*Job, false);
					}
					FreeThreadJob(Job);
				});
			}
			break;

		default:
			checkNoEntry();
			break;
		}
	}

	return 0;
}

#if WITH_EDITOR

void UPersistenceManager::EditorInit()
{
	ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem();

	if (!SaveSystem)
	{
		return;
	}

	const UPersistenceSettings* Settings = GetDefault<UPersistenceSettings>();

	if (!Settings->AllowEditorSaving)
	{
		// In PIE sessions try to delete any existing save data before starting play,
		// to avoid old garbage messing things up.
		SaveSystem->DeleteGame(false, SAVE_PROFILE_NAME, 0);

		for (int32 i = 0; i < 8; ++i)
		{
			SaveSystem->DeleteGame(false, *GetSlotName(i), 0);
		}
	}

	if (Settings->AutomaticallyCreateSave)
	{
		TArray<uint8> ObjectBytes;

		CurrentSlot = 0;

		if (Settings->AllowEditorSaving && SaveSystem->LoadGame(false, *GetSlotName(0), 0, ObjectBytes))
		{
			if (DecompressData(ObjectBytes))
			{
				CurrentData = Cast<USaveGameWorld>(ReadSave(ObjectBytes));
			}
		}

		if (CurrentData == nullptr)
		{
			CurrentData = CreateSaveGame();
		}

		const UGunfireSaveSystemSettings* SaveSettings = GetDefault<UGunfireSaveSystemSettings>();

		// Create a profile save too, if a class is specified
		if (!SaveSettings->SaveProfileClass.IsNull())
		{
			if (Settings->AllowEditorSaving && SaveSystem->LoadGame(false, SAVE_PROFILE_NAME, 0, ObjectBytes))
			{
				if (DecompressData(ObjectBytes))
				{
					UserProfile = Cast<USaveGameProfile>(ReadSave(ObjectBytes));
				}
			}

			if (UserProfile == nullptr)
			{
				UserProfile = CreateSaveProfile();
			}
		}
	}
}

#endif // WITH_EDITOR
