# rain_light
This project was designed for the ESP32 NodeMCU board and is aimed to learn FreeRTOS and the ESP IDF, so most of the functionality is unnecessarily complex.

Also, some things may be hardcoded, as they weren't useful for the learning purpose previously expressed.

## Build
From the root folder, type:
```
$ make menuconfig
```
Configure the project as needed, then it is both possible to compile:
```
$ make
```
or compile and flash:
```
$ make flash
```
The esptool.py will try to reset the ESP32, but if it can't connect simply push the "boot" button in the board while it's trying.
