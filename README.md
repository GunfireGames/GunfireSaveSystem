# Gunfire Save System

GunfireSaveSystem works with the stock Unreal ISaveGameSystem and SaveGame UPROPERTY flag to automatically persist save data for actors.

Save Types
----------

There are two types of saves: world saves and profile saves. World saves contain the save data for all actors in the world and you can have multiple world saves. There is one profile save and it is intended to hold save data that persists across all saves. For instance, you could store preferences here like the audio volume level. If your game has the concept of a single player character that persists across multiple runs of the game you could store the player data here too.

There is only ever one active world save. Any time a save is triggered both the world and the profile will be saved.

In project settings, Plugins, Gunfire Save System, you need to set the Save Game Class and Save Profile Class to the classes you created for this. You can also use the stock USaveGameWorld and USaveGameProfile classes if you don't have any customizations.

Saving Actor Properties
-----------------------

To save properties for an actor it needs to have a Persistence Component added to it. For the simplest case where you just want your properties with the SaveGame flag on them persisted that's all you need to do. There are some properties you can modify on the persistence component if you'd like the current transform saved, or to persist if the actor is destroyed.

Saving and Loading
------------------

To save/load/query the saves use the blueprint functions in the Persistence category. Most of the blueprint functions are async, so you'll need to wait for the results. You can also call the functions directly in UPersistenceManager.

TODO
----

The GunfireSaveSystem currently depends on having all levels use a custom level script actor to store the next persistence id. You could accomplish the same thing by adding a special actor to the level that stores the current value, or ideally just get rid of that requirement completely and scan through the level on load to find the current max and cache that.

Engine Modifications
--------------------

The persistence manager depends on some callbacks that aren't part of the stock Unreal Engine. These could potentially be refactored out with some work, but currently they're required.

In the callbacks section of World.h the following new callbacks should be added:

	// ~@Gunfire Begin ///////////////////////////////////////////////////////////////////

	// Called during PostLoad on a level.  Note: may be asynchronous.
	static FOnLevelChanged			LevelPostLoad;

	// Called prior to actors being initialized in a level.  Set CanInitialize to false to
	// prevent the level from initializing actors. This will be called once per tick until
	// CanInitialize is true.
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FCanLevelActorsInitialize, ULevel*, UWorld*, bool& /*CanInitialize*/);
	static FCanLevelActorsInitialize CanLevelActorsInitialize;

	// Called after all actors have been initialized in a level.
	static FOnLevelChanged			LevelActorsInitialized;

	// ~@Gunfire End /////////////////////////////////////////////////////////////////////

In ULevel::PostLoad, after "UWorldComposition::OnLevelPostLoad(this);" add:

	FWorldDelegates::LevelPostLoad.Broadcast(this, OwningWorld);

In UWorld::AddToWorld, modify the block "if (bExecuteNextStep && !Level->IsFinishedRouteActorInitialization())" to the following:

	// Route various initialization functions and set volumes.
	if (bExecuteNextStep && !Level->IsFinishedRouteActorInitialization())
	{
		// @Gunfire Begin
		bool CanInitialize = true;
		FWorldDelegates::CanLevelActorsInitialize.Broadcast(Level, this, CanInitialize);
		if (!CanInitialize)
		{
			bExecuteNextStep = false;
		}
		else
		{
		// @Gunfire End

		QUICK_SCOPE_CYCLE_COUNTER(STAT_AddToWorldTime_RouteActorInitialize);
		SCOPE_TIME_TO_VAR(&RouteActorInitializeTime);
		const int32 NumActorsToProcess = (!bConsiderTimeLimit || !IsGameWorld() || IsRunningCommandlet()) ? 0 : GLevelStreamingRouteActorInitializationGranularity;
		bStartup = 1;
		do 
		{
			Level->RouteActorInitialize(NumActorsToProcess);
		} while (!Level->IsFinishedRouteActorInitialization() && !IsTimeLimitExceeded(TEXT("routing Initialize on actors"), StartTime, Level, TimeLimit));
		bStartup = 0;

		bExecuteNextStep = Level->IsFinishedRouteActorInitialization() && (!bConsiderTimeLimit || !IsTimeLimitExceeded( TEXT("routing Initialize on actors"), StartTime, Level, TimeLimit ));

		// @Gunfire Begin
		if (bExecuteNextStep)
		{
			FWorldDelegates::LevelActorsInitialized.Broadcast(Level, this);
		}
		}
		// @Gunfire End
	}

Optionally, you can define a CreateDefaultSaveGameSystem callback, so you can better control where savegames go on Windows.

In SaveGameSystem.h, add the following to ISaveGameSystem:

	// @Gunfire Begin
	// Adding a way to override the default save game system for platforms that fall back
	// to the generic one. This needs to be hooked before the first call to
	// IPlatformFeaturesModule::GetSaveGameSystem. The pointer will never be deleted, so
	// you're expected to return a static instance, or expect that it'll leak on shutdown.
	DECLARE_DELEGATE_RetVal(ISaveGameSystem*, FCreateDefaultSaveGameSystem);
	static FCreateDefaultSaveGameSystem CreateDefaultSaveGameSystem;
	// @Gunfire End

Then in PlatformFeatures.cpp modify GetSaveGameSystem to this:

	// @Gunfire - It's kind of crappy to put this here, but SaveGameSystem doesn't have a cpp
	ISaveGameSystem::FCreateDefaultSaveGameSystem ISaveGameSystem::CreateDefaultSaveGameSystem;

	ISaveGameSystem* IPlatformFeaturesModule::GetSaveGameSystem()
	{
		// @Gunfire Begin
		static ISaveGameSystem* DefaultSaveGame = nullptr;

		if (DefaultSaveGame == nullptr)
		{
			if (ISaveGameSystem::CreateDefaultSaveGameSystem.IsBound())
			{
				DefaultSaveGame = ISaveGameSystem::CreateDefaultSaveGameSystem.Execute();
			}

			if (DefaultSaveGame == nullptr)
			{
				static FGenericSaveGameSystem GenericSaveGame;
				DefaultSaveGame = &GenericSaveGame;
			}
		}

		return DefaultSaveGame;
		// @Gunfire End
	}

Once you've done that you can go into WindowsSaveGameSystem.h and set USE_WINDOWS_SAVEGAMESYSTEM to 1 when PLATFORM_WINDOWS is defined. This will redirect Steam and EOS savegames to be in the Windows "Saved Games", and give you a hook where you can do things like put games in a subfolder based on the Steam user profile id.