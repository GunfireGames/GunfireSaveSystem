// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGunfireSaveSystem, Log, All);

class UPersistenceUtils
{
public:
	// Returns true if this actor has SaveGame flagged properties that differ from the
	// defaults (changed on the instance)
	static bool HasModifiedSaveProperties(class AActor* Actor);

private:
	static bool HasModifiedSaveProperties(UClass* Class, UObject* Obj);
};
