#pragma once
#include <cstdint>

struct UsbSerialJtagRegisters {
    union {
        struct {
            std::uint32_t dp_pullup : 1;
            std::uint32_t dm_pullup : 1;
            std::uint32_t dp_pulldown : 1;
            std::uint32_t dm_pulldown : 1;
            std::uint32_t pad_pull_override : 1;
            std::uint32_t usb_pad_enable : 1;
            std::uint32_t unrelated : 26;
        };
        std::uint32_t val;
    } conf0;
};
extern UsbSerialJtagRegisters USB_SERIAL_JTAG;
struct usb_serial_jtag_pull_override_vals_t { bool dp_pu, dm_pu, dp_pd, dm_pd; };
bool usb_serial_jtag_ll_phy_is_pad_enabled();
void usb_serial_jtag_ll_phy_enable_pad(bool enable);
void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t* values);
void usb_serial_jtag_ll_phy_disable_pull_override();
