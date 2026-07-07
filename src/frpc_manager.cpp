#include "frpc_manager.h"

#include "config.h"

#include <winhttp.h>
#include <windows.h>

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>

namespace {

std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            out += L'\\';
        }
        out += ch;
    }
    out += L"\"";
    return out;
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

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    std::filesystem::path path(left);
    path /= right;
    return path.wstring();
}

bool RunHidden(const std::wstring& commandLine, const std::wstring& workDir, DWORD* exitCode) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) {
        *exitCode = code;
    }
    return code == 0;
}

bool DownloadToFile(const std::wstring& url, const std::wstring& filePath,
                    const FrpcManager::LogCallback& log,
                    const FrpcManager::ProgressCallback& progress,
                    std::wstring* error) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);

    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(_countof(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(_countof(path));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        if (error) *error = L"解析下载地址失败: " + LastErrorText();
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
        if (error) *error = L"连接下载服务器失败: " + LastErrorText();
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", std::wstring(path, parts.dwUrlPathLength).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        if (error) *error = L"创建下载请求失败: " + LastErrorText();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    if (!received) {
        if (error) *error = L"下载请求失败: " + LastErrorText();
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
        if (error) {
            std::wostringstream ss;
            ss << L"下载失败，HTTP 状态码: " << status;
            *error = ss.str();
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &contentLength,
                             &contentLengthSize, WINHTTP_NO_HEADER_INDEX)) {
        contentLength = 0;
    }

    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"无法创建下载文件: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    log(L"开始下载 frp 包...");
    if (progress) progress(5);
    std::vector<char> buffer(64 * 1024);
    unsigned long long total = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            if (error) *error = L"读取下载数据失败: " + LastErrorText();
            CloseHandle(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (available == 0) {
            break;
        }

        DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &read)) {
            if (error) *error = L"下载中断: " + LastErrorText();
            CloseHandle(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (read == 0) {
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
            if (error) *error = L"写入下载文件失败: " + LastErrorText();
            CloseHandle(file);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        total += read;
        if (progress && contentLength > 0) {
            int percent = 5 + static_cast<int>((total * 80) / contentLength);
            if (percent > 85) percent = 85;
            progress(percent);
        }
        if (total % (1024 * 1024) < read) {
            std::wostringstream ss;
            ss << L"已下载 " << (total / 1024 / 1024) << L" MB";
            log(ss.str());
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
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

} // namespace

FrpcManager::FrpcManager() = default;

FrpcManager::~FrpcManager() {
    Stop();
}

HANDLE FrpcManager::GetProcessHandle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return process_;
}

void FrpcManager::CloseProcessHandles() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mainThread_) {
        CloseHandle(mainThread_);
        mainThread_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool FrpcManager::Start(const std::wstring& frpcPath,
                        const std::wstring& configPath,
                        const std::wstring& workDir,
                        LogCallback log,
                        ExitCallback onExit,
                        std::wstring* error) {
    if (running_) {
        if (error) *error = L"frpc 已在运行";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        if (error) *error = L"创建日志管道失败: " + LastErrorText();
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = Quote(frpcPath) + L" -c " + Quote(configPath);
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(frpcPath.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        if (error) *error = L"启动 frpc 失败: " + LastErrorText();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = pi.hProcess;
        mainThread_ = pi.hThread;
    }
    running_ = true;
    log(L"frpc 已启动");

    std::thread([readPipe, log]() {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) && read > 0) {
            log(Utf8ToWide(std::string(buffer, buffer + read)));
        }
        CloseHandle(readPipe);
    }).detach();

    HANDLE waitHandle = pi.hProcess;
    std::thread([this, waitHandle, log, onExit]() {
        WaitForSingleObject(waitHandle, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(waitHandle, &code);
        running_ = false;
        log(L"frpc 已退出，退出码: " + std::to_wstring(code));
        if (onExit) {
            onExit(code);
        }
        CloseProcessHandles();
    }).detach();

    return true;
}

void FrpcManager::Stop() {
    HANDLE process = GetProcessHandle();
    if (!process || !running_) {
        return;
    }

    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 3000);
    }
    running_ = false;
}

bool FrpcManager::IsRunning() const {
    return running_;
}

bool FrpcManager::DownloadFrpc(const FrpVersionInfo& version,
                               const std::wstring& mirrorBase,
                               LogCallback log,
                               ProgressCallback progress,
                               std::wstring* error) {
    std::wstring dirError;
    if (!EnsureAppDirectories(&dirError)) {
        if (error) *error = dirError;
        return false;
    }

    std::wstring targetFrpc = GetFrpcPathForVersion(version.version);
    std::wstring archive = JoinPath(GetDownloadsDir(), version.archiveName);
    std::wstring extractRoot = JoinPath(GetDownloadsDir(), L"extract-" + version.version);
    std::wstring extractedFrpc = JoinPath(JoinPath(extractRoot, version.extractDir), L"frpc.exe");

    try {
        std::filesystem::remove_all(extractRoot);
        std::filesystem::create_directories(extractRoot);
    } catch (const std::exception& ex) {
        if (error) *error = L"准备解压目录失败: " + Utf8ToWide(ex.what());
        return false;
    }

    std::wstring downloadUrl = BuildDownloadUrl(version, mirrorBase);
    log(L"准备下载 " + version.displayName);
    log(L"下载地址: " + downloadUrl);
    if (!DownloadToFile(downloadUrl, archive, log, progress, error)) {
        return false;
    }

    log(L"下载完成，开始解压...");
    if (progress) progress(88);
    std::wstring tarPath = L"C:\\Windows\\System32\\tar.exe";
    if (!FileExists(tarPath)) {
        tarPath = L"tar.exe";
    }
    DWORD exitCode = 1;
    std::wstring cmd = Quote(tarPath) + L" -xf " + Quote(archive) + L" -C " + Quote(extractRoot);
    if (!RunHidden(cmd, GetDownloadsDir(), &exitCode)) {
        if (error) {
            *error = L"解压 frp 压缩包失败，tar 退出码: " + std::to_wstring(exitCode);
        }
        return false;
    }

    if (!FileExists(extractedFrpc)) {
        if (error) *error = L"解压后未找到 frpc.exe";
        return false;
    }

    try {
        if (progress) progress(94);
        std::filesystem::create_directories(std::filesystem::path(targetFrpc).parent_path());
        std::filesystem::copy_file(extractedFrpc, targetFrpc,
                                   std::filesystem::copy_options::overwrite_existing);
        log(L"frpc.exe 已安装到: " + targetFrpc);
        if (progress) progress(100);
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = L"复制 frpc.exe 失败: " + Utf8ToWide(ex.what());
        return false;
    }
}
