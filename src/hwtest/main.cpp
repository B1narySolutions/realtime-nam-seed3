// Hardware test firmware: turns the Seed3 into a two-channel audio interface
// that the host drives over USB serial. The host uploads a stimulus, the
// firmware plays it on both outputs while recording both inputs in the same
// audio callbacks, then sends the recording back. With a cable from an output
// jack to the input jack, that is a loopback measurement of the whole analog
// chain; all analysis happens on the host (hwtest/).
//
// It links no amp models: the main firmware fills nearly all of the 128 KB
// of internal flash, and the audio chain under test is the same in bypass.
//
// Protocol: newline-terminated text commands, one reply line each.
//   id              HWTEST proto=1 ... (board, rates, limits, SDRAM check)
//   put <frames>    then frames * 2 float32 (L, R interleaved, little-endian)
//                   as raw bytes; replies OK put <frames> crc=<crc32>
//   run <frames>    plays the first frames of the stimulus, records both inputs
//   knobs <blocks>  records the raw volume and tone ADC readings once a block
//   load <percent>  busy-waits each callback to that share of the budget
// run and knobs reply with a DATA line, the raw float32 recording and an END
// line. Errors reply ERR <reason>. A 'B' byte reboots into DFU, as in the main
// firmware, so make upload works from this firmware too.
#include "daisy_seed.h"
#include "usbd_cdc.h"
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" USBD_HandleTypeDef hUsbDeviceFS;

namespace {
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kSampleRate = 48000;
constexpr size_t kBlockSize = 48;
constexpr uint32_t kBlockBudgetUs = kBlockSize * 1000000 / kSampleRate;
constexpr size_t kChannels = 2;
// 20 s of stereo float32 is 7.7 MB per buffer; the SDRAM holds 64 MB.
constexpr size_t kMaxFrames = 20 * kSampleRate;
constexpr uint32_t kMaxLoadPercent = 95;
enum Knob : uint8_t { kVolumeKnob, kToneKnob, kKnobCount };

// Flash-resident table the synthetic load reads, so it exercises the flash
// and FPU like the model's weight reads do.
constexpr size_t kLoadTableSize = 4096;
constexpr std::array<float, kLoadTableSize> kLoadTable = [] {
    std::array<float, kLoadTableSize> table{};
    for (size_t i = 0; i < kLoadTableSize; ++i)
        table[i] = 1.0f / static_cast<float>(i + 1);
    return table;
}();
} // namespace

daisy::DaisySeed seed;

// What the outputs play, and what the inputs recorded, as interleaved L/R
// frames. Both live in SDRAM, which only the CPU touches: the USB FS core
// copies through its FIFO and the audio DMA uses libDaisy's own buffers.
DSY_SDRAM_BSS static float stimulus[kMaxFrames * kChannels];
DSY_SDRAM_BSS static float recording[kMaxFrames * kChannels];

// Capture state shared with the audio callback. The main loop sets up the
// plain fields while the state is Idle, then hands over with a release store.
enum class CaptureState : uint32_t { Idle, Running, Done };
static std::atomic<CaptureState> capture_state{CaptureState::Idle};
static uint32_t capture_frames = 0;
static uint32_t capture_position = 0;
static bool capture_knobs = false;
static std::atomic<uint32_t> load_percent{0};
static volatile float load_sink = 0.0f;

// Callback timing since the last capture started.
static std::atomic<uint32_t> callbacks{0};
static std::atomic<uint32_t> max_callback_us{0};
static std::atomic<uint32_t> overruns{0};
static uint32_t ticks_per_us = 1;

// USB receive state, owned by the interrupt. A command line is handed to the
// main loop through command_ready; an upload writes straight into stimulus.
static char rx_line[64];
static size_t rx_length = 0;
static char command[sizeof(rx_line)];
static std::atomic<bool> command_ready{false};
static uint32_t upload_remaining = 0;
static uint32_t upload_position = 0;
static uint32_t upload_frames = 0;
static bool upload_discard = false;
static std::atomic<bool> upload_done{false};
static std::atomic<bool> upload_rejected{false};
static std::atomic<bool> bootloader_requested{false};
static uint32_t stimulus_frames = 0;

static void RecordTiming(uint32_t elapsed_us) {
    // Raise the peak; only this callback writes it between captures.
    if (elapsed_us > max_callback_us.load(std::memory_order_relaxed))
        max_callback_us.store(elapsed_us, std::memory_order_relaxed);
    if (elapsed_us >= kBlockBudgetUs)
        overruns.fetch_add(1, std::memory_order_relaxed);
    callbacks.fetch_add(1, std::memory_order_relaxed);
}

// Spins until the callback has used the requested share of its budget, so
// noise measurements can be repeated with the CPU as busy as an amp model.
static void RunSyntheticLoad(uint32_t start_tick) {
    const uint32_t percent = load_percent.load(std::memory_order_relaxed);
    if (percent == 0)
        return;

    const uint32_t target_ticks = percent * kBlockBudgetUs * ticks_per_us / 100;
    float sum = 0.0f;
    size_t index = 0;
    while (daisy::System::GetTick() - start_tick < target_ticks) {
        for (size_t k = 0; k < 64; ++k)
            sum += kLoadTable[index] * kLoadTable[(index + k * 67) % kLoadTableSize];
        index = (index + 1) % kLoadTableSize;
    }
    load_sink = sum;
}

void AudioCallback(daisy::AudioHandle::InputBuffer in, daisy::AudioHandle::OutputBuffer out, size_t frame_count) {
    const uint32_t start_tick = daisy::System::GetTick();

    // Silence unless a capture is running.
    for (size_t i = 0; i < frame_count; ++i) {
        out[0][i] = 0.0f;
        out[1][i] = 0.0f;
    }

    if (capture_state.load(std::memory_order_acquire) == CaptureState::Running) {
        if (capture_knobs) {
            // One frame of raw ADC readings per block.
            recording[capture_position * kChannels] = seed.adc.Get(kVolumeKnob);
            recording[capture_position * kChannels + 1] = seed.adc.Get(kToneKnob);
            capture_position += 1;
        } else {
            // Play the stimulus and record both inputs, sample for sample.
            for (size_t i = 0; i < frame_count && capture_position + i < capture_frames; ++i) {
                const size_t frame = (capture_position + i) * kChannels;
                out[0][i] = stimulus[frame];
                out[1][i] = stimulus[frame + 1];
                recording[frame] = in[0][i];
                recording[frame + 1] = in[1][i];
            }
            capture_position += frame_count;
        }

        // Hand the recording back to the main loop once it is complete.
        if (capture_position >= capture_frames)
            capture_state.store(CaptureState::Done, std::memory_order_release);
    }

    // Optional busy work, then the block's timing.
    RunSyntheticLoad(start_tick);
    RecordTiming((daisy::System::GetTick() - start_tick) / ticks_per_us);
}

// Starts an upload of frames stereo frames into stimulus. A refused upload
// still swallows its bytes, so they can't be read as commands (a stray 'B'
// would reboot into DFU).
static void BeginUpload(const char *argument) {
    const unsigned long frames = strtoul(argument, nullptr, 10);
    if (frames == 0 || frames > 100 * kMaxFrames) {
        upload_rejected.store(true, std::memory_order_relaxed);
        return;
    }
    upload_discard = frames > kMaxFrames || capture_state.load(std::memory_order_relaxed) != CaptureState::Idle;
    upload_frames = static_cast<uint32_t>(frames);
    upload_position = 0;
    upload_remaining = upload_frames * kChannels * sizeof(float);
}

// USB interrupt: collects command lines and upload bytes. Never blocks.
void OnUsbReceive(uint8_t *data, uint32_t *byte_count) {
    auto *stimulus_bytes = reinterpret_cast<uint8_t *>(stimulus);
    for (uint32_t i = 0; i < *byte_count; ++i) {
        const uint8_t byte = data[i];

        // Upload bytes go straight into the stimulus buffer.
        if (upload_remaining > 0) {
            if (!upload_discard)
                stimulus_bytes[upload_position++] = byte;
            if (--upload_remaining == 0)
                (upload_discard ? upload_rejected : upload_done).store(true, std::memory_order_release);
            continue;
        }

        // Commands are lowercase, so a capital B is always the DFU request.
        if (byte == 'B') {
            bootloader_requested.store(true, std::memory_order_relaxed);
            continue;
        }

        // Collect characters until the end of the line.
        if (byte != '\n' && byte != '\r') {
            if (rx_length < sizeof(rx_line) - 1)
                rx_line[rx_length++] = static_cast<char>(byte);
            continue;
        }
        if (rx_length == 0)
            continue;
        rx_line[rx_length] = '\0';
        rx_length = 0;

        // An upload has to start here, before its first byte arrives; any
        // other command waits for the main loop (and is dropped if one is
        // already waiting, which the host never does).
        if (strncmp(rx_line, "put ", 4) == 0)
            BeginUpload(rx_line + 4);
        else if (!command_ready.load(std::memory_order_acquire)) {
            memcpy(command, rx_line, sizeof(command));
            command_ready.store(true, std::memory_order_release);
        }
    }
}

// Blocks until the USB IN endpoint has finished its last transfer, so the
// buffer it was reading can be reused. Gives up after timeout_ms.
static bool WaitTxIdle(uint32_t timeout_ms) {
    const uint32_t start = daisy::System::GetNow();
    for (;;) {
        const auto *cdc = static_cast<USBD_CDC_HandleTypeDef *>(hUsbDeviceFS.pClassData);
        if (cdc == nullptr || cdc->TxState == 0)
            return cdc != nullptr;
        if (daisy::System::GetNow() - start > timeout_ms)
            return false;
    }
}

// Sends bytes to the host in chunks, waiting for each to finish. Returns
// false if the host stops reading. 4000 is not a multiple of the 64-byte
// packet size, so every full chunk ends in a short packet the host sees.
static bool Send(const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    while (size > 0) {
        const size_t chunk = size < 4000 ? size : 4000;
        if (!WaitTxIdle(1000))
            return false;
        if (seed.usb_handle.TransmitInternal(const_cast<uint8_t *>(bytes), chunk) != daisy::UsbHandle::Result::OK)
            return false;
        bytes += chunk;
        size -= chunk;
    }
    return WaitTxIdle(1000);
}

// printf to the host, one line. Integers only: nano.specs has no float printf.
static void Print(const char *format, ...) {
    static char line[192];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length < 0)
        return;
    if (length > static_cast<int>(sizeof(line)) - 2)
        length = sizeof(line) - 2;
    line[length++] = '\n';
    Send(line, length);
}

// Standard CRC-32 (as zlib.crc32), so the host can check every transfer.
static uint32_t Crc32(const void *data, size_t size) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        table_ready = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// Writes and reads back both SDRAM buffers. Returns the first failing word
// index, or -1 if every word matched.
static int32_t TestSdram() {
    static_assert(sizeof(float) == sizeof(uint32_t));
    constexpr size_t kWords = kMaxFrames * kChannels;
    const std::array<uint32_t *, 2> buffers{reinterpret_cast<uint32_t *>(stimulus), reinterpret_cast<uint32_t *>(recording)};

    // A pattern that differs in every address bit, and its complement.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t flip = pass ? 0xFFFFFFFFu : 0u;
        for (size_t b = 0; b < buffers.size(); ++b)
            for (size_t i = 0; i < kWords; ++i)
                buffers[b][i] = (static_cast<uint32_t>(i) * 2654435761u + b) ^ flip;
        for (size_t b = 0; b < buffers.size(); ++b)
            for (size_t i = 0; i < kWords; ++i)
                if (buffers[b][i] != ((static_cast<uint32_t>(i) * 2654435761u + b) ^ flip))
                    return static_cast<int32_t>(b * kWords + i);
    }
    return -1;
}

// Runs a capture of frame_count frames (blocks, for knobs) and sends it back.
static void Capture(uint32_t frame_count, bool knobs) {
    // Set up while the callback ignores the plain fields, then hand over.
    capture_frames = frame_count;
    capture_position = 0;
    capture_knobs = knobs;
    callbacks.store(0, std::memory_order_relaxed);
    max_callback_us.store(0, std::memory_order_relaxed);
    overruns.store(0, std::memory_order_relaxed);
    capture_state.store(CaptureState::Running, std::memory_order_release);

    // Wait for the callback to finish it, with a generous timeout in case
    // audio has stopped.
    const uint32_t expected_ms = knobs ? frame_count : frame_count / (kSampleRate / 1000);
    const uint32_t start = daisy::System::GetNow();
    while (capture_state.load(std::memory_order_acquire) != CaptureState::Done) {
        if (daisy::System::GetNow() - start > expected_ms + 2000) {
            capture_state.store(CaptureState::Idle, std::memory_order_relaxed);
            Print("ERR capture timed out: is audio running?");
            return;
        }
    }
    capture_state.store(CaptureState::Idle, std::memory_order_relaxed);

    // Header, raw recording, trailer.
    const size_t bytes = frame_count * kChannels * sizeof(float);
    Print("DATA kind=%s frames=%lu channels=%u bytes=%lu crc=%08lx callbacks=%lu max_us=%lu overruns=%lu load=%lu", knobs ? "knobs" : "audio", static_cast<unsigned long>(frame_count), static_cast<unsigned>(kChannels), static_cast<unsigned long>(bytes), static_cast<unsigned long>(Crc32(recording, bytes)), static_cast<unsigned long>(callbacks.load()), static_cast<unsigned long>(max_callback_us.load()), static_cast<unsigned long>(overruns.load()), static_cast<unsigned long>(load_percent.load()));
    Send(recording, bytes);
    Print("END");
}

static void HandleCommand(const char *line, int32_t sdram_failure) {
    // Split the command word from its one numeric argument.
    char word[16] = {};
    unsigned long argument = 0;
    const int fields = sscanf(line, "%15s %lu", word, &argument);
    if (fields < 1) {
        Print("ERR empty command");
        return;
    }

    if (strcmp(word, "id") == 0) {
        Print("HWTEST proto=%lu board=%d sr=%lu block=%u maxframes=%lu sdram=%s sdram_fail_word=%ld sysclk_mhz=%lu", static_cast<unsigned long>(kProtocolVersion), static_cast<int>(seed.CheckBoardVersion()), static_cast<unsigned long>(kSampleRate), static_cast<unsigned>(kBlockSize), static_cast<unsigned long>(kMaxFrames), sdram_failure < 0 ? "ok" : "FAIL", static_cast<long>(sdram_failure), static_cast<unsigned long>(daisy::System::GetSysClkFreq() / 1000000));
    } else if (strcmp(word, "load") == 0 && fields == 2) {
        load_percent.store(argument > kMaxLoadPercent ? kMaxLoadPercent : argument, std::memory_order_relaxed);
        Print("OK load %lu", static_cast<unsigned long>(load_percent.load()));
    } else if (strcmp(word, "run") == 0 && fields == 2) {
        if (argument == 0 || argument > stimulus_frames)
            Print("ERR run %lu: only %lu frames uploaded", argument, static_cast<unsigned long>(stimulus_frames));
        else
            Capture(static_cast<uint32_t>(argument), false);
    } else if (strcmp(word, "knobs") == 0 && fields == 2) {
        if (argument == 0 || argument > kMaxFrames)
            Print("ERR knobs %lu: out of range", argument);
        else
            Capture(static_cast<uint32_t>(argument), true);
    } else {
        Print("ERR unknown command: %s", line);
    }
}

int main() {
    // 480 MHz, 48 kHz, 48-sample blocks: the same clocks and audio format as
    // the main firmware, so the board is tested as it runs.
    seed.Init(true);
    ticks_per_us = daisy::System::GetTickFreq() / 1000000;
    seed.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    seed.SetAudioBlockSize(kBlockSize);

    // The volume and tone pots, on the same pins as the main firmware.
    daisy::AdcChannelConfig knobs[kKnobCount];
    knobs[kVolumeKnob].InitSingle(daisy::seed::A0);
    knobs[kToneKnob].InitSingle(daisy::seed::A1);
    seed.adc.Init(knobs, kKnobCount);
    seed.adc.Start();

    // Check the SDRAM before trusting it with recordings.
    const int32_t sdram_failure = TestSdram();

    // USB serial without libDaisy's logger, whose buffering could reorder
    // text and binary data.
    seed.usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
    seed.usb_handle.SetReceiveCallback(OnUsbReceive, daisy::UsbHandle::FS_INTERNAL);
    seed.StartAudio(AudioCallback);

    uint32_t last_blink_ms = 0;
    bool led_on = false;
    while (true) {
        // Reboot into DFU if asked; does not return.
        if (bootloader_requested.load(std::memory_order_relaxed)) {
            seed.StopAudio();
            daisy::System::Delay(50);
            daisy::System::ResetToBootloader();
        }

        // Report a finished or refused upload.
        if (upload_done.exchange(false, std::memory_order_acquire)) {
            stimulus_frames = upload_frames;
            Print("OK put %lu crc=%08lx", static_cast<unsigned long>(upload_frames), static_cast<unsigned long>(Crc32(stimulus, upload_frames * kChannels * sizeof(float))));
        }
        if (upload_rejected.exchange(false, std::memory_order_relaxed))
            Print("ERR put: frames must be 1 to %lu, with no capture running", static_cast<unsigned long>(kMaxFrames));

        // Run the waiting command, then free the slot for the next.
        if (command_ready.load(std::memory_order_acquire)) {
            HandleCommand(command, sdram_failure);
            command_ready.store(false, std::memory_order_release);
        }

        // Blink fast so the test firmware is easy to tell from the main one.
        const uint32_t now = daisy::System::GetNow();
        if (now - last_blink_ms >= 100) {
            last_blink_ms = now;
            led_on = !led_on;
            seed.SetLed(led_on);
        }
    }
}
