// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "PersistenceUtils.h"

#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY(LogGunfireSaveSystem);

bool UPersistenceUtils::HasModifiedSaveProperties(AActor* Actor)
{
	if (Actor == nullptr)
	{
		return false;
	}

	bool HasNonDefaultSaveValues = HasModifiedSaveProperties(Actor->GetClass(), Actor);

	if (!HasNonDefaultSaveValues)
	{
		for (UActorComponent* Component : TInlineComponentArray<UActorComponent*>(Actor))
		{
			HasNonDefaultSaveValues = HasModifiedSaveProperties(Component->GetClass(), Component);

			if (HasNonDefaultSaveValues)
				break;
		}
	}

	return HasNonDefaultSaveValues;
}

bool UPersistenceUtils::HasModifiedSaveProperties(UClass* Class, UObject* Obj)
{
	if (Class == nullptr)
	{
		return false;
	}

	// We only care about object instances, not default objects
	if (Obj->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return false;
	}

	bool HasNonDefault = false;
	UObject* DiffObject = Obj->GetArchetype();

	for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
	{
		if (PropIt->PropertyFlags & CPF_SaveGame)
		{
			const uint8* DataPtr = PropIt->ContainerPtrToValuePtr<uint8>(Obj, 0);
			const uint8* DefaultValue = PropIt->ContainerPtrToValuePtrForDefaults<uint8>(Class, DiffObject, 0);

			if (!PropIt->Identical(DataPtr, DefaultValue))
			{
				UE_LOG(LogGunfireSaveSystem, Verbose, TEXT("Found non-default property '%s' on obj '%s'"), *PropIt->GetName(), *Obj->GetPathName());
				HasNonDefault = true;
				break;
			}
		}
	}

	if (!HasNonDefault)
	{
		HasNonDefault = HasModifiedSaveProperties(Class->GetSuperClass(), Obj);
	}

	return HasNonDefault;
}
