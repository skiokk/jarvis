#pragma once
// board.h - Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1: SH8601+FT3168, V2: CO5300+CST820).
// Pin da pin_config.h Waveshare (identico V1/V2) e dallo schema elettrico ufficiale.

#include <Wire.h>
#include <esp_memory_utils.h>
#include "util.h"
#include "es8311.h"

// --- Pin ---------------------------------------------------------------------
#define PIN_LCD_SDIO0 4
#define PIN_LCD_SDIO1 5
#define PIN_LCD_SDIO2 6
#define PIN_LCD_SDIO3 7
#define PIN_LCD_SCLK 11
#define PIN_LCD_CS 12
// GPIO13 = LCD_TE (tearing effect): non usato, non toccare.
#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 14
#define PIN_TP_INT 21
#define PIN_I2S_MCLK 16
#define PIN_I2S_BCLK 9
#define PIN_I2S_WS 45     // strapping VDD_SPI: nessun pull-up
#define PIN_I2S_DOUT 8    // ESP32 -> ES8311 (DSDIN)
#define PIN_I2S_DIN 10    // ES8311 (ASDOUT) -> ESP32
#define PIN_PA 46         // NS4150B CTRL, attivo alto (pull-down 10k sulla scheda)
#define PIN_BOOT 0
#define LCD_W 368
#define LCD_H 448

// --- Indirizzi I2C -----------------------------------------------------------
#define ADDR_TCA9554 0x20
#define ADDR_AXP2101 0x34
#define ADDR_ES8311 0x18
#define ADDR_FT3168 0x38   // V1
#define ADDR_CST820 0x15   // V2

// TCA9554: EXIO0 LCD_RESET, EXIO1 DSI_PWR_EN, EXIO2 TP_RESET, EXIO3 RTC_INT,
// EXIO4 tasto PWR (ALTO = premuto), EXIO5 AXP_IRQ, EXIO6 IMU_INT, EXIO7 SD_CS.
// Il pin INT del TCA9554 NON è collegato all'ESP32: PWR non può risvegliare dal deep sleep.
#define TCA_REG_INPUT 0x00
#define TCA_REG_OUTPUT 0x01
#define TCA_REG_CONFIG 0x03
#define TCA_OUT_MASK 0x87  // EXIO0,1,2,7 uscite

static SemaphoreHandle_t i2cLock = nullptr;  // mutex ricorsivo attorno a ogni accesso I2C

// --- I2C di base (con i2cLock) -------------------------------------------------
static bool i2cProbe(uint8_t addr) {
  LockGuard g(i2cLock, 300);
  if (!g.ok) return false;
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  LockGuard g(i2cLock, 300);
  if (!g.ok) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  LockGuard g(i2cLock, 300);
  if (!g.ok) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom(addr, n);
  if (got != n) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &v) { return i2cReadRegs(addr, reg, &v, 1); }

// Read-modify-write atomico rispetto agli altri task
static bool i2cUpdateReg(uint8_t addr, uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  LockGuard g(i2cLock, 300);
  if (!g.ok) return false;
  uint8_t v;
  if (!i2cReadReg(addr, reg, v)) return false;
  uint8_t nv = (uint8_t)((v & ~clearMask) | setMask);
  if (nv == v) return true;
  return i2cWriteReg(addr, reg, nv);
}

// --- TCA9554 --------------------------------------------------------------------
static bool gTcaOk = false;

// Sequenza come board_variant.c di Waveshare: EXIO0/1/2 bassi (EXIO7 alto) per 20 ms,
// poi alti, attesa 150 ms prima di sondare il touch.
static bool tcaInit() {
  LockGuard g(i2cLock, 500);
  if (!g.ok) return false;
  bool ok = i2cWriteReg(ADDR_TCA9554, TCA_REG_CONFIG, (uint8_t)~TCA_OUT_MASK);
  ok = ok && i2cWriteReg(ADDR_TCA9554, TCA_REG_OUTPUT, 0x80);
  delay(20);
  ok = ok && i2cWriteReg(ADDR_TCA9554, TCA_REG_OUTPUT, TCA_OUT_MASK);
  gTcaOk = ok;
  return ok;
}

static int tcaReadInputs() {
  uint8_t v;
  if (!gTcaOk || !i2cReadReg(ADDR_TCA9554, TCA_REG_INPUT, v)) return -1;
  return v;
}

// --- AXP2101 ------------------------------------------------------------------------
static bool gAxpOk = false;

struct BattInfo {
  bool valid = false;
  bool vbus = false;      // USB presente (REG00 bit5)
  bool present = false;   // batteria presente (REG00 bit3)
  bool charging = false;  // REG01 bit[6:5] == 01
  int pct = -1;           // REG A4 (0-100)
};

// Inizializzazione: solo registri necessari, sempre in read-modify-write.
static bool axpInit(int chgMa) {
  uint8_t id = 0;
  if (!i2cReadReg(ADDR_AXP2101, 0x03, id)) {
    LOGE("AXP2101 non risponde su 0x34");
    return false;
  }
  if (id != 0x4A) LOGW("AXP2101: ID inatteso 0x%02X (atteso 0x4A)", id);
  // Pin TS: la scheda non ha NTC. REG50 bit4=1 = il TS non influenza il caricatore
  // (come XPowersLib >= 0.3.0 disableTSPinMeasure: REG50 = (REG50 & 0xF0) | 0x10), poi canale ADC TS off (REG30 bit1).
  bool ok = i2cUpdateReg(ADDR_AXP2101, 0x50, 0x0F, 0x10);  // bit[3:0] a 0 (corrente TS spenta), bit4 = 1
  ok = i2cUpdateReg(ADDR_AXP2101, 0x30, 0x02, 0x00) && ok;
  // Fuel gauge: REG18 bit3 (già 1 al power-on). RMW: il bit1 dello stesso registro abilita la ricarica.
  ok = i2cUpdateReg(ADDR_AXP2101, 0x18, 0x00, 0x08) && ok;
  // Corrente di ricarica (REG62 bit[4:0]): 25 mA*N fino a 200 mA, poi 200+100*(N-8)
  if (chgMa > 0) {
    uint8_t n = chgMa <= 200 ? (uint8_t)(chgMa / 25) : (uint8_t)(8 + (chgMa - 200) / 100);
    ok = i2cUpdateReg(ADDR_AXP2101, 0x62, 0x1F, n & 0x1F) && ok;
  }
  uint8_t r18 = 0, r50 = 0, r62 = 0;
  i2cReadReg(ADDR_AXP2101, 0x18, r18);
  i2cReadReg(ADDR_AXP2101, 0x50, r50);
  i2cReadReg(ADDR_AXP2101, 0x62, r62);
  LOGI("AXP2101 ID 0x%02X: REG18=0x%02X REG50=0x%02X REG62=0x%02X (%s)", id, r18, r50, r62, ok ? "ok" : "errori");
  gAxpOk = true;
  return ok;
}

static BattInfo axpRead() {
  BattInfo b;
  if (!gAxpOk) return b;
  uint8_t st[2];
  if (!i2cReadRegs(ADDR_AXP2101, 0x00, st, 2)) return b;
  b.vbus = st[0] & (1 << 5);
  b.present = st[0] & (1 << 3);
  b.charging = ((st[1] >> 5) & 0x03) == 0x01;
  if (b.present) {
    uint8_t p;
    if (i2cReadReg(ADDR_AXP2101, 0xA4, p) && p <= 100) b.pct = p;
  }
  b.valid = true;
  return b;
}

// --- Touch (registro 0x02 = numero di dita su entrambi i chip) ---------------------
static int gBoardRev = 0;   // 1 = V1, 2 = V2
static uint8_t gTouchAddr = 0;
static bool gTouchOk = false;

static int boardDetect(int forced) {
  int rev = 0;
  for (int i = 0; i < 4 && !rev; i++) {
    if (i2cProbe(ADDR_CST820)) rev = 2;       // V2 prima, come la logica Waveshare
    else if (i2cProbe(ADDR_FT3168)) rev = 1;
    else delay(50);
  }
  const int found = rev;
  uint8_t chip = 0;
  if (found == 2 && i2cReadReg(ADDR_CST820, 0xA7, chip)) LOGI("Touch CST8xx su 0x15, chip ID 0x%02X (CST820 = 0xB7)", chip);
  if (found == 1 && i2cReadReg(ADDR_FT3168, 0xA0, chip)) LOGI("Touch FT3x68 su 0x38, chip ID 0x%02X", chip);
  if (forced == 1 || forced == 2) {
    if (found && found != forced) LOGW("Revisione forzata V%d ma il touch rilevato indica V%d", forced, found);
    rev = forced;
  }
  if (rev == 0) {
    LOGW("Touch non trovato (né 0x15 né 0x38): assumo V2 (V1 fuori produzione dal 30/05/2026)");
    rev = 2;
  }
  gTouchOk = (found != 0);
  gTouchAddr = rev == 2 ? ADDR_CST820 : ADDR_FT3168;
  gBoardRev = rev;
  return rev;
}

static void touchConfigure() {
  if (!gTouchOk) return;
  if (gBoardRev == 1) {
    // FT3168: power mode "monitor" (impulso basso su INT al tocco), come il driver Waveshare
    i2cWriteReg(ADDR_FT3168, 0xA5, 0x01);
  } else {
    // CST820: interrupt periodico mentre il dito è appoggiato (esempio Waveshare 02)
    i2cWriteReg(ADDR_CST820, 0xFA, 0x40);
  }
  delay(20);
}

// -1 = errore I2C (es. CST820 in auto-sleep), altrimenti numero di dita
static int touchFingers() {
  if (!gTouchAddr) return -1;
  uint8_t v;
  if (!i2cReadReg(gTouchAddr, 0x02, v)) return -1;
  v &= 0x0F;
  return v <= 2 ? v : -1;  // FT3168 max 2 punti, CST820 1: valori più alti = lettura sporca
}

// --- Display -------------------------------------------------------------------------
#include <U8g2lib.h>  // prima di Arduino_GFX: abilita i font U8g2 (U8G2_FONT_SUPPORT)
#include <Arduino_GFX_Library.h>

static Arduino_DataBus *gBus = nullptr;
static Arduino_OLED *gPanel = nullptr;  // SH8601 e CO5300 derivano da Arduino_OLED (setBrightness)
static Arduino_Canvas *gCanvas = nullptr;

static bool displayCreate(int rev) {
  gBus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0, PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
  if (rev == 1) {
    gPanel = new Arduino_SH8601(gBus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H);  // V1: nessun offset
  } else {
    gPanel = new Arduino_CO5300(gBus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H, 16, 0, 0, 0);  // V2: colonna +16
  }
  if (!gBus || !gPanel) return false;
  gCanvas = new Arduino_Canvas(LCD_W, LCD_H, gPanel, 0, 0, 0);
  if (!gCanvas) return false;
  if (!gCanvas->begin()) {
    LOGE("Canvas: begin fallito (PSRAM per il framebuffer da %u byte?)", (unsigned)(LCD_W * LCD_H * 2));
    return false;
  }
  if (!esp_ptr_external_ram(gCanvas->getFramebuffer())) LOGW("Framebuffer non in PSRAM");
  gCanvas->fillScreen(0x0000);
  gCanvas->flush();
  gPanel->setBrightness(200);
  return true;
}

// --- Codec ES8311 (tutte le chiamate sotto i2cLock: il driver fa read-modify-write) ---
static es8311_handle_t gCodec = nullptr;

static bool codecInit(uint32_t rate, int micGain) {
  LockGuard g(i2cLock, 1000);
  if (!g.ok) return false;
  if (!gCodec) gCodec = es8311_create(0, ES8311_ADDRESS_0);  // porta 0 = Wire
  if (!gCodec) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = (int)(rate * 256),
    .sample_frequency = (int)rate,
  };
  if (es8311_init(gCodec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_microphone_config(gCodec, false) != ESP_OK) return false;  // microfono analogico
  if (es8311_microphone_gain_set(gCodec, (es8311_mic_gain_t)constrain(micGain, 0, 7)) != ESP_OK) return false;
  return true;
}

static bool codecSetRate(uint32_t rate) {
  if (!gCodec) return false;
  LockGuard g(i2cLock, 1000);
  if (!g.ok) return false;
  return es8311_sample_frequency_config(gCodec, (int)(rate * 256), (int)rate) == ESP_OK;
}

// Volume utente 0-100 -> REG32 (0,5 dB/passo, 0xBF = 0 dB). 1..100 = -40..0 dB, 0 = muto
// (stessa scala dell'amplificatore esterno). Mai sopra 0 dB: oltre, l'ES8311 aggiunge guadagno
// digitale e i picchi dei contenuti a fondo scala (radio MP3, voce) verrebbero tagliati.
static uint8_t codecVolReg(int vol) {
  if (vol <= 0) return 0;
  float db = -40.0f + (vol - 1) * (40.0f / 99.0f);
  int reg = 0xBF + (int)lroundf(db * 2.0f);
  return (uint8_t)constrain(reg, 1, 0xBF);
}

static bool codecSetVolume(int vol) {
  if (!gCodec) return false;
  return i2cWriteReg(ADDR_ES8311, 0x32, codecVolReg(vol));
}

static bool codecMute(bool m) {
  if (!gCodec) return false;
  LockGuard g(i2cLock, 500);
  if (!g.ok) return false;
  return es8311_voice_mute(gCodec, m) == ESP_OK;
}

static bool codecSetMicGain(int gain) {
  if (!gCodec) return false;
  LockGuard g(i2cLock, 500);
  if (!g.ok) return false;
  return es8311_microphone_gain_set(gCodec, (es8311_mic_gain_t)constrain(gain, 0, 7)) == ESP_OK;
}

// Sequenza di sospensione di esp_codec_dev (es8311_suspend) per lo standby
static void codecSuspend() {
  if (!gCodec) return;
  static const uint8_t seq[][2] = {
    {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02}, {0x14, 0x00}, {0x0D, 0xFA}, {0x15, 0x00},
    {0x02, 0x10}, {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00}, {0x45, 0x00}, {0x0D, 0xFC}, {0x02, 0x00},
  };
  LockGuard g(i2cLock, 500);
  if (!g.ok) return;
  for (auto &r : seq) i2cWriteReg(ADDR_ES8311, r[0], r[1]);
}

static inline void paSet(bool on) { digitalWrite(PIN_PA, on ? HIGH : LOW); }
