// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "PersistenceManager.h"

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "PersistenceBlueprintFunctions.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBlueprintCommitSaveResultDelegate, EPersistenceSaveResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FBlueprintLoadSaveResultDelegate, EPersistenceLoadResult, Result, USaveGameWorld*, SaveGame, int32, Slot);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FBlueprintLoadProfileSaveResultDelegate, EPersistenceLoadResult, Result, USaveGame*, ProfileSave);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FBlueprintHasSaveResultDelegate, EPersistenceHasResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FBlueprintSaveNoRetDelegate);

UCLASS(Abstract)
class UPersistenceCallbackProxy : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

protected:
	void CachePersistenceManager(UObject* WorldContextObject);

	UPROPERTY(Transient)
	TObjectPtr<UPersistenceManager> PersistenceManager = nullptr;
};

UCLASS()
class UCommitSaveWithResultCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Commit the current save to storage, and returns success or failure when it's done
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UCommitSaveWithResultCallbackProxy* CommitSaveWithResult(UObject* WorldContextObject, FString Reason);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintCommitSaveResultDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintCommitSaveResultDelegate OnFailure;

protected:
	void OnComplete(EPersistenceSaveResult Result);

	FString CachedReason;
};

UCLASS()
class ULoadSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Sets the current save slot and loads the existing save data if it exists, or creates new data if it doesn't.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static ULoadSaveCallbackProxy* LoadSave(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadSaveResultDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadSaveResultDelegate OnFailure;

	int32 Slot = -1;

protected:
	void OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame);
};

UCLASS()
class ULoadProfileSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Loads the profile save and caches it, or creates it if it doesn't exist.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static ULoadProfileSaveCallbackProxy* LoadProfileSave(UObject* WorldContextObject);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadProfileSaveResultDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadProfileSaveResultDelegate OnFailure;

protected:
	void OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame);
};

UCLASS()
class UReadSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Reads in a save and returns it, but doesn't change the current save slot or set the data.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UReadSaveCallbackProxy* ReadSave(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadSaveResultDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintLoadSaveResultDelegate OnFailure;

	int32 Slot = -1;

protected:
	void OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame);
};

UCLASS()
class UHasSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Checks if a save exists in the specified slot.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UHasSaveCallbackProxy* HasSave(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintHasSaveResultDelegate OnComplete;

	int32 Slot = -1;

protected:
	void OnCompleteFunc(EPersistenceHasResult Result);
};

UCLASS()
class UDeleteSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Deletes the specified save
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UDeleteSaveCallbackProxy* DeleteSave(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnFailure;

	int32 Slot = -1;

protected:
	void OnComplete(bool Result);
};

UCLASS()
class UDeleteProfileSaveCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	// Deletes the profile save
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UDeleteProfileSaveCallbackProxy* DeleteProfileSave(UObject* WorldContextObject);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnFailure;

protected:
	void OnComplete(bool Result);
};

UCLASS()
class UHasProfileBackupCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UHasProfileBackupCallbackProxy* HasProfileBackup(UObject* WorldContextObject);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate HasBackup;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate NoBackup;

protected:
	void OnCompleteFunc(bool Result);
};

UCLASS()
class URestoreProfileBackupCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static URestoreProfileBackupCallbackProxy* RestoreProfileBackup(UObject* WorldContextObject);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnFailure;

protected:
	void OnCompleteFunc(bool Result);
};

UCLASS()
class UHasSlotBackupCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UHasSlotBackupCallbackProxy* HasSlotBackup(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate HasBackup;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate NoBackup;

	int32 Slot = -1;

protected:
	void OnCompleteFunc(bool Result);
};

UCLASS()
class URestoreSlotBackupCallbackProxy : public UPersistenceCallbackProxy
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static URestoreSlotBackupCallbackProxy* RestoreSlotBackup(UObject* WorldContextObject, int32 Slot);

	virtual void Activate() override;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FBlueprintSaveNoRetDelegate OnFailure;

	int32 Slot = -1;

protected:
	void OnCompleteFunc(bool Result);
};

UCLASS()
class GUNFIRESAVESYSTEM_API UPersistenceBlueprintFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static UPersistenceManager* GetPersistenceManager(const UObject* WorldContextObject);

	// Commit the current save to storage and return immediately. If you need the result call Commit Save With Result
	// instead.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static void CommitSave(const UObject* WorldContextObject, const FString Reason);

	// Gets the current save. This will return null if a save hasn't been loaded or created by Load Save.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static USaveGameWorld* GetCurrentSave(const UObject* WorldContextObject);

	// Gets the profile save. This will return null if a profile save hasn't been loaded or created by Load Profile Save.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static USaveGameProfile* GetProfileSave(const UObject* WorldContextObject);

	// If Disable Commit is true, any commit calls will be ignored. This is a special case for situations where saving
	// would break things on load, and it is expected this will be messaged to the user by disabling any save option in
	// the menu.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static void SetDisableCommit(const UObject* WorldContextObject, bool DisableCommit);

	// Clear any pending commit locks caused by other objects so that we can save right now no matter what.
	UFUNCTION(BlueprintCallable, Category = "Persistence", meta = (WorldContext = "WorldContextObject"))
	static void ClearAllCommitLocks(const UObject* WorldContextObject);

	// Resolve an actor reference from a persistent reference
	UFUNCTION(BlueprintPure, Category = "Persistence|Reference", meta = (WorldContext = "WorldContextObject"))
	static AActor* GetReference(const UObject* WorldContextObject, UPARAM(ref) FPersistentReference& Reference);

	// Sets a persistent reference via the actor reference provided. This will only persist if this actor has a
	// persistence component!
	UFUNCTION(BlueprintCallable, Category = "Persistence|Reference")
	static void SetReference(UPARAM(ref) FPersistentReference& Reference, AActor* InActor) { Reference.SetReference(InActor); }

	// Copy data from a reference to another, more efficient than getting and setting the actor reference.
	UFUNCTION(BlueprintCallable, Category = "Persistence|Reference")
	static void CopyReference(const FPersistentReference& From, UPARAM(ref) FPersistentReference& To) { To.CopyReferenceFrom(From); }

	// Clears a reference, essentially nulling it.
	UFUNCTION(BlueprintCallable, Category = "Persistence|Reference")
	static void ClearReference(UPARAM(ref) FPersistentReference& Reference) { Reference.ClearReference(); }
};
