#include "WindowsSaveGameSystem.h"

#if USE_WINDOWS_SAVEGAMESYSTEM

#include "HAL/FileManagerGeneric.h"
#include "Misc/PathViews.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include "Windows/HideWindowsPlatformTypes.h"

FWindowsSaveGameSystem& FWindowsSaveGameSystem::Get()
{
	static FWindowsSaveGameSystem System;
	return System;
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

bool FWindowsSaveGameSystem::DoesBackupExist(const TCHAR* Name) const
{
	TStringBuilder<MAX_PATH> SavePath, BackupPath;

	GetSaveGamePath(Name, SavePath);
	const FStringView BasePath = FPathViews::GetBaseFilenameWithPath(SavePath);

	for (int32 i = 1; i <= NumBackups; ++i)
	{
		GetBackupSaveGamePath(BasePath, i, BackupPath);

		const bool bBackupExists = IFileManager::Get().FileExists(*BackupPath);
		if (bBackupExists)
		{
			return true;
		}
	}

	return false;
}

bool FWindowsSaveGameSystem::RestoreBackup(const TCHAR* Name) const
{
	IPlatformFile& PlatformFile = IPlatformFile::GetPlatformPhysical();
	TStringBuilder<MAX_PATH> SavePath, DestPath, SrcPath;

	GetSaveGamePath(Name, SavePath);
	const FStringView BasePath = FPathViews::GetBaseFilenameWithPath(SavePath);

	// If we are restoring then something has gone wrong with the current save. Just rename it, prior to restoring a
	// backup, to signify that it is corrupt. This way it can potentially be evaluated by the dev team later on.
	if (PlatformFile.FileExists(*SavePath))
	{
		const FDateTime ModDateTime = FFileManagerGeneric::Get().GetTimeStamp(*SavePath);

		DestPath.Appendf(TEXT("%s_%s.corrupt"), *SavePath, *ModDateTime.ToString());

		PlatformFile.MoveFile(*DestPath, *SavePath);
	}

	// Keep track of our destination in case there are missing backups. For example, if bak1 is missing but bak2 and
	// bak3 are available, this will ensure that bak2 becomes the main save and bak3 is put in the bak1 slot.
	int32 DestRevision = 0;
	bool bResult = false;

	for (int32 i = 1; i <= NumBackups; ++i)
	{
		if (DestRevision == 0)
		{
			DestPath = SavePath.GetData();
		}
		else
		{
			GetBackupSaveGamePath(BasePath, DestRevision, DestPath);
		}

		GetBackupSaveGamePath(BasePath, i, SrcPath);

		if (PlatformFile.FileExists(*SrcPath))
		{
			if (PlatformFile.FileExists(*DestPath))
			{
				PlatformFile.DeleteFile(*DestPath);
			}

			const bool bFileMoved = PlatformFile.MoveFile(*DestPath, *SrcPath);

			// If we are overwriting the active save, store the result.
			if (DestRevision == 0)
			{
				bResult = bFileMoved;
			}

			DestRevision++;
		}
	}

	return bResult;
}

ISaveGameSystem::ESaveExistsResult FWindowsSaveGameSystem::DoesSaveGameExistWithResult(const TCHAR* Name, const int32 UserIndex, bool& bRestoredFromBackup)
{
	ESaveExistsResult Result;

	bRestoredFromBackup = false;

	do
	{
		Result = FGenericSaveGameSystem::DoesSaveGameExistWithResult(Name, UserIndex);

		// If the save game is corrupt, attempt to restore a backup and try again.
		if (Result == ESaveExistsResult::Corrupt && RestoreBackup(Name))
		{
			bRestoredFromBackup = true;
		}
		else
		{
			break;
		}
	}
	while (bRestoredFromBackup);

	// If we've successfully loaded a backup, mark the result as 'Restored'.
	if (bRestoredFromBackup && Result == ESaveExistsResult::OK)
	{
		bRestoredFromBackup = true;
	}

	return Result;
}

ISaveGameSystem::ESaveExistsResult FWindowsSaveGameSystem::DoesSaveGameExistWithResult(const TCHAR* Name, const int32 UserIndex)
{
	bool bRestoredFromBackup;
	return DoesSaveGameExistWithResult(Name, UserIndex, bRestoredFromBackup);
}

bool FWindowsSaveGameSystem::SaveGame(bool bAttemptToUseUI, const TCHAR* Name, const int32 UserIndex, const TArray<uint8>& Data)
{
	TStringBuilder<MAX_PATH> SavePath, TempPath;

	GetSaveGamePath(Name, SavePath);
	const FStringView BasePath = FPathViews::GetBaseFilenameWithPath(SavePath);
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
	TStringBuilder<MAX_PATH> CurrentBackupPath, NextBackupPath;

	for (int32 i = NumBackups - 1; i >= 0; --i)
	{
		if (i == 0)
		{
			CurrentBackupPath = SavePath;
		}
		else
		{
			GetBackupSaveGamePath(BasePath, i, CurrentBackupPath);	
		}

		GetBackupSaveGamePath(BasePath, i + 1, NextBackupPath);

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
	TStringBuilder<MAX_PATH> TempString;
	GetSaveGamePath(Name, TempString);

	return TempString.ToString();
}

void FWindowsSaveGameSystem::GetSaveGamePath(const TCHAR* Name, TStringBuilderBase<TCHAR>& OutPath) const
{
	OutPath = SavedGamesDir;
	OutPath.AppendChar(TEXT('/'));

	if (UserFolder.Len() > 0)
	{
		OutPath.Append(UserFolder);
		OutPath.AppendChar(TEXT('/'));
	}

	OutPath.Append(Name);
	OutPath.Append(TEXT(".sav"));
}

void FWindowsSaveGameSystem::GetBackupSaveGamePath(const FStringView BasePath, int32 Revision, TStringBuilderBase<TCHAR>& OutPath) const
{
	if (NumBackups > 0)
	{
		OutPath = BasePath;
		OutPath.Appendf(TEXT(".bak%d"), Revision);
	}
}

#endif // USE_WINDOWS_SAVEGAMESYSTEM
