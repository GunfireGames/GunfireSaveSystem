// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "Engine/LevelScriptActor.h"
#include "LevelScriptActorGunfire.generated.h"

//
// We use this to store level specific data.  It's saved with the level, but gets trashed
// every time the level blueprint is compiled.  See FGunfireSaveSystemModule::OnBlueprintPreCompile
// for how we preserve the data.
//
UCLASS()
class GUNFIRESAVESYSTEM_API ALevelScriptActorGunfire : public ALevelScriptActor
{
	friend class FGunfireSaveSystemModule;

	GENERATED_BODY()

public:
	ALevelScriptActorGunfire();

#if WITH_EDITOR
	uint64 GenerateUniqueID();
	virtual void PostLoad() override;
#endif

private:
	// The source of all unique editor IDs
	UPROPERTY(EditAnywhere, Category = Persistence)
	uint64 UniqueIDGenerator;
};
