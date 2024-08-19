// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "SaveGameArchive.h"
#include "PersistenceUtils.h"

#include "GameFramework/Actor.h"
#include "UObject/Package.h"

static const int32 GUNFIRE_SAVEGAME_ARCHIVE_VERSION = 1;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int32 FNameCache::AddName(const FName& Name)
{
	if (const int32* Index = NameMap.Find(Name))
	{
		return *Index;
	}

	const int32 Index = Names.Num();

	NameMap.Add(Name, Index);
	Names.Add(Name);

	return Index;
}

FName FNameCache::GetName(int32 NameIndex) const
{
	if (Names.IsValidIndex(NameIndex))
	{
		return Names[NameIndex];
	}

	return NAME_None;
}

void FNameCache::Serialize(FArchive& Ar)
{
	int32 NumStrings = Names.Num();
	Ar << NumStrings;
	Names.SetNum(NumStrings);

	FString SavedString;

	for (FName& CurName : Names)
	{
		if (Ar.IsSaving())
		{
			CurName.ToString(SavedString);
		}

		Ar << SavedString;

		if (Ar.IsLoading())
		{
			CurName = FName(*SavedString);
		}
	}
}

void FNameCache::Reset()
{
	NameMap.Reset();
	Names.Reset();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FSaveGameArchive::FSaveGameArchive(FArchive& InInnerArchive, FNameCache* SharedNameCache)
	: FObjectAndNameAsStringProxyArchive(InInnerArchive, true)
	, NameCache(SharedNameCache ? *SharedNameCache : LocalNameCache)
{
	SetIsPersistent(true);
	ArIsSaveGame = true;

	// We're always saving in the current format
	if (IsSaving())
	{
		Version = GUNFIRE_SAVEGAME_ARCHIVE_VERSION;
	}

	*this << Version;

	InitialOffset = static_cast<int32>(Tell());

	// If we're not using a shared name cache, read in the name cache or reserve the space for it
	if (!IsSharedNameCache())
	{
		int32 NameCacheOffset = 0;
		*this << NameCacheOffset;

		if (IsLoading())
		{
			const int64 CurrentPos = Tell();
			Seek(NameCacheOffset);

			NameCache.Serialize(*this);

			Seek(CurrentPos);
		}
	}
}

FArchive& FSaveGameArchive::operator<<(UObject*& Obj)
{
	int32 ObjectIndex = -1;

	// If Obj is null we just skip storing it, and write the ObjectId as -1.
	if (IsSaving() && Obj)
	{
		// Have we written this object yet?
		ObjectIndex = Objects.Find(Obj);

		// If not, add it to our list of objects to serialize and use the index as a lookup.
		if (ObjectIndex == -1)
		{
			ObjectIndex = Objects.Add(Obj);
			ObjectsToSerialize.Add(Obj);
		}
	}

	*this << ObjectIndex;

	if (IsLoading())
	{
		if (Objects.IsValidIndex(ObjectIndex))
		{
			Obj = Objects[ObjectIndex];
		}
		else
		{
			Obj = nullptr;
		}
	}

	return *this;
}


uint32 FSaveGameArchive::WriteObjectAndLength(UObject* Object)
{
	// Write out a stub for the size of the object data. We'll rewrite it with the correct size later.
	const int64 ObjectLengthPos = Tell();
	uint32 ObjectLength = 0;
	*this << ObjectLength;

	Object->Serialize(*this);

	const int64 ObjectEndPos = Tell();

	// Calculate the actual object size and seek back and rewrite it.
	ObjectLength = ObjectEndPos - ObjectLengthPos - sizeof(uint32);

	Seek(ObjectLengthPos);
	*this << ObjectLength;
	Seek(ObjectEndPos);

	return ObjectLength;
}

void FSaveGameArchive::WriteBaseObject(UObject* BaseObject, TMap<FName, bool>& ClassCache)
{
	// Write out a stub for the offset where our index for all the objects that were written is.
	const int64 StartPos = Tell();
	int64 ObjectIndexPos = 0;
	*this << ObjectIndexPos;

	// Seed our list of objects to serialize with the base object.
	Objects.Add(BaseObject);
	ObjectsToSerialize.Add(BaseObject);

	// Write out the base object, which will add any object properties it has to the ObjectsToSerialize list. Keep
	// writing out objects until our list of objects to serialize is empty (ie, we've recursed to the deepest objects).
	while (ObjectsToSerialize.Num() > 0)
	{
		UObject* Object = ObjectsToSerialize[0];
		ObjectsToSerialize.RemoveAt(0, 1, false);

		int32 ObjectIndex = Objects.Find(Object);
		*this << ObjectIndex;

		// UClasses don't respect the ArIsSaveGame flag and will write out a bunch of shit, so we don't call Serialize
		// on them. The relevant info (path of the class) will still be written out.
		if (Object->IsA(UClass::StaticClass()))
		{
			uint32 ObjectLength = 0;
			*this << ObjectLength;

			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Class Ref '%s' [%s]"),
				*FNameBuilder(Object->GetFName()), *FNameBuilder(Object->GetClass()->GetFName()));
		}
		else
		{
			const uint32 ObjectLength = WriteObjectAndLength(Object);
			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Object '%s' [%s] - %d bytes"),
				*FNameBuilder(Object->GetFName()), *FNameBuilder(Object->GetClass()->GetFName()), ObjectLength);
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

			FSoftObjectPath ObjectPath;

			// If this is the base object we don't need to write out the path, it'll be passed in on load.
			if (i != 0)
			{
				ObjectPath = Object;
			}

			*this << ObjectPath;
		}
		// Otherwise, write out the class and name so we can recreate it
		else
		{
			uint8 WasLoaded = 0;
			*this << WasLoaded;

			FSoftObjectPath ClassPath(Object->GetClass());
			*this << ClassPath;

			// Write out the object name too, so when we recreate this object on load we can keep the same name
			FName ObjectName = Object->GetFName();
			*this << ObjectName;

			// Write out the index of the outer for this object (or -1 if the outer isn't an object we're writing).
			// TODO: Should we write the full path if it's not something we're writing?
			int32 OuterIndex = Objects.Find(Object->GetOuter());
			verifyf(OuterIndex == INDEX_NONE || OuterIndex < i, TEXT("Writing inner before outer"));
			*this << OuterIndex;
		}
	}

	Objects.SetNum(0);

	// If we're not using a shared name cache, write ours out then go back and update the name cache offset
	if (!IsSharedNameCache())
	{
		int32 NameCacheOffset = static_cast<int32>(Tell());

		NameCache.Serialize(*this);

		const int64 EndOffset = Tell();

		Seek(InitialOffset);
		*this << NameCacheOffset;

		Seek(EndOffset);
	}
}

bool FSaveGameArchive::GetClassesToLoad(TArray<FSoftObjectPath>& ClassesToLoad)
{
	const int64 StartPos = Tell();

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
		FSoftObjectPath ObjectPath;
		*this << ObjectPath;

		// At this point we don't need the name or index, so just read them in and throw them away.
		if (!WasLoaded)
		{
			FName ObjectName;
			*this << ObjectName;

			int32 OuterIndex;
			*this << OuterIndex;
		}

		if (ObjectPath.ResolveObject() == nullptr)
		{
			ClassesToLoad.AddUnique(ObjectPath);
		}
	}

	Seek(StartPos);

	return ClassesToLoad.Num() > 0;
}

void FSaveGameArchive::ReadBaseObject(UObject* BaseObject)
{
	int64 ObjectIndexPos;
	*this << ObjectIndexPos;
	const int64 StartPos = Tell();

	Seek(ObjectIndexPos);

	int32 NumUniqueObjects;
	*this << NumUniqueObjects;
	Objects.SetNum(NumUniqueObjects);

	// Create all the unique objects in advance, so all the pointers are valid before any objects are read in.
	for (int32 i = 0; i < Objects.Num(); i++)
	{
		uint8 WasLoaded;
		*this << WasLoaded;

		// Read in the path to the object or class
		FSoftObjectPath ObjectPath;
		*this << ObjectPath;

		UObject* Object = nullptr;

		if (WasLoaded && i == 0)
		{
			Object = BaseObject;
		}
		else
		{
			Object = ObjectPath.ResolveObject();
			if (!Object)
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Block loading object '%s', this will cause hitches"), *ObjectPath.ToString());
				Object = ObjectPath.TryLoad();
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

			int32 OuterIndex;
			*this << OuterIndex;

			// If we found the class, create the object and read it in
			if (UClass* Class = Cast<UClass>(Object))
			{
				UObject* Outer = nullptr;

				// Try to look up the outer for this object. If it doesn't exist, just use the transient package.
				if (Objects.IsValidIndex(OuterIndex))
				{
					Outer = Objects[OuterIndex];
				}
				if (Outer == nullptr)
				{
					Outer = GetTransientPackage();
				}

				if (i == 0)
				{
					// The first object in the list should always be the base object, so instead of creating it use the
					// passed in one.
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
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Couldn't find class '%s' for savegame object"), *ObjectPath.ToString());
			}
		}
	}

	Seek(StartPos);

	// Now that all the objects are created, go back and read in their data
	for (int32 i = 0; i < Objects.Num(); i++)
	{
		int32 ObjectIndex;
		*this << ObjectIndex;

		uint32 ObjectLength;
		*this << ObjectLength;

		UObject* Object = nullptr;

		if (Objects.IsValidIndex(ObjectIndex) && Objects[ObjectIndex] && ObjectLength > 0)
		{
			Object = Objects[ObjectIndex];

			const int64 ObjectStart = Tell();

			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("Reading object '%s' [%s]"),
				*FNameBuilder(Object->GetFName()), *FNameBuilder(Object->GetClass()->GetFName()));

			Object->Serialize(*this);

			if (Tell() != ObjectStart + ObjectLength)
			{
				UE_LOG(LogGunfireSaveSystem, Warning, TEXT("Object '%s' [%s] didn't read all its data"),
					*FNameBuilder(Object->GetFName()), *FNameBuilder(Object->GetClass()->GetFName()));

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
		{
			ReadComponents(Cast<AActor>(Object));
		}
	}

	Objects.SetNum(0);
}

void FSaveGameArchive::WriteComponents(AActor* Actor, TMap<FName, bool>& ClassCache)
{
	// Write Component Data
	TInlineComponentArray<UActorComponent*> ActorComponents(Actor);

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
			// Grab the class name and use it as the key
			FName ComponentKey = ActorComponent->GetFName();
			*this << ComponentKey;

			const uint32 ComponentLength = WriteObjectAndLength(ActorComponent);

			UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("    Component '%s' [%s] - %d bytes"),
				*FNameBuilder(ComponentKey), *FNameBuilder(ActorComponent->GetClass()->GetFName()), ComponentLength);
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
	{
		Actor->GetComponents(ActorComponents);
	}

	for (int i = 0; i < ComponentCount; i++)
	{
		FName ComponentKey;
		*this << ComponentKey;

		uint32 ComponentLength;
		*this << ComponentLength;

		bool FoundComponent = false;

		for (UActorComponent* ActorComponent : ActorComponents)
		{
			if (ActorComponent->GetFName() == ComponentKey)
			{
				FoundComponent = true;

				UE_LOG(LogGunfireSaveSystem, VeryVerbose, TEXT("  Reading component '%s' [%s]"),
					*FNameBuilder(ComponentKey), *FNameBuilder(ActorComponent->GetClass()->GetFName()));

				ActorComponent->Serialize(*this);

				break;
			}
		}

		// If we didn't find the named component (got renamed or removed), just skip over the data.
		if (!FoundComponent)
		{
			UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("  Missing component '%s', skipping %d bytes"), *FNameBuilder(ComponentKey), ComponentLength);

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

	// Cache the result so we know on future lookups whether this class does or does not need to be saved
	ClassCache.Add(Class->GetFName(), NeedsSaving);

	return NeedsSaving;
}

FArchive& FSaveGameArchive::operator<<(FName& N)
{
	constexpr int32 HAS_NUMBER = 1 << 15;

	uint16 Index = 0;
	int32 Number = 0;

	if (IsSaving())
	{
		// Object names are typically just dupes with the number set, so to avoid writing a bunch of duplicate strings
		// take the number out of the equation.
		FName NoNumberName = N;
		Number = NoNumberName.GetNumber();
		NoNumberName.SetNumber(0);

		Index = NameCache.AddName(NoNumberName);

		// If we ever hit this it's most likely a bug. If it's legit, we'd need to change the index to be 32 bit.
		ensureMsgf((Index & HAS_NUMBER) == 0, TEXT("More than 32K unique names?"));

		// Most names don't have a number, so to save some space don't write the number and just set the high bit on the
		// index instead.
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
		N = NameCache.GetName(Index);
		N.SetNumber(Number);
	}

	return *this;
}
