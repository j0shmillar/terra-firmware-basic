# ESP32 I2S MIC Test

The microphone testing application.
Records the speech using MEMS INMP441 microphone and sends samples to a HTTP server in RAW format.

## Dependencies

* platform.io toolkit
* ESP-IDF (ver. 5.1)

## Build

* Build
```shell
$ pio run -t menuconfig
$ pio run
```
* Upload
```shell
$ pio run -t upload
```
* Connect
```shell
$ pio run -t monitor
```
