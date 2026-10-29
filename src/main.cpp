#include "audio/nam_audio.h"
#include "daisy_seed.h"
#include "fx/effects.h"
#include "link/slp_receiver.h"
#include "models/a2_lite.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

daisy::DaisySeed seed;
static NamAudio audio;
static std::atomic<uint32_t> audio_callbacks{0};
static std::atomic<uint32_t> max_callback_us{0};
static std::atomic<uint32_t> total_callback_us{0};
static std::atomic<uint32_t> overruns{0};
static std::atomic<uint32_t> requested_amp{0};
static std::atomic<bool> requested_cabinet{true};
static std::atomic<bool> bootloader_requested{false};
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Audio counters must be lock-free");

// Master volume and mute from the touchscreen, and peaks for its meters. The
// defaults (gain 1) leave the sound as it was when no touchscreen is connected.
static std::atomic<float> master_target{1.0f};
static float master_gain = 1.0f; // Audio callback only.

// The effects (touchscreen repo seed-dsp/effects): gate, comp and drive before the amp model,
// chorus, delay and reverb after it, in the touchscreen's chain order. The delay's 196 KB go in the
// SDRAM (it reads one line); the reverb reads 12 lines every sample, too slow from the SDRAM (about
// 75 us a block), so its 100 KB go in the DTCM (the stack keeps the top 28 KB); the chorus's 8 KB
// in the ordinary RAM.
static float DSY_SDRAM_BSS delay_mem[fx::Delay::kMemFloats];
static float DTCM_MEM_SECTION reverb_mem[fx::Reverb::kMemFloats];
static float chorus_mem[fx::Chorus::kMemFloats];
static fx::Rig rig;
static std::atomic<char> fx_key{0};               // console key for the effects, from the USB interrupt
static std::atomic<bool> stm_dfu_requested{false}; // 'U': reboot into the STM32's DFU
static std::atomic<float> in_peak{0.0f};
static std::atomic<float> out_peak{0.0f};
static std::atomic<uint32_t> clip_count{0};
static_assert(std::atomic<float>::is_always_lock_free, "Audio atomics must be lock-free");

// Touchscreen link: USART1 on D13/D14 at 1 Mbaud, received by DMA.
static daisy::UartHandler link_uart;
// DMA can only reach some RAM; libDaisy's section macro puts the buffer there.
static uint8_t DMA_BUFFER_MEM_SECTION link_dma_buf[256];
// Single-producer / single-consumer ring between the DMA callback and the main loop.
static uint8_t link_ring[2048];
static std::atomic<uint32_t> ring_head{0};
static std::atomic<uint32_t> ring_tail{0};
static slp_receiver_t link_rx;

// SD amp profiles uploaded by the touchscreen: the receiver writes and CRC-checks into the
// staging buffer; a selected one is copied out at once (the next upload may overwrite staging),
// and into the running buffer only while audio is stopped (A2Lite keeps pointers into it).
static uint8_t upload_staging[A2Lite::kWeights * sizeof(float)];
static float upload_pending[A2Lite::kWeights];
static float upload_running[A2Lite::kWeights];
static bool upload_requested = false;
static uint32_t upload_requested_hash = 0;
// requested_amp / current_command value while an uploaded model runs.
static constexpr uint32_t kUploadedCommand = 255;
// Only changed while audio is stopped.
static bool bypass = true;
// ADC channels for the carrier board's pots: volume (RV2) on Daisy pin 22
// (A0) and tone (RV3) on pin 23 (A1).
enum Knob : uint8_t { kVolumeKnob, kToneKnob, kKnobCount };
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

// Raise a meter peak if this block's is higher; the main loop resets it.
static void RaisePeak(std::atomic<float> &peak, float value) {
    float previous = peak.load(std::memory_order_relaxed);
    while (value > previous && !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

void AudioCallback(daisy::AudioHandle::InputBuffer input_channels, daisy::AudioHandle::OutputBuffer output_channels, size_t frame_count) {
    // Measure in raw timer ticks: GetUs() wraps every ~17.9 s (2^32 ticks at
    // 240 MHz), so subtracting two GetUs() values across the wrap yields
    // garbage. Tick subtraction is exact modulo 2^32.
    const uint32_t start_tick = daisy::System::GetTick();

    // Pass on the latest knob readings, which the ADC's DMA keeps current.
    audio.SetKnobs(seed.adc.GetFloat(kVolumeKnob), seed.adc.GetFloat(kToneKnob));

    // Left input in, through the pedals, the amp and the effects after it, same signal out on
    // both channels (the master loop below copies it).
    float pre[NamAudio::kBlockSize];
    const size_t n = frame_count < NamAudio::kBlockSize ? frame_count : NamAudio::kBlockSize;
    std::copy(input_channels[0], input_channels[0] + n, pre);
    rig.ProcessPre(pre, n);
    audio.Process(pre, output_channels[0], output_channels[1], n, bypass);
    rig.ProcessPost(output_channels[0], n);

    // Ramp to the touchscreen's master gain across the block (no clicks; mute
    // reaches silence within one block) and record peaks for its meters.
    const float target = master_target.load(std::memory_order_relaxed);
    const float step = (target - master_gain) / static_cast<float>(frame_count);
    float in_max = 0.0f;
    float out_max = 0.0f;
    for (size_t i = 0; i < frame_count; ++i) {
        master_gain += step;
        output_channels[0][i] *= master_gain;
        output_channels[1][i] = output_channels[0][i];
        in_max = std::max(in_max, std::fabs(input_channels[0][i]));
        out_max = std::max(out_max, std::fabs(output_channels[0][i]));
    }
    master_gain = target;
    if (out_max >= 1.0f)
        clip_count.fetch_add(1, std::memory_order_relaxed);
    RaisePeak(in_peak, in_max);
    RaisePeak(out_peak, out_max);

    // Record how long the block took.
    const uint32_t elapsed_ticks = daisy::System::GetTick() - start_tick;
    RecordCallbackTiming(elapsed_ticks / ticks_per_us);
}

// USB interrupt: record a command only. Never allocate or construct a model
// here.
void OnUsbReceive(uint8_t *data, uint32_t *byte_count) {
    // '0'-'4' select bypass or an amp; 'C' toggles the cabinet; 'B' asks for
    // the DFU bootloader; effect keys (see ApplyFxKey) toggle an effect.
    for (uint32_t i = 0; i < *byte_count; ++i) {
        if (data[i] >= '0' && data[i] <= '4')
            requested_amp.store(data[i] - '0', std::memory_order_relaxed);
        else if (data[i] == 'C' || data[i] == 'c')
            requested_cabinet.store(!requested_cabinet.load(std::memory_order_relaxed), std::memory_order_relaxed);
        else if (data[i] == 'B')
            bootloader_requested.store(true, std::memory_order_relaxed);
        else if (data[i] == 'U') {
            stm_dfu_requested.store(true, std::memory_order_relaxed);
            bootloader_requested.store(true, std::memory_order_relaxed);
        }
        else if (strchr("DRSGKOMHdrsgkomh", data[i]) && data[i])
            fx_key.store(static_cast<char>(data[i] & ~0x20), std::memory_order_relaxed);
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
#ifdef BOOT_APP
    // Run by the Daisy bootloader: 'B' goes back to it and it waits for a new app (in QSPI); 'U'
    // goes to the STM32's own DFU, which can replace the bootloader itself (or restore a backup).
    daisy::System::ResetToBootloader(stm_dfu_requested.load(std::memory_order_relaxed) ? daisy::System::BootloaderMode::STM
                                                                                        : daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
#else
    daisy::System::ResetToBootloader();
#endif
}

struct AppState {
    AmpId selected_amp = AmpId::Fender;
    uint32_t current_command = 0;
    // An SD profile uploaded by the touchscreen is running (its CRC32), rather than a built-in.
    bool uploaded = false;
    uint32_t uploaded_hash = 0;
    bool model_ready = false;
    uint32_t last_report_ms = 0;
    bool led_on = false;
    uint32_t report_count = 0;
    bool knobs_reported = false;
    uint32_t reported_knob_percent[kKnobCount] = {};
    // Last measured block timing and overruns since boot, for the touchscreen's STATUS.
    uint32_t last_avg_us = 0;
    uint32_t last_max_us = 0;
    uint32_t total_overruns = 0;
    uint32_t last_status_ms = 0;
    uint32_t last_meters_ms = 0;
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

static void PrintHeader() { seed.PrintLine("--- NAM A2-Lite | %lu-sample blocks @ 48 kHz | budget %lu us/block | keys: 0=bypass 1=Twin65 2=AC30 3=JCM800 4=Princeton C=cab G=gate K=comp O=drive M=fuzz H=chorus D=delay R=hall S=spring B=DFU ---", static_cast<unsigned long>(NamAudio::kBlockSize), static_cast<unsigned long>(kBlockBudgetUs)); }

static void InitHardware() {
    seed.Init(true); // 480 MHz boost; the model needs the headroom.
    ticks_per_us = daisy::System::GetTickFreq() / 1000000;

    // Audio format: 48 kHz, 48-sample blocks.
    seed.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    seed.SetAudioBlockSize(NamAudio::kBlockSize);

    // Read the volume and tone pots continuously in the background.
    daisy::AdcChannelConfig knobs[kKnobCount];
    knobs[kVolumeKnob].InitSingle(daisy::seed::A0);
    knobs[kToneKnob].InitSingle(daisy::seed::A1);
    seed.adc.Init(knobs, kKnobCount);
    seed.adc.Start();

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
        state.uploaded = false;
    }
    state.current_command = command;

    // Resume audio and report the result.
    seed.StartAudio(AudioCallback);
    if (bypass)
        seed.PrintLine(">>> bypass");
    else
        seed.PrintLine(">>> %s %s", AmpName(state.selected_amp), state.model_ready ? "loaded" : "FAILED to load");
}

// Switches to an SD profile the touchscreen selected (OnSelectModel copied its weights).
static void ApplyPendingUpload(AppState &state) {
    if (!upload_requested)
        return;
    upload_requested = false;

    // The touchscreen resends its whole state every 5 s: don't reload a model that's running.
    if (state.uploaded && state.uploaded_hash == upload_requested_hash && !bypass && state.model_ready)
        return;

    // Stop DMA, swap the weights the engine reads, and build the model on them.
    seed.StopAudio();
    memcpy(upload_running, upload_pending, sizeof(upload_running));
    bypass = false;
    state.model_ready = audio.LoadUploadedModel(upload_running);
    state.uploaded = true;
    state.uploaded_hash = upload_requested_hash;
    state.current_command = kUploadedCommand;
    requested_amp.store(kUploadedCommand, std::memory_order_relaxed);

    // Resume audio and report the result.
    seed.StartAudio(AudioCallback);
    seed.PrintLine(">>> SD profile %08lx %s", static_cast<unsigned long>(state.uploaded_hash), state.model_ready ? "loaded" : "FAILED to load");
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
    seed.PrintLine(">>> cabinet %s%s", enabled ? "on" : "off", audio.CabinetApplies() ? "" : " (this amp's capture has its own, so it is skipped)");
}

// Prints the knob readings at startup and whenever either has moved by 2%
// or more since last printed. They stay off the status line, which is
// already near libDaisy's 128-byte print limit.
static void ReportKnobs(AppState &state) {
    // Read both knobs as a percentage of full travel.
    uint32_t percent[kKnobCount];
    for (int knob = 0; knob < kKnobCount; ++knob)
        percent[knob] = static_cast<uint32_t>(seed.adc.GetFloat(knob) * 100.0f + 0.5f);

    // Stay quiet unless one has moved further than ADC noise would take it.
    bool moved = !state.knobs_reported;
    for (int knob = 0; knob < kKnobCount; ++knob) {
        const uint32_t previous = state.reported_knob_percent[knob];
        moved = moved || percent[knob] >= previous + 2 || previous >= percent[knob] + 2;
    }
    if (!moved)
        return;

    // Remember what was printed and print it.
    state.knobs_reported = true;
    for (int knob = 0; knob < kKnobCount; ++knob)
        state.reported_knob_percent[knob] = percent[knob];
    seed.PrintLine(">>> volume %lu%%  tone %lu%%", static_cast<unsigned long>(percent[kVolumeKnob]), static_cast<unsigned long>(percent[kToneKnob]));
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
    state.last_avg_us = avg_us;
    state.last_max_us = max_us;
    state.total_overruns += overrun_count;
    const bool active = !bypass && state.model_ready;

    // Print the status line.
    const char *mode = bypass ? "bypass" : (state.model_ready ? "active" : "failed");
    // "amp" means the capture includes its own cabinet, so the EQ is skipped.
    const char *cabinet = !active ? "-" : !audio.CabinetApplies() ? "amp" : audio.CabinetEnabled() ? "on" : "off";
    seed.PrintLine("[%-6s %-22s]  cab %-3s  avg %3lu%% (%4lu us)  peak %3lu%% (%4lu us)  headroom %5ld us  blocks %4lu  overruns %lu%s", mode, active ? (state.uploaded ? "SD profile" : AmpName(state.selected_amp)) : "-", cabinet, static_cast<unsigned long>(avg_pct), static_cast<unsigned long>(avg_us), static_cast<unsigned long>(peak_pct), static_cast<unsigned long>(max_us), static_cast<long>(headroom_us), static_cast<unsigned long>(callbacks), static_cast<unsigned long>(overrun_count), overrun_count ? "  <-- OVERRUN" : "");
    ReportKnobs(state);
#ifdef A2_LITE_PROFILE
    state.profile_blocks += callbacks;
    if (state.report_count % 5 == 0)
        ReportProfile(state);
#endif
}

// UART DMA interrupt: copy bytes only. If the ring is full, drop them; the
// protocol resyncs.
static void OnLinkRx(uint8_t *data, size_t size, void *, daisy::UartHandler::Result result);

// Starts DMA reception with line-error interrupts off. libDaisy's error
// callback leaves listen mode but keeps the IDLE interrupt enabled, and its
// IRQ handler only clears the IDLE flag in listen mode, so a single framing
// error (the touchscreen's 115200-baud boot banner arrives at 1 Mbaud) became
// an endless USART1 interrupt that starved the main loop. Without the error
// interrupts the DMA still delivers a damaged byte, and the protocol's CRC
// drops the frame.
static void StartLinkListening() {
    link_uart.DmaListenStart(link_dma_buf, sizeof(link_dma_buf), OnLinkRx, nullptr);
    USART1->CR3 &= ~USART_CR3_EIE;
    USART1->CR1 &= ~USART_CR1_PEIE;
}

static void OnLinkRx(uint8_t *data, size_t size, void *, daisy::UartHandler::Result result) {
    if (result != daisy::UartHandler::Result::OK)
        return;
    uint32_t head = ring_head.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size; ++i) {
        const uint32_t next = (head + 1) % sizeof(link_ring);
        if (next == ring_tail.load(std::memory_order_acquire))
            break;
        link_ring[head] = data[i];
        head = next;
    }
    ring_head.store(head, std::memory_order_release);
}

// The built-in amps for HELLO, in AmpId order, named by AmpName (filled in InitLink).
static slp_builtin_t builtins[kAmpCount];

static void LinkSend(void *, const uint8_t *bytes, size_t len) {
    // At most ~270 bytes, under 3 ms at 1 Mbaud; audio keeps running in its interrupt.
    link_uart.BlockingTransmit(const_cast<uint8_t *>(bytes), len);
}

static uint32_t LinkNow(void *) { return daisy::System::GetNow(); }

// Volume is 0..100. A squared curve is close to an audio taper and needs no libm.
static void OnMaster(void *, uint8_t volume, bool muted) {
    const float v = volume / 100.0f;
    master_target.store(muted ? 0.0f : v * v, std::memory_order_relaxed);
}

// Same path as the USB keys: record the request; ApplyPendingAmpCommand does the switch.
static void OnSelectModel(void *, const slp_select_model_t *m, const uint8_t *weights) {
    // An SD profile the receiver holds (CRC verified): take a copy and switch to it in the main
    // loop. One it doesn't hold yet keeps the current sound; the touchscreen uploads it, then
    // selects it again.
    if (m->kind == SLP_MODEL_UPLOADED) {
        if (weights) {
            memcpy(upload_pending, weights, sizeof(upload_pending));
            upload_requested_hash = m->hash;
            upload_requested = true;
        }
        return;
    }
    if (m->kind == SLP_MODEL_BUILTIN && m->builtin_id >= 1 && m->builtin_id <= kAmpCount)
        requested_amp.store(m->builtin_id, std::memory_order_relaxed);
    else if (m->kind == SLP_MODEL_BYPASS)
        requested_amp.store(0, std::memory_order_relaxed);
}

static void OnFx(void *, uint8_t fx, bool on, uint8_t model) {
    if (fx == SLP_FX_CAB)
        requested_cabinet.store(on, std::memory_order_relaxed);
    rig.SetFx(fx, on, model); // ignores the effects it doesn't run
}

static void OnParam(void *, uint16_t id, int16_t value) { rig.SetParam(id, value); }

static void OnChain(void *, const uint8_t *fx, uint8_t len) { rig.SetChain(fx, len); }

// Console keys, for bench tests without the touchscreen: each toggles one effect at the
// touchscreen's default settings for that model.
static void ApplyFxKey() {
    const char key = fx_key.exchange(0, std::memory_order_relaxed);
    struct Key {
        char key;
        uint8_t fx, model;
        int16_t knobs[fx::kMaxKnobs];
        const char *name;
    };
    static const Key kKeys[] = {
        {'G', SLP_FX_GATE, 0, {35, 20, 40, 70}, "gate"},
        {'K', SLP_FX_COMP, 0, {45, 55, 25, 50, 50}, "compressor"},
        {'O', SLP_FX_DRIVE, 0, {45, 60, 60}, "drive (Tube Screamer)"},
        {'M', SLP_FX_DRIVE, 1, {75, 45, 50}, "drive (Big Muff)"},
        {'H', SLP_FX_CHORUS, 2, {55, 70, 50}, "chorus (Ensemble)"},
        {'D', SLP_FX_DELAY, 0, {45, 38, 60, 35}, "delay (Digital)"},
        {'R', SLP_FX_REVERB, 0, {65, 25, 55, 35}, "reverb (Hall)"},
        {'S', SLP_FX_REVERB, 3, {35, 5, 45, 30}, "reverb (Spring)"},
    };
    for (const Key &k : kKeys) {
        if (k.key != key)
            continue;
        const bool on = !(rig.On(k.fx) && rig.Model(k.fx) == k.model);
        rig.SetFx(k.fx, on, k.model);
        for (int i = 0; i < fx::kMaxKnobs; ++i)
            rig.SetParam(static_cast<uint16_t>(k.fx << 8 | i), k.knobs[i]);
        seed.PrintLine(">>> %s %s", k.name, on ? "on" : "off");
    }
}

static void OnLink(void *, bool up) { seed.PrintLine(">>> touchscreen link %s", up ? "up" : "lost"); }

static void InitLink() {
    // UART1 on D13/D14 at 1 Mbaud, received by DMA.
    daisy::UartHandler::Config uart{};
    uart.periph = daisy::UartHandler::Config::Peripheral::USART_1;
    uart.mode = daisy::UartHandler::Config::Mode::TX_RX;
    uart.baudrate = 1000000;
    uart.pin_config.tx = daisy::seed::D13;
    uart.pin_config.rx = daisy::seed::D14;
    link_uart.Init(uart);
    StartLinkListening();

    // The receiver: a new boot_id every boot tells the touchscreen to resend everything.
    daisy::Random::Init();
    slp_rx_config_t cfg{};
    cfg.send = LinkSend;
    cfg.now_ms = LinkNow;
    cfg.boot_id = daisy::Random::GetValue();
    cfg.fw_version = "nam-a2";
    cfg.sample_rate_hz = 48000;
    cfg.block_size = NamAudio::kBlockSize;
    for (int i = 0; i < kAmpCount; ++i) {
        builtins[i].id = static_cast<uint8_t>(i + 1);
        snprintf(builtins[i].name, sizeof(builtins[i].name), "%s", AmpName(static_cast<AmpId>(i + 1)));
    }
    cfg.builtins = builtins;
    cfg.model_buf = upload_staging;
    cfg.model_buf_size = sizeof(upload_staging);
    cfg.builtin_count = kAmpCount;
    cfg.on_master = OnMaster;
    cfg.on_select_model = OnSelectModel;
    cfg.on_fx = OnFx;
    cfg.on_param = OnParam;
    cfg.on_chain = OnChain;
    cfg.on_link = OnLink;
    slp_rx_init(&link_rx, &cfg);
}

// Every main-loop pass: hand the received bytes to the receiver.
static void PollLink() {
    // Clear line-error flags left by damaged bytes, and listen again should
    // reception ever stop.
    USART1->ICR = USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF | USART_ICR_PECF;
    if (!link_uart.IsListening()) {
        link_uart.DmaListenStop();
        StartLinkListening();
    }

    uint8_t buf[256];
    size_t n = 0;
    uint32_t tail = ring_tail.load(std::memory_order_relaxed);
    while (tail != ring_head.load(std::memory_order_acquire) && n < sizeof(buf)) {
        buf[n++] = link_ring[tail];
        tail = (tail + 1) % sizeof(link_ring);
    }
    ring_tail.store(tail, std::memory_order_release);
    if (n)
        slp_rx_feed(&link_rx, buf, n);
    slp_rx_poll(&link_rx);
}

static int16_t PeakToCentiDb(float x) { return x > 1e-6f ? static_cast<int16_t>(2000.0f * log10f(x)) : SLP_METER_SILENCE; }

// STATUS twice a second, METERS about 30 times a second.
static void SendTelemetry(AppState &state) {
    const uint32_t now_ms = daisy::System::GetNow();
    if (now_ms - state.last_status_ms >= 500) {
        state.last_status_ms = now_ms;
        slp_status_t st{};
        st.cpu_avg_x10 = static_cast<uint16_t>(state.last_avg_us * 1000 / kBlockBudgetUs);
        st.cpu_peak_x10 = static_cast<uint16_t>(state.last_max_us * 1000 / kBlockBudgetUs);
        st.overruns = state.total_overruns;
        st.model_kind = bypass ? SLP_MODEL_BYPASS : state.uploaded ? SLP_MODEL_UPLOADED : SLP_MODEL_BUILTIN;
        st.builtin_id = bypass || state.uploaded ? 0 : static_cast<uint8_t>(state.selected_amp);
        st.model_hash = state.uploaded && !bypass ? state.uploaded_hash : 0;
        st.flags = SLP_STATUS_AUDIO_RUNNING | (bypass ? SLP_STATUS_BYPASS : 0) | (!bypass && !state.model_ready ? SLP_STATUS_MODEL_FAILED : 0);
        slp_rx_send_status(&link_rx, &st);
    }
    if (now_ms - state.last_meters_ms >= 33) {
        state.last_meters_ms = now_ms;
        slp_meters_t m{};
        m.in_peak_cdb = PeakToCentiDb(in_peak.exchange(0.0f));
        m.out_peak_cdb = PeakToCentiDb(out_peak.exchange(0.0f));
        m.clip_count = clip_count.load(std::memory_order_relaxed);
        slp_rx_send_meters(&link_rx, &m);
    }
}

int main() {
    InitHardware();
    rig.Init(delay_mem, reverb_mem, chorus_mem); // the SDRAM is up after seed.Init()
    InitLink();

    // Boot in bypass with no model loaded, start from where the knobs are, and
    // start audio. Keys 1-4 load an amp.
    AppState state;
    audio.SetKnobs(seed.adc.GetFloat(kVolumeKnob), seed.adc.GetFloat(kToneKnob));
    audio.ResetKnobs();
    seed.StartAudio(AudioCallback);
    state.last_report_ms = daisy::System::GetNow();

    // Audio runs in the callback; this loop handles commands and reporting.
    while (true) {
        EnterBootloaderIfRequested();
        ApplyPendingAmpCommand(state);
        ApplyFxKey();
        ApplyPendingCabinetCommand();
        PollLink();
        ApplyPendingUpload(state);
        ReportStatus(state);
        SendTelemetry(state);
        seed.DelayMs(1);
    }
}
