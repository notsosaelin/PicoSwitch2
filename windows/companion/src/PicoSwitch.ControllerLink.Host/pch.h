#pragma once

#include <windows.h>
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.ApplicationModel.Background.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "android_controller_hid.h"
