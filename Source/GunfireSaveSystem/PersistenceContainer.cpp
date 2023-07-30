// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "PersistenceContainer.h"
#include "PersistenceComponent.h"
#include "PersistenceManager.h"
#include "PersistenceUtils.h"
#include "SaveGameArchive.h"

#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PersistenceContainer)

DECLARE_CYCLE_STAT(TEXT("Container WriteData"), STAT_PersistenceGunfire_ContainerWriteData, STATGROUP_Persistence);
DECLARE_CYCLE_STAT(TEXT("Container Unpack"), STAT_PersistenceGunfire_ContainerUnpack, STATGROUP_Persistence);

// This version is for backwards compatible changes.  Non backwards compatible changes
// should just bump GUNFIRE_PERSISTENCE_VERSION and invalidate all old savegames.
//
// Version History
// 1: Initial version
// 2: Removed UniqueName field
// 3: Switched to FTopLevelAssetPath for dynamic actor references
// 4: Got rid of UE4 version
#define CONTAINER_VERSION 4

//////////////////////////////////////////////////////////////////////////////////////////

struct FSubArchive : public FArchiveProxy
{
	FSubArchive(FArchive& InInnerArchive)
		: FArchiveProxy(InInnerArchive)
	{
		Offset = InnerArchive.Tell();
	}

	virtual int64 Tell() override
	{
		return InnerArchive.Tell() - Offset;
	}

	virtual int64 TotalSize() override
	{
		return InnerArchive.TotalSize() - Offset;
	}

	virtual void Seek(int64 InPos) override
	{
		InnerArchive.Seek(Offset + InPos);
	}

	int64 Offset;
};

//////////////////////////////////////////////////////////////////////////////////////////

UPersistenceContainer::UPersistenceContainer()
	: SpawningActorID(UPersistenceComponent::INVALID_UID)
{
}

void UPersistenceContainer::Pack()
{
	Info.Reset();
	Destroyed.Reset();
	LoadState = EClassLoadState::Uninitialized;
}

void UPersistenceContainer::Unpack()
{
	// Shouldn't be calling unpack if we're already unpacked
	ensure(Info.Num() == 0 && Destroyed.Num() == 0);

	Info.Reset();
	Destroyed.Reset();

	if (Blob.Data.Num() == 0)
	{
		return;
	}

	FMemoryReader Ar(Blob.Data, true);

	FHeader Header = ReadHeader(Ar);

	Ar.Seek(Header.IndexOffset);

	uint32 NumInfos;
	Ar << NumInfos;
	Info.SetNumUninitialized(NumInfos);

	for (FInfo& CurInfo : Info)
	{
		Ar << CurInfo.UniqueID;
		if (Header.Version < 2)
		{
			FName Unused;
			Ar << Unused;
		}
		Ar << CurInfo.Offset;
		Ar << CurInfo.Length;
	}

	uint32 NumDestroyed;
	Ar << NumDestroyed;
	Destroyed.SetNumUninitialized(NumDestroyed);

	for (uint64& DestroyedID : Destroyed)
	{
		Ar << DestroyedID;
		if (Header.Version < 2)
		{
			FName Unused;
			Ar << Unused;
		}
	}
}

void UPersistenceContainer::WriteData(TArrayView<TWeakObjectPtr<UPersistenceComponent>> Components, UPersistenceManager& Manager)
{
	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_ContainerWriteData);

	UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("------------------------------------------------------------------------------------------"));
	UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("Writing persistence container '%s' (%s)"), *Key.ToString(), *GetName());

	Blob.Data.Reset();
	Info.Reset();

	FMemoryWriter Ar(Blob.Data, true);

	// Stub in the header for data we don't calculate until the end, we'll rewrite it later
	FHeader Header;
	Header.Version = CONTAINER_VERSION;
	WriteHeader(Ar, Header);

	int32 NumDynamicActors = 0;

	//
	// Write out the per-actor save data
	//
	for (const TWeakObjectPtr<UPersistenceComponent>& Component : Components)
	{
		if (UPersistenceComponent* RawComponent = Component.Get())
		{
			if (RawComponent->IsDynamic)
			{
				NumDynamicActors++;
			}

			FInfo& ThisInfo = Info[Info.AddUninitialized()];
			ThisInfo.UniqueID = RawComponent->UniqueID;
			ThisInfo.Offset = (uint32)Ar.Tell();

			{
				// When we read the component back in we'll give it an archive with just
				// its data, so wrap the output archive in a subarchive to ensure any
				// offsets written are correct when read back in.
				FSubArchive SubAr(Ar);
				WriteData(RawComponent, Manager, SubAr);
			}

			// Calculate the total size of the save data for this actor
			ThisInfo.Length = (uint32)Ar.Tell() - ThisInfo.Offset;
		}
	}

	//
	// Write out info for spawning dynamic actors
	//
	Header.DynamicOffset = Ar.Tell();
	
	Ar << NumDynamicActors;

	for (const TWeakObjectPtr<UPersistenceComponent>& Component : Components)
	{
		if (UPersistenceComponent* RawComponent = Component.Get())
		{
			AActor* Actor = RawComponent->GetOwner();

			if (RawComponent->IsDynamic)
			{
				UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("Dynamic actor '%s'"), *Actor->GetName());

				Ar << RawComponent->UniqueID;

				FTransform Transform = Actor->GetTransform();

				// Remove offset if there is an offset
				Manager.RemoveLevelOffset(Actor->GetLevel(), Transform);

				Ar << Transform;

				FTopLevelAssetPath ClassPath = Actor->GetClass()->GetClassPathName();
				Ar << ClassPath;
			}
		}
	}

	//
	// Write the index and destroyed actors
	//
	Header.IndexOffset = Ar.Tell();

	uint32 NumInfos = Info.Num();
	Ar << NumInfos;

	for (FInfo& CurInfo : Info)
	{
		Ar << CurInfo.UniqueID;
		Ar << CurInfo.Offset;
		Ar << CurInfo.Length;
	}

	uint32 NumDestroyed = Destroyed.Num();
	Ar << NumDestroyed;

	for (uint64& DestroyedID : Destroyed)
	{
		Ar << DestroyedID;
	}

	//
	// Write the final offsets
	//
	WriteHeader(Ar, Header);
}

void UPersistenceContainer::PreloadDynamicActors(ULevel* Level, UPersistenceManager& Manager)
{
	if (LoadState == EClassLoadState::Uninitialized)
	{
		SpawnDynamicActorsInternal(Level, Manager, false);
	}
}

bool UPersistenceContainer::IsPreloadingDynamicActors(bool bCheckDelegates) const
{
	// If we requested to check delegates, check if the delegate is complete but not triggered yet, and allow the
	// loading to continue in that case.
	if (bCheckDelegates && LoadState == EClassLoadState::Preloading &&
		DynamicActorLoad.IsValid() && DynamicActorLoad->HasLoadCompleted())
	{
		return false;
	}

	return LoadState == EClassLoadState::Preloading;
}

bool UPersistenceContainer::HasSpawnedDynamicActors() const
{
	return LoadState == EClassLoadState::Complete;
}

bool UPersistenceContainer::SpawnDynamicActors(ULevel* Level, UPersistenceManager& Manager)
{
	// If we already finished the load (or everything was already loaded), spawn the
	// dynamic actors now.
	if (LoadState == EClassLoadState::SpawningDynamicActors)
	{
		SpawnDynamicActorsInternal(Level, Manager, true);

		LoadState = EClassLoadState::Complete;

		return true;
	}
	// If we're still preloading, switch to a state where the dynamic actors will be
	// spawned as soon as the load is done.
	else if (LoadState == EClassLoadState::Preloading)
	{
		UE_LOG(LogGunfireSaveSystem, Log, TEXT("Level finished loading before dynamic actors for container '%s' were loaded, delaying spawn"), *Key.ToString());

		LoadState = EClassLoadState::WaitingForPreload;
	}

	return false;
}

void UPersistenceContainer::SpawnDynamicActorsInternal(ULevel* Level, UPersistenceManager& Manager, bool Spawn)
{
	FMemoryReader Ar(Blob.Data, true);

	FHeader Header = ReadHeader(Ar);

	Ar.Seek(Header.DynamicOffset);

	int32 NumDynamicActors;
	Ar << NumDynamicActors;

	TArray<FSoftObjectPath> ClassesToLoad;

	UE_CLOG(NumDynamicActors > 0 && Spawn, LogGunfireSaveSystem, Log, TEXT("Spawning %d dynamic actors for container '%s'"), NumDynamicActors, *Key.ToString());

	for (int32 i = 0; i < NumDynamicActors; i++)
	{
		uint64 UniqueID;
		Ar << UniqueID;

		FTransform Transform;
		Ar << Transform;

		// Add offset if there is one
		Manager.AddLevelOffset(Level, Transform);

		FTopLevelAssetPath ClassPath;

		if (Header.Version >= 3)
		{
			Ar << ClassPath;
		}
		else
		{
			FString OldClassPath;
			Ar << OldClassPath;
			ClassPath.TrySetPath(OldClassPath);
		}

		if (!Spawn)
		{
			ClassesToLoad.AddUnique(FSoftObjectPath(ClassPath));
		}
		else
		{
			// Try to find the class
			UClass* ClassInfo = FindObject<UClass>(ClassPath);

			if (ClassInfo != nullptr)
			{
				FActorSpawnParameters SpawnInfo;
				SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				SpawnInfo.Owner = nullptr;
				SpawnInfo.Instigator = nullptr;
				SpawnInfo.bDeferConstruction = true;

				// Make sure the actor spawns a a part of the level
				SpawnInfo.OverrideLevel = Level;

				if (AActor* Actor = Cast<AActor>(Level->GetWorld()->SpawnActor(ClassInfo, &Transform, SpawnInfo)))
				{
					// Cache off the unique id and finish spawning the actor.  It will call
					// back into the persistence container to find the id when it initializes.
					SpawningActorID = UniqueID;

					UGameplayStatics::FinishSpawningActor(Actor, Transform);

					// Spawned actor didn't take the persistent id.  Did something go wrong?
					ensure(SpawningActorID == UPersistenceComponent::INVALID_UID);

					SpawningActorID = UPersistenceComponent::INVALID_UID;
				}
			}
		}
	}

	if (!Spawn)
	{
		// Always request all the necessary classes, even if they're already loaded.
		// That way we'll have a ref on them so they won't be garbage collected if they
		// are already loaded and get all their refs dropped.
		if (ClassesToLoad.Num() > 0)
		{
			UE_LOG(LogGunfireSaveSystem, Log, TEXT("Requesting load of dynamic actor classes for container '%s'"), *Key.ToString());

			LoadState = EClassLoadState::Preloading;

			DynamicActorLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(
				ClassesToLoad,
				FStreamableDelegate::CreateUObject(this, &ThisClass::OnDynamicActorsLoaded, Level),
				FStreamableManager::AsyncLoadHighPriority);
		}
		else
		{
			LoadState = EClassLoadState::Complete;
		}
	}
	else
	{
		DynamicActorLoad.Reset();
	}
}

void UPersistenceContainer::OnDynamicActorsLoaded(ULevel* Level)
{
	// Still haven't tried to spawn the actors, just flag them as ready
	if (LoadState == EClassLoadState::Preloading)
	{
		LoadState = EClassLoadState::SpawningDynamicActors;

		UE_LOG(LogGunfireSaveSystem, Log, TEXT("Dynamic actors for container '%s' finished loading"), *Key.ToString());
	}
	// Already tried to spawn the actors earlier, so do it now
	else if (LoadState == EClassLoadState::WaitingForPreload)
	{
		LoadState = EClassLoadState::SpawningDynamicActors;

		UE_LOG(LogGunfireSaveSystem, Log, TEXT("Dynamic actors for container '%s' finished loading, spawning actors"), *Key.ToString());

		if (UPersistenceManager* Manager = UPersistenceManager::GetInstance(Level))
		{
			SpawnDynamicActors(Level, *Manager);
			Manager->OnDynamicSpawned.Broadcast(Level);
		}
	}
}

uint64 UPersistenceContainer::GetDynamicActorID()
{
	uint64 Ret = SpawningActorID;
	SpawningActorID = UPersistenceComponent::INVALID_UID;
	return Ret;
}

void UPersistenceContainer::SetDestroyed(UPersistenceComponent* Component)
{
	ensureMsgf(!Destroyed.Contains(Component->UniqueID), TEXT("Adding destroyed actor twice"));

	Destroyed.Emplace(Component->UniqueID);
}

void UPersistenceContainer::LoadData(UPersistenceComponent* Component, UPersistenceManager& Manager) const
{
	// If this goes off we're somehow loading data when this container hasn't been
	// unpacked.  Was the level load missed somehow?
	ensure(Blob.Data.Num() == 0 || Info.Num() > 0 || Destroyed.Num() > 0);

	const FInfo* ActorInfo = Info.FindByPredicate([=](const FInfo& RHS) { return Component->UniqueID == RHS.UniqueID; });

	// If we found saved info for this actor, create a reader for that section of the raw
	// data and read it in.
	if (ActorInfo)
	{
		FLargeMemoryReader Ar(&Blob.Data[ActorInfo->Offset], ActorInfo->Length);
		ReadData(Component, Manager, Ar);
	}
	// Otherwise, check if it's been marked as destroyed
	else if (Destroyed.Contains(Component->UniqueID))
	{
		Component->DestroyPersistentActor();
	}
}

void UPersistenceContainer::WriteHeader(FArchive& Ar, FHeader Header) const
{
	Ar.Seek(0);

	Ar << Header.Version;
	Ar << Header.IndexOffset;
	Ar << Header.DynamicOffset;
}

UPersistenceContainer::FHeader UPersistenceContainer::ReadHeader(FArchive& Ar) const
{
	Ar.Seek(0);

	FHeader Header;
	Ar << Header.Version;
	if (Header.Version < 4)
	{
		int32 UE4Version;
		Ar << UE4Version;
	}
	Ar << Header.IndexOffset;
	Ar << Header.DynamicOffset;

	return Header;
}

void UPersistenceContainer::WriteData(UPersistenceComponent* Component, UPersistenceManager& Manager, FArchive& Ar)
{
	AActor* Actor = Component->GetOwner();

	// Store transform
	bool SaveTransform = Component->PersistTransform;
	Ar << SaveTransform;
	if (SaveTransform)
	{
		FTransform Transform = Actor->GetTransform();

		// Remove offset if there is a level offset
		Manager.RemoveLevelOffset(Actor->GetLevel(), Transform);

		Ar << Transform;
	}

	// Write Actor Data.
	{
		FSaveGameArchive PAr(Ar, Component->HasModifiedSaveValues);
		PAr.WriteBaseObject(Actor, Manager.GetClassCache());
	}
}

void UPersistenceContainer::ReadData(UPersistenceComponent* Component, UPersistenceManager& Manager, FArchive& Ar) const
{
	SCOPE_CYCLE_COUNTER(STAT_PersistenceGunfire_ContainerUnpack);

	AActor* Actor = Component->GetOwner();

	UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("Reading Actor '%s' [%s]"), *Actor->GetName(), *Actor->GetClass()->GetName());

	// Read transform
	bool SaveTransform;
	Ar << SaveTransform;

	if (SaveTransform)
	{
		FTransform Transform;
		Ar << Transform;

		// Add offset if there is an offset
		Manager.AddLevelOffset(Actor->GetLevel(), Transform);

		Actor->SetActorTransform(Transform);
	}

	// Read Actor Data
	{
		FSaveGameArchive PAr(Ar);
		PAr.ReadBaseObject(Actor);
	}
}
