# Jarvis: specifica tecnica

Jarvis è un robottino da scrivania a forma di capsula: assistente vocale con occhi animati, orologio-meteo e radio.

> Versione del firmware **1.0.0**, ottobre 2026.
> Il firmware compila senza errori e senza warning. Le scelte su pin, registri e API sono controllate su schema elettrico, datasheet e sorgenti del core.
> **È la prima versione e non è ancora stata collaudata sulla scheda.** La checklist del primo avvio è al §9.

Indice: [1. Obiettivo](#1-obiettivo) · [2. Scelte di progetto](#2-scelte-di-progetto) · [3. Hardware](#3-hardware) · [4. Firmware](#4-firmware) · [5. Case 3D](#5-case-3d) · [6. Problemi trovati e risolti](#6-problemi-trovati-e-risolti-durante-lo-sviluppo) · [7. Limiti noti](#7-limiti-noti) · [8. Possibili sviluppi](#8-possibili-sviluppi) · [9. Collaudo](#9-collaudo-al-primo-avvio) · [10. Fonti](#10-fonti)

---

## 1. Obiettivo

Un assistente vocale da scrivania, a batteria, con queste funzioni:

- **Personaggio:** due occhi animati sul display, in un robottino a capsula.
- **Parola di attivazione offline «Jarvis»:** il riconoscimento avviene sulla scheda, senza internet.
- **Conversazione:** domanda a voce → trascrizione → risposta di un modello linguistico → voce italiana dallo speaker, con sottotitoli sincronizzati.
- **Orologio e meteo:** seconda modalità con una barra di stato da telefono (Wi-Fi, percentuale della batteria, icona di ricarica).
- **Radio via internet** a comando vocale («Jarvis, metti Radio 105»).
- **Touch:** lo schermo al posto di un tasto per cambiare modalità.
- **Pagina web delle impostazioni:** chiavi API, Wi-Fi, voce, volume e altro. Comprende:
  - configurazione iniziale con access point e captive portal;
  - aggiornamento del firmware via Wi-Fi (OTA).
- **Costi ricorrenti zero:** solo servizi con piano gratuito.
- **Speaker più grande in futuro:** predisposizione per un amplificatore I2S esterno.
- **Case** a capsula stampato in 3D.

Requisiti:
- costo minimo;
- montaggio il più semplice possibile, senza saldature;
- ricarica da USB-C;
- display "quasi quadrato", con occhi luminosi su sfondo nero.

---

## 2. Scelte di progetto

| Opzione | Esito | Motivo |
|---|---|---|
| Adafruit ESP32-S3 TFT Feather (già in casa) | scartata | Nessun audio integrato: andava bene solo come client delle API |
| LilyGO T-Display-S3 + INMP441 + MAX98357A + TTP223 | scartata | Funzionava, ma il display da 1,9" (170×320) è molto allungato e servivano 14 fili e delle saldature |
| **Waveshare ESP32-S3-Touch-AMOLED-1.8** | **scelta** | Display quasi quadrato 368×448. L'AMOLED dà occhi "luminosi" su nero vero. Microfono, speaker, caricabatterie e touch sono integrati: niente saldature |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | in riserva | 410×502, CO5300 + FT3168, ES8311 + ES7210 con due microfoni (AEC possibile), 32 MB di flash. Il firmware andrebbe adattato |
| Waveshare ESP32-C6-Touch-AMOLED-2.06 | scartata | Niente PSRAM e un solo core a 160 MHz. Il front-end audio (AFE) di esp-sr non supporta il C6 e «Jarvis» esiste solo come WakeNet9: niente wake word, niente framebuffer |
| Speaker Bluetooth | scartato | L'ESP32-S3 ha solo Bluetooth LE, senza BR/EDR: niente A2DP |
| Alexa, Amazon Music | scartati | Gli strumenti AVS per i nuovi dispositivi non sono più disponibili; per Amazon Music non c'è un'API pubblica (solo una beta chiusa) |
| Spotify | rimandato | Dal febbraio 2026 le app in Development Mode richiedono Premium, con un solo Client ID e al massimo 5 utenti. Spotify Connect su ESP32 è ancora sperimentale |

**Servizi:**
- **Groq**, piano gratuito, per la trascrizione (Whisper) e le risposte (Qwen 3.8, con riserva GPT-OSS).
- **Azure Speech**, piano gratuito F0, per la voce italiana. Il TTS di Groq esiste solo in inglese e arabo.
- **Open-Meteo** (meteo e geocoding), **radio-browser.info** (elenco radio) e **NTP**: gratuiti e senza chiave.

---

## 3. Hardware

### 3.1 Distinta componenti

| # | Componente | Note | Prezzo indicativo (ottobre 2026) |
|---|---|---|---|
| 1 | Waveshare ESP32-S3-Touch-AMOLED-1.8, versione **con batteria** | Arriva nel suo case nero, con lo speaker 12×10 mm e una LiPo con spina MX1.25 già compatibile (polarità corretta). Le schede nuove sono V2 | 34 € + spedizione |
| 2 | Cavo USB-C dati | Serve solo per il primo caricamento del firmware; va bene quello di un telefono, purché passi i dati | – |
| 3 | Filamento PLA bianco e nero, 7 viti M2 × 8 autofilettanti | Per il case a capsula (§5) | pochi euro |
| (opz.) | LiPo 103040 da circa 1200 mAh con protezione (PCM) | Più autonomia. Il case ha già il vano; la spina deve essere MX1.25 a 2 poli, altrimenti serve un adattatore (§3.8) | 5–10 € |
| (opz.) | MAX98357A + speaker 40 mm 4 Ω 3 W | Audio più forte. **Richiede saldature** sul connettore di espansione (§3.7) | circa 4 € |

Non serve un interruttore: il tasto PWR e l'AXP2101 gestiscono accensione, spegnimento e ricarica.

### 3.2 La scheda

- **SoC ESP32-S3R8:** 2 core LX7 a 240 MHz, 512 KB di SRAM, **8 MB di PSRAM octal**, Wi-Fi 4, BLE 5. Flash esterna da **16 MB** (W25Q128JVSIQ).
- **Display AMOLED 1,8" 368×448** su bus QSPI. Ne esistono due revisioni:
  - **V1:** driver SH8601, touch FT3168 (I2C 0x38).
  - **V2:** driver CO5300, touch CST820 (I2C 0x15; il codice Waveshare usa ancora il nome `CST816x`).

  La **V1 è fuori produzione** e dal 30 maggio 2026 Waveshare spedisce solo la V2. Il firmware le supporta entrambe e prova prima la V2.
- **Area attiva:** 28,70 × 34,94 mm, con **angoli arrotondati di circa 7 mm** (circa 90 px). Gli angoli del framebuffer non si vedono, quindi l'interfaccia non mette nulla negli angoli.
- **Audio:**
  - codec **ES8311** con un microfono analogico;
  - amplificatore **NS4150B** (classe D mono, alimentato a 3,3 V), abilitato da GPIO46 con livello ALTO e pull-down sulla scheda;
  - speaker di serie 12×10 mm, 8 Ω, 1 W, collegato con i suoi fili.
- **Alimentazione:** **AXP2101** per ricarica, misura della carica e accensione; connettore batteria MX1.25 a 2 poli. Non c'è un LED di carica.
- **Espansore I/O TCA9554** (0x20): reset del display e del touch, tasto PWR (§3.4).
- **Non usati dal firmware:** RTC PCF85063, IMU QMI8658, microSD.
- **Tasti:**
  - **PWR**, collegato all'AXP2101: un clic accende, tenuto 6 s spegne;
  - **BOOT** (GPIO0).
- **Documentazione Waveshare:** lo schema elettrico (PDF, novembre 2024) e il modello STEP di scheda e display sono sul wiki, non nel repository GitHub.

### 3.3 Mappa dei pin

Il file `pin_config.h` di Waveshare è identico per V1 e V2.

| Funzione | GPIO |
|---|---|
| LCD QSPI SDIO0..3 | 4, 5, 6, 7 |
| LCD SCLK / CS | 11 / 12 |
| LCD TE (tearing effect) | 13 (non usato, da non toccare) |
| I2C SDA / SCL | 15 / 14 |
| Touch INT | 21 |
| I2S MCLK / BCLK / WS (LRCK) | 16 / 9 / 45 |
| I2S DOUT (ESP32 → ES8311) / DIN (ES8311 → ESP32) | 8 / 10 |
| Enable amplificatore (PA) | 46 (attivo alto) |
| BOOT | 0 |
| SD (non usata) | CLK 2, CMD 1, D0 3; D3/CS su EXIO7 |

Nel core 3.3.12 l'ordine di `setPins` è `(BCLK, WS, DOUT, DIN, MCLK)`, quindi `setPins(9, 45, 8, 10, 16)`, come nell'esempio Waveshare `15_ES8311`.

### 3.4 Espansore TCA9554

| Linea | Funzione |
|---|---|
| EXIO0 | reset del display |
| EXIO1 | alimentazione del display (DSI_PWR_EN) |
| EXIO2 | reset del touch |
| EXIO3 | interrupt dell'RTC |
| EXIO4 | **tasto PWR** (ALTO = premuto, tramite un BSS138 invertitore) |
| EXIO5 | IRQ dell'AXP2101 |
| EXIO6 | interrupt dell'IMU |
| EXIO7 | D3/CS della microSD (tenuto alto) |

Il pin INT del TCA9554 **non è collegato all'ESP32**. Quindi:
- il tasto PWR si legge a polling;
- **PWR non può risvegliare l'ESP32 dal deep sleep**: per uscire dallo standby servono il touch (GPIO21) o BOOT (GPIO0).

Sequenza d'avvio, come `board_variant.c` di Waveshare:
1. EXIO0/1/2 bassi ed EXIO7 alto per 20 ms;
2. poi tutti alti;
3. 150 ms di attesa prima di interrogare il touch.

### 3.5 Indirizzi I2C

| Chip | Indirizzo |
|---|---|
| TCA9554 | 0x20 |
| AXP2101 | 0x34 (ID 0x4A nel registro 0x03) |
| ES8311 | 0x18 |
| FT3168 (V1) | 0x38 |
| CST820 (V2) | 0x15 (chip ID 0xB7 nel registro 0xA7) |
| PCF85063 | 0x51 (non usato) |
| QMI8658 | 0x6A/0x6B (non usato) |

### 3.6 Registri dell'AXP2101

Tutte le scritture sono **read-modify-write**. Le linee di alimentazione (DCDC1 = 3,3 V di sistema, ALDO1 = 3,3 V analogica dell'audio, RTCLDO) non vanno toccate.

| Registro | Uso |
|---|---|
| 0x00 bit5 | USB presente (VBUS good) |
| 0x00 bit3 | batteria presente |
| 0x01 bit[6:5] | direzione della corrente: 01 = in carica, 10 = in scarica, 00 = riposo (il bit7 è riservato) |
| 0xA4 | percentuale della batteria, 0–100 (indicativa: è stimata dalla tensione) |
| 0x18 bit3 | fuel gauge: vale già 1 all'accensione. Si conferma con `REG18 \|= 0x08`. Nello stesso registro il **bit1 abilita la ricarica**: una scrittura diretta la spegnerebbe |
| 0x50 bit4 | **pin TS come ingresso fisso che non influenza il caricatore**. La scheda non ha un termistore: senza questo bit la ricarica può comportarsi in modo anomalo. Valore: `REG50 = (REG50 & 0xF0) \| 0x10` |
| 0x30 bit1 | canale ADC di misura del TS, spento |
| 0x62 bit[4:0] | corrente di ricarica: 25 mA × N fino a 200 mA, poi 200 + 100 × (N − 8). Il valore predefinito del firmware è **200 mA**, cioè circa 0,5 C della batteria Waveshare |

> Il pin TS si disattiva con il **registro 0x50**: azzerare solo il bit1 del registro 0x30 spegne la misura, ma non scollega il TS dal caricatore.
> XPowersLib fa così dalla 0.3.0 (`disableTSPinMeasure()`). La 0.2.6 inclusa negli esempi Waveshare toccava solo il registro 0x30.

### 3.7 Connettore di espansione

- **Formato:** 15 fori passanti con **passo 1,27 mm** (la pagina prodotto dice 1 mm, ma è un refuso), senza pettine montato.
- **Pin, in ordine:** VBUS, GND, 3V3, GND, TXD (43), RXD (44), SCL (14), SDA (15), **GPIO17, 18, 38, 39, 40, 41, 42**.
- **MAX98357A:** si collega su tre dei GPIO 17, 18, 38–42 (BCLK, LRC, DIN), che si scelgono dalla pagina web.
- **Alimentazione:** VBUS dà 5 V **solo con la USB collegata**. Per i 3 W a batteria il modulo andrebbe alimentato da VBAT, che però non è sul connettore. A 3,3 V la potenza è molto più bassa e i picchi gravano sulla linea di sistema.

### 3.8 Batteria

- **Connettore:** MX1.25 a 2 poli sul retro, con l'apertura verso il basso. Secondo lo schema **pin 1 = +** (VBAT), verso il centro della scheda.
- **Polarità:** le spine da 1,25 mm di marche diverse possono avere i fili invertiti, quindi **va controllata con un tester** prima di collegare. Se è invertita si scambiano i due contatti nella spina, senza saldare.
- **Batteria Waveshare** (inclusa nella versione "con batteria"): circa 350–400 mAh, formato di circa 3,85 × 24 × 28 mm.
- **Autonomia:** secondo Waveshare, circa 1 ora con lo schermo sempre acceso alla massima luminosità e 3–4 ore con lo schermo spento.
- **Batteria più grande:** una 103040 da circa 1200 mAh entra nel vano del case. Su AliExpress queste batterie hanno quasi sempre la spina PH2.0, quindi serve un adattatore PH2.0 → MX1.25 già cablato, e la polarità va ricontrollata. Con una batteria più grande conviene alzare la corrente di ricarica dalla pagina web (al massimo metà della capacità: 1000 mAh → 500 mA).

---

## 4. Firmware

### 4.1 Toolchain

| Componente | Versione |
|---|---|
| Core `esp32:esp32` (arduino-esp32) | **3.3.12**, con ESP-IDF 5.5.5 ed esp-sr **2.5.3** |
| GFX Library for Arduino | 1.6.9 (SH8601, CO5300, `Arduino_ESP32QSPI`, canvas) |
| ArduinoJson | 7.4.3 |
| U8g2 | 2.36.19 (solo i font) |

- **FQBN:** `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc`.
- **Decoder MP3:** quello Helix già incluso nel core (`mp3dec.h`). Non servono librerie esterne.
- **Arduino IDE 2:** impostazioni in [`firmware/COMPILAZIONE.md`](../firmware/COMPILAZIONE.md).
- **Font:** il supporto dei font U8g2 in Arduino_GFX si attiva solo se lo sketch include `<U8g2lib.h>` prima di `<Arduino_GFX_Library.h>`. Lo sketch lo fa esplicitamente.

### 4.2 Partizioni

`partitions.csv` è nello sketch e si usa con lo schema di partizioni **Custom**.

```
nvs      data nvs      0x9000   0x5000
otadata  data ota      0xe000   0x2000
app0     app  ota_0    0x10000  0x600000   (6 MB)
app1     app  ota_1    0x610000 0x600000   (6 MB)
model    data spiffs   0xC10000 0x100000   <- modello esp-sr «Jarvis»
spiffs   data spiffs   0xD10000 0x2E0000   (libera)
coredump data coredump 0xFF0000 0x10000
```

L'app occupa circa **3,1 MB**. Più di metà è la libreria esp-dl (1,15 MB): con esp-sr 2.5.3 l'AFE se la porta dietro e il linker non la può eliminare. Il resto:
- stack Wi-Fi: circa 450 KB;
- mbedTLS: circa 145 KB;
- modello: 291 KB;
- sketch: circa 100 KB.

Gli slot da 6 MB lasciano spazio per gli aggiornamenti futuri. Con gli schemi da 3 MB del menu il margine sarebbe stato di soli 37 KB.

### 4.3 Modello della wake word

- **Origine:** il modello `wn9_jarvis_tts` (WakeNet9) viene da esp-sr, commit `0a6e15d5`, versione 2.5.3. Uso solo questo modello, impacchettato nel formato `srmodels.bin` con lo script `model/pack_model.py` di esp-sr: **291.168 byte**, CRC-32 `0x69459385`.
- **Nel firmware:** è incluso come array in `sr_model_jarvis.h`, generato con `firmware/tools/gen_model_header.py`.
- **Copia nella partizione `model`:** `ensureModel()` lo confronta con il contenuto della partizione a blocchi da 4 KB. Se è diverso cancella la partizione, lo scrive passando da un buffer di 4 KB in RAM interna, poi lo verifica. Al primo avvio servono circa 3 secondi, con la scritta «Preparo il riconoscimento vocale».
- **Perché prima di `esp_srmodel_init("model")`:** questa funzione mappa tutta la partizione e mette la lista in cache, senza controlli di integrità.
- **Allineamento:** non serve. I dati del modello stanno a un offset ≡ 2 (mod 4), come nel `srmodels.bin` ufficiale di arduino-esp32.

### 4.4 File

| File | Contenuto |
|---|---|
| `Jarvis.ino` | `setup`/`loop`; task dei comandi (touch, PWR, BOOT); flusso della conversazione, radio, meteo, standby; applicazione delle impostazioni |
| `util.h` | versione, log, allocazioni in PSRAM (anche per ArduinoJson), lock RAII, eventi, funzioni di testo (pulizia, UTF-8, escape HTML/XML, URL) |
| `settings.h` | struttura `Settings`, valori predefiniti, caricamento e salvataggio in NVS (namespace `jarvis`), `cfgLock`, `pendingCfg` |
| `board.h` | pin, I2C con `i2cLock`, TCA9554, AXP2101, touch, rilevamento della revisione, creazione del pannello, ES8311 (init, frequenza, volume, mute, sospensione) |
| `audio.h` | I2S duplex; coda di riproduzione in PSRAM e task `play`; bip; cambio frequenza; amplificatore esterno; task `feed` e `fetch` (AFE); modello |
| `ai.h` | client HTTPS, Groq Whisper, Groq chat con modelli di riserva, Azure TTS in streaming, Open-Meteo (meteo e geocoding) |
| `radio.h` | ricerca su radio-browser, riconoscimento del comando vocale, stream HTTP/HTTPS/ICY con redirect, decodifica MP3 |
| `ui.h` | canvas 368×448 in PSRAM: occhi, sottotitoli, barre di stato, orologio e meteo, radio, configurazione, OTA; task `ui` |
| `web.h` | pagina delle impostazioni, captive portal, OTA con conferma (rollback), mDNS, access point |
| `es8311.c/.h`, `es8311_reg.h` | driver Espressif (Apache-2.0), identico a quello degli esempi Waveshare |
| `sr_model_jarvis.h` | modello «Jarvis» (file generato) |
| `partitions.csv` | tabella delle partizioni |

### 4.5 Task FreeRTOS e sincronizzazione

| Task | Core | Priorità | Stack | Ruolo |
|---|---|---|---|---|
| `loopTask` | 1 | 1 | **20 KB** | flussi, HTTPS/TLS, radio e MP3, impostazioni |
| `feed` | 0 | 5 | 6 KB | I2S → canale del microfono → `afe->feed()` |
| `fetch` | 0 | 5 | 12 KB | `afe->fetch()`, wake word, registrazione con VAD. È l'**unico** che chiama `enable/disable_wakenet` |
| `play` | 1 | 6 | 4 KB | coda in PSRAM → I2S (integrato o esterno), livello audio |
| `ui` | 1 | 1 | 10 KB | disegno e invio a bande; lettura della batteria ogni 5 s |
| `input` | 1 | 3 | 4 KB | touch (interrupt su TP_INT + I2C), PWR (EXIO4), BOOT |
| `web` | 1 | 1 | 8 KB | `server.handleClient()` |

**Lock e dati condivisi**
- **I2C:** il mutex ricorsivo `i2cLock` protegge ogni accesso, compresi i read-modify-write del driver ES8311. Il core 3.3.12 rende atomica ogni singola transazione, ma le sequenze vanno protette.
- **Impostazioni:** `cfg` la scrive **solo il loop**. La pagina web prepara `pendingCfg` sotto `cfgLock`; il loop la applica e la salva. Anche `Preferences` si usa solo dal loop.
- **Interfaccia:** `uiLock` protegge testi, meteo e fuso orario.

**I2S**
- Le letture (feed) e le scritture (play) usano canali separati, senza mutex: nell'IDF ogni canale ha il suo mutex.
- Il cambio di frequenza (`setAudioRate`, chiamato solo dal loop) usa l'handshake `i2sPaused` / `feedBusy` / `playBusy`: ogni task imposta "busy" *prima* di controllare "paused".
- In duplex l'RX prende il clock dal TX, quindi si riconfigura prima il TX e poi l'RX.

**Coda di riproduzione**
- È un buffer circolare a singolo produttore (il loop: voce o radio) e singolo consumatore (`play`).
- Gli indici sono `std::atomic` in [0, 2N). La capacità è di 144.000 frame stereo, cioè 576 KB in PSRAM (3 s a 48 kHz).
- Lo svuotamento avviene con un flag e una conferma dal consumatore.

**Eventi**
- Una coda FreeRTOS di 16 eventi porta al loop tocco, tocco lungo, PWR, BOOT, BOOT lungo e wake word.
- Se la coda è piena il task `fetch` riattiva da solo la wake word, così non resta mai spenta.

### 4.6 Pipeline audio

- **Formato:** I2S standard Philips, 16 bit stereo, MCLK = 256·fs (fisso in ESP_I2S).
- **Ordine d'avvio:** amplificatore spento → I2S (MCLK attivo) → `Wire` → codec, come nell'esempio Waveshare.
- **Microfono:** è mono. Il canale (L o R) viene **rilevato** nei primi 2 s confrontando l'energia dei due canali.
- **Voce e wake word a 16 kHz.** Azure risponde in `raw-16khz-16bit-mono-pcm`, quindi non serve ricampionare. Il mono viene copiato su L e R.
- **Radio:**
  - alla prima frame MP3 l'I2S e l'ES8311 passano alla frequenza dello stream; sono accettate tutte quelle della tabella del driver con MCLK 256·fs, da 8 a 48 kHz;
  - durante il cambio il DAC è in mute, perché l'NS4150B resta acceso fino a 4 s dopo l'ultimo suono;
  - durante la radio il microfono è spento, perché ADC e DAC condividono la LRCK;
  - alla fine si torna a 16 kHz.
- **Stereo:** il DAC dell'ES8311 è mono e prende solo il canale sinistro (REG09 bit7 = 0). Anche il MAX98357A è mono. Per questo la radio stereo viene ridotta a mono con (L + R) / 2.
- **Volume:**
  - la scala dell'utente va da 1 a 100, cioè da −40 a **0 dB**; 0 è muto;
  - con lo speaker integrato si scrive direttamente il registro 0x32 dell'ES8311 (0xBF = 0 dB). La funzione del driver arriva a +32 dB, ma sopra 0 dB voce e radio, masterizzate quasi a fondo scala, vengono tagliate;
  - con l'amplificatore esterno si usa la stessa scala in software, con il DAC integrato muto e il PA spento.
- **Prefill:** 0,3 s per la voce e 0,5 s per la radio. Se la coda si svuota, si ricarica prima di riprendere.

### 4.7 Wake word, registrazione e VAD

**Configurazione dell'AFE**
- Chiamata: `afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST)`, cioè 1 microfono, senza AEC né riduzione del rumore.
- **VAD:** il VAD è quello WebRTC. Senza un modello `vadnet` nella partizione, `vad_model_name` resta NULL. `vad_min_noise_ms` è 300.
- **Sensibilità:** `wakenet_mode` vale `DET_MODE_90` (normale) oppure `DET_MODE_95` ("aggressiva": più rilevamenti e più falsi allarmi), da impostazioni.
- **Memoria e core:** `AFE_MEMORY_ALLOC_MORE_PSRAM`, core 0.
- **Puntatore:** `esp_afe_handle_from_config()` restituisce un puntatore `const`.

**Registrazione**
1. Dopo il bip la wake word si spegne.
2. I primi **250 ms** si scartano, perché contengono l'eco del bip.
3. **Alla fine di questa finestra si azzera il VAD** con `afe->reset_vad()`. Senza questo passaggio l'eco del bip lo terrebbe su «parlato» e chiuderebbe la registrazione troppo presto.
4. L'audio si accumula in PSRAM.
5. La registrazione finisce:
   - con **700 ms** di silenzio dopo il parlato (almeno 96 ms);
   - oppure dopo **10 s**;
   - oppure viene annullata dopo **5 s** senza parlato.

**Buffer della domanda** (PSRAM): `[intestazione multipart][WAV 44 B][PCM][chiusura]`. L'intestazione è allineata a destra, così si manda tutto con un solo POST.

### 4.8 Servizi esterni

| Servizio | Richiesta | Dettagli |
|---|---|---|
| Groq Whisper | `POST https://api.groq.com/openai/v1/audio/transcriptions` | Multipart con `model` (predefinito `whisper-large-v3-turbo`), `language=it`, `response_format=json`, `temperature=0`, `file` WAV 16 kHz mono. Su 400/403/404/429/498/5xx o timeout prova l'altro Whisper (`whisper-large-v3`). Filtra le "allucinazioni" tipiche sull'audio muto («Sottotitoli … amara.org» e simili) |
| Groq chat | `POST https://api.groq.com/openai/v1/chat/completions` | Predefinito **`qwen/qwen3.8-27b`** con `reasoning_effort:"none"` e 400 token. Riserva **`openai/gpt-oss-120b`**, poi `openai/gpt-oss-20b`, con `reasoning_effort:"low"`, `include_reasoning:false` e 600 token (i token di ragionamento contano nel budget). Passa alla riserva su 400/403/404/408/429/498/5xx, timeout o risposta vuota; mai su 401. Messaggi: prompt di sistema con contesto (data, ora, città, meteo attuale) + ultime 8 battute; la storia si azzera dopo 10 minuti; i blocchi `<think>` vengono tolti |
| Azure TTS | `POST https://<regione>.tts.speech.microsoft.com/cognitiveservices/v1` | Header `Ocp-Apim-Subscription-Key`, `Content-Type: application/ssml+xml`, `X-Microsoft-OutputFormat: raw-16khz-16bit-mono-pcm`, `User-Agent`. SSML `<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='it-IT'><voice name='it-IT-ElsaNeural'>…</voice></speak>`, con escape XML di `& < > " '`. Un secondo tentativo su 429. La risposta va in streaming nella coda (chunked o con Content-Length) |
| Meteo | `GET https://api.open-meteo.com/v1/forecast?latitude=…&longitude=…&current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4` | Ogni 15 minuti; ogni minuto finché non arriva il primo |
| Geocoding | `GET https://geocoding-api.open-meteo.com/v1/search?count=1&language=it&format=json&name=…` | Quando cambia la città (la città geolocalizzata è salvata in `geoCity`). Una risposta senza `results` vuol dire «città non trovata» |
| Radio | `GET https://all.api.radio-browser.info/json/stations/search?limit=10&hidebroken=true&order=votes&reverse=true&codec=MP3&name=…[&countrycode=IT]` | Se non risponde si usa `de1.api.radio-browser.info`. Si usa `url_resolved`. Strategia di scelta al §4.9 |
| Ora | NTP `pool.ntp.org`, `time.google.com` | Fuso `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Rome) |

**Limiti del piano gratuito Groq** (tabella ufficiale, ottobre 2026), per modello:
- chat: 30 richieste al minuto, 1.000 al giorno, 8.000 token al minuto, 200.000 al giorno;
- Whisper: 20 richieste al minuto, 2.000 al giorno, 28.800 secondi di audio al giorno (7.200 all'ora), con almeno 10 s fatturati per richiesta.

`qwen/qwen3.8-27b` è ancora in "preview" e potrebbe essere ritirato con poco preavviso: in quel caso subentrano i modelli di riserva, e il modello si cambia dalla pagina web. Nel piano gratuito i modelli Llama sono deprecati da agosto 2026.

**Azure F0:** 500.000 caratteri al mese con le voci neurali, al massimo 20 richieste ogni 60 secondi.

**Note sul client HTTPS**
- Ogni richiesta apre una connessione nuova (`setReuse(false)`, `tls.stop()`).
- TLS è usato **senza verifica dei certificati** (`setInsecure()`). È una semplificazione accettata per un uso domestico.
- Nel core 3.3.12 `HTTPClient::writeToStream()` non ha un timeout complessivo. In più, con TLS `readBytes()` restituisce subito 0 quando non ci sono dati, anche durante un trasferimento normale. Per questo le "sink" usate per le risposte contano il tempo dall'ultimo byte ricevuto (12–25 s; 15 s per la voce), poi impostano l'errore di scrittura e la lettura si chiude. Whisper, chat e ricerca delle radio si chiudono anche quando si tocca lo schermo.
- Le risposte JSON si leggono con ArduinoJson in PSRAM, con filtri.

### 4.9 Radio

**Riconoscimento del comando**
- La frase trascritta è un comando radio se contiene «radio» **e** uno di questi verbi: metti, mettimi, accendi, fammi sentire, fammi ascoltare, ascolta, suona, avvia, fai partire, voglio sentire, voglio ascoltare.
- Tolti verbi e parole di riempimento, quello che resta è il nome da cercare. «Uno», «due» e «tre» diventano cifre, e il punto fra due cifre resta (es. «102.5»).
- Se non resta nulla, o resta solo «radio», si usa la radio preferita.
- Prima di partire Jarvis dice «Ecco <nome della stazione>».

**Scelta della stazione** (su radio-browser la ricerca per nome funziona come un «contiene»)
1. **Prima ricerca:** radio italiane, solo MP3, niente HLS, 10 risultati. Vince il nome identico. Poi il nome che inizia con la richiesta; un **sottocanale** («Radio 105 - Hits», «Nome: tema», «Nome | …») perde un punto. A parità di punteggio vince il nome più corto, poi i voti.
2. **Seconda ricerca**, se non c'è nulla e la richiesta inizia con «radio»: si ripete senza quella parola, ma si accetta **solo lo stesso nome a meno di «radio»**. Così «radio deejay» non diventa «Deejay Fox Radio».
3. **Fuori dall'Italia** si accetta solo il nome identico. Altrimenti «Radio Deejay» sceglierebbe «Radio Deejay HR», una radio croata.

**Radio preferita predefinita: «Radio 105 Network»** (stream MP3 a 128 kbps e 48 kHz).
- Il nome è quello esatto: con «Radio 105» la ricerca sceglieva un sottocanale.
- Radio Deejay e RTL 102.5 (canale principale) trasmettono **solo in HLS/AAC**, quindi non sono supportate.

**Stream**
- Client HTTP/1.0 con **HTTPS**, senza `Icy-MetaData`; accetta `ICY 200 OK`.
- Segue fino a 4 redirect, anche relativi, verso altri host o verso HTTPS. Rai Radio 1, per esempio, ne fa due verso HTTPS.
- Content-Type HLS, AAC, OGG, FLAC e HTML vengono rifiutati.
- Dopo 10 s senza dati lo stream si considera interrotto.
- Se lo stream cade dopo almeno 20 s di ascolto, Jarvis si riconnette fino a 3 volte, senza fermare l'audio già in coda. A ogni riconnessione il decoder MP3 viene ricreato.

**Arresto:** con un tocco sullo schermo, con PWR o con BOOT.

### 4.10 Interfaccia

**Disegno**
- Il canvas 368×448 RGB565 sta in PSRAM (330 KB).
- Si disegna **a bande**: si ridisegnano e si inviano al pannello solo le fasce cambiate, allineate a righe pari. Un invio completo costa circa 20 ms di CPU.
- Disegna solo il task `ui`, a circa 30 fps nella modalità assistente, 10 fps per l'orologio, 20 fps per la radio.

**Modalità assistente**
- In alto c'è una barra di stato piccola: Wi-Fi, ora, batteria.
- **Occhi:** rettangoli arrotondati di circa 100 × 124 px, a 44 px di distanza. Gli stati sono:
  - **riposo:** sguardi casuali e battito delle palpebre;
  - **ascolto:** occhi più grandi e barre del livello del microfono;
  - **pensiero:** sguardo in alto e tre puntini;
  - **voce:** occhi sorridenti e bocca che segue il volume;
  - **errore:** palpebre tristi;
  - **standby.**
- **Sottotitoli:** 3 righe in `helvR18`, fino a 4 KB di testo e 160 righe.
  - Mentre la voce arriva avanzano a 14 caratteri al secondo. Quando tutta la voce è in coda, avanzano in proporzione ai frame riprodotti e non tornano mai indietro.
  - La riga in corso non si ricentra mentre si scrive.
- In basso c'è un suggerimento: «Di' «Jarvis» o tieni premuto».

**Modalità orologio e meteo**
- Barra di stato grande: tacche del Wi-Fi; batteria con percentuale e fulmine giallo in carica; «USB» se manca la batteria.
- Ora in `logisoso92`, con i due punti lampeggianti. L'impaginazione usa gli avanzamenti fissi del font, quindi è centrata a ogni ora.
- Sotto: data in italiano, icona del meteo disegnata in base al codice WMO, temperatura, descrizione, città, umidità, vento e previsione di 3 giorni.

**Altre schermate**
- **Radio:** occhi felici chiusi, equalizzatore animato con il livello audio, nome della stazione, «Tocca lo schermo per fermare».
- **Configurazione:** nome della rete `Jarvis-XXXX` e indirizzo `192.168.4.1`. Se la pagina è protetta lo segnala.
- **Aggiornamento:** barra di avanzamento dell'OTA.
- **Avvisi:** un riquadro temporaneo, per esempio l'indirizzo della pagina all'avvio.

**Testo**
- Tutto passa da `cleanText()`. Virgolette e apostrofi tipografici, trattini, puntini, spazi speciali ed euro vengono convertiti; le emoji vengono tolte.
- I font `_tf` di U8g2 coprono il Latin-1 (à è é ì ò ù °), ma non questi caratteri.
- Il markdown viene tolto solo dal testo da leggere ad alta voce.

### 4.11 Comandi

| Gesto | Azione |
|---|---|
| «Jarvis» | ascolto (solo in modalità assistente) |
| Tocco breve | cambia modalità (assistente ↔ orologio). Mentre Jarvis ascolta, pensa, parla o suona: **interrompe** |
| Tocco lungo (1 s) | ascolto immediato, senza wake word (anche dall'orologio) |
| PWR breve | da ferma: **standby** (deep sleep). Mentre ascolta, parla o suona: interrompe |
| PWR tenuto 6 s | spegnimento (lo fa l'AXP2101) |
| BOOT breve | nuova conversazione (la storia si azzera) |
| BOOT tenuto 5 s | al rilascio, riavvio nella **configurazione aperta**. Ferma subito radio e risposte |

**Standby**
- Prima di dormire Jarvis aspetta che il dito si stacchi e che BOOT sia rilasciato. Poi spegne il PA, sospende il codec, spegne il pannello e il Wi-Fi.
- Si risveglia con il **tocco** (TP_INT basso) o con **BOOT**, tramite `esp_sleep_enable_ext1_wakeup_io` con `ANY_LOW` su GPIO21 e GPIO0 e i pull-up RTC.
- Se TP_INT resta basso, il risveglio funziona solo con BOOT.
- **PWR non risveglia**: vedi §3.4.

**Durante l'avvio** (che può durare fino a 20 s, tra Wi-Fi e altro) tocchi, PWR e BOOT breve vengono ignorati. Resta valido solo BOOT 5 s.

### 4.12 Pagina web e configurazione

**Indirizzo:** `http://jarvis.local` (mDNS) oppure l'IP mostrato sul display.

**Protezione**
- Basic Auth con utente `admin` e la password scelta.
- L'autenticazione vale per **ogni** rotta, compreso il caricamento OTA, ed è verificata **già nella callback di upload**: il WebServer la chiama prima dell'handler della rotta.

**Configurazione iniziale (access point)**
- Parte quando non c'è un SSID salvato, quando il Wi-Fi non risponde entro 20 s, oppure dopo BOOT 5 s.
- Crea un AP aperto `Jarvis-XXXX`, con il nome ricavato dal MAC (`esp_read_mac`). Prima di avviarlo chiama `WiFi.disconnect(true)` e `setAutoReconnect(false)`.
- DNS captive `*` → 192.168.4.1. Nel core 3.x è asincrono: `processNextRequest()` non fa nulla.
- Risponde alle sonde di Android, iOS, Windows e Firefox (`generate_204`, `hotspot-detect.html`, `connecttest.txt`, `ncsi.txt`, `fwlink` e altre).
- Cerca le reti in background per il menu a tendina.

**Configurazione aperta o protetta**
- È **aperta** (senza password) solo al primo avvio o dopo BOOT 5 s, cioè con qualcuno fisicamente davanti a Jarvis.
- Se parte da sola perché il router non risponde (per esempio dopo un blackout), resta **protetta** dalla password della pagina e lo schermo lo indica. Senza questa regola chiunque nel raggio del Wi-Fi potrebbe caricare un firmware.

**Riavvii**
- Dopo il salvataggio Jarvis si riavvia.
- Se in configurazione passano 5 minuti senza attività sulla pagina e senza client collegati, si riavvia per ritentare il Wi-Fi.

**Rotte:** `GET /` (modulo), `POST /save`, `GET /update` (modulo OTA), `POST /update` (caricamento). Tutte le altre rimandano a `/`.

**Chiavi:** non vengono mai mostrate. La pagina scrive solo «salvata, finisce con XXXX», e un campo vuoto lascia invariato il valore.

**Validazione dei campi**
- Lunghezze massime.
- Caratteri ammessi nelle chiavi e nei nomi dei modelli.
- Tre pin diversi per l'amplificatore, scelti fra quelli del connettore.
- Escape HTML su tutto ciò che viene riscritto nella pagina.

**Richiedono un riavvio:** Wi-Fi, sensibilità della wake word, amplificatore e pin, revisione della scheda. Tutto in configurazione.

**OTA**
- Usa `Update` con controllo di `begin`, `write`, `end` e `isFinished`.
- Se il caricamento si interrompe, lo stato si chiude nella callback `UPLOAD_FILE_ABORTED`. Se il file era già completo e verificato, Jarvis si riavvia sulla nuova versione.
- Dopo 30 s senza dati l'OTA viene annullato.

**Rollback**
- `verifyRollbackLater()` restituisce `true`. La nuova immagine viene confermata solo dopo l'avvio di display, audio e pagina web (e prima di riavvii e standby).
- Se l'avvio si ferma (display, PSRAM, interfaccia) con l'immagine non confermata, Jarvis chiede il ritorno alla versione precedente.

### 4.13 Impostazioni (NVS, namespace `jarvis`)

| Chiave | Predefinito | Note |
|---|---|---|
| `wifiSsid` / `wifiPass` | – | |
| `webPass` | – | password della pagina (utente `admin`) |
| `groqKey` | – | |
| `chatModel` | `qwen/qwen3.8-27b` | |
| `fbModel` | `openai/gpt-oss-120b` | riserva; poi `openai/gpt-oss-20b` |
| `sttModel` | `whisper-large-v3-turbo` | oppure `whisper-large-v3` |
| `azureKey` | – | |
| `azureRegion` | `westeurope` | |
| `voice` | `it-IT-ElsaNeural` | anche Isabella, Diego, Giuseppe (multilingue), Fiamma, Benigno |
| `prompt` | personalità di Jarvis (robottino simpatico e un po' ironico), al massimo 3 frasi brevi, niente markdown | |
| `city` / `lat` / `lon` | Roma / 41,8919 / 12,5113 | lat e lon aggiornate dal geocoding |
| `geoCity` | – | città a cui si riferiscono lat e lon |
| `tz` | `CET-1CEST,M3.5.0,M10.5.0/3` | si applica subito, senza riavvio |
| `volume` | 70 | 0–100 (circa −12 dB) |
| `micGain` | 4 | 0–7 (ES8311, 6 dB per passo) |
| `wakeSens` | 0 | 0 normale, 1 più sensibile |
| `radioFav` | `Radio 105 Network` | |
| `ampExt` / `ampBclk` / `ampLrc` / `ampDin` | 0 / −1 / −1 / −1 | amplificatore esterno, pin 17, 18, 38–42 |
| `startMode` | 1 | 0 orologio, 1 assistente (si aggiorna a ogni cambio) |
| `forceSetup` | false | riavvio nella configurazione aperta (BOOT 5 s) |
| `boardRev` | 0 | 0 automatica, 1 = V1, 2 = V2 |
| `chgMa` | 200 | corrente di ricarica in mA (0 = non modificare) |
| `cfgVer` | 1 | versione dello schema delle impostazioni |

Le impostazioni si salvano solo se sono cambiate, per consumare meno la flash. I nomi delle chiavi hanno al massimo 15 caratteri.

### 4.14 Installazione

- **Senza IDE:** scrivere `Jarvis_completo_0x0.bin`, preso dalla pagina delle [Release](https://github.com/skiokk/jarvis/releases/latest), all'indirizzo **0x0** con esptool-js (https://espressif.github.io/esptool-js/) da Chrome o Edge. Al primo caricamento conviene fare "Erase flash". L'immagine contiene bootloader, partizioni e app.
- **Aggiornamenti:** caricare dalla pagina `/update` il file `Jarvis_aggiornamento_OTA.bin` dell'[ultima release](https://github.com/skiokk/jarvis/releases/latest).
- **Da sorgente:** Arduino IDE 2 o arduino-cli, con le impostazioni di [`firmware/COMPILAZIONE.md`](../firmware/COMPILAZIONE.md).

---

## 5. Case 3D

Il case è un robottino a capsula: corpo bianco di 56 × 114 × 35,5 mm, alto 117,5 mm con i piedi. È modellato in CadQuery, con tutte le misure come parametri.
- **Parti:** visiera nera separata, telaio della scheda con ganci, due tastini per BOOT e PWR, braccia in posa con perni, piedi a incastro, inserto per la batteria Waveshare.
- **Spazio previsto:** una 103040, l'altoparlante da 40 mm con la sua griglia e il MAX98357A.
- **Stampa:** si stampa senza supporti e si chiude con 7 viti M2 × 8.
- **Misure:** quelle della scheda vengono dal modello STEP ufficiale Waveshare. Le misure stimate e il pezzo di prova da stampare per primo sono descritti in [`case3d/LEGGIMI_case.md`](../case3d/LEGGIMI_case.md).

---

## 6. Problemi trovati e risolti durante lo sviluppo

Durante lo sviluppo ho confrontato ogni scelta con lo schema elettrico, i datasheet, i sorgenti del core e risposte reali dei servizi. Questi sono i punti in cui la documentazione o la prima idea erano sbagliate.

**Hardware**
1. **Pin TS dell'AXP2101.** Azzerare il bit1 del registro 0x30 spegne solo la misura: il TS si scollega dal caricatore con il **bit4 del registro 0x50**. Gli esempi Waveshare usano una XPowersLib vecchia (0.2.6) che non lo fa.
2. **Registro 0x18 dell'AXP2101.** Contiene anche l'abilitazione della ricarica (bit1): va scritto in read-modify-write, mai direttamente.
3. **Stato di carica.** Sta nei bit [6:5] del registro 0x01, non in [7:5].
4. **Connettore di espansione.** Il passo è 1,27 mm e non 1 mm, e i pin sono fori passanti: l'upgrade dello speaker richiede saldature. Il connettore porta solo 3V3 e 5 V con la USB collegata.
5. **Tasto PWR.** Non può risvegliare dallo standby, perché la linea INT dell'espansore non è collegata all'ESP32.
6. **Angoli del display.** Sono arrotondati (circa 90 px), quindi niente icone negli angoli.
7. **Revisioni della scheda.** La V1 è fuori produzione, quindi la V2 è il caso principale.

**Audio**

8. **Volume.** La funzione del driver ES8311 supera 0 dB (+32 dB al massimo) e taglia i picchi: il firmware scrive il registro direttamente, al massimo a 0 dB.
9. **Radio stereo.** L'ES8311 riproduce solo il canale sinistro: serve il downmix a mono.
10. **Eco del bip.** Teneva il VAD su «parlato» e chiudeva la registrazione dopo circa 1 s: il VAD si azzera alla fine dei 250 ms scartati.
11. **Colpo nello speaker al cambio di frequenza.** Il DAC va in mute mentre i clock ripartono.

**Rete e servizi**

12. **Radio Deejay** non ha uno stream MP3, e la ricerca «ovunque» sceglieva una radio croata con lo stesso nome. La radio preferita è diventata Radio 105, e fuori dall'Italia serve il nome identico.
13. **Ricerca per nome.** È un «contiene», quindi vincevano i sottocanali più votati («Radio 105 - Hits», «Virgin Radio Classic Rock»). Ora conta la somiglianza del nome, e i sottocanali sono penalizzati.
14. **Stream con HTTPS e redirect fra host diversi** (Rai Radio 1): il client li gestisce entrambi.
15. **`writeToStream` senza timeout complessivo**, con `readBytes()` che su TLS restituisce subito 0: con un server muto il loop restava bloccato. Il timeout ora è nelle sink.
16. **Modello di riserva.** `gpt-oss-120b` è migliore del 20b, con gli stessi limiti gratuiti. GPT-OSS non accetta `reasoning_format`, e i token di ragionamento consumano il budget.
17. **SSML di Azure.** Manca `xmlns`, che la documentazione indica come obbligatorio.
18. **radio-browser.** La documentazione chiede di non fissare un solo server: si usa `all.api.radio-browser.info`.

**Pagina web e OTA**

19. **Autenticazione dell'OTA.** La callback di upload gira prima dell'handler, quindi l'autenticazione va controllata lì. Altrimenti si potrebbe caricare un firmware senza password.
20. **OTA interrotto.** Il WebServer non chiama più l'handler della rotta e Jarvis restava sulla schermata di aggiornamento: lo stato ora si chiude nella callback di abort.
21. **Rollback.** Il core conferma la nuova immagine prima di `setup()`, quindi il rollback non scattava mai. La conferma ora è rimandata a dopo l'avvio.
22. **Configurazione partita da sola senza password.** Dopo un blackout, con l'AP aperto chiunque poteva riconfigurare Jarvis: ora resta protetta.

**Flusso e interfaccia**

23. **Wake word spenta per sempre** se una conversazione finiva in anticipo (Wi-Fi assente, chiave mancante, OTA). Ora si riarma a ogni uscita.
24. **Gesti fatti durante l'avvio.** Venivano eseguiti tutti insieme alla fine dell'avvio.
25. **Sottotitoli fuori sincrono** con una velocità di lettura fissa: ora seguono la durata reale della voce.
26. **Testo tagliato a metà di un carattere UTF-8.** Rovinava il disegno del testo successivo e il JSON della storia.
27. **Timer del meteo dopo 24,8 giorni di accensione** (overflow di `millis()`).

**Toolchain**

28. **Spazio dell'app.** Il firmware supera i 3 MB per via di esp-dl, che il linker non può eliminare: servono slot app da 6 MB (§4.2).
29. **Versione di U8g2.** La 2.37.1 non esiste; la più recente è la 2.36.19.
30. **Decoder MP3.** Il core include già il decoder Helix: una seconda copia (libhelix) creava simboli duplicati.

---

## 7. Limiti noti

- **Mai provato su hardware:** vedi il §9.
- **Wake word con un solo microfono:** non c'è AEC, quindi la parola di attivazione non si sente mentre Jarvis parla o suona; si interrompe con il tocco. La semantica esatta di `DET_MODE_90/95` non è documentata (libreria chiusa).
- **Autonomia:** wake word sempre attiva, Wi-Fi senza risparmio energetico e AMOLED acceso: con la batteria Waveshare l'autonomia è breve. È un oggetto da scrivania, quasi sempre sulla USB.
- **Volume:** il predefinito 70 vale circa −12 dB, quindi al primo ascolto può sembrare basso; si alza dalla pagina. Lo speaker di serie è piccolo.
- **Radio solo MP3:** niente HLS né AAC (Radio Deejay, RTL 102.5 principale). Su reti lente possono esserci pause, ma la coda si ricarica da sola. Mentre uno stream lento si apre, il tocco ha effetto solo alla fine del tentativo in corso (fino a circa 8 + 10 + 10 s).
- **Sicurezza:**
  - TLS senza verifica dei certificati;
  - chiavi salvate in NVS non cifrata;
  - pagina in HTTP con Basic Auth, che viaggia in chiaro sulla rete e sull'AP di configurazione aperto.

  Va bene per un uso domestico.
- **Batteria:** la percentuale dell'AXP2101 è indicativa.
- **Risposte HTTP:** una risposta HTTP/1.1 delimitata solo dalla chiusura della connessione non passa dalla sink, quindi in quel caso un server muto non viene rilevato. I servizi usati rispondono chunked o con Content-Length.
- **Risposte chunked su TLS:** nel core 3.3.12 il CRLF a fine blocco si legge senza attesa. Se arrivasse in un record TLS successivo, la lettura finirebbe in timeout. Con Open-Meteo non succede; con Groq e Azure non l'ho potuto verificare senza chiavi. Al peggio la voce già ricevuta viene riprodotta e per Groq subentra la riserva.

---

## 8. Possibili sviluppi

- **Più radio:** supporto HLS e AAC, per Radio Deejay, RTL 102.5 e altre.
- **Scheda AMOLED 2.06:** due microfoni con ES7210 e AEC, per sentire la parola di attivazione anche mentre Jarvis parla o suona.
- **Speaker da 40 mm** con MAX98357A, alimentato da VBAT.
- **Luminosità:** abbassare lo schermo dopo un po' di inattività, per allungare l'autonomia.
- **Spotify Connect**, se diventerà praticabile su ESP32.

---

## 9. Collaudo al primo avvio

Collegare la scheda con il monitor seriale aperto (USB CDC, 115200) e controllare, in ordine:

1. `Scheda V1/V2` con il chip ID del touch: per il CST820 è atteso 0xB7. Il display deve essere centrato e senza righe sporche (offset di colonna 16 sulla V2).
2. `AXP2101 ID 0x4A: REG18=… REG50=… REG62=…`: REG50 deve avere il bit4 a 1. Poi controllare che la ricarica funzioni.
3. `Modello «Jarvis» …` e `AFE: feed N campioni x 1 canali, fetch M campioni`.
4. `Microfono sul canale L/R (energia …)`, circa 2 s dopo l'avvio. Le barre del microfono devono muoversi durante l'ascolto.
5. Configurazione dal telefono (captive portal), poi `Wi-Fi connesso … IP …`. Ogni minuto devono comparire `Stato: …` e `Stack liberi …`.
6. «Jarvis» detto a 1–2 m. Se risponde male, regolare il guadagno del microfono e la sensibilità.
7. Una domanda completa: voce chiara, senza click, sottotitoli in sincronia.
8. «Jarvis, metti la radio» (Radio 105), poi un tocco per fermarla.
9. Standby con PWR e risveglio col tocco e con BOOT.
10. Un aggiornamento OTA con lo stesso file.

L'ultimo errore compare anche nella pagina web («Ultimo errore: …»).

---

## 10. Fonti

- Waveshare ESP32-S3-Touch-AMOLED-1.8:
  - repository: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
  - wiki (schema elettrico e STEP): https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8
  - documentazione: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8
- Datasheet di AXP2101, ES8311, TCA9554 ed ESP32-S3R8. XPowersLib: https://github.com/lewisxhe/XPowersLib
- Arduino-esp32 3.3.12: https://github.com/espressif/arduino-esp32 · esp-sr: https://github.com/espressif/esp-sr
- Groq:
  - modelli: https://console.groq.com/docs/models
  - parametri di ragionamento: https://console.groq.com/docs/reasoning
  - limiti: https://console.groq.com/docs/rate-limits
- Azure Speech:
  - REST TTS: https://learn.microsoft.com/azure/ai-services/speech-service/rest-text-to-speech
  - prezzi: https://azure.microsoft.com/pricing/details/speech/
- Open-Meteo: https://open-meteo.com/en/docs · radio-browser: https://api.radio-browser.info
- Waveshare ESP32-S3-Touch-AMOLED-2.06: https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm
- esptool-js: https://espressif.github.io/esptool-js/
