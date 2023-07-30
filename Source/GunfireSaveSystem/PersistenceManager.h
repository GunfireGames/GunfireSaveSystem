// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Engine/DeveloperSettings.h"
#include "HAL/Runnable.h"
#include "PersistenceTypes.h"
#include "PersistenceManager.generated.h"

DECLARE_STATS_GROUP(TEXT("PersistenceGunfire"), STATGROUP_Persistence, STATCAT_Advanced);

class UPersistenceComponent;
class UPersistenceContainer;
class USaveGame;
class USaveGameWorld;
class USaveGameProfile;

// A persistent actor reference. This will locate a reference from a persistent key, if the
// actor is available. Please avoid using this when possible, as this is somewhat slow due
// to iterating over all persistent components in the world.
//
// WARNING: An actor reference will only persist if the owning actor AND the saved reference 
// both have persistence components!
USTRUCT(BlueprintType)
struct FPersistentReference
{
	GENERATED_BODY()

	AActor* GetReference(UWorld* World);
	void SetReference(AActor* InActor);
	void CopyReferenceFrom(const FPersistentReference& OtherReference);
	void ClearReference();

protected:
	UPROPERTY(SaveGame)
	FPersistenceKey Key;

	UPROPERTY(Transient)
	TObjectPtr<AActor> CachedActor = nullptr;
};

UCLASS(config = EditorPerProjectUserSettings)
class GUNFIRESAVESYSTEM_API UPersistenceSettings : public UObject
{
	GENERATED_BODY()
public:
	// Should the editor allow persistent saving? Thus not clear out all saves created each time you hit play.
	UPROPERTY(EditAnywhere, config, Category = "Persistence")
	bool AllowEditorSaving = false;

	// Should the editor automatically created a save file if it does not exist in slot 0?
	UPROPERTY(EditAnywhere, config, Category = "Persistence")
	bool AutomaticallyCreateSave = true;
};

UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "Gunfire Save System"))
class GUNFIRESAVESYSTEM_API UGunfireSaveSystemSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return "Plugins"; }

	// The class for world save data.  There can be multiple saves of this type, but only
	// one will ever be active at a given time.
	UPROPERTY(config, EditAnywhere, Category = "Save System")
	TSoftClassPtr<class USaveGameWorld> SaveGameClass;

	// The class for the profile save data.  There is only one instance of this, and it's
	// for data that is not associated with a particular save game slot, like unlocks.
	UPROPERTY(config, EditAnywhere, Category = "Save System")
	TSoftClassPtr<class USaveGameProfile> SaveProfileClass;
};

DECLARE_DELEGATE_RetVal(int32, FGetBuildNumber);
DECLARE_DELEGATE_TwoParams(FPersistenceUserMessage, const FString&, const UObject*)
DECLARE_DELEGATE_TwoParams(FLoadSaveComplete, EPersistenceLoadResult, USaveGame*)
DECLARE_DELEGATE_OneParam(FDeleteSaveComplete, bool)
DECLARE_DELEGATE_OneParam(FCommitSaveComplete, EPersistenceSaveResult)
DECLARE_DELEGATE_OneParam(FHasSaveComplete, EPersistenceHasResult)
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FBackgroundWork);

//
// The persistence manager handles loading and saving persistent world data for a game
// session.  There is one persistence manager instance hung off the game instance, so it
// is created at game start and destroyed at game end.
//
UCLASS(config = Engine, defaultconfig)
class GUNFIRESAVESYSTEM_API UPersistenceManager : public UGameInstanceSubsystem, public FRunnable
{
	GENERATED_BODY()

public:
	// Bind this so the build number can be written to the save file.  If this is
	// implemented it's expect to increase with each new build (ie, the changelist number
	// or something similar), and if a save with a higher build number than what this
	// returns is attempted to be loaded it will fail and return that the save is too new
	// (to help avoid issues with saves from patched builds from being loaded without the
	// patch).  Leaving this unbound or returning zero will disable the build number checks.
	static FGetBuildNumber GetBuildNumber;

	// Bind this to catch high priority messages that should be displayed to the user,
	// typically about configuration issues.  This is intended to be used in the editor
	// to pipe messages to the output window.  If it's unbound the messages will be
	// printed to the log.
	static FPersistenceUserMessage UserMessage;

public:
	UPersistenceManager();

	virtual void BeginDestroy() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	void ResetPersistence();

	// Loads the profile save data, creating it if it doesn't exist.
	// Profile save data is for data that is not associated with a particular save game
	// slot, like unlocks.
	void LoadProfileSave(FLoadSaveComplete Callback);

	// Gets the profile save.  This will return null if a profile save hasn't been loaded
	// or created by Load Profile Save.
	USaveGameProfile* GetProfileSave() const { return UserProfile; }

	void DeleteProfileSave(FDeleteSaveComplete Callback = FDeleteSaveComplete());

	// Loads the save in the requested slot or creates a new save if the slot is empty.
	void LoadSave(int32 Slot, FLoadSaveComplete Callback);

	// For querying purposes, reads the save in the specified slot and returns it, but
	// does not cache it or set it as the current save slot.
	void ReadSave(int32 Slot, FLoadSaveComplete Callback);

	// For querying purposes, checks if there's a valid save in the specified slot or if
	// it's empty.
	void HasSave(int32 Slot, FHasSaveComplete Callback);

	void DeleteSave(int32 Slot, FDeleteSaveComplete Callback = FDeleteSaveComplete());

	// Commits the current save data to storage (world save and any profiles).
	// When the commit is complete OnSaveGame will be called with the result.
	void CommitSave(const FString& Reason, FCommitSaveComplete Callback = FCommitSaveComplete());

	// Commits the current save data to a new slot, and sets that to be the current one.
	// When the commit is complete OnSaveGame will be called with the result.
	void CommitSaveToSlot(int32 Slot, FCommitSaveComplete Callback = FCommitSaveComplete());

	// Checks if the profile or a slot has a backup, and can restore it if there is one.
	// These should only be needed when a save is flagged as corrupted, to attempt to
	// restore the previous version.
	void HasProfileBackup(FDeleteSaveComplete Callback = FDeleteSaveComplete());
	void RestoreProfileBackup(FDeleteSaveComplete Callback = FDeleteSaveComplete());
	void HasSlotBackup(int32 Slot, FDeleteSaveComplete Callback = FDeleteSaveComplete());
	void RestoreSlotBackup(int32 Slot, FDeleteSaveComplete Callback = FDeleteSaveComplete());

	int32 GetCurrentSlot() const { return CurrentSlot; }

	// Gets the current save.  This will return null if a save hasn't been loaded or
	// created by Load Save.
	USaveGameWorld* GetCurrentSave() const { return CurrentData; }

	// If Disable Commit is true, any commit calls will be ignored.  This is a special
	// case for situations where saving would break things on load, and it is expected
	// this will be messaged to the user by disabling any save option in the menu.
	void SetDisableCommit(bool DisableCommit);

	bool AreCommitsDisabled() const { return bDisableCommit; }

	// Deletes all containers in the current save with the specified name, or containing
	// the specified name.  Useful for situations like level instances that are spawned
	// dynamically then removed permanently when they're completed.
	//
	// Note: Be very careful with the SubstringMatch option.  If your string isn't unique
	// enough you could end up removing unrelated containers.
	void DeleteContainers(const FString& ContainerName, bool SubstringMatch);

	// Any persistent actor is guaranteed to have a globally unique key, which can be a
	// handy way to look them up.  GetActorKey returns a value that can be saved or sent
	// across the network, and FindActorByKey will find that actor (if they're already
	// loaded).
	FPersistenceKey GetActorKey(AActor* Actor) const;
	AActor* FindActorByKey(FPersistenceKey Key) const;

	bool IsSaving() const { return NumSavesPending > 0; }
	bool HasPendingSave() const { return NumSavesPending > 1; }

	// Sets the current user index indicating which controller ID profile to save to.
	void SetUserIndex(int32 Index) { UserIndex = Index; }
	int32 GetUserIndex() const { return UserIndex; }

	// User has signed out, etc. and is no longer valid.
	void InvalidateUser() { UserProfile = nullptr; ResetPersistence(); };

	// Once set, saving is disabled for the entire play session. Useful for demos.
	void SetNeverCommit() { bNeverCommit = true; }

	//////////////////////////////////////////////////////////////////////////////////////
	//
	// Persistence Component access
	//

	// Used by persistence components to register themselves with the manager.
	void Register(UPersistenceComponent* pComponent);
	void Unregister(UPersistenceComponent* pComponent, ULevel* OverrideLevel = nullptr);

	// Returns the container for a given persistence component (if it exists)
	UPersistenceContainer* GetContainer(const UPersistenceComponent* Component);

	// Marks a component as destroyed
	void SetComponentDestroyed(UPersistenceComponent* Component);

	// Special case for persistence components that use a save key instead of being
	// persisted with their level.  Should be called when the component is being removed
	// from the world, to catch any unsaved changes.
	void WriteComponent(UPersistenceComponent* Component);

	TMap<FName, bool>& GetClassCache() { return ClassCache; }

	// This needs to be called with the old level when a persistent actor is moved to a
	// new level.
	void OnLevelChanged(UPersistenceComponent* pComponent, ULevel* OldLevel);

	// Checks if dynamic actors have spawned yet for a container
	bool HasSpawnedDynamicActorsForContainer(const FName& Name);

	//////////////////////////////////////////////////////////////////////////////////////
	//
	// Functionality for levels that may change transform in between sessions
	//

	// Sets level offset for the specified level
	void SetLevelOffset(class ULevelStreaming* Level, const FVector& Offset);

	// Removes level offset from the transform for the specified level
	void RemoveLevelOffset(ULevel* Level, FTransform& Transform);

	// Adds level offset to the transform for the specified level
	void AddLevelOffset(ULevel* Level, FTransform& Transform);

	//////////////////////////////////////////////////////////////////////////////////////
	//
	// Events
	//
	DECLARE_EVENT(UPersistenceManager, FPreSaveEvent)
	DECLARE_EVENT_OneParam(UPersistenceManager, FDeleteEvent, bool)
	DECLARE_EVENT_TwoParams(UPersistenceManager, FLoadEvent, EPersistenceLoadResult, USaveGameWorld*)
	DECLARE_EVENT_TwoParams(UPersistenceManager, FLoadProfileEvent, EPersistenceLoadResult, USaveGameProfile*)
	DECLARE_EVENT_OneParam(UPersistenceManager, FSaveEvent, EPersistenceSaveResult)
	DECLARE_EVENT_OneParam(UPersistenceManager, FDynamicSpawnedEvent, ULevel*)

	// Called before a save starts.  Data on the world or profile save can be updated now
	// to be included in the new save.
	FPreSaveEvent OnPreSaveGame;
	// Called when a game has been saved
	FSaveEvent OnSaveGame;
	// Called when a save game has been loaded
	FLoadEvent OnLoadGame;
	// Called when the save profile has been loaded
	FLoadProfileEvent OnLoadProfile;
	// Called when a save game has been deleted
	FDeleteEvent OnDeleteGame;
	// Called when all persistent dynamic actors for a level have been spawned
	FDynamicSpawnedEvent OnDynamicSpawned;

	// Called any time a background persistence work (commit, load, delete, etc) is
	// beginning.  This will only be called once if multiple jobs are running at the same
	// time, it's intended just for user notification that background work is in progress,
	// not to catch individual events.
	UPROPERTY(BlueprintAssignable)
	FBackgroundWork OnBackgroundWorkBegin;

	// Called when all background persistence work is done.
	UPROPERTY(BlueprintAssignable)
	FBackgroundWork OnBackgroundWorkEnd;

public:
	static UPersistenceManager* GetInstance(const UObject* WorldContextObject);

	// Generate a persistent, unique ID.  This is used both at edit and run time.
	static uint64 GeneratePID(ULevel* pLevel);

	// Serializes all SaveGame tagged properties for an object to/from a blob.
	void ToBinary(UObject* Object, TArray<uint8>& ObjectBytes);
	void FromBinary(UObject* Object, const TArray<uint8>& ObjectBytes);

protected:
	enum class EJobType
	{
		Uninitialized,
		LoadSlot,
		LoadProfile,
		ReadSlot,
		HasSlot,
		DeleteSlot,
		DeleteProfile,
		Commit,
		HasSlotBackup,
		HasProfileBackup,
		RestoreSlotBackup,
		RestoreProfileBackup,
	};

	struct FThreadJob
	{
		TWeakObjectPtr<UPersistenceManager> Manager;
		EJobType Type = EJobType::Uninitialized;
		TArray<uint8> WorldData;
		TArray<uint8> ProfileData;
		int32 Slot = -1;
		FLoadSaveComplete LoadCallback;
		FHasSaveComplete HasCallback;
		FDeleteSaveComplete DeleteCallback;
		FCommitSaveComplete SaveCallback;
		TSharedPtr<struct FStreamableHandle> AsyncLoad;
	};

	void WriteSave(USaveGame* SaveGame, TArray<uint8>& SaveBlob);
	bool InitSaveArchive(FArchive& Archive, const TArray<uint8>& SaveBlob, FTopLevelAssetPath& SaveGameClassPath, EPersistenceLoadResult* Result);
	bool PreloadSave(FThreadJob& Job, const TArray<uint8>& SaveBlob);
	USaveGame* ReadSave(const TArray<uint8>& SaveBlob, EPersistenceLoadResult* Result = nullptr);
	void OnSaveClassesLoaded(FThreadJob* Job);

	void CompressData(TArray<uint8>& SaveBlob);
	bool DecompressData(TArray<uint8>& SaveBlob);

#if WITH_EDITOR
	void EditorInit();
#endif

	static void OutputUserMessage(const FString& Message, const UObject* ContextObject);

	static FString GetSlotName(int32 Slot);

	void LoadProfileSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result);
	void DeleteProfileSaveDone(const FThreadJob& Job, bool Result);
	void LoadSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result);
	void ReadSaveDone(const FThreadJob& Job, EPersistenceLoadResult Result);
	void HasSaveDone(const FThreadJob& Job, EPersistenceHasResult Result);
	void CommitSaveDone(const FThreadJob& Job, EPersistenceSaveResult Result);
	void DeleteSaveDone(const FThreadJob& Job, bool Result);
	void BackupOperationDone(const FThreadJob& Job, bool Result);

	UPersistenceContainer* GetContainer(const FName& Name, bool CreateIfMissing);
	inline const FName& GetContainerKey(const UPersistenceComponent* Component) const;
	bool DeleteContainer(const FName& ContainerName, bool BlockLoadedLevel);
	void PackContainer(const FName& Name);

	USaveGameWorld* CreateSaveGame();
	USaveGameProfile* CreateSaveProfile();

	void OnLevelAddedToWorld(ULevel* Level, UWorld* World);
	void OnLevelPostLoad(ULevel* Level, UWorld* World);
	void OnPreWorldInitialization(UWorld* World, const UWorld::InitializationValues IVS);
	void OnCanLevelActorsInitialize(ULevel* Level, UWorld* World, bool& CanInitialize);
	void OnLevelActorsInitialized(ULevel* Level, UWorld* World);
	void OnLevelPreRemoveFromWorld(ULevel* Level, UWorld* World);
	void OnLevelRemovedFromWorld(ULevel* Level, UWorld* World);
	void OnPreLoadMap(const FString& MapURL);
	void OnPostLoadMap(UWorld* LoadedWorld);
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void OnSuspend();

	void ProcessCachedLoads();

	void QueueJob(FThreadJob* Job);
	static void FreeThreadJob(FThreadJob* Job);
	virtual uint32 Run() override;

	int32 UserIndex = 0;
	int32 CurrentSlot = -1;

	int32 NumSavesPending = 0;

	// The current save data.  This may include uncommitted changes.
	UPROPERTY(Transient)
	TObjectPtr<USaveGameWorld> CurrentData = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USaveGameProfile> UserProfile = nullptr;

	// Quick lookup to get the container name from a loaded level
	UPROPERTY(Transient)
	TMap<TObjectPtr<ULevel>, FName> LoadedLevels;

	// All the currently active persistent objects, organized by container. Persistence
	// components should always unregister themselves before destructing, so we should
	// never actually have a null pointer in here.
	TMap<FName, TArray<TWeakObjectPtr<UPersistenceComponent>>> RegisteredActors;

	bool IsCachingUnloads = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ULevel>> CachedUnloads;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ULevel>> CachedLoads;

	bool bDisableCommit = false;

	bool bNeverCommit = false;

	// A mapping from UClass names to whether or not they have any persistent data, as an
	// optimization so we don't have to dig through all the properties each time.
	TMap<FName, bool> ClassCache;

	int32 NumBackgroundJobs = 0;

	// Persistence thread properties
	FRunnableThread*	Thread = nullptr;
	FEvent*				ThreadHasWork = nullptr;
	FCriticalSection	ThreadJobsLock;
	TArray<FThreadJob*>	ThreadJobs;
	TArray<FThreadJob*>	QueuedJobs;
	bool				HasRunningThreadJob = false;
	bool				ThreadShouldStop = false;

	struct FLevelOffset
	{
		TWeakObjectPtr<ULevelStreaming> Level;
		FVector							Offset;
	};

	// Level offsets can be provided so that actors are persisted without the offsets,
	// then the offsets are restored when the actors are loaded from persistence.
	// This allows levels to move around and still persist properly
	TArray<FLevelOffset> LevelOffsets;
};
