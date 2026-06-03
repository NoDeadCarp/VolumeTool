#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QStringList>
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
    // 处理来自系统设备通知的列表刷新请求。
    void handleDeviceListChanged();

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

    QCheckBox *restartAudioEngineCheck;
    QLabel *audioEngineStatusLabel;
    QLabel *audioEnginePathLabel;
    QListWidget *deviceLogList;

    bool comInitialized = false;
    bool internalVolumeChange = false;
    VolumeCallback *volumeCallback = nullptr;
    IAudioEndpointVolume *callbackVolume = nullptr;
    DeviceNotificationCallback *deviceNotificationCallback = nullptr;
    IMMDeviceEnumerator *notificationEnumerator = nullptr;
    QTimer *deviceRefreshTimer = nullptr;
    QTimer *voicemeeterRestartTimer = nullptr;

    // 控制页当前可直接操作的设备列表。
    std::vector<AudioDeviceEntry> devices;
    // 设备变动日志只保留内存中的最近 100 条。
    QStringList deviceEventLogs;
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
    void scheduleVoicemeeterRestart();
    // 通过 Voicemeeter Remote API 真正执行 audio engine 重启。
    void restartVoicemeeterAudioEngine();
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
