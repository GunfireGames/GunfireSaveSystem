// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

//
// A helper for serializing FName's with less waste by only serializing the string once, regardless of how many times
// it's used.
//
struct FNameCache
{
public:
	// Adds a name to the cache and returns a unique id for looking it up on load
	int32 AddName(const FName& Name);

	FName GetName(int32 NameIndex) const;

	void Serialize(FArchive& Archive);

	void Reset();

private:
	TMap<FName, int32> NameMap;
	TArray<FName> Names;
};

//
// An archive that writes only properties with the SaveGame metadata. It can also handle writing out objects that
// contain references to other objects. It's only designed to support a single object plus all its referenced objects,
// not multiple unrelated objects.
//
struct GUNFIRESAVESYSTEM_API FSaveGameArchive final : private FObjectAndNameAsStringProxyArchive
{
public:
	// If you're going to be writing multiple save game archives sequentially, you can save space by passing in a shared
	// name cache instead of letting each archive write their own. It's up to the caller to serialize the shared cache.
	FSaveGameArchive(FArchive& InInnerArchive, FNameCache* SharedNameCache = nullptr);

	void SetNoDelta(bool NoDelta) { ArNoDelta = NoDelta; }

	// Call this before ReadBaseObject to get a list of any classes that need to be loaded, so you can load them in
	// advance. If you don't do this and any classes are unloaded, ReadBaseObject will block load them.
	bool GetClassesToLoad(TArray<FSoftObjectPath>& ClassesToLoad);

	// These are the only functions exposed, all the individual << serialization operators are intended for internal use
	// only.
	void WriteBaseObject(UObject* BaseObject, TMap<FName, bool>& ClassCache);
	void ReadBaseObject(UObject* BaseObject);

private:
	bool IsSharedNameCache() const { return &NameCache != &LocalNameCache; }
	uint32 WriteObjectAndLength(UObject* Object);
	void WriteComponents(AActor* Actor, TMap<FName, bool>& ClassCache);
	void ReadComponents(AActor* Actor);

	// Returns true if this class has any SaveGame flagged properties
	bool CheckClassNeedsSaving(UClass* Class, TMap<FName, bool>& ClassCache);

	// For visibility of the operators we don't override
	using FObjectAndNameAsStringProxyArchive::operator<<;

	virtual FArchive& operator<<(UObject*& Obj) override;
	virtual FArchive& operator<<(FName& N) override;

private:
	int32 Version = 0;
	int32 InitialOffset = 0;

	// Unique objects we've serialized
	TArray<UObject*> Objects;

	// A queue of objects waiting to be serialized
	TArray<UObject*> ObjectsToSerialize;

	FNameCache& NameCache;
	FNameCache LocalNameCache;
};
