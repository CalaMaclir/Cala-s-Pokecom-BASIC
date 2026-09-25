#include "storage.hpp"
#include "program_file_guard.hpp"
#include "safe_file.hpp"

#include <cctype>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <dirent.h>
#include <sys/stat.h>

#include "blockdevice/sd.h"
#include "filesystem/fat.h"
#include "filesystem/vfs.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "platform.hpp"
#include "picocalc_display.hpp"

namespace rmb::storage {

namespace {

constexpr uint kSdSckPin = 18;
constexpr uint kSdMosiPin = 19;
constexpr uint kSdMisoPin = 16;
constexpr uint kSdCsPin = 17;
constexpr uint kSdDetectPin = 22;
constexpr uint32_t kSdSpiHz = 15625000;

bool mounted = false;
bool storage_locked = false;
std::atomic<Owner> storage_owner{Owner::Firmware};
enum class PendingReturn : std::uint8_t { None, SafeEject, Forced, UnsafeDisconnect };
std::atomic<PendingReturn> pending_return{PendingReturn::None};
std::atomic<UsbEvent> usb_event{UsbEvent::None};
std::atomic<std::uint32_t> raw_io_active{0};
static_assert(std::atomic<Owner>::is_always_lock_free,
              "USB ownership transitions must not block in IRQ context");
static_assert(std::atomic<UsbEvent>::is_always_lock_free,
              "USB event publication must not block in IRQ context");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "USB raw I/O accounting must not block in IRQ context");
std::uint32_t msc_blocks = 0;
blockdevice_t* sd_device = nullptr;
filesystem_t* fat_fs = nullptr;
char error_text[96] = "SD NOT INITIALIZED";

void set_error(const char* text) {
    std::snprintf(error_text, sizeof(error_text), "%s", text ? text : "SD ERROR");
}

bool regular_io_allowed() {
    const Owner current = storage_owner.load(std::memory_order_acquire);
    if (current != Owner::Firmware) {
        set_error(current == Owner::Unavailable
            ? "SD CARD NOT AVAILABLE - RECOVERY REQUIRED"
            : "SD CARD OWNED BY USB HOST");
        return false;
    }
    if (!storage_locked) return true;
    set_error("STORAGE BUSY");
    return false;
}

bool has_bas_extension(const char* name) {
    if (!name) return false;
    const std::size_t n = std::strlen(name);
    if (n < 4) return false;
    const char* p = name + n - 4;
    return p[0] == '.' &&
           (p[1] == 'B' || p[1] == 'b') &&
           (p[2] == 'A' || p[2] == 'a') &&
           (p[3] == 'S' || p[3] == 's');
}

bool make_path(const char* input, char* out, std::size_t capacity) {
    if (!input || !*input || !out || capacity < 8) {
        set_error("BAD FILENAME");
        return false;
    }

    while (*input == ' ' || *input == '\t') ++input;

    char name[80] = {};
    std::size_t n = 0;

    while (*input && *input != ' ' && *input != '\t') {
        const unsigned char c = static_cast<unsigned char>(*input++);
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
            set_error("BAD FILENAME");
            return false;
        }
        if (n + 1 >= sizeof(name)) {
            set_error("FILENAME TOO LONG");
            return false;
        }
        name[n++] = static_cast<char>(c);
    }
    name[n] = '\0';

    if (n == 0) {
        set_error("BAD FILENAME");
        return false;
    }

    if (!has_bas_extension(name)) {
        if (n + 4 >= sizeof(name)) {
            set_error("FILENAME TOO LONG");
            return false;
        }
        std::strcat(name, ".BAS");
    }

    if (std::snprintf(out, capacity, "/%s", name) >= static_cast<int>(capacity)) {
        set_error("FILENAME TOO LONG");
        return false;
    }
    return true;
}

bool make_bmp_path(const char* input, char* out, std::size_t capacity) {
    if (!input || !*input || !out || capacity < 8) {
        set_error("BAD FILENAME");
        return false;
    }

    while (*input == ' ' || *input == '\t') ++input;

    char name[80] = {};
    std::size_t n = 0;

    while (*input && *input != ' ' && *input != '\t') {
        const unsigned char ch = static_cast<unsigned char>(*input++);
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            set_error("BAD FILENAME");
            return false;
        }
        if (n + 1 >= sizeof(name)) {
            set_error("FILENAME TOO LONG");
            return false;
        }
        name[n++] = static_cast<char>(ch);
    }
    name[n] = '\0';

    if (n == 0) {
        set_error("BAD FILENAME");
        return false;
    }

    bool has_bmp = false;
    if (n >= 4) {
        const char* p = name + n - 4;
        has_bmp =
            p[0] == '.' &&
            (p[1] == 'B' || p[1] == 'b') &&
            (p[2] == 'M' || p[2] == 'm') &&
            (p[3] == 'P' || p[3] == 'p');
    }

    if (!has_bmp) {
        if (n + 4 >= sizeof(name)) {
            set_error("FILENAME TOO LONG");
            return false;
        }
        std::strcat(name, ".BMP");
    }

    if (std::snprintf(out, capacity, "/%s", name) >=
        static_cast<int>(capacity)) {
        set_error("FILENAME TOO LONG");
        return false;
    }

    return true;
}

void put_le16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xffu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
}

void put_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xffu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}


} // namespace

Owner owner() { return storage_owner.load(std::memory_order_acquire); }
const char* owner_name() {
    switch (owner()) {
    case Owner::Firmware: return "CPB";
    case Owner::UsbHost: return "USB HOST";
    case Owner::Transition: return "TRANSITION";
    default: return "UNAVAILABLE";
    }
}
bool firmware_owns_card() { return owner() == Owner::Firmware; }

bool begin_usb_host_ownership() {
    if (storage_locked) { set_error("STORAGE BUSY"); return false; }
    if (!mounted || !card_present() || !sd_device) {
        set_error("SD CARD NOT AVAILABLE");
        return false;
    }
    Owner expected = Owner::Firmware;
    if (!storage_owner.compare_exchange_strong(expected, Owner::Transition,
            std::memory_order_acq_rel)) {
        set_error(expected == Owner::UsbHost ? "USB STORAGE ALREADY ACTIVE" : "STORAGE TRANSITION BUSY");
        return false;
    }
    pending_return.store(PendingReturn::None, std::memory_order_release);
    usb_event.store(UsbEvent::None, std::memory_order_release);
    if (fs_unmount("/") == -1) {
        storage_owner.store(Owner::Firmware, std::memory_order_release);
        set_error("SD UNMOUNT FAILED");
        return false;
    }
    mounted = false;
    const int synced = sd_device->sync ? sd_device->sync(sd_device) : 0;
    const std::uint64_t bytes = sd_device->size ? sd_device->size(sd_device) : 0;
    const std::uint64_t blocks = bytes / usb_block_size();
    if (synced != 0 || bytes == 0 || bytes % usb_block_size() != 0 ||
        blocks > std::numeric_limits<std::uint32_t>::max()) {
        if (fs_mount("/", fat_fs, sd_device) != -1) {
            mounted = true;
            storage_owner.store(Owner::Firmware, std::memory_order_release);
        } else {
            storage_owner.store(Owner::Unavailable, std::memory_order_release);
        }
        set_error(synced != 0 ? "SD SYNC FAILED" : "SD CAPACITY UNSUPPORTED");
        return false;
    }
    msc_blocks = static_cast<std::uint32_t>(blocks);
    storage_owner.store(Owner::UsbHost, std::memory_order_release);
    set_error("USB HOST OWNS SD CARD");
    return true;
}

bool request_usb_safe_return(bool host_ejected) {
    if (!host_ejected) return false;
    Owner expected = Owner::UsbHost;
    if (storage_owner.compare_exchange_strong(expected, Owner::Transition,
            std::memory_order_acq_rel)) {
        pending_return.store(PendingReturn::SafeEject, std::memory_order_release);
        return true;
    }
    return expected == Owner::Transition &&
           pending_return.load(std::memory_order_acquire) == PendingReturn::SafeEject;
}

bool request_usb_force_return() {
    Owner expected = Owner::UsbHost;
    if (storage_owner.compare_exchange_strong(expected, Owner::Transition,
            std::memory_order_acq_rel)) {
        pending_return.store(PendingReturn::Forced, std::memory_order_release);
        return true;
    }
    return expected == Owner::Transition &&
           pending_return.load(std::memory_order_acquire) == PendingReturn::Forced;
}

void notify_usb_disconnect() {
    const PendingReturn pending = pending_return.load(std::memory_order_acquire);
    if (pending == PendingReturn::SafeEject || pending == PendingReturn::Forced) return;
    Owner value = storage_owner.load(std::memory_order_acquire);
    while ((value == Owner::UsbHost || value == Owner::Transition) &&
           !storage_owner.compare_exchange_weak(value, Owner::Unavailable,
               std::memory_order_acq_rel)) {}
    if (value == Owner::UsbHost || value == Owner::Transition) {
        pending_return.store(PendingReturn::UnsafeDisconnect, std::memory_order_release);
        usb_event.store(UsbEvent::UnsafeDisconnect, std::memory_order_release);
    }
}

void poll() {
    const Owner current = owner();
    if (current == Owner::UsbHost && !card_present()) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        pending_return.store(PendingReturn::None, std::memory_order_release);
        usb_event.store(UsbEvent::CardRemoved, std::memory_order_release);
        set_error("SD CARD REMOVED DURING USB STORAGE");
        return;
    }
    const PendingReturn pending = pending_return.load(std::memory_order_acquire);
    if (current != Owner::Transition ||
        (pending != PendingReturn::SafeEject && pending != PendingReturn::Forced) ||
        raw_io_active.load(std::memory_order_acquire) != 0) return;
    pending_return.store(PendingReturn::None, std::memory_order_release);
    if (!card_present()) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        usb_event.store(UsbEvent::CardRemoved, std::memory_order_release);
        set_error("SD CARD REMOVED DURING USB STORAGE");
        return;
    }
    if ((sd_device->sync && sd_device->sync(sd_device) != 0) ||
        fs_mount("/", fat_fs, sd_device) == -1) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        usb_event.store(UsbEvent::IoError, std::memory_order_release);
        set_error("SD REMOUNT FAILED AFTER USB EJECT");
        return;
    }
    mounted = true;
    msc_blocks = 0;
    storage_owner.store(Owner::Firmware, std::memory_order_release);
    usb_event.store(UsbEvent::ReturnedToFirmware, std::memory_order_release);
    set_error("OK");
}

UsbEvent take_usb_event() {
    const UsbEvent event = usb_event.exchange(UsbEvent::None, std::memory_order_acq_rel);
    // Called only by the foreground. IRQ/raw callbacks publish enums and do
    // not enter snprintf or any other libc/FatFs path.
    if (event == UsbEvent::UnsafeDisconnect) set_error("USB STORAGE DISCONNECTED UNSAFELY");
    else if (event == UsbEvent::IoError) set_error("USB SD I/O ERROR - RECOVERY REQUIRED");
    else if (event == UsbEvent::CardRemoved) set_error("SD CARD REMOVED DURING USB STORAGE");
    else if (event == UsbEvent::ReturnedToFirmware) set_error("OK");
    return event;
}

bool recover_firmware_ownership() {
    Owner expected = Owner::Unavailable;
    if (!storage_owner.compare_exchange_strong(expected, Owner::Transition,
            std::memory_order_acq_rel)) {
        if (expected == Owner::Firmware) return true;
        set_error("USB HOST STILL OWNS SD CARD");
        return false;
    }
    if (!card_present() || !sd_device ||
        (sd_device->sync && sd_device->sync(sd_device) != 0) ||
        fs_mount("/", fat_fs, sd_device) == -1) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        set_error("SD RECOVERY FAILED");
        return false;
    }
    mounted = true;
    msc_blocks = 0;
    pending_return.store(PendingReturn::None, std::memory_order_release);
    storage_owner.store(Owner::Firmware, std::memory_order_release);
    usb_event.store(UsbEvent::ReturnedToFirmware, std::memory_order_release);
    set_error("OK");
    return true;
}

std::uint32_t usb_block_count() {
    // IRQ context: never touch GPIO or sleep here. Foreground poll() turns
    // removal into Unavailable; raw I/O errors close the remaining race.
    return owner() == Owner::UsbHost ? msc_blocks : 0;
}

bool usb_read_block(std::uint32_t lba, std::uint32_t offset, void* buffer, std::uint32_t size) {
    if (!buffer || offset != 0 || size != usb_block_size() ||
        lba >= msc_blocks || !sd_device || !sd_device->read) return false;
    raw_io_active.fetch_add(1, std::memory_order_acq_rel);
    if (owner() != Owner::UsbHost) {
        raw_io_active.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    const std::uint64_t address = static_cast<std::uint64_t>(lba) * usb_block_size();
    const bool ok = sd_device->read(sd_device, buffer, address, size) == 0;
    if (!ok) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        pending_return.store(PendingReturn::None, std::memory_order_release);
        usb_event.store(UsbEvent::IoError, std::memory_order_release);
    }
    raw_io_active.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
}

bool usb_write_block(std::uint32_t lba, std::uint32_t offset, const void* buffer, std::uint32_t size) {
    if (!buffer || offset != 0 || size != usb_block_size() ||
        lba >= msc_blocks || !sd_device || !sd_device->program) return false;
    raw_io_active.fetch_add(1, std::memory_order_acq_rel);
    if (owner() != Owner::UsbHost) {
        raw_io_active.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    const std::uint64_t address = static_cast<std::uint64_t>(lba) * usb_block_size();
    const bool ok = sd_device->program(sd_device, buffer, address, size) == 0;
    if (!ok) {
        storage_owner.store(Owner::Unavailable, std::memory_order_release);
        pending_return.store(PendingReturn::None, std::memory_order_release);
        usb_event.store(UsbEvent::IoError, std::memory_order_release);
    }
    raw_io_active.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
}

bool card_present() {
    gpio_init(kSdDetectPin);
    gpio_set_dir(kSdDetectPin, GPIO_IN);
    gpio_pull_up(kSdDetectPin);
    sleep_ms(2);
    return gpio_get(kSdDetectPin) == 0;
}

bool init() {
    if (!regular_io_allowed()) return false;
    if (mounted) {
        if (!card_present()) {
            set_error("SD CARD REMOVED");
            return false;
        }
        return true;
    }

    gpio_init(kSdDetectPin);
    gpio_set_dir(kSdDetectPin, GPIO_IN);
    gpio_pull_up(kSdDetectPin);
    sleep_ms(20);

    // PicoCalc SD detect is active low.
    if (gpio_get(kSdDetectPin)) {
        set_error("SD CARD NOT FOUND");
        return false;
    }

    if (!sd_device) {
        sd_device = blockdevice_sd_create(
            spi0,
            kSdMosiPin,
            kSdMisoPin,
            kSdSckPin,
            kSdCsPin,
            kSdSpiHz,
            true
        );
    }

    if (!sd_device) {
        set_error("SD DRIVER INIT FAILED");
        return false;
    }

    if (!fat_fs) {
        fat_fs = filesystem_fat_create();
    }
    if (!fat_fs) {
        set_error("FAT INIT FAILED");
        return false;
    }

    const int result = fs_mount("/", fat_fs, sd_device);
    if (result == -1) {
        set_error("SD MOUNT FAILED");
        return false;
    }

    mounted = true;
    set_error("OK");
    return true;
}

bool remount() {
    if (!regular_io_allowed()) return false;
    if (mounted) {
        if (fs_unmount("/") == -1) {
            set_error("SD UNMOUNT FAILED");
            return false;
        }
        mounted = false;
    }

    sleep_ms(20);
    return init();
}

bool available() {
    return mounted && firmware_owns_card();
}

const char* last_error() {
    return error_text;
}

bool try_lock() {
    if (storage_locked || !mounted) {
        if (storage_locked) set_error("STORAGE BUSY");
        else set_error("SD NOT AVAILABLE");
        return false;
    }
    storage_locked = true;
    return true;
}

void unlock() { storage_locked = false; }
bool busy() { return storage_locked; }

bool program_exists(const char* name) {
    if (!init()) return false;

    char path[96] = {};
    if (!make_path(name, path, sizeof(path))) return false;

    FILE* fp = std::fopen(path, "r");
    if (!fp) {
        set_error("OK");
        return false;
    }

    std::fclose(fp);
    set_error("OK");
    return true;
}

bool save_program(const char* name, ProgramStore& program) {
    const bool ok = program.save(name);
    set_error(ok ? "OK" : program.error());
    return ok;
}

bool load_program(const char* name, ProgramStore& program) {
    const bool ok = program.load(name);
    set_error(ok ? "OK" : program.error());
    return ok;
}

bool list_program_files() {
    if (!init()) return false;

    DIR* dir = opendir("/");
    if (!dir) {
        set_error("CANNOT OPEN SD DIRECTORY");
        return false;
    }

    std::size_t count = 0;
    while (dirent* ent = readdir(dir)) {
        if (!ent->d_name || ent->d_name[0] == '\0') continue;
        if (!has_bas_extension(ent->d_name) || program_files::reserved(ent->d_name)) continue;

        platform::put_string(ent->d_name);
        platform::put_string("\r\n");
        ++count;
    }

    closedir(dir);

    if (count == 0) {
        platform::put_string("(no .BAS files)\r\n");
    }

    set_error("OK");
    return true;
}

bool list_root_files() {
    if (!regular_io_allowed() || !init()) return false;

    DIR* dir = opendir("/");
    if (!dir) {
        set_error("CANNOT OPEN SD DIRECTORY");
        return false;
    }

    std::size_t count = 0;
    while (dirent* ent = readdir(dir)) {
        if (!ent->d_name || !program_files::visible_in_directory(ent->d_name) ||
            !SafeFileWriter::valid_root_name(ent->d_name)) continue;

        char path[96] = {};
        if (std::snprintf(path, sizeof(path), "/%s", ent->d_name) >=
            static_cast<int>(sizeof(path))) continue;
        struct stat value;
        if (stat(path, &value) != 0) continue;

        char row[96] = {};
        if (S_ISREG(value.st_mode)) {
            std::snprintf(row, sizeof(row), "%-38.38s %10lu\r\n",
                          ent->d_name,
                          static_cast<unsigned long>(value.st_size));
        } else if (S_ISDIR(value.st_mode)) {
            std::snprintf(row, sizeof(row), "%-46.46s <DIR>\r\n",
                          ent->d_name);
        } else {
            continue;
        }
        platform::put_string(row);
        ++count;
    }
    closedir(dir);

    if (count == 0) platform::put_string("(no files)\r\n");
    set_error("OK");
    return true;
}

std::size_t collect_program_files(
    char* output,
    std::size_t max_files,
    std::size_t slot_size
) {
    if (!regular_io_allowed()) return 0;
    if (!output || max_files == 0 || slot_size < 2) return 0;
    if (!init()) return 0;

    DIR* dir = opendir("/");
    if (!dir) {
        set_error("CANNOT OPEN SD DIRECTORY");
        return 0;
    }

    std::size_t count = 0;
    while (dirent* ent = readdir(dir)) {
        if (count >= max_files) break;
        if (!ent->d_name || ent->d_name[0] == '\0') continue;
        if (!has_bas_extension(ent->d_name) || program_files::reserved(ent->d_name)) continue;

        char* slot = output + count * slot_size;
        std::snprintf(slot, slot_size, "%s", ent->d_name);
        ++count;
    }

    closedir(dir);
    set_error("OK");
    return count;
}

std::size_t collect_transfer_files(
    char* output,
    std::size_t max_files,
    std::size_t slot_size
) {
    if (!regular_io_allowed()) return 0;
    if (!output || max_files == 0 || slot_size < 2) return 0;
    if (!init()) return 0;

    DIR* dir = opendir("/");
    if (!dir) {
        set_error("CANNOT OPEN SD DIRECTORY");
        return 0;
    }

    std::size_t count = 0;
    while (dirent* ent = readdir(dir)) {
        if (count >= max_files) break;
        if (!ent->d_name || !program_files::visible_for_transfer(ent->d_name) ||
            !SafeFileWriter::valid_root_name(ent->d_name)) continue;

        char path[96] = {};
        if (std::snprintf(path, sizeof(path), "/%s", ent->d_name) >=
            static_cast<int>(sizeof(path))) continue;
        struct stat value;
        if (stat(path, &value) != 0 || !S_ISREG(value.st_mode)) continue;

        char* slot = output + count * slot_size;
        std::snprintf(slot, slot_size, "%s", ent->d_name);
        ++count;
    }

    closedir(dir);
    set_error("OK");
    return count;
}

bool read_root_text(
    const char* filename,
    char* output,
    std::size_t capacity
) {
    if (!regular_io_allowed()) return false;
    if (!filename || !*filename || !output || capacity == 0) return false;
    output[0] = '\0';
    if (!init()) return false;

    char path[96] = {};
    if (std::snprintf(path, sizeof(path), "/%s", filename) >=
        static_cast<int>(sizeof(path))) {
        set_error("FILENAME TOO LONG");
        return false;
    }

    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        set_error("FILE NOT FOUND");
        return false;
    }

    const std::size_t n = std::fread(output, 1, capacity - 1, fp);
    output[n] = '\0';
    const bool ok = !std::ferror(fp) && std::feof(fp);
    std::fclose(fp);

    if (!ok) {
        set_error(n + 1 >= capacity ? "FILE TOO LARGE" : "READ ERROR");
        return false;
    }

    set_error("OK");
    return true;
}

bool write_root_text(const char* filename, const char* text) {
    if (!filename || !*filename || !text) return false;
    if (program_files::in_use(filename)) { set_error("FILE IN USE"); return false; }
    if (!init()) return false;

    char path[96] = {};
    if (std::snprintf(path, sizeof(path), "/%s", filename) >=
        static_cast<int>(sizeof(path))) {
        set_error("FILENAME TOO LONG");
        return false;
    }

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        set_error("CANNOT OPEN FILE");
        return false;
    }

    const std::size_t len = std::strlen(text);
    bool ok = std::fwrite(text, 1, len, fp) == len;
    if (std::fclose(fp) != 0) ok = false;

    if (!ok) {
        set_error("WRITE ERROR");
        return false;
    }

    set_error("OK");
    return true;
}

bool screenshot_exists(const char* name) {
    if (!init()) return false;

    char path[96] = {};
    if (!make_bmp_path(name, path, sizeof(path))) return false;

    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        set_error("OK");
        return false;
    }

    std::fclose(fp);
    set_error("OK");
    return true;
}

bool save_screenshot(
    const char* name,
    int x1,
    int y1,
    int x2,
    int y2
) {
    if (!init()) return false;

    if (x1 > x2) {
        const int t = x1; x1 = x2; x2 = t;
    }
    if (y1 > y2) {
        const int t = y1; y1 = y2; y2 = t;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > 319) x2 = 319;
    if (y2 > 319) y2 = 319;

    if (x1 > x2 || y1 > y2) {
        set_error("BAD SCREENSHOT REGION");
        return false;
    }

    char path[96] = {};
    if (!make_bmp_path(name, path, sizeof(path))) return false;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        set_error("CANNOT OPEN FILE");
        return false;
    }

    const int image_width = x2 - x1 + 1;
    const int image_height = y2 - y1 + 1;
    const std::uint32_t row_bytes =
        static_cast<std::uint32_t>(image_width * 3);
    const std::uint32_t row_stride = (row_bytes + 3u) & ~3u;
    const std::uint32_t pixel_bytes =
        row_stride * static_cast<std::uint32_t>(image_height);
    const std::uint32_t file_size = 54u + pixel_bytes;

    std::uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, file_size);
    put_le32(header + 10, 54u);
    put_le32(header + 14, 40u);
    put_le32(header + 18, static_cast<std::uint32_t>(image_width));
    put_le32(header + 22, static_cast<std::uint32_t>(image_height));
    put_le16(header + 26, 1u);
    put_le16(header + 28, 24u);
    put_le32(header + 34, pixel_bytes);

    bool ok =
        std::fwrite(header, 1, sizeof(header), fp) == sizeof(header);

    std::uint8_t row[320 * 3 + 3] = {};
    const std::uint8_t padding[3] = {0, 0, 0};
    const std::size_t pad =
        static_cast<std::size_t>(row_stride - row_bytes);

    // BMP is stored bottom-up.
    for (int y = y2; ok && y >= y1; --y) {
        if (!picocalc::display::read_visible_row_bgr(
                y,
                x1,
                x2,
                row
            )) {
            ok = false;
            break;
        }

        if (std::fwrite(
                row,
                1,
                static_cast<std::size_t>(row_bytes),
                fp
            ) != static_cast<std::size_t>(row_bytes)) {
            ok = false;
            break;
        }

        if (pad != 0 &&
            std::fwrite(padding, 1, pad, fp) != pad) {
            ok = false;
            break;
        }
    }

    if (std::fclose(fp) != 0) ok = false;

    if (!ok) {
        set_error("SCREENSHOT WRITE ERROR");
        return false;
    }

    set_error("OK");
    return true;
}

} // namespace rmb::storage
