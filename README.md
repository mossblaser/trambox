TramBox
=======

An ESP8266 powered IoT (Internet of Trams) device which indicates the number of
minutes before the departure of the next [Manchester Metrolink
tram](https://tfgm.com/public-transport/tram) on an analogue gauge.

![The TramBox](http://jhnet.co.uk/misc/trambox.jpg)

Connect a ~10 uF capacitor across the pins of the gauge, and connect to pin D1
of a [nodemcu board](). Flash this firmware using:

    $ pio run -t upload

Then configure the firmware via the simple serial-based UI:

    $ pio device monitor
    Press one of the following keys to configure:
      w: Set WiFi SSID and password
      t: Set TFGM API key
      r: Set metrolink route
      d: Calibrate display
    [press a key]

You will need to setup:

* A WiFi SSID and password
* A [TFGM API key](https://developer.tfgm.com/developer)
* A start and destination station. The time displayed will be the next tram
  leaving the starting station which stops at the destination station.
* The display calibration, specifically the angle of the needle for each number
  of minutes which may be displayed.

The settings will be stored in EEPROM.
