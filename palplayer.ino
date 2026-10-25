/*
 * PalPlayer firmware
 * ------------------
 * Keychain music player based on the ESP32-WROOM-32E.
 *
 * Two audio sources:
 *   - BLUETOOTH mode : A2DP sink (phone streams to us), AVRCP for
 *                      play/pause/next/prev and track metadata.
 *   - SD mode        : MP3 files played from a microSD card (SPI).
 *
 * Long-press PLAY (>= 1.5 s) switches between the two modes. Only one
 * audio path is ever active: both want the same I2S port, so the other
 * one is fully stopped before the new one starts.
 *
 * IMPORTANT: Build with Arduino-ESP32 core 2.0.x (2.0.17).
 * The ESP32-A2DP library does NOT work on core 3.x.
 *
 * Hardware notes (from the verified schematic):
 *   - The amplifier (MAX98357A) has no software volume or shutdown:
 *     volume is done digitally (A2DP sink volume in BT mode, output
 *     gain in SD mode).
 *   - Power is a hard slide switch. No battery sensing is wired, so
 *     there is deliberately no battery gauge anywhere in this file.
 *   - The charge LEDs belong to the TP4056 charger chip, not to us.
 */

// ---------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>
#include "BluetoothA2DPSink.h"     // ESP32-A2DP by pschatzmann
#include <AudioGeneratorMP3.h>    // ESP8266Audio
#include <AudioFileSourceSD.h>    // ESP8266Audio
#include <AudioOutputI2S.h>       // ESP8266Audio

#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Pin map (authoritative, from the schematic)
// ---------------------------------------------------------------------------
static const int PIN_I2S_BCLK = 26;   // MAX98357A BCLK
static const int PIN_I2S_LRC  = 25;   // MAX98357A LRC / WS
static const int PIN_I2S_DIN  = 32;   // MAX98357A DIN

static const int PIN_I2C_SDA  = 21;   // OLED SH1106
static const int PIN_I2C_SCL  = 22;

static const int PIN_SD_MOSI  = 23;   // microSD SPI
static const int PIN_SD_MISO  = 19;
static const int PIN_SD_SCK   = 18;
static const int PIN_SD_CS    = 5;

static const int PIN_BTN_PREV = 13;   // all buttons: active LOW, internal
static const int PIN_BTN_PLAY = 14;   // pull-ups, hardware debounce caps
static const int PIN_BTN_NEXT = 27;
static const int PIN_BTN_VOLD = 16;   // VOL-
static const int PIN_BTN_VOLU = 17;   // VOL+

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const char*     BT_NAME          = "PalPlayer";
static const uint32_t  DEBOUNCE_MS      = 40;
static const uint32_t  LONG_PRESS_MS    = 1500;   // PLAY long-press = mode
static const uint32_t  VOL_REPEAT_FIRST = 400;    // hold VOL: first repeat
static const uint32_t  VOL_REPEAT_MS    = 150;    // ...then repeat period
static const uint32_t  DISPLAY_MS       = 100;    // display throttle
static const uint32_t  SCROLL_MS        = 120;    // title scroll step
static const size_t    MAX_TRACKS       = 200;
static const int       BT_VOL_STEP      = 8;      // A2DP volume: 0..127
static const int       VOL_UI_MAX       = 20;     // volume bar blocks

// ---------------------------------------------------------------------------
// Button state (defined early on purpose: Arduino's automatic prototype
// generation needs this type visible before the functions that use it)
// ---------------------------------------------------------------------------
struct Button {
  int      pin;
  bool     rawLast;        // last raw reading (HIGH = released)
  bool     pressed;        // debounced state (true = held down)
  uint32_t rawChangedAt;   // when the raw reading last changed
  uint32_t pressedAt;      // when the debounced press began
  bool     longFired;      // PLAY: long-press already acted on
  uint32_t nextRepeat;     // VOL: next hold-repeat time
};

// Explicit prototypes for the functions taking a Button& (stops the
// Arduino preprocessor from generating its own before the struct).
static void shortPress(Button &b);
static void pollButton(Button &b);

// ---------------------------------------------------------------------------
// Modes and playback states
// ---------------------------------------------------------------------------
enum Mode { MODE_BT, MODE_SD };
enum SDState { SD_STOPPED, SD_PLAYING, SD_PAUSED };

static Mode    g_mode     = MODE_BT;
static SDState g_sdState  = SD_STOPPED;
static bool    g_btConnected = false;
static bool    g_btPlaying   = false;   // from A2DP audio-state callback

// ---------------------------------------------------------------------------
// Display (SH1106 128x64, hardware I2C, full frame buffer)
// ---------------------------------------------------------------------------
static U8G2_SH1106_128X64_NONAME_F_HW_I2C
    u8g2(U8G2_R0, /* reset = */ U8X8_PIN_NONE,
         /* clock = */ PIN_I2C_SCL, /* data = */ PIN_I2C_SDA);

// What is on screen right now (also used to detect changes)
static String  g_btTitle   = "";
static String  g_btArtist  = "";
static String  g_sdTitle   = "";        // filename without extension
static String  g_message   = "";        // transient line ("NO SD CARD"...)
static uint32_t g_messageUntil = 0;

// Title scrolling
static int      g_scrollOffset = 0;
static uint32_t g_lastScroll   = 0;
static String   g_scrollTrack  = "";    // title the offset belongs to

// ---------------------------------------------------------------------------
// Bluetooth (A2DP sink)
// ---------------------------------------------------------------------------
static BluetoothA2DPSink a2dp_sink;
static int g_btVolume = 64;             // 0..127, midpoint at boot

static void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (text == nullptr) return;
  // ESP_AVRC_MD_ATTR_TITLE = 0x1, ESP_AVRC_MD_ATTR_ARTIST = 0x2
  if (id == 0x1) {
    g_btTitle = (const char *)text;
    Serial.printf("[BT] Title : %s\n", text);
  } else if (id == 0x2) {
    g_btArtist = (const char *)text;
    Serial.printf("[BT] Artist: %s\n", text);
  }
}

static void connection_state_changed(esp_a2d_connection_state_t state,
                                     void * /*ptr*/) {
  g_btConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  Serial.printf("[BT] Connection state: %d (%s)\n", (int)state,
                g_btConnected ? "connected" : "not connected");
  if (!g_btConnected) g_btPlaying = false;
}

static void audio_state_changed(esp_a2d_audio_state_t state,
                                void * /*ptr*/) {
  // ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND=0, STOPPED=1, STARTED=2
  g_btPlaying = (state == ESP_A2D_AUDIO_STATE_STARTED);
  Serial.printf("[BT] Audio state: %d (%s)\n", (int)state,
                g_btPlaying ? "playing" : "not playing");
}

static void startBluetooth() {
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = PIN_I2S_BCLK,
    .ws_io_num    = PIN_I2S_LRC,
    .data_out_num = PIN_I2S_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  a2dp_sink.set_pin_config(pins);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_on_audio_state_changed(audio_state_changed);
  a2dp_sink.set_volume((uint8_t)g_btVolume);
  a2dp_sink.start(BT_NAME);
  Serial.printf("[BT] Advertising as \"%s\"\n", BT_NAME);
}

static void stopBluetooth() {
  // end() stops the sink and releases the I2S driver so ESP8266Audio
  // can claim the same port.
  a2dp_sink.end();
  g_btConnected = false;
  g_btPlaying   = false;
  Serial.println("[BT] Sink stopped");
}

// ---------------------------------------------------------------------------
// SD card / MP3 playback (ESP8266Audio)
// ---------------------------------------------------------------------------
static AudioGeneratorMP3 *g_mp3  = nullptr;
static AudioFileSourceSD *g_file = nullptr;
static AudioOutputI2S    *g_out  = nullptr;

static std::vector<String> g_tracks;    // full paths, e.g. "/song.mp3"
static int  g_trackIndex = 0;
static int  g_uiVolume   = 12;          // SD volume, 0..20 UI scale

static bool hasMp3Extension(const String &name) {
  int dot = name.lastIndexOf('.');
  if (dot < 0) return false;
  String ext = name.substring(dot + 1);
  ext.toLowerCase();
  return ext == "mp3";
}

// Scan the card root for .mp3 files (case-insensitive), sorted, capped.
static bool scanCard() {
  g_tracks.clear();
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[SD] Mount failed (no card?)");
    return false;
  }
  File root = SD.open("/");
  if (!root) {
    Serial.println("[SD] Cannot open root directory");
    return false;
  }
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      // SD library may return a bare name or a full path; normalise.
      if (!name.startsWith("/")) name = "/" + name;
      if (hasMp3Extension(name)) {
        if (g_tracks.size() < MAX_TRACKS) g_tracks.push_back(name);
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  std::sort(g_tracks.begin(), g_tracks.end(),
            [](const String &a, const String &b) {
              String la = a, lb = b;
              la.toLowerCase();
              lb.toLowerCase();
              return la.compareTo(lb) < 0;
            });

  if (g_trackIndex >= (int)g_tracks.size()) g_trackIndex = 0;
  Serial.printf("[SD] %u MP3 file(s) found\n", (unsigned)g_tracks.size());
  return g_tracks.size() > 0;
}

static String titleFromPath(const String &path) {
  int slash = path.lastIndexOf('/');
  String t = (slash >= 0) ? path.substring(slash + 1) : path;
  int dot = t.lastIndexOf('.');
  if (dot > 0) t = t.substring(0, dot);
  return t;
}

static float uiVolumeToGain() {
  // 0..20 UI scale -> 0.0..1.0 gain (kept <= 1.0: no distortion games)
  return g_uiVolume / (float)VOL_UI_MAX;
}

static void stopSDPlayback() {
  if (g_mp3) {
    g_mp3->stop();
    delete g_mp3;
    g_mp3 = nullptr;
  }
  if (g_file) {
    delete g_file;
    g_file = nullptr;
  }
  g_sdState = SD_STOPPED;
}

// Start (or restart) the track at g_trackIndex. Returns false on error.
static bool startSDTrack() {
  stopSDPlayback();
  if (g_tracks.empty()) return false;
  if (g_out == nullptr) {
    // Default output mode is EXTERNAL_I2S (external MAX98357A DAC/amp).
    g_out = new AudioOutputI2S();
    g_out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  }
  g_out->SetGain(uiVolumeToGain());

  const String &path = g_tracks[g_trackIndex];
  g_file = new AudioFileSourceSD();
  if (!g_file->open(path.c_str())) {
    Serial.printf("[SD] Cannot open %s\n", path.c_str());
    delete g_file;
    g_file = nullptr;
    return false;
  }
  g_mp3 = new AudioGeneratorMP3();
  if (!g_mp3->begin(g_file, g_out)) {
    Serial.printf("[SD] Decoder failed on %s\n", path.c_str());
    delete g_mp3;
    g_mp3 = nullptr;
    delete g_file;
    g_file = nullptr;
    return false;
  }
  g_sdTitle = titleFromPath(path);
  g_sdState = SD_PLAYING;
  Serial.printf("[SD] Playing %d/%u: %s\n", g_trackIndex + 1,
                (unsigned)g_tracks.size(), path.c_str());
  return true;
}

// Pause/resume for ESP8266Audio. The MP3 generator has no true pause:
// pause = fully stop the decoder, resume = start the track again
// (from the beginning). Simple and crash-proof.
static void toggleSDPlayPause() {
  if (g_sdState == SD_PLAYING) {
    stopSDPlayback();
    g_sdState = SD_PAUSED;
    Serial.println("[SD] Paused (decoder stopped)");
  } else {
    if (startSDTrack()) {
      Serial.println("[SD] Resumed (track restarts)");
    } else {
      showMessage("SD ERROR");
    }
  }
}

static void nextSDTrack() {
  if (g_tracks.empty()) return;
  g_trackIndex = (g_trackIndex + 1) % (int)g_tracks.size();
  if (g_sdState != SD_PAUSED) startSDTrack();
  else g_sdTitle = titleFromPath(g_tracks[g_trackIndex]);
}

static void prevSDTrack() {
  if (g_tracks.empty()) return;
  g_trackIndex = (g_trackIndex - 1 + (int)g_tracks.size()) %
                 (int)g_tracks.size();
  if (g_sdState != SD_PAUSED) startSDTrack();
  else g_sdTitle = titleFromPath(g_tracks[g_trackIndex]);
}

// Release the SD audio output (used when leaving SD mode).
static void releaseSDOutput() {
  stopSDPlayback();
  if (g_out) {
    g_out->stop();          // frees the I2S driver
    delete g_out;
    g_out = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Mode switching
// ---------------------------------------------------------------------------
static void showMessage(const String &msg) {
  g_message = msg;
  g_messageUntil = millis() + 2000;
  Serial.printf("[UI] %s\n", msg.c_str());
}

static void enterSDMode() {
  if (g_mode == MODE_SD) return;
  Serial.println("[MODE] Switching to SD...");
  stopBluetooth();
  g_mode = MODE_SD;
  if (scanCard()) {
    g_trackIndex = 0;
    g_sdTitle = titleFromPath(g_tracks[0]);
    g_sdState = SD_PAUSED;              // start paused, per boot rule
    Serial.println("[MODE] SD mode, paused on first track");
  } else {
    // No card / no MP3s: fall straight back to Bluetooth.
    Serial.println("[MODE] No playable SD content, back to Bluetooth");
    g_mode = MODE_BT;
    showMessage("NO SD CARD");
    startBluetooth();
  }
}

static void enterBTMode() {
  if (g_mode == MODE_BT) return;
  Serial.println("[MODE] Switching to Bluetooth...");
  releaseSDOutput();
  g_mode = MODE_BT;
  startBluetooth();
}

static void toggleMode() {
  if (g_mode == MODE_BT) enterSDMode();
  else                   enterBTMode();
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static int currentVolumeBlocks() {
  if (g_mode == MODE_BT) return (g_btVolume * VOL_UI_MAX) / 127;
  return g_uiVolume;
}

static bool isPlayingNow() {
  if (g_mode == MODE_BT) return g_btPlaying;
  return g_sdState == SD_PLAYING;
}

static String currentTitle() {
  if (g_message.length() > 0 && (int32_t)(g_messageUntil - millis()) > 0)
    return g_message;
  if (g_mode == MODE_BT) return g_btTitle.length() ? g_btTitle
                                                   : String("(no title)");
  if (!g_tracks.empty()) return g_sdTitle;
  return String("(no card)");
}

static String currentSubline() {
  if (g_mode == MODE_BT) {
    if (!g_btConnected) return String("waiting for phone...");
    return g_btArtist;
  }
  if (g_tracks.empty()) return String("");
  char buf[24];
  snprintf(buf, sizeof(buf), "track %d/%u", g_trackIndex + 1,
           (unsigned)g_tracks.size());
  return String(buf);
}

// Redraw the whole screen (full-buffer U8g2). Called throttled.
static void drawDisplay() {
  String title = currentTitle();
  String sub   = currentSubline();

  // Title changed -> restart scrolling from the beginning.
  if (title != g_scrollTrack) {
    g_scrollTrack  = title;
    g_scrollOffset = 0;
  }

  u8g2.clearBuffer();

  // --- Line 1: mode + state -------------------------------------------
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, g_mode == MODE_BT ? "BLUETOOTH" : "SD CARD");
  const char *stateTxt = isPlayingNow() ? "PLAY" : "PAUS";
  u8g2.drawStr(128 - u8g2.getStrWidth(stateTxt), 10, stateTxt);

  // --- Big line: scrolling track title ---------------------------------
  u8g2.setFont(u8g2_font_7x14B_tf);
  int titleW = u8g2.getStrWidth(title.c_str());
  u8g2.setClipWindow(0, 14, 128, 36);
  if (titleW <= 126) {
    u8g2.drawStr(0, 32, title.c_str());
    g_scrollOffset = 0;
  } else {
    // Scroll left, then wrap around with a gap.
    int span = titleW + 24;
    u8g2.drawStr(-g_scrollOffset, 32, title.c_str());
    u8g2.drawStr(-g_scrollOffset + span, 32, title.c_str());
    if ((uint32_t)(millis() - g_lastScroll) >= SCROLL_MS) {
      g_lastScroll = millis();
      g_scrollOffset += 2;
      if (g_scrollOffset >= span) g_scrollOffset = 0;
    }
  }
  u8g2.setMaxClipWindow();

  // --- Sub line: artist (BT) or track n/N (SD) --------------------------
  u8g2.setFont(u8g2_font_6x10_tf);
  if (sub.length() > 21) sub = sub.substring(0, 21);
  u8g2.drawStr(0, 48, sub.c_str());

  // --- Bottom: volume bar, 20 blocks ------------------------------------
  int blocks = currentVolumeBlocks();
  u8g2.drawStr(0, 62, "VOL");
  for (int i = 0; i < VOL_UI_MAX; i++) {
    int x = 22 + i * 5;
    if (i < blocks) u8g2.drawBox(x, 56, 4, 6);
    else            u8g2.drawFrame(x, 56, 4, 6);
  }

  u8g2.sendBuffer();
}

// Cheap "did anything visible change?" snapshot for the throttle.
static String g_lastScreenKey = "";
static String screenKey() {
  String k = currentTitle() + "|" + currentSubline() + "|" +
             String(currentVolumeBlocks()) + "|" +
             String(isPlayingNow()) + "|" + String((int)g_mode) + "|" +
             String(g_scrollOffset);
  return k;
}

// ---------------------------------------------------------------------------
// Buttons (the Button struct itself lives near the top of this file)
// ---------------------------------------------------------------------------
static Button btnPrev = { PIN_BTN_PREV, true, false, 0, 0, false, 0 };
static Button btnPlay = { PIN_BTN_PLAY, true, false, 0, 0, false, 0 };
static Button btnNext = { PIN_BTN_NEXT, true, false, 0, 0, false, 0 };
static Button btnVolD = { PIN_BTN_VOLD, true, false, 0, 0, false, 0 };
static Button btnVolU = { PIN_BTN_VOLU, true, false, 0, 0, false, 0 };

static void volumeUp() {
  if (g_mode == MODE_BT) {
    g_btVolume = min(127, g_btVolume + BT_VOL_STEP);
    a2dp_sink.set_volume((uint8_t)g_btVolume);
    Serial.printf("[VOL] BT volume %d/127\n", g_btVolume);
  } else {
    g_uiVolume = min(VOL_UI_MAX, g_uiVolume + 1);
    if (g_out) g_out->SetGain(uiVolumeToGain());
    Serial.printf("[VOL] SD volume %d/%d\n", g_uiVolume, VOL_UI_MAX);
  }
}

static void volumeDown() {
  if (g_mode == MODE_BT) {
    g_btVolume = max(0, g_btVolume - BT_VOL_STEP);
    a2dp_sink.set_volume((uint8_t)g_btVolume);
    Serial.printf("[VOL] BT volume %d/127\n", g_btVolume);
  } else {
    g_uiVolume = max(0, g_uiVolume - 1);
    if (g_out) g_out->SetGain(uiVolumeToGain());
    Serial.printf("[VOL] SD volume %d/%d\n", g_uiVolume, VOL_UI_MAX);
  }
}

// Short-press actions (fired on release, except VOL which fires on press).
static void shortPress(Button &b) {
  if (g_mode == MODE_BT) {
    if (&b == &btnPlay) {
      if (g_btPlaying) { a2dp_sink.pause(); Serial.println("[BT] Pause (AVRCP)"); }
      else             { a2dp_sink.play();  Serial.println("[BT] Play (AVRCP)"); }
    } else if (&b == &btnNext) {
      a2dp_sink.next();     Serial.println("[BT] Next (AVRCP)");
    } else if (&b == &btnPrev) {
      a2dp_sink.previous(); Serial.println("[BT] Previous (AVRCP)");
    }
  } else {
    if (&b == &btnPlay)      toggleSDPlayPause();
    else if (&b == &btnNext) nextSDTrack();
    else if (&b == &btnPrev) prevSDTrack();
  }
}

// Generic debounced polling for one button.
// PLAY: short press acts on release; long press (>=1.5 s) toggles mode.
// VOL buttons: act on press, then auto-repeat while held.
// NEXT/PREV: act on release.
static void pollButton(Button &b) {
  uint32_t now = millis();
  bool raw = (digitalRead(b.pin) == LOW);   // true while physically held

  if (raw != b.rawLast) {
    b.rawLast = raw;
    b.rawChangedAt = now;
  }
  if (raw != b.pressed && (uint32_t)(now - b.rawChangedAt) >= DEBOUNCE_MS) {
    b.pressed = raw;
    if (b.pressed) {                        // ---- debounced press ----
      b.pressedAt  = now;
      b.longFired  = false;
      if (&b == &btnVolU || &b == &btnVolD) {
        if (&b == &btnVolU) volumeUp(); else volumeDown();
        b.nextRepeat = now + VOL_REPEAT_FIRST;
      }
    } else {                                // ---- debounced release ----
      if (&b == &btnPlay) {
        if (!b.longFired) shortPress(b);
      } else if (&b == &btnNext || &b == &btnPrev) {
        shortPress(b);
      }
      // VOL buttons: nothing on release (already stepped on press)
    }
  }

  // Actions that depend on *staying* held:
  if (b.pressed) {
    if (&b == &btnPlay && !b.longFired &&
        (uint32_t)(now - b.pressedAt) >= LONG_PRESS_MS) {
      b.longFired = true;
      toggleMode();
    }
    if ((&b == &btnVolU || &b == &btnVolD) &&
        (int32_t)(now - b.nextRepeat) >= 0) {
      if (&b == &btnVolU) volumeUp(); else volumeDown();
      b.nextRepeat = now + VOL_REPEAT_MS;
    }
  }
}

static void pollAllButtons() {
  pollButton(btnPrev);
  pollButton(btnPlay);
  pollButton(btnNext);
  pollButton(btnVolD);
  pollButton(btnVolU);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== PalPlayer booting ===");

  // OLED
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(18, 30, "PalPlayer");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(30, 48, "starting...");
  u8g2.sendBuffer();

  // Buttons (hardware debounce caps are fitted; we debounce anyway)
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_VOLD, INPUT_PULLUP);
  pinMode(PIN_BTN_VOLU, INPUT_PULLUP);

  // SD SPI bus pins (the SD library uses these via begin(CS))
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  // Boot rule: SD card with MP3s -> SD mode (paused).
  // Otherwise -> Bluetooth mode, advertising.
  if (scanCard()) {
    g_mode = MODE_SD;
    g_trackIndex = 0;
    g_sdTitle = titleFromPath(g_tracks[0]);
    g_sdState = SD_PAUSED;
    Serial.println("[MODE] Boot into SD mode (paused)");
  } else {
    g_mode = MODE_BT;
    startBluetooth();
    Serial.println("[MODE] Boot into Bluetooth mode");
  }

  drawDisplay();
  g_lastScreenKey = screenKey();
  Serial.println("=== PalPlayer ready ===");
}

void loop() {
  pollAllButtons();

  // --- SD playback engine ---------------------------------------------
  if (g_mode == MODE_SD && g_sdState == SD_PLAYING && g_mp3) {
    if (!g_mp3->loop()) {
      // loop() == false: track finished OR a read error (card pulled).
      Serial.println("[SD] Track ended (or read error), advancing");
      stopSDPlayback();
      if (!g_tracks.empty()) {
        g_trackIndex = (g_trackIndex + 1) % (int)g_tracks.size();
        if (!startSDTrack()) {
          g_sdState = SD_STOPPED;
          showMessage("SD ERROR");
        }
      } else {
        g_sdState = SD_STOPPED;
      }
    }
  }

  // --- Display refresh (throttled, only on change / while scrolling) --
  static uint32_t lastDraw = 0;
  if ((uint32_t)(millis() - lastDraw) >= DISPLAY_MS) {
    String key = screenKey();
    bool scrolling = false;
    {
      String t = currentTitle();
      // rough width check with the title font metrics:
      u8g2.setFont(u8g2_font_7x14B_tf);
      scrolling = u8g2.getStrWidth(t.c_str()) > 126 && t == g_scrollTrack;
    }
    if (key != g_lastScreenKey || scrolling) {
      lastDraw = millis();
      drawDisplay();
      g_lastScreenKey = screenKey();
    }
  }

  delay(2);   // keeps the button polling snappy without busy-spinning
}
