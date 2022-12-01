// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "WindowsSaveGameSystem.h"

class FGunfireSaveSystemModule : public IModuleInterface
{
	typedef FGunfireSaveSystemModule ThisClass;

public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

#if USE_WINDOWS_SAVEGAMESYSTEM
	FWindowsSaveGameSystem WindowsSaveGameSystem;
#endif

#if WITH_EDITOR
protected:
	class ALevelScriptActorGunfire* GetBlueprintLevelScriptActor(class UBlueprint* Blueprint);
	void ReparentLevel(ULevel* InLevel, UWorld* InWorld);

	void OnLevelAddedToWorld(ULevel* InLevel, UWorld* InWorld);
	void OnMapOpened(const FString& Filename, bool bAsTemplate);
	void OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS);
	void OnBlueprintPreCompile(class UBlueprint* Blueprint);
	void OnBlueprintCompiled(class UBlueprint* Blueprint);

	uint64 CurrentLevelActorUniqueID = 0;

	// Cache of levels that need to be re-parented after the map has loaded
	TArray<ULevel*> DeferredReparentedLevels;
#endif
};
