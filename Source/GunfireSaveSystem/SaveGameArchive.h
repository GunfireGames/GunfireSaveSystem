// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

//
// An archive that writes object references (ie, any UObject properties will only have a
// reference to the object saved, not anything on the object itself), and puts FNames into
// a lookup table.  This saves space when there are a lot of duplicate FNames being written.
//
// WriteTable must be called after all serialization is done, to write the string table.
//
struct FObjectRefAndFNameArchive : public FObjectAndNameAsStringProxyArchive
{
public:
	FObjectRefAndFNameArchive(FArchive& InInnerArchive, bool bInLoadIfFindFails);

	virtual ~FObjectRefAndFNameArchive();

	virtual FArchive& operator<<(class FName& N) override;

	void WriteTable();

private:
	void ReadTable();

private:
	int64 InitialOffset;
	int64 StringTableOffset;
	TMap<FName, int32> NameMap;
	TArray<FName> Names;
};

//
// An archive that writes only properties with the SaveGame metadata.  It can also handle
// writing out objects that contain references to other objects.
// It's only designed to support a single object plus all it's referenced objects, not
// multiple unrelated objects.
//
struct GUNFIRESAVESYSTEM_API FSaveGameArchive : private FObjectRefAndFNameArchive
{
public:
	FSaveGameArchive(FArchive& InInnerArchive, bool NoDelta = false);

	// Call this before ReadBaseObject to get a list of any classes that need to be loaded,
	// so you can load them in advance.  If you don't do this and any classes are unloaded,
	// ReadBaseObject will block load them.
	bool GetClassesToLoad(TArray<FSoftObjectPath>& ClassesToLoad);

	// These are the only functions exposed, all the individual << serialization operators
	// are intended for internal use only.
	void WriteBaseObject(UObject* BaseObject, TMap<FName, bool>& ClassCache);
	void ReadBaseObject(UObject* BaseObject);

	using FObjectRefAndFNameArchive::operator<<; // For visibility of the overloads we don't override

private:
	uint32 WriteObjectAndLength(UObject* Object);
	void WriteComponents(class AActor* Actor, TMap<FName, bool>& ClassCache);
	void ReadComponents(class AActor* Actor);
	// Returns true if this class has any SaveGame flagged properties
	bool CheckClassNeedsSaving(UClass* Class, TMap<FName, bool>& ClassCache);

	virtual FArchive& operator<<(class UObject*& Obj) override;
	virtual FArchive& operator<<(struct FSoftObjectPtr& Value) override;
	virtual FArchive& operator<<(struct FSoftObjectPath& Value) override;

private:
	int32 Version;
	FString TempString;
	TArray<UObject*> Objects;
	TArray<UObject*> ObjectsToSerialize;
};
