# Steam Audio's runtime: phonon.dll goes beside the game client, which loads it by hand
# and has its own binaural mix without it. Only the headers are in the repository.
include(FetchContent)
FetchContent_Declare(kcds_steamaudio
  URL "https://github.com/ValveSoftware/steam-audio/releases/download/v4.6.1/steamaudio_4.6.1.zip"
  URL_HASH "SHA256=e9c56314f1066f631f193b0cb0ad886bd32daa3a9758b39c25a614ca453149a7"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR kcds-no-cmake-project)
FetchContent_MakeAvailable(kcds_steamaudio)
set(KCDS_STEAMAUDIO_DLL "${kcds_steamaudio_SOURCE_DIR}/lib/windows-x64/phonon.dll"
    CACHE INTERNAL "Steam Audio's runtime")
