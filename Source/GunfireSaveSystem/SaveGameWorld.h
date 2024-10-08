// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "SaveGamePersistence.h"
#include "SaveGameWorld.generated.h"

//
// The save game class for persistent world data. Any data from persistence components will be automatically saved in
// here. If there is project specific data this can be subclassed and new data added as properties (with the SaveGame
// flag set).
//
UCLASS()
class GUNFIRESAVESYSTEM_API USaveGameWorld : public USaveGamePersistence
{
	friend class UPersistenceManager;

	GENERATED_BODY()

public:
	UPROPERTY(SaveGame, BlueprintReadOnly)
	bool RequiresFullGame = false;

protected:
	// Save data for each level in the world with persistent actors. Containers will also be created for actors that use
	// a save key.
	UPROPERTY(SaveGame, BlueprintReadOnly)
	TArray<TObjectPtr<class UPersistenceContainer>> Containers;
};
