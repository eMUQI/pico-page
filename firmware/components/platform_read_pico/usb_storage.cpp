#include "pico_page/platform.h"
#include "usb_block_range.h"
#include <algorithm>
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "read_pico_board.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include <atomic>
#include <cstring>
#include <limits>

namespace {
sdmmc_card_t card{};
SemaphoreHandle_t io_lock = nullptr;
std::atomic<bool> accepting{false}, invalidated{false}, ejected{false};
bool host_started = false, usb_started = false;
std::uint8_t* sector = nullptr;
constexpr std::uint32_t sector_bytes = 512;

bool available() {
    if (!read_pico_sd_present()) invalidated.store(true);
    return accepting.load() && !invalidated.load() && !ejected.load();
}

int32_t transfer(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer,
                 uint32_t size, bool write) {
    if (lun || !io_lock || !buffer || size > INT32_MAX) return -1;
    const std::uint64_t start = std::uint64_t(lba) * sector_bytes + offset;
    if (!pico_page::usb_block_range(card.csd.capacity, lba, offset, size)) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }
    if (xSemaphoreTake(io_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return -1;
    bool ok = available();
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::uint32_t done = 0;
    // USB transfers may begin or end within a sector; preserve all other bytes.
    while (ok && done < size) {
        const auto position = start + done;
        const auto block = static_cast<std::uint32_t>(position / sector_bytes);
        const auto within = static_cast<std::uint32_t>(position % sector_bytes);
        const auto count = std::min(size - done, sector_bytes - within);
        ok = available();
        if (!ok) break;
        if (!write || within || count != sector_bytes)
            ok = sdmmc_read_sectors(&card, sector, block, 1) == ESP_OK;
        if (ok && write) {
            std::memcpy(sector + within, bytes + done, count);
            ok = sdmmc_write_sectors(&card, sector, block, 1) == ESP_OK;
        } else if (ok) {
            std::memcpy(bytes + done, sector + within, count);
        }
        done += count;
    }
    if (!ok) invalidated.store(true);
    xSemaphoreGive(io_lock);
    if (!ok) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, write ? 0x0c : 0x11, 0);
        return -1;
    }
    return static_cast<int32_t>(size);
}
}

namespace pico_page::platform {
bool start_usb_storage(std::string& error) {
    error.clear();
    if (host_started || usb_started) { error = "USB storage already initialized"; return false; }
    if (!release_card_for_usb(error)) return false;
    io_lock = xSemaphoreCreateMutex();
    sector = static_cast<std::uint8_t*>(heap_caps_malloc(sector_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!io_lock || !sector) { error = "USB storage allocation failed"; return false; }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    // Pins and 1-bit bus match the pinned Read Pico BSP.
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1; slot.clk = GPIO_NUM_38; slot.cmd = GPIO_NUM_42; slot.d0 = GPIO_NUM_44;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
    slot.cd = slot.wp = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    auto result = sdmmc_host_init();
    host_started = result == ESP_OK;
    if (result == ESP_OK) result = sdmmc_host_init_slot(host.slot, &slot);
    if (result == ESP_OK) result = sdmmc_card_init(&host, &card);
    if (result == ESP_OK && card.csd.sector_size != sector_bytes) result = ESP_ERR_NOT_SUPPORTED;
    if (result != ESP_OK) { error = esp_err_to_name(result); return false; }
    invalidated.store(false); ejected.store(false); accepting.store(true);
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    static const char* strings[] = {"\x09\x04", "Pico Page", "Pico Page TF Card", "PICO-PAGE-TF", "TF Card"};
    config.descriptor.string = strings;
    config.descriptor.string_count = 5;
    result = tinyusb_driver_install(&config);
    usb_started = result == ESP_OK;
    if (!usb_started) { accepting.store(false); error = esp_err_to_name(result); }
    return usb_started;
}

bool stop_usb_storage(std::string& error) {
    error.clear();
    accepting.store(false);
    if (io_lock) {
        if (xSemaphoreTake(io_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
            error = "USB storage I/O still busy";
            return false;
        }
        xSemaphoreGive(io_lock);
    }
    if (usb_started) {
        tud_disconnect();
        const auto result = tinyusb_driver_uninstall();
        if (result != ESP_OK) { error = esp_err_to_name(result); return false; }
        usb_started = false;
    }
    if (host_started) {
        const auto result = sdmmc_host_deinit();
        if (result != ESP_OK) { error = esp_err_to_name(result); return false; }
        host_started = false;
    }
    if (io_lock) { vSemaphoreDelete(io_lock); io_lock = nullptr; }
    heap_caps_free(sector); sector = nullptr;
    return true;
}
}

extern "C" {
// Ownership is explicit; attach, detach and SCSI eject never mount local FAT.
void msc_storage_mount_to_usb(void) {}
void msc_storage_mount_to_app(void) {}
void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4]) {
    std::memcpy(vendor, "PicoPage", 8);
    std::memcpy(product, "TF Card         ", 16);
    std::memcpy(revision, "1.0 ", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!lun && available()) return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
    return false;
}
void tud_msc_capacity_cb(uint8_t, uint32_t* count, uint16_t* size) {
    *count = card.csd.capacity; *size = sector_bytes;
}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t, bool start, bool load_eject) {
    if (lun) return false;
    if (load_eject) ejected.store(!start);
    return true;
}
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t size) {
    return transfer(lun, lba, offset, buffer, size, false);
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t size) {
    return transfer(lun, lba, offset, buffer, size, true);
}
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void*, uint16_t) {
    // All writes are synchronous, so SYNCHRONIZE CACHE has no pending software data.
    if (command[0] == 0x35 || command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) return 0;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}
}
