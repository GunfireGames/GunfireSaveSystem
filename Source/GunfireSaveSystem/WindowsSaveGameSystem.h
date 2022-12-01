#pragma once

#include "SaveGameSystem.h"

#if PLATFORM_WINDOWS
// Set this to 1 if you've made the required edits to SaveGameSystem (see README.md)
#define USE_WINDOWS_SAVEGAMESYSTEM 0
#else
#define USE_WINDOWS_SAVEGAMESYSTEM 0
#endif

#if USE_WINDOWS_SAVEGAMESYSTEM

//
// This save game system is designed for Windows builds that don't have a platform
// specific override for savegames (ie, Steam and EOS, but not GDK). It overrides the
// default savegame location to be in the Windows "Saved Games" folder instead of buried
// in app data, and also has the ability to have a directory suffix so we don't put
// savegames for different Steam users into the same folder.
//
class FWindowsSaveGameSystem : public FGenericSaveGameSystem
{
public:
	FWindowsSaveGameSystem();

	virtual ~FWindowsSaveGameSystem() {}

	// Sets a suffix for the savegame path. This is intended to be used to differentiate
	// between different users/game stores, so it could be something like "Steam_<userid>".
	void SetUserFolder(const FStringView& UserFolderIn);

protected:
	virtual FString GetSaveGamePath(const TCHAR* Name) override;

	FString SavedGamesDir;
	FString UserFolder;
};

#endif // USE_WINDOWS_SAVEGAMESYSTEM
