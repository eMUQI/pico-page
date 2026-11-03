# Product identity for both this firmware and its host regression/simulator build.
set(PICO_PAGE_BOARD_ID "read_pico")
set(PICO_PAGE_MANIFEST_URL "https://picopage.wulu.zone/firmware/manifest.json")
# Matches the shipped partition layout; the OTA backend also checks the actual slot.
set(PICO_PAGE_UPDATE_SLOT_BYTES 4194304)
