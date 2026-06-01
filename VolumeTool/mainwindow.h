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
#include <mmdeviceapi.h>

struct DeviceItem {
    QString id;
    QString name;
    IMMDevice *device;
    IAudioEndpointVolume *volume;
};

class MainWindow;

class VolumeCallback : public IAudioEndpointVolumeCallback
{
public:
    explicit VolumeCallback(MainWindow *owner);

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notifyData) override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, VOID **object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    std::atomic<ULONG> refCount{1};
    MainWindow *owner;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

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

    // 当前可操作的输出设备及其音量接口。
    std::vector<DeviceItem> devices;

    void initAudio();
    void setupUi();
    void clearDevices();
    void refreshDevices(bool showVirtual);
    void updateSyncDeviceList();
    void updateControlLockState();
    void loadSettings();
    void updateAutoStart(bool enabled);
    void registerVolumeCallbackForCurrentDevice();
    void unregisterVolumeCallback();
    void applyVolumeToSelectedDevices(float volumeScalar);
    QString applicationDisplayName() const;

private slots:
    void onDeviceChanged(int index);
    void onSliderChanged(int value);
    void loadDevices(bool showVirtual);
    void onSyncWindowsVolumeToggled(bool checked);
    void onAutoStartToggled(bool checked);
    bool isVirtualDevice(IMMDevice *device);
};
