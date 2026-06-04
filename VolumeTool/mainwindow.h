#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <vector>

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include "audiodevicemanager.h"
#include "voicemeeterremoteclient.h"

class MainWindow;

class VolumeCallback : public IAudioEndpointVolumeCallback
{
public:
    // 保存主窗口指针，供系统音量回调时回到 UI 层。
    explicit VolumeCallback(MainWindow *owner);

    // 当系统音量变化时由 Core Audio 回调。
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notifyData) override;
    // COM 接口查询入口。
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, VOID **object) override;
    // 增加 COM 回调对象引用计数。
    ULONG STDMETHODCALLTYPE AddRef() override;
    // 释放 COM 回调对象引用计数。
    ULONG STDMETHODCALLTYPE Release() override;

private:
    std::atomic<ULONG> refCount{1};
    MainWindow *owner;
};

class DeviceNotificationCallback : public IMMNotificationClient
{
public:
    // 保存主窗口指针，设备变化时回调到界面层。
    explicit DeviceNotificationCallback(MainWindow *owner);

    // 默认播放设备变化时触发。
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override;
    // 新设备加入时触发。
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    // 设备移除时触发。
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    // 设备状态变化时触发。
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    // 设备属性变化时触发。
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;
    // COM 接口查询入口。
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, VOID **object) override;
    // 增加 COM 回调对象引用计数。
    ULONG STDMETHODCALLTYPE AddRef() override;
    // 释放 COM 回调对象引用计数。
    ULONG STDMETHODCALLTYPE Release() override;

private:
    // 把系统设备变化事件合并后转发给主窗口刷新。
    void scheduleRefresh();
    // 判断设备属于输入还是输出方向，并通知主窗口。action 为 "connect"/"disconnect" 或空。
    void notifyDeviceChange(LPCWSTR pwstrDeviceId, const QString &action);
    // 判断设备 ID 是否对应虚拟设备，用于过滤日志。
    bool isVirtualDeviceId(LPCWSTR pwstrDeviceId) const;

    std::atomic<ULONG> refCount{1};
    MainWindow *owner;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 创建主窗口并初始化界面、音频和设置状态。
    MainWindow(QWidget *parent = nullptr);
    // 释放回调和设备接口资源。
    ~MainWindow();

    // 处理来自 Windows 默认音量回调的外部音量变化。
    void handleExternalVolumeChange(float volumeScalar);
    // 记录一条设备变动日志，并更新界面中的日志列表。
    void appendDeviceEventLog(const QString &message);
    // 记录一条设备事件日志，并对短时间内的重复事件做去重。
    void appendDeviceEventLog(const QString &eventKey, const QString &message);
    // 根据设备 ID 生成适合日志显示的设备名称文本。
    QString describeDeviceForLog(const QString &deviceId) const;
    // 清空内存中的设备变动日志和去重状态。
    void clearDeviceEventLogs();
    // 处理来自系统设备通知的列表刷新请求。
    void handleDeviceListChanged();
    // 处理来自系统设备通知的 Voicemeeter 重启调度请求，direction 和 action 已由回调线程查询。
    void handleDeviceChangeForRestart(const QString &deviceId, const QString &direction, const QString &action);
    // 返回音频设备管理器的引用，供回调线程查询设备方向。
    AudioDeviceManager &audioDeviceMgr() { return audioDeviceManager; }
    // 返回已缓存的设备方向信息，用于设备已断开时仍能判断方向。
    QString cachedDeviceDirection(const QString &deviceId) const;

private:
    QTabWidget *tabWidget;
    QWidget *controlPage;
    QWidget *settingsPage;
    QWidget *voicemeeterPage;

    QComboBox *deviceBox;
    QCheckBox *showVirtualCheck;
    QSlider *slider;
    QLabel *label;
    QLabel *syncModeHintLabel;

    QCheckBox *autoStartCheck;
    QCheckBox *syncWindowsVolumeCheck;
    QLabel *syncDeviceHintLabel;
    QListWidget *syncDeviceList;

    QCheckBox *restartAudioEngineOnRenderConnectCheck;
    QCheckBox *restartAudioEngineOnRenderDisconnectCheck;
    QCheckBox *restartAudioEngineOnCaptureConnectCheck;
    QCheckBox *restartAudioEngineOnCaptureDisconnectCheck;
    QLabel *audioEngineStatusLabel;
    QLabel *audioEnginePathLabel;
    QPushButton *manualRestartAudioEngineButton;
    QListWidget *deviceLogList;
    QPushButton *clearDeviceLogButton;

    bool comInitialized = false;
    bool internalVolumeChange = false;
    VolumeCallback *volumeCallback = nullptr;
    IAudioEndpointVolume *callbackVolume = nullptr;
    DeviceNotificationCallback *deviceNotificationCallback = nullptr;
    IMMDeviceEnumerator *notificationEnumerator = nullptr;
    QTimer *deviceRefreshTimer = nullptr;
    // 合并短时间内多个设备变化事件后再执行重启，避免蓝牙耳机连接时 render/capture 分别触发两次重启。
    QTimer *voicemeeterRestartTimer = nullptr;

    // 控制页当前可直接操作的设备列表。
    std::vector<AudioDeviceEntry> devices;
    // 设备变动日志只保留内存中的最近 100 条。
    QStringList deviceEventLogs;
    // 记录最近一次各类设备事件的时间，用来抑制高频重复日志。
    QMap<QString, qint64> deviceEventLogTimestamps;
    // 记录每个事件 key 对应的日志索引，用于在去重窗口内更新已有日志。
    QMap<QString, int> deviceEventLogIndices;
    // 记录最近一次 Voicemeeter 重启请求时间（按 direction-action 维度），避免同一事件短时间重复重启。
    // 不同事件（如"输出设备断开"和"输出设备连接"）互不影响。
    QMap<QString, qint64> lastVoicemeeterRestartPerAction;
    // 记录最近一次触发重启的设备方向+状态，用于日志中区分和蓝牙耳机合并。
    QString lastVoicemeeterRestartDirection;
    QString lastVoicemeeterRestartAction;
    // 缓存已知设备的方向（"render" 或 "capture"），设备移除后仍可查询。
    QMap<QString, QString> deviceDirectionCache;
    AudioDeviceManager audioDeviceManager;

    // 初始化窗口线程需要的 COM 环境。
    void initAudio();
    // 创建控制页和设置页的界面控件。
    void setupUi();
    // 清理当前缓存的设备接口。
    void clearDevices();
    // 刷新控制页设备列表。
    void refreshDevices(bool showVirtual);
    // 刷新设置页“同步设备”勾选列表。
    void updateSyncDeviceList();
    // 根据是否开启同步模式更新控制页禁用状态。
    void updateControlLockState();
    // 根据系统是否安装 Voicemeeter 更新重启 audio engine 选项状态。
    void updateAudioEngineOptionState();
    // 从系统读取开机自启等初始状态。
    void loadSettings();
    // 检测当前系统中是否安装了 Voicemeeter。
    bool isVoicemeeterInstalled() const;
    // 返回检测到的 Voicemeeter 安装路径，未找到时返回空字符串。
    QString findVoicemeeterInstallPath() const;
    // 返回 Voicemeeter Remote API 是否可用。
    bool isVoicemeeterRemoteApiAvailable(const QString &installPath) const;
    // 写入或移除开机自启注册表项。
    void updateAutoStart(bool enabled);
    // 给 Windows 默认播放设备注册音量变化回调。
    void registerVolumeCallbackForCurrentDevice();
    // 取消当前注册的系统音量回调。
    void unregisterVolumeCallback();
    // 注册系统音频设备变更通知。
    void registerDeviceNotifications();
    // 取消系统音频设备变更通知。
    void unregisterDeviceNotifications();
    // 将一个音量值广播到设置页勾选的所有设备。
    void applyVolumeToSelectedDevices(float volumeScalar);
    // 在设备频繁变动时合并多次请求，延迟重启 Voicemeeter audio engine。
    // direction 标识方向："render"/"capture"/"all"，action 标识动作："connect"/"disconnect"。
    void scheduleVoicemeeterRestart(const QString &direction, const QString &action);
    // 通过 Voicemeeter Remote API 真正执行 audio engine 重启。
    // isManual 为 true 时跳过防抖检查，日志标注"手动"。
    void restartVoicemeeterAudioEngine(bool isManual = false);
    // 返回软件在注册表里使用的显示名称。
    QString applicationDisplayName() const;

    VoicemeeterRemoteClient voicemeeterRemoteClient;

private slots:
    // 控制页切换设备时刷新滑条显示。
    void onDeviceChanged(int index);
    // 用户拖动滑条时应用音量。
    void onSliderChanged(int value);
    // 根据“显示虚拟设备”状态重新加载控制页设备。
    void loadDevices(bool showVirtual);
    // 切换 Windows 音量同步模式。
    void onSyncWindowsVolumeToggled(bool checked);
    // 切换开机自启状态。
    void onAutoStartToggled(bool checked);
};
