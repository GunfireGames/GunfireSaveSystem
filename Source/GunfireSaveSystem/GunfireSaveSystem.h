// Copyright Gunfire Games, LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "WindowsSaveGameSystem.h"
#include "Misc/EngineVersionComparison.h"

#if UE_VERSION_NEWER_THAN(5, 3, 0)
class FGunfireSaveSystemModule : public ISaveGameSystemModule
#else
class FGunfireSaveSystemModule : public IModuleInterface
#endif
{
	typedef FGunfireSaveSystemModule ThisClass;

public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

#if UE_VERSION_NEWER_THAN(5, 3, 0)
	virtual ISaveGameSystem* GetSaveGameSystem() override;
#endif

#if WITH_EDITOR
protected:
	FDelegateHandle MapOpenedHandle;
#endif
};
