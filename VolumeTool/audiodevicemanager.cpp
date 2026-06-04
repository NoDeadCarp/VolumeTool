#include "audiodevicemanager.h"

#include <QDebug>

#include <functiondiscoverykeys_devpkey.h>

namespace {

constexpr DWORD kActiveRenderDevices = DEVICE_STATE_ACTIVE;

}

IMMDeviceEnumerator *AudioDeviceManager::createEnumerator() const
{
    // 设备枚举是所有音频操作的入口，这里集中创建。
    IMMDeviceEnumerator *enumerator = nullptr;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                        nullptr,
                                        CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        qWarning() << "CoCreateInstance failed:" << Qt::hex << hr;
        return nullptr;
    }

    return enumerator;
}

IMMDeviceEnumerator *AudioDeviceManager::createNotificationEnumerator() const
{
    // 设备变化通知和设备枚举共用同一类枚举器，这里单独暴露给窗口层注册通知。
    return createEnumerator();
}

QString AudioDeviceManager::readDeviceName(IMMDevice *device) const
{
    // 控制页和设置页都用系统友好名称展示设备。
    if (!device) {
        return {};
    }

    IPropertyStore *props = nullptr;
    const HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props) {
        return {};
    }

    PROPVARIANT varName;
    PropVariantInit(&varName);
    const HRESULT nameHr = props->GetValue(PKEY_Device_FriendlyName, &varName);

    QString name;
    if (SUCCEEDED(nameHr) && varName.vt == VT_LPWSTR && varName.pwszVal) {
        name = QString::fromWCharArray(varName.pwszVal);
    }

    PropVariantClear(&varName);
    props->Release();
    return name;
}

QString AudioDeviceManager::readDeviceId(IMMDevice *device) const
{
    // 设备 ID 用来跨页面、跨刷新稳定定位同一个设备。
    if (!device) {
        return {};
    }

    LPWSTR rawDeviceId = nullptr;
    const HRESULT hr = device->GetId(&rawDeviceId);
    if (FAILED(hr) || !rawDeviceId) {
        return {};
    }

    const QString id = QString::fromWCharArray(rawDeviceId);
    CoTaskMemFree(rawDeviceId);
    return id;
}

bool AudioDeviceManager::isVirtualDevice(IMMDevice *device) const
{
    // 这里用简单规则识别常见虚拟音频设备。
    const QString name = readDeviceName(device);
    const QString id = readDeviceId(device);

    return name.contains("Virtual", Qt::CaseInsensitive)
        || name.contains("VB-Audio", Qt::CaseInsensitive)
        || name.contains("Voicemeeter", Qt::CaseInsensitive)
        || name.contains("AMD High Definition Audio", Qt::CaseInsensitive)
        || name.contains("NVIDIA High Definition Audio", Qt::CaseInsensitive)
        || name.contains("NVIDIA Virtual Audio", Qt::CaseInsensitive)
        || id.startsWith("ROOT\\")
        || id.contains("SWD\\")
        || id.contains("HDAUDIO\\", Qt::CaseInsensitive);
}

std::vector<AudioDeviceEntry> AudioDeviceManager::loadRenderDevices(bool includeVirtual) const
{
    // 返回带音量接口的完整设备对象，供控制页直接操作。
    std::vector<AudioDeviceEntry> results;

    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return results;
    }

    IMMDeviceCollection *collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, kActiveRenderDevices, &collection);
    if (FAILED(hr) || !collection) {
        qWarning() << "EnumAudioEndpoints failed:" << Qt::hex << hr;
        enumerator->Release();
        return results;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        qWarning() << "GetCount failed:" << Qt::hex << hr;
        collection->Release();
        enumerator->Release();
        return results;
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        AudioDeviceEntry entry;
        entry.name = readDeviceName(device);
        entry.id = readDeviceId(device);
        entry.isVirtual = isVirtualDevice(device);

        if (entry.name.isEmpty() || entry.id.isEmpty()) {
            device->Release();
            continue;
        }

        if (!includeVirtual && entry.isVirtual) {
            device->Release();
            continue;
        }

        IAudioEndpointVolume *volume = nullptr;
        hr = device->Activate(__uuidof(IAudioEndpointVolume),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void **>(&volume));
        if (FAILED(hr) || !volume) {
            device->Release();
            continue;
        }

        entry.device = device;
        entry.volume = volume;
        results.push_back(entry);
    }

    collection->Release();
    enumerator->Release();
    return results;
}

std::vector<AudioDeviceSnapshot> AudioDeviceManager::loadAllRenderDeviceSnapshots() const
{
    // 设置页只需要展示和勾选，不需要长期持有音量接口。
    std::vector<AudioDeviceSnapshot> results;

    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return results;
    }

    IMMDeviceCollection *collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, kActiveRenderDevices, &collection);
    if (FAILED(hr) || !collection) {
        qWarning() << "EnumAudioEndpoints failed:" << Qt::hex << hr;
        enumerator->Release();
        return results;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        qWarning() << "GetCount failed:" << Qt::hex << hr;
        collection->Release();
        enumerator->Release();
        return results;
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        AudioDeviceSnapshot snapshot;
        snapshot.name = readDeviceName(device);
        snapshot.id = readDeviceId(device);
        snapshot.isVirtual = isVirtualDevice(device);

        if (!snapshot.name.isEmpty() && !snapshot.id.isEmpty()) {
            results.push_back(snapshot);
        }

        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return results;
}

std::vector<AudioDeviceSnapshot> AudioDeviceManager::loadAllCaptureDeviceSnapshots() const
{
    // 枚举所有活动输入设备，供缓存方向使用。
    std::vector<AudioDeviceSnapshot> results;

    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return results;
    }

    IMMDeviceCollection *collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return results;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        collection->Release();
        enumerator->Release();
        return results;
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        AudioDeviceSnapshot snapshot;
        snapshot.name = readDeviceName(device);
        snapshot.id = readDeviceId(device);
        snapshot.isVirtual = isVirtualDevice(device);

        if (!snapshot.id.isEmpty()) {
            results.push_back(snapshot);
        }

        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return results;
}

void AudioDeviceManager::releaseDevices(std::vector<AudioDeviceEntry> &devices) const
{
    // 所有在 loadRenderDevices 中申请的 COM 资源都在这里配对释放。
    for (auto &device : devices) {
        if (device.volume) {
            device.volume->Release();
        }
        if (device.device) {
            device.device->Release();
        }
    }
    devices.clear();
}

bool AudioDeviceManager::getDefaultRenderEndpoint(IAudioEndpointVolume **volume, QString *deviceId) const
{
    // 同步模式始终围绕 Windows 默认播放设备工作。
    if (!volume) {
        return false;
    }

    *volume = nullptr;

    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return false;
    }

    IMMDevice *device = nullptr;
    const HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        qWarning() << "GetDefaultAudioEndpoint failed:" << Qt::hex << hr;
        return false;
    }

    if (deviceId) {
        *deviceId = readDeviceId(device);
    }

    const HRESULT activateHr = device->Activate(__uuidof(IAudioEndpointVolume),
                                                CLSCTX_ALL,
                                                nullptr,
                                                reinterpret_cast<void **>(volume));
    device->Release();
    if (FAILED(activateHr) || !*volume) {
        qWarning() << "Activate default endpoint volume failed:" << Qt::hex << activateHr;
        return false;
    }

    return true;
}

bool AudioDeviceManager::getDefaultRenderVolume(float *volumeScalar) const
{
    // 用默认设备音量初始化或刷新控制页滑条。
    if (!volumeScalar) {
        return false;
    }

    IAudioEndpointVolume *volume = nullptr;
    if (!getDefaultRenderEndpoint(&volume, nullptr)) {
        return false;
    }

    const bool ok = readDeviceVolume(volume, volumeScalar);
    volume->Release();
    return ok;
}

bool AudioDeviceManager::readDeviceVolume(IAudioEndpointVolume *volume, float *volumeScalar) const
{
    // 统一封装读取音量，减少窗口层直接碰 HRESULT。
    if (!volume || !volumeScalar) {
        return false;
    }

    const HRESULT hr = volume->GetMasterVolumeLevelScalar(volumeScalar);
    if (FAILED(hr)) {
        qWarning() << "GetMasterVolumeLevelScalar failed:" << Qt::hex << hr;
        return false;
    }

    return true;
}

bool AudioDeviceManager::setDeviceVolume(IAudioEndpointVolume *volume, float volumeScalar, const GUID *eventContext) const
{
    // 对已有音量接口直接写入，适合控制页当前设备或默认设备。
    if (!volume) {
        return false;
    }

    const HRESULT hr = volume->SetMasterVolumeLevelScalar(volumeScalar, eventContext);
    if (FAILED(hr)) {
        qWarning() << "SetMasterVolumeLevelScalar failed:" << Qt::hex << hr;
        return false;
    }

    return true;
}

bool AudioDeviceManager::setDeviceVolumeById(const QString &deviceId, float volumeScalar, const GUID *eventContext) const
{
    // 设置页同步设备可能不在控制页缓存里，所以按 ID 重新定位。
    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return false;
    }

    IMMDevice *device = nullptr;
    const std::wstring wideId = deviceId.toStdWString();
    HRESULT hr = enumerator->GetDevice(wideId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        qWarning() << "GetDevice failed for synced device:" << deviceId << Qt::hex << hr;
        return false;
    }

    IAudioEndpointVolume *volume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume),
                          CLSCTX_ALL,
                          nullptr,
                          reinterpret_cast<void **>(&volume));
    device->Release();
    if (FAILED(hr) || !volume) {
        qWarning() << "Activate failed for synced device:" << deviceId << Qt::hex << hr;
        return false;
    }

    const bool ok = setDeviceVolume(volume, volumeScalar, eventContext);
    volume->Release();
    return ok;
}

QString AudioDeviceManager::readDeviceNameById(const QString &deviceId) const
{
    // 设备通知里通常只给 ID，这里按 ID 反查友好名称，方便日志直接定位到具体设备。
    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return {};
    }

    IMMDevice *device = nullptr;
    const std::wstring wideId = deviceId.toStdWString();
    const HRESULT hr = enumerator->GetDevice(wideId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return {};
    }

    const QString name = readDeviceName(device);
    device->Release();
    return name;
}

EDataFlow AudioDeviceManager::getDeviceDataFlow(IMMDevice *device) const
{
    // 通过 IMMEndpoint 接口查询设备数据流方向。
    if (!device) {
        return eRender;
    }

    IMMEndpoint *endpoint = nullptr;
    const HRESULT endpointHr = device->QueryInterface(__uuidof(IMMEndpoint),
                                                       reinterpret_cast<void **>(&endpoint));
    if (FAILED(endpointHr) || !endpoint) {
        return eRender;
    }

    EDataFlow dataFlow = eRender;
    const HRESULT flowHr = endpoint->GetDataFlow(&dataFlow);
    endpoint->Release();
    if (FAILED(flowHr)) {
        return eRender;
    }

    return dataFlow;
}

EDataFlow AudioDeviceManager::getDeviceDataFlow(const QString &deviceId) const
{
    // 通过设备 ID 查询真实数据流方向，比字符串匹配可靠。
    IMMDeviceEnumerator *enumerator = createEnumerator();
    if (!enumerator) {
        return eRender;
    }

    IMMDevice *device = nullptr;
    const std::wstring wideId = deviceId.toStdWString();
    const HRESULT hr = enumerator->GetDevice(wideId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return eRender;
    }

    const EDataFlow flow = getDeviceDataFlow(device);
    device->Release();
    return flow;
}
