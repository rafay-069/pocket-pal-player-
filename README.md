# Pal Player

Hey! This is my project — a small music player, about the size of a keychain. I designed the whole thing myself, the PCB, the parts, everything.

It plays music in two ways. You can connect your phone to it with Bluetooth and play songs like a normal speaker, or you can put MP3 songs on a microSD card and it plays them on its own, no phone needed.

## What it can do

- Play music from your phone over Bluetooth
- Play MP3s from a microSD card by itself
- Show the song name on a small OLED screen
- Buttons for previous, play/pause, next, and volume up/down
- Charge with USB-C (same cable as most phones now)
- A red light while charging, green when the battery is full
- A slide switch to fully turn it off

## How it works (simple version)

The ESP32 chip is the brain of the player. It gets the music — either from Bluetooth or from the SD card — and sends it to a small amplifier chip (MAX98357A), which makes it loud enough for the speaker. The battery is a 600mAh LiPo, and a TP4056 chip handles the charging from USB-C. Another small chip (AP2112) gives the ESP32 a clean 3.3V to run on.

## Details

- **Board size:** 75 × 45 mm, 2 layers
- **Main chip:** ESP32-WROOM-32E
- **Amplifier:** MAX98357A
- **Speaker:** 28mm, 8 ohm, 2W
- **Screen:** 1.3 inch OLED, 128×64
- **Battery:** 600mAh LiPo
- **Charging:** USB-C, about 580mA
- **Case:** 3D printed, 83 × 54 mm

## Parts list (BOM)
## Bill of Materials (BOM)

| # | Component | Specifications / Package | Qty | Function |
| :-: | :--- | :--- | :-: | :--- |
| 1 | ESP32-WROOM-32E | Dual-Core 240MHz, SMD Module | 1 | Main Microcontroller, Bluetooth Classic & Audio Decoding |
| 2 | MAX98357A | QFN-16 / I2S Class-D Mono Amp | 1 | Digital Audio Amplifier (Speaker Output) |
| 3 | TP4056 | SOP-8 / ESOP-8 Linear Charger | 1 | Li-Ion Battery Charging Management |
| 4 | AP2112K-3.3 | SOT-23-5, 600mA Low-Dropout LDO | 1 | 3.3V System Power Regulation |
| 5 | 1.3" OLED Display | 128×64, SH1106 Driver (I2C) | 1 | Track Title, Status & Face UI Display |
| 6 | USB-C Port | GCT USB4105 (16-Pin Hybrid) | 1 | 5V Power Input & Battery Recharging |
| 7 | MicroSD Socket | Push-Pull SMD (SPI Mode) | 1 | Memory Card Storage for MP3 Files |
| 8 | JST-PH Connectors | 2-Pin, 2.0mm Pitch Male Header | 2 | Pluggable Sockets for Battery & Speaker |
| 9 | Pin Header 1×6 | 2.54mm Pitch Through-Hole | 1 | UART Programming & Firmware Flashing Port |
| 10 | Tactile Push Buttons | 6×6mm Momentary Switches | 7 | Media Controls, Volume, Boot & Reset |
| 11 | Slide Switch | SPDT Right-Angle SMD / THT | 1 | Master Hardware Power ON / OFF |
| 12 | Status LEDs | 0805 SMD (Red + Green) | 2 | Battery Charging (CHRG) & Full (STDBY) Indicators |
| 13 | Resistors | 0805 / 0603 SMD Assorted | 12 | USB-C CC Pull-Downs, I2C/SPI Pull-Ups & LED Limiters |
| 14 | Capacitors | 0805 / 0603 SMD Ceramic (MLCC) | 20 | Power Rail Decoupling & Button Debounce Filtering |
| 15 | Mini Speaker | 28mm, 8Ω, 2W (JST-PH Harness) | 1 | Acoustic Audio Output (Plugs In, Not Soldered) |
| 16 | LiPo Battery | 3.7V, 600mAh (JST-PH Harness) | 1 | Portable System Power (Plugs In, Not Soldered) |

# pocket-pal-player-
