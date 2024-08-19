// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "GameFramework/SaveGame.h"
#include "SaveGamePersistence.generated.h"

UCLASS(Abstract)
class GUNFIRESAVESYSTEM_API USaveGamePersistence : public USaveGame
{
	GENERATED_BODY()

public:
	// Called just before a save is committed, can be used to add save-related data (i.e. playtime, location, etc.)
	UFUNCTION(BlueprintImplementableEvent, Category = "Persistence")
	void PreCommit(class UPersistenceManager* PersistenceManager);

	virtual void PreCommitNative(class UPersistenceManager* PersistenceManager) {}
};
