// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "PersistenceBlueprintFunctions.h"
#include "SaveGameWorld.h"
#include "Engine/Engine.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PersistenceBlueprintFunctions)

void UPersistenceCallbackProxy::CachePersistenceManager(UObject* WorldContextObject)
{
	PersistenceManager = UPersistenceManager::GetInstance(WorldContextObject);
}

UCommitSaveWithResultCallbackProxy* UCommitSaveWithResultCallbackProxy::CommitSaveWithResult(UObject* WorldContextObject, FString Reason)
{
	UCommitSaveWithResultCallbackProxy* Ret = NewObject<UCommitSaveWithResultCallbackProxy>();
	Ret->CachedReason = Reason;
	Ret->CachePersistenceManager(WorldContextObject);
	return Ret;
}

void UCommitSaveWithResultCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->CommitSave(CachedReason, FCommitSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast(EPersistenceSaveResult::Unknown);
	}
}

void UCommitSaveWithResultCallbackProxy::OnComplete(EPersistenceSaveResult Result)
{
	if (Result == EPersistenceSaveResult::Success)
	{
		OnSuccess.Broadcast(Result);
	}
	else
	{
		OnFailure.Broadcast(Result);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

ULoadSaveCallbackProxy* ULoadSaveCallbackProxy::LoadSave(UObject* WorldContextObject, int32 Slot)
{
	ULoadSaveCallbackProxy* Ret = NewObject<ULoadSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void ULoadSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->LoadSave(Slot, FLoadSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast(EPersistenceLoadResult::Unknown, nullptr, Slot);
	}
}

void ULoadSaveCallbackProxy::OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame)
{
	if (Result == EPersistenceLoadResult::Success || Result == EPersistenceLoadResult::DoesNotExist)
	{
		OnSuccess.Broadcast(Result, Cast<USaveGameWorld>(SaveGame), Slot);
	}
	else
	{
		OnFailure.Broadcast(Result, Cast<USaveGameWorld>(SaveGame), Slot);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

ULoadProfileSaveCallbackProxy* ULoadProfileSaveCallbackProxy::LoadProfileSave(UObject* WorldContextObject)
{
	ULoadProfileSaveCallbackProxy* Ret = NewObject<ULoadProfileSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	return Ret;
}

void ULoadProfileSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->LoadProfileSave(FLoadSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast(EPersistenceLoadResult::Unknown, nullptr);
	}
}

void ULoadProfileSaveCallbackProxy::OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame)
{
	if (Result == EPersistenceLoadResult::Success || Result == EPersistenceLoadResult::DoesNotExist)
	{
		OnSuccess.Broadcast(Result, SaveGame);
	}
	else
	{
		OnFailure.Broadcast(Result, SaveGame);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UReadSaveCallbackProxy* UReadSaveCallbackProxy::ReadSave(UObject* WorldContextObject, int32 Slot)
{
	UReadSaveCallbackProxy* Ret = NewObject<UReadSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void UReadSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->ReadSave(Slot, FLoadSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast(EPersistenceLoadResult::Unknown, nullptr, Slot);
	}
}

void UReadSaveCallbackProxy::OnComplete(EPersistenceLoadResult Result, USaveGame* SaveGame)
{
	if (Result == EPersistenceLoadResult::Success)
	{
		OnSuccess.Broadcast(Result, Cast<USaveGameWorld>(SaveGame), Slot);
	}
	else
	{
		OnFailure.Broadcast(Result, Cast<USaveGameWorld>(SaveGame), Slot);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UHasSaveCallbackProxy* UHasSaveCallbackProxy::HasSave(UObject* WorldContextObject, int32 Slot)
{
	UHasSaveCallbackProxy* Ret = NewObject<UHasSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void UHasSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->HasSave(Slot, FHasSaveComplete::CreateUObject(this, &ThisClass::OnCompleteFunc));
	}
	else
	{
		OnComplete.Broadcast(EPersistenceHasResult::Unknown);
	}
}

void UHasSaveCallbackProxy::OnCompleteFunc(EPersistenceHasResult Result)
{
	OnComplete.Broadcast(Result);
}

//////////////////////////////////////////////////////////////////////////////////////////

UDeleteSaveCallbackProxy* UDeleteSaveCallbackProxy::DeleteSave(UObject* WorldContextObject, int32 Slot)
{
	UDeleteSaveCallbackProxy* Ret = NewObject<UDeleteSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void UDeleteSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->DeleteSave(Slot, FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast();
	}
}

void UDeleteSaveCallbackProxy::OnComplete(bool Result)
{
	if (Result)
	{
		OnSuccess.Broadcast();
	}
	else
	{
		OnFailure.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UDeleteProfileSaveCallbackProxy* UDeleteProfileSaveCallbackProxy::DeleteProfileSave(UObject* WorldContextObject)
{
	UDeleteProfileSaveCallbackProxy* Ret = NewObject<UDeleteProfileSaveCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	return Ret;
}

void UDeleteProfileSaveCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->DeleteProfileSave(FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnComplete));
	}
	else
	{
		OnFailure.Broadcast();
	}
}

void UDeleteProfileSaveCallbackProxy::OnComplete(bool Result)
{
	if (Result)
	{
		OnSuccess.Broadcast();
	}
	else
	{
		OnFailure.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UHasProfileBackupCallbackProxy* UHasProfileBackupCallbackProxy::HasProfileBackup(UObject* WorldContextObject)
{
	UHasProfileBackupCallbackProxy* Ret = NewObject<UHasProfileBackupCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	return Ret;
}

void UHasProfileBackupCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->HasProfileBackup(FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnCompleteFunc));
	}
	else
	{
		NoBackup.Broadcast();
	}
}

void UHasProfileBackupCallbackProxy::OnCompleteFunc(bool Result)
{
	if (Result)
	{
		HasBackup.Broadcast();
	}
	else
	{
		NoBackup.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

URestoreProfileBackupCallbackProxy* URestoreProfileBackupCallbackProxy::RestoreProfileBackup(UObject* WorldContextObject)
{
	URestoreProfileBackupCallbackProxy* Ret = NewObject<URestoreProfileBackupCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	return Ret;
}

void URestoreProfileBackupCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->RestoreProfileBackup(FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnCompleteFunc));
	}
	else
	{
		OnFailure.Broadcast();
	}
}

void URestoreProfileBackupCallbackProxy::OnCompleteFunc(bool Result)
{
	if (Result)
	{
		OnSuccess.Broadcast();
	}
	else
	{
		OnFailure.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UHasSlotBackupCallbackProxy* UHasSlotBackupCallbackProxy::HasSlotBackup(UObject* WorldContextObject, int32 Slot)
{
	UHasSlotBackupCallbackProxy* Ret = NewObject<UHasSlotBackupCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void UHasSlotBackupCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->HasSlotBackup(Slot, FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnCompleteFunc));
	}
	else
	{
		NoBackup.Broadcast();
	}
}

void UHasSlotBackupCallbackProxy::OnCompleteFunc(bool Result)
{
	if (Result)
	{
		HasBackup.Broadcast();
	}
	else
	{
		NoBackup.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

URestoreSlotBackupCallbackProxy* URestoreSlotBackupCallbackProxy::RestoreSlotBackup(UObject* WorldContextObject, int32 Slot)
{
	URestoreSlotBackupCallbackProxy* Ret = NewObject<URestoreSlotBackupCallbackProxy>();
	Ret->CachePersistenceManager(WorldContextObject);
	Ret->Slot = Slot;
	return Ret;
}

void URestoreSlotBackupCallbackProxy::Activate()
{
	if (PersistenceManager)
	{
		PersistenceManager->RestoreSlotBackup(Slot, FDeleteSaveComplete::CreateUObject(this, &ThisClass::OnCompleteFunc));
	}
	else
	{
		OnFailure.Broadcast();
	}
}

void URestoreSlotBackupCallbackProxy::OnCompleteFunc(bool Result)
{
	if (Result)
	{
		OnSuccess.Broadcast();
	}
	else
	{
		OnFailure.Broadcast();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////

UPersistenceManager* UPersistenceBlueprintFunctionLibrary::GetPersistenceManager(const UObject* WorldContextObject)
{
	return UPersistenceManager::GetInstance(WorldContextObject);
}

void UPersistenceBlueprintFunctionLibrary::CommitSave(const UObject* WorldContextObject, const FString Reason)
{
	if (UPersistenceManager* PersistenceManager = UPersistenceManager::GetInstance(WorldContextObject))
	{
		PersistenceManager->CommitSave(Reason);
	}
}

USaveGameWorld* UPersistenceBlueprintFunctionLibrary::GetCurrentSave(const UObject* WorldContextObject)
{
	if (const UPersistenceManager* PersistenceManager = UPersistenceManager::GetInstance(WorldContextObject))
	{
		return PersistenceManager->GetCurrentSave();
	}

	return nullptr;
}

USaveGameProfile* UPersistenceBlueprintFunctionLibrary::GetProfileSave(const UObject* WorldContextObject)
{
	if (const UPersistenceManager* PersistenceManager = UPersistenceManager::GetInstance(WorldContextObject))
	{
		return PersistenceManager->GetProfileSave();
	}

	return nullptr;
}

void UPersistenceBlueprintFunctionLibrary::SetDisableCommit(const UObject* WorldContextObject, bool DisableCommit)
{
	if (UPersistenceManager* PersistenceManager = UPersistenceManager::GetInstance(WorldContextObject))
	{
		PersistenceManager->SetDisableCommit(DisableCommit);
	}
}

AActor* UPersistenceBlueprintFunctionLibrary::GetReference(const UObject* WorldContextObject, FPersistentReference& Reference)
{
	if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
	{
		return Reference.GetReference(World);
	}

	return nullptr;
}
