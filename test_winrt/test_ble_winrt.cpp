#include <iostream>
#include <windows.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

int main()
{
    init_apartment(apartment_type::multi_threaded);
    std::cout << "Testing C++/WinRT BLE..." << std::endl;

    uint64_t target_mac = 0xA4C138D5C220ULL; // A4:C1:38:D5:C2:20
    guid service_uuid("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
    guid char_uuid("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");

    try
    {
        std::cout << "Connecting to BLE device MAC: A4:C1:38:D5:C2:20..." << std::endl;
        auto device = BluetoothLEDevice::FromBluetoothAddressAsync(target_mac).get();
        if (!device)
        {
            std::cout << "Device not found!" << std::endl;
            return 1;
        }

        std::cout << "Device connected: " << winrt::to_string(device.Name()) << std::endl;
        auto services_result = device.GetGattServicesForUuidAsync(service_uuid).get();
        if (services_result.Status() == GattCommunicationStatus::Success)
        {
            auto services = services_result.Services();
            if (services.Size() > 0)
            {
                auto service = services.GetAt(0);
                auto char_result = service.GetCharacteristicsForUuidAsync(char_uuid).get();
                if (char_result.Status() == GattCommunicationStatus::Success)
                {
                    auto chars = char_result.Characteristics();
                    if (chars.Size() > 0)
                    {
                        auto gatt_char = chars.GetAt(0);
                        auto read_result = gatt_char.ReadValueAsync().get();
                        if (read_result.Status() == GattCommunicationStatus::Success)
                        {
                            IBuffer buffer = read_result.Value();
                            DataReader reader = DataReader::FromBuffer(buffer);
                            uint8_t b1 = reader.ReadByte();
                            uint8_t b2 = reader.ReadByte();
                            uint8_t b3 = reader.ReadByte();

                            int16_t temp_raw = (int16_t)(b1 | (b2 << 8));
                            double temp = temp_raw / 100.0;
                            double hum = b3;

                            std::cout << "SUCCESS! Temp: " << temp << " C, Humidity: " << hum << " %" << std::endl;
                            return 0;
                        }
                    }
                }
            }
        }
        std::cout << "GATT Read failed!" << std::endl;
    }
    catch (const winrt::hresult_error& ex)
    {
        std::cout << "Error: " << winrt::to_string(ex.message()) << std::endl;
    }
    return 1;
}
