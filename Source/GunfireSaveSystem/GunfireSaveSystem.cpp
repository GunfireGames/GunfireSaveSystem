// Copyright Gunfire Games, LLC. All Rights Reserved.

#include "GunfireSaveSystem.h"
#include "PersistenceManager.h"
#include "PersistenceComponent.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UnrealEdGlobals.h"
#endif

#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelScriptActorGunfire.h"

#define LOCTEXT_NAMESPACE "FGunfirePersistenceModule"

void FGunfireSaveSystemModule::StartupModule()
{
#if WITH_EDITOR
	// Listen for world creation, so we can do some deferred initialization
	FWorldDelegates::OnPreWorldInitialization.AddRaw(this, &FGunfireSaveSystemModule::OnWorldCreated);

	// Listen for a map opening as marking existing layers dirty can only be done once
	// loading has completed. This is also the point at which persistent ids are generated
	// for objects that had a PersistenceComponent to their blueprint but the map has not
	// yet been saved.  This ensures the appropriate layers get dirtied so they can be
	// saved without the user having to actively modify them.
	FEditorDelegates::OnMapOpened.AddRaw(this, &FGunfireSaveSystemModule::OnMapOpened);

	// Listen for any level changes so we can re-parent them to our custom type
	FWorldDelegates::LevelAddedToWorld.AddRaw(this, &FGunfireSaveSystemModule::OnLevelAddedToWorld);
#endif
}

void FGunfireSaveSystemModule::ShutdownModule()
{
#if WITH_EDITOR
	if (GEditor)
		GEditor->OnBlueprintPreCompile().RemoveAll(this);
#endif
}

#if WITH_EDITOR

// Ensures all levels added are parented to our custom type
void FGunfireSaveSystemModule::OnLevelAddedToWorld(ULevel* InLevel, UWorld* InWorld)
{
	ReparentLevel(InLevel, InWorld);
}

// After the map has opened, dirty all deferred levels and re-parent the persistent level
void FGunfireSaveSystemModule::OnMapOpened(const FString& Filename, bool bAsTemplate)
{
	for (int l = 0; l < DeferredReparentedLevels.Num(); ++l)
	{
		// Mark the level as dirty to ensure this change is saved
		DeferredReparentedLevels[l]->MarkPackageDirty();
	}
	DeferredReparentedLevels.Empty();

	// Make sure the persistent level is updated as well
	if (GWorld != nullptr)
	{
		ReparentLevel(GWorld->PersistentLevel, GWorld);
	}

	// Now iterate over all actors and dirty any levels that have a persistence
	// component without a puid to ensure they get generated on save
	for (TActorIterator<AActor> It(GWorld); It; ++It)
	{
		AActor* pActor = *It;
		if (UPersistenceComponent* pPersistence = pActor->FindComponentByClass<UPersistenceComponent>())
		{
			if (pPersistence->NeedsPersistentID())
			{
				pPersistence->GetComponentLevel()->MarkPackageDirty();
			}
		}
	}
}

// Handles re-parenting any level to our custom level type.
void FGunfireSaveSystemModule::ReparentLevel(ULevel* InLevel, UWorld* InWorld)
{
	if (InLevel == nullptr || InWorld == nullptr)
	{
		return;
	}

	ALevelScriptActor* pLevelScriptActor = InLevel->GetLevelScriptActor();

	// Determine custom class
	UClass* pBaseClass = ALevelScriptActorGunfire::StaticClass();  // As a fall-back, use ABaseLeveScriptActor
	if (GEngine->LevelScriptActorClass != nullptr)
	{
		pBaseClass = GEngine->LevelScriptActorClass;
	}

	UBlueprint* pBlueprintObj = (UBlueprint*)InLevel->GetLevelScriptBlueprint();
	check(pBlueprintObj != nullptr);  // GetLevelScriptBlueprint should have created this for us

	// If the new level isn't of our custom type, convert it now
	if (pLevelScriptActor == NULL || pBlueprintObj->ParentClass != pBaseClass)
	{
		if (InWorld->IsGameWorld())
		{
			// If playing, and a level has not of our custom parent type, complain.
			UE_LOG(LogBlueprint, Error, TEXT("Cannot reparent level blueprint %s from %s to %s during runtime; Aborting!"), *pBlueprintObj->GetFullName(), *pBlueprintObj->ParentClass->GetName(), *pBaseClass->GetName());
			return;
		}

		UE_LOG(LogBlueprint, Warning, TEXT("Reparenting level blueprint %s from %s to %s..."),
			*pBlueprintObj->GetFullName(),
			pBlueprintObj->ParentClass ? *pBlueprintObj->ParentClass->GetName() : TEXT("(null)"),
			*pBaseClass->GetName());

		// Re-parent the blueprint class
		pBlueprintObj->ParentClass = pBaseClass;

		// Compile the blueprint now that the parent has changed.
		FKismetEditorUtilities::CompileBlueprint(pBlueprintObj);

		if (GIsEditorLoadingPackage)
		{
			DeferredReparentedLevels.Add(InLevel);
		}
		else
		{
			InLevel->MarkPackageDirty();
		}
	}
}

ALevelScriptActorGunfire* FGunfireSaveSystemModule::GetBlueprintLevelScriptActor(UBlueprint* Blueprint)
{
	if (Blueprint->ParentClass &&
		Blueprint->ParentClass->IsChildOf(ALevelScriptActorGunfire::StaticClass()))
	{
		if (ULevel* Level = Cast<ULevel>(Blueprint->GetOuter()))
		{
			return Cast<ALevelScriptActorGunfire>(Level->GetLevelScriptActor());
		}
	}

	return nullptr;
}

void FGunfireSaveSystemModule::OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS)
{
	static bool bInitialized = false;

	// GUnrealEd isn't valid when the module starts, so we wait to register this callback
	// until the initial world create (should happen before you even see the editor).
	if (!bInitialized && GUnrealEd)
	{
		bInitialized = true;
		GEditor->OnBlueprintPreCompile().AddRaw(this, &FGunfireSaveSystemModule::OnBlueprintPreCompile);
	}
}

void FGunfireSaveSystemModule::OnBlueprintPreCompile(UBlueprint* Blueprint)
{
	// Every time the level script blueprint is recompiled our properties get trashed.
	// Cache them off and restore after the compile is done.
	if (ALevelScriptActorGunfire* ScriptActor = GetBlueprintLevelScriptActor(Blueprint))
	{
		Blueprint->OnCompiled().AddRaw(this, &FGunfireSaveSystemModule::OnBlueprintCompiled);
		CurrentLevelActorUniqueID = ScriptActor->UniqueIDGenerator;
	}
}

void FGunfireSaveSystemModule::OnBlueprintCompiled(UBlueprint* Blueprint)
{
	if (ALevelScriptActorGunfire* ScriptActor = GetBlueprintLevelScriptActor(Blueprint))
	{
		ScriptActor->UniqueIDGenerator = CurrentLevelActorUniqueID;
		CurrentLevelActorUniqueID = 0;
	}
	else
	{
		ensureMsgf(false, TEXT("Didn't get the level script actor, going to lose our persistent id"));
	}

	Blueprint->OnCompiled().RemoveAll(this);
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FGunfireSaveSystemModule, GunfireSaveSystem)