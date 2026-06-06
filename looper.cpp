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
    COUNTING,    // count-in clicks before first record
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

// Metronome click voice: noise → filter → envelope
WhiteNoise click_noise;
Svf        click_filter;
AdEnv      click_env;
static const float CLICK_LEVEL = 0.3f;   // click volume in the mix

// Count-in timing
static const float COUNT_IN_BPM   = 87.f;  // hardcoded for now; encoder later
static const int   COUNT_IN_BEATS = 4;
uint32_t beat_samples     = 0;   // samples per beat, set in main()
uint32_t count_in_counter = 0;   // sample counter within the current beat
int      count_in_beats   = 0;   // how many clicks have fired

bool armed = false;   // overdub armed, waiting for the loop to restart

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
    armed       = false;
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
            count_in_counter = beat_samples;  // makes the first click fire instantly
            count_in_beats   = 0;
            state            = LooperState::COUNTING;
            break;

        case LooperState::COUNTING:
            state = LooperState::IDLE;   // press again = cancel
            break;

        case LooperState::RECORDING:
            play_head = 0;
            state     = LooperState::PLAYING;
            break;

        case LooperState::PLAYING:
            armed = !armed;   // arm overdub, or cancel if already armed
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

        case LooperState::COUNTING:
            state = LooperState::IDLE;
            break;

        case LooperState::RECORDING:
            play_head = 0;
            state     = LooperState::PLAYING;
            break;

        case LooperState::PLAYING:
            armed = false;            // cancel pending arm
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

        case LooperState::COUNTING:
            blink_rec.SetBlink(100);
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
            blink_play.SetBlink(400);  // slow green blink = stopped/paused
            break;
    }
}
// ─────────────────────────────────────────
//  Update OLED — state + loop time + progress bar
// ─────────────────────────────────────────
void UpdateDisplay() {
    static uint32_t last_update_ms = 0;
    uint32_t now = System::GetNow();
    
    // Throttle to 20 fps to avoid hogging the I2C bus
    if (now - last_update_ms < 50) return;
    last_update_ms = now;
    
    oled.Fill(false);
    
    // ── Top line: state name ──
    oled.SetCursor(0, 0);
    const char* state_text;
    switch (state) {
        case LooperState::IDLE:        state_text = "IDLE";    break;
        case LooperState::COUNTING:    state_text = "COUNT";   break;
        case LooperState::RECORDING:   state_text = "REC";     break;
        case LooperState::PLAYING:     state_text = "PLAY";    break;
        case LooperState::OVERDUBBING: state_text = "OVERDUB"; break;
        case LooperState::STOPPED:     state_text = "STOP";    break;
    }
    oled.WriteString(state_text, Font_11x18, true);
    if (state == LooperState::COUNTING) {
        char beat_buf[4];
        snprintf(beat_buf, sizeof(beat_buf), "%d", count_in_beats);
        oled.SetCursor(108, 8);
        oled.WriteString(beat_buf, Font_11x18, true);
    }
    
    // ── Top-right: ARMED cue (blinking) takes priority over time ──
    if (state == LooperState::PLAYING && armed) {
        if ((now / 250) % 2 == 0) {           // ~2 Hz blink
            oled.SetCursor(80, 4);
            oled.WriteString("ARMED", Font_6x8, true);
        }
    } else if (loop_length > 0) {
        uint32_t cur_sec   = play_head   / static_cast<uint32_t>(SAMPLE_RATE);
        uint32_t total_sec = loop_length / static_cast<uint32_t>(SAMPLE_RATE);

        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%lu:%02lu/%lu:%02lu",
                 cur_sec / 60, cur_sec % 60,
                 total_sec / 60, total_sec % 60);

        oled.SetCursor(74, 4);
        oled.WriteString(time_buf, Font_6x8, true);
    }
    
    // ── Bottom: progress bar ──
    if (loop_length > 0) {
        // Bar runs from x=0 to x=127, y=24 to y=30 (6 pixels tall)
        int bar_width = (play_head * 128) / loop_length;
        
        // Outline
        for (int x = 0; x < 128; x++) {
            oled.DrawPixel(x, 22, true);
            oled.DrawPixel(x, 31, true);
        }
        oled.DrawPixel(0, 22, true);
        oled.DrawPixel(127, 31, true);
        
        // Filled portion
        for (int x = 0; x < bar_width; x++) {
            for (int y = 23; y < 31; y++) {
                oled.DrawPixel(x, y, true);
            }
        }
    }
    
    oled.Update();
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
        // Click voice — silent unless click_env was triggered
        float n       = click_noise.Process();
        click_filter.Process(n);
        float env_val = click_env.Process();
        float click   = click_filter.Low() * env_val * CLICK_LEVEL;
        float wet = 0.f;

        switch (state) {
            case LooperState::IDLE:
                wet = dry;
                break;

            case LooperState::COUNTING:
                wet = dry;  // monitor your guitar while counting in
                if (count_in_counter >= beat_samples) {
                    count_in_counter = 0;
                    if (count_in_beats >= COUNT_IN_BEATS) {
                        loop_length = 0;
                        play_head   = 0;
                        state       = LooperState::RECORDING;  // downbeat!
                    } else {
                        click_env.Trigger();
                        count_in_beats++;
                    }
                }
                count_in_counter++;
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
                    wet       = loop_buffer[play_head] + dry;
                    play_head = (play_head + 1) % loop_length;
                    // Armed overdub punches in exactly at the loop top
                    if (armed && play_head == 0) {
                        armed = false;
                        state = LooperState::OVERDUBBING;
                    }
                }
                break;

            case LooperState::OVERDUBBING:
                if (loop_length > 0) {
                    float old              = loop_buffer[play_head];
                    loop_buffer[play_head] = (old * 0.75f) + (dry * 0.5f);  // what gets stored
                    wet                    = old + dry;                      // what you hear
                    play_head              = (play_head + 1) % loop_length;
                }
                break;

            case LooperState::STOPPED:
                wet = dry; // pass guitar through when stopped
                break;
        }

        out[0][i] = wet + click;
        out[1][i] = wet + click;
    }
}

// ─────────────────────────────────────────
//  Main
// ─────────────────────────────────────────
int main() {
    hw.Init();

    MyOled::Config disp_cfg;
    oled.Init(disp_cfg);

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

    // click track
    click_noise.Init();
    click_noise.SetAmp(1.f);

    click_filter.Init(SAMPLE_RATE);
    click_filter.SetFreq(2000.f);   // lower = duller/more muted, higher = sharper pick
    click_filter.SetRes(0.3f);

    click_env.Init(SAMPLE_RATE);
    click_env.SetTime(ADENV_SEG_ATTACK, 0.001f);  // 1ms snap
    click_env.SetTime(ADENV_SEG_DECAY,  0.040f);  // 40ms decay = percussive tick
    click_env.SetMin(0.f);
    click_env.SetMax(1.f);

    beat_samples = (uint32_t)((60.f / COUNT_IN_BPM) * SAMPLE_RATE);

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
        UpdateDisplay();

        daisy::System::Delay(1);
    }
}