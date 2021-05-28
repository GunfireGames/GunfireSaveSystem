// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "LevelScriptActorGunfire.h"
#include "PersistenceComponent.h"
#include "PersistenceUtils.h"

ALevelScriptActorGunfire::ALevelScriptActorGunfire()
	: UniqueIDGenerator(UPersistenceComponent::INVALID_UID)
{
	bReplicates = false;
}

#if WITH_EDITOR

uint64 ALevelScriptActorGunfire::GenerateUniqueID()
{
	// Generate new id
	UniqueIDGenerator++;

	// We should never hit this limit, so if it happens crash the editor so we can see
	// what's going wrong with this level.
	verifyf(UniqueIDGenerator < UPersistenceComponent::RUNTIME_BASE_UID, TEXT("Ran out of unique ids"));

	// Mark associated level as dirty
	MarkPackageDirty();

	return UniqueIDGenerator;
}

void ALevelScriptActorGunfire::PostLoad()
{
	Super::PostLoad();

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		bool NeedsIDs = false;

		ULevel* Level = GetLevel();

		for (AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr)
			{
				UPersistenceComponent* Component = Actor->FindComponentByClass<UPersistenceComponent>();

				if (Component)
				{
					if (!Component->HasValidPersistentID() && Component->NeedsPersistentID())
					{
						UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Actor '%s' needs a persistent id"), *Actor->GetName());

						NeedsIDs = true;
					}
				}
			}
		}

		// If any actors need persistent ids (most likely old data) mark the level as
		// dirty so they'll get one generated.
		if (NeedsIDs)
			MarkPackageDirty();
	}
}

#endif // WITH_EDITOR
