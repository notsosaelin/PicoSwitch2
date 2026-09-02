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
    guid const hid_service = BluetoothUuidHelper::FromShortId(0x1812);
    guid const report_map = BluetoothUuidHelper::FromShortId(0x2A4B);
    guid const report_characteristic = BluetoothUuidHelper::FromShortId(0x2A4D);
    guid const hid_information = BluetoothUuidHelper::FromShortId(0x2A4A);
    guid const hid_control_point = BluetoothUuidHelper::FromShortId(0x2A4C);
    guid const protocol_mode_uuid = BluetoothUuidHelper::FromShortId(0x2A4E);
    guid const report_reference = BluetoothUuidHelper::FromShortId(0x2908);

    constexpr uint32_t ipc_magic = 0x314C4350;
    constexpr uint16_t ipc_version = 1;
    constexpr size_t ipc_header_size = 28;
    constexpr uint32_t helper_build = 0x00010000;
    constexpr uint32_t bridge_contract = 4;
    constexpr uint32_t descriptor_bytes = 161;
    constexpr uint32_t input_report_bytes = 26;
    constexpr uint32_t output_report_bytes = 4;
    constexpr size_t maximum_payload = 512;
    constexpr int64_t orphan_timeout_ms = 3000;

    enum class message_type : uint16_t
    {
        host_hello = 1,
        main_hello = 2,
        input_report = 16,
        heartbeat = 17,
        stop = 18,
        host_state = 32,
        output_report = 33,
        diagnostics = 34,
    };

    enum class host_state : uint8_t
    {
        ready = 0,
        starting = 1,
        advertising = 2,
        waiting = 3,
        connected = 4,
        disconnected = 5,
        stopped = 6,
        error = 7,
    };

    constexpr std::array<uint8_t, 32> descriptor_sha256{
        0xf2, 0x73, 0x15, 0xbf, 0xdf, 0x48, 0xb7, 0xab,
        0x5f, 0x76, 0x33, 0x6f, 0x06, 0x5f, 0xa2, 0x7d,
        0x9e, 0x04, 0xa4, 0x5f, 0xdd, 0x17, 0xf9, 0x6e,
        0x4e, 0x75, 0x24, 0x73, 0xa6, 0x72, 0x50, 0x54,
    };

    template <typename Range>
    IBuffer buffer(Range const& bytes)
    {
        DataWriter writer;
        writer.WriteBytes(array_view<uint8_t const>(bytes));
        return writer.DetachBuffer();
    }

    std::vector<uint8_t> bytes_from(IBuffer const& value)
    {
        std::vector<uint8_t> bytes(value.Length());
        DataReader::FromBuffer(value).ReadBytes(bytes);
        return bytes;
    }

    void require_success(BluetoothError error, wchar_t const* operation)
    {
        if (error != BluetoothError::Success)
        {
            throw hresult_error(E_FAIL, hstring(operation) + L" returned " + to_hstring(static_cast<int32_t>(error)));
        }
    }

    bool write_all(HANDLE handle, uint8_t const* data, size_t size)
    {
        while (size > 0)
        {
            DWORD written = 0;
            auto const chunk = static_cast<DWORD>((std::min)(size, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
            {
                return false;
            }
            data += written;
            size -= written;
        }
        return true;
    }

    bool read_all(HANDLE handle, uint8_t* data, size_t size)
    {
        while (size > 0)
        {
            DWORD read = 0;
            auto const chunk = static_cast<DWORD>((std::min)(size, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!ReadFile(handle, data, chunk, &read, nullptr) || read == 0)
            {
                return false;
            }
            data += read;
            size -= read;
        }
        return true;
    }

    template <typename T>
    void put_le(std::span<uint8_t> output, size_t offset, T value)
    {
        static_assert(std::is_integral_v<T>);
        for (size_t index = 0; index < sizeof(T); ++index)
        {
            output[offset + index] = static_cast<uint8_t>(
                static_cast<std::make_unsigned_t<T>>(value) >> (index * 8));
        }
    }

    template <typename T>
    T get_le(std::span<uint8_t const> input, size_t offset)
    {
        static_assert(std::is_integral_v<T>);
        std::make_unsigned_t<T> value = 0;
        for (size_t index = 0; index < sizeof(T); ++index)
        {
            value |= static_cast<std::make_unsigned_t<T>>(input[offset + index]) << (index * 8);
        }
        return static_cast<T>(value);
    }

    int hex_nibble(wchar_t value)
    {
        if (value >= L'0' && value <= L'9') return value - L'0';
        if (value >= L'a' && value <= L'f') return value - L'a' + 10;
        if (value >= L'A' && value <= L'F') return value - L'A' + 10;
        return -1;
    }

    bool parse_challenge(hstring const& text, std::array<uint8_t, 32>& output)
    {
        if (text.size() != output.size() * 2) return false;
        for (size_t index = 0; index < output.size(); ++index)
        {
            auto const high = hex_nibble(text[static_cast<uint32_t>(index * 2)]);
            auto const low = hex_nibble(text[static_cast<uint32_t>((index * 2) + 1)]);
            if (high < 0 || low < 0) return false;
            output[index] = static_cast<uint8_t>((high << 4) | low);
        }
        return true;
    }

    int64_t qpc_now()
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    int64_t qpc_frequency()
    {
        static int64_t const result = []
        {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return result;
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
        auto const caller = details.CallerPackageFamilyName();
        auto const own_family = Package::Current().Id().FamilyName();
        if (caller != own_family)
        {
            Log(L"rejected app-service caller package=" + caller);
            connection.Close();
            connection = nullptr;
            task_deferral.Complete();
            task_deferral = nullptr;
            return;
        }
        connection.RequestReceived(
            [strong = get_strong()](AppServiceConnection const& sender, AppServiceRequestReceivedEventArgs const& args)
            {
                strong->OnRequestReceived(sender, args);
            });
        Log(L"app-service activated execution=windowsApp/AppContainer");
        Log(L"app-service caller authenticated as same package");
        Log(L"descriptor=161 bytes sha256=f27315bfdf48b7ab5f76336f065fa27d9e04a45fdd17f96e4e752473a6725054");
    }

    void ControllerLinkTask::OnCanceled(
        IBackgroundTaskInstance const&,
        BackgroundTaskCancellationReason reason)
    {
        Log(L"task canceled reason=" + to_hstring(static_cast<int32_t>(reason)));
        NeutralizeInput();
        StopAdvertising();
        ClosePipe();
        try { PersistLogAsync().get(); } catch (...) {}
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
                auto pipe_value = message.TryLookup(L"pipeName");
                auto challenge_value = message.TryLookup(L"challenge");
                if (!pipe_value || !challenge_value)
                {
                    throw hresult_invalid_argument(L"start requires pipeName and challenge");
                }
                auto started = co_await StartAdvertisingAsync(
                    unbox_value<hstring>(pipe_value),
                    unbox_value<hstring>(challenge_value));
                response.Insert(L"ok", box_value(started));
                response.Insert(L"status", box_value(started ? L"started" : L"aborted"));
            }
            else if (command == L"stop")
            {
                NeutralizeInput();
                StopAdvertising();
                ClosePipe();
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
            SendHostState(static_cast<uint8_t>(host_state::error), to_string(error.message()));
            response.Insert(L"ok", box_value(false));
            response.Insert(L"error", box_value(error.message()));
        }
        catch (std::exception const& error)
        {
            auto message = to_hstring(error.what());
            Log(L"request failed message=" + message);
            SendHostState(static_cast<uint8_t>(host_state::error), error.what());
            response.Insert(L"ok", box_value(false));
            response.Insert(L"error", box_value(message));
        }

        co_await PersistLogAsync();
        co_await request.SendResponseAsync(response);
        request_deferral.Complete();
    }

    IAsyncOperation<bool> ControllerLinkTask::StartAdvertisingAsync(
        hstring const& pipe_name,
        hstring const& challenge)
    {
        {
            std::scoped_lock lock(state_gate);
            if (provider)
            {
                auto status = provider.AdvertisementStatus();
                co_return status == GattServiceProviderAdvertisementStatus::Started ||
                    status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData;
            }
            if (starting) co_return false;
            starting = true;
        }

        try
        {
            if (!ConnectPipe(pipe_name, challenge))
            {
                throw hresult_error(E_ACCESSDENIED, L"authenticated same-package pipe handshake failed");
            }
            SendHostState(static_cast<uint8_t>(host_state::ready));
            SendHostState(static_cast<uint8_t>(host_state::starting));

            Log(L"start requested service=00001812-0000-1000-8000-00805f9b34fb");
            auto provider_result = co_await GattServiceProvider::CreateAsync(hid_service);
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
            hid_info_parameters.StaticValue(buffer(std::array<uint8_t, 4>{ 0x11, 0x01, 0x00, 0x02 }));
            auto hid_info_result = co_await new_provider.Service().CreateCharacteristicAsync(hid_information, hid_info_parameters);
            require_success(hid_info_result.Error(), L"CreateCharacteristicAsync(HID information)");

            GattLocalCharacteristicParameters control_parameters;
            control_parameters.CharacteristicProperties(GattCharacteristicProperties::WriteWithoutResponse);
            auto control_result = co_await new_provider.Service().CreateCharacteristicAsync(hid_control_point, control_parameters);
            require_success(control_result.Error(), L"CreateCharacteristicAsync(control point)");
            auto new_control_point = control_result.Characteristic();

            GattLocalCharacteristicParameters protocol_parameters;
            protocol_parameters.CharacteristicProperties(GattCharacteristicProperties::Read | GattCharacteristicProperties::WriteWithoutResponse);
            auto protocol_result = co_await new_provider.Service().CreateCharacteristicAsync(protocol_mode_uuid, protocol_parameters);
            require_success(protocol_result.Error(), L"CreateCharacteristicAsync(protocol mode)");
            auto new_protocol_mode = protocol_result.Characteristic();

            new_input_report.ReadRequested({ get_weak(), &ControllerLinkTask::OnInputRead });
            new_output_report.ReadRequested({ get_weak(), &ControllerLinkTask::OnOutputRead });
            new_output_report.WriteRequested({ get_weak(), &ControllerLinkTask::OnOutputWrite });
            new_control_point.WriteRequested({ get_weak(), &ControllerLinkTask::OnControlWrite });
            new_protocol_mode.ReadRequested({ get_weak(), &ControllerLinkTask::OnProtocolRead });
            new_protocol_mode.WriteRequested({ get_weak(), &ControllerLinkTask::OnProtocolWrite });
            subscribed_token = new_input_report.SubscribedClientsChanged(
                [weak = get_weak()](GattLocalCharacteristic const& sender, IInspectable const&)
                {
                    if (auto self = weak.get())
                    {
                        auto const clients = sender.SubscribedClients();
                        self->Log(L"input subscribers=" + to_hstring(clients.Size()));
                        if (clients.Size() == 0)
                        {
                            self->SendHostState(static_cast<uint8_t>(host_state::disconnected));
                            self->SendHostState(static_cast<uint8_t>(host_state::waiting));
                            return;
                        }
                        for (auto const& client : clients)
                        {
                            if (client.MaxNotificationSize() < input_report_bytes)
                            {
                                self->SendHostState(
                                    static_cast<uint8_t>(host_state::error),
                                    "connected client notification size is below 26 bytes");
                                return;
                            }
                        }
                        self->SendHostState(static_cast<uint8_t>(host_state::connected));
                    }
                });

            advertisement_token = new_provider.AdvertisementStatusChanged(
                [weak = get_weak()](GattServiceProvider const& sender, GattServiceProviderAdvertisementStatusChangedEventArgs const& event_args)
                {
                    if (auto self = weak.get())
                    {
                        auto const status = sender.AdvertisementStatus();
                        self->Log(L"AdvertisementStatus=" + to_hstring(static_cast<int32_t>(status)) +
                            L" Error=" + to_hstring(static_cast<int32_t>(event_args.Error())));
                        if (status == GattServiceProviderAdvertisementStatus::Aborted)
                        {
                            std::scoped_lock lock(self->state_gate);
                            if (!self->starting)
                            {
                                self->SendHostState(static_cast<uint8_t>(host_state::error), "advertising aborted at runtime");
                            }
                        }
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
                    control_point = new_control_point;
                    protocol_mode = new_protocol_mode;
                }
            }
            if (!started)
            {
                new_provider.StopAdvertising();
                new_provider.AdvertisementStatusChanged(advertisement_token);
                SendHostState(static_cast<uint8_t>(host_state::error), "advertising did not reach Started before deadline");
                ClosePipe();
                co_return false;
            }

            // NEGATIVE KNOWLEDGE — do not add a co-resident
            // BluetoothLEAdvertisementPublisher here to "force legacy PDUs".
            //
            // The theory was attractive: PicoSwitch2 is built without
            // ENABLE_LE_EXTENDED_ADVERTISING, so its central only ever receives
            // LEGACY advertising reports, and if Windows advertised this hosted
            // GATT service through an extended-PDU set the adapter could never
            // see it.
            //
            // BTHPORT HCIRAW ETW on 2026-09-02 (Intel AX210, driver 24.40.10.8)
            // falsified the premise. StartAdvertising programs TWO sets:
            //
            //   handle 1  props 0x0013  connectable + scannable + LEGACY PDUs
            //             ADV  = Flags, Complete 16-bit UUIDs 0x180A,0x1812
            //             SCAN_RSP = Complete Local Name
            //   handle 2  props 0x0001  connectable, extended PDUs
            //
            // Windows already emits exactly the legacy ADV_IND the adapter needs,
            // beside an extended set for modern centrals. A second publisher adds
            // only a THIRD set that is provably useless here: the WinRT publisher
            // has no IsConnectable (that property exists on the *received* args
            // and on GattServiceProviderAdvertisingParameters, not on the
            // publisher), so it can only emit ADV_NONCONN_IND — measured as
            // props 0x0010 — carrying no HID UUID. Nothing can connect to it and
            // the adapter's discovery predicate rejects it, while it still costs
            // radio airtime the connectable set needs.
            //
            // See docs/experiments/windows-hogp-legacy-advertising-2026-09-02.md.
            SendHostState(static_cast<uint8_t>(host_state::advertising));
            SendHostState(static_cast<uint8_t>(host_state::waiting));
            std::thread([strong = get_strong()] { strong->PipeLoop(); }).detach();
            std::thread([strong = get_strong()] { strong->WatchdogLoop(); }).detach();
            co_return true;
        }
        catch (...)
        {
            {
                std::scoped_lock lock(state_gate);
                starting = false;
            }
            ClosePipe();
            throw;
        }
    }

    fire_and_forget ControllerLinkTask::OnInputRead(
        GattLocalCharacteristic const&,
        GattReadRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            std::array<uint8_t, input_report_bytes> value{};
            {
                std::scoped_lock lock(state_gate);
                value = latest_input;
            }
            request.RespondWithValue(buffer(value));
        }
        deferral.Complete();
    }

    fire_and_forget ControllerLinkTask::OnOutputRead(
        GattLocalCharacteristic const&,
        GattReadRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            std::array<uint8_t, output_report_bytes> value{};
            {
                std::scoped_lock lock(state_gate);
                value = latest_output;
            }
            request.RespondWithValue(buffer(value));
        }
        deferral.Complete();
    }

    fire_and_forget ControllerLinkTask::OnOutputWrite(
        GattLocalCharacteristic const&,
        GattWriteRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            auto bytes = bytes_from(request.Value());
            if (bytes.size() == output_report_bytes + 1 && bytes[0] == 2)
            {
                bytes.erase(bytes.begin());
            }
            if (bytes.size() >= output_report_bytes)
            {
                bytes.resize(output_report_bytes);
                {
                    std::scoped_lock lock(state_gate);
                    std::copy(bytes.begin(), bytes.end(), latest_output.begin());
                }
                auto const count = output_received.fetch_add(1) + 1;
                WritePipeFrame(static_cast<uint16_t>(message_type::output_report), bytes);
                if (count == 1 || count % 128 == 0)
                {
                    Log(L"output report received count=" + to_hstring(count));
                }
            }
            else
            {
                Log(L"malformed output report bytes=" + to_hstring(bytes.size()));
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
        }
        deferral.Complete();
    }

    fire_and_forget ControllerLinkTask::OnControlWrite(
        GattLocalCharacteristic const&,
        GattWriteRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            auto bytes = bytes_from(request.Value());
            if (bytes.size() == 1)
            {
                Log(L"control point=" + to_hstring(bytes[0]));
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
        }
        deferral.Complete();
    }

    fire_and_forget ControllerLinkTask::OnProtocolRead(
        GattLocalCharacteristic const&,
        GattReadRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            request.RespondWithValue(buffer(std::array<uint8_t, 1>{ protocol_mode_value.load() }));
        }
        deferral.Complete();
    }

    fire_and_forget ControllerLinkTask::OnProtocolWrite(
        GattLocalCharacteristic const&,
        GattWriteRequestedEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto operation = args.GetRequestAsync();
        auto request = co_await operation;
        if (request)
        {
            auto bytes = bytes_from(request.Value());
            if (bytes.size() == 1 && bytes[0] <= 1)
            {
                protocol_mode_value.store(bytes[0]);
                Log(L"protocol mode=" + to_hstring(bytes[0]));
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
        }
        deferral.Complete();
    }

    bool ControllerLinkTask::ConnectPipe(hstring const& pipe_name, hstring const& challenge_text)
    {
        std::wstring_view const name(pipe_name);
        constexpr std::wstring_view prefix = L"LOCAL\\PicoSwitch.ControllerLink.";
        if (!name.starts_with(prefix) || name.size() <= prefix.size() ||
            name.find_first_of(L"/\\", prefix.size()) != std::wstring_view::npos)
        {
            return false;
        }
        std::array<uint8_t, 32> challenge{};
        if (!parse_challenge(challenge_text, challenge)) return false;

        std::wstring const path = L"\\\\.\\pipe\\" + std::wstring(name);
        using create_named_pipe_w = HANDLE(WINAPI*)(
            LPCWSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
        using connect_named_pipe = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED);
        auto const kernel = GetModuleHandleW(L"kernel32.dll");
        auto const create_pipe = reinterpret_cast<create_named_pipe_w>(
            GetProcAddress(kernel, "CreateNamedPipeW"));
        auto const connect_pipe = reinterpret_cast<connect_named_pipe>(
            GetProcAddress(kernel, "ConnectNamedPipe"));
        if (!create_pipe || !connect_pipe)
        {
            Log(L"named pipe server APIs are unavailable");
            return false;
        }

        HANDLE handle = create_pipe(
            path.c_str(),
            0x00000003u | 0x00080000u, // PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE
            0x00000000u | 0x00000008u, // PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS
            1,
            4096,
            4096,
            0,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            Log(L"CreateNamedPipeW failed error=" + to_hstring(static_cast<uint32_t>(GetLastError())));
            return false;
        }
        if (!connect_pipe(handle, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            Log(L"ConnectNamedPipe failed error=" + to_hstring(static_cast<uint32_t>(GetLastError())));
            CloseHandle(handle);
            return false;
        }
        pipe_handle.store(handle);
        pipe_running.store(true);
        last_main_message_qpc.store(qpc_now());

        std::array<uint8_t, 84> hello{};
        put_le<uint32_t>(hello, 0, helper_build);
        put_le<uint32_t>(hello, 4, bridge_contract);
        put_le<uint32_t>(hello, 8, descriptor_bytes);
        put_le<uint32_t>(hello, 12, input_report_bytes);
        put_le<uint32_t>(hello, 16, output_report_bytes);
        std::copy(descriptor_sha256.begin(), descriptor_sha256.end(), hello.begin() + 20);
        std::copy(challenge.begin(), challenge.end(), hello.begin() + 52);
        if (!WritePipeFrame(static_cast<uint16_t>(message_type::host_hello), hello))
        {
            ClosePipe();
            return false;
        }

        uint16_t type = 0;
        uint64_t sequence = 0;
        int64_t timestamp = 0;
        std::vector<uint8_t> payload;
        if (!ReadPipeFrame(type, sequence, timestamp, payload) ||
            type != static_cast<uint16_t>(message_type::main_hello) || !payload.empty())
        {
            ClosePipe();
            return false;
        }
        Log(L"same-package pipe handshake complete");
        return true;
    }

    bool ControllerLinkTask::ReadPipeFrame(
        uint16_t& type,
        uint64_t& sequence,
        int64_t& timestamp,
        std::vector<uint8_t>& payload)
    {
        auto const handle = pipe_handle.load();
        if (handle == INVALID_HANDLE_VALUE) return false;
        std::array<uint8_t, ipc_header_size> header{};
        if (!read_all(handle, header.data(), header.size())) return false;
        if (get_le<uint32_t>(header, 0) != ipc_magic || get_le<uint16_t>(header, 4) != ipc_version)
        {
            return false;
        }
        type = get_le<uint16_t>(header, 6);
        auto const length = get_le<uint32_t>(header, 8);
        if (length > maximum_payload) return false;
        sequence = get_le<uint64_t>(header, 12);
        timestamp = get_le<int64_t>(header, 20);
        payload.resize(length);
        return length == 0 || read_all(handle, payload.data(), payload.size());
    }

    bool ControllerLinkTask::WritePipeFrame(uint16_t type, std::span<uint8_t const> payload)
    {
        if (payload.size() > maximum_payload) return false;
        std::scoped_lock lock(pipe_write_gate);
        auto const handle = pipe_handle.load();
        if (handle == INVALID_HANDLE_VALUE) return false;

        std::array<uint8_t, ipc_header_size> header{};
        put_le<uint32_t>(header, 0, ipc_magic);
        put_le<uint16_t>(header, 4, ipc_version);
        put_le<uint16_t>(header, 6, type);
        put_le<uint32_t>(header, 8, static_cast<uint32_t>(payload.size()));
        put_le<uint64_t>(header, 12, pipe_sequence.fetch_add(1) + 1);
        put_le<int64_t>(header, 20, qpc_now());
        return write_all(handle, header.data(), header.size()) &&
            (payload.empty() || write_all(handle, payload.data(), payload.size()));
    }

    void ControllerLinkTask::SendHostState(uint8_t state, std::string const& detail)
    {
        std::vector<uint8_t> payload(1 + detail.size());
        payload[0] = state;
        std::copy(detail.begin(), detail.end(), payload.begin() + 1);
        WritePipeFrame(static_cast<uint16_t>(message_type::host_state), payload);
    }

    void ControllerLinkTask::PipeLoop()
    {
        init_apartment(apartment_type::multi_threaded);
        while (pipe_running.load())
        {
            uint16_t type = 0;
            uint64_t sequence = 0;
            int64_t timestamp = 0;
            std::vector<uint8_t> payload;
            if (!ReadPipeFrame(type, sequence, timestamp, payload)) break;
            last_main_message_qpc.store(qpc_now());
            switch (static_cast<message_type>(type))
            {
                case message_type::input_report:
                    if (payload.size() != input_report_bytes)
                    {
                        SendHostState(static_cast<uint8_t>(host_state::error), "malformed input report length");
                        continue;
                    }
                    input_received.fetch_add(1);
                    NotifyInput(payload);
                    break;
                case message_type::heartbeat:
                    if (!payload.empty()) pipe_running.store(false);
                    break;
                case message_type::stop:
                    NeutralizeInput();
                    StopAdvertising();
                    SendHostState(static_cast<uint8_t>(host_state::stopped));
                    pipe_running.store(false);
                    break;
                default:
                    SendHostState(static_cast<uint8_t>(host_state::error), "unexpected main-to-host message");
                    pipe_running.store(false);
                    break;
            }
        }

        if (provider)
        {
            NeutralizeInput();
            StopAdvertising();
        }
        ClosePipe();
        Log(L"pipe reader exited inputReceived=" + to_hstring(input_received.load()) +
            L" notified=" + to_hstring(input_notified.load()) +
            L" rejected=" + to_hstring(input_rejected.load()) +
            L" output=" + to_hstring(output_received.load()));
    }

    void ControllerLinkTask::WatchdogLoop()
    {
        init_apartment(apartment_type::multi_threaded);
        while (pipe_running.load())
        {
            std::this_thread::sleep_for(250ms);
            auto const elapsed_ms = (qpc_now() - last_main_message_qpc.load()) * 1000 / qpc_frequency();
            if (elapsed_ms <= orphan_timeout_ms) continue;
            Log(L"main heartbeat expired; neutralizing and stopping");
            NeutralizeInput();
            StopAdvertising();
            ClosePipe();
            break;
        }
    }

    void ControllerLinkTask::NotifyInput(std::span<uint8_t const> report)
    {
        GattLocalCharacteristic characteristic{ nullptr };
        {
            std::scoped_lock lock(state_gate);
            std::copy(report.begin(), report.end(), latest_input.begin());
            characteristic = input_report;
        }
        if (!characteristic || characteristic.SubscribedClients().Size() == 0) return;
        try
        {
            for (auto const& client : characteristic.SubscribedClients())
            {
                if (client.MaxNotificationSize() < input_report_bytes)
                {
                    input_rejected.fetch_add(1);
                    return;
                }
            }
            auto results = characteristic.NotifyValueAsync(buffer(report)).get();
            bool accepted = false;
            for (auto const& result : results)
            {
                if (result.Status() == GattCommunicationStatus::Success) accepted = true;
            }
            if (accepted) input_notified.fetch_add(1);
            else input_rejected.fetch_add(1);
        }
        catch (...)
        {
            input_rejected.fetch_add(1);
        }
    }

    void ControllerLinkTask::NeutralizeInput()
    {
        std::array<uint8_t, input_report_bytes> neutral{};
        neutral[0] = neutral[1] = neutral[2] = neutral[3] = 128;
        neutral[9] = 8;
        NotifyInput(neutral);
    }

    void ControllerLinkTask::ClosePipe()
    {
        pipe_running.store(false);
        std::scoped_lock lock(pipe_write_gate);
        auto const handle = pipe_handle.exchange(INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(handle, nullptr);
            CloseHandle(handle);
        }
    }

    void ControllerLinkTask::StopAdvertising()
    {
        GattServiceProvider old_provider{ nullptr };
        GattLocalCharacteristic old_input{ nullptr };
        {
            std::scoped_lock lock(state_gate);
            old_provider = provider;
            old_input = input_report;
            provider = nullptr;
            input_report = nullptr;
            output_report = nullptr;
            control_point = nullptr;
            protocol_mode = nullptr;
            starting = false;
        }
        if (old_input)
        {
            try { old_input.SubscribedClientsChanged(subscribed_token); } catch (...) {}
        }
        if (old_provider)
        {
            old_provider.StopAdvertising();
            try { old_provider.AdvertisementStatusChanged(advertisement_token); } catch (...) {}
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
        for (auto const& line : snapshot) text = text + line + L"\r\n";
        auto file = co_await ApplicationData::Current().LocalFolder().CreateFileAsync(
            L"controller-link-host.log", CreationCollisionOption::ReplaceExisting);
        co_await FileIO::WriteTextAsync(file, text);
    }
}
