#include "usb_storage_stubs.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

UsbSerialJtagRegisters USB_SERIAL_JTAG{};
RtcRegisters RTCCNTL{};
namespace {
esp_err_t install_result = ESP_OK, uninstall_result = ESP_OK;
bool stack_initialized = false, initialize_on_failure = false, io_busy = false;
bool card_release_ok = true;
unsigned phy_writes = 0, failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << '\n';
        ++failures;
    }
}

void prepare(unsigned original_phy = 1) {
    install_result = uninstall_result = ESP_OK;
    stack_initialized = initialize_on_failure = io_busy = false;
    card_release_ok = true;
    phy_writes = 0;
    USB_SERIAL_JTAG.conf0 = {bool(original_phy & 2), bool(original_phy & 1)};
    RTCCNTL.usb_conf = {bool(original_phy & 4), bool(original_phy & 8)};
}

bool console_restored(unsigned original_phy = 1) {
    return USB_SERIAL_JTAG.conf0.phy_sel == bool(original_phy & 2) &&
           USB_SERIAL_JTAG.conf0.usb_pad_enable == bool(original_phy & 1) &&
           RTCCNTL.usb_conf.sw_hw_usb_phy_sel == bool(original_phy & 4) &&
           RTCCNTL.usb_conf.sw_usb_phy_sel == bool(original_phy & 8);
}
}

const char* esp_err_to_name(esp_err_t) { return "injected USB error"; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return std::malloc(1); }
int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return io_busy ? 0 : pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t) {}
void vSemaphoreDelete(SemaphoreHandle_t mutex) { std::free(mutex); }
void* heap_caps_malloc(std::size_t bytes, unsigned) { return std::malloc(bytes); }
void heap_caps_free(void* allocation) { std::free(allocation); }
esp_err_t sdmmc_host_init() { return ESP_OK; }
esp_err_t sdmmc_host_init_slot(int, const sdmmc_slot_config_t*) { return ESP_OK; }
esp_err_t sdmmc_card_init(const sdmmc_host_t*, sdmmc_card_t*) { return ESP_OK; }
esp_err_t sdmmc_host_deinit() { return ESP_OK; }
esp_err_t sdmmc_read_sectors(sdmmc_card_t*, void*, std::uint64_t, std::size_t) { return ESP_OK; }
esp_err_t sdmmc_write_sectors(sdmmc_card_t*, const void*, std::uint64_t, std::size_t) { return ESP_OK; }
bool read_pico_sd_present() { return true; }
bool pico_page::platform::release_card_for_usb(std::string&) { return card_release_ok; }

esp_err_t tinyusb_driver_install(const tinyusb_config_t*) {
    RTCCNTL.usb_conf = {true, true};
    stack_initialized = install_result == ESP_OK || initialize_on_failure;
    return install_result;
}

esp_err_t tinyusb_driver_uninstall() {
    if (uninstall_result == ESP_OK) stack_initialized = false;
    return uninstall_result;
}

bool tud_inited() { return stack_initialized; }
void tud_disconnect() {}
void tud_msc_set_sense(std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t) {}
bool usb_serial_jtag_ll_phy_is_pad_enabled() { return USB_SERIAL_JTAG.conf0.usb_pad_enable; }
void usb_serial_jtag_ll_phy_enable_external(bool external) {
    assert(!stack_initialized);
    USB_SERIAL_JTAG.conf0.phy_sel = external;
    RTCCNTL.usb_conf = {true, external};
    ++phy_writes;
}
void usb_serial_jtag_ll_phy_enable_pad(bool enabled) {
    assert(!stack_initialized);
    USB_SERIAL_JTAG.conf0.usb_pad_enable = enabled;
    ++phy_writes;
}

int main() {
    using namespace pico_page::platform;
    std::string error;
    for (unsigned original_phy = 0; original_phy < 16; ++original_phy) {
        for (const auto failure : {ESP_ERR_NO_MEM, ESP_ERR_NOT_FINISHED}) {
            prepare(original_phy);
            install_result = failure;
            check(!start_usb_storage(error), "installation failure is reported");
            check(console_restored(original_phy), "installation failure restores original console PHY");
            check(stop_usb_storage(error), "failed installation resources can be released");
        }

        prepare(original_phy);
        check(start_usb_storage(error), "storage starts");
        check(RTCCNTL.usb_conf.sw_usb_phy_sel, "storage retains OTG ownership");
        check(!start_usb_storage(error), "duplicate start is refused");
        check(stop_usb_storage(error), "storage stops");
        check(console_restored(original_phy), "successful stop restores original console PHY");
        const auto writes_after_stop = phy_writes;
        check(stop_usb_storage(error) && phy_writes == writes_after_stop, "repeated stop leaves PHY unchanged");
    }

    for (const auto failure : {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE}) {
        prepare();
        install_result = failure;
        check(!start_usb_storage(error), "startup ownership failure is reported");
        check(phy_writes == 0 && RTCCNTL.usb_conf.sw_usb_phy_sel, "startup failure does not reclaim unknown PHY ownership");
        check(stop_usb_storage(error), "startup failure resources can be released for restart");
        check(phy_writes == 0, "startup failure cleanup does not reclaim unknown PHY ownership");
    }

    prepare();
    install_result = ESP_ERR_NO_MEM;
    initialize_on_failure = true;
    check(!start_usb_storage(error), "initialized stack failure is reported");
    check(phy_writes == 0, "initialized stack retains PHY ownership after failure");
    check(stop_usb_storage(error), "failed startup can proceed to restart");
    check(phy_writes == 0, "initialized stack is not reclaimed during failure cleanup");

    prepare();
    check(start_usb_storage(error), "storage starts before teardown failure");
    io_busy = true;
    check(!stop_usb_storage(error) && phy_writes == 0, "busy I/O retains PHY ownership");
    io_busy = false;
    uninstall_result = ESP_ERR_INVALID_STATE;
    check(!stop_usb_storage(error) && phy_writes == 0, "uninstall failure retains PHY ownership");
    uninstall_result = ESP_OK;
    check(stop_usb_storage(error) && console_restored(), "successful teardown retry restores console");

    prepare(0);
    card_release_ok = false;
    check(!start_usb_storage(error), "card handoff failure is reported");
    check(stop_usb_storage(error) && phy_writes == 0, "failure before PHY handoff preserves console");
    if (failures) return 1;
    std::cout << "PASS USB storage PHY recovery and failure ownership\n";
}
