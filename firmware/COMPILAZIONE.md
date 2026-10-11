# Jarvis: compilazione e rilascio del firmware

Firmware **1.0.0**. Compila senza errori e senza warning (`--warnings all`). Il funzionamento del firmware è descritto in [`docs/SPECIFICA.md`](../docs/SPECIFICA.md).

## 1. Versioni usate

| Componente | Versione | Note |
|---|---|---|
| Core `esp32:esp32` (arduino-esp32) | **3.3.12** | ESP-IDF 5.5.5, esp-sr 2.5.3. Restare sulla serie 3.3.x: la 4.0 cambia versione di ESP-IDF |
| GFX Library for Arduino | 1.6.9 | |
| ArduinoJson | 7.4.3 | |
| U8g2 | 2.36.19 | solo i font |
| arduino-cli | 1.5 | oppure Arduino IDE 2 |

Il decoder MP3 è quello Helix già incluso nel core: non serve installare altre librerie.

## 2. Compilare con arduino-cli

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install "GFX Library for Arduino@1.6.9" "ArduinoJson@7.4.3" "U8g2@2.36.19"

cd firmware/Jarvis
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc \
  --warnings all --build-path ~/.cache/jarvis-build
```

Con `PartitionScheme=custom` si usa il `partitions.csv` dello sketch: due slot app da 6 MB, la partizione `model` e le altre (vedi il §4.2 della specifica). Gli schemi del menu da 3 MB non bastano, perché l'app occupa circa 3,1 MB.

Per caricare direttamente sulla scheda:

```bash
arduino-cli upload -p /dev/ttyACM0 \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc \
  --input-dir ~/.cache/jarvis-build
```

Se il caricamento fallisce (per esempio perché il programma sulla scheda va in crash), si entra a mano in modalità download:
1. spegnere la scheda tenendo premuto PWR per 6 s;
2. tenere premuto BOOT e premere PWR per riaccenderla;
3. rilasciare BOOT.

Dopo il caricamento la scheda resta in modalità download: va spenta e riaccesa (fonte: FAQ del wiki Waveshare). Prima di caricare bisogna chiudere il monitor seriale.

## 3. Compilare con Arduino IDE 2

1. In *Preferenze → URL aggiuntive per il gestore schede* aggiungere `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
2. Dal gestore schede installare **esp32** di Espressif, versione **3.3.12**.
3. Dal gestore librerie installare GFX Library for Arduino, ArduinoJson e U8g2 (versioni del §1).
4. Aprire `Jarvis/Jarvis.ino` e scegliere la scheda **ESP32S3 Dev Module** con queste impostazioni:

| Voce | Valore |
|---|---|
| USB CDC On Boot | **Enabled** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **Custom** (usa il `partitions.csv` dello sketch) |
| PSRAM | **OPI PSRAM** |
| Flash Mode | QIO 80MHz (predefinito); se la scheda non si avvia, provare **DIO** |
| USB Mode | Hardware CDC and JTAG (predefinito) |
| Upload Speed | 921600 |

## 4. Binari di rilascio

I binari non sono nel repository: si scaricano dalla pagina delle [Release](https://github.com/skiokk/jarvis/releases). I comandi qui sotto li generano nella cartella locale `release/`, esclusa da git.

| File | Uso |
|---|---|
| `Jarvis_completo_0x0.bin` | Prima installazione: immagine unica con bootloader, tabella delle partizioni, `boot_app0` e app. Va scritta all'indirizzo **0x0** con esptool-js (https://espressif.github.io/esptool-js/) da Chrome o Edge. Al primo caricamento conviene fare "Erase flash" |
| `Jarvis_aggiornamento_OTA.bin` | Aggiornamenti: si carica dalla pagina `http://jarvis.local/update` |

Comandi per generarli, con l'esptool del core (`~/.arduino15/packages/esp32/tools/esptool_py/*/esptool`) e `B` = cartella di build:

```bash
cp $B/Jarvis.ino.bin release/Jarvis_aggiornamento_OTA.bin
esptool --chip esp32s3 merge-bin -o release/Jarvis_completo_0x0.bin \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 $B/Jarvis.ino.bootloader.bin 0x8000 $B/Jarvis.ino.partitions.bin \
  0xe000 $B/boot_app0.bin 0x10000 $B/Jarvis.ino.bin
esptool --chip esp32s3 image-info release/Jarvis_completo_0x0.bin   # checksum e hash: "valid"
```

- **Parametri della flash:** DIO, 80 MHz e 16 MB vengono dal file `flash_args` della build. Il bootloader passa poi a QIO, perché la configurazione è `qio_opi`.
- **Dimensione:** l'immagine unica non è riempita fino a 16 MB.
- **Partizione `model`:** non serve scriverla a parte. Il modello «Jarvis» è dentro il firmware e viene copiato nella partizione al primo avvio, in circa 3 s.

| File (1.0.0) | Byte | SHA-256 |
|---|---|---|
| `Jarvis_aggiornamento_OTA.bin` | 3.108.224 | `c440d0d23903da53fc6074b0edfadf8e561c3be9e8fbec9990919c861f7b1205` |
| `Jarvis_completo_0x0.bin` | 3.173.760 | `8cb7be3baf35bdb1fad498f2849a7b87c441ba81740a26a9860160923d6ba196` |

## 5. Memoria

| | Valore |
|---|---|
| App (flash) | 3.107.979 byte, cioè il 49 % di uno slot da 6 MB |
| RAM globale | 70.912 byte (21 %); restano 256.768 byte per heap e stack |
| PSRAM (allocata all'avvio) | canvas 329.728 B, coda audio 576.000 B, buffer della domanda 320.812 B, sottotitoli circa 13 KB, più AFE e decoder MP3 |

Principali voci dell'app, dal file `.map`:
- esp-dl e dl_lib: 1,15 MB. Li richiede l'AFE di esp-sr 2.5.3 e il linker non li può eliminare.
- Stack Wi-Fi, lwIP e wpa: circa 450 KB.
- mbedTLS: circa 145 KB.
- Modello «Jarvis»: 291 KB.
- Sketch: circa 100 KB.
- Font U8g2: circa 32 KB.

**TLS** usa solo la RAM interna. Se il log segnala «RAM interna bassa prima di TLS», vanno ridotti gli stack o i servizi attivi. Ogni minuto il log riporta la RAM libera (minima e blocco più grande), la PSRAM, le interruzioni dell'audio e lo stack libero di ogni task.

## 6. Rigenerare il modello «Jarvis»

`sr_model_jarvis.h` è generato e non va modificato a mano. Per rigenerarlo, partendo dalla versione di esp-sr inclusa nel core:

```bash
git clone https://github.com/espressif/esp-sr && git -C esp-sr checkout 0a6e15d5ea9c7f5402093439f3af4c9250ae0d8d
mkdir -p pack/wn9_jarvis_tts && cp esp-sr/model/wakenet_model/wn9_jarvis_tts/* pack/wn9_jarvis_tts/
python3 -I esp-sr/model/pack_model.py -m pack -o srmodels_jarvis.bin
python3 -I tools/gen_model_header.py pack/srmodels_jarvis.bin Jarvis/sr_model_jarvis.h \
  --esp-sr-commit 0a6e15d5ea9c7f5402093439f3af4c9250ae0d8d --esp-sr-version 2.5.3
```

Il risultato atteso è un'immagine di 291.168 byte con CRC-32 `0x69459385`.

## 7. Diagnostica

Il firmware scrive un log dettagliato sulla USB CDC (115200 baud). Le righe da controllare al primo avvio sono nel §9 della specifica. Anche senza monitor seriale, la pagina web mostra revisione della scheda, Wi-Fi, batteria, memoria e ultimo errore.
