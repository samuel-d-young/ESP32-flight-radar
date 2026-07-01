# Flight Radar — web installer (3 hardware variants)

A static site that flashes Flight Radar firmware to any of three real,
compiled builds directly from the browser, using
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) over Web Serial.
No backend, no build step.

**Not supported, and can't be:** classic AVR Arduinos (Uno, Nano, Mega,
Leonardo). They lack the Espressif serial bootloader that browser-based
flashing depends on — no browser tool can flash them.

## The three builds

| Card | Display | Chips | Library | Manifest |
|---|---|---|---|---|
| Round display | GC9A01, 240×240 round SPI | ESP8266 or ESP32 (auto-detected) | TFT_eSPI | `manifest-round.json` |
| Rectangular display | ILI9341, 240×320 SPI | ESP8266 or ESP32 (auto-detected) | TFT_eSPI | `manifest-rect.json` |
| ESP32-4848S040 | 480×480 RGB-parallel panel + GT911 touch, all-in-one module | ESP32-S3 only | LovyanGFX | `manifest-4848s040.json` |

The ESP32-4848S040 needs its own manifest/button because it's a different
display *technology* (16-bit RGB parallel bus + 3-wire SPI init, not plain
4-wire SPI) — TFT_eSPI can't drive it at all, so it uses LovyanGFX instead,
with LovyanGFX's built-in `Panel_ST7701_guition_esp32_4848S040` init sequence
for this exact board.

## Files

```
index.html                            installer page — 3 firmware cards + display library
manifest-round.json                   GC9A01 round display manifest
manifest-rect.json                    ILI9341 rectangular display manifest
manifest-4848s040.json                ESP32-4848S040 manifest
flight_radar_round.ino                round display firmware source (TFT_eSPI, cross-platform)
flight_radar_rect.ino                 rectangular display firmware source (TFT_eSPI, cross-platform)
flight_radar_4848s040.ino             ESP32-4848S040 firmware source (LovyanGFX)
firmware/
  flight_radar_esp8266_round.bin      compiled: ESP8266 + GC9A01
  flight_radar_esp32_round.bin        compiled: ESP32 + GC9A01 (merged image)
  flight_radar_esp8266_rect.bin       compiled: ESP8266 + ILI9341
  flight_radar_esp32_rect.bin         compiled: ESP32 + ILI9341 (merged image)
  flight_radar_esp32s3_4848s040.bin   compiled: ESP32-S3 + ESP32-4848S040 (merged image)
```

## Deploy to Vercel

Plain static site — no build command or framework detection needed.

```bash
npm i -g vercel        # if you don't already have it
cd flight-radar-installer
vercel --prod
```
Or drag the folder onto https://vercel.com/new, or push to GitHub and import
in the Vercel dashboard for auto-redeploy on every push.

## Using it

1. Open the deployed URL in **Chrome or Edge on desktop**.
2. Connect your board over USB.
3. Click the **Install** button on whichever of the three cards matches your
   hardware, pick the serial port, and wait for it to flash.
4. After reboot, connect to the **FlightRadar-Setup** Wi-Fi network from your
   phone and fill in your home Wi-Fi + location + radar range. Saved to flash
   permanently.
5. For the round/rectangular builds: wire the display using the
   **display library** at the bottom of the page (pick board, then display).
   The ESP32-4848S040 needs no wiring — it's a single module, just power it.

## ESP32-4848S040 specifics

- **Power over USB-C, 5V.** Use a wall charger or desktop USB port — laptop
  USB ports often can't supply enough current with WiFi + backlight active.
- **Board settings if recompiling:** Board = "ESP32S3 Dev Module", PSRAM =
  OPI PSRAM, Flash Size = 16MB, Flash Mode = QIO 80MHz, USB CDC On Boot =
  Disabled, Upload Mode = UART0/Hardware CDC.
- **Flash headroom is tight** — the compiled binary uses ~97% of the default
  1.2MB app partition. If you add features, switch to a partition scheme
  with more app space (e.g. `PartitionScheme=app3M_fat9M_16MB`) and
  recompile; that needs a fresh full core rebuild (several minutes), not an
  incremental one.
- **Tap anywhere on screen to force an immediate traffic refresh** (3s
  debounce) — the only touch feature implemented so far.
- Library required: `LovyanGFX` (Library Manager). The exact panel class is
  `lgfx::Panel_ST7701_guition_esp32_4848S040`, bundled with LovyanGFX — no
  custom init sequence needed.

## Recompiling (reference)

```bash
# Round display — ESP8266
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --output-dir build_round_esp8266 flight_radar_round.ino

# Round display — ESP32
arduino-cli compile --fqbn esp32:esp32:esp32 --export-binaries \
  --output-dir build_round_esp32 flight_radar_round.ino
# -> build_round_esp32/flight_radar_round.ino.merged.bin

# Rectangular display — ESP8266 / ESP32: same as above with flight_radar_rect.ino

# ESP32-4848S040
arduino-cli compile --export-binaries \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default,CDCOnBoot=default" \
  --output-dir build_4848s040 flight_radar_4848s040.ino
# -> build_4848s040/flight_radar_4848s040.ino.merged.bin
```

Required libraries (Library Manager): `TFT_eSPI`, `ArduinoJson`,
`WiFiManager` (round/rect builds); `LovyanGFX`, `ArduinoJson`, `WiFiManager`
(4848S040 build).

All compiled binaries are flashed at offset `0` — the ESP32/ESP32-S3 ones are
pre-merged images (bootloader + partitions + app combined), so the manifest
structure never needs to change even as you update the firmware.

## Updating firmware later

Replace the relevant file(s) in `firmware/`, bump `version` in the matching
manifest, redeploy. No other changes needed unless you add a new chip family
or display type — in which case, add a new manifest + a new card in
`index.html`'s `.fw-cards` block.
