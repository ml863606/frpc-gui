#include "config.h"

#include <shlobj.h>
#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    std::filesystem::path path(left);
    path /= right;
    return path.wstring();
}

std::wstring ExecutableDir() {
    wchar_t path[MAX_PATH]{};
    DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return L".";
    }
    return std::filesystem::path(path).parent_path().wstring();
}

std::string ReadAllBytes(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool WriteAllBytes(const std::wstring& path, const std::string& bytes, std::wstring* error) {
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) {
            *error = L"无法写入文件: " + path;
        }
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return true;
}

std::wstring EscapeJson(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring EscapeToml(const std::wstring& value) {
    return EscapeJson(value);
}

std::wstring NormalizeProxyType(std::wstring type) {
    for (auto& ch : type) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (type == L"udp" || type == L"http" || type == L"https") {
        return type;
    }
    return L"tcp";
}

bool FindJsonString(const std::wstring& json, const std::wstring& key, std::wstring& value) {
    const std::wstring marker = L"\"" + key + L"\"";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) {
        return false;
    }
    pos = json.find(L':', pos + marker.size());
    if (pos == std::wstring::npos) {
        return false;
    }
    pos = json.find(L'"', pos + 1);
    if (pos == std::wstring::npos) {
        return false;
    }
    ++pos;

    std::wstring out;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        wchar_t ch = json[pos];
        if (escape) {
            switch (ch) {
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            default: out += ch; break;
            }
            escape = false;
            continue;
        }
        if (ch == L'\\') {
            escape = true;
            continue;
        }
        if (ch == L'"') {
            value = out;
            return true;
        }
        out += ch;
    }
    return false;
}

bool FindJsonInt(const std::wstring& json, const std::wstring& key, int& value) {
    const std::wstring marker = L"\"" + key + L"\"";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) {
        return false;
    }
    pos = json.find(L':', pos + marker.size());
    if (pos == std::wstring::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && iswspace(json[pos])) {
        ++pos;
    }
    size_t end = pos;
    if (end < json.size() && (json[end] == L'-' || json[end] == L'+')) {
        ++end;
    }
    while (end < json.size() && iswdigit(json[end])) {
        ++end;
    }
    if (end == pos) {
        return false;
    }
    value = _wtoi(json.substr(pos, end - pos).c_str());
    return true;
}

std::vector<std::wstring> ExtractJsonObjectsFromArray(const std::wstring& json,
                                                      const std::wstring& key) {
    std::vector<std::wstring> objects;
    const std::wstring marker = L"\"" + key + L"\"";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) {
        return objects;
    }
    pos = json.find(L'[', pos + marker.size());
    if (pos == std::wstring::npos) {
        return objects;
    }

    int arrayDepth = 0;
    int objectDepth = 0;
    bool inString = false;
    bool escape = false;
    size_t objectStart = std::wstring::npos;

    for (; pos < json.size(); ++pos) {
        wchar_t ch = json[pos];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == L'\\') {
                escape = true;
            } else if (ch == L'"') {
                inString = false;
            }
            continue;
        }
        if (ch == L'"') {
            inString = true;
            continue;
        }
        if (ch == L'[') {
            ++arrayDepth;
            continue;
        }
        if (ch == L']') {
            --arrayDepth;
            if (arrayDepth == 0) {
                break;
            }
            continue;
        }
        if (arrayDepth <= 0) {
            continue;
        }
        if (ch == L'{') {
            if (objectDepth == 0) {
                objectStart = pos;
            }
            ++objectDepth;
            continue;
        }
        if (ch == L'}') {
            --objectDepth;
            if (objectDepth == 0 && objectStart != std::wstring::npos) {
                objects.push_back(json.substr(objectStart, pos - objectStart + 1));
                objectStart = std::wstring::npos;
            }
        }
    }
    return objects;
}

std::vector<ProxyConfig> ParseProxies(const std::wstring& json) {
    std::vector<ProxyConfig> proxies;
    for (const auto& object : ExtractJsonObjectsFromArray(json, L"proxies")) {
        ProxyConfig proxy;
        FindJsonString(object, L"name", proxy.name);
        FindJsonString(object, L"type", proxy.type);
        FindJsonString(object, L"localIP", proxy.localIP);
        FindJsonInt(object, L"localPort", proxy.localPort);
        FindJsonInt(object, L"remotePort", proxy.remotePort);
        if (proxy.name.empty()) proxy.name = L"tcp" + std::to_wstring(proxies.size() + 1);
        proxy.type = NormalizeProxyType(proxy.type);
        if (proxy.localIP.empty()) proxy.localIP = L"127.0.0.1";
        if (proxy.localPort <= 0) proxy.localPort = 8080;
        if (proxy.remotePort <= 0) proxy.remotePort = 6000;
        proxies.push_back(proxy);
    }
    return proxies;
}

} // namespace

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        codePage = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(codePage, flags, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0);
    }
    if (len <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()),
                        result.data(), len);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), len, nullptr, nullptr);
    return result;
}

std::wstring GetAppDataDir() {
    return ExecutableDir();
}

std::wstring GetBinDir() {
    return JoinPath(GetAppDataDir(), L"bin");
}

std::wstring GetDownloadsDir() {
    return JoinPath(GetAppDataDir(), L"downloads");
}

std::wstring GetFrpcPath() {
    return GetFrpcPathForVersion(L"0.69.1");
}

std::wstring GetFrpcPathForVersion(const std::wstring& version) {
    return JoinPath(JoinPath(GetBinDir(), version), L"frpc.exe");
}

std::wstring GetConfigPath() {
    return JoinPath(GetAppDataDir(), L"config.json");
}

std::wstring GetTomlPath() {
    return JoinPath(GetAppDataDir(), L"frpc.toml");
}

bool EnsureAppDirectories(std::wstring* error) {
    try {
        std::filesystem::create_directories(GetBinDir());
        std::filesystem::create_directories(GetDownloadsDir());
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = L"无法创建运行目录: " + Utf8ToWide(ex.what());
        }
        return false;
    }
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool LoadConfig(AppConfig& config, std::wstring* error) {
    std::string bytes = ReadAllBytes(GetConfigPath());
    if (bytes.empty()) {
        return true;
    }

    std::wstring json = Utf8ToWide(bytes);
    FindJsonString(json, L"selectedVersion", config.selectedVersion);
    FindJsonString(json, L"frpsPublicIP", config.frpsPublicIP);
    FindJsonInt(json, L"frpsPort", config.frpsPort);
    FindJsonString(json, L"authMethod", config.authMethod);
    FindJsonString(json, L"authToken", config.authToken);
    FindJsonString(json, L"downloadMirror", config.downloadMirror);
    if (config.frpsPublicIP.empty()) {
        FindJsonString(json, L"serverAddr", config.frpsPublicIP);
    }
    if (config.frpsPort <= 0) {
        FindJsonInt(json, L"serverPort", config.frpsPort);
    }
    if (config.authToken.empty()) {
        FindJsonString(json, L"token", config.authToken);
    }
    config.proxies = ParseProxies(json);

    if (config.selectedVersion.empty()) config.selectedVersion = L"0.69.1";
    if (config.frpsPort <= 0) config.frpsPort = 7000;
    if (config.authMethod.empty()) config.authMethod = L"none";
    if (config.downloadMirror.empty()) config.downloadMirror = L"https://gh.zwy.one";
    if (config.proxies.empty()) {
        ProxyConfig legacy;
        FindJsonString(json, L"proxyName", legacy.name);
        FindJsonString(json, L"proxyType", legacy.type);
        FindJsonString(json, L"localIP", legacy.localIP);
        FindJsonInt(json, L"localPort", legacy.localPort);
        FindJsonInt(json, L"remotePort", legacy.remotePort);
        if (legacy.name.empty()) legacy.name = L"tcp1";
        legacy.type = NormalizeProxyType(legacy.type);
        if (legacy.localIP.empty()) legacy.localIP = L"127.0.0.1";
        if (legacy.localPort <= 0) legacy.localPort = 8080;
        if (legacy.remotePort <= 0) legacy.remotePort = 6000;
        config.proxies.push_back(legacy);
    }
    if (error) error->clear();
    return true;
}

bool SaveConfig(const AppConfig& config, std::wstring* error) {
    std::wostringstream json;
    json << L"{\n"
         << L"  \"selectedVersion\": \"" << EscapeJson(config.selectedVersion) << L"\",\n"
         << L"  \"frpsPublicIP\": \"" << EscapeJson(config.frpsPublicIP) << L"\",\n"
         << L"  \"frpsPort\": " << config.frpsPort << L",\n"
         << L"  \"authMethod\": \"" << EscapeJson(config.authMethod) << L"\",\n"
         << L"  \"authToken\": \"" << EscapeJson(config.authToken) << L"\",\n"
         << L"  \"downloadMirror\": \"" << EscapeJson(config.downloadMirror) << L"\",\n"
         << L"  \"proxies\": [\n";
    for (size_t i = 0; i < config.proxies.size(); ++i) {
        const auto& proxy = config.proxies[i];
        json << L"    {\n"
             << L"      \"name\": \"" << EscapeJson(proxy.name) << L"\",\n"
             << L"      \"type\": \"" << EscapeJson(NormalizeProxyType(proxy.type)) << L"\",\n"
             << L"      \"localIP\": \"" << EscapeJson(proxy.localIP) << L"\",\n"
             << L"      \"localPort\": " << proxy.localPort << L",\n"
             << L"      \"remotePort\": " << proxy.remotePort << L"\n"
             << L"    }" << (i + 1 < config.proxies.size() ? L"," : L"") << L"\n";
    }
    json << L"  ]\n"
         << L"}\n";
    return WriteAllBytes(GetConfigPath(), WideToUtf8(json.str()), error);
}

bool WriteFrpcToml(const AppConfig& config, std::wstring* error) {
    std::wostringstream toml;
    toml << L"serverAddr = \"" << EscapeToml(config.frpsPublicIP) << L"\"\n"
         << L"serverPort = " << config.frpsPort << L"\n\n";
    if (config.authMethod == L"token" && !config.authToken.empty()) {
        toml << L"[auth]\n"
             << L"method = \"token\"\n"
             << L"token = \"" << EscapeToml(config.authToken) << L"\"\n\n";
    }
    for (const auto& proxy : config.proxies) {
        std::wstring type = NormalizeProxyType(proxy.type);
        toml << L"[[proxies]]\n"
             << L"name = \"" << EscapeToml(proxy.name) << L"\"\n"
             << L"type = \"" << EscapeToml(type) << L"\"\n"
             << L"localIP = \"" << EscapeToml(proxy.localIP) << L"\"\n"
             << L"localPort = " << proxy.localPort << L"\n";
        if (type == L"tcp" || type == L"udp") {
            toml << L"remotePort = " << proxy.remotePort << L"\n";
        }
        toml << L"\n";
    }

    return WriteAllBytes(GetTomlPath(), WideToUtf8(toml.str()), error);
}
