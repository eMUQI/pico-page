#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_NO_MEM = 1;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 2;
constexpr esp_err_t ESP_ERR_TIMEOUT = 3;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 4;
constexpr esp_err_t ESP_ERR_NOT_FINISHED = 5;
const char* esp_err_to_name(esp_err_t result);

using SemaphoreHandle_t = void*;
using TickType_t = unsigned;
constexpr int pdTRUE = 1;
constexpr TickType_t pdMS_TO_TICKS(unsigned milliseconds) { return milliseconds; }
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
constexpr unsigned MALLOC_CAP_DMA = 1, MALLOC_CAP_INTERNAL = 2;
void* heap_caps_malloc(std::size_t bytes, unsigned capabilities);
void heap_caps_free(void* allocation);

constexpr int GPIO_NUM_NC = -1, GPIO_NUM_38 = 38, GPIO_NUM_42 = 42, GPIO_NUM_44 = 44;
constexpr int SDMMC_FREQ_HIGHSPEED = 40000, SDMMC_SLOT_FLAG_INTERNAL_PULLUP = 1;
struct sdmmc_host_t { int slot = 0, max_freq_khz = 0; };
struct sdmmc_slot_config_t { int width, clk, cmd, d0, d1, d2, d3, cd, wp, flags = 0; };
struct sdmmc_card_t { struct { std::uint32_t capacity = 16, sector_size = 512; } csd; };
inline sdmmc_host_t SDMMC_HOST_DEFAULT() { return {}; }
inline sdmmc_slot_config_t SDMMC_SLOT_CONFIG_DEFAULT() { return {}; }
esp_err_t sdmmc_host_init();
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t* config);
esp_err_t sdmmc_card_init(const sdmmc_host_t* host, sdmmc_card_t* card);
esp_err_t sdmmc_host_deinit();
esp_err_t sdmmc_read_sectors(sdmmc_card_t*, void*, std::uint64_t, std::size_t);
esp_err_t sdmmc_write_sectors(sdmmc_card_t*, const void*, std::uint64_t, std::size_t);
bool read_pico_sd_present();

struct tinyusb_config_t { struct { const char** string; unsigned string_count; } descriptor; };
inline tinyusb_config_t TINYUSB_DEFAULT_CONFIG() { return {}; }
esp_err_t tinyusb_driver_install(const tinyusb_config_t* config);
esp_err_t tinyusb_driver_uninstall();
bool tud_inited();
void tud_disconnect();
void tud_msc_set_sense(std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t);
constexpr unsigned SCSI_SENSE_ILLEGAL_REQUEST = 5, SCSI_SENSE_MEDIUM_ERROR = 3;
constexpr unsigned SCSI_SENSE_NOT_READY = 2, SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL = 0x1e;

struct UsbSerialJtagRegisters { struct { bool phy_sel = false, usb_pad_enable = true; } conf0; };
struct RtcRegisters { struct { bool sw_hw_usb_phy_sel = false, sw_usb_phy_sel = false; } usb_conf; };
extern UsbSerialJtagRegisters USB_SERIAL_JTAG;
extern RtcRegisters RTCCNTL;
bool usb_serial_jtag_ll_phy_is_pad_enabled();
void usb_serial_jtag_ll_phy_enable_external(bool external);
void usb_serial_jtag_ll_phy_enable_pad(bool enabled);

namespace pico_page::platform {
bool release_card_for_usb(std::string& error);
bool start_usb_storage(std::string& error);
bool stop_usb_storage(std::string& error);
}
