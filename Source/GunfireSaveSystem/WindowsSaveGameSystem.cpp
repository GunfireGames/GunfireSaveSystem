#include "WindowsSaveGameSystem.h"
#include "GunfireSaveSystem.h"

#include "Misc/PathViews.h"

#if USE_WINDOWS_SAVEGAMESYSTEM

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "PlatformFeatures.h"

FWindowsSaveGameSystem& FWindowsSaveGameSystem::Get()
{
	FGunfireSaveSystemModule& SaveSystem = FModuleManager::GetModuleChecked<FGunfireSaveSystemModule>("GunfireSaveSystem");
	return SaveSystem.WindowsSaveGameSystem;
}

FWindowsSaveGameSystem::FWindowsSaveGameSystem()
{
	// If we're saving to the user dir (appdata), put our save files in the Windows "Saved Games" folder instead.
	// The ini files and other saved data will still go in the app data folder, this is just for savegames.
	if (FPaths::ShouldSaveToUserDir())
	{
		TCHAR* SavedGamesPath = nullptr;

		// Get the Windows save game folder (User\Saved Games)
		HRESULT Ret = SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &SavedGamesPath);
		if (SUCCEEDED(Ret))
		{
			SavedGamesDir = FPaths::Combine(SavedGamesPath, FApp::GetProjectName());
			FPaths::NormalizeFilename(SavedGamesDir);

			CoTaskMemFree(SavedGamesPath);
		}

		// This shouldn't ever happen, if it does we need to fix it
		ensureMsgf(!SavedGamesDir.IsEmpty(), TEXT("Unable to get save game path"));
	}

	// If we're not saving to the saved games dir (or couldn't get it for some reason), use the default location.
	if (SavedGamesDir.IsEmpty())
	{
		SavedGamesDir = FString::Printf(TEXT("%sSaveGames"), *FPaths::ProjectSavedDir());
	}

	// Hook the callback so when there's no platform defined save game system and it
	// falls back to the generic one it'll create ours instead.
	CreateDefaultSaveGameSystem.BindLambda(
		[&]()
		{
			return this;
		});

	// Now that we've bound our save game system creation callback, force the save
	// game system to be instantiated so we can ensure that it's ours. If it isn't we
	// need to move our callback registration earlier.
	ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem();

	ensureMsgf(SaveSystem == this, TEXT("We didn't override the save game system like we expected"));
}

void FWindowsSaveGameSystem::SetUserFolder(const FStringView& UserFolderIn)
{
	UserFolder = UserFolderIn;
}

void FWindowsSaveGameSystem::SetBackupSettings(int32 NumBackupsIn, double BackupIntervalSecondsIn)
{
	NumBackups = NumBackupsIn;
	BackupIntervalSeconds = BackupIntervalSecondsIn;
}

bool FWindowsSaveGameSystem::SaveGame(bool bAttemptToUseUI, const TCHAR* Name, const int32 UserIndex, const TArray<uint8>& Data)
{
	const FString SavePath = GetSaveGamePath(Name);
	const FStringView BasePath = FPathViews::GetBaseFilenameWithPath(SavePath);

	TStringBuilder<128> TempPath;
	TempPath = BasePath;
	TempPath.Append(TEXT(".tmp"));

	// First, write the save to a temp file. This lessens the risk of a freak power outage or crash catching us with
	// partially written data.
	if (FFileHelper::SaveArrayToFile(Data, *TempPath))
	{
		// Before we overwrite our current save, give the backup function a chance to back it up
		RotateBackups(Name, BasePath, SavePath);

		IPlatformFile& PlatformFile = IPlatformFile::GetPlatformPhysical();

		// Delete the existing save if it exists
		if (PlatformFile.FileExists(*SavePath))
		{
			PlatformFile.DeleteFile(*SavePath);
		}

		// Move the new save from the temp file to the final location
		return PlatformFile.MoveFile(*SavePath, *TempPath);
	}

	return false;
}

void FWindowsSaveGameSystem::RotateBackups(const TCHAR* Name, const FStringView BasePath, const FStringView SavePath)
{
	// If we're not backing up saves we're done
	if (NumBackups <= 0)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();

	double* LastTime = LastBackupTime.Find(Name);

	// If this is the first time we've saved don't do backups. That way if someone is starting and shutting down the
	// game a bunch they won't wipe all their backups. We wait until they've been playing for our backup interval before
	// doing our first rotation.
	if (LastTime == nullptr)
	{
		LastBackupTime.Add(Name, CurrentTime);
		return;
	}

	// If our backup interval hasn't passed since the last save, don't rotate
	if (*LastTime + BackupIntervalSeconds > CurrentTime)
	{
		return;
	}

	*LastTime = CurrentTime;

	IPlatformFile& PlatformFile = IPlatformFile::GetPlatformPhysical();

	for (int32 i = NumBackups - 1; i >= 0; --i)
	{
		TStringBuilder<128> CurrentBackupPath, NextBackupPath;

		if (i == 0)
		{
			CurrentBackupPath = SavePath;
		}
		else
		{
			CurrentBackupPath = BasePath;
			CurrentBackupPath.Appendf(TEXT(".bak%d"), i);
		}

		NextBackupPath = BasePath;
		NextBackupPath.Appendf(TEXT(".bak%d"), i + 1);

		if (PlatformFile.FileExists(*CurrentBackupPath))
		{
			if (PlatformFile.FileExists(*NextBackupPath))
			{
				PlatformFile.DeleteFile(*NextBackupPath);
			}

			// If we're rotating the actual save to the first backup, just to be extra safe copy the file instead of
			// moving it. We want to minimize the amount of time where we have no save file.
			if (i == 0)
			{
				PlatformFile.CopyFile(*NextBackupPath, *CurrentBackupPath);

				const FDateTime FileTime = PlatformFile.GetTimeStamp(*CurrentBackupPath);

				// Copying the file resets the time to the current time, so to make it more clear to the user, copy the
				// timestamp from the old file to the new one.
				if (FileTime != FDateTime::MinValue())
				{
					PlatformFile.SetTimeStamp(*NextBackupPath, FileTime);
				}
			}
			else
			{
				PlatformFile.MoveFile(*NextBackupPath, *CurrentBackupPath);
			}
		}
	}
}

FString FWindowsSaveGameSystem::GetSaveGamePath(const TCHAR* Name)
{
	TStringBuilder<128> TempString;

	TempString.Append(SavedGamesDir);
	TempString.AppendChar(TEXT('/'));

	if (UserFolder.Len() > 0)
	{
		TempString.Append(UserFolder);
		TempString.AppendChar(TEXT('/'));
	}

	TempString.Append(Name);
	TempString.Append(TEXT(".sav"));

	return TempString.ToString();
}

#endif // USE_WINDOWS_SAVEGAMESYSTEM
