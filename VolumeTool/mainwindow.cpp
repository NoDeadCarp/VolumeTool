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
    if (flow == eRender && role == eConsole) {
        if (owner) {
            const QString deviceText = deviceDisplayText(owner, deviceId);
            owner->appendDeviceEventLog(QStringLiteral("default-render:%1").arg(deviceEventId(deviceId)),
                                        QStringLiteral("默认输出设备已变动：%1").arg(deviceText));
        }
        scheduleRefresh();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceAdded(LPCWSTR deviceId)
{
    if (owner) {
        const QString deviceText = deviceDisplayText(owner, deviceId);
        owner->appendDeviceEventLog(QStringLiteral("device-added:%1").arg(deviceEventId(deviceId)),
                                    QStringLiteral("检测到新设备加入：%1").arg(deviceText));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceRemoved(LPCWSTR deviceId)
{
    if (owner) {
        const QString deviceText = deviceDisplayText(owner, deviceId);
        owner->appendDeviceEventLog(QStringLiteral("device-removed:%1").arg(deviceEventId(deviceId)),
                                    QStringLiteral("检测到设备被移除：%1").arg(deviceText));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState)
{
    if (owner) {
        const QString deviceText = deviceDisplayText(owner, deviceId);
        owner->appendDeviceEventLog(QStringLiteral("device-state:%1:%2").arg(deviceEventId(deviceId)).arg(newState),
                                    QStringLiteral("%1 状态变为：%2").arg(deviceText, deviceStateToText(newState)));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key)
{
    Q_UNUSED(deviceId)
    Q_UNUSED(key)
    scheduleRefresh();
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
      restartAudioEngineCheck(new QCheckBox(QStringLiteral("设备变动时重启 audio engine"), this)),
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
    voicemeeterRestartTimer->setInterval(3000);
    connect(voicemeeterRestartTimer, &QTimer::timeout, this, &MainWindow::restartVoicemeeterAudioEngine);

    connect(deviceBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceChanged);
    connect(slider, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);
    connect(showVirtualCheck, &QCheckBox::toggled, this, &MainWindow::loadDevices);
    connect(syncWindowsVolumeCheck, &QCheckBox::toggled, this, &MainWindow::onSyncWindowsVolumeToggled);
    connect(autoStartCheck, &QCheckBox::toggled, this, &MainWindow::onAutoStartToggled);
    connect(manualRestartAudioEngineButton, &QPushButton::clicked, this, [this]() {
        restartVoicemeeterAudioEngine();
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
    voicemeeterActionLayout->addWidget(restartAudioEngineCheck);
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

    restartAudioEngineCheck->setEnabled(remoteApiAvailable);
    audioEngineStatusLabel->setText(installed ? QStringLiteral("已安装") : QStringLiteral("未检测到安装"));
    audioEngineStatusLabel->setStyleSheet(installed ? QStringLiteral("color: #15803d;") : QStringLiteral("color: #dc2626;"));
    audioEnginePathLabel->setText(installed
        ? QStringLiteral("安装路径：%1").arg(installPath)
        : QStringLiteral("安装路径：未检测到安装路径"));
    audioEnginePathLabel->setWordWrap(true);
    audioEnginePathLabel->setStyleSheet(installed ? QStringLiteral("color: #374151;") : QStringLiteral("color: #6b7280;"));
    restartAudioEngineCheck->setToolTip(!installed
        ? QStringLiteral("未检测到 Voicemeeter，安装后才能使用这个选项。")
        : (remoteApiAvailable
            ? QStringLiteral("检测到 Remote API，可在设备变动 3 秒后自动重启 audio engine。")
            : QStringLiteral("检测到 Voicemeeter，但未找到 Remote API DLL。")));

    if (!remoteApiAvailable) {
        QSignalBlocker blocker(restartAudioEngineCheck);
        restartAudioEngineCheck->setChecked(false);
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
    if (lastTimestamp > 0 && now - lastTimestamp < kDeviceLogDedupWindowMs) {
        return;
    }

    deviceEventLogTimestamps.insert(eventKey, now);
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
    lastVoicemeeterRestartRequestMs = 0;

    if (deviceLogList) {
        deviceLogList->clear();
    }
}

void MainWindow::handleDeviceListChanged()
{
    if (deviceRefreshTimer) {
        deviceRefreshTimer->start();
    }

    scheduleVoicemeeterRestart();
}

void MainWindow::scheduleVoicemeeterRestart()
{
    if (!restartAudioEngineCheck || !restartAudioEngineCheck->isChecked()) {
        return;
    }

    const QString installPath = findVoicemeeterInstallPath();
    if (!isVoicemeeterRemoteApiAvailable(installPath)) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastVoicemeeterRestartRequestMs > 0
        && now - lastVoicemeeterRestartRequestMs < kVoicemeeterRestartWindowMs) {
        appendDeviceEventLog(QStringLiteral("voicemeeter-restart-skipped"),
                             QStringLiteral("Voicemeeter audio engine 重启请求已在时间窗口内，已跳过重复触发"));
        return;
    }

    lastVoicemeeterRestartRequestMs = now;
    if (voicemeeterRestartTimer) {
        voicemeeterRestartTimer->start();
    }
}

void MainWindow::restartVoicemeeterAudioEngine()
{
    const QString installPath = findVoicemeeterInstallPath();
    QString errorMessage;
    if (!voicemeeterRemoteClient.restartAudioEngine(installPath, &errorMessage)) {
        qWarning() << "Voicemeeter restart failed:" << errorMessage;
        appendDeviceEventLog(QStringLiteral("voicemeeter-restart-failed"),
                             QStringLiteral("Voicemeeter audio engine 重启失败"));
        return;
    }

    appendDeviceEventLog(QStringLiteral("voicemeeter-restarted"),
                         QStringLiteral("Voicemeeter audio engine 已重启"));
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
