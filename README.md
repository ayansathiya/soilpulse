# Soil Probe

An Arduino UNO R4 WiFi instrument that measures soil temperature, pH and compaction (cone index). Live values show on a 1.3" OLED, and a browser dashboard can read the same data over USB.

## Repo layout

| Path | What it is |
|---|---|
| `firmware/soil_probe/` | Main sketch |
| `firmware/oled_test/` | Display-only test sketch for checking OLED wiring |
| `docs/index.html` | Dashboard (Web Serial live mode, with demo data when not connected) |

## Hardware

- Arduino UNO R4 WiFi
- DS18B20 waterproof temperature probe and a 4.7 kOhm resistor
- Load cell with a 30 degree cone (323 mm^2 base), and an HX711 amplifier module
- Glass pH probe and a pH amplifier board with a BNC socket
- 1.3" 128x64 I2C OLED (SH1106 or SSD1306)
- Optional: push button to tare the load cell and reset the peak

## Wiring

| Part | Connections |
|---|---|
| DS18B20 | red to 5V, black to GND, yellow to D2, 4.7k resistor between D2 and 5V |
| HX711 | VCC to 5V, GND to GND, DT to D3, SCK to D4. Load cell wires to E+, E-, A+, A- |
| pH board | V+ to 5V, G to GND, Po to A0 |
| OLED | VCC to 3.3V, GND to GND, SDA to A4, SCL to A5 |
| Button (optional) | D7 to GND |

If the load cell force reads negative when pushing, swap A+ and A-.

## Setup

1. Install these libraries from the Arduino Library Manager: U8g2, OneWire (latest), DallasTemperature, HX711 (Bogdan Necula).
2. Open `firmware/oled_test/oled_test.ino` first and upload it. If you see "Hello soil!", the display is wired correctly.
3. Open `firmware/soil_probe/soil_probe.ino`, select the board "Arduino UNO R4 WiFi", and upload.

If the OLED picture is garbled or shifted, swap the SH1106 constructor for the SSD1306 one (both sketches have the line ready to uncomment).

## Calibration

Open the Serial Monitor at 115200 baud, then:

1. Load cell: with nothing on the cone send `z`. Put a known mass on it (for example 2 kg) and send `s2.0`. Copy the printed value into `HX_SCALE` in the sketch.
2. pH: put the probe in pH 7 buffer, send `w`, and copy the volts into `PH_V7`. Repeat in pH 4 buffer for `PH_V4`. Check against pH 10 buffer as an independent test.
3. Re-upload after editing the constants.

## Serial commands

| Command | Action |
|---|---|
| `r` | One reading, printed as `temp_C,pH` |
| `w` | Raw temperature and pH voltage, for calibration |
| `z` | Tare the load cell and reset the peak |
| `s<kg>` | Calibrate the load cell with a known mass, for example `s2.0` |
| `p` | Penetrometer mode. Send `n` after each 5 cm of depth and `x` to stop |

## Dashboard

Open `docs/index.html` in Chrome or Edge, click Connect Arduino, and choose the board. Close the Arduino Serial Monitor first, since only one program can use the port. Until you connect, the page runs on simulated demo data.

To host it online, enable GitHub Pages (Settings, Pages, deploy from branch `main`, folder `/docs`). Web Serial works on the HTTPS Pages site in Chrome and Edge.

## Method notes

- pH is measured in a soil and distilled water slurry (1:5 by volume), stirred and left to settle. Glass probes should not be pushed into dry soil.
- Cone index is force divided by cone base area. Roots struggle at around 2000 kPa. Push at a steady slow speed and log every 5 cm.
- Temperature is used to correct the pH slope. Calibrate with buffers at a known temperature and set `PH_CAL_TEMP_C`.
- The verdict text in the dashboard gives rough guidelines for the reading, not lab-grade classifications. Validate against an independent test kit or lab result.
