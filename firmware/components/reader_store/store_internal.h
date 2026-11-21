#pragma once

#include "pico_page/store.h"

namespace pico_page::store_internal {
/** Installs the shared UTF-8 parser options once, before any TOML parse. */
void configure_toml();
bool relative_path(const std::string& path);
StoreStatus safe_path(const std::string& root, const std::string& path);
StoreStatus read_file(const std::string& root, const std::string& path,
                      std::size_t limit, std::string& contents,
                      const std::function<bool(std::size_t)>& admit = {});
/** Publishes exact bytes with comparison, synced temporary verification and backup. */
StoreStatus commit_file(const std::string& root, const std::string& path,
                        const std::optional<std::string>& expected, const std::string& bytes,
                        std::size_t limit, std::string& error, bool preserve_backup = false);
}
