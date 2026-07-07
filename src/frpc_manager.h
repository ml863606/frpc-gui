#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "version_manager.h"

class FrpcManager {
public:
    using LogCallback = std::function<void(const std::wstring&)>;
    using ExitCallback = std::function<void(DWORD)>;
    using ProgressCallback = std::function<void(int)>;

    FrpcManager();
    ~FrpcManager();

    bool Start(const std::wstring& frpcPath,
               const std::wstring& configPath,
               const std::wstring& workDir,
               LogCallback log,
               ExitCallback onExit,
               std::wstring* error);
    void Stop();
    bool IsRunning() const;

    bool DownloadFrpc(const FrpVersionInfo& version,
                      const std::wstring& mirrorBase,
                      LogCallback log,
                      ProgressCallback progress,
                      std::wstring* error);

private:
    HANDLE GetProcessHandle() const;
    void CloseProcessHandles();

    mutable std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE mainThread_ = nullptr;
    std::atomic<bool> running_{false};
};
