#pragma once

#include <string>
#include <vector>
#include <functional>

struct FrpVersionInfo {
    std::wstring version;
    std::wstring displayName;
    std::wstring archiveName;
    std::wstring downloadUrl;
    std::wstring extractDir;
};

std::wstring GetVersionsDir();
std::wstring GetVersionFilePath(const std::wstring& version);

bool EnsureDefaultVersionFiles(std::wstring* error);
bool LoadFrpVersions(std::vector<FrpVersionInfo>& versions, std::wstring* error);
bool SaveFrpVersion(const FrpVersionInfo& version, std::wstring* error);
bool RefreshFrpVersionsFromGitHub(std::vector<FrpVersionInfo>& versions,
                                  const std::function<void(const std::wstring&)>& log,
                                  std::wstring* error);
const FrpVersionInfo* FindFrpVersion(const std::vector<FrpVersionInfo>& versions,
                                     const std::wstring& version);
