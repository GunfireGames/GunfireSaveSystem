// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "SaveGameWorld.h"
#include "PersistenceComponent.h"

USaveGameWorld::USaveGameWorld()
	: UniqueIDGenerator(UPersistenceComponent::RUNTIME_BASE_UID)
{
}

uint64 USaveGameWorld::GenerateUniqueID()
{
	UniqueIDGenerator++;

	if (UniqueIDGenerator == UPersistenceComponent::INVALID_UID)
	{
		// It seems really unlikely that we would wrap around and still have id's in use,
		// but there's a chance we'll get an overlap if this happens.
		ensure(0);

		UniqueIDGenerator = UPersistenceComponent::RUNTIME_BASE_UID + 1;
	}

	return UniqueIDGenerator;
}
