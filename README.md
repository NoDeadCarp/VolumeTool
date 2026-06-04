# VolumeTool

Windows 音频设备音量管理工具，支持独立控制各设备音量、多设备音量同步，以及 Voicemeeter audio engine 自动重启。

## 功能

### 独立设备音量控制

- 枚举系统中所有音频输出设备，通过滑条独立调节每个设备的音量
- 自动识别并过滤虚拟设备（Voicemeeter、NVIDIA Virtual Audio、AMD/NVIDIA High Definition Audio 等）
- 可选择是否显示虚拟设备

### Windows 音量同步

- 开启后，滑条与 Windows 默认输出设备音量双向联动
- 支持勾选多个设备，拖动滑条时同时调整所有已勾选设备的音量
- 外部修改系统音量时自动同步到滑条和所有勾选设备
- 使用 COM 回调避免循环联动

### Voicemeeter 自动重启

Voicemeeter 在音频设备热插拔后经常需要手动重启 audio engine 才能正常工作，此功能自动完成这一过程：

- 自动检测 Voicemeeter 安装路径和 Remote API 可用性
- 四种触发条件，可独立开关：
  - 输出设备连接时重启
  - 输出设备断开时重启
  - 输入设备连接时重启
  - 输入设备断开时重启
- 支持手动重启，日志中标注"手动"触发来源
- 蓝牙耳机连接时自动合并 render/capture 事件，只重启一次
- 快速插拔时按事件类型独立防抖（断开和连接互不影响）
- 设备变动日志记录最近 100 条事件，带时间戳和去重

## 界面预览

应用包含三个标签页：

- **控制** — 设备选择和音量滑条
- **设置** — 开机自启、Windows 音量同步、同步设备勾选
- **Voicemeeter** — 自动重启选项、手动重启按钮、设备变动日志

## 运行要求

- Windows 10 及以上
- Microsoft Visual C++ 运行库
- Voicemeeter（可选，仅自动重启功能需要，需安装并带有 `VoicemeeterRemote64.dll`）

## 许可

本项目仅供个人学习使用。
