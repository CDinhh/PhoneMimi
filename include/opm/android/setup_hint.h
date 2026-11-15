#pragma once

// Which "how to connect" hint to show for phones plugged in by cable.
// Pure logic; the USB listing lives in usb_vid_pids() (Windows SetupAPI).

#include <cstdint>
#include <string>
#include <vector>

namespace opm::android {

enum class Brand { Other = 0, Samsung = 1, Xiaomi = 2, Oppo = 3 };
enum class SetupHint { None = 0, DebuggingOff = 1, Unauthorized = 2, IPhoneCable = 3 };

struct UsbId { uint16_t vid = 0, pid = 0; };
struct SetupState { SetupHint hint = SetupHint::None; Brand brand = Brand::Other; };

// "USB\VID_2717&PID_FF48\..." -> {0x2717, 0xFF48}; {0,0} if not a USB id.
UsbId parse_usb_id(const std::string& instance_id);
bool is_android_vendor(uint16_t vid);
bool is_iphone(UsbId id);
Brand brand_for(uint16_t vid);

// adb_usb_ready: adb lists a USB phone as "device".
// adb_unauthorized: adb lists a USB phone as "unauthorized".
SetupState decide_setup(const std::vector<UsbId>& usb, bool adb_usb_ready, bool adb_unauthorized);

// Present USB devices (Windows); empty elsewhere.
std::vector<UsbId> usb_ids_present();

} // namespace opm::android
