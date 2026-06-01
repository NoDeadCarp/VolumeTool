#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QSlider>
#include <QTabWidget>
#include <QWidget>

#include <atomic>
#include <vector>

#include <endpointvolume.h>

#include "audiodevicemanager.h"

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

private:
    QTabWidget *tabWidget;
    QWidget *controlPage;
    QWidget *settingsPage;

    QComboBox *deviceBox;
    QCheckBox *showVirtualCheck;
    QSlider *slider;
    QLabel *label;
    QLabel *syncModeHintLabel;

    QCheckBox *autoStartCheck;
    QCheckBox *restartAudioEngineCheck;
    QCheckBox *syncWindowsVolumeCheck;
    QLabel *syncDeviceHintLabel;
    QListWidget *syncDeviceList;

    bool comInitialized = false;
    bool internalVolumeChange = false;
    VolumeCallback *volumeCallback = nullptr;
    IAudioEndpointVolume *callbackVolume = nullptr;

    // 控制页当前可直接操作的设备列表。
    std::vector<AudioDeviceEntry> devices;
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
    // 从系统读取开机自启等初始状态。
    void loadSettings();
    // 写入或移除开机自启注册表项。
    void updateAutoStart(bool enabled);
    // 给 Windows 默认播放设备注册音量变化回调。
    void registerVolumeCallbackForCurrentDevice();
    // 取消当前注册的系统音量回调。
    void unregisterVolumeCallback();
    // 将一个音量值广播到设置页勾选的所有设备。
    void applyVolumeToSelectedDevices(float volumeScalar);
    // 返回软件在注册表里使用的显示名称。
    QString applicationDisplayName() const;

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
