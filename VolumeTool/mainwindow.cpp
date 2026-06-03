#include "MainWindow.h"

#include <QCoreApplication>
#include <QGroupBox>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

constexpr const wchar_t *kAutoStartRegistryPath = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const GUID kVolumeSyncContext = {
    0x0fdd79ca, 0x95d0, 0x47a5, {0x97, 0x74, 0xf8, 0x41, 0x82, 0x37, 0x33, 0x9c}
};

}

DeviceNotificationCallback::DeviceNotificationCallback(MainWindow *owner)
    : owner(owner)
{
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR)
{
    // 默认播放设备切换后，控制页和同步逻辑都需要刷新。
    if (flow == eRender && role == eConsole) {
        scheduleRefresh();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceAdded(LPCWSTR)
{
    // 插入新设备后刷新列表。
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceRemoved(LPCWSTR)
{
    // 设备移除后刷新列表。
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnDeviceStateChanged(LPCWSTR, DWORD)
{
    // 启用/禁用等状态变化也会影响展示结果。
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
{
    // 名称等属性变化后同步更新界面显示。
    scheduleRefresh();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationCallback::QueryInterface(REFIID iid, VOID **object)
{
    // 标准 COM 接口查询实现。
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
    // 增加设备通知回调对象引用。
    return ++refCount;
}

ULONG STDMETHODCALLTYPE DeviceNotificationCallback::Release()
{
    // 归还引用并在归零时销毁对象。
    const ULONG count = --refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

void DeviceNotificationCallback::scheduleRefresh()
{
    // 设备通知通常会连续触发，这里统一切回 UI 线程做防抖刷新。
    if (!owner) {
        return;
    }

    QMetaObject::invokeMethod(owner, [this]() {
        if (owner) {
            owner->handleDeviceListChanged();
        }
    }, Qt::QueuedConnection);
}

VolumeCallback::VolumeCallback(MainWindow *owner)
    : owner(owner)
{
}

HRESULT STDMETHODCALLTYPE VolumeCallback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notifyData)
{
    // Windows 默认设备音量变化后，从这里把事件转发回主窗口。
    if (!owner || !notifyData) {
        return E_POINTER;
    }

    // 忽略由软件自己触发的音量回调，避免循环联动。
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
    // 标准 COM 接口查询实现。
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
    // 增加回调对象生命周期引用。
    return ++refCount;
}

ULONG STDMETHODCALLTYPE VolumeCallback::Release()
{
    // 归还引用并在计数归零时删除对象。
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
      syncModeHintLabel(new QLabel(QStringLiteral("已开启 Windows 音量同步，请到“设置”页的“同步设备”中勾选需要联动的设备。"), this)),
      autoStartCheck(new QCheckBox(QStringLiteral("开机自启"), this)),
      restartAudioEngineCheck(new QCheckBox(QStringLiteral("设备变动时重启 audio engine"), this)),
      syncWindowsVolumeCheck(new QCheckBox(QStringLiteral("同步 Windows 音量滑轨"), this)),
      syncDeviceHintLabel(new QLabel(QStringLiteral("勾选后可选择需要跟随软件一起同步的设备。"), this)),
      syncDeviceList(new QListWidget(this)),
      deviceRefreshTimer(new QTimer(this))
{
    // 构造阶段只做三件事：搭 UI、准备音频环境、绑定交互。
    setupUi();
    initAudio();
    loadSettings();

    deviceRefreshTimer->setSingleShot(true);
    deviceRefreshTimer->setInterval(200);
    connect(deviceRefreshTimer, &QTimer::timeout, this, [this]() {
        loadDevices(showVirtualCheck->isChecked());
    });

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
    // 退出前先撤掉回调，再释放设备资源。
    unregisterDeviceNotifications();
    unregisterVolumeCallback();
    clearDevices();

    if (comInitialized) {
        CoUninitialize();
    }
}

void MainWindow::setupUi()
{
    // 控制页负责即时操作，设置页负责同步和行为选项。
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
    updateControlLockState();
}

void MainWindow::initAudio()
{
    // Core Audio 基于 COM，窗口线程先初始化 COM 环境。
    const HRESULT hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr)) {
        comInitialized = true;
    }
}

void MainWindow::loadSettings()
{
    // 目前只有开机自启需要在启动时回显到界面。
    QSettings runSettings(QString::fromWCharArray(kAutoStartRegistryPath), QSettings::NativeFormat);

    // 这里只回显勾选状态，不在初始化阶段再次写注册表。
    QSignalBlocker blocker(autoStartCheck);
    autoStartCheck->setChecked(runSettings.contains(applicationDisplayName()));
}

QString MainWindow::applicationDisplayName() const
{
    // 统一软件名称，避免注册表键名和显示名称不一致。
    const QString explicitName = QCoreApplication::applicationName().trimmed();
    return explicitName.isEmpty() ? QStringLiteral("VolumeTool") : explicitName;
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
    // 设备接口由 AudioDeviceManager 统一释放，避免窗口层手动漏掉。
    audioDeviceManager.releaseDevices(devices);
}

void MainWindow::refreshDevices(bool showVirtual)
{
    // 控制页设备列表刷新时，同时带动设置页同步列表更新。
    unregisterVolumeCallback();
    clearDevices();

    // 清空下拉框时临时屏蔽信号，避免触发无效设备切换。
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
    // 设置页始终显示全部设备，并尽量保留用户已有勾选。
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
    // 同步模式开启后，控制页设备选择改为只读。
    const bool syncEnabled = syncWindowsVolumeCheck->isChecked();
    deviceBox->setEnabled(!syncEnabled);
    showVirtualCheck->setEnabled(!syncEnabled);
    syncModeHintLabel->setVisible(syncEnabled);
}

void MainWindow::registerVolumeCallbackForCurrentDevice()
{
    // 同步模式下只监听 Windows 默认设备，保证和系统音量条一致。
    unregisterVolumeCallback();

    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    // 同步模式监听的是 Windows 默认输出设备，而不是控制页下拉框当前项。
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
    // 切换模式或退出时，要把默认设备回调干净撤掉。
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
    // 注册系统设备变更通知，让插拔设备后界面可以自动刷新。
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
    // 退出前或重新注册前，先把旧的设备通知回调解绑。
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
    // 把统一后的目标音量广播给设置页勾选的每个设备。
    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    // 同步模式下只根据设置页勾选结果广播，不依赖控制页筛选状态。
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
    // 这是 Windows 音量条变动进入软件后的统一入口。
    if (!syncWindowsVolumeCheck->isChecked()) {
        return;
    }

    // Windows 侧音量变化时，先刷新滑条，再同步到勾选设备。
    internalVolumeChange = true;
    slider->setValue(qRound(volumeScalar * 100.0f));
    internalVolumeChange = false;

    applyVolumeToSelectedDevices(volumeScalar);
}

void MainWindow::handleDeviceListChanged()
{
    // 多个系统通知会在短时间连续触发，这里统一交给定时器做轻量防抖刷新。
    if (deviceRefreshTimer) {
        deviceRefreshTimer->start();
    }
}

void MainWindow::onDeviceChanged(int index)
{
    // 非同步模式下跟随当前设备，同步模式下跟随 Windows 默认音量。
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
    // 重新加载控制页设备，并把滑条同步到新的首项或默认状态。
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
    // 切换同步模式时，要同时更新 UI 锁定和回调监听关系。
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
    // 用户勾选变化后立刻写回系统设置。
    updateAutoStart(checked);
}
