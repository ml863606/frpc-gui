#include "version_manager.h"

#include "config.h"

#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    std::filesystem::path path(left);
    path /= right;
    return path.wstring();
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
            *error = L"无法写入版本文件: " + path;
        }
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return true;
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

std::wstring LastErrorText(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = len > 0 ? std::wstring(buffer, len) : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

bool HttpGetUtf8(const std::wstring& url, std::string& body, std::wstring* error) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(_countof(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(_countof(path));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        if (error) *error = L"解析 GitHub API 地址失败: " + LastErrorText();
        return false;
    }

    HINTERNET session = WinHttpOpen(L"frp-desk/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = L"初始化 WinHTTP 失败: " + LastErrorText();
        return false;
    }

    HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(),
                                       parts.nPort, 0);
    if (!connect) {
        if (error) *error = L"连接 GitHub API 失败: " + LastErrorText();
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", std::wstring(path, parts.dwUrlPathLength).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        if (error) *error = L"创建 GitHub API 请求失败: " + LastErrorText();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t headers[] =
        L"User-Agent: frp-desk\r\n"
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(wcslen(headers)),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    if (!received) {
        if (error) *error = L"请求 GitHub API 失败: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        if (error) *error = L"GitHub API 返回状态码: " + std::to_wstring(status);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    body.clear();
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, static_cast<DWORD>(sizeof(buffer)), &read)) {
            if (error) *error = L"读取 GitHub API 响应失败: " + LastErrorText();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (read == 0) {
            break;
        }
        body.append(buffer, buffer + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

std::vector<std::wstring> ExtractTopLevelObjects(const std::wstring& json) {
    std::vector<std::wstring> objects;
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t start = std::wstring::npos;
    for (size_t i = 0; i < json.size(); ++i) {
        wchar_t ch = json[i];
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
        if (ch == L'{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (ch == L'}') {
            --depth;
            if (depth == 0 && start != std::wstring::npos) {
                objects.push_back(json.substr(start, i - start + 1));
                start = std::wstring::npos;
            }
        }
    }
    return objects;
}

std::wstring FindWindowsAmd64Archive(const std::wstring& releaseObject) {
    size_t pos = 0;
    while ((pos = releaseObject.find(L"\"name\"", pos)) != std::wstring::npos) {
        std::wstring name;
        if (!FindJsonString(releaseObject.substr(pos), L"name", name)) {
            break;
        }
        if (name.find(L"windows_amd64.zip") != std::wstring::npos) {
            return name;
        }
        pos += 6;
    }
    return L"";
}

FrpVersionInfo DefaultFrpVersion() {
    return {
        L"0.69.1",
        L"frp 0.69.1",
        L"frp_0.69.1_windows_amd64.zip",
        L"https://github.com/fatedier/frp/releases/download/v0.69.1/frp_0.69.1_windows_amd64.zip",
        L"frp_0.69.1_windows_amd64",
        L""
    };
}

} // namespace

std::wstring GetVersionsDir() {
    return JoinPath(GetAppDataDir(), L"versions");
}

std::wstring GetVersionFilePath(const std::wstring& version) {
    (void)version;
    return GetVersionsSummaryPath();
}

std::wstring GetVersionsSummaryPath() {
    return JoinPath(GetVersionsDir(), L"frp-versions.json");
}

static void RemoveLegacyVersionFiles() {
    try {
        if (!std::filesystem::exists(GetVersionsDir())) {
            return;
        }
        const std::filesystem::path summary(GetVersionsSummaryPath());
        for (const auto& entry : std::filesystem::directory_iterator(GetVersionsDir())) {
            if (!entry.is_regular_file() || entry.path().extension() != L".json") {
                continue;
            }
            if (entry.path().filename() == summary.filename()) {
                continue;
            }
            std::filesystem::remove(entry.path());
        }
    } catch (...) {
    }
}

bool EnsureDefaultVersionFiles(std::wstring* error) {
    try {
        std::filesystem::create_directories(GetVersionsDir());
    } catch (const std::exception& ex) {
        if (error) {
            *error = L"无法创建版本目录: " + Utf8ToWide(ex.what());
        }
        return false;
    }

    RemoveLegacyVersionFiles();
    if (!FileExists(GetVersionsSummaryPath())) {
        std::vector<FrpVersionInfo> defaults{DefaultFrpVersion()};
        return SaveFrpVersions(defaults, error);
    }
    return true;
}

bool LoadFrpVersions(std::vector<FrpVersionInfo>& versions, std::wstring* error) {
    versions.clear();
    if (!EnsureDefaultVersionFiles(error)) {
        return false;
    }

    std::wstring json = Utf8ToWide(ReadAllBytes(GetVersionsSummaryPath()));
    for (const auto& object : ExtractTopLevelObjects(json)) {
        FrpVersionInfo info;
        FindJsonString(object, L"version", info.version);
        FindJsonString(object, L"displayName", info.displayName);
        FindJsonString(object, L"archiveName", info.archiveName);
        FindJsonString(object, L"downloadUrl", info.downloadUrl);
        FindJsonString(object, L"extractDir", info.extractDir);
        FindJsonString(object, L"publishedAt", info.publishedAt);
        if (!info.version.empty() && !info.downloadUrl.empty()) {
            if (info.displayName.empty()) info.displayName = L"frp " + info.version;
            versions.push_back(info);
        }
    }

    if (versions.empty()) {
        versions.push_back(DefaultFrpVersion());
    }
    std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
        return a.publishedAt > b.publishedAt;
    });
    return true;
}

bool SaveFrpVersions(const std::vector<FrpVersionInfo>& versions, std::wstring* error) {
    try {
        std::filesystem::create_directories(GetVersionsDir());
    } catch (const std::exception& ex) {
        if (error) {
            *error = L"无法创建版本目录: " + Utf8ToWide(ex.what());
        }
        return false;
    }

    RemoveLegacyVersionFiles();
    std::wostringstream json;
    json << L"[\n";
    for (size_t i = 0; i < versions.size(); ++i) {
        const auto& version = versions[i];
        json << L"  {\n"
             << L"    \"version\": \"" << EscapeJson(version.version) << L"\",\n"
             << L"    \"displayName\": \"" << EscapeJson(version.displayName) << L"\",\n"
             << L"    \"archiveName\": \"" << EscapeJson(version.archiveName) << L"\",\n"
             << L"    \"downloadUrl\": \"" << EscapeJson(version.downloadUrl) << L"\",\n"
             << L"    \"extractDir\": \"" << EscapeJson(version.extractDir) << L"\",\n"
             << L"    \"publishedAt\": \"" << EscapeJson(version.publishedAt) << L"\"\n"
             << L"  }" << (i + 1 == versions.size() ? L"\n" : L",\n");
    }
    json << L"]\n";
    return WriteAllBytes(GetVersionsSummaryPath(), WideToUtf8(json.str()), error);
}

bool RefreshFrpVersionsFromGitHub(std::vector<FrpVersionInfo>& versions,
                                  const std::function<void(const std::wstring&)>& log,
                                  std::wstring* error) {
    std::vector<FrpVersionInfo> refreshed;
    for (int page = 1; page <= 20; ++page) {
        std::wstring url = L"https://api.github.com/repos/fatedier/frp/releases?per_page=100&page=" +
                           std::to_wstring(page);
        if (log) log(L"读取 GitHub releases 第 " + std::to_wstring(page) + L" 页...");
        std::string bytes;
        if (!HttpGetUtf8(url, bytes, error)) {
            return false;
        }
        std::wstring json = Utf8ToWide(bytes);
        auto releaseObjects = ExtractTopLevelObjects(json);
        if (releaseObjects.empty()) {
            break;
        }
        for (const auto& object : releaseObjects) {
            std::wstring tag;
            FindJsonString(object, L"tag_name", tag);
            if (tag.empty()) {
                continue;
            }
            std::wstring publishedAt;
            FindJsonString(object, L"published_at", publishedAt);
            std::wstring archive = FindWindowsAmd64Archive(object);
            if (archive.empty()) {
                continue;
            }
            std::wstring version = tag;
            if (!version.empty() && version.front() == L'v') {
                version.erase(version.begin());
            }
            FrpVersionInfo info;
            info.version = version;
            info.displayName = L"frp " + version;
            info.archiveName = archive;
            info.extractDir = archive;
            if (info.extractDir.size() > 4 &&
                info.extractDir.substr(info.extractDir.size() - 4) == L".zip") {
                info.extractDir.resize(info.extractDir.size() - 4);
            }
            info.downloadUrl = L"https://github.com/fatedier/frp/releases/download/" +
                               tag + L"/" + archive;
            info.publishedAt = publishedAt;
            refreshed.push_back(info);
        }
    }

    if (refreshed.empty()) {
        if (error) *error = L"没有从 GitHub releases 中找到 windows_amd64.zip 版本";
        return false;
    }

    std::sort(refreshed.begin(), refreshed.end(), [](const auto& a, const auto& b) {
        return a.publishedAt > b.publishedAt;
    });
    if (!SaveFrpVersions(refreshed, error)) {
        return false;
    }
    versions = refreshed;
    if (log) log(L"GitHub 版本同步完成，共 " + std::to_wstring(versions.size()) + L" 个版本");
    return true;
}

const FrpVersionInfo* FindFrpVersion(const std::vector<FrpVersionInfo>& versions,
                                     const std::wstring& version) {
    for (const auto& item : versions) {
        if (item.version == version) {
            return &item;
        }
    }
    return versions.empty() ? nullptr : &versions.front();
}
