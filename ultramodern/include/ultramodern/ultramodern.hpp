#ifndef __ultramodern_HPP__
#define __ultramodern_HPP__

#include <thread>
#include <cassert>
#include <stdexcept>
#include <span>
#include <chrono>
#include <filesystem>
#include <vector>

#undef MOODYCAMEL_DELETE_FUNCTION
#define MOODYCAMEL_DELETE_FUNCTION = delete
#include "lightweightsemaphore.h"

#include "ultra64.h"

#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/rsp.hpp"
#include "ultramodern/threads.hpp"

struct UltraThreadContext {
    std::thread host_thread;
    moodycamel::LightweightSemaphore running;
    moodycamel::LightweightSemaphore initialized;
};

namespace ultramodern {

constexpr uint32_t save_size = 1024 * 1024 / 8; // Maximum save size, 1Mbit for flash

// Initialization.
void preinit(RDRAM_ARG renderer::WindowHandle window_handle);
void init_saving(RDRAM_ARG1);
void init_events(RDRAM_ARG renderer::WindowHandle window_handle);
void init_timers(RDRAM_ARG1);
void init_thread_cleanup();

// Saving
void change_save_file(const std::u8string& subfolder, const std::u8string& name);
std::filesystem::path get_save_file_path();
// Returns the directory under which save subfolders and files are stored.
std::filesystem::path get_save_root_path();
// Returns the exact byte count accepted by import_save_file.
size_t get_save_file_size();
// Changes the save root. When load_existing is true, the active save is loaded
// from the new root; otherwise the current in-memory save is written there.
bool set_save_root_path(const std::filesystem::path& root, bool load_existing);
// Flushes pending writes and reports whether the active save reached storage.
bool flush_save_file();
// Produces a consistent snapshot after flushing pending writes.
bool snapshot_save_file(std::vector<uint8_t>& snapshot);
// Replaces the active save atomically, preserving the existing backup behavior.
// The data size must exactly match the configured save type.
bool import_save_file(std::span<const uint8_t> data);
// Reloads the active save from storage (including backup recovery).
bool reload_save_file();

// Thread queues.
constexpr PTR(PTR(OSThread)) running_queue = (PTR(PTR(OSThread)))-1;

void thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue, PTR(OSThread) toadd);
PTR(OSThread) thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue);
bool thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_);
bool thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue);
PTR(OSThread) thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue);

// Message queues.
enum class EventMessageSource : int {
    Timer,
    Sp,
    Si,
    Ai,
    Vi,
    Pi,
    Dp,
};

struct MessageQueueControl {
    bool requeue_timer = true;
    bool requeue_sp = true;
    bool requeue_si = true;
    bool requeue_ai = false;
    bool requeue_vi = false;
    bool requeue_pi = false;
    bool requeue_dp = true;
};
void set_message_queue_control(const MessageQueueControl& mqc);
void enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src);
void enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked);
void wait_for_external_message(RDRAM_ARG1);
void wait_for_external_message_timed(RDRAM_ARG1, u32 millis);

// Thread scheduling.
void check_running_queue(RDRAM_ARG1);
void run_next_thread_and_wait(RDRAM_ARG1);
void resume_thread_and_wait(RDRAM_ARG OSThread* t);
void schedule_running_thread(RDRAM_ARG PTR(OSThread) t);
void cleanup_thread(UltraThreadContext* thread_context);
struct thread_terminated : std::exception {};

enum class ThreadPriority {
    Low,
    Normal,
    High,
    VeryHigh,
    Critical
};

void set_native_thread_name(const std::string& name);
void set_native_thread_priority(ThreadPriority pri);
PTR(OSThread) this_thread();
void set_main_thread();
bool is_game_thread();
void submit_rsp_task(RDRAM_ARG PTR(OSTask) task);
void send_si_message();
uint32_t get_speed_multiplier();

// Time
std::chrono::high_resolution_clock::time_point get_start();
std::chrono::high_resolution_clock::duration time_since_start();
void measure_input_latency();
void sleep_milliseconds(uint32_t millis);
void sleep_until(const std::chrono::high_resolution_clock::time_point& time_point);
// Pause/resume only the VI scheduler while the app is backgrounded or unfocused.
// This is not an emulated clock pause: timers and osGetTime/osGetCount remain
// based on host monotonic time, and VI work resumes against current time.
void set_vi_scheduler_paused(bool paused);
bool is_vi_scheduler_paused();
// Backwards-compatible aliases for older platform lifecycle code. Prefer the
// VI scheduler names above when adding new call sites so this is not confused
// with a full emulated-time pause.
void set_app_paused(bool paused);
bool is_app_paused();

// Graphics
uint32_t get_target_framerate(uint32_t original);
uint32_t get_display_refresh_rate();
float get_resolution_scale();
void trigger_config_action();

// Audio
void init_audio();
void set_audio_frequency(uint32_t freq);
void queue_audio_buffer(RDRAM_ARG PTR(s16) audio_data, uint32_t byte_count);
uint32_t get_remaining_audio_bytes();

struct audio_callbacks_t {
    using queue_samples_t = void(int16_t*, size_t);
    using get_samples_remaining_t = size_t();
    using set_frequency_t = void(uint32_t);
    queue_samples_t* queue_samples;
    get_samples_remaining_t* get_frames_remaining;
    set_frequency_t* set_frequency;
};

// TODO: Most of the members of this struct are not used by ultramodern. Should we move them to librecomp instead?
struct gfx_callbacks_t {
    using gfx_data_t = void*;
    using create_gfx_t = gfx_data_t();
    using create_window_t = renderer::WindowHandle(gfx_data_t);
    using update_gfx_t = void(gfx_data_t);

    create_gfx_t* create_gfx;
    create_window_t* create_window;
    update_gfx_t* update_gfx;
};

bool is_game_started();
void quit();
void join_event_threads();
void join_thread_cleaner_thread();
void join_saving_thread();

void set_audio_callbacks(const audio_callbacks_t& callbacks);

/**
 * Register all the callbacks used by `ultramodern`, most of them being optional.
 *
 * The following arguments contain mandatory callbacks that need to be registered (i.e., can't be `nullptr`):
 * - `rsp_callbacks`
 * - `renderer_callbacks`
 *
 * It must be called only once and it must be called before `ultramodern::preinit`.
 */
void set_callbacks(
    const rsp::callbacks_t& rsp_callbacks,
    const renderer::callbacks_t& renderer_callbacks,
    const audio_callbacks_t& audio_callbacks,
    const input::callbacks_t& input_callbacks,
    const gfx_callbacks_t& gfx_callbacks,
    const events::callbacks_t& events_callbacks,
    const error_handling::callbacks_t& error_handling_callbacks,
    const threads::callbacks_t& threads_callbacks
);
} // namespace ultramodern

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define debug_printf(...)
//#define debug_printf(...) printf(__VA_ARGS__);

#endif
