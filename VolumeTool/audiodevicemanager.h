#pragma once

#include <QString>

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <vector>

struct AudioDeviceEntry {
    QString id;
    QString name;
    bool isVirtual = false;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *volume = nullptr;
};

struct AudioDeviceSnapshot {
    QString id;
    QString name;
    bool isVirtual = false;
};

class AudioDeviceManager
{
public:
    // 枚举当前可用的播放设备，并按需过滤虚拟设备。
    std::vector<AudioDeviceEntry> loadRenderDevices(bool includeVirtual) const;
    // 获取所有播放设备的轻量信息，供设置页列表展示。
    std::vector<AudioDeviceSnapshot> loadAllRenderDeviceSnapshots() const;
    // 释放枚举阶段缓存的 COM 设备接口。
    void releaseDevices(std::vector<AudioDeviceEntry> &devices) const;

    // 获取 Windows 默认播放设备的音量接口，可选返回设备 ID。
    bool getDefaultRenderEndpoint(IAudioEndpointVolume **volume, QString *deviceId = nullptr) const;
    // 读取 Windows 默认播放设备当前音量。
    bool getDefaultRenderVolume(float *volumeScalar) const;
    // 读取指定音量接口当前的音量值。
    bool readDeviceVolume(IAudioEndpointVolume *volume, float *volumeScalar) const;
    // 通过已有音量接口设置音量。
    bool setDeviceVolume(IAudioEndpointVolume *volume, float volumeScalar, const GUID *eventContext = nullptr) const;
    // 通过设备 ID 重新定位设备并设置音量。
    bool setDeviceVolumeById(const QString &deviceId, float volumeScalar, const GUID *eventContext = nullptr) const;
    // 创建用于监听系统音频设备变化的枚举器。
    IMMDeviceEnumerator *createNotificationEnumerator() const;

private:
    // 创建 Core Audio 设备枚举器。
    IMMDeviceEnumerator *createEnumerator() const;
    // 读取设备在系统中的显示名称。
    QString readDeviceName(IMMDevice *device) const;
    // 读取设备唯一 ID。
    QString readDeviceId(IMMDevice *device) const;
    // 根据名称和 ID 判断该设备是否属于常见虚拟设备。
    bool isVirtualDevice(IMMDevice *device) const;
};
