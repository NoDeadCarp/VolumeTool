#pragma once

#include <QString>

class VoicemeeterRemoteClient
{
public:
    // 检测安装目录下是否存在可用的 Remote API DLL。
    bool isAvailable(const QString &installPath) const;
    // 通过 Remote API 连接 Voicemeeter 并执行 Command.Restart=1。
    bool restartAudioEngine(const QString &installPath, QString *errorMessage = nullptr) const;

private:
    // 根据安装目录选择当前进程可加载的 Remote API DLL。
    QString resolveDllPath(const QString &installPath) const;
};
