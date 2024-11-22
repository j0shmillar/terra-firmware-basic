## setup

set-up [ESP-IDF](https://github.com/espressif/esp-idf) then run:

```idf.py flash``` 

or ```idf.py flash monitor``` to monitor output.

## Hardware

camera
- GPIO pin layout in ```cam.h```.
- 3v3 -> 3v3, GND -> GND1
- Leave RET unconnected

GPS
- TX -> RX, RX -> TX, GND -> GND1, VDD -> 3v3

Mic
- GPIO pin layout in ```mic.h```
- VDD -> EN, GND -> GND2

PIR
- VDD -> VBAT*, GND -> GND2, SD->D13
