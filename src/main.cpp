#include "audio/nam_audio.h"
#include "daisy_seed.h"
#include "models/a2_lite.h"
#include <atomic>
#include <cstdint>

daisy::DaisySeed seed;
static NamAudio audio;
static std::atomic<uint32_t> audio_callbacks{0};
static std::atomic<uint32_t> max_callback_us{0};
static std::atomic<uint32_t> total_callback_us{0};
static std::atomic<uint32_t> overruns{0};
static std::atomic<uint32_t> requested_amp{1};
static std::atomic<bool> requested_cabinet{true};
static std::atomic<bool> bootloader_requested{false};
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Audio counters must be lock-free");
// Only changed while audio is stopped.
static bool bypass = false;
static uint32_t ticks_per_us = 1; // Set in InitHardware after clocks are configured.
// Wall-clock time one audio block covers; the callback must finish within it.
static constexpr uint32_t kBlockBudgetUs = static_cast<uint32_t>(NamAudio::kBlockSize * 1000000.0 / NamAudio::kSampleRate);

static void RecordCallbackTiming(uint32_t elapsed_us) {
    // Raise the peak if this is the longest callback since the last status line.
    uint32_t previous = max_callback_us.load(std::memory_order_relaxed);
    while (elapsed_us > previous && !max_callback_us.compare_exchange_weak(previous, elapsed_us, std::memory_order_relaxed)) {
    }

    // Add to the totals the status line averages and counts from.
    total_callback_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    if (elapsed_us >= kBlockBudgetUs)
        overruns.fetch_add(1, std::memory_order_relaxed);
    audio_callbacks.fetch_add(1, std::memory_order_relaxed);
}

void AudioCallback(daisy::AudioHandle::InputBuffer input_channels, daisy::AudioHandle::OutputBuffer output_channels, size_t frame_count) {
    // Measure in raw timer ticks: GetUs() wraps every ~17.9 s (2^32 ticks at
    // 240 MHz), so subtracting two GetUs() values across the wrap yields
    // garbage. Tick subtraction is exact modulo 2^32.
    const uint32_t start_tick = daisy::System::GetTick();

    // Left input in, same signal out on both channels.
    audio.Process(input_channels[0], output_channels[0], output_channels[1], frame_count, bypass);

    // Record how long the block took.
    const uint32_t elapsed_ticks = daisy::System::GetTick() - start_tick;
    RecordCallbackTiming(elapsed_ticks / ticks_per_us);
}

// USB interrupt: record a command only. Never allocate or construct a model
// here.
void OnUsbReceive(uint8_t *data, uint32_t *byte_count) {
    // '0'-'3' select bypass or an amp; 'C' toggles the cabinet; 'B' asks for
    // the DFU bootloader.
    for (uint32_t i = 0; i < *byte_count; ++i) {
        if (data[i] >= '0' && data[i] <= '3')
            requested_amp.store(data[i] - '0', std::memory_order_relaxed);
        else if (data[i] == 'C' || data[i] == 'c')
            requested_cabinet.store(!requested_cabinet.load(std::memory_order_relaxed), std::memory_order_relaxed);
        else if (data[i] == 'B')
            bootloader_requested.store(true, std::memory_order_relaxed);
    }
}

// Sent by scripts/enter_dfu.sh so make upload works without the BOOT and
// RESET buttons. Drives the BOOT pin high and resets into the STM32 ROM DFU
// bootloader; does not return.
static void EnterBootloaderIfRequested() {
    if (!bootloader_requested.load(std::memory_order_relaxed))
        return;

    // Stop audio and announce the reboot.
    seed.StopAudio();
    seed.PrintLine(">>> rebooting into DFU bootloader");
    seed.DelayMs(50); // Let the USB transfer finish before the reset.
    daisy::System::ResetToBootloader();
}

struct AppState {
    AmpId selected_amp = AmpId::Fender;
    uint32_t current_command = 1;
    bool model_ready = false;
    uint32_t last_report_ms = 0;
    bool led_on = false;
    uint32_t report_count = 0;
#ifdef A2_LITE_PROFILE
    uint32_t profile_blocks = 0;
    uint32_t input_mark = 0;
    uint32_t layer_marks[a2_lite::kLayers] = {};
    uint32_t head_mark = 0;
#endif
};

#ifdef A2_LITE_PROFILE
// Prints average cycles per block for each A2-Lite stage since the last
// table. cyc/MAC divides a layer's cycles by its dilated-tap multiply-adds
// (kernel * 9 per frame), so it is comparable across kernel sizes.
static void ReportProfile(AppState &state) {
    const uint32_t blocks = state.profile_blocks;
    state.profile_blocks = 0;
    if (blocks == 0)
        return;

    // Average cycles per block since the last table, from a running counter.
    const auto take = [blocks](volatile uint32_t &counter, uint32_t &mark) {
        const uint32_t now = counter;
        const uint32_t delta = now - mark;
        mark = now;
        return delta / blocks;
    };

    // Table header and the input stage.
    const uint32_t input = take(a2_lite::profile::input_cycles, state.input_mark);
    uint32_t total = input;
    seed.PrintLine("=== A2-Lite cycles/block over %lu blocks (budget %lu cycles) ===", static_cast<unsigned long>(blocks), static_cast<unsigned long>(daisy::System::GetSysClkFreq() / 1000000 * kBlockBudgetUs));
    seed.PrintLine("  input     %6lu", static_cast<unsigned long>(input));

    // One row per layer, with cycles per multiply-add.
    for (int i = 0; i < a2_lite::kLayers; ++i) {
        const uint32_t cycles = take(a2_lite::profile::layer_cycles[i], state.layer_marks[i]);
        total += cycles;
        const uint32_t macs = static_cast<uint32_t>(a2_lite::kKernelSizes[i]) * a2_lite::kWeightsPerTap * NamAudio::kBlockSize;
        const uint32_t cyc_per_mac_x100 = cycles * 100 / macs;
        seed.PrintLine("  L%02d k%2d d%3d %6lu  %lu.%02lu cyc/MAC", i, a2_lite::kKernelSizes[i], a2_lite::kDilations[i], static_cast<unsigned long>(cycles), static_cast<unsigned long>(cyc_per_mac_x100 / 100), static_cast<unsigned long>(cyc_per_mac_x100 % 100));
    }

    // Head and total.
    const uint32_t head = take(a2_lite::profile::head_cycles, state.head_mark);
    total += head;
    seed.PrintLine("  head      %6lu", static_cast<unsigned long>(head));
    seed.PrintLine("  total     %6lu", static_cast<unsigned long>(total));
}
#endif

static void PrintHeader() { seed.PrintLine("--- NAM A2-Lite | %lu-sample blocks @ 48 kHz | budget %lu us/block | keys: 0=bypass 1=Twin65 2=AC30 3=JCM800 C=cab B=DFU ---", static_cast<unsigned long>(NamAudio::kBlockSize), static_cast<unsigned long>(kBlockBudgetUs)); }

static void InitHardware() {
    seed.Init(true); // 480 MHz boost; the model needs the headroom.
    ticks_per_us = daisy::System::GetTickFreq() / 1000000;

    // Audio format: 48 kHz, 48-sample blocks.
    seed.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    seed.SetAudioBlockSize(NamAudio::kBlockSize);

    // Serial logging without waiting for a host to connect.
    seed.StartLog(false);
#ifdef A2_LITE_PROFILE
    a2_lite::profile::EnableCycleCounter();
#endif

    // Route incoming USB bytes to the key handler.
    seed.usb_handle.SetReceiveCallback(OnUsbReceive, daisy::UsbHandle::FS_INTERNAL);
}

static void ApplyPendingAmpCommand(AppState &state) {
    // Do nothing unless the USB handler has recorded a new command.
    const uint32_t command = requested_amp.load(std::memory_order_relaxed);
    if (command == state.current_command)
        return;

    // Stop DMA before changing the model or its state. Construction and
    // prewarming may allocate and take longer than an audio block.
    seed.StopAudio();

    // Command 0 is bypass; anything else loads that amp.
    bypass = command == 0;
    if (!bypass) {
        state.selected_amp = static_cast<AmpId>(command);
        state.model_ready = audio.LoadAmpModel(state.selected_amp);
    }
    state.current_command = command;

    // Resume audio and report the result.
    seed.StartAudio(AudioCallback);
    if (bypass)
        seed.PrintLine(">>> bypass");
    else
        seed.PrintLine(">>> %s %s", AmpName(state.selected_amp), state.model_ready ? "loaded" : "FAILED to load");
}

static void ApplyPendingCabinetCommand() {
    // Do nothing unless the USB handler has toggled the cabinet.
    const bool enabled = requested_cabinet.load(std::memory_order_relaxed);
    if (enabled == audio.CabinetEnabled())
        return;

    // Stop DMA so the callback never sees the filter mid-reset.
    seed.StopAudio();
    audio.SetCabinet(enabled);
    seed.StartAudio(AudioCallback);
    seed.PrintLine(">>> cabinet %s", enabled ? "on" : "off");
}

static void ReportStatus(AppState &state) {
    // Report once per second.
    const uint32_t now_ms = daisy::System::GetNow();
    if (now_ms - state.last_report_ms < 1000)
        return;

    // Toggle the LED, and print the header every 20 status lines.
    state.last_report_ms = now_ms;
    state.led_on = !state.led_on;
    seed.SetLed(state.led_on);
    if (state.report_count++ % 20 == 0)
        PrintHeader();

    // Read and reset the counters the audio callback has been filling.
    const uint32_t callbacks = audio_callbacks.exchange(0);
    const uint32_t max_us = max_callback_us.exchange(0);
    const uint32_t total_us = total_callback_us.exchange(0);
    const uint32_t overrun_count = overruns.exchange(0);

    // Turn them into averages, percentages of the budget, and headroom.
    const uint32_t avg_us = callbacks ? total_us / callbacks : 0;
    const uint32_t avg_pct = avg_us * 100 / kBlockBudgetUs;
    const uint32_t peak_pct = max_us * 100 / kBlockBudgetUs;
    const int32_t headroom_us = static_cast<int32_t>(kBlockBudgetUs) - static_cast<int32_t>(max_us);
    const bool active = !bypass && state.model_ready;

    // Print the status line.
    const char *mode = bypass ? "bypass" : (state.model_ready ? "active" : "failed");
    const char *cabinet = active ? (audio.CabinetEnabled() ? "on" : "off") : "-";
    seed.PrintLine("[%-6s %-22s]  cab %-3s  avg %3lu%% (%4lu us)  peak %3lu%% (%4lu us)  headroom %5ld us  blocks %4lu  overruns %lu%s", mode, active ? AmpName(state.selected_amp) : "-", cabinet, static_cast<unsigned long>(avg_pct), static_cast<unsigned long>(avg_us), static_cast<unsigned long>(peak_pct), static_cast<unsigned long>(max_us), static_cast<long>(headroom_us), static_cast<unsigned long>(callbacks), static_cast<unsigned long>(overrun_count), overrun_count ? "  <-- OVERRUN" : "");
#ifdef A2_LITE_PROFILE
    state.profile_blocks += callbacks;
    if (state.report_count % 5 == 0)
        ReportProfile(state);
#endif
}

int main() {
    InitHardware();

    // Load the default amp and start audio.
    AppState state;
    state.model_ready = audio.LoadAmpModel(state.selected_amp);
    seed.StartAudio(AudioCallback);
    state.last_report_ms = daisy::System::GetNow();

    // Audio runs in the callback; this loop handles commands and reporting.
    while (true) {
        EnterBootloaderIfRequested();
        ApplyPendingAmpCommand(state);
        ApplyPendingCabinetCommand();
        ReportStatus(state);
        seed.DelayMs(1);
    }
}
