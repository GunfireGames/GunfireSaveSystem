// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "SaveGamePersistence.h"
#include "SaveGameProfile.generated.h"

USTRUCT()
struct FSaveGameUserSetting
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	FString Key;

	UPROPERTY(SaveGame)
	FString Value;
};

USTRUCT()
struct FSaveGameAchievementProgress
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	FName AchievementId;

	UPROPERTY(SaveGame)
	int32 Value = 0;

	int32 UnlockValue = 0;

	bool OutOfSync = false;
};

// The base class for profile saves
UCLASS()
class GUNFIRESAVESYSTEM_API USaveGameProfile : public USaveGamePersistence
{
	GENERATED_BODY()

public:
	// The game user settings, for platforms where they can't be saved in an ini (consoles)
	UPROPERTY(SaveGame)
	TArray<FSaveGameUserSetting> UserSettings;

	UPROPERTY(SaveGame)
	TArray<FSaveGameAchievementProgress> AchievementProgress;

	void ClearUserSettings() { UserSettings.Empty(); }
};
