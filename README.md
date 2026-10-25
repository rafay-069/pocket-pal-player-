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

52 parts are soldered on the board. The speaker and battery are not soldered — they plug in with small connectors.
ESP32-WROOM-32E ×1 — the brain, Bluetooth + runs everything
MAX98357A ×1 — amplifier, makes the sound loud
TP4056 ×1 — charges the battery
AP2112K-3.3 ×1 — makes 3.3V power for the ESP32
1.3 inch OLED screen (SH1106) ×1 — shows the song name
USB-C port (GCT USB4105) ×1 — for charging
microSD socket ×1 — where the memory card goes
JST-PH connectors ×2 — battery and speaker plugs
Pin header 1×6 ×1 — for uploading code
Small buttons (6×6mm) ×7 — controls + boot + reset
Slide switch ×1 — power on/off
LEDs (red + green) ×2 — charging lights
Resistors ×12 — small parts that set things like charging speed and USB detection
Capacitors ×20 — smooth the power and stop button glitches
Speaker 28mm 8 ohm 2W ×1 — plugs in, plays the sound
Battery 600mAh LiPo ×1 — plugs in, powers everything

---


# pocket-pal-player-
