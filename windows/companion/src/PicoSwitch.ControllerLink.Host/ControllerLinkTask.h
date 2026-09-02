#pragma once

#include "ControllerLinkTask.g.h"

namespace winrt::PicoSwitch::ControllerLink::Host::implementation
{
    struct ControllerLinkTask : ControllerLinkTaskT<ControllerLinkTask>
    {
        ControllerLinkTask() = default;
        void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance const& task_instance);

    private:
        void OnCanceled(
            Windows::ApplicationModel::Background::IBackgroundTaskInstance const&,
            Windows::ApplicationModel::Background::BackgroundTaskCancellationReason reason);
        winrt::fire_and_forget OnRequestReceived(
            Windows::ApplicationModel::AppService::AppServiceConnection const&,
            Windows::ApplicationModel::AppService::AppServiceRequestReceivedEventArgs const& args);
        winrt::fire_and_forget OnInputRead(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattReadRequestedEventArgs const& args);
        winrt::fire_and_forget OnOutputRead(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattReadRequestedEventArgs const& args);
        winrt::fire_and_forget OnOutputWrite(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteRequestedEventArgs const& args);
        winrt::fire_and_forget OnControlWrite(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteRequestedEventArgs const& args);
        winrt::fire_and_forget OnProtocolWrite(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteRequestedEventArgs const& args);
        winrt::fire_and_forget OnProtocolRead(
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const&,
            Windows::Devices::Bluetooth::GenericAttributeProfile::GattReadRequestedEventArgs const& args);
        Windows::Foundation::IAsyncOperation<bool> StartAdvertisingAsync(
            winrt::hstring const& pipe_name,
            winrt::hstring const& challenge);
        void StopAdvertising();
        bool ConnectPipe(winrt::hstring const& pipe_name, winrt::hstring const& challenge);
        void ClosePipe();
        void PipeLoop();
        void WatchdogLoop();
        bool ReadPipeFrame(uint16_t& type, uint64_t& sequence, int64_t& timestamp, std::vector<uint8_t>& payload);
        bool WritePipeFrame(uint16_t type, std::span<uint8_t const> payload);
        void SendHostState(uint8_t state, std::string const& detail = {});
        void NotifyInput(std::span<uint8_t const> report);
        void NeutralizeInput();
        Windows::Foundation::IAsyncAction PersistLogAsync();
        void Log(winrt::hstring const& message);

        Windows::ApplicationModel::Background::BackgroundTaskDeferral task_deferral{ nullptr };
        Windows::ApplicationModel::AppService::AppServiceConnection connection{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProvider provider{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic input_report{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic output_report{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic control_point{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic protocol_mode{ nullptr };
        winrt::event_token advertisement_token{};
        winrt::event_token subscribed_token{};
        std::mutex state_gate;
        std::mutex pipe_write_gate;
        std::vector<winrt::hstring> log_lines;
        std::chrono::steady_clock::time_point started_at{ std::chrono::steady_clock::now() };
        std::array<uint8_t, 26> latest_input{
            128, 128, 128, 128, 0, 0, 0, 0, 0, 8,
        };
        std::array<uint8_t, 4> latest_output{};
        std::atomic<HANDLE> pipe_handle{ INVALID_HANDLE_VALUE };
        std::atomic<bool> pipe_running{ false };
        std::atomic<int64_t> last_main_message_qpc{ 0 };
        std::atomic<uint64_t> pipe_sequence{ 0 };
        std::atomic<uint64_t> input_received{ 0 };
        std::atomic<uint64_t> input_notified{ 0 };
        std::atomic<uint64_t> input_rejected{ 0 };
        std::atomic<uint64_t> output_received{ 0 };
        std::atomic<uint8_t> protocol_mode_value{ 1 };
        bool starting{ false };
    };
}

namespace winrt::PicoSwitch::ControllerLink::Host::factory_implementation
{
    struct ControllerLinkTask : ControllerLinkTaskT<ControllerLinkTask, implementation::ControllerLinkTask> {};
}
