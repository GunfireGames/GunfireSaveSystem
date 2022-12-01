// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "PersistenceComponent.h"
#include "PersistenceContainer.h"
#include "PersistenceManager.h"
#include "PersistenceUtils.h"

UPersistenceComponent::UPersistenceComponent()
{
	bWantsInitializeComponent = true;
}

inline bool UPersistenceComponent::IsPersistableObject(AActor* pObject) const
{
	return (pObject != nullptr && !pObject->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
#if WITH_EDITOR
		// In the editor, ignore the preview object
		&& !pObject->HasAllFlags(RF_Transactional | RF_Transient)
		&& pObject->GetLevel()->GetFlags() != RF_Transactional
#endif
		);
}

#if WITH_EDITOR

void UPersistenceComponent::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	HasModifiedSaveValues = UPersistenceUtils::HasModifiedSaveProperties(GetOwner());

	GeneratePersistentID();
}

#endif

void UPersistenceComponent::InitializeComponent()
{
	Super::InitializeComponent();

	if (ShouldPersist())
	{
		// With rapid loading / unloading of levels. Actors can still be  
		// initializing their components after the level unload delegate has fired.
		// Their outer will be marked as pending kill, so bail out early if that is true.
		if (AActor* Parent = Cast<AActor>(GetOuter()))
		{
			if (Parent->IsPendingKillPending())
			{
				return;
			}
		}

		UPersistenceContainer* Container = nullptr;

		UPersistenceManager* Manager = UPersistenceManager::GetInstance(this);
		if (Manager != nullptr)
		{
			Container = Manager->GetContainer(this);
		}

		// If there isn't a valid persistent id this must be a dynamic spawn
		if (!HasValidPersistentID())
		{
			IsDynamic = true;

			// If the actor is being spawned by a save game being loaded its persistent id
			// is cached in the container, so grab it from there.
			if (Container)
			{
				UniqueID = Container->GetDynamicActorID();
			}

			// If we didn't find an id this must be the first spawn, go ahead and create
			// one now.
			if (UniqueID == INVALID_UID)
			{
				GeneratePersistentID();
			}
		}

		if (HasValidPersistentID() && Container)
		{
			// Attempt to load the saved data into the parent object
			Container->LoadData(this, *Manager);
		}
	}
}

void UPersistenceComponent::UninitializeComponent()
{
	if (HasBegunPlay())
	{
		const FString ActorName = GetOwner() ? GetOwner()->GetName() : TEXT("<no-owner>");
		UE_LOG(LogGunfireSaveSystem, Error, TEXT("[UPersistenceComponent] We're about to crash! Attempting to uninitialize persistence component for '%s' without ending play first! Did you destroy an actor on its own BeginPlay?"), *ActorName);
	}

	Super::UninitializeComponent();
}

void UPersistenceComponent::BeginPlay()
{
	RegisterWithManager(true);

	Super::BeginPlay();
}

void UPersistenceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ShouldPersist() && HasValidPersistentID()) 
	{
		if (UPersistenceManager* Manager = UPersistenceManager::GetInstance(this))
		{
			if ((EndPlayReason == EEndPlayReason::Destroyed) && PersistDestroyed)
			{
				// Don't need to do anything if a dynamic object is destroyed, next time
				// the container is saved it'll be removed. We also ignore objects already
				// being destroyed to avoid setting them as destroyed twice.
				if (!IsDynamic && !bHasBeenDestroyed)
				{
					// Because this is a static actor placed on the map in the editor,
					// we can't just remove the actor data.  We actually have to mark
					// that the actor has been destroyed, so upon map load the actor
					// will be gracefully removed
					Manager->SetComponentDestroyed(this);
				}
			}
			else if (!SaveKey.IsNone())
			{
				// If this container uses a save key it won't be auto-saved by the level
				// unloading.  Force a save manually.
				Manager->WriteComponent(this);
			}
		}
	}

	RegisterWithManager(false);

	Super::EndPlay(EndPlayReason);
}

void UPersistenceComponent::RegisterWithManager(bool bEnable)
{
	if (ShouldPersist())
	{
		if (HasValidPersistentID())
		{
			if (UPersistenceManager* PersistenceManager = UPersistenceManager::GetInstance(this))
			{
				if (bEnable)
				{
					PersistenceManager->Register(this);
				}
				else
				{
					PersistenceManager->Unregister(this);
				}
			}
		}
		else
		{
			UE_CLOG(!TempOverridePersist, LogGunfireSaveSystem, Warning, TEXT("Actor '%s' doesn't have a valid persistent id."), *GetOwner()->GetName());
		}
	}
}

bool UPersistenceComponent::NeedsPersistentID() const
{
#if WITH_EDITOR
	if (GCompilingBlueprint)
	{
		return false;
	}
#endif

	// Only generate a new ID if our current one is invalid
	if (!HasValidPersistentID())
	{
		if (GetWorld() == nullptr)
		{
			// Invalid world
			return false;
		}

		if (!IsPersistableObject(GetOwner()))
		{
			// Invalid world/owner
			return false;
		}

		return true;
	}

	return false;
}

void UPersistenceComponent::OnLevelChanged(ULevel* OldLevel)
{
	UPersistenceManager::GetInstance(this)->OnLevelChanged(this, OldLevel);
}

void UPersistenceComponent::GeneratePersistentID()
{
	if (NeedsPersistentID())
	{
		UniqueID = UPersistenceManager::GeneratePID(GetComponentLevel());
	}
}

bool UPersistenceComponent::ShouldPersist() const
{
	if (TempOverridePersist)
	{
		return false;
	}

	AActor* Owner = GetOwner();

	if (!Owner || !Owner->GetWorld())
	{
		UE_LOG(LogGunfireSaveSystem, Error, TEXT("[UPersistenceComponent] Owner or world does not exist!"));
		return false;
	}

	return
		Owner->GetNetMode() < NM_Client &&
		(!Owner->HasAnyFlags(RF_Transient) || !SaveKey.IsNone());
}

void UPersistenceComponent::SetOverridePersist(bool Persist)
{ 
	TempOverridePersist = Persist; 
	IsDynamic = !Persist;
}

void UPersistenceComponent::DestroyPersistentActor()
{
	GetOwner()->SetLifeSpan(0.01f);
	bHasBeenDestroyed = true;
}
