// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "GunfireSaveSystem.h"
#include "PersistenceComponent.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Engine/World.h"
#include "EngineUtils.h"

void FGunfireSaveSystemModule::StartupModule()
{
#if WITH_EDITOR
	// Listen for a map opening as marking existing layers dirty can only be done once loading has completed. Then we
	// check for persistence components that don't have a unique id and generate one (mainly just for stuff added
	// pre-format change, so the properties got nuked). This ensures the appropriate layers get dirtied, so they can be
	// saved without the user having to actively modify them.
	MapOpenedHandle = FEditorDelegates::OnMapOpened.AddLambda(
		[](const FString& Filename, bool bAsTemplate)
		{
			for (AActor* Actor : TActorRange<AActor>(GWorld))
			{
				if (UPersistenceComponent* Persistence = Actor->FindComponentByClass<UPersistenceComponent>())
				{
					if (Persistence->NeedsPersistentId())
					{
						Persistence->GetComponentLevel()->MarkPackageDirty();
					}
				}
			}
		});
#endif
}

void FGunfireSaveSystemModule::ShutdownModule()
{
#if WITH_EDITOR
	FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
#endif
}

#if UE_VERSION_NEWER_THAN(5, 3, 0)

ISaveGameSystem* FGunfireSaveSystemModule::GetSaveGameSystem()
{
#if USE_WINDOWS_SAVEGAMESYSTEM
	return &FWindowsSaveGameSystem::Get();
#else
	// If we're not using the Windows save system this shouldn't end up getting called.
	checkNoEntry();
	return nullptr;
#endif
}

#endif // UE_VERSION_NEWER_THAN(5, 3, 0)

IMPLEMENT_MODULE(FGunfireSaveSystemModule, GunfireSaveSystem)
