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

	// Sent before a ULevel is removed from the world via UWorld::RemoveFromWorld or 
	// LoadMap (a NULL object means the LoadMap case, because all levels will be 
	// removed from the world without a RemoveFromWorld call for each)
	static FOnLevelChanged			LevelPreRemoveFromWorld;

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

In UWorld::RemoveFromWorld, at the beginning of the block "if ( bFinishRemovingLevel )" add:

	FWorldDelegates::LevelPreRemoveFromWorld.Broadcast(Level, this);

In UWorld::AddToWorld, modify the block "if( bExecuteNextStep && !Level->bAlreadyRoutedActorInitialize )" to the following:

	// Route various initialization functions and set volumes.
	if( bExecuteNextStep && !Level->bAlreadyRoutedActorInitialize )
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
		bStartup = 1;
		Level->RouteActorInitialize();
		Level->bAlreadyRoutedActorInitialize = true;
		bStartup = 0;

		bExecuteNextStep = (!bConsiderTimeLimit || !IsTimeLimitExceeded( TEXT("routing Initialize on actors"), StartTime, Level, TimeLimit ));

		// @Gunfire Begin
		FWorldDelegates::LevelActorsInitialized.Broadcast(Level, this);
		}
		// @Gunfire End
	}


