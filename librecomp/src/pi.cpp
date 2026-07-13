#include <memory>
#include <fstream>
#include <array>
#include <cstring>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/files.hpp"
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

static std::vector<uint8_t> rom;

bool recomp::is_rom_loaded() {
    return !rom.empty();
}

void recomp::set_rom_contents(std::vector<uint8_t>&& new_rom) {
    rom = std::move(new_rom);
}

std::span<const uint8_t> recomp::get_rom() {
    return rom;
}

constexpr uint32_t k1_to_phys(uint32_t addr) {
    return addr & 0x1FFFFFFF;
}

constexpr uint32_t phys_to_k1(uint32_t addr) {
    return addr | 0xA0000000;
}

extern "C" void __osPiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}

extern "C" void __osPiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}

extern "C" void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSPiHandle* handle = TO_PTR(OSPiHandle, recomp::cart_handle);
    handle->type = 0; // cart
    handle->baseAddress = phys_to_k1(recomp::rom_base);
    handle->domain = 0;

    ctx->r2 = (gpr)recomp::cart_handle;
}

extern "C" void osDriveRomInit_recomp(uint8_t * rdram, recomp_context * ctx) {
    OSPiHandle* handle = TO_PTR(OSPiHandle, recomp::drive_handle);
    handle->type = 1; // bulk
    handle->baseAddress = phys_to_k1(recomp::drive_base);
    handle->domain = 0;

    ctx->r2 = (gpr)recomp::drive_handle;
}

extern "C" void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    ;
}

void recomp::do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes) {
    // TODO use word copies when possible

    // TODO handle misaligned DMA
    assert((physical_addr & 0x1) == 0 && "Only PI DMA from aligned ROM addresses is currently supported");
    assert((ram_address & 0x7) == 0 && "Only PI DMA to aligned RDRAM addresses is currently supported");
    uint8_t* rom_addr = rom.data() + physical_addr - recomp::rom_base;
    for (size_t i = 0; i < num_bytes; i++) {
        MEM_B(i, ram_address) = *rom_addr;
        rom_addr++;
    }
}

void recomp::do_rom_pio(uint8_t* rdram, gpr ram_address, uint32_t physical_addr) {
    assert((physical_addr & 0x3) == 0 && "PIO not 4-byte aligned in device, currently unsupported");
    assert((ram_address & 0x3) == 0 && "PIO not 4-byte aligned in RDRAM, currently unsupported");
    uint8_t* rom_addr = rom.data() + physical_addr - recomp::rom_base;
    MEM_B(0, ram_address) = *rom_addr++;
    MEM_B(1, ram_address) = *rom_addr++;
    MEM_B(2, ram_address) = *rom_addr++;
    MEM_B(3, ram_address) = *rom_addr++;
}

struct {
    std::vector<char> save_buffer;
    std::thread saving_thread;
    std::filesystem::path save_file_path;
    std::filesystem::path save_root_path;
    moodycamel::LightweightSemaphore write_sempahore;
    std::mutex save_buffer_mutex;
    std::mutex control_mutex;
    std::mutex control_state_mutex;
    std::condition_variable control_state_changed;
    uint64_t control_requested_generation = 0;
    uint64_t control_acknowledged_generation = 0;
    uint64_t control_released_generation = 0;
    bool worker_running = false;
    std::shared_mutex operation_mutex;
    std::mutex path_mutex;
    std::atomic_bool initialized = false;
    std::atomic_bool last_write_succeeded = true;
} save_context;

const std::u8string save_folder = u8"saves";

extern std::filesystem::path config_path;

std::filesystem::path ultramodern::get_save_file_path() {
    std::lock_guard lock{ save_context.path_mutex };
    return save_context.save_file_path;
}

std::filesystem::path ultramodern::get_save_root_path() {
    std::lock_guard lock{ save_context.path_mutex };
    return save_context.save_root_path;
}

size_t ultramodern::get_save_file_size() {
    std::lock_guard lock{ save_context.save_buffer_mutex };
    return save_context.save_buffer.size();
}

void set_save_file_path(const std::u8string& subfolder, const std::u8string& name) {
    std::lock_guard lock{ save_context.path_mutex };
    std::filesystem::path save_folder_path = save_context.save_root_path;
    if (!subfolder.empty()) {
        save_folder_path = save_folder_path / subfolder;
    }
    save_context.save_file_path = save_folder_path / (name + u8".bin");
}

bool update_save_file() {
    bool saving_failed = false;
    const std::filesystem::path save_file_path = ultramodern::get_save_file_path();
    {
        std::ofstream save_file = recomp::open_output_file_with_backup(save_file_path, std::ios_base::binary);

        if (save_file.good()) {
            std::lock_guard lock{ save_context.save_buffer_mutex };
            save_file.write(save_context.save_buffer.data(), save_context.save_buffer.size());
        }
        else {
            saving_failed = true;
        }
    }
    if (!saving_failed) {
        saving_failed = !recomp::finalize_output_file_with_backup(save_file_path);
    }
    if (saving_failed) {
        ultramodern::error_handling::message_box("Failed to write to the save file. Check your file permissions and whether the save folder has been moved to Dropbox or similar, as this can cause issues.");
    }
    save_context.last_write_succeeded = !saving_failed;
    return !saving_failed;
}

extern std::atomic_bool exited;

void saving_thread_func(RDRAM_ARG1) {
    while (true) {
        bool save_buffer_updated = false;
        // Repeatedly wait for a new action to be sent.
        constexpr int64_t wait_time_microseconds = 10000;
        constexpr int max_actions = 128;
        int num_actions = 0;

        // Wait up to the given timeout for a write to come in. Allow multiple writes to coalesce together into a single save.
        // Cap the number of coalesced writes to guarantee that the save buffer eventually gets written out to the file even if the game
        // is constantly sending writes.
        while (save_context.write_sempahore.wait(wait_time_microseconds) && num_actions < max_actions) {
            save_buffer_updated = true;
            num_actions++;
        }

        // If an action came through that affected the save file, save the updated contents.
        if (save_buffer_updated) {
            update_save_file();
        }

        {
            std::unique_lock control_lock{ save_context.control_state_mutex };
            if (save_context.control_requested_generation > save_context.control_acknowledged_generation) {
                const uint64_t generation = save_context.control_requested_generation;
                save_context.control_acknowledged_generation = generation;
                save_context.control_state_changed.notify_all();
                save_context.control_state_changed.wait(control_lock, [generation] {
                    return save_context.control_released_generation >= generation;
                });
            }
            if (exited.load()) {
                save_context.worker_running = false;
                save_context.initialized = false;
                save_context.control_state_changed.notify_all();
                break;
            }
        }
    }
}

void save_write_ptr(const void* in, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    std::shared_lock operation_lock{ save_context.operation_mutex };
    {
        std::lock_guard lock { save_context.save_buffer_mutex };
        memcpy(&save_context.save_buffer[offset], in, count);
    }
    
    save_context.write_sempahore.signal();
}

void save_write(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    std::shared_lock operation_lock{ save_context.operation_mutex };
    {
        std::lock_guard lock { save_context.save_buffer_mutex };
        for (gpr i = 0; i < count; i++) {
            save_context.save_buffer[offset + i] = MEM_B(i, rdram_address);
        }
    }

    save_context.write_sempahore.signal();
}

void save_read(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    std::shared_lock operation_lock{ save_context.operation_mutex };
    std::lock_guard lock { save_context.save_buffer_mutex };
    for (gpr i = 0; i < count; i++) {
        MEM_B(i, rdram_address) = save_context.save_buffer[offset + i];
    }
}

void save_clear(uint32_t start, uint32_t size, char value) {
    assert(start + size < save_context.save_buffer.size());

    std::shared_lock operation_lock{ save_context.operation_mutex };
    {
        std::lock_guard lock { save_context.save_buffer_mutex };
        std::fill_n(save_context.save_buffer.begin() + start, size, value);
    }

    save_context.write_sempahore.signal();
}

size_t get_save_size(recomp::SaveType save_type) {
    switch (save_type) {
        case recomp::SaveType::AllowAll:
        case recomp::SaveType::Flashram:
            return 0x20000;
        case recomp::SaveType::Sram:
            return 0x8000;
        case recomp::SaveType::Eep16k:
            return 0x800;
        case recomp::SaveType::Eep4k:
            return 0x200;
        case recomp::SaveType::None:
            return 0;
    }
    return 0;
}

bool read_save_file(const std::filesystem::path& save_file_path, bool allow_missing) {
    if (allow_missing) {
        std::error_code error;
        std::filesystem::create_directories(save_file_path.parent_path(), error);
        if (error) {
            return false;
        }
    }

    std::vector<char> loaded_save(save_context.save_buffer.size(), 0);
    std::ifstream save_file = recomp::open_input_file_with_backup(save_file_path, std::ios_base::binary);
    if (save_file.good()) {
        save_file.read(loaded_save.data(), loaded_save.size());
        if (static_cast<size_t>(save_file.gcount()) != loaded_save.size() || save_file.peek() != std::ifstream::traits_type::eof()) {
            return false;
        }
    }
    else if (!allow_missing) {
        return false;
    }

    std::lock_guard lock{ save_context.save_buffer_mutex };
    save_context.save_buffer = std::move(loaded_save);
    return true;
}

bool read_save_file(bool allow_missing) {
    return read_save_file(ultramodern::get_save_file_path(), allow_missing);
}

void ultramodern::init_saving(RDRAM_ARG1) {
    {
        std::lock_guard lock{ save_context.path_mutex };
        if (save_context.save_root_path.empty()) {
            save_context.save_root_path = config_path / save_folder;
        }
    }
    set_save_file_path(u8"", recomp::current_game_id());

    save_context.save_buffer.resize(get_save_size(recomp::get_save_type()));

    read_save_file(true);

    {
        std::lock_guard control_lock{ save_context.control_state_mutex };
        save_context.worker_running = true;
        save_context.initialized = true;
    }
    save_context.saving_thread = std::thread{saving_thread_func, PASS_RDRAM};
}

namespace {
class SaveControlTransaction {
public:
    SaveControlTransaction() : operation_lock{ save_context.operation_mutex }, control_lock{ save_context.control_mutex } {
        std::unique_lock state_lock{ save_context.control_state_mutex };
        if (!save_context.worker_running || exited.load()) {
            return;
        }
        generation = ++save_context.control_requested_generation;
        save_context.control_state_changed.wait(state_lock, [this] {
            return save_context.control_acknowledged_generation >= generation || !save_context.worker_running;
        });
        paused = save_context.control_acknowledged_generation >= generation;
    }
    bool ready() const { return paused; }
    ~SaveControlTransaction() {
        if (paused) {
            std::lock_guard state_lock{ save_context.control_state_mutex };
            save_context.control_released_generation = generation;
            save_context.control_state_changed.notify_all();
        }
    }
private:
    std::unique_lock<std::shared_mutex> operation_lock;
    std::unique_lock<std::mutex> control_lock;
    bool paused = false;
    uint64_t generation = 0;
};
}

bool ultramodern::flush_save_file() {
    SaveControlTransaction transaction;
    return transaction.ready() && save_context.last_write_succeeded.load();
}

bool ultramodern::snapshot_save_file(std::vector<uint8_t>& snapshot) {
    SaveControlTransaction transaction;
    if (!transaction.ready() || !save_context.last_write_succeeded.load()) {
        return false;
    }
    std::lock_guard lock{ save_context.save_buffer_mutex };
    snapshot.assign(save_context.save_buffer.begin(), save_context.save_buffer.end());
    return true;
}

bool ultramodern::import_save_file(std::span<const uint8_t> data) {
    SaveControlTransaction transaction;
    if (!transaction.ready() || data.size() != save_context.save_buffer.size()) {
        return false;
    }
    std::vector<char> old_save;
    {
        std::lock_guard lock{ save_context.save_buffer_mutex };
        old_save = save_context.save_buffer;
        std::memcpy(save_context.save_buffer.data(), data.data(), data.size());
    }
    if (update_save_file()) {
        return true;
    }
    {
        std::lock_guard lock{ save_context.save_buffer_mutex };
        save_context.save_buffer = std::move(old_save);
    }
    return false;
}

bool ultramodern::reload_save_file() {
    SaveControlTransaction transaction;
    return transaction.ready() && read_save_file(false);
}

bool ultramodern::set_save_root_path(const std::filesystem::path& root, bool load_existing) {
    if (root.empty()) {
        return false;
    }
    SaveControlTransaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    std::filesystem::path relative_path;
    std::filesystem::path old_root;
    std::filesystem::path old_file;
    {
        std::lock_guard lock{ save_context.path_mutex };
        old_root = save_context.save_root_path;
        old_file = save_context.save_file_path;
        relative_path = save_context.save_file_path.lexically_relative(save_context.save_root_path);
        if (relative_path.empty() || *relative_path.begin() == "..") {
            return false;
        }
    }
    const std::filesystem::path new_file = root / relative_path;
    if (load_existing) {
        if (!read_save_file(new_file, false)) {
            return false;
        }
        std::lock_guard path_lock{ save_context.path_mutex };
        save_context.save_root_path = root;
        save_context.save_file_path = new_file;
        return true;
    }
    {
        std::lock_guard path_lock{ save_context.path_mutex };
        save_context.save_root_path = root;
        save_context.save_file_path = new_file;
    }
    if (update_save_file()) {
        return true;
    }
    {
        std::lock_guard lock{ save_context.path_mutex };
        save_context.save_root_path = std::move(old_root);
        save_context.save_file_path = std::move(old_file);
    }
    return false;
}

void ultramodern::change_save_file(const std::u8string& subfolder, const std::u8string& name) {
    SaveControlTransaction transaction;
    if (!transaction.ready()) {
        return;
    }
    std::filesystem::path save_folder_path = ultramodern::get_save_root_path();
    if (!subfolder.empty()) {
        save_folder_path /= subfolder;
    }
    const std::filesystem::path new_file = save_folder_path / (name + u8".bin");
    if (!read_save_file(new_file, true)) {
        return;
    }
    std::lock_guard path_lock{ save_context.path_mutex };
    save_context.save_file_path = new_file;
}

void ultramodern::join_saving_thread() {
    if (save_context.saving_thread.joinable()) {
        save_context.saving_thread.join();
    }
    save_context.initialized = false;
}

void do_dma(RDRAM_ARG PTR(OSMesgQueue) mq, gpr rdram_address, uint32_t physical_addr, uint32_t size, uint32_t direction) {
    // TODO asynchronous transfer
    // TODO implement unaligned DMA correctly
    if (direction == 0) {
        if (physical_addr >= recomp::rom_base) {
            // read cart rom
            recomp::do_rom_read(rdram, rdram_address, physical_addr, size);

            // Send a message to the mq to indicate that the transfer completed
            ultramodern::enqueue_external_message(mq, 0, false, true);
        } else if (physical_addr >= recomp::sram_base) {
            if (!recomp::sram_allowed()) {
                ultramodern::error_handling::message_box("Attempted to use SRAM saving with other save type");
                ULTRAMODERN_QUICK_EXIT();
            }
            // read sram
            save_read(rdram, rdram_address, physical_addr - recomp::sram_base, size);

            // Send a message to the mq to indicate that the transfer completed
            ultramodern::enqueue_external_message(mq, 0, false, true);
        } else {
            fprintf(stderr, "[WARN] PI DMA read from unknown region, phys address 0x%08X\n", physical_addr);
        }
    } else {
        if (physical_addr >= recomp::rom_base) {
            // write cart rom
            throw std::runtime_error("ROM DMA write unimplemented");
        } else if (physical_addr >= recomp::sram_base) {
            if (!recomp::sram_allowed()) {
                ultramodern::error_handling::message_box("Attempted to use SRAM saving with other save type");
                ULTRAMODERN_QUICK_EXIT();
            }
            // write sram
            save_write(rdram, rdram_address, physical_addr - recomp::sram_base, size);

            // Send a message to the mq to indicate that the transfer completed
            ultramodern::enqueue_external_message(mq, 0, false, true);
        } else {
            fprintf(stderr, "[WARN] PI DMA write to unknown region, phys address 0x%08X\n", physical_addr);
        }
    }
}

extern "C" void osPiStartDma_recomp(RDRAM_ARG recomp_context* ctx) {
    uint32_t mb = ctx->r4;
    uint32_t pri = ctx->r5;
    uint32_t direction = ctx->r6;
    uint32_t devAddr = ctx->r7 | recomp::rom_base;
    gpr dramAddr = MEM_W(0x10, ctx->r29);
    uint32_t size = MEM_W(0x14, ctx->r29);
    PTR(OSMesgQueue) mq = MEM_W(0x18, ctx->r29);
    uint32_t physical_addr = k1_to_phys(devAddr);

    debug_printf("[pi] DMA from 0x%08X into 0x%08X of size 0x%08X\n", devAddr, dramAddr, size);

    do_dma(PASS_RDRAM mq, dramAddr, physical_addr, size, direction);

    ctx->r2 = 0;
}

extern "C" void osEPiStartDma_recomp(RDRAM_ARG recomp_context* ctx) {
    OSPiHandle* handle = TO_PTR(OSPiHandle, ctx->r4);
    OSIoMesg* mb = TO_PTR(OSIoMesg, ctx->r5);
    uint32_t direction = ctx->r6;
    uint32_t devAddr = handle->baseAddress | mb->devAddr;
    gpr dramAddr = mb->dramAddr;
    uint32_t size = mb->size;
    PTR(OSMesgQueue) mq = mb->hdr.retQueue;
    uint32_t physical_addr = k1_to_phys(devAddr);

    debug_printf("[pi] DMA from 0x%08X into 0x%08X of size 0x%08X\n", devAddr, dramAddr, size);

    do_dma(PASS_RDRAM mq, dramAddr, physical_addr, size, direction);

    ctx->r2 = 0;
}

extern "C" void osEPiReadIo_recomp(RDRAM_ARG recomp_context * ctx) {
    OSPiHandle* handle = TO_PTR(OSPiHandle, ctx->r4);
    uint32_t devAddr = handle->baseAddress | ctx->r5;
    gpr dramAddr = ctx->r6;
    uint32_t physical_addr = k1_to_phys(devAddr);

    if (physical_addr > recomp::rom_base) {
        // cart rom
        recomp::do_rom_pio(PASS_RDRAM dramAddr, physical_addr);
    } else {
        // sram
        assert(false && "SRAM ReadIo unimplemented");
    }

    ctx->r2 = 0;
}

extern "C" void osPiGetStatus_recomp(RDRAM_ARG recomp_context * ctx) {
    ctx->r2 = 0;
}

extern "C" void osPiRawStartDma_recomp(RDRAM_ARG recomp_context * ctx) {
    ultramodern::error_handling::message_box(
        "Stub `osPiRawStartDma_recomp` function called!\n"
        "Most games do not call this function directly, which means the libultra function\n"
        "that uses this function was not properly named.\n"
        "\n"
        "If you triggered this message, please make sure you have properly identified\n"
        "every libultra function on your recompiled game. If you are sure every libultra\n"
        "function has been identified and you still get this problem then open an issue on\n"
        "the N64ModernRuntime Github repository mentioning the game you are trying to\n"
        "recompile and steps to reproduce the issue.\n"
        "\n"
        "The application will close now, bye and good luck!"
    );
    ULTRAMODERN_QUICK_EXIT();
}

extern "C" void osEPiRawStartDma_recomp(RDRAM_ARG recomp_context * ctx) {
    ultramodern::error_handling::message_box(
        "Stub `osEPiRawStartDma_recomp` function called!\n"
        "Most games do not call this function directly, which means the libultra function\n"
        "that uses this function was not properly named.\n"
        "\n"
        "If you triggered this message, please make sure you have properly identified\n"
        "every libultra function on your recompiled game. If you are sure every libultra\n"
        "function has been identified and you still get this problem then open an issue on\n"
        "the N64ModernRuntime Github repository mentioning the game you are trying to\n"
        "recompile and steps to reproduce the issue.\n"
        "\n"
        "The application will close now, bye and good luck!"
    );
    ULTRAMODERN_QUICK_EXIT();
}
