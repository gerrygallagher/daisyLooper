#include "daisy_seed.h"
#include "daisysp.h"
#include "dev/oled_ssd130x.h"
#include "hid/disp/oled_display.h"

using namespace daisy;
using namespace daisysp;

// ─────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────
static const size_t LOOP_BUFFER_SECONDS = 60;
static const float  SAMPLE_RATE         = 48000.f;
static const size_t LOOP_BUFFER_SAMPLES = static_cast<size_t>(LOOP_BUFFER_SECONDS * SAMPLE_RATE);

static const uint32_t LONG_PRESS_MS = 1000;  // hold SW2 to clear

// Pin assignments
static const Pin PIN_SW1 = seed::D0;   // Record / Overdub footswitch
static const Pin PIN_SW2 = seed::D1;   // Play / Stop footswitch
static const Pin PIN_LED_REC  = seed::D13; // Red LED  — record status
static const Pin PIN_LED_PLAY = seed::D14; // Green LED — play status

// ─────────────────────────────────────────
//  State machine
// ─────────────────────────────────────────
enum class LooperState {
    IDLE,        // nothing recorded yet
    RECORDING,   // capturing input to buffer
    PLAYING,     // looping playback
    OVERDUBBING, // layering new audio onto loop
    STOPPED      // loop exists but playback is paused
};

// ─────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────
DaisySeed hw;
using MyOled = daisy::OledDisplay<daisy::SSD130xI2c128x32Driver>;  
MyOled oled;

float DSY_SDRAM_BSS loop_buffer[LOOP_BUFFER_SAMPLES];

LooperState state       = LooperState::IDLE;
size_t      loop_length = 0;
size_t      play_head   = 0;

Switch sw1, sw2;
GPIO   led_rec, led_play;

// ─────────────────────────────────────────
//  LED blinker — non-blocking
// ─────────────────────────────────────────
struct Blinker {
    enum class Mode { OFF, ON, BLINK };
    Mode     mode      = Mode::OFF;
    uint32_t period_ms = 500;
    uint32_t last_ms   = 0;
    bool     state     = false;

    void SetOff()                       { mode = Mode::OFF; }
    void SetOn()                        { mode = Mode::ON;  }
    void SetBlink(uint32_t period)      { mode = Mode::BLINK; period_ms = period; }

    void Update(GPIO& led) {
        if (mode == Mode::OFF) { led.Write(false); return; }
        if (mode == Mode::ON)  { led.Write(true);  return; }
        uint32_t now = System::GetNow();
        if (now - last_ms >= period_ms) {
            state = !state;
            led.Write(state);
            last_ms = now;
        }
    }
};

Blinker blink_rec, blink_play;

// ─────────────────────────────────────────
//  Long-press detector for SW2
// ─────────────────────────────────────────
// ─────────────────────────────────────────
//  SW2 press detector — distinguishes short vs long press
//
//  Returns:
//    1 = short press (fires on release, only if held < LONG_PRESS_MS)
//    2 = long press  (fires immediately when threshold crossed)
//    0 = nothing
// ─────────────────────────────────────────
struct PressDetector {
    uint32_t press_start  = 0;
    bool     held         = false;
    bool     long_fired   = false;

    int Update(Switch& sw) {
        sw.Debounce();
        bool pressed = sw.Pressed();

        // Just went down — start timing
        if (pressed && !held) {
            press_start = System::GetNow();
            held        = true;
            long_fired  = false;
        }

        // Held long enough — fire long press once
        if (held && !long_fired && (System::GetNow() - press_start >= LONG_PRESS_MS)) {
            long_fired = true;
            return 2;
        }

        // Released — fire short press only if it wasn't a long press
        if (!pressed && held) {
            held = false;
            if (!long_fired) return 1;
        }

        return 0;
    }
};

PressDetector sw2_press;

// ─────────────────────────────────────────
//  Buffer helpers
// ─────────────────────────────────────────
void ClearLoop() {
    const int flash_count = 3;
    const int flash_ms = 100;

    for (size_t i = 0; i < flash_count; i++)
    {
        led_rec.Write(true);
        led_play.Write(true);
        daisy::System::Delay(flash_ms);
        led_rec.Write(false);
        led_play.Write(false);
        daisy::System::Delay(flash_ms);
    }
    loop_length = 0;
    play_head   = 0;
    state       = LooperState::IDLE;
    for (size_t i = 0; i < LOOP_BUFFER_SAMPLES; i++) loop_buffer[i] = 0.f;
}

// ─────────────────────────────────────────
//  Switch 1 — Record / Overdub
//
//  IDLE        → start recording
//  RECORDING   → stop recording, go to PLAYING
//  PLAYING     → enter overdub
//  OVERDUBBING → stop overdub, back to PLAYING
//  STOPPED     → enter overdub (also resumes playback)
// ─────────────────────────────────────────
void HandleSW1() {
    switch (state) {
        case LooperState::IDLE:
            loop_length = 0;
            play_head   = 0;
            state       = LooperState::RECORDING;
            break;

        case LooperState::RECORDING:
            play_head = 0;
            state     = LooperState::PLAYING;
            break;

        case LooperState::PLAYING:
            state = LooperState::OVERDUBBING;
            break;

        case LooperState::OVERDUBBING:
            state = LooperState::PLAYING;
            break;

        case LooperState::STOPPED:
            play_head = 0;
            state     = LooperState::OVERDUBBING;
            break;
    }
}

// ─────────────────────────────────────────
//  Switch 2 — Play / Stop  (long press = clear)
//
//  IDLE        → nothing (no loop to play)
//  RECORDING   → stop recording + play  (same as SW1 — convenience)
//  PLAYING     → stop (freeze playhead)
//  OVERDUBBING → stop overdub + stop playback
//  STOPPED     → resume playing
// ─────────────────────────────────────────
void HandleSW2Short() {
    switch (state) {
        case LooperState::IDLE:
            break; // nothing to do

        case LooperState::RECORDING:
            play_head = 0;
            state     = LooperState::PLAYING;
            break;

        case LooperState::PLAYING:
            state = LooperState::STOPPED;
            break;

        case LooperState::OVERDUBBING:
            state = LooperState::STOPPED;
            break;

        case LooperState::STOPPED:
            play_head = 0;
            state     = LooperState::PLAYING;
            break;
    }
}

// ─────────────────────────────────────────
//  LED logic
// ─────────────────────────────────────────
void UpdateLeds() {
    switch (state) {
        case LooperState::IDLE:
            blink_rec.SetOff();
            blink_play.SetOff();
            break;

        case LooperState::RECORDING:
            blink_rec.SetBlink(200);   // fast red blink = recording
            blink_play.SetOff();
            break;

        case LooperState::PLAYING:
            blink_rec.SetOff();
            blink_play.SetOn();        // solid green = playing
            break;

        case LooperState::OVERDUBBING:
            blink_rec.SetBlink(400);   // slow red blink = overdubbing
            blink_play.SetOn();        // green stays on (still playing)
            break;

        case LooperState::STOPPED:
            blink_rec.SetOff();
            blink_play.SetBlink(800);  // slow green blink = stopped/paused
            break;
    }
}

// ─────────────────────────────────────────
//  Audio callback
// ─────────────────────────────────────────
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    for (size_t i = 0; i < size; i++) {
        float dry = in[0][i];
        float wet = 0.f;

        switch (state) {
            case LooperState::IDLE:
                wet = dry;
                break;

            case LooperState::RECORDING:
                if (loop_length < LOOP_BUFFER_SAMPLES) {
                    loop_buffer[loop_length++] = dry;
                } else {
                    // Buffer full — auto-commit and play
                    play_head = 0;
                    state     = LooperState::PLAYING;
                }
                wet = dry; // monitor while recording
                break;

            case LooperState::PLAYING:
                if (loop_length > 0) {
                    wet       = loop_buffer[play_head];
                    play_head = (play_head + 1) % loop_length;
                }
                break;

            case LooperState::OVERDUBBING:
                if (loop_length > 0) {
                    // 0.75 feedback prevents clipping on repeated overdubs
                    loop_buffer[play_head] = (loop_buffer[play_head] * 0.75f) + (dry * 0.5f);
                    wet       = loop_buffer[play_head];
                    play_head = (play_head + 1) % loop_length;
                }
                break;

            case LooperState::STOPPED:
                wet = dry; // pass guitar through when stopped
                break;
        }

        out[0][i] = wet;
        out[1][i] = wet;
    }
}

// ─────────────────────────────────────────
//  Main
// ─────────────────────────────────────────
int main() {
    hw.Init();

    MyOled::Config disp_cfg;
    oled.Init(disp_cfg);
    oled.Fill(false);
    oled.SetCursor(0, 0);
    oled.WriteString("BOOTED", Font_7x10, true);
    oled.Update();

    hw.SetAudioBlockSize(4);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    // Footswitches — active-low, internal pull-up
    sw1.Init(PIN_SW1, 1000.f, Switch::Type::TYPE_MOMENTARY,
             Switch::Polarity::POLARITY_INVERTED, Switch::Pull::PULL_UP);
    sw2.Init(PIN_SW2, 1000.f, Switch::Type::TYPE_MOMENTARY,
             Switch::Polarity::POLARITY_INVERTED, Switch::Pull::PULL_UP);

    // LEDs
    led_rec.Init(PIN_LED_REC,  GPIO::Mode::OUTPUT);
    led_play.Init(PIN_LED_PLAY, GPIO::Mode::OUTPUT);

    // Clear SDRAM buffer
    for (size_t i = 0; i < LOOP_BUFFER_SAMPLES; i++) loop_buffer[i] = 0.f;

    hw.StartAudio(AudioCallback);

    while (true) {
        // SW1 — check for short press
        sw1.Debounce();
        if (sw1.RisingEdge()) HandleSW1();

        // SW2 — detect short press (on release) or long press (on hold)
        int sw2_action = sw2_press.Update(sw2);
        if (sw2_action == 2) {
            ClearLoop();
        } else if (sw2_action == 1) {
            HandleSW2Short();
        }

        UpdateLeds();
        blink_rec.Update(led_rec);
        blink_play.Update(led_play);

        daisy::System::Delay(1);
    }
}