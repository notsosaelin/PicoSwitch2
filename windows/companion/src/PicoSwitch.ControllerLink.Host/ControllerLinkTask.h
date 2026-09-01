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
        Windows::Foundation::IAsyncOperation<bool> StartAdvertisingAsync();
        void StopAdvertising();
        Windows::Foundation::IAsyncAction PersistLogAsync();
        void Log(winrt::hstring const& message);

        Windows::ApplicationModel::Background::BackgroundTaskDeferral task_deferral{ nullptr };
        Windows::ApplicationModel::AppService::AppServiceConnection connection{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProvider provider{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic input_report{ nullptr };
        Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic output_report{ nullptr };
        winrt::event_token advertisement_token{};
        std::mutex state_gate;
        std::vector<winrt::hstring> log_lines;
        std::chrono::steady_clock::time_point started_at{ std::chrono::steady_clock::now() };
        bool starting{ false };
    };
}

namespace winrt::PicoSwitch::ControllerLink::Host::factory_implementation
{
    struct ControllerLinkTask : ControllerLinkTaskT<ControllerLinkTask, implementation::ControllerLinkTask> {};
}
