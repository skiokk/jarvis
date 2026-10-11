# Licenze di terze parti

Il codice originale di questo progetto è distribuito con licenza MIT (file `LICENSE`). Contiene anche questi componenti di terzi.

## Inclusi nel repository

### Driver ES8311 (`firmware/Jarvis/es8311.c`, `es8311.h`, `es8311_reg.h`)

Copyright 2015-2022 Espressif Systems (Shanghai) CO LTD. Licenza **Apache-2.0** (https://www.apache.org/licenses/LICENSE-2.0). Il file è identico a quello degli esempi Waveshare per ESP32-S3-Touch-AMOLED-1.8.

### Modello della wake word «Jarvis» (`firmware/Jarvis/sr_model_jarvis.h`)

Il file contiene il modello `wn9_jarvis_tts` di esp-sr (https://github.com/espressif/esp-sr), commit `0a6e15d5`, impacchettato in formato `srmodels.bin`. Licenza:

```
ESPRESSIF MIT License

Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>

Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
it is free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

La licenza vale solo per l'uso su prodotti Espressif (l'ESP32-S3 lo è).

## Usati per compilare (presenti nei binari pubblicati nelle Release)

| Componente | Licenza |
|---|---|
| arduino-esp32 3.3.12 (core Arduino per ESP32) | LGPL-2.1-or-later |
| ESP-IDF 5.5.5 ed esp-sr 2.5.3 (Espressif) | Apache-2.0 / ESPRESSIF MIT |
| Decoder MP3 Helix (esp-libhelix-mp3, incluso nel core) | RealNetworks RPSL/RCSL |
| GFX Library for Arduino 1.6.9 | BSD |
| ArduinoJson 7.4.3 | MIT |
| U8g2 2.36.19 (font) | BSD-2-Clause; i font hanno le licenze indicate nel progetto U8g2 |

## Case 3D

- Le misure della scheda vengono dal modello STEP ufficiale pubblicato da Waveshare sul suo wiki. Il modello Waveshare non è incluso in questo repository.
- Il visualizzatore `anteprima_3d.html` carica three.js 0.160.0 (licenza MIT) da cdn.jsdelivr.net.
