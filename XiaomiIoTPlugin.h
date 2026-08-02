#pragma once
#include "PluginInterface.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <ctime>
#include <unordered_map>
#include <climits>

class CXiaomiIoTPlugin;

enum class PlugSwitchState { Unknown = -1, Off = 0, On = 1 };

struct PowerHistoryPoint
{
    std::time_t timestamp{};
    double power{};
};

struct GroupConfig
{
    std::wstring id = L"default";
    std::wstring name = L"默认分组";
    bool enabled = true;
    int refresh_interval_ms = 0;       // 0 = inherit global
    int precision = -1;                // -1 = inherit global
    bool alarm_enabled = false;
    double power_limit = 0.0;          // 0 = inherit global total rule only
};

struct PlugDeviceConfig
{
    // Persisted configuration
    std::wstring id;
    std::wstring ip = L"192.168.2.6";
    std::wstring token;
    std::wstring label = L"插座";
    std::wstring model;
    std::wstring group_id = L"default";
    bool is_chuangmi = true;
    bool auto_fetched = false;
    bool enabled = true;
    int refresh_interval_ms = 0;       // 0 = inherit group/global
    int precision = -1;                // -1 = inherit group/global
    bool alarm_enabled = false;
    double power_limit = 0.0;          // 0 = inherit group/global

    // Runtime state (never used as an identity)
    double power = 0.0;
    bool online = false;
    PlugSwitchState switch_state = PlugSwitchState::Unknown;
    bool switch_supported = false;
    int failed_polls = 0;
    std::time_t last_seen = 0;
};

struct PluginConfig
{
    std::vector<PlugDeviceConfig> devices;
    std::vector<GroupConfig> groups;

    std::wstring total_label = L"总功率";
    std::wstring ble_mac = L"A4:C1:38:D5:C2:20";
    std::wstring temp_label = L"温度";
    std::wstring hum_label = L"湿度";

    int refresh_interval_ms = 1000;
    int precision = 2;
    int thermometer_refresh_interval_ms = 10000;
    int thermometer_precision = 2;
    double electricity_price = 0.6;

    bool alarm_power_enabled = false;
    double alarm_power_limit = 2000.0;
    int alarm_cooldown_minutes = 10;
    int offline_failures = 3;
    bool temp_alarm_enabled = false;
    double temp_low = 0.0;
    double temp_high = 40.0;
    bool humidity_alarm_enabled = false;
    double humidity_low = 0.0;
    double humidity_high = 90.0;

    int window_x = INT_MIN;
    int window_y = INT_MIN;
    int window_width = 650;
    int window_height = 720;
};

class CTotalPowerItem : public IPluginItem
{
public:
    CTotalPowerItem(CXiaomiIoTPlugin& plugin) : m_plugin(plugin) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    int IsDrawResourceUsageGraph() const override { return 1; }
    float GetResourceUsageGraphValue() const override;
private:
    CXiaomiIoTPlugin& m_plugin;
};

class CDynamicPlugItem : public IPluginItem
{
public:
    CDynamicPlugItem(CXiaomiIoTPlugin& plugin, std::wstring device_id)
        : m_plugin(plugin), m_device_id(std::move(device_id)) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
private:
    CXiaomiIoTPlugin& m_plugin;
    std::wstring m_device_id;
    mutable std::wstring m_id_cache;
};

class CPlugStatusItem : public IPluginItem
{
public:
    CPlugStatusItem(CXiaomiIoTPlugin& plugin, std::wstring device_id)
        : m_plugin(plugin), m_device_id(std::move(device_id)) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
private:
    CXiaomiIoTPlugin& m_plugin;
    std::wstring m_device_id;
    mutable std::wstring m_id_cache;
};

class CGroupPowerItem : public IPluginItem
{
public:
    CGroupPowerItem(CXiaomiIoTPlugin& plugin, std::wstring group_id)
        : m_plugin(plugin), m_group_id(std::move(group_id)) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
private:
    CXiaomiIoTPlugin& m_plugin;
    std::wstring m_group_id;
    mutable std::wstring m_id_cache;
};

class CTemperatureItem : public IPluginItem
{
public:
    CTemperatureItem(CXiaomiIoTPlugin& plugin) : m_plugin(plugin) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
private:
    CXiaomiIoTPlugin& m_plugin;
};

class CHumidityItem : public IPluginItem
{
public:
    CHumidityItem(CXiaomiIoTPlugin& plugin) : m_plugin(plugin) {}
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
private:
    CXiaomiIoTPlugin& m_plugin;
};

class CXiaomiIoTPlugin : public ITMPlugin
{
public:
    CXiaomiIoTPlugin();
    ~CXiaomiIoTPlugin();
    static CXiaomiIoTPlugin& Instance();

    int GetAPIVersion() const override { return 7; }
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetTooltipInfo() override;
    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;

    std::wstring GetTotalPowerValueText();
    std::wstring GetPlugPowerValueText(const std::wstring& device_id);
    std::wstring GetPlugPowerValueText(size_t device_index);
    std::wstring GetPlugStatusValueText(const std::wstring& device_id);
    std::wstring GetGroupPowerValueText(const std::wstring& group_id);
    std::wstring GetTemperatureValueText();
    std::wstring GetHumidityValueText();
    std::wstring GetTotalPowerLabelText();
    std::wstring GetPlugLabelText(const std::wstring& device_id);
    std::wstring GetPlugLabelText(size_t device_index);
    std::wstring GetGroupLabelText(const std::wstring& group_id);
    std::wstring GetTemperatureLabelText();
    std::wstring GetHumidityLabelText();
    float GetPowerGraphValue() const;

    std::vector<PowerHistoryPoint> GetPowerHistorySnapshot(const std::wstring& series_id) const;
    std::vector<PowerHistoryPoint> GetPowerHistorySnapshot() const;
    std::vector<std::pair<std::wstring, std::wstring>> GetHistorySeries() const;
    void SetHistorySeries(const std::wstring& series_id);
    std::wstring GetHistoryStatisticsText() const;
    void ClearPowerHistory();

    PluginConfig GetConfig();
    void SaveConfig(const PluginConfig& config);
    void ResetDefaultConfig();
    void ResetCategoryDefaults(int category);
    bool ExportConfig(const std::wstring& path, std::wstring& error);
    bool ImportConfig(const std::wstring& path, std::wstring& error);
    void TogglePlugAsync(const std::wstring& device_id);
    ITrafficMonitor* GetApp() { return m_app; }

private:
    struct AlarmRuntime { bool active = false; std::time_t last_notice = 0; };
    std::wstring GetConfigFilePath() const;
    std::wstring GetHistoryFilePath() const;
    void LoadConfigFromFile();
    void SaveConfigToFile();
    void RebuildPlugItems();
    void StartBackgroundThreads();
    void BleWorkerThread();
    void PlugWorkerThread();
    void LoadPowerHistoryFromFile();
    void RecordPowerHistory(const std::unordered_map<std::wstring, double>& samples);
    void RecordPowerHistory(double total_power);
    void EvaluateAlarmsLocked(const PluginConfig& cfg);
    void NotifyAlarmLocked(const std::wstring& key, bool active, const std::wstring& text);
    int EffectiveRefreshMs(const PluginConfig& cfg, const PlugDeviceConfig& dev) const;
    int EffectivePrecision(const PluginConfig& cfg, const PlugDeviceConfig& dev) const;
    double EffectivePowerLimit(const PluginConfig& cfg, const PlugDeviceConfig& dev) const;
    const PlugDeviceConfig* FindDeviceLocked(const std::wstring& device_id) const;
    PlugDeviceConfig* FindDeviceLocked(const std::wstring& device_id);
    void EnsureConfigDefaults(PluginConfig& config);

    CTotalPowerItem m_total_power_item;
    std::vector<std::unique_ptr<CDynamicPlugItem>> m_plug_items;
    std::vector<std::unique_ptr<CPlugStatusItem>> m_status_items;
    std::vector<std::unique_ptr<CGroupPowerItem>> m_group_items;
    CTemperatureItem m_temp_item;
    CHumidityItem m_humidity_item;
    ITrafficMonitor* m_app{ nullptr };
    mutable std::mutex m_data_mutex;
    std::atomic<bool> m_running{ false };
    std::thread m_ble_thread;
    std::thread m_plug_thread;
    bool m_loaded_app_config{ false };
    PluginConfig m_config;
    double m_total_power{ 0.0 };
    bool m_ble_online{ false };
    double m_ble_temperature{ 0.0 };
    double m_ble_humidity{ 0.0 };
    std::unordered_map<std::wstring, std::deque<PowerHistoryPoint>> m_power_history;
    std::time_t m_last_history_sample{ 0 };
    std::wstring m_selected_history_series = L"total";
    std::unordered_map<std::wstring, AlarmRuntime> m_alarm_runtime;
    bool m_alarm_latched{ false }; // retained for old configuration compatibility
    std::wstring m_tooltip_cache;
    std::wstring m_cmd_name_cache;
    static CXiaomiIoTPlugin s_instance;
};
