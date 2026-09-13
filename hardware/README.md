# Parts and wiring
The pin map matches the recovered FAST SILENT V6 source and V6 wiring guide.

| Part / signal | ESP32-S3 connection |
|---|---|
| SSD1306 OLED VCC / GND | 3V3 / GND |
| OLED SDA / SCL | GPIO8 / GPIO9 |
| INMP441 VDD / GND | 3V3 / GND |
| INMP441 L/R | GND (left slot) |
| INMP441 SCK / WS / SD | GPIO14 / GPIO15 / GPIO16 |
| MAX98357A VIN / GND | Regulated 5V / common GND |
| MAX98357A BCLK / LRC / DIN | GPIO14 / GPIO15 / GPIO17 |
| MAX98357A SD / EN / SD_MODE | GPIO5, if exposed on the breakout |
| Push button | GPIO4 to GND; internal pull-up |
| WS2812 DIN | GPIO18 through 330 Ω; level shift for a 5V pixel if needed |
| WS2812 VCC / GND | Regulated 5V / common GND |
| 4–8 Ω speaker, 3–5W | MAX98357A SPK+ and SPK− |

Parts: one ESP32-S3, one INMP441, one MAX98357A, one speaker, one 128×64 SSD1306 OLED, one WS2812, one button, 330 Ω resistor, wires, suitable USB/5V supply and local amplifier decoupling (470–1000 µF plus 0.1 µF).

GPIO14 and GPIO15 are intentionally shared clock lines; the microphone and amplifier have separate data pins. Keep them short. Never connect either speaker terminal to ground. If SD/EN is absent, leave GPIO5 unconnected; data-line quieting remains, but the firmware cannot guarantee a muted amplifier while clocks change. Verify your breakout's SD pin ratings before connecting it.
