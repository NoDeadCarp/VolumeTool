#include "voicemeeterremoteclient.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>

namespace {

using VBVMR_Login_Fn = long(__stdcall *)();
using VBVMR_Logout_Fn = long(__stdcall *)();
using VBVMR_RunVoicemeeter_Fn = long(__stdcall *)(long);
using VBVMR_SetParameterFloat_Fn = long(__stdcall *)(char *, float);

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

    if (!login || !logout || !runVoicemeeter || !setParameterFloat) {
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

    char restartCommand[] = "Command.Restart";
    result = setParameterFloat(restartCommand, 1.0f);
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
