#include "WindowsSaveGameSystem.h"

#if USE_WINDOWS_SAVEGAMESYSTEM

#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include "Windows/HideWindowsPlatformTypes.h"

#include "PlatformFeatures.h"

FWindowsSaveGameSystem::FWindowsSaveGameSystem()
{
#if !WITH_EDITOR
	if (FPaths::ShouldSaveToUserDir())
	{
		TCHAR* SavedGamesPath = nullptr;

		// Get the Windows save game folder (User\Saved Games)
		HRESULT Ret = SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &SavedGamesPath);
		if (SUCCEEDED(Ret))
		{
			SavedGamesDir = FPaths::Combine(SavedGamesPath, FApp::GetProjectName());
			FPaths::NormalizeFilename(SavedGamesDir);

			CoTaskMemFree(SavedGamesPath);
		}

		// This shouldn't ever happen, if it does we need to fix it
		checkf(!SavedGamesDir.IsEmpty(), TEXT("Unable to get save game path"));

		// Hook the callback so when there's no platform defined save game system and it
		// falls back to the generic one it'll create ours instead.
		ISaveGameSystem::CreateDefaultSaveGameSystem.BindLambda(
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
#endif // !WITH_EDITOR
}

void FWindowsSaveGameSystem::SetUserFolder(const FStringView& UserFolderIn)
{
	UserFolder = UserFolderIn;
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

