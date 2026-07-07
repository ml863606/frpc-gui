#include "config.h"
#include "frpc_manager.h"
#include "resource.h"
#include "version_manager.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"FrpDeskWindow";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_APPEND_LOG = WM_APP + 2;
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 3;
constexpr UINT WM_FRPC_EXITED = WM_APP + 4;
constexpr UINT WM_VERSIONS_REFRESHED = WM_APP + 5;

constexpr UINT ID_TRAY = 100;
constexpr UINT ID_NAV_STATUS = 101;
constexpr UINT ID_NAV_PROXY = 102;
constexpr UINT ID_NAV_VERSION = 103;
constexpr UINT ID_NAV_FRPS = 104;
constexpr UINT ID_NAV_LOG = 105;
constexpr UINT ID_START = 201;
constexpr UINT ID_STOP = 202;
constexpr UINT ID_ADD_PROXY = 203;
constexpr UINT ID_SAVE_FRPS = 204;
constexpr UINT ID_DOWNLOAD = 205;
constexpr UINT ID_REFRESH_VERSIONS = 206;
constexpr UINT ID_ADD_PROXY_OK = 207;
constexpr UINT ID_ADD_PROXY_CANCEL = 208;
constexpr UINT ID_TRAY_SHOW = 301;
constexpr UINT ID_TRAY_START = 302;
constexpr UINT ID_TRAY_STOP = 303;
constexpr UINT ID_TRAY_EXIT = 304;

enum class Page : int {
    Status = 0,
    Proxy = 1,
    Version = 2,
    Frps = 3,
    Log = 4
};

struct Controls {
    HWND nav[5]{};
    HWND pages[5]{};

    HWND statusText{};
    HWND statusVersion{};
    HWND statusFrps{};
    HWND statusProxy{};
    HWND start{};
    HWND stop{};

    HWND proxySummary{};
    HWND proxyCards{};
    HWND addProxy{};

    HWND versionSummary{};
    HWND versionCards{};
    HWND refreshVersions{};
    HWND download{};
    HWND mirrorLabel{};
    HWND mirrorCombo{};

    HWND frpsLabels[5]{};
    HWND selectedVersion{};
    HWND frpsPublicIP{};
    HWND frpsPort{};
    HWND authMethod{};
    HWND authToken{};
    HWND saveFrps{};

    HWND log{};
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
Controls g_controls;
AppConfig g_config;
FrpcManager g_frpc;
std::vector<FrpVersionInfo> g_versions;
NOTIFYICONDATAW g_nid{};
Page g_currentPage = Page::Status;
bool g_downloading = false;
bool g_refreshingVersions = false;
bool g_exiting = false;

struct AddProxyDialogState {
    HWND hwnd{};
    HWND name{};
    HWND localIP{};
    HWND localPort{};
    HWND remotePort{};
    bool done = false;
    bool accepted = false;
    ProxyConfig proxy;
};

std::wstring LastErrorText(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = len > 0 ? std::wstring(buffer, len) : L"未知错误";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

void PostLog(const std::wstring& text) {
    if (!g_mainWindow) return;
    PostMessageW(g_mainWindow, WM_APPEND_LOG, 0,
                 reinterpret_cast<LPARAM>(new std::wstring(text)));
}

std::wstring GetText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(hwnd, value.data(), len + 1);
    value.resize(static_cast<size_t>(len));
    return value;
}

int GetIntText(HWND hwnd, int fallback) {
    BOOL translated = FALSE;
    int value = GetDlgItemInt(GetParent(hwnd), GetDlgCtrlID(hwnd), &translated, FALSE);
    return translated ? value : fallback;
}

void SetText(HWND hwnd, const std::wstring& value) {
    SetWindowTextW(hwnd, value.c_str());
}

void SetIntText(HWND hwnd, int value) {
    SetWindowTextW(hwnd, std::to_wstring(value).c_str());
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
                 int id, HWND parent) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text, style, 0, 0, 0, 0, parent,
                                reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                                g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return hwnd;
}

HWND MakeLabel(HWND parent, const wchar_t* text) {
    return MakeControl(L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, parent);
}

HWND MakeEdit(HWND parent, int id) {
    return MakeControl(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                       WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, id, parent);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text) {
    return MakeControl(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                       0, id, parent);
}

HWND MakePage(HWND parent) {
    return MakeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, parent);
}

LRESULT CALLBACK PageSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND && g_mainWindow) {
        return SendMessageW(g_mainWindow, msg, wParam, lParam);
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, PageSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void AppendLogToEdit(const std::wstring& rawText) {
    if (!g_controls.log) return;

    std::wstring text = rawText;
    for (wchar_t& ch : text) {
        if (ch == L'\n') ch = L'\r';
    }
    if (text.empty() || (text.back() != L'\r' && text.back() != L'\n')) {
        text += L"\r\n";
    }

    int len = GetWindowTextLengthW(g_controls.log);
    SendMessageW(g_controls.log, EM_SETSEL, len, len);
    SendMessageW(g_controls.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));

    LRESULT lineCount = SendMessageW(g_controls.log, EM_GETLINECOUNT, 0, 0);
    if (lineCount > 1000) {
        LRESULT cut = SendMessageW(g_controls.log, EM_LINEINDEX, lineCount - 1000, 0);
        if (cut > 0) {
            SendMessageW(g_controls.log, EM_SETSEL, 0, cut);
            SendMessageW(g_controls.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        }
    }
}

void FillVersionCombo() {
    SendMessageW(g_controls.selectedVersion, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (size_t i = 0; i < g_versions.size(); ++i) {
        const auto& item = g_versions[i];
        SendMessageW(g_controls.selectedVersion, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(item.displayName.c_str()));
        if (item.version == g_config.selectedVersion) {
            selected = static_cast<int>(i);
        }
    }
    SendMessageW(g_controls.selectedVersion, CB_SETCURSEL, selected, 0);
}

void FillMirrorCombo() {
    SendMessageW(g_controls.mirrorCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_controls.mirrorCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"https://gh.zwy.one"));
    SendMessageW(g_controls.mirrorCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"direct"));
    SetText(g_controls.mirrorCombo, g_config.downloadMirror);
}

void RebuildVersionCards() {
    if (!g_controls.versionCards) return;
    std::wstring text = L"";
    for (const auto& item : g_versions) {
        text += item.displayName + L"\r\n";
        text += L"版本号: " + item.version + L"\r\n";
        text += L"配置文件: " + GetVersionFilePath(item.version) + L"\r\n";
        text += L"frpc: " + GetFrpcPathForVersion(item.version);
        text += FileExists(GetFrpcPathForVersion(item.version)) ? L"  [已安装]\r\n\r\n" : L"  [未安装]\r\n\r\n";
    }
    if (text.empty()) {
        text = L"暂无版本配置。";
    }
    SetText(g_controls.versionCards, text);

    std::wstring summary = L"当前选择: " + g_config.selectedVersion +
        L"    版本目录: " + GetVersionsDir();
    SetText(g_controls.versionSummary, summary);
}

void RebuildProxyCards() {
    std::wstring text;
    for (size_t i = 0; i < g_config.proxies.size(); ++i) {
        const auto& proxy = g_config.proxies[i];
        text += L"TCP 代理 #" + std::to_wstring(i + 1) + L"\r\n";
        text += L"名称: " + proxy.name + L"\r\n";
        text += L"本地: " + proxy.localIP + L":" + std::to_wstring(proxy.localPort) + L"\r\n";
        text += L"远端端口: " + std::to_wstring(proxy.remotePort) + L"\r\n\r\n";
    }
    if (text.empty()) {
        text = L"暂无代理。点击“新增代理”创建 TCP 代理。";
    }
    SetText(g_controls.proxyCards, text);
    SetText(g_controls.proxySummary, L"TCP 代理数量: " + std::to_wstring(g_config.proxies.size()));
}

bool LoadVersions() {
    std::wstring error;
    if (!LoadFrpVersions(g_versions, &error)) {
        MessageBoxW(g_mainWindow, error.c_str(), L"读取版本失败", MB_ICONERROR);
        return false;
    }
    const FrpVersionInfo* current = FindFrpVersion(g_versions, g_config.selectedVersion);
    if (current) {
        g_config.selectedVersion = current->version;
    }
    if (g_controls.selectedVersion) {
        FillVersionCombo();
        RebuildVersionCards();
    }
    return true;
}

void FillUi(const AppConfig& config) {
    if (g_controls.selectedVersion) {
        FillVersionCombo();
    }
    if (g_controls.mirrorCombo) {
        FillMirrorCombo();
    }
    SetText(g_controls.frpsPublicIP, config.frpsPublicIP);
    SetIntText(g_controls.frpsPort, config.frpsPort);
    SendMessageW(g_controls.authMethod, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_controls.authMethod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"token"));
    SendMessageW(g_controls.authMethod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"none"));
    SendMessageW(g_controls.authMethod, CB_SETCURSEL, config.authMethod == L"none" ? 1 : 0, 0);
    SetText(g_controls.authToken, config.authToken);

    RebuildProxyCards();
}

bool ReadFrpsUi(AppConfig& config, bool requireComplete) {
    int sel = static_cast<int>(SendMessageW(g_controls.selectedVersion, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < static_cast<int>(g_versions.size())) {
        config.selectedVersion = g_versions[static_cast<size_t>(sel)].version;
    }

    config.frpsPublicIP = GetText(g_controls.frpsPublicIP);
    config.frpsPort = GetIntText(g_controls.frpsPort, 7000);
    int methodSel = static_cast<int>(SendMessageW(g_controls.authMethod, CB_GETCURSEL, 0, 0));
    config.authMethod = methodSel == 1 ? L"none" : L"token";
    config.authToken = GetText(g_controls.authToken);
    config.downloadMirror = GetText(g_controls.mirrorCombo);
    if (config.downloadMirror.empty()) {
        config.downloadMirror = L"https://gh.zwy.one";
    }

    if (requireComplete && config.frpsPublicIP.empty()) {
        MessageBoxW(g_mainWindow, L"请在 frps 设置中填写公网 IP 或域名。", L"配置不完整",
                    MB_ICONWARNING);
        return false;
    }
    return true;
}

bool ReadProxyUi(AppConfig& config) {
    if (config.proxies.empty()) {
        config.proxies.push_back(ProxyConfig{});
    }
    return true;
}

bool SaveCurrentConfig(bool showSuccess, bool requireComplete) {
    AppConfig next = g_config;
    if (!ReadFrpsUi(next, requireComplete) || !ReadProxyUi(next)) {
        return false;
    }

    std::wstring error;
    if (!SaveConfig(next, &error)) {
        MessageBoxW(g_mainWindow, error.c_str(), L"保存失败", MB_ICONERROR);
        return false;
    }
    if (!WriteFrpcToml(next, &error)) {
        MessageBoxW(g_mainWindow, error.c_str(), L"生成 frpc.toml 失败", MB_ICONERROR);
        return false;
    }
    g_config = next;
    RebuildProxyCards();
    RebuildVersionCards();
    PostLog(L"配置已保存: " + GetConfigPath());
    PostLog(L"frpc.toml 已生成: " + GetTomlPath());
    if (showSuccess) {
        MessageBoxW(g_mainWindow, L"配置已保存。", L"frp-desk", MB_ICONINFORMATION);
    }
    return true;
}

std::wstring CurrentFrpcPath() {
    return GetFrpcPathForVersion(g_config.selectedVersion);
}

const FrpVersionInfo* CurrentVersion() {
    return FindFrpVersion(g_versions, g_config.selectedVersion);
}

void UpdateButtons() {
    bool running = g_frpc.IsRunning();
    EnableWindow(g_controls.start, !running && !g_downloading);
    EnableWindow(g_controls.stop, running);
    EnableWindow(g_controls.download, !running && !g_downloading && !g_refreshingVersions);
    EnableWindow(g_controls.addProxy, !g_downloading && !running);
    EnableWindow(g_controls.saveFrps, !g_downloading);
    EnableWindow(g_controls.refreshVersions, !g_downloading && !g_refreshingVersions);
    EnableWindow(g_controls.mirrorCombo, !g_downloading);

    SetWindowTextW(g_controls.statusText, running ? L"状态：运行中" :
                   (g_downloading ? L"状态：正在下载 frpc" :
                    (g_refreshingVersions ? L"状态：正在同步版本" : L"状态：未运行")));

    SetText(g_controls.statusVersion, L"frp 版本：" + g_config.selectedVersion +
                                  (FileExists(CurrentFrpcPath()) ? L"（已安装）" : L"（未安装）"));
    SetText(g_controls.statusFrps, L"frps：" + g_config.frpsPublicIP + L":" +
                               std::to_wstring(g_config.frpsPort));
    SetText(g_controls.statusProxy, L"TCP 代理数量：" + std::to_wstring(g_config.proxies.size()));
}

void StartDownload() {
    if (g_downloading) return;
    AppConfig next = g_config;
    ReadFrpsUi(next, false);
    g_config.selectedVersion = next.selectedVersion;
    g_config.downloadMirror = next.downloadMirror;
    SaveConfig(g_config, nullptr);
    const FrpVersionInfo* version = CurrentVersion();
    if (!version) {
        MessageBoxW(g_mainWindow, L"没有可用的 frp 版本配置。", L"无法下载", MB_ICONWARNING);
        return;
    }
    FrpVersionInfo selected = *version;
    std::wstring mirror = g_config.downloadMirror;
    g_downloading = true;
    UpdateButtons();
    std::thread([selected, mirror]() {
        std::wstring error;
        bool ok = g_frpc.DownloadFrpc(selected, mirror, PostLog, &error);
        if (!ok) {
            PostLog(L"下载 frpc 失败: " + error);
            PostMessageW(g_mainWindow, WM_DOWNLOAD_DONE, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(error)));
            return;
        }
        PostMessageW(g_mainWindow, WM_DOWNLOAD_DONE, 1, 0);
    }).detach();
}

void StartRefreshVersions() {
    if (g_refreshingVersions) return;
    g_refreshingVersions = true;
    UpdateButtons();
    std::thread([]() {
        std::vector<FrpVersionInfo> versions;
        std::wstring error;
        bool ok = RefreshFrpVersionsFromGitHub(versions, PostLog, &error);
        if (ok) {
            PostMessageW(g_mainWindow, WM_VERSIONS_REFRESHED, 1,
                         reinterpret_cast<LPARAM>(new std::vector<FrpVersionInfo>(std::move(versions))));
        } else {
            PostLog(L"同步 GitHub 版本失败: " + error);
            PostMessageW(g_mainWindow, WM_VERSIONS_REFRESHED, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(error)));
        }
    }).detach();
}

void StartFrpc() {
    if (!SaveCurrentConfig(false, true)) {
        return;
    }
    if (!FileExists(CurrentFrpcPath())) {
        MessageBoxW(g_mainWindow, L"当前版本未安装 frpc.exe，请到 frp版本管理 页面手动下载。", L"无法启动",
                    MB_ICONWARNING);
        return;
    }

    std::wstring error;
    bool ok = g_frpc.Start(CurrentFrpcPath(), GetTomlPath(),
                           std::filesystem::path(CurrentFrpcPath()).parent_path().wstring(),
                           PostLog,
                           [](DWORD) { PostMessageW(g_mainWindow, WM_FRPC_EXITED, 0, 0); },
                           &error);
    if (!ok) {
        MessageBoxW(g_mainWindow, error.c_str(), L"启动失败", MB_ICONERROR);
        PostLog(L"启动失败: " + error);
    }
    UpdateButtons();
}

void StopFrpc() {
    g_frpc.Stop();
    PostLog(L"已请求停止 frpc");
    UpdateButtons();
}

void SwitchPage(Page page) {
    g_currentPage = page;
    for (int i = 0; i < 5; ++i) {
        ShowWindow(g_controls.pages[i], i == static_cast<int>(page) ? SW_SHOW : SW_HIDE);
        EnableWindow(g_controls.nav[i], i != static_cast<int>(page));
    }
    if (page == Page::Version) {
        LoadVersions();
    }
    UpdateButtons();
}

void AddTrayIcon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(g_nid.szTip, L"frp-desk");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    if (g_nid.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }
}

void ShowMainWindow() {
    ShowWindow(g_mainWindow, SW_SHOW);
    SetForegroundWindow(g_mainWindow);
}

void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"显示窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, g_frpc.IsRunning() ? MF_GRAYED : MF_STRING, ID_TRAY_START, L"启动");
    AppendMenuW(menu, g_frpc.IsRunning() ? MF_STRING : MF_GRAYED, ID_TRAY_STOP, L"停止");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");
    SetForegroundWindow(g_mainWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_mainWindow, nullptr);
    DestroyMenu(menu);
}

void LayoutFormRow(HWND label, HWND edit, int x, int& y, int labelW, int editW);

LRESULT CALLBACK AddProxyDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AddProxyDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AddProxyDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd = hwnd;

        HWND labels[4]{};
        labels[0] = MakeLabel(hwnd, L"代理名称");
        state->name = MakeEdit(hwnd, 2101);
        labels[1] = MakeLabel(hwnd, L"本地 IP");
        state->localIP = MakeEdit(hwnd, 2102);
        labels[2] = MakeLabel(hwnd, L"本地端口");
        state->localPort = MakeEdit(hwnd, 2103);
        labels[3] = MakeLabel(hwnd, L"远端端口");
        state->remotePort = MakeEdit(hwnd, 2104);

        MakeButton(hwnd, ID_ADD_PROXY_OK, L"保存");
        MakeButton(hwnd, ID_ADD_PROXY_CANCEL, L"取消");

        SetText(state->name, state->proxy.name);
        SetText(state->localIP, state->proxy.localIP);
        SetIntText(state->localPort, state->proxy.localPort);
        SetIntText(state->remotePort, state->proxy.remotePort);

        for (int i = 0; i < 4; ++i) {
            SetWindowLongPtrW(labels[i], GWLP_ID, 2200 + i);
        }
        return 0;
    }
    case WM_SIZE: {
        int margin = 16;
        int y = 16;
        int labelW = 84;
        int editW = 250;
        HWND labels[4]{
            GetDlgItem(hwnd, 2200),
            GetDlgItem(hwnd, 2201),
            GetDlgItem(hwnd, 2202),
            GetDlgItem(hwnd, 2203)
        };
        LayoutFormRow(labels[0], state->name, margin, y, labelW, editW);
        LayoutFormRow(labels[1], state->localIP, margin, y, labelW, editW);
        LayoutFormRow(labels[2], state->localPort, margin, y, labelW, editW);
        LayoutFormRow(labels[3], state->remotePort, margin, y, labelW, editW);
        MoveWindow(GetDlgItem(hwnd, ID_ADD_PROXY_OK), margin + labelW + 12, y + 8, 88, 30, TRUE);
        MoveWindow(GetDlgItem(hwnd, ID_ADD_PROXY_CANCEL), margin + labelW + 108, y + 8, 88, 30, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_ADD_PROXY_OK) {
            state->proxy.name = GetText(state->name);
            state->proxy.localIP = GetText(state->localIP);
            state->proxy.localPort = GetIntText(state->localPort, 0);
            state->proxy.remotePort = GetIntText(state->remotePort, 0);
            if (state->proxy.name.empty() || state->proxy.localIP.empty() ||
                state->proxy.localPort <= 0 || state->proxy.remotePort <= 0) {
                MessageBoxW(hwnd, L"请完整填写代理名称、本地 IP、本地端口、远端端口。",
                            L"代理配置不完整", MB_ICONWARNING);
                return 0;
            }
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == ID_ADD_PROXY_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->done = true;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowAddProxyDialog(ProxyConfig& proxy) {
    constexpr wchar_t kAddProxyClass[] = L"FrpDeskAddProxyWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AddProxyDialogProc;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kAddProxyClass;
        if (!RegisterClassExW(&wc)) {
            MessageBoxW(g_mainWindow, (L"注册新增代理窗口失败: " + LastErrorText()).c_str(),
                        L"frp-desk", MB_ICONERROR);
            return false;
        }
        registered = true;
    }

    AddProxyDialogState state;
    state.proxy = proxy;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kAddProxyClass, L"新增 TCP 代理",
                                WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 410, 235,
                                g_mainWindow, nullptr, g_instance, &state);
    if (!hwnd) {
        MessageBoxW(g_mainWindow, (L"创建新增代理窗口失败: " + LastErrorText()).c_str(),
                    L"frp-desk", MB_ICONERROR);
        return false;
    }

    EnableWindow(g_mainWindow, FALSE);
    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_mainWindow, TRUE);
    SetForegroundWindow(g_mainWindow);
    if (state.accepted) {
        proxy = state.proxy;
    }
    return state.accepted;
}

void AddProxy() {
    ProxyConfig proxy;
    proxy.name = L"tcp" + std::to_wstring(g_config.proxies.size() + 1);
    if (ShowAddProxyDialog(proxy)) {
        g_config.proxies.push_back(proxy);
        SaveCurrentConfig(false, false);
        RebuildProxyCards();
        UpdateButtons();
        PostLog(L"已新增 TCP 代理: " + proxy.name);
    }
}

void LayoutFormRow(HWND label, HWND edit, int x, int& y, int labelW, int editW) {
    MoveWindow(label, x, y + 5, labelW, 20, TRUE);
    MoveWindow(edit, x + labelW + 12, y, editW, 26, TRUE);
    y += 34;
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = static_cast<int>(rc.right - rc.left);
    int h = static_cast<int>(rc.bottom - rc.top);
    int menuW = 178;
    int margin = 14;
    int pageX = menuW + 1;
    int pageW = std::max(360, w - pageX);

    for (int i = 0; i < 5; ++i) {
        MoveWindow(g_controls.nav[i], 12, 20 + i * 42, menuW - 24, 34, TRUE);
        MoveWindow(g_controls.pages[i], pageX, 0, pageW, h, TRUE);
    }

    int x = margin;
    int y = 20;
    int fullW = pageW - margin * 2;

    MoveWindow(g_controls.statusText, x, y, fullW, 28, TRUE); y += 42;
    MoveWindow(g_controls.statusVersion, x, y, fullW, 24, TRUE); y += 30;
    MoveWindow(g_controls.statusFrps, x, y, fullW, 24, TRUE); y += 30;
    MoveWindow(g_controls.statusProxy, x, y, fullW, 24, TRUE); y += 44;
    MoveWindow(g_controls.start, x, y, 100, 34, TRUE);
    MoveWindow(g_controls.stop, x + 112, y, 100, 34, TRUE);

    y = 20;
    int labelW = 92;
    int editW = std::max(160, fullW - labelW - 12);
    MoveWindow(g_controls.proxySummary, x, y, fullW, 24, TRUE); y += 34;
    MoveWindow(g_controls.addProxy, x, y, 120, 32, TRUE); y += 46;
    MoveWindow(g_controls.proxyCards, x, y, fullW, std::max(160, h - y - margin), TRUE);

    y = 20;
    MoveWindow(g_controls.versionSummary, x, y, fullW, 24, TRUE); y += 34;
    MoveWindow(g_controls.mirrorLabel, x, y + 5, 82, 22, TRUE);
    MoveWindow(g_controls.mirrorCombo, x + 92, y, std::min(260, fullW - 92), 120, TRUE);
    y += 38;
    MoveWindow(g_controls.refreshVersions, x, y, 140, 32, TRUE);
    MoveWindow(g_controls.download, x + 152, y, 130, 32, TRUE); y += 46;
    MoveWindow(g_controls.versionCards, x, y, fullW, std::max(160, h - y - margin), TRUE);

    y = 20;
    LayoutFormRow(g_controls.frpsLabels[0], g_controls.selectedVersion, x, y, labelW, editW);
    LayoutFormRow(g_controls.frpsLabels[1], g_controls.frpsPublicIP, x, y, labelW, editW);
    LayoutFormRow(g_controls.frpsLabels[2], g_controls.frpsPort, x, y, labelW, editW);
    LayoutFormRow(g_controls.frpsLabels[3], g_controls.authMethod, x, y, labelW, editW);
    LayoutFormRow(g_controls.frpsLabels[4], g_controls.authToken, x, y, labelW, editW);
    MoveWindow(g_controls.saveFrps, x, y + 10, 120, 34, TRUE);

    MoveWindow(g_controls.log, margin, margin, pageW - margin * 2, h - margin * 2, TRUE);
}

void CreateControls(HWND hwnd) {
    g_controls.nav[0] = MakeButton(hwnd, ID_NAV_STATUS, L"运行状态");
    g_controls.nav[1] = MakeButton(hwnd, ID_NAV_PROXY, L"代理设置");
    g_controls.nav[2] = MakeButton(hwnd, ID_NAV_VERSION, L"frp版本管理");
    g_controls.nav[3] = MakeButton(hwnd, ID_NAV_FRPS, L"frps设置");
    g_controls.nav[4] = MakeButton(hwnd, ID_NAV_LOG, L"日志界面");

    for (int i = 0; i < 5; ++i) {
        g_controls.pages[i] = MakePage(hwnd);
        SetWindowSubclass(g_controls.pages[i], PageSubclassProc, 1, 0);
    }

    HWND statusPage = g_controls.pages[static_cast<int>(Page::Status)];
    g_controls.statusText = MakeLabel(statusPage, L"状态：未运行");
    g_controls.statusVersion = MakeLabel(statusPage, L"frp 版本：");
    g_controls.statusFrps = MakeLabel(statusPage, L"frps：");
    g_controls.statusProxy = MakeLabel(statusPage, L"TCP 代理：");
    g_controls.start = MakeButton(statusPage, ID_START, L"启动服务");
    g_controls.stop = MakeButton(statusPage, ID_STOP, L"停止服务");

    HWND proxyPage = g_controls.pages[static_cast<int>(Page::Proxy)];
    g_controls.proxySummary = MakeLabel(proxyPage, L"TCP 代理数量: 0");
    g_controls.addProxy = MakeButton(proxyPage, ID_ADD_PROXY, L"新增代理");
    g_controls.proxyCards = MakeControl(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                        WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                        ES_READONLY, WS_EX_CLIENTEDGE, 1001, proxyPage);

    HWND versionPage = g_controls.pages[static_cast<int>(Page::Version)];
    g_controls.versionSummary = MakeLabel(versionPage, L"版本目录：");
    g_controls.mirrorLabel = MakeLabel(versionPage, L"镜像站点");
    g_controls.mirrorCombo = MakeControl(L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         CBS_DROPDOWN | WS_VSCROLL,
                                         0, 1100, versionPage);
    g_controls.refreshVersions = MakeButton(versionPage, ID_REFRESH_VERSIONS, L"同步GitHub版本");
    g_controls.download = MakeButton(versionPage, ID_DOWNLOAD, L"下载当前版本");
    g_controls.versionCards = MakeControl(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                          WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                          ES_READONLY, WS_EX_CLIENTEDGE, 1101, versionPage);

    HWND frpsPage = g_controls.pages[static_cast<int>(Page::Frps)];
    g_controls.frpsLabels[0] = MakeLabel(frpsPage, L"frp 版本");
    g_controls.selectedVersion = MakeControl(L"COMBOBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                             CBS_DROPDOWNLIST | WS_VSCROLL,
                                             0, 1201, frpsPage);
    g_controls.frpsLabels[1] = MakeLabel(frpsPage, L"公网 IP");
    g_controls.frpsPublicIP = MakeEdit(frpsPage, 1202);
    g_controls.frpsLabels[2] = MakeLabel(frpsPage, L"端口");
    g_controls.frpsPort = MakeEdit(frpsPage, 1203);
    g_controls.frpsLabels[3] = MakeLabel(frpsPage, L"验证方式");
    g_controls.authMethod = MakeControl(L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                        CBS_DROPDOWNLIST | WS_VSCROLL,
                                        0, 1204, frpsPage);
    g_controls.frpsLabels[4] = MakeLabel(frpsPage, L"Token");
    g_controls.authToken = MakeEdit(frpsPage, 1205);
    g_controls.saveFrps = MakeButton(frpsPage, ID_SAVE_FRPS, L"保存 frps");

    HWND logPage = g_controls.pages[static_cast<int>(Page::Log)];
    g_controls.log = MakeControl(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                 WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                 ES_READONLY, WS_EX_CLIENTEDGE, 1301, logPage);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_mainWindow = hwnd;
        CreateControls(hwnd);
        LoadVersions();
        FillUi(g_config);
        AddTrayIcon(hwnd);
        SwitchPage(Page::Status);
        PostLog(L"运行目录: " + GetAppDataDir());
        return 0;
    case WM_SIZE:
        Layout(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_NAV_STATUS: SwitchPage(Page::Status); break;
        case ID_NAV_PROXY: SwitchPage(Page::Proxy); break;
        case ID_NAV_VERSION: SwitchPage(Page::Version); break;
        case ID_NAV_FRPS: SwitchPage(Page::Frps); break;
        case ID_NAV_LOG: SwitchPage(Page::Log); break;
        case ID_ADD_PROXY:
            AddProxy();
            break;
        case ID_SAVE_FRPS:
            SaveCurrentConfig(true, false);
            UpdateButtons();
            break;
        case ID_START:
        case ID_TRAY_START:
            StartFrpc();
            break;
        case ID_STOP:
        case ID_TRAY_STOP:
            StopFrpc();
            break;
        case ID_DOWNLOAD:
            StartDownload();
            break;
        case ID_REFRESH_VERSIONS:
            StartRefreshVersions();
            break;
        case ID_TRAY_SHOW:
            ShowMainWindow();
            break;
        case ID_TRAY_EXIT:
            g_exiting = true;
            DestroyWindow(hwnd);
            break;
        default:
            if (HIWORD(wParam) == CBN_SELCHANGE &&
                reinterpret_cast<HWND>(lParam) == g_controls.selectedVersion) {
                AppConfig next = g_config;
                ReadFrpsUi(next, false);
                g_config.selectedVersion = next.selectedVersion;
                RebuildVersionCards();
                UpdateButtons();
            }
            break;
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowMainWindow();
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    case WM_APPEND_LOG: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        AppendLogToEdit(*text);
        return 0;
    }
    case WM_DOWNLOAD_DONE:
        g_downloading = false;
        RebuildVersionCards();
        UpdateButtons();
        if (wParam == 0) {
            std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
            MessageBoxW(hwnd,
                        (L"下载 frpc 失败。\n\n" + *error +
                         L"\n\n可以手动把 frpc.exe 放到:\n" + CurrentFrpcPath()).c_str(),
                        L"下载失败", MB_ICONWARNING);
        } else {
            MessageBoxW(hwnd, L"frpc.exe 下载完成。", L"frp-desk", MB_ICONINFORMATION);
        }
        return 0;
    case WM_FRPC_EXITED:
        UpdateButtons();
        return 0;
    case WM_VERSIONS_REFRESHED:
        g_refreshingVersions = false;
        if (wParam == 1) {
            std::unique_ptr<std::vector<FrpVersionInfo>> versions(
                reinterpret_cast<std::vector<FrpVersionInfo>*>(lParam));
            g_versions = std::move(*versions);
            const FrpVersionInfo* current = FindFrpVersion(g_versions, g_config.selectedVersion);
            if (current) {
                g_config.selectedVersion = current->version;
            }
            FillVersionCombo();
            RebuildVersionCards();
            MessageBoxW(hwnd, L"GitHub frp 版本同步完成。", L"frp-desk", MB_ICONINFORMATION);
        } else {
            std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
            MessageBoxW(hwnd, (L"同步 GitHub 版本失败。\n\n" + *error).c_str(),
                        L"同步失败", MB_ICONWARNING);
        }
        UpdateButtons();
        return 0;
    case WM_CLOSE:
        if (!g_exiting) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon();
        g_frpc.Stop();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCmd) {
    g_instance = instance;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    std::wstring error;
    if (!EnsureAppDirectories(&error)) {
        MessageBoxW(nullptr, error.c_str(), L"frp-desk", MB_ICONERROR);
        return 1;
    }
    EnsureDefaultVersionFiles(&error);
    LoadConfig(g_config, &error);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, (L"注册窗口类失败: " + LastErrorText()).c_str(),
                    L"frp-desk", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"frp-desk - 极轻 frp 客户端",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                760, 560, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, (L"创建窗口失败: " + LastErrorText()).c_str(),
                    L"frp-desk", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
