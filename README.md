# MiniWX Receiver

A compact weather **display** for the **GeekMagic SmallTV-Ultra** (ESP8266 + ST7789 240×240 IPS). It shows live weather on a clean clock-and-metrics screen, follows up to **three stations**, draws **history graphs**, and can take its data either from a **MiniWX Server** on your LAN (see my MiniWX Server project on my GIT) or directly from **APRS-IS**. The interface is trilingual (Romanian / English / Hungarian).

> **Firmware Ver. 3.02** — Developed and maintained by **YO7ZRO**.

---

## Table of contents

1. [What it does](#what-it-does)
2. [Hardware](#hardware)
3. [Flashing the firmware](#flashing-the-firmware)
4. [First start — WiFi setup](#first-start--wifi-setup)
5. [Data sources](#data-sources)
6. [What the screen shows](#what-the-screen-shows)
7. [Multi-station & slideshow](#multi-station--slideshow)
8. [History graphs](#history-graphs)
9. [Web configuration — every setting](#web-configuration--every-setting)
10. [Brightness & schedule](#brightness--schedule)
11. [Languages](#languages)
12. [Tips & troubleshooting](#tips--troubleshooting)
13. [Credits](#credits)

---

## What it does

- Displays **temperature, pressure, humidity, dew point and real-feel** on a 240×240 colour screen.
- Big, easy-to-read **clock** with localized date.
- A small **weather character** (face) that changes expression with the felt temperature.
- Follows up to **3 weather stations** and rotates through them automatically.
- Draws on-device **history graphs** (1 hour to 1 week) with per-station coloured lines.
- Two interchangeable **data sources**: a local **MiniWX Server**, or **APRS-IS** (read-only).
- Fully configured from a **web page**; trilingual **RO / EN / HU**.
- Day/night **brightness schedule**.

---

## Hardware

Built for the **GeekMagic SmallTV-Ultra** (ESP8266 / ESP-12F with a ST7789V 240×240 IPS panel). The board pinout is already configured in `platformio.ini`:

| Signal | Pin |
|--------|-----|
| MOSI | GPIO13 (HSPI) |
| SCLK | GPIO14 (HSPI) |
| CS | tied to GND on the PCB |
| DC | GPIO0 |
| RST | GPIO2 |
| Backlight (BL) | GPIO5 — **active LOW** |

The display is driven by **TFT_eSPI**, configured entirely through `build_flags` (you do **not** need to edit the library's `User_Setup.h`).

---

## Flashing the firmware

### Pre-built `.bin` (no compiling)

Download the firmware from the project's **Releases** page and flash a blank board with **esptool** or a GUI flasher (NodeMCU PyFlasher, Espressif Flash Download Tool):

```bash
pip install esptool
esptool.py --chip esp8266 --port <PORT> erase_flash
esptool.py --chip esp8266 --port <PORT> --baud 460800 write_flash --flash_size detect 0x0 <firmware>.bin
```

Use `COM3` / `/dev/ttyUSB0` / `/dev/cu.usbserial-XXXX` for `<PORT>`. Drop to `115200` baud if `460800` is unstable.

### From source (PlatformIO)

```
esp8266_tft/
├── platformio.ini      # board + TFT_eSPI configuration
└── src/main.cpp        # firmware
```

Dependency (auto-installed): **TFT_eSPI** (`bodmer/TFT_eSPI`).

```bash
pio run -t upload
pio device monitor      # 115200 baud
```

> **Display troubleshooting** (notes in `platformio.ini`): if colours look inverted add `-DTFT_INVERSION_OFF=1`; if the image is rotated/mirrored adjust `tft.setRotation(...)`; if you see noise/artefacts lower `-DSPI_FREQUENCY` to `27000000` or `20000000`.

---

## First start — WiFi setup

The receiver ships with no WiFi credentials. On first boot (or if it cannot join the saved network):

1. It starts a setup access point named **`MiniWeather-Setup-XXXX`** (`XXXX` = chip ID in hex). The screen shows the AP name and the setup URL.
2. Connect a phone/laptop to that AP and open **`http://192.168.4.1`** in a browser.
3. Enter your WiFi credentials and the rest of the configuration (see below), then save.
4. The receiver reboots and joins your network.

---

## Data sources

The receiver can get its weather data in **two ways**, selected by **Source type** on the config page:

### 1. MiniWX local (`/jquery`)

The receiver polls a **MiniWX Server** on your local network at its `/jquery` endpoint and reads the live values. This is a **single-source** mode — set:

- **BME host/IP** → the IP address of your MiniWX Server (e.g. `192.168.88.191`).
- **Sensor read interval (s)** → how often to poll (match it to the server).

### 2. APRS-IS (read-only)

The receiver logs in to an **APRS-IS** server (read-only, passcode `-1`) and follows up to **three stations** by callsign, parsing their weather packets. Use this to show stations that are not on your LAN — anything reporting weather to the APRS network. Set:

- **Station callsign 1 / 2 / 3** → e.g. `YO7ZRO-13`.
- **APRS-IS login callsign**, **APRS-IS server** (default `rotate.aprs2.net`), **APRS-IS port** (default `14580`).
- Optional **Station name 1/2/3** and **Temp. correction 1/2/3** (see below).

---

## What the screen shows

The main page is laid out for glanceability:

- **Clock** — large `HH:MM`, redrawn only when the minute changes (no flicker).
- **Date** — with a localized month name.
- **Location label** — the displayed station's name (or `Station N` if no name is set). In APRS mode a small **colour pill** marks which station you are looking at.
- **Five metrics**, each on its own row:

| Metric | Unit |
|--------|------|
| Temperature | °C |
| Pressure | hPa |
| Humidity | % |
| Dew point | °C |
| Real feel | °C |

- **Weather character** — a little face on the right that changes with the **felt temperature**, using six categories based on the meteorological UTCI scale:

| State | Felt temperature |
|-------|------------------|
| Winter | below 0 °C |
| Cold | 0–9 °C |
| Cool | 9–18 °C |
| OK (comfortable) | 18–26 °C |
| Warm | 26–32 °C |
| Hot | 32 °C and above |

- **Footer** — the device IP on the left, `Dev. by YO7ZRO Ver. 3.02` on the right.

Values are refreshed each poll; if the source goes silent the readings are shown in a neutral grey to flag that the data is stale.

---

## Multi-station & slideshow

When more than one station is active (APRS mode), the receiver runs a **slideshow**:

1. Each active station's **main page** is shown for **Main page time** seconds.
2. Then the **graphs** are shown for **Graph slide interval** seconds each (set the interval to `0` to disable graphs entirely and only cycle the main pages).

Each station has a fixed colour used on screen and in the graph legend:

- **Station 1 — orange**
- **Station 2 — cyan**
- **Station 3 — green**

You can also pin which station is shown first/by default with **Displayed station**.

---

## History graphs

Three metrics are graphed — **temperature, humidity and pressure** — with one coloured line per active station and a shared Y scale.

**Selectable window** (`Graph window`):

| Option | Span |
|--------|------|
| 1 h | last hour |
| 12 h | last 12 hours |
| 24 h | last day |
| 1 week | last 7 days |

**How the history is stored.** The receiver keeps a **multi-resolution** buffer so a week of data fits in a tiny device:

- a **fine** tier (≈1 minute of 2-second samples) and a **minute** tier (≈2 hours) live in RAM;
- the **hourly** tier (168 hours = **7 days**) lives in a ring file in **flash** (`/hour.bin`), written about once per hour per station and read straight from flash when drawing — so the week-long window costs almost no RAM and the flash is spared by writing rarely.

If the clock has not synced yet, or there is not enough data, the graph shows a `waiting for time…` / `collecting data…` message instead of an empty frame.

---

## Web configuration — every setting

Open the configuration page in a browser (during setup at `http://192.168.4.1`, afterwards at the receiver's IP). All settings are stored on the device and survive reboots.

**WiFi & general**

| Setting | Meaning |
|---------|---------|
| **Password** | WiFi password. |
| **BME host/IP** | MiniWX Server address (used in *MiniWX local* mode). |
| **Location** | Free-text location label. |
| **Timezone offset (hours)** | Offset from UTC for the clock. |
| **Language** | Română / English / Magyar. |

**Weather data source**

| Setting | Meaning |
|---------|---------|
| **Source type** | `MiniWX local (/jquery)` or `APRS-IS`. |
| **Station callsign 1 / 2 / 3** | APRS callsigns to follow (e.g. `YO7ZRO-13`). |
| **Station name 1 / 2 / 3** | Friendly name shown on screen (empty → `Station N`). |
| **Temp. correction 1 / 2 / 3 (C)** | Per-station temperature offset, e.g. `-2.0`. |
| **Displayed station** | Which station is shown by default. |
| **APRS-IS login callsign** | Your login (read-only access, passcode `-1`). |
| **APRS-IS server** | Default `rotate.aprs2.net`. |
| **APRS-IS port** | Default `14580`. |
| **Sensor read interval (s)** | Poll/refresh interval (min 5 s). |

A **Reset APRS to defaults** button restores the server (`rotate.aprs2.net`) and port (`14580`).

**Display & timing**

| Setting | Meaning |
|---------|---------|
| **Main page time (s)** | How long each station's main page is shown. |
| **Graph slide interval (s)** | Time per graph slide; `0` = no graphs. |
| **Graph window** | 1 h / 12 h / 24 h / 1 week. |
| **Brightness (%)** | Normal screen brightness (5–100). |
| **Brightness schedule** | Enable a dimmed period with start/end hours. |

> The **BME path** field is fixed (`jquery`) and shown read-only — it is the MiniWX Server endpoint and should not be changed.

### Per-station temperature correction

If a station's sensor reads consistently high or low, enter a correction in **Temp. correction (C)** for that station (e.g. `-2.0`). It is applied at parse time, so the corrected value feeds the screen, the dew-point / real-feel calculation **and** the history graphs — everything stays consistent.

---

## Brightness & schedule

- **Brightness (%)** sets the normal level (the backlight is PWM-driven, active LOW).
- Enable **Brightness schedule** to dim the screen during set hours — useful at night. Set the start and end hours (0–23); the schedule may span midnight.

---

## Languages

The whole interface — both the web configuration page and the on-screen labels (metrics, date, status messages) — is available in **Romanian, English and Hungarian**, selectable with the **Language** setting. Screen text uses no diacritics, by design, to match the on-board font.

---

## Tips & troubleshooting

- **A station reads a bit high/low.** Use **Temp. correction** for that station.
- **Clock is off by an hour.** Adjust **Timezone offset** (no automatic daylight-saving).
- **Graph says "waiting for time".** The clock has not synced yet; give it a moment after boot / WiFi connect.
- **No graphs in the slideshow.** Set **Graph slide interval** to a value greater than `0`.
- **Colours look wrong / image rotated / noisy.** See the display-troubleshooting note in *Flashing the firmware*.
- **Can't reach the receiver.** If WiFi failed it falls back to the `MiniWeather-Setup-XXXX` AP for reconfiguration.

---

## Credits

- Firmware **MiniWX Receiver** (Ver. 3.02) — developed and maintained by **YO7ZRO**.
- Display library: **TFT_eSPI** by Bodmer.

73 de **YO7ZRO**
