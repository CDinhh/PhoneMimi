#include <opm/android/setup_hint.h>

#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#pragma comment(lib, "setupapi.lib")
#endif

namespace opm::android {

UsbId parse_usb_id(const std::string& id) {
    std::string u = id;
    for (auto& c : u) c = char(std::toupper((unsigned char)c));
    auto v = u.find("VID_"), p = u.find("PID_");
    if (u.rfind("USB\\", 0) != 0 || v == std::string::npos || p == std::string::npos) return {};
    UsbId r;
    r.vid = uint16_t(std::strtoul(u.substr(v + 4, 4).c_str(), nullptr, 16));
    r.pid = uint16_t(std::strtoul(u.substr(p + 4, 4).c_str(), nullptr, 16));
    return r;
}

bool is_android_vendor(uint16_t vid) {
    switch (vid) {
        case 0x04E8:  // Samsung
        case 0x2717:  // Xiaomi
        case 0x22D9:  // OPPO / realme
        case 0x2A70:  // OnePlus
        case 0x18D1:  // Google
        case 0x2D95:  // vivo
        case 0x12D1:  // Huawei
        case 0x22B8:  // Motorola
        case 0x0FCE:  // Sony
        case 0x1004:  // LG
        case 0x0B05:  // ASUS
            return true;
        default:
            return false;
    }
}

bool is_iphone(UsbId id) { return id.vid == 0x05AC && id.pid >= 0x1290 && id.pid <= 0x12AF; }

Brand brand_for(uint16_t vid) {
    switch (vid) {
        case 0x04E8: return Brand::Samsung;
        case 0x2717: return Brand::Xiaomi;
        case 0x22D9: return Brand::Oppo;
        default: return Brand::Other;
    }
}

SetupState decide_setup(const std::vector<UsbId>& usb, bool adb_usb_ready, bool adb_unauthorized) {
    if (adb_unauthorized) {
        for (auto& d : usb)
            if (is_android_vendor(d.vid)) return {SetupHint::Unauthorized, brand_for(d.vid)};
        return {SetupHint::Unauthorized, Brand::Other};
    }
    if (!adb_usb_ready)
        for (auto& d : usb)
            if (is_android_vendor(d.vid)) return {SetupHint::DebuggingOff, brand_for(d.vid)};
    for (auto& d : usb)
        if (is_iphone(d)) return {SetupHint::IPhoneCable, Brand::Other};
    return {};
}

std::vector<UsbId> usb_ids_present() {
    std::vector<UsbId> out;
#ifdef _WIN32
    HDEVINFO set = SetupDiGetClassDevsA(nullptr, "USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    char buf[512];
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        if (!SetupDiGetDeviceInstanceIdA(set, &info, buf, sizeof(buf), nullptr)) continue;
        UsbId id = parse_usb_id(buf);
        if (id.vid) out.push_back(id);
    }
    SetupDiDestroyDeviceInfoList(set);
#endif
    return out;
}

} // namespace opm::android
