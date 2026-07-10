#include "app_update.h"

#include "config.h"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <string>

namespace {

std::wstring LastErrorText(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 ? std::wstring(buffer, length) : L"未知错误";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

bool FindJsonString(const std::wstring& json, const std::wstring& key, std::wstring& value) {
    const std::wstring marker = L"\"" + key + L"\"";
    size_t pos = json.find(marker);
    if (pos == std::wstring::npos) return false;
    pos = json.find(L':', pos + marker.size());
    if (pos == std::wstring::npos) return false;
    pos = json.find(L'\"', pos + 1);
    if (pos == std::wstring::npos) return false;
    ++pos;

    std::wstring result;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        wchar_t ch = json[pos];
        if (escaped) {
            result += ch;
            escaped = false;
        } else if (ch == L'\\') {
            escaped = true;
        } else if (ch == L'\"') {
            value = result;
            return true;
        } else {
            result += ch;
        }
    }
    return false;
}

bool GetGitHubReleases(std::string& body, std::wstring* error) {
    constexpr wchar_t kUrl[] = L"https://api.github.com/repos/ml863606/frpc-gui/releases";
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(_countof(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(_countof(path));

    if (!WinHttpCrackUrl(kUrl, 0, 0, &parts)) {
        if (error) *error = L"解析更新地址失败: " + LastErrorText();
        return false;
    }

    HINTERNET session = WinHttpOpen(L"frpc-gui/0.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = L"初始化更新检查失败: " + LastErrorText();
        return false;
    }
    HINTERNET connect = WinHttpConnect(session,
                                       std::wstring(host, parts.dwHostNameLength).c_str(),
                                       parts.nPort, 0);
    if (!connect) {
        if (error) *error = L"连接 GitHub 失败: " + LastErrorText();
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", std::wstring(path, parts.dwUrlPathLength).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        if (error) *error = L"创建更新请求失败: " + LastErrorText();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    constexpr wchar_t kHeaders[] =
        L"User-Agent: frpc-gui/0.1.0\r\n"
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    BOOL sent = WinHttpSendRequest(request, kHeaders, static_cast<DWORD>(wcslen(kHeaders)),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    if (!received) {
        if (error) *error = L"请求 GitHub 失败: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
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
            if (error) *error = L"读取更新信息失败: " + LastErrorText();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (read == 0) break;
        body.append(buffer, buffer + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

std::wstring NormalizeVersion(std::wstring version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) {
        version.erase(version.begin());
    }
    return version;
}

} // namespace

bool CheckForAppUpdate(const std::wstring& currentVersion,
                       AppUpdateInfo& info,
                       std::wstring* error) {
    std::string body;
    if (!GetGitHubReleases(body, error)) return false;

    info = AppUpdateInfo{};
    const std::wstring json = Utf8ToWide(body);
    if (!FindJsonString(json, L"tag_name", info.latestVersion)) {
        return true;
    }

    info.hasRelease = true;
    FindJsonString(json, L"html_url", info.releaseUrl);
    info.updateAvailable = StrCmpLogicalW(NormalizeVersion(info.latestVersion).c_str(),
                                          NormalizeVersion(currentVersion).c_str()) > 0;
    return true;
}

