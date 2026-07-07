#include "app.h"

#include "config.h"
#include "frpc_manager.h"
#include "version_manager.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <vector>

namespace {

AppConfig g_config;
std::vector<FrpVersionInfo> g_versions;
FrpcManager g_frpc;
std::vector<std::string> g_appLogs;
std::vector<std::string> g_frpcLogs;
std::wstring g_downloadingVersion;
int g_downloadProgress = 0;
bool g_connectionTesting = false;
std::wstring g_connectionTestStatus = L"尚未测试";
int g_connectionTestState = 0;
bool g_serviceFailed = false;
std::mutex g_stateMutex;

slint::SharedString S(const std::wstring& value) {
    return slint::SharedString(WideToUtf8(value));
}

std::wstring W(const slint::SharedString& value) {
    return Utf8ToWide(std::string(value.data(), value.size()));
}

std::wstring CurrentFrpcPath() {
    return GetFrpcPathForVersion(g_config.selectedVersion);
}

const FrpVersionInfo* CurrentVersion() {
    return FindFrpVersion(g_versions, g_config.selectedVersion);
}

int VersionIndexFor(const std::wstring& version) {
    for (size_t i = 0; i < g_versions.size(); ++i) {
        if (g_versions[i].version == version) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

int DownloadSourceIndex(const std::wstring& mirror) {
    return mirror == L"direct" ? 1 : 0;
}

std::wstring DownloadMirrorFromIndex(int index) {
    return index == 1 ? L"direct" : L"https://gh.zwy.one";
}

int ParsePort(const std::wstring& text, int fallback) {
    wchar_t* end = nullptr;
    long value = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || value < 1 || value > 65535) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool TryParsePort(const std::wstring& text, int& port) {
    wchar_t* end = nullptr;
    long value = std::wcstol(text.c_str(), &end, 10);
    while (end && *end != L'\0' && std::iswspace(static_cast<unsigned short>(*end))) {
        ++end;
    }
    if (end == text.c_str() || !end || *end != L'\0' || value < 1 || value > 65535) {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

std::wstring ProxyTypeFromIndex(int index) {
    switch (index) {
    case 1: return L"udp";
    case 2: return L"http";
    case 3: return L"https";
    default: return L"tcp";
    }
}

int ProxyTypeIndex(const std::wstring& type) {
    std::wstring lower = type;
    for (auto& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    if (lower == L"udp") return 1;
    if (lower == L"http") return 2;
    if (lower == L"https") return 3;
    return 0;
}

std::wstring ProxyTypeDisplay(const std::wstring& type) {
    switch (ProxyTypeIndex(type)) {
    case 1: return L"UDP";
    case 2: return L"HTTP";
    case 3: return L"HTTPS";
    default: return L"TCP";
    }
}

std::wstring WinsockErrorText(int code) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, static_cast<DWORD>(code), 0,
                               reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = len > 0 ? std::wstring(buffer, len) : L"网络错误 " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

bool TestTcpConnection(const std::wstring& host, int port, int timeoutMs, std::wstring* error) {
    WSADATA data{};
    int startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup != 0) {
        if (error) *error = L"初始化网络失败: " + WinsockErrorText(startup);
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    std::string hostUtf8 = WideToUtf8(host);
    std::string portText = std::to_string(port);
    int resolved = getaddrinfo(hostUtf8.c_str(), portText.c_str(), &hints, &results);
    if (resolved != 0) {
        if (error) *error = L"解析地址失败: " + WinsockErrorText(resolved);
        WSACleanup();
        return false;
    }

    std::wstring lastError = L"连接失败";
    for (addrinfo* item = results; item; item = item->ai_next) {
        SOCKET sock = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (sock == INVALID_SOCKET) {
            lastError = WinsockErrorText(WSAGetLastError());
            continue;
        }

        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);
        int connected = connect(sock, item->ai_addr, static_cast<int>(item->ai_addrlen));
        if (connected == 0) {
            closesocket(sock);
            freeaddrinfo(results);
            WSACleanup();
            return true;
        }

        int connectError = WSAGetLastError();
        if (connectError == WSAEWOULDBLOCK || connectError == WSAEINPROGRESS ||
            connectError == WSAEINVAL) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval timeout{};
            timeout.tv_sec = timeoutMs / 1000;
            timeout.tv_usec = (timeoutMs % 1000) * 1000;
            int selected = select(0, nullptr, &writeSet, nullptr, &timeout);
            if (selected > 0 && FD_ISSET(sock, &writeSet)) {
                int socketError = 0;
                int socketErrorLen = sizeof(socketError);
                getsockopt(sock, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socketError), &socketErrorLen);
                if (socketError == 0) {
                    closesocket(sock);
                    freeaddrinfo(results);
                    WSACleanup();
                    return true;
                }
                lastError = WinsockErrorText(socketError);
            } else if (selected == 0) {
                lastError = L"连接超时";
            } else {
                lastError = WinsockErrorText(WSAGetLastError());
            }
        } else {
            lastError = WinsockErrorText(connectError);
        }

        closesocket(sock);
    }

    freeaddrinfo(results);
    WSACleanup();
    if (error) *error = lastError;
    return false;
}

std::wstring BuildDownloadUrl(const FrpVersionInfo& version, std::wstring mirrorBase) {
    while (!mirrorBase.empty() && mirrorBase.back() == L'/') {
        mirrorBase.pop_back();
    }
    if (mirrorBase.empty() || mirrorBase == L"direct") {
        return version.downloadUrl;
    }
    std::wstring tag = version.version;
    if (tag.empty() || tag.front() != L'v') {
        tag = L"v" + tag;
    }
    return mirrorBase + L"/github.com/fatedier/frp/releases/download/" +
           tag + L"/" + version.archiveName;
}

bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

ProxyRow BuildProxyRow(const ProxyConfig& proxy, size_t index) {
    ProxyRow row;
    row.index = static_cast<int>(index + 1);
    row.proxy_type = S(ProxyTypeDisplay(proxy.type));
    row.type_index = ProxyTypeIndex(proxy.type);
    row.name = S(proxy.name);
    row.local_ip = S(proxy.localIP);
    row.local_port = proxy.localPort;
    row.remote_port = proxy.remotePort;
    row.enabled = proxy.enabled;
    return row;
}

bool ValidateProxy(const ProxyConfig& proxy) {
    return ProxyTypeIndex(proxy.type) >= 0 &&
           !proxy.name.empty() && !proxy.localIP.empty() &&
           proxy.localPort > 0 && proxy.localPort <= 65535 &&
           proxy.remotePort > 0 && proxy.remotePort <= 65535;
}

std::shared_ptr<slint::Model<ProxyPair>> BuildProxyPairs() {
    std::vector<ProxyPair> pairs;
    pairs.reserve((g_config.proxies.size() + 1) / 2);
    for (size_t i = 0; i < g_config.proxies.size(); i += 2) {
        ProxyPair pair;
        pair.left = BuildProxyRow(g_config.proxies[i], i);
        if (i + 1 < g_config.proxies.size()) {
            pair.right = BuildProxyRow(g_config.proxies[i + 1], i + 1);
            pair.has_right = true;
        } else {
            pair.right = ProxyRow{};
            pair.has_right = false;
        }
        pairs.push_back(std::move(pair));
    }
    return std::make_shared<slint::VectorModel<ProxyPair>>(std::move(pairs));
}

VersionRow BuildVersionRow(const FrpVersionInfo& item) {
    VersionRow row;
    const bool installed = FileExists(GetFrpcPathForVersion(item.version));
    row.display_name = S(item.displayName);
    row.version = S(item.version);
    row.archive_name = S(item.archiveName);
    row.config_path = S(GetVersionFilePath(item.version));
    row.frpc_path = S(GetFrpcPathForVersion(item.version));
    row.published_at = S(item.publishedAt);
    row.installed = installed;
    return row;
}

std::shared_ptr<slint::Model<VersionPair>> BuildVersionPairs() {
    std::vector<VersionPair> pairs;
    pairs.reserve((g_versions.size() + 1) / 2);
    for (size_t i = 0; i < g_versions.size(); i += 2) {
        VersionPair pair;
        pair.left = BuildVersionRow(g_versions[i]);
        if (i + 1 < g_versions.size()) {
            pair.right = BuildVersionRow(g_versions[i + 1]);
            pair.has_right = true;
        } else {
            pair.right = VersionRow{};
            pair.has_right = false;
        }
        pairs.push_back(std::move(pair));
    }
    return std::make_shared<slint::VectorModel<VersionPair>>(std::move(pairs));
}

std::shared_ptr<slint::Model<slint::SharedString>> BuildVersionOptions() {
    std::vector<slint::SharedString> options;
    options.reserve(g_versions.size() > 0 ? g_versions.size() : 1);
    for (const auto& item : g_versions) {
        options.push_back(S(item.version));
    }
    if (options.empty()) {
        options.push_back(S(g_config.selectedVersion.empty() ? L"0.69.1" : g_config.selectedVersion));
    }
    return std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(options));
}

std::string BuildLogText(const std::vector<std::string>& logs) {
    std::string text;
    for (const auto& line : logs) {
        text += line;
        text += "\n";
    }
    return text;
}

std::string JoinPids(const std::vector<DWORD>& pids) {
    std::ostringstream stream;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i > 0) {
            stream << ",";
        }
        stream << pids[i];
    }
    return stream.str();
}

std::string CurrentLogTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &nowTime);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::wstring LogFilePath(bool frpcLog) {
    std::filesystem::path path(GetLogsDir());
    path /= frpcLog ? L"frpc.log" : L"app.log";
    return path.wstring();
}

std::vector<std::string> LoadRecentLogLines(bool frpcLog, size_t maxLines = 500) {
    std::ifstream file(std::filesystem::path(LogFilePath(frpcLog)), std::ios::binary);
    if (!file) {
        return {};
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (lines.size() > maxLines) {
            lines.erase(lines.begin(), lines.begin() + static_cast<ptrdiff_t>(lines.size() - maxLines));
        }
    }
    return lines;
}

void AppendLogFile(bool frpcLog, const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(GetLogsDir()), ec);

    std::ofstream file(std::filesystem::path(LogFilePath(frpcLog)), std::ios::binary | std::ios::app);
    if (!file) {
        return;
    }
    for (const auto& line : lines) {
        file << line << "\r\n";
    }
}

void ClearLogFile(bool frpcLog) {
    std::ofstream file(std::filesystem::path(LogFilePath(frpcLog)), std::ios::binary | std::ios::trunc);
}

using AppHandle = slint::ComponentHandle<AppWindow>;

void RefreshUi(const AppHandle& ui) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    ui->set_selected_version(S(g_config.selectedVersion));
    ui->set_frpc_version_options(BuildVersionOptions());
    ui->set_selected_version_index(VersionIndexFor(g_config.selectedVersion));
    ui->set_frps_public_ip(S(g_config.frpsPublicIP));
    ui->set_frps_port(g_config.frpsPort);
    ui->set_frps_port_text(slint::SharedString(std::to_string(g_config.frpsPort)));
    ui->set_connection_testing(g_connectionTesting);
    ui->set_connection_test_status(S(g_connectionTestStatus));
    ui->set_connection_test_state(g_connectionTestState);
    ui->set_auth_method_index(g_config.authMethod == L"token" ? 1 : 0);
    ui->set_auth_token(S(g_config.authToken));
    ui->set_download_mirror(S(g_config.downloadMirror.empty() ? L"https://gh.zwy.one" : g_config.downloadMirror));
    ui->set_download_source_index(DownloadSourceIndex(g_config.downloadMirror));
    ui->set_proxy_pairs(BuildProxyPairs());
    ui->set_proxy_count(static_cast<int>(g_config.proxies.size()));
    ui->set_version_pairs(BuildVersionPairs());
    ui->set_downloading_version(S(g_downloadingVersion));
    ui->set_download_progress(g_downloadProgress);
    ui->set_app_log_text(slint::SharedString(BuildLogText(g_appLogs)));
    ui->set_frpc_log_text(slint::SharedString(BuildLogText(g_frpcLogs)));
    std::vector<DWORD> frpcPids;
    DWORD managedFrpcPid = g_frpc.RunningFrpcPid();
    if (managedFrpcPid != 0) {
        frpcPids.push_back(managedFrpcPid);
    } else {
        frpcPids = g_frpc.FindExactFrpcPids(CurrentFrpcPath());
    }
    const bool running = !frpcPids.empty();
    int serviceState = running ? 1 : (g_serviceFailed ? 2 : 0);
    std::string statusText = running
        ? "运行中 PID: " + JoinPids(frpcPids)
        : (g_serviceFailed ? "启动失败" : "未启动");
    ui->set_service_state(serviceState);
    ui->set_status_text(slint::SharedString(statusText));
}

std::vector<std::string> AppendLogLines(std::vector<std::string>& logs, const std::string& utf8) {
    std::vector<std::string> appended;
    const std::string timestamp = CurrentLogTimestamp();
    size_t start = 0;
    while (start < utf8.size()) {
        size_t end = utf8.find('\n', start);
        std::string line = utf8.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            appended.push_back(timestamp + " " + line);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (utf8.empty()) appended.push_back(timestamp + " ");
    logs.insert(logs.end(), appended.begin(), appended.end());
    if (logs.size() > 500) {
        logs.erase(logs.begin(), logs.begin() + static_cast<ptrdiff_t>(logs.size() - 500));
    }
    return appended;
}

void AddLogTo(std::vector<std::string>& logs,
              const std::wstring& text,
              const slint::ComponentWeakHandle<AppWindow>& weak,
              bool frpcLog) {
    std::string utf8 = WideToUtf8(text);
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        std::vector<std::string> appended = AppendLogLines(logs, utf8);
        AppendLogFile(frpcLog, appended);
        snapshot = BuildLogText(logs);
    }

    slint::invoke_from_event_loop([weak, snapshot, frpcLog] {
        if (auto ui = weak.lock()) {
            if (frpcLog) {
                (*ui)->set_frpc_log_text(slint::SharedString(snapshot));
            } else {
                (*ui)->set_app_log_text(slint::SharedString(snapshot));
            }
        }
    });
}

void AddAppLog(const std::wstring& text, const slint::ComponentWeakHandle<AppWindow>& weak) {
    AddLogTo(g_appLogs, text, weak, false);
}

void AddFrpcLog(const std::wstring& text, const slint::ComponentWeakHandle<AppWindow>& weak) {
    AddLogTo(g_frpcLogs, text, weak, true);
}

void ReadUiToConfig(const AppHandle& ui) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    int versionIndex = ui->get_selected_version_index();
    if (versionIndex >= 0 && versionIndex < static_cast<int>(g_versions.size())) {
        g_config.selectedVersion = g_versions[static_cast<size_t>(versionIndex)].version;
    } else {
        g_config.selectedVersion = W(ui->get_selected_version());
    }
    g_config.frpsPublicIP = W(ui->get_frps_public_ip());
    g_config.frpsPort = ParsePort(W(ui->get_frps_port_text()), 7000);
    g_config.authMethod = ui->get_auth_method_index() == 1 ? L"token" : L"none";
    g_config.authToken = W(ui->get_auth_token());
    g_config.downloadMirror = DownloadMirrorFromIndex(ui->get_download_source_index());
    if (g_config.downloadMirror.empty()) {
        g_config.downloadMirror = L"https://gh.zwy.one";
    }
}

bool SaveCurrentConfig(const AppHandle& ui, bool requireComplete) {
    ReadUiToConfig(ui);
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (requireComplete && g_config.frpsPublicIP.empty()) {
            MessageBoxW(nullptr, L"请在 frps 设置中填写公网 IP 或域名。", L"配置不完整", MB_ICONWARNING);
            return false;
        }
        std::wstring error;
        if (!SaveConfig(g_config, &error)) {
            MessageBoxW(nullptr, error.c_str(), L"保存失败", MB_ICONERROR);
            return false;
        }
        if (!WriteFrpcToml(g_config, &error)) {
            MessageBoxW(nullptr, error.c_str(), L"生成 frpc.toml 失败", MB_ICONERROR);
            return false;
        }
    }
    RefreshUi(ui);
    return true;
}

bool LoadVersions() {
    std::wstring error;
    if (!LoadFrpVersions(g_versions, &error)) {
        MessageBoxW(nullptr, error.c_str(), L"读取版本失败", MB_ICONERROR);
        return false;
    }
    const FrpVersionInfo* current = FindFrpVersion(g_versions, g_config.selectedVersion);
    if (current) g_config.selectedVersion = current->version;
    return true;
}

void StartFrpc(const AppHandle& ui, const slint::ComponentWeakHandle<AppWindow>& weak) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_serviceFailed = false;
    }
    if (!SaveCurrentConfig(ui, true)) return;
    if (!FileExists(CurrentFrpcPath())) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_serviceFailed = true;
        }
        MessageBoxW(nullptr, L"当前版本未安装 frpc.exe，请到 frp版本管理 页面手动下载。", L"无法启动", MB_ICONWARNING);
        RefreshUi(ui);
        return;
    }

    AddAppLog(L"启动 frpc: " + CurrentFrpcPath(), weak);

    std::wstring error;
    bool ok = g_frpc.Start(CurrentFrpcPath(), GetTomlPath(),
                           std::filesystem::path(CurrentFrpcPath()).parent_path().wstring(),
                           [weak](const std::wstring& line) { AddFrpcLog(line, weak); },
                           [weak](DWORD) {
                               slint::invoke_from_event_loop([weak] {
                                   if (auto ui = weak.lock()) {
                                       {
                                           std::lock_guard<std::mutex> lock(g_stateMutex);
                                           g_serviceFailed = false;
                                       }
                                       RefreshUi(*ui);
                                   }
                               });
                           },
                           &error);
    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_serviceFailed = true;
        }
        MessageBoxW(nullptr, error.c_str(), L"启动失败", MB_ICONERROR);
        AddAppLog(L"启动失败: " + error, weak);
    } else {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_serviceFailed = false;
        }
        AddAppLog(L"frpc 启动命令已发送", weak);
    }
    RefreshUi(ui);
}

void StartDownload(const AppHandle& ui, const slint::ComponentWeakHandle<AppWindow>& weak,
                   const std::wstring& requestedVersion = L"") {
    ReadUiToConfig(ui);
    FrpVersionInfo selected;
    std::wstring mirror;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_downloadingVersion.empty()) {
            MessageBoxW(nullptr, L"已有版本正在下载，请等待当前下载完成。", L"正在下载", MB_ICONINFORMATION);
            return;
        }
        if (!requestedVersion.empty()) {
            g_config.selectedVersion = requestedVersion;
            SaveConfig(g_config, nullptr);
        }
        const FrpVersionInfo* version = requestedVersion.empty()
            ? CurrentVersion()
            : FindFrpVersion(g_versions, requestedVersion);
        if (!version) {
            MessageBoxW(nullptr, L"没有可用的 frp 版本配置。", L"无法下载", MB_ICONWARNING);
            return;
        }
        selected = *version;
        mirror = g_config.downloadMirror;
        g_downloadingVersion = selected.version;
        g_downloadProgress = 1;
        SaveConfig(g_config, nullptr);
    }

    ui->set_status_text("正在下载 frpc");
    ui->set_selected_version(S(selected.version));
    ui->set_selected_version_index(VersionIndexFor(selected.version));
    ui->set_downloading_version(S(selected.version));
    ui->set_download_progress(1);
    std::thread([selected, mirror, weak]() {
        std::wstring error;
        bool ok = g_frpc.DownloadFrpc(selected, mirror, [weak](const std::wstring& line) {
            AddAppLog(line, weak);
        }, [weak, version = selected.version](int progress) {
            if (progress < 0) progress = 0;
            if (progress > 100) progress = 100;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (g_downloadingVersion == version) {
                    g_downloadProgress = progress;
                }
            }
            slint::invoke_from_event_loop([weak, version, progress] {
                if (auto ui = weak.lock()) {
                    (*ui)->set_downloading_version(S(version));
                    (*ui)->set_download_progress(progress);
                }
            });
        }, &error);
        slint::invoke_from_event_loop([weak, ok, error] {
            if (auto ui = weak.lock()) {
                if (!ok) {
                    MessageBoxW(nullptr, (L"下载 frpc 失败。\n\n" + error).c_str(), L"下载失败", MB_ICONWARNING);
                } else {
                    MessageBoxW(nullptr, L"frpc.exe 下载完成。", L"frpc-gui", MB_ICONINFORMATION);
                }
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_downloadingVersion.clear();
                    g_downloadProgress = 0;
                }
                RefreshUi(*ui);
            }
        });
    }).detach();
}

void StartRefreshVersions(const AppHandle& ui, const slint::ComponentWeakHandle<AppWindow>& weak) {
    ui->set_status_text("正在同步版本");
    std::thread([weak]() {
        std::vector<FrpVersionInfo> versions;
        std::wstring error;
        bool ok = RefreshFrpVersionsFromGitHub(versions, [weak](const std::wstring& line) {
            AddAppLog(line, weak);
        }, &error);

        slint::invoke_from_event_loop([weak, ok, error, versions = std::move(versions)]() mutable {
            if (auto ui = weak.lock()) {
                if (ok) {
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        g_versions = std::move(versions);
                        const FrpVersionInfo* current = FindFrpVersion(g_versions, g_config.selectedVersion);
                        if (current) g_config.selectedVersion = current->version;
                    }
                    MessageBoxW(nullptr, L"GitHub frp 版本同步完成。", L"frpc-gui", MB_ICONINFORMATION);
                } else {
                    MessageBoxW(nullptr, (L"同步 GitHub 版本失败。\n\n" + error).c_str(), L"同步失败", MB_ICONWARNING);
                }
                RefreshUi(*ui);
            }
        });
    }).detach();
}

void StartConnectionTest(const AppHandle& ui, const slint::ComponentWeakHandle<AppWindow>& weak) {
    std::wstring host = W(ui->get_frps_public_ip());
    int port = 0;
    if (host.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_connectionTestStatus = L"请先填写公网 IP 或域名。";
            g_connectionTestState = 3;
        }
        ui->set_connection_test_status(S(L"请先填写公网 IP 或域名。"));
        ui->set_connection_test_state(3);
        return;
    }
    if (!TryParsePort(W(ui->get_frps_port_text()), port)) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_connectionTestStatus = L"端口无效，请输入 1-65535。";
            g_connectionTestState = 3;
        }
        ui->set_connection_test_status(S(L"端口无效，请输入 1-65535。"));
        ui->set_connection_test_state(3);
        return;
    }

    std::wstring testingStatus;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_connectionTesting) {
            return;
        }
        g_connectionTesting = true;
        g_connectionTestStatus = L"正在测试 " + host + L":" + std::to_wstring(port) + L" ...";
        g_connectionTestState = 1;
        testingStatus = g_connectionTestStatus;
    }
    ui->set_connection_testing(true);
    ui->set_connection_test_status(S(testingStatus));
    ui->set_connection_test_state(1);

    std::thread([weak, host, port]() {
        std::wstring error;
        bool ok = TestTcpConnection(host, port, 3000, &error);
        slint::invoke_from_event_loop([weak, host, port, ok, error] {
            if (auto ui = weak.lock()) {
                std::wstring status;
                if (ok) {
                    status = L"端口可连接: " + host + L":" + std::to_wstring(port);
                    AddAppLog(L"连通性测试成功: " + host + L":" + std::to_wstring(port), weak);
                } else {
                    status = L"连接失败: " + error;
                    AddAppLog(L"连通性测试失败: " + host + L":" + std::to_wstring(port) + L" - " + error,
                           weak);
                }
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_connectionTesting = false;
                    g_connectionTestStatus = status;
                    g_connectionTestState = ok ? 2 : 3;
                }
                (*ui)->set_connection_testing(false);
                (*ui)->set_connection_test_status(S(status));
                (*ui)->set_connection_test_state(ok ? 2 : 3);
            }
        });
    }).detach();
}

void ToggleProxyEnabled(const AppHandle& ui,
                        const slint::ComponentWeakHandle<AppWindow>& weak,
                        int index) {
    std::wstring logLine;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        size_t pos = index > 0 ? static_cast<size_t>(index - 1) : g_config.proxies.size();
        if (pos >= g_config.proxies.size()) {
            MessageBoxW(nullptr, L"代理不存在，可能已经被删除。", L"切换失败", MB_ICONWARNING);
            return;
        }

        ProxyConfig& proxy = g_config.proxies[pos];
        proxy.enabled = !proxy.enabled;
        logLine = std::wstring(proxy.enabled ? L"启用代理: " : L"停用代理: ") + proxy.name;
        SaveConfig(g_config, nullptr);
        WriteFrpcToml(g_config, nullptr);
    }
    AddAppLog(logLine, weak);
    RefreshUi(ui);
}

void RunProxyPortTest(const slint::ComponentWeakHandle<AppWindow>& weak,
                      const ProxyConfig& proxy,
                      const std::wstring& remoteHost) {
    AddAppLog(L"开始测试代理端口: " + proxy.name, weak);
    std::thread([weak, proxy, remoteHost]() {
        std::wstring localError;
        std::wstring remoteError;
        bool localOk = TestTcpConnection(proxy.localIP, proxy.localPort, 3000, &localError);
        bool remoteOk = TestTcpConnection(remoteHost, proxy.remotePort, 3000, &remoteError);

        slint::invoke_from_event_loop([weak, proxy, remoteHost, localOk, remoteOk,
                                       localError, remoteError] {
            std::wstring localEndpoint = proxy.localIP + L":" + std::to_wstring(proxy.localPort);
            std::wstring remoteEndpoint = remoteHost + L":" + std::to_wstring(proxy.remotePort);
            std::wstring message = (localOk && remoteOk) ? L"端口测试通过。\n\n" : L"端口测试未通过。\n\n";
            message += localOk ? L"本地端口可连接: " + localEndpoint
                               : L"本地端口连接失败: " + localEndpoint + L" - " + localError;
            message += L"\n";
            message += remoteOk ? L"远端端口可连接: " + remoteEndpoint
                                : L"远端端口连接失败: " + remoteEndpoint + L" - " + remoteError;

            AddAppLog((localOk ? L"本地端口测试成功: " : L"本地端口测试失败: ") +
                          localEndpoint + (localOk ? L"" : L" - " + localError),
                      weak);
            AddAppLog((remoteOk ? L"远端端口测试成功: " : L"远端端口测试失败: ") +
                          remoteEndpoint + (remoteOk ? L"" : L" - " + remoteError),
                      weak);
            MessageBoxW(nullptr, message.c_str(), L"代理端口测试",
                        (localOk && remoteOk) ? MB_ICONINFORMATION : MB_ICONWARNING);
        });
    }).detach();
}

void SetProxyInputTestState(const AppHandle& ui, int mode, int localState, int remoteState) {
    if (mode == 0) {
        ui->set_add_proxy_local_test_state(localState);
        ui->set_add_proxy_remote_test_state(remoteState);
    } else {
        ui->set_edit_proxy_local_test_state(localState);
        ui->set_edit_proxy_remote_test_state(remoteState);
    }
}

void SetProxyInputTestState(const slint::ComponentWeakHandle<AppWindow>& weak,
                            int mode,
                            int localState,
                            int remoteState) {
    if (auto ui = weak.lock()) {
        SetProxyInputTestState(*ui, mode, localState, remoteState);
    }
}

void StartProxyPortTest(const AppHandle& ui,
                        const slint::ComponentWeakHandle<AppWindow>& weak,
                        int index) {
    ReadUiToConfig(ui);

    ProxyConfig proxy;
    std::wstring remoteHost;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        size_t pos = index > 0 ? static_cast<size_t>(index - 1) : g_config.proxies.size();
        if (pos >= g_config.proxies.size()) {
            MessageBoxW(nullptr, L"代理不存在，可能已经被删除。", L"测试失败", MB_ICONWARNING);
            return;
        }
        proxy = g_config.proxies[pos];
        remoteHost = g_config.frpsPublicIP;
    }

    if (remoteHost.empty()) {
        MessageBoxW(nullptr, L"请先在 frps 设置中填写公网 IP 或域名。", L"测试失败", MB_ICONWARNING);
        return;
    }

    RunProxyPortTest(weak, proxy, remoteHost);
}

void StartProxyInputPortTest(const AppHandle& ui,
                             const slint::ComponentWeakHandle<AppWindow>& weak,
                             int mode,
                             slint::SharedString localIp,
                             slint::SharedString localPortText,
                             slint::SharedString remotePortText) {
    ReadUiToConfig(ui);

    ProxyConfig proxy;
    proxy.name = L"当前输入";
    proxy.localIP = W(localIp);
    const bool localPortValid = TryParsePort(W(localPortText), proxy.localPort);
    const bool remotePortValid = TryParsePort(W(remotePortText), proxy.remotePort);

    std::wstring remoteHost;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        remoteHost = g_config.frpsPublicIP;
    }

    const bool canTestLocal = !proxy.localIP.empty() && localPortValid;
    const bool canTestRemote = !remoteHost.empty() && remotePortValid;
    if (!canTestLocal || !canTestRemote) {
        SetProxyInputTestState(ui, mode, canTestLocal ? 1 : 3, canTestRemote ? 1 : 3);
        if (!canTestLocal && !canTestRemote) {
            AddAppLog(L"代理端口测试失败: 输入不完整或端口无效", weak);
            return;
        }
    }

    AddAppLog(L"开始测试代理端口: " + proxy.name, weak);
    std::thread([weak, mode, proxy, remoteHost, canTestLocal, canTestRemote]() {
        std::wstring localError;
        std::wstring remoteError;
        bool localOk = false;
        bool remoteOk = false;
        if (canTestLocal) {
            localOk = TestTcpConnection(proxy.localIP, proxy.localPort, 3000, &localError);
        }
        if (canTestRemote) {
            remoteOk = TestTcpConnection(remoteHost, proxy.remotePort, 3000, &remoteError);
        }

        slint::invoke_from_event_loop([weak, mode, proxy, remoteHost, canTestLocal, canTestRemote,
                                       localOk, remoteOk, localError, remoteError] {
            std::wstring localEndpoint = proxy.localIP + L":" + std::to_wstring(proxy.localPort);
            std::wstring remoteEndpoint = remoteHost + L":" + std::to_wstring(proxy.remotePort);
            int localState = canTestLocal ? (localOk ? 2 : 3) : 3;
            int remoteState = canTestRemote ? (remoteOk ? 2 : 3) : 3;

            if (canTestLocal) {
                AddAppLog((localOk ? L"本地端口测试成功: " : L"本地端口测试失败: ") +
                              localEndpoint + (localOk ? L"" : L" - " + localError),
                          weak);
            }
            if (canTestRemote) {
                AddAppLog((remoteOk ? L"远端端口测试成功: " : L"远端端口测试失败: ") +
                              remoteEndpoint + (remoteOk ? L"" : L" - " + remoteError),
                          weak);
            }
            SetProxyInputTestState(weak, mode, localState, remoteState);
        });
    }).detach();
}

void CopyDownloadUrl(const AppHandle& ui, const slint::SharedString& versionText) {
    ReadUiToConfig(ui);
    std::wstring version = W(versionText);
    std::wstring url;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const FrpVersionInfo* item = FindFrpVersion(g_versions, version);
        if (!item) {
            MessageBoxW(nullptr, L"没有找到该版本的下载信息。", L"复制失败", MB_ICONWARNING);
            return;
        }
        url = BuildDownloadUrl(*item, g_config.downloadMirror);
    }
    if (!CopyTextToClipboard(url)) {
        MessageBoxW(nullptr, L"写入剪贴板失败。", L"复制失败", MB_ICONWARNING);
        return;
    }
    MessageBoxW(nullptr, L"下载链接已复制到剪贴板。", L"frpc-gui", MB_ICONINFORMATION);
}

void OpenVersionFolder(const slint::SharedString& versionText) {
    std::wstring version = W(versionText);
    std::wstring frpcPath = GetFrpcPathForVersion(version);
    std::filesystem::path folder = std::filesystem::path(frpcPath).parent_path();
    if (!std::filesystem::exists(folder)) {
        MessageBoxW(nullptr, L"该版本尚未下载，本地目录不存在。", L"无法打开目录", MB_ICONWARNING);
        return;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"open", folder.wstring().c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(result) <= 32) {
        MessageBoxW(nullptr, L"打开本地目录失败。", L"frpc-gui", MB_ICONWARNING);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::wstring error;
    EnsureAppDirectories(&error);
    EnsureDefaultVersionFiles(&error);
    LoadConfig(g_config, &error);
    LoadVersions();
    g_appLogs = LoadRecentLogLines(false);
    g_frpcLogs = LoadRecentLogLines(true);

    auto ui = AppWindow::create();
    slint::ComponentWeakHandle<AppWindow> weak(ui);

    AddAppLog(L"运行目录: " + GetAppDataDir(), weak);
    AddAppLog(L"日志目录: " + GetLogsDir(), weak);
    RefreshUi(ui);

    ui->on_start_service([ui, weak] { StartFrpc(ui, weak); });
    ui->on_stop_service([ui, weak] {
        AddAppLog(L"停止 frpc 服务", weak);
        g_frpc.Stop();
        g_frpc.StopExactFrpc(CurrentFrpcPath());
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_serviceFailed = false;
        }
        RefreshUi(ui);
    });
    ui->on_save_frps([ui] {
        if (SaveCurrentConfig(ui, false)) {
            MessageBoxW(nullptr, L"配置已保存。", L"frpc-gui", MB_ICONINFORMATION);
        }
    });
    ui->on_test_frps_connection([ui, weak] { StartConnectionTest(ui, weak); });
    ui->on_sync_versions([ui, weak] { StartRefreshVersions(ui, weak); });
    ui->on_download_current([ui, weak] { StartDownload(ui, weak); });
    ui->on_download_version([ui, weak](slint::SharedString version) {
        StartDownload(ui, weak, W(version));
    });
    ui->on_copy_download_url([ui](slint::SharedString version) {
        CopyDownloadUrl(ui, version);
    });
    ui->on_open_version_folder([](slint::SharedString version) {
        OpenVersionFolder(version);
    });
    ui->on_clear_current_log([ui](int tab) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (tab == 0) {
            g_appLogs.clear();
            ClearLogFile(false);
            ui->set_app_log_text("");
        } else {
            g_frpcLogs.clear();
            ClearLogFile(true);
            ui->set_frpc_log_text("");
        }
    });
    ui->on_add_proxy([ui](int typeIndex, slint::SharedString name, slint::SharedString localIp,
                          slint::SharedString localPortText, slint::SharedString remotePortText) {
        int localPort = 0;
        int remotePort = 0;
        ProxyConfig proxy;
        proxy.type = ProxyTypeFromIndex(typeIndex);
        proxy.name = W(name);
        proxy.localIP = W(localIp);
        if (!TryParsePort(W(localPortText), localPort) || !TryParsePort(W(remotePortText), remotePort)) {
            MessageBoxW(nullptr, L"端口无效，请输入 1-65535。", L"代理配置不完整", MB_ICONWARNING);
            return;
        }
        proxy.localPort = localPort;
        proxy.remotePort = remotePort;
        if (!ValidateProxy(proxy)) {
            MessageBoxW(nullptr, L"请完整填写代理名称、本地 IP、本地端口、远端端口。", L"代理配置不完整", MB_ICONWARNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_config.proxies.push_back(proxy);
            SaveConfig(g_config, nullptr);
            WriteFrpcToml(g_config, nullptr);
        }
        RefreshUi(ui);
    });
    ui->on_edit_proxy([ui](int index, int typeIndex, slint::SharedString name, slint::SharedString localIp,
                           slint::SharedString localPortText, slint::SharedString remotePortText) {
        int localPort = 0;
        int remotePort = 0;
        ProxyConfig proxy;
        proxy.type = ProxyTypeFromIndex(typeIndex);
        proxy.name = W(name);
        proxy.localIP = W(localIp);
        if (!TryParsePort(W(localPortText), localPort) || !TryParsePort(W(remotePortText), remotePort)) {
            MessageBoxW(nullptr, L"端口无效，请输入 1-65535。", L"代理配置不完整", MB_ICONWARNING);
            return;
        }
        proxy.localPort = localPort;
        proxy.remotePort = remotePort;
        if (!ValidateProxy(proxy)) {
            MessageBoxW(nullptr, L"请完整填写代理名称、本地 IP、本地端口、远端端口。", L"代理配置不完整", MB_ICONWARNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            size_t pos = index > 0 ? static_cast<size_t>(index - 1) : g_config.proxies.size();
            if (pos >= g_config.proxies.size()) {
                MessageBoxW(nullptr, L"代理不存在，可能已经被删除。", L"编辑失败", MB_ICONWARNING);
                return;
            }
            proxy.enabled = g_config.proxies[pos].enabled;
            g_config.proxies[pos] = proxy;
            SaveConfig(g_config, nullptr);
            WriteFrpcToml(g_config, nullptr);
        }
        RefreshUi(ui);
    });
    ui->on_toggle_proxy([ui, weak](int index) {
        ToggleProxyEnabled(ui, weak, index);
    });
    ui->on_test_proxy([ui, weak](int index) {
        StartProxyPortTest(ui, weak, index);
    });
    ui->on_test_proxy_input([ui, weak](int mode,
                                       slint::SharedString localIp,
                                       slint::SharedString localPortText,
                                       slint::SharedString remotePortText) {
        StartProxyInputPortTest(ui, weak, mode, localIp, localPortText, remotePortText);
    });
    ui->on_delete_proxy([ui](int index) {
        if (MessageBoxW(nullptr, L"确定删除这个代理吗？", L"删除代理",
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            size_t pos = index > 0 ? static_cast<size_t>(index - 1) : g_config.proxies.size();
            if (pos >= g_config.proxies.size()) {
                MessageBoxW(nullptr, L"代理不存在，可能已经被删除。", L"删除失败", MB_ICONWARNING);
                return;
            }
            g_config.proxies.erase(g_config.proxies.begin() + static_cast<ptrdiff_t>(pos));
            SaveConfig(g_config, nullptr);
            WriteFrpcToml(g_config, nullptr);
        }
        RefreshUi(ui);
    });

    ui->run();
    g_frpc.Stop();
    return 0;
}
