# Daisy Seed Looper Pedal

Two-footswitch looper for guitar. Mono in, mono out. Up to 60 seconds in SDRAM.

## Controls

### SW1 — Record / Overdub (red LED)
| Current state | Action           |
|---------------|------------------|
| Idle          | Start recording  |
| Recording     | Stop → Play      |
| Playing       | Enter overdub    |
| Overdubbing   | Stop overdub     |
| Stopped       | Resume + overdub |

### SW2 — Play / Stop (green LED) · Long press = Clear
| Current state | Action           |
|---------------|------------------|
| Idle          | Nothing          |
| Recording     | Stop → Play      |
| Playing       | Stop             |
| Overdubbing   | Stop + stop play |
| Stopped       | Resume playing   |
| Any (1s hold) | Clear loop       |

## LED cheat sheet
| LED          | Behaviour         | Meaning            |
|--------------|-------------------|--------------------|
| Red (SW1)    | Off               | Idle / playing     |
| Red (SW1)    | Fast blink 200ms  | Recording          |
| Red (SW1)    | Slow blink 400ms  | Overdubbing        |
| Green (SW2)  | Off               | Idle               |
| Green (SW2)  | Solid             | Playing            |
| Green (SW2)  | Slow blink 800ms  | Stopped            |

## Pin assignments
| Function         | Daisy pin |
|------------------|-----------|
| SW1 (Rec/OD)     | D0        |
| SW2 (Play/Stop)  | D1        |
| Red LED          | D13       |
| Green LED        | D14       |
| Audio in (L)     | Codec     |
| Audio out (L/R)  | Codec     |

Footswitches wire between pin and GND. Internal pull-ups are enabled.
LEDs wire between pin and GND through a 220Ω resistor.

## Building

```bash
# Clone dependencies alongside this folder
git clone https://github.com/electro-smith/libDaisy
git clone https://github.com/electro-smith/DaisySP

# Build
make

# Flash (hold BOOT, tap RESET on Seed first)
make program-dfu
```

## VS Code setup

See the Electrosmith VS Code template:
https://github.com/electro-smith/DaisyExamples

Recommended extensions:
- C/C++ (Microsoft)
- Cortex-Debug
- LinkerScript (if you want to browse the linker files)
