// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "PersistenceTypes.h"
#include "SaveGameArchive.h"

#include "PersistenceContainer.generated.h"

class UPersistenceComponent;
class UPersistenceManager;

//
// A persistence container contains the save data for all actors in that container.
// Typically a container corresponds to a level instance (per instance since a level could be loaded multiple times at
// different offsets), but if a persistence component has a save key set a container will be created for just that actor.
//
UCLASS()
class GUNFIRESAVESYSTEM_API UPersistenceContainer : public UObject
{
	GENERATED_BODY()

protected:
	struct FInfo
	{
		FGuid UniqueId;
		uint32 Offset = 0;
		uint32 Length = 0;
	};

	struct FHeader
	{
		uint32 Version = 0;
		FPackageFileVersion UEVersion;
		uint32 DynamicOffset = 0;
		uint32 IndexOffset = 0;

		// Lookup info for individual actors data, only valid when this persistence container is in use.
		TArray<FInfo> Info;

		// Persistent id's of non-dynamic destroyed actors.
		TArray<FGuid> Destroyed;

		// The custom versions for our most recently written or loaded archive.
		FCustomVersionContainer CustomVersions;

		// Unique names for all actors in this container
		FNameCache NameCache;

		void Reset();

		void Serialize(FArchive& Ar);
		void SerializeVariable(FArchive& Ar);

		void InitArchive(FArchive& Ar) const;

		bool IsUnpacked() const { return Info.Num() != 0 || Destroyed.Num() != 0; }
		bool IsPacked() const { return Info.Num() == 0 && Destroyed.Num() == 0; }
	};

public:
	void SetKey(const FName& InKey) { Key = InKey; }
	const FName& GetKey() const { return Key; }

	void Pack();
	void Unpack();
	bool IsUnpacked() const { return Header.IsUnpacked(); }
	bool IsPacked() const { return Header.IsPacked() && Blob.Data.Num() > 0; }

	bool HasDestroyed() const { return Header.Destroyed.Num() > 0; }

	// Replaces the contents of the container with the save data from the specified components
	void WriteData(TArrayView<TWeakObjectPtr<UPersistenceComponent>> Components, UPersistenceManager& Manager);

	// Loads any existing save data for the actor owning this component.
	void LoadData(UPersistenceComponent* Component, UPersistenceManager& Manager) const;

	// Preloads data for any dynamic actors that aren't already loaded. This should be called as early as possible when
	// a level starts loading, and before calling SpawnDynamicActors.
	void PreloadDynamicActors(ULevel* Level, UPersistenceManager& Manager);

	bool IsPreloadingDynamicActors(bool bCheckDelegates = false) const;
	bool HasSpawnedDynamicActors() const;

	// Called after a level is done loading, to spawn any persistent dynamic actors
	bool SpawnDynamicActors(ULevel* Level, UPersistenceManager& Manager);

	// Special case for when we're dynamically spawning actors from a save. We can't set the persistent id in time, so
	// we cache it locally and let the persistence component call back in to get it.
	FGuid GetSpawningActorId();

	// When a placed persistent actor is destroyed it needs to call this to save the fact that it should be destroyed
	// when this save game loads.
	void SetDestroyed(UPersistenceComponent* Component);

protected:
	void WriteData(UPersistenceComponent* Component, UPersistenceManager& Manager, FArchive& Ar);
	void ReadData(UPersistenceComponent* Component, UPersistenceManager& Manager, FArchive& Ar) const;

	void SpawnDynamicActorsInternal(ULevel* Level, UPersistenceManager& Manager, bool Spawn);

	void OnDynamicActorsLoaded(ULevel* Level);

	UPROPERTY(SaveGame, BlueprintReadOnly)
	FName Key;

	// All the save data is for a container is stored as a blob, so we only have to unpack it when it's actually needed.
	UPROPERTY(SaveGame)
	FPersistenceBlob Blob;

	FHeader Header;

	// The unique id for the currently spawning actor
	FGuid SpawningActorId;

	enum class EClassLoadState
	{
		Uninitialized,
		Preloading,
		WaitingForPreload,
		SpawningDynamicActors,
		Complete,
	};

	EClassLoadState LoadState = EClassLoadState::Uninitialized;
	TSharedPtr<struct FStreamableHandle> DynamicActorLoad;
};
