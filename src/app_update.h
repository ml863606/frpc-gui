#pragma once

#include <string>

struct AppUpdateInfo {
    bool hasRelease = false;
    bool updateAvailable = false;
    std::wstring latestVersion;
    std::wstring releaseUrl;
};

bool CheckForAppUpdate(const std::wstring& currentVersion,
                       AppUpdateInfo& info,
                       std::wstring* error);

