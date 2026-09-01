#include "pch.h"
#include "ControllerLinkTask.h"
#include "ControllerLinkTask.g.cpp"

using namespace std::chrono_literals;
using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::AppService;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace
{
    guid const control_service{ L"6f9d1a54-2c31-4b8e-9f07-1d3a5c7e9b20" };
    guid const report_map = BluetoothUuidHelper::FromShortId(0x2A4B);
    guid const report_characteristic = BluetoothUuidHelper::FromShortId(0x2A4D);
    guid const hid_information = BluetoothUuidHelper::FromShortId(0x2A4A);
    guid const hid_control_point = BluetoothUuidHelper::FromShortId(0x2A4C);
    guid const protocol_mode = BluetoothUuidHelper::FromShortId(0x2A4E);
    guid const report_reference = BluetoothUuidHelper::FromShortId(0x2908);

    template <typename Range>
    IBuffer buffer(Range const& bytes)
    {
        DataWriter writer;
        writer.WriteBytes(array_view<uint8_t const>(bytes));
        return writer.DetachBuffer();
    }

    void require_success(BluetoothError error, wchar_t const* operation)
    {
        if (error != BluetoothError::Success)
        {
            throw hresult_error(E_FAIL, hstring(operation) + L" returned " + to_hstring(static_cast<int32_t>(error)));
        }
    }

}

namespace winrt::PicoSwitch::ControllerLink::Host::implementation
{
    void ControllerLinkTask::Run(IBackgroundTaskInstance const& task_instance)
    {
        task_deferral = task_instance.GetDeferral();
        task_instance.Canceled({ get_weak(), &ControllerLinkTask::OnCanceled });
        auto details = task_instance.TriggerDetails().as<AppServiceTriggerDetails>();
        connection = details.AppServiceConnection();
        // backgroundtaskhost releases its activation reference after Run returns.
        // The connection therefore owns a strong reference to this task until
        // cancellation closes the connection and breaks the cycle.
        connection.RequestReceived(
            [strong = get_strong()](AppServiceConnection const& sender, AppServiceRequestReceivedEventArgs const& args)
            {
                strong->OnRequestReceived(sender, args);
            });
        Log(L"app-service activated execution=windowsApp/AppContainer");
        Log(L"descriptor=161 bytes sha256=f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054");
    }

    void ControllerLinkTask::OnCanceled(
        IBackgroundTaskInstance const&,
        BackgroundTaskCancellationReason reason)
    {
        Log(L"task canceled reason=" + to_hstring(static_cast<int32_t>(reason)));
        try
        {
            PersistLogAsync().get();
        }
        catch (...)
        {
        }
        StopAdvertising();
        if (connection)
        {
            connection.Close();
            connection = nullptr;
        }
        if (task_deferral)
        {
            task_deferral.Complete();
            task_deferral = nullptr;
        }
    }

    fire_and_forget ControllerLinkTask::OnRequestReceived(
        AppServiceConnection const&,
        AppServiceRequestReceivedEventArgs const& args)
    {
        auto request_deferral = args.GetDeferral();
        auto request = args.Request();
        ValueSet response;
        try
        {
            // Copy the WinRT request before the first suspension. The event args
            // parameter is a delegate-stack reference and must not be used after
            // an await resumes this fire-and-forget handler.
            auto message = request.Message();
            auto command_value = message.TryLookup(L"command");
            auto command = command_value ? unbox_value<hstring>(command_value) : hstring{};
            Log(L"request command=" + command);

            if (command == L"hello")
            {
                response.Insert(L"ok", box_value(true));
                response.Insert(L"execution", box_value(L"windowsApp/AppContainer"));
                response.Insert(L"ipcVersion", box_value(1));
                response.Insert(L"bridgeContract", box_value(4));
                response.Insert(L"descriptorBytes", box_value(161));
                response.Insert(L"descriptorSha256", box_value(L"f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054"));
            }
            else if (command == L"start")
            {
                auto started = co_await StartAdvertisingAsync();
                response.Insert(L"ok", box_value(started));
                response.Insert(L"status", box_value(started ? L"started" : L"aborted"));
            }
            else if (command == L"stop")
            {
                StopAdvertising();
                response.Insert(L"ok", box_value(true));
                response.Insert(L"status", box_value(L"stopped"));
            }
            else
            {
                response.Insert(L"ok", box_value(false));
                response.Insert(L"error", box_value(L"unknown command"));
            }
        }
        catch (hresult_error const& error)
        {
            Log(L"request failed hresult=" + to_hstring(error.code()) + L" message=" + error.message());
            response.Insert(L"ok", box_value(false));
            response.Insert(L"error", box_value(error.message()));
        }

        co_await PersistLogAsync();
        co_await request.SendResponseAsync(response);
        request_deferral.Complete();
    }

    IAsyncOperation<bool> ControllerLinkTask::StartAdvertisingAsync()
    {
        {
            std::scoped_lock lock(state_gate);
            if (provider)
            {
                auto status = provider.AdvertisementStatus();
                co_return status == GattServiceProviderAdvertisementStatus::Started ||
                    status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData;
            }
            if (starting)
            {
                co_return false;
            }
            starting = true;
        }

        try
        {
            Log(L"start requested service=6f9d1a54-2c31-4b8e-9f07-1d3a5c7e9b20");
            auto provider_result = co_await GattServiceProvider::CreateAsync(control_service);
            Log(L"CreateAsync=" + to_hstring(static_cast<int32_t>(provider_result.Error())));
            require_success(provider_result.Error(), L"GattServiceProvider::CreateAsync");
            auto new_provider = provider_result.ServiceProvider();
            auto const protection = GattProtectionLevel::EncryptionRequired;

            GattLocalCharacteristicParameters report_map_parameters;
            report_map_parameters.CharacteristicProperties(GattCharacteristicProperties::Read);
            report_map_parameters.ReadProtectionLevel(protection);
            report_map_parameters.StaticValue(buffer(array_view<uint8_t const>(
                ANDROID_CONTROLLER_V2_HID_DESCRIPTOR,
                ANDROID_CONTROLLER_V2_HID_DESCRIPTOR + sizeof(ANDROID_CONTROLLER_V2_HID_DESCRIPTOR))));
            auto report_map_result = co_await new_provider.Service().CreateCharacteristicAsync(report_map, report_map_parameters);
            require_success(report_map_result.Error(), L"CreateCharacteristicAsync(report map)");
            auto report_map_value = report_map_result.Characteristic();

            GattLocalCharacteristicParameters input_parameters;
            input_parameters.CharacteristicProperties(GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify);
            input_parameters.ReadProtectionLevel(protection);
            auto input_result = co_await new_provider.Service().CreateCharacteristicAsync(report_characteristic, input_parameters);
            require_success(input_result.Error(), L"CreateCharacteristicAsync(input report)");
            auto new_input_report = input_result.Characteristic();

            GattLocalCharacteristicParameters output_parameters;
            output_parameters.CharacteristicProperties(
                GattCharacteristicProperties::Read |
                GattCharacteristicProperties::Write |
                GattCharacteristicProperties::WriteWithoutResponse);
            output_parameters.ReadProtectionLevel(protection);
            output_parameters.WriteProtectionLevel(protection);
            auto output_result = co_await new_provider.Service().CreateCharacteristicAsync(report_characteristic, output_parameters);
            require_success(output_result.Error(), L"CreateCharacteristicAsync(output report)");
            auto new_output_report = output_result.Characteristic();

            GattLocalDescriptorParameters input_reference_parameters;
            input_reference_parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
            input_reference_parameters.StaticValue(buffer(std::array<uint8_t, 2>{ 0x01, 0x01 }));
            auto input_reference_result = co_await new_input_report.CreateDescriptorAsync(report_reference, input_reference_parameters);
            require_success(input_reference_result.Error(), L"CreateDescriptorAsync(input 0x2908)");

            GattLocalDescriptorParameters output_reference_parameters;
            output_reference_parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
            output_reference_parameters.StaticValue(buffer(std::array<uint8_t, 2>{ 0x02, 0x02 }));
            auto output_reference_result = co_await new_output_report.CreateDescriptorAsync(report_reference, output_reference_parameters);
            require_success(output_reference_result.Error(), L"CreateDescriptorAsync(output 0x2908)");

            GattLocalCharacteristicParameters hid_info_parameters;
            hid_info_parameters.CharacteristicProperties(GattCharacteristicProperties::Read);
            hid_info_parameters.ReadProtectionLevel(protection);
            std::array<uint8_t, 4> const hid_info_value{ 0x11, 0x01, 0x00, 0x02 };
            hid_info_parameters.StaticValue(buffer(hid_info_value));
            auto hid_info_result = co_await new_provider.Service().CreateCharacteristicAsync(hid_information, hid_info_parameters);
            require_success(hid_info_result.Error(), L"CreateCharacteristicAsync(HID information)");
            auto hid_info_value_characteristic = hid_info_result.Characteristic();

            GattLocalCharacteristicParameters control_point_parameters;
            control_point_parameters.CharacteristicProperties(GattCharacteristicProperties::WriteWithoutResponse);
            auto control_point_result = co_await new_provider.Service().CreateCharacteristicAsync(hid_control_point, control_point_parameters);
            require_success(control_point_result.Error(), L"CreateCharacteristicAsync(control point)");
            auto control_point_value = control_point_result.Characteristic();

            GattLocalCharacteristicParameters protocol_parameters;
            protocol_parameters.CharacteristicProperties(GattCharacteristicProperties::Read | GattCharacteristicProperties::WriteWithoutResponse);
            std::array<uint8_t, 1> const protocol_value{ 0x01 };
            protocol_parameters.StaticValue(buffer(protocol_value));
            auto protocol_result = co_await new_provider.Service().CreateCharacteristicAsync(protocol_mode, protocol_parameters);
            require_success(protocol_result.Error(), L"CreateCharacteristicAsync(protocol mode)");
            auto protocol_value_characteristic = protocol_result.Characteristic();

            advertisement_token = new_provider.AdvertisementStatusChanged(
                [weak = get_weak()](GattServiceProvider const& sender, GattServiceProviderAdvertisementStatusChangedEventArgs const& event_args)
                {
                    if (auto self = weak.get())
                    {
                        self->Log(L"AdvertisementStatus=" + to_hstring(static_cast<int32_t>(sender.AdvertisementStatus())) +
                            L" Error=" + to_hstring(static_cast<int32_t>(event_args.Error())));
                    }
                });

            GattServiceProviderAdvertisingParameters advertising;
            advertising.IsConnectable(true);
            advertising.IsDiscoverable(true);
            new_provider.StartAdvertising(advertising);
            Log(L"StartAdvertising returned IsConnectable=true IsDiscoverable=true");

            auto const deadline = std::chrono::steady_clock::now() + 8s;
            bool started = false;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto const status = new_provider.AdvertisementStatus();
                if (status == GattServiceProviderAdvertisementStatus::Started ||
                    status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData)
                {
                    started = true;
                    break;
                }
                co_await resume_after(25ms);
            }

            Log(L"settled=" + to_hstring(static_cast<int32_t>(new_provider.AdvertisementStatus())) + L" started=" + to_hstring(started));
            {
                std::scoped_lock lock(state_gate);
                starting = false;
                if (started)
                {
                    provider = new_provider;
                    input_report = new_input_report;
                    output_report = new_output_report;
                }
            }
            if (!started)
            {
                new_provider.StopAdvertising();
                new_provider.AdvertisementStatusChanged(advertisement_token);
            }
            co_return started;
        }
        catch (...)
        {
            std::scoped_lock lock(state_gate);
            starting = false;
            throw;
        }
    }

    void ControllerLinkTask::StopAdvertising()
    {
        GattServiceProvider old_provider{ nullptr };
        {
            std::scoped_lock lock(state_gate);
            old_provider = provider;
            provider = nullptr;
            input_report = nullptr;
            output_report = nullptr;
            starting = false;
        }
        if (old_provider)
        {
            old_provider.StopAdvertising();
            old_provider.AdvertisementStatusChanged(advertisement_token);
            Log(L"advertising stopped");
        }
    }

    void ControllerLinkTask::Log(hstring const& message)
    {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started_at).count() / 1000.0;
        wchar_t prefix[96]{};
        swprintf_s(prefix, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u [t+%.3fms] ",
            local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
            local.wSecond, local.wMilliseconds, elapsed);
        std::scoped_lock lock(state_gate);
        log_lines.push_back(hstring(prefix) + message);
    }

    IAsyncAction ControllerLinkTask::PersistLogAsync()
    {
        std::vector<hstring> snapshot;
        {
            std::scoped_lock lock(state_gate);
            snapshot = log_lines;
        }
        hstring text;
        for (auto const& line : snapshot)
        {
            text = text + line + L"\r\n";
        }
        auto file = co_await ApplicationData::Current().LocalFolder().CreateFileAsync(
            L"controller-link-host.log", CreationCollisionOption::ReplaceExisting);
        co_await FileIO::WriteTextAsync(file, text);
    }
}
