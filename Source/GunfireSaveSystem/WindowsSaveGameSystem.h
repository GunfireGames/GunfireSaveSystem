#pragma once

#include "SaveGameSystem.h"
#include "Misc/EngineVersionComparison.h"

#if PLATFORM_WINDOWS && !PLATFORM_WINGDK && UE_VERSION_NEWER_THAN(5, 3, 0)
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
// This requires the following lines in WindowsEngine.ini:
//
//  [PlatformFeatures]
//  SaveGameSystemModule = GunfireSaveSystem
//
class GUNFIRESAVESYSTEM_API FWindowsSaveGameSystem : public FGenericSaveGameSystem
{
public:
	FWindowsSaveGameSystem();
	virtual ~FWindowsSaveGameSystem() {}

	// Sets a suffix for the savegame path. This is intended to be used to differentiate between different users/game
	// stores, so it could be something like "Steam_<userid>".
	void SetUserFolder(const FStringView& UserFolderIn);

	// Sets a number of backups to keep per unique save name, and the interval in seconds to create new backups.
	void SetBackupSettings(int32 NumBackupsIn, double BackupIntervalSecondsIn);

	// Returns true if a backup of the specified save type exists.
	bool DoesBackupExist(const TCHAR* Name) const;

	// Returns true if the first available backup was restored. This will overwrite the current save and will rotate all
	// existing backups up the chain.
	bool RestoreBackup(const TCHAR* Name) const;

	// Overload to allow us to indicate whether we restored a save from a backup.
	ESaveExistsResult DoesSaveGameExistWithResult(const TCHAR* Name, const int32 UserIndex, bool& bRestoredFromBackup);

	// ISaveGameSystem Begin
	virtual ESaveExistsResult DoesSaveGameExistWithResult(const TCHAR* Name, const int32 UserIndex) override;
	virtual bool SaveGame(bool bAttemptToUseUI, const TCHAR* Name, const int32 UserIndex, const TArray<uint8>& Data) override;
	virtual FString GetSaveGamePath(const TCHAR* Name) override;
	// ISaveGameSystem End

	static FWindowsSaveGameSystem& Get();

protected:
	void RotateBackups(const TCHAR* Name, const FStringView BasePath, const FStringView SavePath);
	void GetSaveGamePath(const TCHAR* Name, TStringBuilderBase<TCHAR>& OutPath) const;
	void GetBackupSaveGamePath(const FStringView BasePath, int32 Revision, TStringBuilderBase<TCHAR>& OutPath) const;

	FString SavedGamesDir;
	FString UserFolder;

	int32 NumBackups = 0;
	double BackupIntervalSeconds = 60.0 * 10.0;
	TMap<FString, double> LastBackupTime;
};

#endif // USE_WINDOWS_SAVEGAMESYSTEM
