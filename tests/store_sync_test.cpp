#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static fs::path failing_directory;
static unsigned directory_syncs = 0;
static unsigned fail_from_sync = 0;
static bool fail_file_sync = false;

// Fault only fsync; all reads, writes, verification and publication use real files.
static int checked_fsync(int fd) {
    struct stat descriptor{}, selected{};
    assert(fstat(fd, &descriptor) == 0);
    if (fail_file_sync && S_ISREG(descriptor.st_mode)) { errno = EIO; return -1; }
    if (!failing_directory.empty() && stat(failing_directory.c_str(), &selected) == 0 &&
        selected.st_dev == descriptor.st_dev && selected.st_ino == descriptor.st_ino) {
        ++directory_syncs;
        if (fail_from_sync && directory_syncs >= fail_from_sync) { errno = EIO; return -1; }
    }
    return fsync(fd);
}

#define fsync checked_fsync
#include "../firmware/components/reader_store/store.cpp"
#undef fsync

int main() {
    char temporary[] = "/tmp/pico-store-sync-XXXXXX";
    const char* directory = mkdtemp(temporary);
    assert(directory);
    const fs::path root(directory);
    fs::create_directory(root / "Books");
    std::ofstream(root / "Books/book.epub") << "book";
    pico_page::FileStore store(root.string());
    pico_page::ReadingRecord record{"book", "source-v1", "writer", 1, {"chapter", "node", 0}};
    assert(store.save_progress("Books/book.epub", record).record);

    failing_directory = root / "Books/book.epub.picopage";
    fail_from_sync = 2;
    ++record.local_revision;
    ++record.anchor.offset;
    assert(!store.save_progress("Books/book.epub", record).record);
    assert(store.load_progress("Books/book.epub", "book", "source-v1").record->local_revision == 2);
    assert(!store.save_progress("Books/book.epub", record).record);
    fail_from_sync = 0;
    assert(store.save_progress("Books/book.epub", record).record);

    fail_file_sync = true;
    assert(!store.save_progress("Books/book.epub", record).record);
    fail_file_sync = false;
    assert(store.save_progress("Books/book.epub", record).record);
    fs::remove(root / "Books/book.epub.picopage/progress.json");
    auto backup = store.load_progress("Books/book.epub", "book", "source-v1");
    assert(backup.record && backup.recovered_backup);
    fail_file_sync = true;
    assert(!store.save_progress("Books/book.epub", *backup.record).record);
    fail_file_sync = false;
    assert(store.save_progress("Books/book.epub", *backup.record).record);

    std::ofstream(root / "Books/new.epub") << "new book";
    failing_directory = root / "Books";
    directory_syncs = 0;
    fail_from_sync = 1;
    assert(!store.save_progress("Books/new.epub", record).record);
    assert(fs::is_directory(root / "Books/new.epub.picopage"));
    assert(!store.save_progress("Books/new.epub", record).record);
    fail_from_sync = 0;
    assert(store.save_progress("Books/new.epub", record).record);

    std::string error;
    failing_directory.clear();
    assert(store.save_last_book("Books/book.epub", error) == pico_page::StoreStatus::Ok);
    fail_file_sync = true;
    assert(store.save_last_book("Books/new.epub", error) == pico_page::StoreStatus::IoError);
    fail_file_sync = false;
    assert(store.load_last_book(error) == std::optional<std::string>("Books/book.epub"));
    failing_directory = root / "PicoPage/data";
    directory_syncs = 0;
    fail_from_sync = 2;
    assert(store.save_last_book("Books/new.epub", error) == pico_page::StoreStatus::IoError);
    assert(store.load_last_book(error) == std::optional<std::string>("Books/new.epub"));
    assert(store.save_last_book("Books/new.epub", error) == pico_page::StoreStatus::IoError);
    fail_from_sync = 0;
    assert(store.save_last_book("Books/new.epub", error) == pico_page::StoreStatus::Ok);
    std::ofstream(root / "PicoPage/data/last-book.txt") << "../invalid.epub\n";
    directory_syncs = 0;
    fail_from_sync = 1;
    assert(store.save_last_book("Books/book.epub", error) == pico_page::StoreStatus::IoError);
    fs::remove(root / "PicoPage/data/last-book.txt");
    assert(store.load_last_book(error) == std::optional<std::string>("Books/new.epub"));

    failing_directory.clear();
    fail_from_sync = 0;
    auto importing = store.begin_import("Books/unsynced.epub", 9, 0xcbf43926u, error);
    assert(importing && importing->append(0, "123456789", 9, error) == pico_page::StoreStatus::Ok);
    fail_file_sync = true;
    assert(importing->finish(error) == pico_page::StoreStatus::IoError);
    fail_file_sync = false;
    assert(importing->finish(error) == pico_page::StoreStatus::Invalid);
    importing.reset();
    assert(!fs::exists(root / "Books/unsynced.epub"));
    importing = store.begin_import("Books/published.epub", 9, 0xcbf43926u, error);
    assert(importing && importing->append(0, "123456789", 9, error) == pico_page::StoreStatus::Ok);
    failing_directory = root / "Books";
    directory_syncs = 0;
    fail_from_sync = 1;
    assert(importing->finish(error) == pico_page::StoreStatus::IoError);
    importing.reset();
    assert(fs::exists(root / "Books/published.epub"));
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    assert(store.inspect_asset("Books/published.epub", size, crc, error) == pico_page::StoreStatus::Ok);
    assert(size == 9 && crc == 0xcbf43926u);
    assert(!store.begin_import("Books/published.epub", 9, crc, error));

    failing_directory.clear();
    pico_page::BookSnapshot snapshot;
    assert(store.book_snapshot("Books/book.epub", snapshot) == pico_page::StoreStatus::Ok);
    fail_file_sync = true;
    assert(store.manage_book(snapshot, false, "book", "source-v1") == pico_page::StoreStatus::IoError);
    assert(store.load_progress("Books/book.epub", "book", "source-v1").record);
    fail_file_sync = false;
    assert(store.book_snapshot("Books/book.epub", snapshot) == pico_page::StoreStatus::Ok);
    assert(store.manage_book(snapshot, false, "book", "source-v1") == pico_page::StoreStatus::Ok);
    fs::remove(root / "Books/book.epub.picopage/progress.json");
    assert(store.load_progress("Books/book.epub", "book", "source-v1").status == pico_page::StoreStatus::Cleared);

    fs::remove_all(root);
    std::cout << "store: failed durability remains unacknowledged across retries\n";
}
