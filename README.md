# PowerFeather Test Build

## Dependencies

* platform.io toolkit
* ESP-IDF (ver. 5.1)

## Fixes:
In
```managed_components/powerfeather__powerfeather-sdk/src/Utils/MasterI2C.h```
change ln 46 to:

```_port((i2c_port_t)port), _sdaPin(sdaPin), _sclPin(sclPin), _freq(freq) {};```

In ```managed_components/powerfeather__powerfeather-sdk/src/Utils/MasterI2C.cpp```
remove ln 54:

```ESP_LOGD(TAG, "Start with port: %d, sda: %d, scl: %d, freq: %d.", _port, _sdaPin, _sclPin, _freq);```
