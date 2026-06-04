#include "voicemeeterremoteclient.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>

namespace {

using VBVMR_Login_Fn = long(__stdcall *)();
using VBVMR_Logout_Fn = long(__stdcall *)();
using VBVMR_RunVoicemeeter_Fn = long(__stdcall *)(long);
using VBVMR_SetParameterFloat_Fn = long(__stdcall *)(char *, float);
using VBVMR_IsParametersDirty_Fn = long(__stdcall *)();

constexpr long kOk = 0;
constexpr int kMaxReadyRetries = 50;
constexpr int kReadyWaitMs = 40;

}

QString VoicemeeterRemoteClient::resolveDllPath(const QString &installPath) const
{
    const QDir dir(installPath);
    const QStringList candidates = {
#if defined(_WIN64)
        QStringLiteral("VoicemeeterRemote64.dll"),
#endif
        QStringLiteral("VoicemeeterRemote.dll")
    };

    for (const QString &fileName : candidates) {
        const QString path = dir.absoluteFilePath(fileName);
        if (QFileInfo::exists(path)) {
            return QDir::toNativeSeparators(path);
        }
    }

    return {};
}

bool VoicemeeterRemoteClient::isAvailable(const QString &installPath) const
{
    return !resolveDllPath(installPath).isEmpty();
}

bool VoicemeeterRemoteClient::restartAudioEngine(const QString &installPath, QString *errorMessage) const
{
    const QString dllPath = resolveDllPath(installPath);
    if (dllPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("未找到 Voicemeeter Remote API DLL。");
        }
        return false;
    }

    HMODULE module = LoadLibraryW(reinterpret_cast<LPCWSTR>(dllPath.utf16()));
    if (!module) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Remote API DLL 加载失败。");
        }
        return false;
    }

    const auto login = reinterpret_cast<VBVMR_Login_Fn>(GetProcAddress(module, "VBVMR_Login"));
    const auto logout = reinterpret_cast<VBVMR_Logout_Fn>(GetProcAddress(module, "VBVMR_Logout"));
    const auto runVoicemeeter = reinterpret_cast<VBVMR_RunVoicemeeter_Fn>(GetProcAddress(module, "VBVMR_RunVoicemeeter"));
    const auto setParameterFloat = reinterpret_cast<VBVMR_SetParameterFloat_Fn>(GetProcAddress(module, "VBVMR_SetParameterFloat"));
    const auto isParametersDirty = reinterpret_cast<VBVMR_IsParametersDirty_Fn>(GetProcAddress(module, "VBVMR_IsParametersDirty"));

    if (!login || !logout || !runVoicemeeter || !setParameterFloat || !isParametersDirty) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Remote API DLL 缺少必要导出函数。");
        }
        FreeLibrary(module);
        return false;
    }

    long result = login();
    if (result < 0) {
        // 未运行时尝试拉起 Voicemeeter，再重新登录一次。
        runVoicemeeter(0);
        Sleep(1200);
        result = login();
    }

    if (result < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Voicemeeter 登录失败，错误码：%1").arg(result);
        }
        FreeLibrary(module);
        return false;
    }

    // Login 成功后轮询等待 Voicemeeter 真正就绪。
    bool ready = false;
    for (int i = 0; i < kMaxReadyRetries; ++i) {
        if (isParametersDirty() >= kOk) {
            ready = true;
            break;
        }
        Sleep(kReadyWaitMs);
    }

    if (!ready) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Voicemeeter 登录成功但未就绪，等待超时。");
        }
        logout();
        FreeLibrary(module);
        return false;
    }

    char restartCommand[] = "Command.Restart";
    result = setParameterFloat(restartCommand, 1.0f);

    // 等待一段时间让 Voicemeeter 有机会处理重启命令后再断开连接。
    Sleep(500);
    logout();
    FreeLibrary(module);

    if (result < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("发送 Command.Restart 失败，错误码：%1").arg(result);
        }
        return false;
    }

    return true;
}
