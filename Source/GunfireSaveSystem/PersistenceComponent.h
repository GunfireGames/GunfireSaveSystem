// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"

#include "UObject/ObjectSaveContext.h"

#include "PersistenceComponent.generated.h"

//
// A Persistence Component should be added to any actor that needs to
// persist data in save games.  It will automatically save any properties
// flagged with SaveGame on the actor or any components.
//
UCLASS(meta = (BlueprintSpawnableComponent))
class GUNFIRESAVESYSTEM_API UPersistenceComponent : public UActorComponent
{
	friend class UPersistenceContainer;
	friend class UPersistenceManager;

	GENERATED_BODY()

public:
	UPersistenceComponent();

	// Determines if the persistent ID is valid or not
	bool HasValidPersistentID() const { return (UniqueID != INVALID_UID) || !SaveKey.IsNone(); }

	// Return true if we should persist
	bool ShouldPersist() const;

	// Determines whether this object needs a persistent id or not.
	bool NeedsPersistentID() const;

	// Used to halt persistence on a dynamic object who would normally persist. DO NOT USE LIGHTLY.
	void SetOverridePersist(bool Override);

	// This needs to be called with the old level when a persistent actor is moved to a
	// new level.
	void OnLevelChanged(ULevel* OldLevel);
	
protected:
	inline bool IsPersistableObject(AActor* pObject) const;

#if WITH_EDITOR
	// Called when the owner for this component is being saved
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

	virtual void InitializeComponent() override;
	virtual void UninitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void RegisterWithManager(bool bEnable);

	// Generates a persistent id for the owning actor
	void GeneratePersistentID();

protected:
	// An automatically generated unique id for looking up save data.
	UPROPERTY(VisibleInstanceOnly, NonPIEDuplicateTransient, Category = Persistence)
	uint64 UniqueID = INVALID_UID;

	// The name of the save container to store save data for this actor in.  If this is
	// not set a value will be automatically generated based on the level name.  Typically
	// this should not be set, it's for special cases such as the player character, which
	// need to be persisted but aren't placed in a specific level.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = Persistence)
	FName SaveKey;

	// True if this actor has any saved properties set to different values from the
	// defaults (ie, they are editable on instances and they were changed on this instance).
	UPROPERTY(VisibleInstanceOnly, Category = Persistence)
	bool HasModifiedSaveValues = false;

	// Adding override for actors who normally need to persist, but need to avoid
	// persistence at certain key times. 
	// DO NOT USE THIS LIGHTLY, or dynamic actors may be lost.
	bool TempOverridePersist = false;

	bool IsDynamic = false;

	bool bHasBeenDestroyed = false;

	// Public so they can be set in constructors, but these shouldn't be changed at
	// runtime otherwise.
public:
	// Set to true if you want to persist the transform of this actor.
	UPROPERTY(EditDefaultsOnly, Category = Persistence)
	bool PersistTransform = false;

	// If true, this object will persist when it's destroyed, and on a subsequent load of
	// the map the object will be removed.
	UPROPERTY(EditDefaultsOnly, Category = Persistence)
	bool PersistDestroyed = false;

	// Invalid unique ID.  The generator will skip this.
	static const uint64 INVALID_UID = 0;

	// Objects generated at run time have the most significant bit set so they don't overlap
	// with editor generated ids
	static const uint64 RUNTIME_BASE_UID = 0x8000000000000000;

	// Destroys the owner of this component safely. Called from the persistence container for
	// persistent actors that should persist being destroyed.
	void DestroyPersistentActor();

	// Returns if this actor has been latently destroyed due to persistence
	bool HasBeenDestroyed() const { return bHasBeenDestroyed; }
};
