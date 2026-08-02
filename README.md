# 小米智能设备插件 for TrafficMonitor

> 在 TrafficMonitor 中查看和管理局域网内的小米智能插座与蓝牙温湿度计。

![Windows](https://img.shields.io/badge/Windows-x64-0078D4?logo=windows&logoColor=white)
![TrafficMonitor Plugin API](https://img.shields.io/badge/TrafficMonitor-Plugin%20API%20v7-3B82F6)
![Version](https://img.shields.io/badge/version-1.0.0-success)

插件通过局域网本地 MiIO 通信工作，不需要登录米家账号，也不依赖云端服务。

## 功能概览

- 监控每台智能插座功率、总功率和分组功率。
- 显示插座开关状态、在线/离线状态及最后成功更新时间。
- 双击插座功率显示项可异步切换开关，并通过 TrafficMonitor 气泡通知反馈结果。
- 显示蓝牙温湿度设备的数据，支持独立刷新频率和显示精度。
- 按分钟记录总功率、分组功率和单设备功率，自动保留 30 天历史。
- 提供实时值、24 小时曲线、今日峰值、平均值和最低值。
- 支持总功率、单插座功率、离线、温度上下限、湿度上下限报警及恢复提醒。
- 支持报警冷却时间，避免异常持续时重复通知。
- 支持设备命名、分组、启用/禁用、排序，以及全局、分组、单设备三级设置覆盖。
- 支持本地网段扫描、CIDR 输入、批量发现导入和完整配置导入/导出。
- 自动记住设置窗口位置和大小，支持分类恢复默认设置。

## 环境与兼容性

- Windows x64。
- 支持 TrafficMonitor Plugin API v7 的 TrafficMonitor 版本。
- 设备与电脑需要位于可互通的局域网内。
- 已内置常见 `chuangmi.*` 与 `cuco.*` 插座的功率和开关属性映射。

不同设备型号和固件的 MiIO 属性可能不同。对于未知型号或无可用开关属性的设备，插件会显示“状态未知”，不会通过功率推测开关状态。

## 安装

1. 在 [Releases](https://github.com/daimiaopeng/TrafficMonitor-Xiaomi-IoT-Plugin/releases) 下载最新压缩包。
2. 将 `XiaomiIoTPlugin.dll` 复制到 TrafficMonitor 的 `plugins` 目录，例如：

   ```text
   %USERPROFILE%\Documents\TrafficMonitor\plugins\
   ```

3. 重启 TrafficMonitor。
4. 打开插件“**小米智能设备插件**”的高级设置，填入设备 IP、Token、显示名称和分组。
5. 点击“保存并生效”。

升级后仍显示旧信息时，请完全退出 TrafficMonitor，确认 `plugins` 目录只有新的 `XiaomiIoTPlugin.dll`，再重新启动程序。

## 设置说明

| 页面 | 用途 |
| --- | --- |
| 智能插座 | 管理插座 IP、Token、名称、分组、启用状态、排序及开关控制。 |
| 蓝牙温湿度 | 配置蓝牙温湿度设备的刷新频率与显示精度。 |
| 告警与电费 | 设置总功率、温湿度、离线报警、冷却时间与电费单价。 |
| 通信与全网发现 | 测试本地通信、扫描网段，并批量导入发现的设备。 |
| 历史曲线 | 查看总功率、分组或单设备的实时值、24 小时曲线和当日统计。 |

刷新频率与显示精度按以下优先级生效：

```text
全局默认 → 分组覆盖 → 单设备覆盖
```

### 分组管理

可使用 `客厅`、`卧室`、`办公室` 等名称建立分组。分组可用于汇总功率、设置分组刷新率/精度/报警阈值、批量启用或禁用设备，并在历史曲线中筛选数据。

### 报警

所有报警使用 TrafficMonitor 气泡通知。异常触发时提示一次，恢复正常时再提示一次。默认连续 3 次轮询失败判定设备离线，默认报警冷却时间为 10 分钟；两项均可修改。

### 自动发现与导入

在“通信与全网发现”页面可输入 CIDR 网段，例如：

```text
192.168.2.0/24
```

扫描结果支持批量导入。自动发现只保存 IP、设备 ID 和可识别信息；新导入设备默认禁用，必须手动填入真实 Token 后才能查询型号、读取状态或控制设备。发现过程中的全 `FF` 占位 Token 不会被保存为设备 Token。

## 数据与安全

插件会在 TrafficMonitor 的插件配置目录保存：

| 文件 | 说明 |
| --- | --- |
| `XiaomiIoTPlugin.ini` | 设备、分组、刷新、报警和窗口设置。 |
| `XiaomiIoTPlugin_power_history.csv` | 分钟级功率历史，自动裁剪至最近 30 天。 |
| 导出的 JSON 文件 | 用于备份或迁移完整配置。 |
| `.env`（可选） | 仅在 INI 中缺少 Token 时提供本地 Token 回退值。 |

旧版 INI 会在读取时自动补齐稳定设备 ID、默认分组和默认规则。

`.env` 已被 Git 忽略。可复制 `.env.example` 为 `.env` 后填写 Token；首次运行或 INI 未包含 Token 时，插件会从配置目录的 `.env` 读取。已有 INI Token 始终优先，不会被 `.env` 覆盖。

```dotenv
XIAOMI_CHUANGMI_TOKEN=你的32位Token
XIAOMI_CUCO_TOKEN=你的32位Token
# 多设备配置可使用：XIAOMI_PLUG_1_TOKEN、XIAOMI_PLUG_2_TOKEN……
```

当前版本按兼容方式仍支持在 INI 保存 Token，导出的 JSON 也可能包含 Token。请勿上传或分享配置文件、Token、局域网 IP、设备 ID 或含这些信息的截图。Token 等同于局域网控制凭据，请妥善保管。

## 从源码构建

安装 Visual Studio C++ x64 编译工具后，在本目录执行：

```bat
build.bat
```

脚本会编译 `XiaomiIoTPlugin.dll` 并部署到本机 TrafficMonitor 插件目录。若提示 DLL 被占用，请完全退出 TrafficMonitor 后重新执行。

若 Visual Studio 不在脚本预设位置，请修改 `build.bat` 中的 `vcvars64.bat` 路径。

## GitHub Actions

仓库包含自动构建与发布工作流：

- 推送到 `main` 或创建 Pull Request 时执行 x64 编译验证。
- 推送 `v1.0.0` 这类标签时，自动打包 DLL、构建脚本、接口头文件与本 README，并创建 GitHub Release。

```bash
git tag v1.0.0
git push origin v1.0.0
```

## 反馈

提交 Issue 时请提供 TrafficMonitor/Windows 版本、设备型号和固件版本（如已知）、不含敏感信息的错误提示或截图，以及可复现步骤。请勿在 Issue 中提交 Token、完整配置文件或设备公网信息。

## 作者

- 作者：daimiaopeng
- 项目主页：[daimiaopeng/TrafficMonitor-Xiaomi-IoT-Plugin](https://github.com/daimiaopeng/TrafficMonitor-Xiaomi-IoT-Plugin)

## 许可证

当前仓库暂未附带许可证文件。如需使用、修改或分发，请先联系作者确认授权方式。
