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

    // 忽略由软件自己发起的回调，避免循环联动。
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
    // 标准 COM 接口查询。
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
    // 增加回调对象引用计数。
    return ++refCount;
}

ULONG STDMETHODCALLTYPE VolumeCallback::Release()
{
    // 释放回调对象引用计数并在归零时销毁对象。
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

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR)
{
    // 默认输出设备切换后，刷新列表并记一条日志。
    if (flow == eRender && role == eConsole) {
        if (owner) {
            owner->appendDeviceEventLog(QStringLiteral("默认输出设备已变动"));
        }
        scheduleRefresh();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceAdded(LPCWSTR)
{
    // 新设备加入后刷新列表并记录日志。
    if (owner) {
        owner->appendDeviceEventLog(QStringLiteral("检测到新设备加入"));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceRemoved(LPCWSTR)
{
    // 设备移除后刷新列表并记录日志。
    if (owner) {
        owner->appendDeviceEventLog(QStringLiteral("检测到设备被移除"));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceStateChanged(LPCWSTR, DWORD)
{
    // 启用、禁用等状态变化也会影响设备展示。
    if (owner) {
        owner->appendDeviceEventLog(QStringLiteral("检测到设备状态变化"));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
{
    // 设备属性变化后刷新列表并记录日志。
    if (owner) {
        owner->appendDeviceEventLog(QStringLiteral("检测到设备属性变化"));
    }
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::QueryInterface(REFIID iid, VOID **object)
{
    // 标准 COM 接口查询。
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
    // 增加设备通知回调对象引用计数。
    return ++refCount;
}

ULONG STDMETHODCALLTYPE DeviceNotificationCallback::Release()
{
    // 释放设备通知回调对象引用计数并在归零时销毁对象。
    const ULONG count = --refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

void DeviceNotificationCallback::scheduleRefresh()
{
    // 系统通常会连续发出多条通知，这里统一切回 UI 线程刷新。
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
      deviceLogList(new QListWidget(this)),
      deviceRefreshTimer(new QTimer(this)),
      voicemeeterRestartTimer(new QTimer(this))
{
    // 构造阶段完成界面、音频环境和事件绑定。
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
    connect(voicemeeterRestartTimer, &QTimer::timeout,
            this, &MainWindow::restartVoicemeeterAudioEngine);

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

    registerDeviceNotifications();
    loadDevices(showVirtualCheck->isChecked());
}

MainWindow::~MainWindow()
{
    // 退出前先解注册回调，再释放设备资源。
    unregisterDeviceNotifications();
    unregisterVolumeCallback();
    clearDevices();

    if (comInitialized) {
        CoUninitialize();
    }
}

void MainWindow::setupUi()
{
    // 控制页负责即时操作，设置页负责通用设置，Voicemeeter 页负责相关状态和日志。
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

    auto *deviceLogGroup = new QGroupBox(QStringLiteral("设备变动日志"), voicemeeterPage);
    auto *deviceLogLayout = new QVBoxLayout(deviceLogGroup);
    deviceLogList->setSelectionMode(QAbstractItemView::NoSelection);
    deviceLogLayout->addWidget(deviceLogList);

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
    // Core Audio 基于 COM，窗口线程需要先初始化 COM 环境。
    const HRESULT hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr)) {
        comInitialized = true;
    }
}

void MainWindow::loadSettings()
{
    // 启动时读取开机自启状态并同步到界面。
    QSettings runSettings(QString::fromWCharArray(kAutoStartRegistryPath), QSettings::NativeFormat);
    QSignalBlocker blocker(autoStartCheck);
    autoStartCheck->setChecked(runSettings.contains(applicationDisplayName()));
    updateAudioEngineOptionState();
}

QString MainWindow::applicationDisplayName() const
{
    // 统一软件名称，避免注册表键名和显示名称不一致。
    const QString explicitName = QCoreApplication::applicationName().trimmed();
    return explicitName.isEmpty() ? QStringLiteral("VolumeTool") : explicitName;
}

bool MainWindow::isVoicemeeterInstalled() const
{
    // 直接复用安装路径检测结果。
    return !findVoicemeeterInstallPath().isEmpty();
}

bool MainWindow::isVoicemeeterRemoteApiAvailable(const QString &installPath) const
{
    // 只有安装目录中存在 Remote API DLL，才允许启用相关自动化功能。
    return voicemeeterRemoteClient.isAvailable(installPath);
}

QString MainWindow::findVoicemeeterInstallPath() const
{
    // 优先从卸载注册表中提取真实的绝对安装目录。
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
    // 只有安装了 Voicemeeter 且 Remote API 可用，重启 audio engine 选项才有意义。
    const QString installPath = findVoicemeeterInstallPath();
    const bool installed = !installPath.isEmpty();
    const bool remoteApiAvailable = installed && isVoicemeeterRemoteApiAvailable(installPath);

    restartAudioEngineCheck->setEnabled(remoteApiAvailable);
    audioEngineStatusLabel->setText(installed
        ? QStringLiteral("已安装")
        : QStringLiteral("未检测到安装"));
    audioEngineStatusLabel->setStyleSheet(installed
        ? QStringLiteral("color: #15803d;")
        : QStringLiteral("color: #dc2626;"));
    audioEnginePathLabel->setText(installed
        ? QStringLiteral("安装路径：%1").arg(installPath)
        : QStringLiteral("安装路径：未检测到安装路径"));
    audioEnginePathLabel->setWordWrap(true);
    audioEnginePathLabel->setStyleSheet(installed
        ? QStringLiteral("color: #374151;")
        : QStringLiteral("color: #6b7280;"));
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
    // 开机自启通过当前用户 Run 注册表项控制。
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
    // 设备接口统一交给 AudioDeviceManager 释放。
    audioDeviceManager.releaseDevices(devices);
}

void MainWindow::refreshDevices(bool showVirtual)
{
    // 刷新控制页设备列表时，同时刷新设置页同步设备列表。
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
    // 同步设备列表始终显示全部输出设备，并尽量保留已有勾选。
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
    // 开启同步模式后，控制页的设备选择改为只读。
    const bool syncEnabled = syncWindowsVolumeCheck->isChecked();
    deviceBox->setEnabled(!syncEnabled);
    showVirtualCheck->setEnabled(!syncEnabled);
    syncModeHintLabel->setVisible(syncEnabled);
}

void MainWindow::registerVolumeCallbackForCurrentDevice()
{
    // 同步模式下只监听 Windows 默认播放设备，保证和系统音量条一致。
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
    // 切换模式或退出时，注销当前默认设备音量回调。
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
    // 注册系统音频设备变更通知，让界面可以自动刷新。
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
    // 退出前或重新注册前，先解除旧的设备通知回调。
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
    // 将统一后的目标音量广播给设置页勾选的所有同步设备。
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
    // 这是 Windows 音量变化进入软件后的统一入口。
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
    // 日志只保留在内存中，最多 100 条。
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

void MainWindow::handleDeviceListChanged()
{
    // 多个系统通知会在短时间连续触发，这里统一交给定时器做防抖刷新。
    if (deviceRefreshTimer) {
        deviceRefreshTimer->start();
    }

    scheduleVoicemeeterRestart();
}

void MainWindow::scheduleVoicemeeterRestart()
{
    // 设备热插拔时常常连发多次通知，这里延后 3 秒，只重启一次。
    if (!restartAudioEngineCheck || !restartAudioEngineCheck->isChecked()) {
        return;
    }

    const QString installPath = findVoicemeeterInstallPath();
    if (!isVoicemeeterRemoteApiAvailable(installPath)) {
        return;
    }

    if (voicemeeterRestartTimer) {
        voicemeeterRestartTimer->start();
    }
}

void MainWindow::restartVoicemeeterAudioEngine()
{
    // 使用官方 Remote API 发送 Command.Restart，让 Voicemeeter 重新初始化输入输出设备。
    const QString installPath = findVoicemeeterInstallPath();
    QString errorMessage;
    if (!voicemeeterRemoteClient.restartAudioEngine(installPath, &errorMessage)) {
        qWarning() << "Voicemeeter restart failed:" << errorMessage;
        appendDeviceEventLog(QStringLiteral("Voicemeeter audio engine 重启失败"));
        return;
    }

    appendDeviceEventLog(QStringLiteral("Voicemeeter audio engine 已重启"));
}

void MainWindow::onDeviceChanged(int index)
{
    // 非同步模式跟随当前设备，同步模式跟随 Windows 默认音量。
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
    // 滑条是软件唯一的音量输入源，模式不同会走不同控制路径。
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
    // 重新加载控制页设备，并同步滑条到新的首项或默认状态。
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
    // 切换同步模式时，同时更新 UI 锁定状态和音量回调绑定关系。
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
    // 用户勾选变化后立即写回系统设置。
    updateAutoStart(checked);
}
