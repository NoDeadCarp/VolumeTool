#include "MainWindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGroupBox>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace {

constexpr const wchar_t *kAutoStartRegistryPath = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const GUID kVolumeSyncContext = {
    0x0fdd79ca, 0x95d0, 0x47a5, {0x97, 0x74, 0xf8, 0x41, 0x82, 0x37, 0x33, 0x9c}
};

}

VolumeCallback::VolumeCallback(MainWindow *owner)
    : owner(owner)
{
}

HRESULT STDMETHODCALLTYPE VolumeCallback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notifyData)
{
    if (!owner || !notifyData) {
        return E_POINTER;
    }

    if (IsEqualGUID(notifyData->guidEventContext, kVolumeSyncContext)) {
        return S_OK;
    }

    const float volumeScalar = notifyData->fMasterVolume;
    QMetaObject::invokeMethod(owner, [this, volumeScalar]() {
        if (owner) {
            owner->handleExternalVolumeChange(volumeScalar);
        }
    }, Qt::QueuedConnection);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE VolumeCallback::QueryInterface(REFIID iid, VOID **object)
{
    if (!object) {
        return E_POINTER;
    }

    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
        *object = static_cast<IAudioEndpointVolumeCallback *>(this);
        AddRef();
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE VolumeCallback::AddRef()
{
    return ++refCount;
}

ULONG STDMETHODCALLTYPE VolumeCallback::Release()
{
    const ULONG count = --refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      tabWidget(new QTabWidget(this)),
      controlPage(new QWidget(this)),
      settingsPage(new QWidget(this)),
      deviceBox(new QComboBox(this)),
      showVirtualCheck(new QCheckBox(QStringLiteral("显示虚拟设备"), this)),
      slider(new QSlider(Qt::Horizontal, this)),
      label(new QLabel(QStringLiteral("音量控制"), this)),
      autoStartCheck(new QCheckBox(QStringLiteral("开机自启"), this)),
      restartAudioEngineCheck(new QCheckBox(QStringLiteral("设备变动时重启 audio engine"), this)),
      syncWindowsVolumeCheck(new QCheckBox(QStringLiteral("同步 Windows 音量滑轨"), this)),
      syncDeviceHintLabel(new QLabel(QStringLiteral("勾选后可选择需要跟随软件一起同步的设备。"), this)),
      syncDeviceList(new QListWidget(this))
{
    setupUi();
    initAudio();
    loadSettings();

    connect(deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDeviceChanged);
    connect(slider, &QSlider::valueChanged,
            this, &MainWindow::onSliderChanged);
    connect(showVirtualCheck, &QCheckBox::toggled,
            this, &MainWindow::loadDevices);
    connect(syncWindowsVolumeCheck, &QCheckBox::toggled,
            this, &MainWindow::onSyncWindowsVolumeToggled);
    connect(autoStartCheck, &QCheckBox::toggled,
            this, &MainWindow::onAutoStartToggled);

    loadDevices(showVirtualCheck->isChecked());
}

MainWindow::~MainWindow()
{
    unregisterVolumeCallback();
    clearDevices();

    if (comInitialized) {
        CoUninitialize();
    }
}

void MainWindow::setupUi()
{
    auto *controlLayout = new QVBoxLayout(controlPage);
    slider->setRange(0, 100);

    controlLayout->addWidget(showVirtualCheck);
    controlLayout->addWidget(deviceBox);
    controlLayout->addWidget(label);
    controlLayout->addWidget(slider);
    controlLayout->addStretch();

    auto *settingsLayout = new QVBoxLayout(settingsPage);
    auto *behaviorGroup = new QGroupBox(QStringLiteral("行为设置"), settingsPage);
    auto *behaviorLayout = new QVBoxLayout(behaviorGroup);

    behaviorLayout->addWidget(autoStartCheck);
    behaviorLayout->addWidget(restartAudioEngineCheck);
    behaviorLayout->addWidget(syncWindowsVolumeCheck);

    auto *syncGroup = new QGroupBox(QStringLiteral("同步设备"), settingsPage);
    auto *syncLayout = new QVBoxLayout(syncGroup);

    syncDeviceHintLabel->setWordWrap(true);
    syncLayout->addWidget(syncDeviceHintLabel);
    syncLayout->addWidget(syncDeviceList);

    settingsLayout->addWidget(behaviorGroup);
    settingsLayout->addWidget(syncGroup);
    settingsLayout->addStretch();

    tabWidget->addTab(controlPage, QStringLiteral("控制"));
    tabWidget->addTab(settingsPage, QStringLiteral("设置"));
    setCentralWidget(tabWidget);

    syncDeviceList->setEnabled(false);
    syncDeviceHintLabel->setEnabled(false);
}

void MainWindow::initAudio()
{
    // Core Audio 基于 COM，先初始化当前线程环境。
    const HRESULT hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr)) {
        comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        qWarning() << "CoInitialize failed:" << Qt::hex << hr;
    }
}

void MainWindow::loadSettings()
{
    QSettings runSettings(QString::fromWCharArray(kAutoStartRegistryPath), QSettings::NativeFormat);
    const QString key = applicationDisplayName();

    QSignalBlocker blocker(autoStartCheck);
    autoStartCheck->setChecked(runSettings.contains(key));
}

QString MainWindow::applicationDisplayName() const
{
    const QString explicitName = QCoreApplication::applicationName().trimmed();
    if (!explicitName.isEmpty()) {
        return explicitName;
    }

    return QStringLiteral("VolumeTool");
}

void MainWindow::updateAutoStart(bool enabled)
{
    QSettings runSettings(QString::fromWCharArray(kAutoStartRegistryPath), QSettings::NativeFormat);
    const QString key = applicationDisplayName();

    if (enabled) {
        const QString exePath = QCoreApplication::applicationFilePath().replace('/', '\\');
        runSettings.setValue(key, QStringLiteral("\"%1\"").arg(exePath));
    } else {
        runSettings.remove(key);
    }
}

void MainWindow::clearDevices()
{
    // 刷新设备列表前，先释放旧的 COM 接口，避免句柄泄漏。
    for (auto &d : devices) {
        if (d.volume) {
            d.volume->Release();
        }
        if (d.device) {
            d.device->Release();
        }
    }
    devices.clear();
}

void MainWindow::refreshDevices(bool showVirtual)
{
    unregisterVolumeCallback();
    clearDevices();

    // 清空下拉框时临时屏蔽信号，避免触发无效的设备切换。
    deviceBox->blockSignals(true);
    deviceBox->clear();
    deviceBox->blockSignals(false);

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        qWarning() << "CoCreateInstance failed:" << Qt::hex << hr;
        updateSyncDeviceList();
        return;
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        qWarning() << "EnumAudioEndpoints failed:" << Qt::hex << hr;
        enumerator->Release();
        updateSyncDeviceList();
        return;
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        qWarning() << "GetCount failed:" << Qt::hex << hr;
        collection->Release();
        enumerator->Release();
        updateSyncDeviceList();
        return;
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        // 根据名称和实例 ID 过滤常见虚拟输出设备。
        const bool virtualDevice = isVirtualDevice(device);
        if (!showVirtual && virtualDevice) {
            device->Release();
            continue;
        }

        IPropertyStore *props = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &props);
        if (FAILED(hr) || !props) {
            device->Release();
            continue;
        }

        PROPVARIANT varName;
        PROPVARIANT varId;
        PropVariantInit(&varName);
        PropVariantInit(&varId);

        const HRESULT nameHr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        const HRESULT idHr = props->GetValue(PKEY_Device_InstanceId, &varId);
        if (FAILED(nameHr) || FAILED(idHr) || varName.vt != VT_LPWSTR || !varName.pwszVal
            || varId.vt != VT_LPWSTR || !varId.pwszVal) {
            PropVariantClear(&varName);
            PropVariantClear(&varId);
            props->Release();
            device->Release();
            continue;
        }

        IAudioEndpointVolume *volume = nullptr;
        hr = device->Activate(__uuidof(IAudioEndpointVolume),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void **>(&volume));
        if (FAILED(hr) || !volume) {
            PropVariantClear(&varName);
            PropVariantClear(&varId);
            props->Release();
            device->Release();
            continue;
        }

        DeviceItem item;
        item.id = QString::fromWCharArray(varId.pwszVal);
        item.name = QString::fromWCharArray(varName.pwszVal);
        item.device = device;
        item.volume = volume;

        devices.push_back(item);
        deviceBox->addItem(item.name);

        PropVariantClear(&varName);
        PropVariantClear(&varId);
        props->Release();
    }

    collection->Release();
    enumerator->Release();
    updateSyncDeviceList();
}

void MainWindow::updateSyncDeviceList()
{
    const QStringList previouslyChecked = [&]() {
        QStringList checkedIds;
        for (int i = 0; i < syncDeviceList->count(); ++i) {
            QListWidgetItem *item = syncDeviceList->item(i);
            if (item && item->checkState() == Qt::Checked) {
                checkedIds.append(item->data(Qt::UserRole).toString());
            }
        }
        return checkedIds;
    }();

    syncDeviceList->clear();

    for (const auto &device : devices) {
        auto *item = new QListWidgetItem(device.name, syncDeviceList);
        item->setData(Qt::UserRole, device.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(previouslyChecked.contains(device.id) ? Qt::Checked : Qt::Unchecked);
    }

    if (devices.empty()) {
        auto *item = new QListWidgetItem(QStringLiteral("当前没有可供选择的输出设备"), syncDeviceList);
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
}

void MainWindow::registerVolumeCallbackForCurrentDevice()
{
    unregisterVolumeCallback();

    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    const int index = deviceBox->currentIndex();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        return;
    }

    volumeCallback = new VolumeCallback(this);
    callbackVolume = devices[index].volume;
    callbackVolume->AddRef();

    const HRESULT hr = callbackVolume->RegisterControlChangeNotify(volumeCallback);
    if (FAILED(hr)) {
        qWarning() << "RegisterControlChangeNotify failed:" << Qt::hex << hr;
        callbackVolume->Release();
        callbackVolume = nullptr;
        volumeCallback->Release();
        volumeCallback = nullptr;
    }
}

void MainWindow::unregisterVolumeCallback()
{
    if (callbackVolume && volumeCallback) {
        callbackVolume->UnregisterControlChangeNotify(volumeCallback);
    }

    if (callbackVolume) {
        callbackVolume->Release();
        callbackVolume = nullptr;
    }

    if (volumeCallback) {
        volumeCallback->Release();
        volumeCallback = nullptr;
    }
}

void MainWindow::applyVolumeToSelectedDevices(float volumeScalar)
{
    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    for (int i = 0; i < syncDeviceList->count(); ++i) {
        QListWidgetItem *item = syncDeviceList->item(i);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }

        const QString selectedId = item->data(Qt::UserRole).toString();
        for (auto &device : devices) {
            if (device.id != selectedId || !device.volume) {
                continue;
            }

            const HRESULT hr = device.volume->SetMasterVolumeLevelScalar(volumeScalar, &kVolumeSyncContext);
            if (FAILED(hr)) {
                qWarning() << "SetMasterVolumeLevelScalar failed for synced device:" << device.name << Qt::hex << hr;
            }
            break;
        }
    }
}

void MainWindow::handleExternalVolumeChange(float volumeScalar)
{
    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    internalVolumeChange = true;
    slider->setValue(qRound(volumeScalar * 100.0f));
    internalVolumeChange = false;

    applyVolumeToSelectedDevices(volumeScalar);
}

void MainWindow::onDeviceChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        slider->blockSignals(true);
        slider->setValue(0);
        slider->blockSignals(false);
        slider->setEnabled(false);
        unregisterVolumeCallback();
        return;
    }

    // 切换设备时，把系统当前音量同步到滑条位置。
    float volumeScalar = 0.0f;
    const HRESULT hr = devices[index].volume->GetMasterVolumeLevelScalar(&volumeScalar);
    if (FAILED(hr)) {
        qWarning() << "GetMasterVolumeLevelScalar failed:" << Qt::hex << hr;
        unregisterVolumeCallback();
        return;
    }

    slider->setEnabled(true);
    slider->blockSignals(true);
    slider->setValue(qRound(volumeScalar * 100.0f));
    slider->blockSignals(false);

    registerVolumeCallbackForCurrentDevice();
}

void MainWindow::onSliderChanged(int value)
{
    const int index = deviceBox->currentIndex();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        return;
    }

    // Qt 的 0-100 范围转换成 Core Audio 的 0.0-1.0 标量。
    const float volumeScalar = static_cast<float>(value) / 100.0f;
    const GUID *eventContext = syncWindowsVolumeCheck->isChecked() ? &kVolumeSyncContext : nullptr;

    const HRESULT hr = devices[index].volume->SetMasterVolumeLevelScalar(volumeScalar, eventContext);
    if (FAILED(hr)) {
        qWarning() << "SetMasterVolumeLevelScalar failed:" << Qt::hex << hr;
    }

    if (!internalVolumeChange) {
        applyVolumeToSelectedDevices(volumeScalar);
    }
}

void MainWindow::loadDevices(bool showVirtual)
{
    refreshDevices(showVirtual);

    if (devices.empty()) {
        onDeviceChanged(-1);
        return;
    }

    // 设备刷新后默认选中第一项，并立即同步它的音量。
    deviceBox->setCurrentIndex(0);
    onDeviceChanged(0);
}

void MainWindow::onSyncWindowsVolumeToggled(bool checked)
{
    syncDeviceHintLabel->setEnabled(checked);
    syncDeviceList->setEnabled(checked);
    registerVolumeCallbackForCurrentDevice();
}

void MainWindow::onAutoStartToggled(bool checked)
{
    updateAutoStart(checked);
}

bool MainWindow::isVirtualDevice(IMMDevice *device)
{
    IPropertyStore *props = nullptr;
    const HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props) {
        return false;
    }

    PROPVARIANT varName;
    PROPVARIANT varId;
    PropVariantInit(&varName);
    PropVariantInit(&varId);

    const HRESULT nameHr = props->GetValue(PKEY_Device_FriendlyName, &varName);
    const HRESULT idHr = props->GetValue(PKEY_Device_InstanceId, &varId);

    const QString name = (SUCCEEDED(nameHr) && varName.vt == VT_LPWSTR && varName.pwszVal)
        ? QString::fromWCharArray(varName.pwszVal)
        : QString();
    const QString id = (SUCCEEDED(idHr) && varId.vt == VT_LPWSTR && varId.pwszVal)
        ? QString::fromWCharArray(varId.pwszVal)
        : QString();

    props->Release();
    PropVariantClear(&varName);
    PropVariantClear(&varId);

    return name.contains("Virtual", Qt::CaseInsensitive)
        || name.contains("VB-Audio", Qt::CaseInsensitive)
        || name.contains("Voicemeeter", Qt::CaseInsensitive)
        || id.startsWith("ROOT\\")
        || id.contains("SWD\\");
}
