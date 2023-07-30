// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "SaveGameArchive.h"
#include "PersistenceUtils.h"

#include "GameFramework/Actor.h"
#include "UObject/Package.h"

FObjectRefAndFNameArchive::FObjectRefAndFNameArchive(FArchive& InInnerArchive, bool bInLoadIfFindFails)
	: FObjectAndNameAsStringProxyArchive(InInnerArchive, bInLoadIfFindFails)
	, InitialOffset(0)
	, StringTableOffset(0)
{
	InitialOffset = Tell();

	*this << StringTableOffset;

	if (IsLoading())
	{
		ReadTable();
	}
}

FObjectRefAndFNameArchive::~FObjectRefAndFNameArchive()
{
	// TODO: Check that WriteTable was called
}

FArchive& FObjectRefAndFNameArchive::operator<<(class FName& N)
{
	static const int32 HAS_NUMBER = 1 << 15;

	uint16 Index = 0;
	int32 Number = 0;

	if (IsSaving())
	{
		// Object names are typically just dupes with the number set, so to avoid writing
		// a bunch of duplicate strings take the number out of the equation.
		FName NoNumberName = N;
		Number = NoNumberName.GetNumber();
		NoNumberName.SetNumber(0);

		if (const int32* ExistingIndex = NameMap.Find(NoNumberName))
		{
			Index = *ExistingIndex;
		}
		else
		{
			Index = NameMap.Num();
			NameMap.Add(NoNumberName, Index);
		}

		ensureMsgf((Index & HAS_NUMBER) == 0, TEXT("More than 32K unique names?"));

		// Most names don't have a number, so to save some space don't write the number
		// and just set the high bit on the index instead.
		if (Number != 0)
		{
			Index |= HAS_NUMBER;
		}
	}

	*this << Index;

	if ((Index & HAS_NUMBER) != 0)
	{
		Index &= ~HAS_NUMBER;

		*this << Number;
	}

	if (IsLoading())
	{
		N = Names[Index];
		N.SetNumber(Number);
	}

	return *this;
}

void FObjectRefAndFNameArchive::WriteTable()
{
	StringTableOffset = Tell();

	NameMap.ValueSort(TLess<int32>());

	int32 NumStrings = NameMap.Num();
	*this << NumStrings;

	FString SavedString;
	for (const TPair<FName, int32>& NamePair : NameMap)
	{
		SavedString = NamePair.Key.ToString();
		*this << SavedString;
	}

	NameMap.Empty();

	int64 EndOffset = Tell();

	Seek(InitialOffset);
	*this << StringTableOffset;

	Seek(EndOffset);
}

void FObjectRefAndFNameArchive::ReadTable()
{
	int64 CurrentPos = Tell();
	Seek(StringTableOffset);

	int32 NumStrings = 0;
	*this << NumStrings;
	Names.SetNum(NumStrings);

	FString SavedString;
	for (int32 i = 0; i < NumStrings; i++)
	{
		*this << SavedString;
		Names[i] = FName(*SavedString);
	}

	Seek(CurrentPos);
}

//////////////////////////////////////////////////////////////////////////////////////////

static const int32 GUNFIRE_SAVEGAME_ARCHIVE_VERSION = 1;

FSaveGameArchive::FSaveGameArchive(FArchive& InInnerArchive, bool NoDelta)
	: FObjectRefAndFNameArchive(InInnerArchive, true)
	, Version(GUNFIRE_SAVEGAME_ARCHIVE_VERSION)
{
	SetIsPersistent(true);
	ArIsSaveGame = true;
	ArNoDelta = NoDelta;

	*this << Version;
}

FArchive& FSaveGameArchive::operator<<(class UObject*& Obj)
{
	int32 ObjectID = -1;

	// If Obj is null we just skip storing it, and write the ObjectID as -1.
	if (IsSaving() && Obj)
	{
		// Have we written this object yet?
		ObjectID = Objects.Find(Obj);

		// If not, generate a unique id for referencing it and add it to our list of
		// objects to serialize.
		if (ObjectID == -1)
		{
			ObjectID = Objects.Add(Obj);
			ObjectsToSerialize.Add(Obj);
		}
	}

	*this << ObjectID;

	if (IsLoading())
	{
		if (Objects.IsValidIndex(ObjectID))
		{
			Obj = Objects[ObjectID];
		}
		else
		{
			Obj = nullptr;
		}
	}

	return *this;
}

FArchive& FSaveGameArchive::operator<<(struct FSoftObjectPtr& Value)
{
	*this << Value.GetUniqueID();

	return *this;
}

FArchive& FSaveGameArchive::operator<<(struct FSoftObjectPath& Value)
{
	FString Path = Value.ToString();

	*this << Path;

	if (IsLoading())
	{
		Value.SetPath(MoveTemp(Path));
	}

	return *this;
}

uint32 FSaveGameArchive::WriteObjectAndLength(UObject* Object)
{
	// Write out a stub for the size of the object data.  We'll rewrite it with
	// the correct size later.
	int64 ObjectLengthPos = Tell();
	uint32 ObjectLength = 0;
	*this << ObjectLength;

	Object->Serialize(*this);

	int64 ObjectEndPos = Tell();

	// Calculate the actual object size and seek back and rewrite it.
	ObjectLength = ObjectEndPos - ObjectLengthPos - sizeof(uint32);

	Seek(ObjectLengthPos);
	*this << ObjectLength;
	Seek(ObjectEndPos);

	return ObjectLength;
}

void FSaveGameArchive::WriteBaseObject(UObject* BaseObject, TMap<FName, bool>& ClassCache)
{
	// Write out a stub for the offset where our index for all the objects that were
	// written is.
	int64 StartPos = Tell();
	int64 ObjectIndexPos = 0;
	*this << ObjectIndexPos;

	// Seed our list of objects to serialize with the base object.
	Objects.Add(BaseObject);
	ObjectsToSerialize.Add(BaseObject);

	// Write out the base object, which will add any object properties it has to the
	// ObjectsToSerialize list.  Keep writing out objects until our list of objects to
	// serialize is empty (ie, we've recursed to the deepest objects).
	while (ObjectsToSerialize.Num() > 0)
	{
		UObject* Object = ObjectsToSerialize[0];
		ObjectsToSerialize.RemoveAt(0, 1, false);

		int32 ObjectID = Objects.Find(Object);
		*this << ObjectID;

		// UClasses don't respect the ArIsSaveGame flag and will write out a bunch of shit,
		// so we don't call Serialize on them. The relevant info (path of the class) will
		// still be written out.
		if (Object->IsA(UClass::StaticClass()))
		{
			uint32 ObjectLength = 0;
			*this << ObjectLength;

			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Class Ref '%s' [%s]"), *Object->GetName(), *Object->GetClass()->GetName());
		}
		else
		{
			uint32 ObjectLength = WriteObjectAndLength(Object);
			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Object '%s' [%s] - %d bytes"), *Object->GetName(), *Object->GetClass()->GetName(), ObjectLength);
		}

		uint8 IsActor = 0;

		// Actor components aren't marked as SaveGame, but we want to save any data in them
		if (AActor* Actor = Cast<AActor>(Object))
		{
			IsActor = 1;
			*this << IsActor;

			WriteComponents(Actor, ClassCache);
		}
		else
		{
			*this << IsActor;
		}
	}

	// Cache off the index start offset and go back and rewrite the correct value
	ObjectIndexPos = Tell();
	Seek(StartPos);
	*this << ObjectIndexPos;
	Seek(ObjectIndexPos);

	// Write out our index
	int32 NumUniqueObjects = Objects.Num();
	*this << NumUniqueObjects;

	for (int32 i = 0; i < NumUniqueObjects; i++)
	{
		UObject* Object = Objects[i];

		// If this is a placed object, just write out the path to the object
		if (Object->HasAllFlags(RF_WasLoaded))
		{
			uint8 WasLoaded = 1;
			*this << WasLoaded;

			// If this is the base object we don't need to write out the path, it'll be
			// passed in on load.
			if (i == 0)
			{
				TempString.Reset();
			}
			else
			{
				TempString = Object->GetPathName();
			}

			*this << TempString;
		}
		// Otherwise, write out the class and name so we can recreate it
		else
		{
			uint8 WasLoaded = 0;
			*this << WasLoaded;

			TempString = Object->GetClass()->GetPathName();
			*this << TempString;

			FName ObjectName = Object->GetFName();
			*this << ObjectName;

			// Write out the ID of the outer for this object (or -1 if the outer isn't an
			// object we're writing).
			// TODO: Should we write the full path if it's not something we're writing?
			int32 OuterID = Objects.Find(Object->GetOuter());
			verifyf(OuterID == -1 || OuterID < i, TEXT("Writing inner before outer"));
			*this << OuterID;
		}
	}

	Objects.SetNum(0);

	WriteTable();
}

bool FSaveGameArchive::GetClassesToLoad(TArray<FSoftObjectPath>& ClassesToLoad)
{
	int64 StartPos = Tell();

	int64 ObjectIndexPos;
	*this << ObjectIndexPos;

	Seek(ObjectIndexPos);

	int32 NumUniqueObjects;
	*this << NumUniqueObjects;

	for (int32 i = 0; i < NumUniqueObjects; i++)
	{
		uint8 WasLoaded;
		*this << WasLoaded;

		// Read in the path to the object or class
		*this << TempString;

		if (!WasLoaded)
		{
			FName ObjectName;
			*this << ObjectName;

			int32 OuterID;
			*this << OuterID;
		}

		UObject* Object = FindObject<UObject>(nullptr, *TempString, false);
		if (!Object)
		{
			ClassesToLoad.AddUnique(TempString);
		}
	}

	Seek(StartPos);

	return ClassesToLoad.Num() > 0;
}

void FSaveGameArchive::ReadBaseObject(UObject* BaseObject)
{
	int64 ObjectIndexPos;
	*this << ObjectIndexPos;
	int64 StartPos = Tell();

	Seek(ObjectIndexPos);

	int32 NumUniqueObjects;
	*this << NumUniqueObjects;
	Objects.SetNum(NumUniqueObjects);

	// Create all the unique objects in advance, so all the pointers are valid before
	// any objects are read in.
	for (int32 i = 0; i < Objects.Num(); i++)
	{
		uint8 WasLoaded;
		*this << WasLoaded;

		// Read in the path to the object or class
		*this << TempString;

		UObject* Object = nullptr;

		if (WasLoaded && i == 0)
		{
			Object = BaseObject;
		}
		else
		{
			Object = FindObject<UObject>(nullptr, *TempString, false);
			if (!Object)
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Block loading object '%s', this will cause hitches"), *TempString);
				Object = LoadObject<UClass>(nullptr, *TempString);
			}
		}

		if (WasLoaded)
		{
			Objects[i] = Object;
		}
		else
		{
			FName ObjectName;
			*this << ObjectName;

			int32 OuterID;
			*this << OuterID;

			// If we found the class, create the object and read it in
			if (UClass* Class = Cast<UClass>(Object))
			{
				// Try to look up the outer for this object.  If it doesn't exist, just use
				// the transient package.
				UObject* Outer = nullptr;
				if (Objects.IsValidIndex(OuterID))
					Outer = Objects[OuterID];
				if (Outer == nullptr)
					Outer = GetTransientPackage();

				if (i == 0)
				{
					// The first object in the list should always be the base object, so
					// instead of creating it use the passed in one.
					if (!Class->IsChildOf(BaseObject->GetClass()))
					{
						UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Savegame class changed, failing load"));
						return;
					}

					Objects[i] = BaseObject;
				}
				else
				{
					// Create a new instance
					Objects[i] = NewObject<UObject>(Outer, Class, ObjectName);
				}
			}
			else
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Couldn't find class '%s' for savegame object"), *TempString);
			}
		}
	}

	Seek(StartPos);

	// Now that all the objects are created, go back and read in their data
	for (int32 i = 0; i < Objects.Num(); i++)
	{
		int32 ObjectID;
		*this << ObjectID;

		uint32 ObjectLength;
		*this << ObjectLength;

		UObject* Object = nullptr;

		if (Objects.IsValidIndex(ObjectID) && Objects[ObjectID] && ObjectLength > 0)
		{
			Object = Objects[ObjectID];

			int64 ObjectStart = Tell();

			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("Reading object '%s' [%s]"), *Object->GetName(), *Object->GetClass()->GetName());

			Object->Serialize(*this);

			if (Tell() != ObjectStart + ObjectLength)
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Object '%s' [%s] didn't read all its data"), *Object->GetName(), *Object->GetClass()->GetName());
				Seek(ObjectStart + ObjectLength);
			}
		}
		else
		{
			Seek(Tell() + ObjectLength);
		}

		uint8 IsActor;
		*this << IsActor;

		if (IsActor)
			ReadComponents(Cast<AActor>(Object));
	}

	Objects.SetNum(0);
}

void FSaveGameArchive::WriteComponents(AActor* Actor, TMap<FName, bool>& ClassCache)
{
	// Write Component Data
	TInlineComponentArray<UActorComponent*> ActorComponents;
	Actor->GetComponents(ActorComponents);

	int32 ComponentCount = 0;

	for (UActorComponent* ActorComponent : ActorComponents)
	{
		if (CheckClassNeedsSaving(ActorComponent->GetClass(), ClassCache))
		{
			ComponentCount++;
		}
	}

	*this << ComponentCount;

	for (UActorComponent* ActorComponent : ActorComponents)
	{
		if (CheckClassNeedsSaving(ActorComponent->GetClass(), ClassCache))
		{
			// Grab the class name and use it as the InKey
			FString ComponentKey = ActorComponent->GetName();
			*this << ComponentKey;

			uint32 ComponentLength = WriteObjectAndLength(ActorComponent);
			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("    Component '%s' [%s] - %d bytes"), *ComponentKey, *ActorComponent->GetClass()->GetName(), ComponentLength);
		}
	}
}

void FSaveGameArchive::ReadComponents(AActor* Actor)
{
	// Read Component Data
	int32 ComponentCount;
	*this << ComponentCount;

	TInlineComponentArray<UActorComponent*> ActorComponents;

	if (Actor)
		Actor->GetComponents(ActorComponents);

	for (int i = 0; i < ComponentCount; i++)
	{
		FString ComponentKey;
		*this << ComponentKey;

		uint32 ComponentLength;
		*this << ComponentLength;

		bool FoundComponent = false;

		for (UActorComponent* ActorComponent : ActorComponents)
		{
			if (ActorComponent->GetName() == ComponentKey)
			{
				FoundComponent = true;

				UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Reading component '%s' [%s]"), *ComponentKey, *ActorComponent->GetClass()->GetName());

				ActorComponent->Serialize(*this);

				break;
			}
		}

		// If we didn't find the named component (got renamed or removed),
		// just skip over the data.
		if (!FoundComponent)
		{
			UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("  Missing component '%s', skipping %d bytes"), *ComponentKey, ComponentLength);

			Seek(Tell() + ComponentLength);
		}
	}
}

bool FSaveGameArchive::CheckClassNeedsSaving(UClass* Class, TMap<FName, bool>& ClassCache)
{
	// Ignore invalid classes
	if (Class == nullptr)
	{
		return false;
	}

	// Check if this class has been processed before
	if (const bool* NeedsSaving = ClassCache.Find(Class->GetFName()))
	{
		return *NeedsSaving;
	}

	bool NeedsSaving = false;

	// Check if any properties on this class are flagged to be saved
	for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
	{
		if (PropIt->PropertyFlags & CPF_SaveGame)
		{
			NeedsSaving = true;
			break;
		}
	}

	// If no save data was found on this class, check the super.
	if (!NeedsSaving)
	{
		NeedsSaving = CheckClassNeedsSaving(Class->GetSuperClass(), ClassCache);
	}

	// Cache the result so we know on future lookups whether this class does or
	// does not need to be saved
	ClassCache.Add(Class->GetFName(), NeedsSaving);

	return NeedsSaving;
}
