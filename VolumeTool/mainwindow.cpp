#include "MainWindow.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <thread>

namespace {

constexpr const wchar_t *kAutoStartRegistryPath = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const GUID kVolumeSyncContext = {
    0x0fdd79ca, 0x95d0, 0x47a5, {0x97, 0x74, 0xf8, 0x41, 0x82, 0x37, 0x33, 0x9c}
};
constexpr qint64 kDeviceLogDedupWindowMs = 3000;
constexpr qint64 kVoicemeeterRestartWindowMs = 8000;

QString deviceStateToText(DWORD state)
{
    switch (state) {
    case DEVICE_STATE_ACTIVE:
        return QStringLiteral("已启用");
    case DEVICE_STATE_DISABLED:
        return QStringLiteral("已禁用");
    case DEVICE_STATE_NOTPRESENT:
        return QStringLiteral("未连接");
    case DEVICE_STATE_UNPLUGGED:
        return QStringLiteral("已断开");
    default:
        return QStringLiteral("未知状态");
    }
}

QString deviceEventId(LPCWSTR rawDeviceId)
{
    if (!rawDeviceId) {
        return QStringLiteral("unknown");
    }
    return QString::fromWCharArray(rawDeviceId);
}

QString deviceDisplayText(MainWindow *owner, LPCWSTR rawDeviceId)
{
    if (!owner || !rawDeviceId) {
        return QStringLiteral("未知设备");
    }
    return owner->describeDeviceForLog(QString::fromWCharArray(rawDeviceId));
}

}

VolumeCallback::VolumeCallback(MainWindow *owner)
    : owner(owner)
{
}

HRESULT STDMETHODCALLTYPE VolumeCallback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notifyData)
{
    // Windows 默认设备音量变化后，把结果回传到主界面。
    if (!owner || !notifyData) {
        return E_POINTER;
    }

    // 忽略软件自己触发的回调，避免循环联动。
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

DeviceNotificationCallback::DeviceNotificationCallback(MainWindow *owner)
    : owner(owner)
{
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR deviceId)
{
    Q_UNUSED(role)
    if (flow == eRender || flow == eCapture || flow == eAll) {
        if (owner) {
            const QString directionLabel = (flow == eCapture)
                ? QStringLiteral("默认输入设备已变动")
                : (flow == eAll)
                    ? QStringLiteral("默认输入输出设备已变动")
                    : QStringLiteral("默认输出设备已变动");
            const QString deviceText = deviceDisplayText(owner, deviceId);
            owner->appendDeviceEventLog(QStringLiteral("default-device:%1").arg(deviceEventId(deviceId)),
                                        QStringLiteral("%1：%2").arg(directionLabel, deviceText));
        }

        // 默认设备变化时无法确定连接/断开，使用 "connect" 作为默认动作。
        notifyDeviceChange(deviceId, QStringLiteral("connect"));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceAdded(LPCWSTR deviceId)
{
    if (!isVirtualDeviceId(deviceId)) {
        if (owner) {
            const QString deviceText = deviceDisplayText(owner, deviceId);
            owner->appendDeviceEventLog(QStringLiteral("device-added:%1").arg(deviceEventId(deviceId)),
                                        QStringLiteral("检测到新设备加入：%1").arg(deviceText));
        }
    }
    notifyDeviceChange(deviceId, QStringLiteral("connect"));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceRemoved(LPCWSTR deviceId)
{
    if (!isVirtualDeviceId(deviceId)) {
        if (owner) {
            const QString deviceText = deviceDisplayText(owner, deviceId);
            owner->appendDeviceEventLog(QStringLiteral("device-removed:%1").arg(deviceEventId(deviceId)),
                                        QStringLiteral("检测到设备被移除：%1").arg(deviceText));
        }
    }
    notifyDeviceChange(deviceId, QStringLiteral("disconnect"));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState)
{
    // 判断设备是连接还是断开：ACTIVE 视为连接，其他状态视为断开。
    const QString action = (newState == DEVICE_STATE_ACTIVE)
        ? QStringLiteral("connect")
        : QStringLiteral("disconnect");

    if (!isVirtualDeviceId(deviceId)) {
        if (owner) {
            const QString deviceText = deviceDisplayText(owner, deviceId);
            owner->appendDeviceEventLog(QStringLiteral("device-state:%1").arg(deviceEventId(deviceId)),
                                        QStringLiteral("%1 状态变为：%2").arg(deviceText, deviceStateToText(newState)));
        }
    }
    notifyDeviceChange(deviceId, action);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key)
{
    Q_UNUSED(key)
    // 属性变化无法确定连接/断开，不触发重启。
    notifyDeviceChange(deviceId, QString());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::QueryInterface(REFIID iid, VOID **object)
{
    if (!object) {
        return E_POINTER;
    }

    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient *>(this);
        AddRef();
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeviceNotificationCallback::AddRef()
{
    return ++refCount;
}

ULONG STDMETHODCALLTYPE DeviceNotificationCallback::Release()
{
    const ULONG count = --refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

bool DeviceNotificationCallback::isVirtualDeviceId(LPCWSTR pwstrDeviceId) const
{
    if (!owner || !pwstrDeviceId) {
        return false;
    }

    const QString deviceId = QString::fromWCharArray(pwstrDeviceId);

    // 先通过设备 ID 直接判断（不需要设备句柄，不会因设备断开而失败）。
    if (deviceId.startsWith("ROOT\\", Qt::CaseInsensitive)
        || deviceId.contains("SWD\\", Qt::CaseInsensitive)
        || deviceId.contains("HDAUDIO\\", Qt::CaseInsensitive)) {
        return true;
    }

    // 再检查缓存。
    const QString cached = owner->cachedDeviceDirection(deviceId);
    if (cached == QStringLiteral("virtual")) {
        return true;
    }

    // 最后尝试实时查询设备名称。
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return false;
    }

    IMMDevice *device = nullptr;
    const std::wstring wideId = deviceId.toStdWString();
    hr = enumerator->GetDevice(wideId.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return false;
    }

    const bool isVirtual = owner->audioDeviceMgr().isVirtualDevice(device);
    device->Release();
    return isVirtual;
}

void DeviceNotificationCallback::scheduleRefresh()
{
    if (!owner) {
        return;
    }

    QMetaObject::invokeMethod(owner, [this]() {
        if (owner) {
            owner->handleDeviceListChanged();
        }
    }, Qt::QueuedConnection);
}

void DeviceNotificationCallback::notifyDeviceChange(LPCWSTR pwstrDeviceId, const QString &action)
{
    // 在 COM 回调线程中直接查询设备方向，避免延迟到 UI 线程后设备已消失导致查询失败。
    const QString deviceId = pwstrDeviceId ? QString::fromWCharArray(pwstrDeviceId) : QString();
    QString direction;

    if (!deviceId.isEmpty()) {
        // 先尝试实时查询设备方向。
        IMMDeviceEnumerator *enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (SUCCEEDED(hr) && enumerator) {
            IMMDevice *device = nullptr;
            const std::wstring wideId = deviceId.toStdWString();
            hr = enumerator->GetDevice(wideId.c_str(), &device);
            enumerator->Release();
            if (SUCCEEDED(hr) && device) {
                // 跳过虚拟设备（Voicemeeter 等），它们的变动只是物理设备变化的联动结果。
                if (owner && owner->audioDeviceMgr().isVirtualDevice(device)) {
                    device->Release();
                    scheduleRefresh();
                    return;
                }

                const EDataFlow dataFlow = owner->audioDeviceMgr().getDeviceDataFlow(device);
                device->Release();
                direction = (dataFlow == eCapture) ? QStringLiteral("capture")
                          : (dataFlow == eAll) ? QStringLiteral("all")
                          : QStringLiteral("render");
            }
        }

        // 设备已断开/移除时 GetDevice 会失败，使用缓存的方向信息。
        // 同时也跳过缓存中的虚拟设备。
        if (direction.isEmpty() && owner) {
            const QString cached = owner->cachedDeviceDirection(deviceId);
            if (cached == QStringLiteral("virtual")) {
                scheduleRefresh();
                return;
            }
            if (!cached.isEmpty()) {
                direction = cached;
            }
        }
    }

    if (direction.isEmpty()) {
        direction = QStringLiteral("render");
    }

    // action 为空时不触发重启（如属性变化事件），仅刷新设备列表。
    if (owner) {
        if (action.isEmpty()) {
            scheduleRefresh();
        } else {
            QMetaObject::invokeMethod(owner, [this, deviceId, direction, action]() {
                if (owner) {
                    owner->handleDeviceChangeForRestart(deviceId, direction, action);
                }
            }, Qt::QueuedConnection);
        }
    }

    scheduleRefresh();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      tabWidget(new QTabWidget(this)),
      controlPage(new QWidget(this)),
      settingsPage(new QWidget(this)),
      voicemeeterPage(new QWidget(this)),
      deviceBox(new QComboBox(this)),
      showVirtualCheck(new QCheckBox(QStringLiteral("显示虚拟设备"), this)),
      slider(new QSlider(Qt::Horizontal, this)),
      label(new QLabel(QStringLiteral("音量控制"), this)),
      syncModeHintLabel(new QLabel(QStringLiteral("已开启 Windows 音量同步，请到“设置”页的“同步设备”中勾选需要联动的设备。"), this)),
      autoStartCheck(new QCheckBox(QStringLiteral("开机自启"), this)),
      syncWindowsVolumeCheck(new QCheckBox(QStringLiteral("同步 Windows 音量滑轨"), this)),
      syncDeviceHintLabel(new QLabel(QStringLiteral("勾选后可选择需要跟随软件一起同步的设备。"), this)),
      syncDeviceList(new QListWidget(this)),
      restartAudioEngineOnRenderConnectCheck(new QCheckBox(QStringLiteral("输出设备连接时重启 audio engine"), this)),
      restartAudioEngineOnRenderDisconnectCheck(new QCheckBox(QStringLiteral("输出设备断开时重启 audio engine"), this)),
      restartAudioEngineOnCaptureConnectCheck(new QCheckBox(QStringLiteral("输入设备连接时重启 audio engine"), this)),
      restartAudioEngineOnCaptureDisconnectCheck(new QCheckBox(QStringLiteral("输入设备断开时重启 audio engine"), this)),
      audioEngineStatusLabel(new QLabel(this)),
      audioEnginePathLabel(new QLabel(this)),
      manualRestartAudioEngineButton(new QPushButton(QStringLiteral("手动重启 audio engine"), this)),
      deviceLogList(new QListWidget(this)),
      clearDeviceLogButton(new QPushButton(QStringLiteral("清除日志"), this)),
      deviceRefreshTimer(new QTimer(this)),
      voicemeeterRestartTimer(new QTimer(this))
{
    setupUi();
    initAudio();
    loadSettings();

    deviceRefreshTimer->setSingleShot(true);
    deviceRefreshTimer->setInterval(200);
    connect(deviceRefreshTimer, &QTimer::timeout, this, [this]() {
        loadDevices(showVirtualCheck->isChecked());
    });

    voicemeeterRestartTimer->setSingleShot(true);
    voicemeeterRestartTimer->setInterval(2000);
    connect(voicemeeterRestartTimer, &QTimer::timeout, this, [this]() {
        restartVoicemeeterAudioEngine(false);
    });

    connect(deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceChanged);
    connect(slider, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);
    connect(showVirtualCheck, &QCheckBox::toggled, this, &MainWindow::loadDevices);
    connect(syncWindowsVolumeCheck, &QCheckBox::toggled, this, &MainWindow::onSyncWindowsVolumeToggled);
    connect(autoStartCheck, &QCheckBox::toggled, this, &MainWindow::onAutoStartToggled);
    connect(manualRestartAudioEngineButton, &QPushButton::clicked, this, [this]() {
        restartVoicemeeterAudioEngine(true);
    });
    connect(clearDeviceLogButton, &QPushButton::clicked, this, [this]() {
        clearDeviceEventLogs();
    });

    registerDeviceNotifications();
    loadDevices(showVirtualCheck->isChecked());
}

MainWindow::~MainWindow()
{
    unregisterDeviceNotifications();
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

    syncModeHintLabel->setWordWrap(true);
    syncModeHintLabel->setStyleSheet(QStringLiteral("color: #b45309;"));
    syncModeHintLabel->hide();

    controlLayout->addWidget(showVirtualCheck);
    controlLayout->addWidget(deviceBox);
    controlLayout->addWidget(syncModeHintLabel);
    controlLayout->addWidget(label);
    controlLayout->addWidget(slider);
    controlLayout->addStretch();

    auto *settingsLayout = new QVBoxLayout(settingsPage);
    auto *behaviorGroup = new QGroupBox(QStringLiteral("行为设置"), settingsPage);
    auto *behaviorLayout = new QVBoxLayout(behaviorGroup);
    behaviorLayout->addWidget(autoStartCheck);
    behaviorLayout->addWidget(syncWindowsVolumeCheck);

    auto *syncGroup = new QGroupBox(QStringLiteral("同步设备"), settingsPage);
    auto *syncLayout = new QVBoxLayout(syncGroup);
    syncDeviceHintLabel->setWordWrap(true);
    syncLayout->addWidget(syncDeviceHintLabel);
    syncLayout->addWidget(syncDeviceList);

    settingsLayout->addWidget(behaviorGroup);
    settingsLayout->addWidget(syncGroup);
    settingsLayout->addStretch();

    auto *voicemeeterLayout = new QVBoxLayout(voicemeeterPage);
    auto *voicemeeterStatusGroup = new QGroupBox(QStringLiteral("Voicemeeter 状态"), voicemeeterPage);
    auto *voicemeeterStatusLayout = new QVBoxLayout(voicemeeterStatusGroup);
    voicemeeterStatusLayout->addWidget(audioEngineStatusLabel);
    voicemeeterStatusLayout->addWidget(audioEnginePathLabel);

    auto *voicemeeterActionGroup = new QGroupBox(QStringLiteral("Voicemeeter 功能"), voicemeeterPage);
    auto *voicemeeterActionLayout = new QVBoxLayout(voicemeeterActionGroup);
    voicemeeterActionLayout->addWidget(restartAudioEngineOnRenderConnectCheck);
    voicemeeterActionLayout->addWidget(restartAudioEngineOnRenderDisconnectCheck);
    voicemeeterActionLayout->addWidget(restartAudioEngineOnCaptureConnectCheck);
    voicemeeterActionLayout->addWidget(restartAudioEngineOnCaptureDisconnectCheck);
    voicemeeterActionLayout->addWidget(manualRestartAudioEngineButton);

    auto *deviceLogGroup = new QGroupBox(QStringLiteral("设备变动日志"), voicemeeterPage);
    auto *deviceLogLayout = new QVBoxLayout(deviceLogGroup);
    deviceLogList->setSelectionMode(QAbstractItemView::NoSelection);
    deviceLogLayout->addWidget(deviceLogList);
    deviceLogLayout->addWidget(clearDeviceLogButton);

    voicemeeterLayout->addWidget(voicemeeterStatusGroup);
    voicemeeterLayout->addWidget(voicemeeterActionGroup);
    voicemeeterLayout->addWidget(deviceLogGroup);
    voicemeeterLayout->addStretch();

    tabWidget->addTab(controlPage, QStringLiteral("控制"));
    tabWidget->addTab(settingsPage, QStringLiteral("设置"));
    tabWidget->addTab(voicemeeterPage, QStringLiteral("Voicemeeter"));
    setCentralWidget(tabWidget);

    syncDeviceList->setEnabled(false);
    syncDeviceHintLabel->setEnabled(false);
    updateAudioEngineOptionState();
    updateControlLockState();
}

void MainWindow::initAudio()
{
    const HRESULT hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr)) {
        comInitialized = true;
    }
}

void MainWindow::loadSettings()
{
    QSettings runSettings(QString::fromWCharArray(kAutoStartRegistryPath), QSettings::NativeFormat);
    QSignalBlocker blocker(autoStartCheck);
    autoStartCheck->setChecked(runSettings.contains(applicationDisplayName()));
    updateAudioEngineOptionState();
}

QString MainWindow::applicationDisplayName() const
{
    const QString explicitName = QCoreApplication::applicationName().trimmed();
    return explicitName.isEmpty() ? QStringLiteral("VolumeTool") : explicitName;
}

bool MainWindow::isVoicemeeterInstalled() const
{
    return !findVoicemeeterInstallPath().isEmpty();
}

bool MainWindow::isVoicemeeterRemoteApiAvailable(const QString &installPath) const
{
    return voicemeeterRemoteClient.isAvailable(installPath);
}

QString MainWindow::findVoicemeeterInstallPath() const
{
    auto normalizeDirectory = [](QString candidate) -> QString {
        candidate = candidate.trimmed().remove('"');
        if (candidate.isEmpty()) {
            return {};
        }

        QFileInfo info(candidate);
        if (info.isDir() && info.isAbsolute()) {
            return QDir::toNativeSeparators(info.absoluteFilePath());
        }

        if (info.exists() && info.isFile()) {
            return QDir::toNativeSeparators(info.absolutePath());
        }

        return {};
    };

    auto extractPathFromCommand = [&](const QString &command) -> QString {
        const QString trimmed = command.trimmed();
        if (trimmed.isEmpty()) {
            return {};
        }

        static const QRegularExpression quotedExe(QStringLiteral("^\"([^\"]+\\.exe)\""), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression plainExe(QStringLiteral("^([^\\s]+\\.exe)"), QRegularExpression::CaseInsensitiveOption);

        QRegularExpressionMatch match = quotedExe.match(trimmed);
        if (!match.hasMatch()) {
            match = plainExe.match(trimmed);
        }

        if (match.hasMatch()) {
            return normalizeDirectory(match.captured(1));
        }

        return {};
    };

    const QStringList uninstallRoots = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall")
    };

    for (const QString &root : uninstallRoots) {
        QSettings rootSettings(root, QSettings::NativeFormat);
        const QStringList groups = rootSettings.childGroups();
        for (const QString &group : groups) {
            rootSettings.beginGroup(group);
            const QString displayName = rootSettings.value(QStringLiteral("DisplayName")).toString();
            const QString installLocation = rootSettings.value(QStringLiteral("InstallLocation")).toString();
            const QString publisher = rootSettings.value(QStringLiteral("Publisher")).toString();
            const QString uninstallString = rootSettings.value(QStringLiteral("UninstallString")).toString();
            const QString displayIcon = rootSettings.value(QStringLiteral("DisplayIcon")).toString();
            rootSettings.endGroup();

            if (displayName.contains(QStringLiteral("Voicemeeter"), Qt::CaseInsensitive)
                || installLocation.contains(QStringLiteral("Voicemeeter"), Qt::CaseInsensitive)
                || publisher.contains(QStringLiteral("VB-Audio"), Qt::CaseInsensitive)) {
                const QString installDir = normalizeDirectory(installLocation);
                if (!installDir.isEmpty()) {
                    return installDir;
                }

                const QString iconDir = normalizeDirectory(displayIcon.split(',', Qt::SkipEmptyParts).value(0));
                if (!iconDir.isEmpty()) {
                    return iconDir;
                }

                const QString uninstallDir = extractPathFromCommand(uninstallString);
                if (!uninstallDir.isEmpty()) {
                    return uninstallDir;
                }
            }
        }
    }

    return {};
}

void MainWindow::updateAudioEngineOptionState()
{
    const QString installPath = findVoicemeeterInstallPath();
    const bool installed = !installPath.isEmpty();
    const bool remoteApiAvailable = installed && isVoicemeeterRemoteApiAvailable(installPath);

    restartAudioEngineOnRenderConnectCheck->setEnabled(remoteApiAvailable);
    restartAudioEngineOnRenderDisconnectCheck->setEnabled(remoteApiAvailable);
    restartAudioEngineOnCaptureConnectCheck->setEnabled(remoteApiAvailable);
    restartAudioEngineOnCaptureDisconnectCheck->setEnabled(remoteApiAvailable);
    manualRestartAudioEngineButton->setEnabled(remoteApiAvailable);
    audioEngineStatusLabel->setText(installed ? QStringLiteral("已安装") : QStringLiteral("未检测到安装"));
    audioEngineStatusLabel->setStyleSheet(installed ? QStringLiteral("color: #15803d;") : QStringLiteral("color: #dc2626;"));
    audioEnginePathLabel->setText(installed
        ? QStringLiteral("安装路径：%1").arg(installPath)
        : QStringLiteral("安装路径：未检测到安装路径"));
    audioEnginePathLabel->setWordWrap(true);
    audioEnginePathLabel->setStyleSheet(installed ? QStringLiteral("color: #374151;") : QStringLiteral("color: #6b7280;"));

    const QString commonToolTip = !installed
        ? QStringLiteral("未检测到 Voicemeeter，安装后才能使用这个选项。")
        : (remoteApiAvailable
            ? QStringLiteral("检测到 Remote API，设备变动后自动重启 audio engine。")
            : QStringLiteral("检测到 Voicemeeter，但未找到 Remote API DLL。"));
    restartAudioEngineOnRenderConnectCheck->setToolTip(commonToolTip);
    restartAudioEngineOnRenderDisconnectCheck->setToolTip(commonToolTip);
    restartAudioEngineOnCaptureConnectCheck->setToolTip(commonToolTip);
    restartAudioEngineOnCaptureDisconnectCheck->setToolTip(commonToolTip);

    if (!remoteApiAvailable) {
        QSignalBlocker renderConnectBlocker(restartAudioEngineOnRenderConnectCheck);
        QSignalBlocker renderDisconnectBlocker(restartAudioEngineOnRenderDisconnectCheck);
        QSignalBlocker captureConnectBlocker(restartAudioEngineOnCaptureConnectCheck);
        QSignalBlocker captureDisconnectBlocker(restartAudioEngineOnCaptureDisconnectCheck);
        restartAudioEngineOnRenderConnectCheck->setChecked(false);
        restartAudioEngineOnRenderDisconnectCheck->setChecked(false);
        restartAudioEngineOnCaptureConnectCheck->setChecked(false);
        restartAudioEngineOnCaptureDisconnectCheck->setChecked(false);
    }
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
    audioDeviceManager.releaseDevices(devices);
}

void MainWindow::refreshDevices(bool showVirtual)
{
    unregisterVolumeCallback();
    clearDevices();

    deviceBox->blockSignals(true);
    deviceBox->clear();
    deviceBox->blockSignals(false);

    devices = audioDeviceManager.loadRenderDevices(showVirtual);
    for (const auto &device : devices) {
        deviceBox->addItem(device.name);
    }

    // 刷新设备方向缓存：渲染设备标记为 render，录音设备标记为 capture，
    // 同时出现在两边的设备（如带麦克风的 USB 耳机）标记为 all，
    // 虚拟设备标记为 virtual（插拔物理设备时虚拟设备的联动变化应忽略）。
    deviceDirectionCache.clear();
    for (const auto &device : devices) {
        if (device.isVirtual) {
            deviceDirectionCache.insert(device.id, QStringLiteral("virtual"));
        } else {
            deviceDirectionCache.insert(device.id, QStringLiteral("render"));
        }
    }
    const auto captureSnapshots = audioDeviceManager.loadAllCaptureDeviceSnapshots();
    for (const auto &snapshot : captureSnapshots) {
        if (snapshot.isVirtual) {
            deviceDirectionCache.insert(snapshot.id, QStringLiteral("virtual"));
            continue;
        }
        auto it = deviceDirectionCache.find(snapshot.id);
        if (it != deviceDirectionCache.end() && it.value() == QStringLiteral("render")) {
            it.value() = QStringLiteral("all");
        } else {
            deviceDirectionCache.insert(snapshot.id, QStringLiteral("capture"));
        }
    }

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

    const auto snapshots = audioDeviceManager.loadAllRenderDeviceSnapshots();
    for (const auto &snapshot : snapshots) {
        auto *item = new QListWidgetItem(snapshot.name, syncDeviceList);
        item->setData(Qt::UserRole, snapshot.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(previouslyChecked.contains(snapshot.id) ? Qt::Checked : Qt::Unchecked);
    }

    if (snapshots.empty()) {
        auto *item = new QListWidgetItem(QStringLiteral("当前没有可供选择的输出设备"), syncDeviceList);
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
}

void MainWindow::updateControlLockState()
{
    const bool syncEnabled = syncWindowsVolumeCheck->isChecked();
    deviceBox->setEnabled(!syncEnabled);
    showVirtualCheck->setEnabled(!syncEnabled);
    syncModeHintLabel->setVisible(syncEnabled);
}

void MainWindow::registerVolumeCallbackForCurrentDevice()
{
    unregisterVolumeCallback();

    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    IAudioEndpointVolume *defaultVolume = nullptr;
    if (!audioDeviceManager.getDefaultRenderEndpoint(&defaultVolume, nullptr)) {
        return;
    }

    volumeCallback = new VolumeCallback(this);
    callbackVolume = defaultVolume;

    const HRESULT hr = callbackVolume->RegisterControlChangeNotify(volumeCallback);
    if (FAILED(hr)) {
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

void MainWindow::registerDeviceNotifications()
{
    unregisterDeviceNotifications();

    notificationEnumerator = audioDeviceManager.createNotificationEnumerator();
    if (!notificationEnumerator) {
        return;
    }

    deviceNotificationCallback = new DeviceNotificationCallback(this);
    const HRESULT hr = notificationEnumerator->RegisterEndpointNotificationCallback(deviceNotificationCallback);
    if (FAILED(hr)) {
        deviceNotificationCallback->Release();
        deviceNotificationCallback = nullptr;
        notificationEnumerator->Release();
        notificationEnumerator = nullptr;
    }
}

void MainWindow::unregisterDeviceNotifications()
{
    if (notificationEnumerator && deviceNotificationCallback) {
        notificationEnumerator->UnregisterEndpointNotificationCallback(deviceNotificationCallback);
    }

    if (deviceNotificationCallback) {
        deviceNotificationCallback->Release();
        deviceNotificationCallback = nullptr;
    }

    if (notificationEnumerator) {
        notificationEnumerator->Release();
        notificationEnumerator = nullptr;
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
        if (!selectedId.isEmpty()) {
            audioDeviceManager.setDeviceVolumeById(selectedId, volumeScalar, &kVolumeSyncContext);
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

void MainWindow::appendDeviceEventLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    deviceEventLogs.append(QStringLiteral("[%1] %2").arg(timestamp, message));

    while (deviceEventLogs.size() > 100) {
        deviceEventLogs.removeFirst();
        // 裁剪首条后，所有索引需要前移一位；小于等于 0 的清除。
        QMap<QString, int> updatedIndices;
        for (auto it = deviceEventLogIndices.constBegin(); it != deviceEventLogIndices.constEnd(); ++it) {
            const int newIndex = it.value() - 1;
            if (newIndex >= 0) {
                updatedIndices.insert(it.key(), newIndex);
            }
        }
        deviceEventLogIndices = updatedIndices;
    }

    if (!deviceLogList) {
        return;
    }

    deviceLogList->clear();
    deviceLogList->addItems(deviceEventLogs);
    deviceLogList->scrollToBottom();
}

void MainWindow::appendDeviceEventLog(const QString &eventKey, const QString &message)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastTimestamp = deviceEventLogTimestamps.value(eventKey, 0);

    // 系统会在短时间内重复抛出同类事件，这里按时间窗口去重。
    // 但对于设备状态变化（如蓝牙耳机连接），短时间内可能先触发"已断开"再触发"已启用"，
    // 需要更新为最新状态，而非丢弃后续事件。
    if (lastTimestamp > 0 && now - lastTimestamp < kDeviceLogDedupWindowMs) {
        // 更新已有日志条目为最新消息。
        const int logIndex = deviceEventLogIndices.value(eventKey, -1);
        if (logIndex >= 0 && logIndex < deviceEventLogs.size()) {
            const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            const QString newEntry = QStringLiteral("[%1] %2").arg(timestamp, message);
            deviceEventLogs[logIndex] = newEntry;
            if (deviceLogList && logIndex < deviceLogList->count()) {
                deviceLogList->item(logIndex)->setText(newEntry);
            }
        }
        return;
    }

    deviceEventLogTimestamps.insert(eventKey, now);
    // 记录该 eventKey 对应的日志索引，供后续更新使用。
    deviceEventLogIndices.insert(eventKey, deviceEventLogs.size());
    appendDeviceEventLog(message);
}

QString MainWindow::describeDeviceForLog(const QString &deviceId) const
{
    const QString name = audioDeviceManager.readDeviceNameById(deviceId).trimmed();
    if (!name.isEmpty()) {
        return name;
    }

    if (deviceId.isEmpty()) {
        return QStringLiteral("未知设备");
    }

    const int tailLength = 24;
    const QString shortId = deviceId.size() > tailLength
        ? QStringLiteral("...%1").arg(deviceId.right(tailLength))
        : deviceId;
    return QStringLiteral("未知设备（%1）").arg(shortId);
}

void MainWindow::clearDeviceEventLogs()
{
    deviceEventLogs.clear();
    deviceEventLogTimestamps.clear();
    deviceEventLogIndices.clear();
    lastVoicemeeterRestartPerAction.clear();

    if (deviceLogList) {
        deviceLogList->clear();
    }
}

void MainWindow::handleDeviceListChanged()
{
    if (deviceRefreshTimer) {
        deviceRefreshTimer->start();
    }
}

void MainWindow::handleDeviceChangeForRestart(const QString &deviceId, const QString &direction, const QString &action)
{
    Q_UNUSED(deviceId)
    scheduleVoicemeeterRestart(direction, action);
}

QString MainWindow::cachedDeviceDirection(const QString &deviceId) const
{
    return deviceDirectionCache.value(deviceId, QString());
}

void MainWindow::scheduleVoicemeeterRestart(const QString &direction, const QString &action)
{
    // 根据设备方向和动作检查对应的 checkbox。
    const bool isAll = (direction == QStringLiteral("all"));

    // 查找对应方向+动作的 checkbox。
    auto checkForDirectionAction = [&](const QString &dir) -> QCheckBox * {
        if (dir == QStringLiteral("render")) {
            return (action == QStringLiteral("connect")) ? restartAudioEngineOnRenderConnectCheck
                 : (action == QStringLiteral("disconnect")) ? restartAudioEngineOnRenderDisconnectCheck
                 : nullptr;
        } else if (dir == QStringLiteral("capture")) {
            return (action == QStringLiteral("connect")) ? restartAudioEngineOnCaptureConnectCheck
                 : (action == QStringLiteral("disconnect")) ? restartAudioEngineOnCaptureDisconnectCheck
                 : nullptr;
        }
        return nullptr;
    };

    const bool shouldRestart = [&]() {
        if (isAll) {
            // 蓝牙耳机：render 和 capture 的对应 checkbox 任一勾选即触发。
            QCheckBox *renderCb = checkForDirectionAction(QStringLiteral("render"));
            QCheckBox *captureCb = checkForDirectionAction(QStringLiteral("capture"));
            return (renderCb && renderCb->isChecked()) || (captureCb && captureCb->isChecked());
        }
        QCheckBox *cb = checkForDirectionAction(direction);
        return cb && cb->isChecked();
    }();
    if (!shouldRestart) {
        return;
    }

    // 自动重启防抖：按 direction+action 维度，8 秒内同一类事件不重复调度。
    // 不同事件（如"输出设备断开"和"输出设备连接"）互不影响，允许快速拔插分别触发重启。
    const QString dedupKey = QStringLiteral("%1-%2").arg(direction, action);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastTime = lastVoicemeeterRestartPerAction.value(dedupKey, 0);
    if (lastTime > 0 && now - lastTime < kVoicemeeterRestartWindowMs) {
        return;
    }
    lastVoicemeeterRestartPerAction.insert(dedupKey, now);

    // 合并方向和动作：蓝牙耳机连接时 Windows 会先触发 render+connect 再触发 capture+connect，
    // 定时器延迟内合并为 all+connect，只重启一次。
    if (lastVoicemeeterRestartDirection.isEmpty()) {
        lastVoicemeeterRestartDirection = direction;
        lastVoicemeeterRestartAction = action;
    } else if (action == lastVoicemeeterRestartAction) {
        // 同一动作下合并方向：render + capture → all
        if (direction != lastVoicemeeterRestartDirection
            && direction != QStringLiteral("all")
            && lastVoicemeeterRestartDirection != QStringLiteral("all")) {
            if ((direction == QStringLiteral("render") && lastVoicemeeterRestartDirection == QStringLiteral("capture"))
                || (direction == QStringLiteral("capture") && lastVoicemeeterRestartDirection == QStringLiteral("render"))) {
                lastVoicemeeterRestartDirection = QStringLiteral("all");
            }
        }
    }

    if (voicemeeterRestartTimer) {
        voicemeeterRestartTimer->start();
    }
}

void MainWindow::restartVoicemeeterAudioEngine(bool isManual)
{
    const QString direction = lastVoicemeeterRestartDirection;
    const QString action = lastVoicemeeterRestartAction;

    const QString triggerSource = isManual
        ? QStringLiteral("手动")
        : QStringLiteral("%1%2").arg(
              direction == QStringLiteral("capture") ? QStringLiteral("输入设备")
              : direction == QStringLiteral("all") ? QStringLiteral("输入输出设备")
              : QStringLiteral("输出设备"),
              action == QStringLiteral("disconnect") ? QStringLiteral("断开") : QStringLiteral("连接"));

    // 在后台线程执行注册表查询和重启，避免 Sleep、轮询和注册表遍历阻塞 UI。
    std::thread([this, triggerSource]() {
        const QString installPath = findVoicemeeterInstallPath();
        if (!isVoicemeeterRemoteApiAvailable(installPath)) {
            return;
        }
        QString errorMessage;
        const bool ok = voicemeeterRemoteClient.restartAudioEngine(installPath, &errorMessage);
        // 回到 UI 线程写日志。
        QMetaObject::invokeMethod(this, [this, ok, errorMessage, triggerSource]() {
            if (!ok) {
                qWarning() << "Voicemeeter restart failed:" << errorMessage;
                appendDeviceEventLog(QStringLiteral("voicemeeter-restart-failed"),
                                     QStringLiteral("Voicemeeter audio engine 重启失败（触发来源：%1）").arg(triggerSource));
            } else {
                appendDeviceEventLog(QStringLiteral("voicemeeter-restarted"),
                                     QStringLiteral("Voicemeeter audio engine 已重启（触发来源：%1）").arg(triggerSource));
            }
        }, Qt::QueuedConnection);
    }).detach();

    // 重启后重置合并状态，为下次事件做准备。
    lastVoicemeeterRestartDirection.clear();
    lastVoicemeeterRestartAction.clear();
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

    if (syncWindowsVolumeCheck->isChecked()) {
        float defaultVolumeScalar = 0.0f;
        if (audioDeviceManager.getDefaultRenderVolume(&defaultVolumeScalar)) {
            slider->setEnabled(true);
            slider->blockSignals(true);
            slider->setValue(qRound(defaultVolumeScalar * 100.0f));
            slider->blockSignals(false);
        }

        registerVolumeCallbackForCurrentDevice();
        return;
    }

    float volumeScalar = 0.0f;
    if (!audioDeviceManager.readDeviceVolume(devices[index].volume, &volumeScalar)) {
        return;
    }

    slider->setEnabled(true);
    slider->blockSignals(true);
    slider->setValue(qRound(volumeScalar * 100.0f));
    slider->blockSignals(false);
}

void MainWindow::onSliderChanged(int value)
{
    const float volumeScalar = static_cast<float>(value) / 100.0f;

    if (syncWindowsVolumeCheck->isChecked()) {
        IAudioEndpointVolume *defaultVolume = nullptr;
        if (audioDeviceManager.getDefaultRenderEndpoint(&defaultVolume, nullptr)) {
            audioDeviceManager.setDeviceVolume(defaultVolume, volumeScalar, &kVolumeSyncContext);
            defaultVolume->Release();
        }

        if (!internalVolumeChange) {
            applyVolumeToSelectedDevices(volumeScalar);
        }
        return;
    }

    const int index = deviceBox->currentIndex();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        return;
    }

    audioDeviceManager.setDeviceVolume(devices[index].volume, volumeScalar, nullptr);
}

void MainWindow::loadDevices(bool showVirtual)
{
    refreshDevices(showVirtual);
    updateControlLockState();

    if (devices.empty()) {
        onDeviceChanged(-1);
        return;
    }

    deviceBox->setCurrentIndex(0);
    onDeviceChanged(0);
}

void MainWindow::onSyncWindowsVolumeToggled(bool checked)
{
    syncDeviceHintLabel->setEnabled(checked);
    syncDeviceList->setEnabled(checked);
    updateControlLockState();

    if (checked) {
        float defaultVolumeScalar = 0.0f;
        if (audioDeviceManager.getDefaultRenderVolume(&defaultVolumeScalar)) {
            slider->blockSignals(true);
            slider->setValue(qRound(defaultVolumeScalar * 100.0f));
            slider->blockSignals(false);
        }
        registerVolumeCallbackForCurrentDevice();
    } else {
        unregisterVolumeCallback();
        onDeviceChanged(deviceBox->currentIndex());
    }
}

void MainWindow::onAutoStartToggled(bool checked)
{
    updateAutoStart(checked);
}
