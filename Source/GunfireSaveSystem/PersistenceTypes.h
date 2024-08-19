// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PersistenceTypes.generated.h"

UENUM(BlueprintType)
enum class EPersistenceLoadResult : uint8
{
	// Loaded successfully
	Success,

	// There is no save in this slot. If this is a load call a new save will be created and returned, if it's a read
	// save call a null save will be returned.
	DoesNotExist,

	// The save is corrupt, the only option should be to delete it.
	Corrupt,

	// There was a corrupt save in the slot, but it was replaced with a backup.
	Restored,

	// This save was saved with a newer build of the game, inform the user to install updates.
	TooNew,

	Unknown,
};

UENUM(BlueprintType)
enum class EPersistenceHasResult : uint8
{
	// The slot is empty
	Empty,

	// There is a valid save in the slot
	Exists,

	// There is a corrupt save in the slot
	Corrupt,

	// There was a corrupt save in the slot, but it was replaced with a backup.
	Restored,

	// There was an unknown error checking the slot
	Unknown,
};

UENUM(BlueprintType)
enum class EPersistenceSaveResult : uint8
{
	// Saved successfully
	Success,

	// Saving is currently disabled
	Disabled,

	// Save request was ignored because another save is running or queued
	Busy,

	// Failed for an unknown reason
	Unknown,
};

USTRUCT()
struct GUNFIRESAVESYSTEM_API FPersistenceKey
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FName ContainerKey;

	UPROPERTY(SaveGame)
	FGuid PersistentId;

	bool Equals(const FPersistenceKey& Other) const
	{
		return (ContainerKey == Other.ContainerKey) && (PersistentId == Other.PersistentId);
	}

	bool IsValid() const { return PersistentId.IsValid(); }
};

// The default property saving code is pretty dumb and will write an array of uint8's one byte at a time with a ton of
// overhead. To work around that, we define our own serializer that does it the optimal way.
USTRUCT()
struct GUNFIRESAVESYSTEM_API FPersistenceBlob
{
	GENERATED_BODY()

	TArray<uint8> Data;

	bool Serialize(FArchive& Ar)
	{
		Ar << Data;
		return true;
	}

	bool operator==(const FPersistenceBlob& Other) const
	{
		return Data == Other.Data;
	}
};

template<>
struct TStructOpsTypeTraits<FPersistenceBlob> : public TStructOpsTypeTraitsBase2<FPersistenceBlob>
{
	enum
	{
		WithSerializer = true,
		// Need this so the struct serializer knows if we changed and need to be written
		WithIdenticalViaEquality = true,
	};
};