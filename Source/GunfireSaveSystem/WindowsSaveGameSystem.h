#pragma once

#include "SaveGameSystem.h"

#if PLATFORM_WINDOWS
// Set this to 1 if you've made the required edits to SaveGameSystem (see README.md)
#define USE_WINDOWS_SAVEGAMESYSTEM 1
#else
#define USE_WINDOWS_SAVEGAMESYSTEM 0
#endif

#if USE_WINDOWS_SAVEGAMESYSTEM

//
// This save game system is designed for Windows builds that don't have a platform specific override for savegames (ie,
// Steam and EOS, but not GDK). It overrides the default savegame location to be in the Windows "Saved Games" folder
// instead of buried in app data, and also has the ability to have a directory suffix, so we don't put savegames for
// different users/game stores into the same folder.
//
class GUNFIRESAVESYSTEM_API FWindowsSaveGameSystem : public FGenericSaveGameSystem
{
public:
	FWindowsSaveGameSystem();
	virtual ~FWindowsSaveGameSystem() {}

	// Sets a suffix for the savegame path. This is intended to be used to differentiate between different users/game
	// stores, so it could be something like "Steam_<userid>".
	void SetUserFolder(const FStringView& UserFolderIn);

	// Sets a number of backups to keep per unique save name, and the interval in seconds to create new backups. This
	// isn't something where you can programmatically roll back to a backup, it's just intended for emergency cases
	// where a user has lost or corrupted their save somehow and can try renaming a backup.
	void SetBackupSettings(int32 NumBackupsIn, double BackupIntervalSecondsIn);

	// ISaveGameSystem Begin
	virtual bool SaveGame(bool bAttemptToUseUI, const TCHAR* Name, const int32 UserIndex, const TArray<uint8>& Data) override;
	virtual FString GetSaveGamePath(const TCHAR* Name) override;
	// ISaveGameSystem End

	static FWindowsSaveGameSystem& Get();

protected:
	void RotateBackups(const TCHAR* Name, const FStringView BasePath, const FStringView SavePath);

	FString SavedGamesDir;
	FString UserFolder;

	int32 NumBackups = 0;
	double BackupIntervalSeconds = 60.0 * 10.0;
	TMap<FString, double> LastBackupTime;
};

#endif // USE_WINDOWS_SAVEGAMESYSTEM
