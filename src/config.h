#pragma once

#include <string>
#include <vector>

struct ProxyConfig {
    std::wstring name = L"tcp1";
    std::wstring type = L"tcp";
    std::wstring localIP = L"127.0.0.1";
    int localPort = 8080;
    int remotePort = 6000;
};

struct AppConfig {
    std::wstring selectedVersion = L"0.69.1";
    std::wstring frpsPublicIP = L"";
    int frpsPort = 7000;
    std::wstring authMethod = L"none";
    std::wstring authToken = L"";
    std::wstring downloadMirror = L"https://gh.zwy.one";
    std::vector<ProxyConfig> proxies = { ProxyConfig{} };
};

std::wstring GetAppDataDir();
std::wstring GetBinDir();
std::wstring GetDownloadsDir();
std::wstring GetFrpcPath();
std::wstring GetFrpcPathForVersion(const std::wstring& version);
std::wstring GetConfigPath();
std::wstring GetTomlPath();

bool EnsureAppDirectories(std::wstring* error);
bool FileExists(const std::wstring& path);

bool LoadConfig(AppConfig& config, std::wstring* error);
bool SaveConfig(const AppConfig& config, std::wstring* error);
bool WriteFrpcToml(const AppConfig& config, std::wstring* error);

std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
