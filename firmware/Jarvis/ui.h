#pragma once
// ui.h - interfaccia su canvas 368x448 in PSRAM: occhi, sottotitoli, barra di stato,
// orologio-meteo, schermate setup/radio/messaggi. Disegna SOLO il task ui.
// Aggiornamento a bande: si ridisegna e si invia al pannello solo la fascia cambiata.
// Gli angoli dell'area attiva sono arrotondati (~90 px): niente icone negli angoli.

#include <WiFi.h>
#include <time.h>
#include "board.h"
#include "audio.h"

enum Screen : int { SCR_MESSAGE = 0, SCR_ASSIST, SCR_CLOCK, SCR_RADIO, SCR_SETUP, SCR_OTA };
enum Eyes : int { EYES_IDLE = 0, EYES_LISTEN, EYES_THINK, EYES_SPEAK, EYES_ERROR, EYES_SLEEP };
enum SubMode : int { SUB_NONE = 0, SUB_SYNC, SUB_TIME, SUB_ALL };

#define COL_BLACK 0x0000
#define COL_WHITE 0xFFFF
#define COL_EYE RGB565(80, 215, 255)
#define COL_EYE_HI RGB565(150, 240, 255)
#define COL_EYE_SAD RGB565(110, 150, 255)
#define COL_DIM RGB565(140, 140, 155)
#define COL_DARK RGB565(45, 45, 55)
#define COL_YELLOW RGB565(255, 210, 0)
#define COL_GREEN RGB565(60, 220, 90)
#define COL_RED RGB565(255, 70, 60)
#define COL_BLUE RGB565(90, 160, 255)
#define COL_ORANGE RGB565(255, 160, 40)

struct Weather {
  bool ok = false;
  float temp = 0, wind = 0;
  int hum = 0, code = 0;
  bool isDay = true;
  int dCode[3] = {0, 0, 0};
  float dMax[3] = {0, 0, 0}, dMin[3] = {0, 0, 0};
  int dWday[3] = {0, 0, 0};
  char city[48] = "";
};

static SemaphoreHandle_t uiLock = nullptr;
static std::atomic<int> gScreen{SCR_MESSAGE};
static std::atomic<int> gEyes{EYES_SLEEP};
static std::atomic<int> gOtaPct{-1};
static std::atomic<bool> gUiSleepReq{false};
static std::atomic<bool> gUiSleepAck{false};
static std::atomic<bool> gUiWake{false};

// Sottotitoli: la risposta può arrivare a 600 token (gpt-oss), circa 2.700 caratteri: buffer in PSRAM.
#define SUB_MAX 4096    // byte di testo
#define SUB_LINES 160   // righe da ~25 caratteri

// Dati protetti da uiLock
static Weather gWeather;
static uint32_t gWeatherVer = 0;
static char *gSubText = nullptr;  // SUB_MAX byte in PSRAM (uiInit)
static uint32_t gSubVer = 0;
static int gSubMode = SUB_NONE;
static uint32_t gSubStartFrames = 0, gSubRate = SR_RATE, gSubStartMs = 0;
static uint32_t gSubTotalFrames = 0;  // durata della voce (0 = non ancora nota: stima a 14 caratteri/s)
static bool gSetupNeedsPass = false;  // configurazione automatica con pagina protetta (scritto prima del task web)
static char gToast[100];
static uint32_t gToastUntil = 0, gToastVer = 0;
static char gMsgTitle[48], gMsg1[100], gMsg2[100];
static uint32_t gMsgVer = 0;
static char gStation[64];
static uint32_t gStationVer = 0;

// --- API thread-safe (loop, web) ---------------------------------------------------------------------
static void uiSetScreen(int s) { gScreen.store(s); }
static void uiSetEyes(int e) { gEyes.store(e); }

// Tutto il testo disegnato passa da cleanText (font Latin-1: ’ “ ” – — … e emoji non esistono).
static void uiSubtitle(const String &text, int mode, uint32_t startFrames, uint32_t rate) {
  String t = cleanText(text, false);
  LockGuard g(uiLock, 200);
  if (!g.ok || !gSubText) return;
  safeCopy(gSubText, SUB_MAX, t.c_str());
  gSubMode = mode;
  gSubStartFrames = startFrames;
  gSubRate = rate ? rate : SR_RATE;
  gSubStartMs = millis();
  gSubTotalFrames = 0;
  gSubVer++;
}
// Voce tutta in coda: da qui i sottotitoli avanzano in proporzione ai frame riprodotti
static void uiSubtitleSync(uint32_t startFrames, uint32_t totalFrames) {
  LockGuard g(uiLock, 200);
  if (!g.ok || gSubMode != SUB_SYNC) return;
  gSubStartFrames = startFrames;
  gSubTotalFrames = totalFrames;
}
static void uiSubtitleMode(int mode) {
  LockGuard g(uiLock, 200);
  if (!g.ok) return;
  gSubMode = mode;
  gSubVer++;
}
static void uiSubtitleClear() { uiSubtitle(String(), SUB_NONE, 0, 0); }

static void uiToast(const String &text, uint32_t ms) {
  String t = cleanText(text, false);
  LockGuard g(uiLock, 200);
  if (!g.ok) return;
  safeCopy(gToast, sizeof(gToast), t.c_str());
  gToastUntil = millis() + ms;
  gToastVer++;
}

static void uiMessage(const char *title, const String &l1, const String &l2) {
  String a = cleanText(l1, false), b = cleanText(l2, false);
  LockGuard g(uiLock, 200);
  if (!g.ok) return;
  safeCopy(gMsgTitle, sizeof(gMsgTitle), title);
  safeCopy(gMsg1, sizeof(gMsg1), a.c_str());
  safeCopy(gMsg2, sizeof(gMsg2), b.c_str());
  gMsgVer++;
}

static void uiSetStation(const String &name) {
  String n = cleanText(name, false);  // nomi radio-browser: spesso con – ’ ed emoji
  LockGuard g(uiLock, 200);
  if (!g.ok) return;
  safeCopy(gStation, sizeof(gStation), n.c_str());
  gStationVer++;
}

static void uiSetWeather(const Weather &w) {
  LockGuard g(uiLock, 200);
  if (!g.ok) return;
  gWeather = w;
  gWeatherVer++;
}

// Spegne il pannello (deep sleep): lo fa il task ui, che possiede il bus QSPI.
static void uiSleep() {
  gUiSleepAck.store(false);
  gUiSleepReq.store(true);
  uint32_t t0 = millis();
  while (!gUiSleepAck.load() && millis() - t0 < 1500) vTaskDelay(pdMS_TO_TICKS(10));
}

// --- Primitive di disegno ------------------------------------------------------------------------------
static inline uint32_t hmix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hstr(uint32_t h, const char *s) {
  while (*s) h = hmix(h, (uint8_t)*s++);
  return h;
}

static void flushBand(int y0, int y1) {
  if (!gCanvas || !gPanel) return;
  y0 &= ~1;                     // righe pari (finestra di indirizzo allineata)
  y1 = (y1 + 1) & ~1;
  if (y0 < 0) y0 = 0;
  if (y1 > LCD_H) y1 = LCD_H;
  if (y1 <= y0) return;
  uint16_t *fb = gCanvas->getFramebuffer();
  gPanel->draw16bitRGBBitmap(0, y0, fb + (size_t)y0 * LCD_W, LCD_W, y1 - y0);
}

static void clearBand(int y0, int y1) { gCanvas->fillRect(0, y0, LCD_W, y1 - y0, COL_BLACK); }

static uint16_t textWidth(const char *s, const uint8_t *font) {
  gCanvas->setFont(font);
  int16_t x1, y1;
  uint16_t w, h;
  gCanvas->getTextBounds(s, 0, 100, &x1, &y1, &w, &h);
  return w;
}

// align: 0 sinistra, 1 centro, 2 destra; y = linea di base
static void drawText(const char *s, const uint8_t *font, int x, int y, uint16_t col, int align) {
  gCanvas->setFont(font);
  gCanvas->setTextColor(col);
  int xx = x;
  if (align) {
    int16_t x1, y1;
    uint16_t w, h;
    gCanvas->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
    xx = align == 1 ? x - (int)w / 2 - x1 : x - (int)w - x1;
  }
  gCanvas->setCursor(xx, y);
  gCanvas->print(s);
}

// Tronca con "..." per stare in maxW pixel (UTF-8 sicuro)
static void fitText(const char *in, char *out, size_t cap, const uint8_t *font, int maxW) {
  safeCopy(out, cap, in);
  if (textWidth(out, font) <= maxW) return;
  size_t len = strlen(out);
  while (len > 0) {
    do { len--; } while (len > 0 && ((uint8_t)out[len] & 0xC0) == 0x80);
    out[len] = 0;
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s...", out);
    if (textWidth(tmp, font) <= maxW) { safeCopy(out, cap, tmp); return; }
  }
}

// --- Icone di stato ------------------------------------------------------------------------------------------
static void drawWifi(int x, int y, int bars, bool connected, int scale) {
  for (int i = 0; i < 4; i++) {
    int h = (i + 1) * 4 * scale;
    uint16_t c = (connected && i < bars) ? COL_WHITE : COL_DARK;
    gCanvas->fillRoundRect(x + i * 6 * scale, y + 16 * scale - h, 4 * scale, h, scale, c);
  }
  if (!connected) {
    gCanvas->drawLine(x + 14 * scale, y, x + 22 * scale, y + 8 * scale, COL_RED);
    gCanvas->drawLine(x + 22 * scale, y, x + 14 * scale, y + 8 * scale, COL_RED);
  }
}

static void drawBolt(int cx, int cy, int s, uint16_t c) {
  gCanvas->fillTriangle(cx + s / 4, cy - s, cx - s / 2, cy + s / 6, cx + s / 8, cy + s / 6, c);
  gCanvas->fillTriangle(cx - s / 4, cy + s, cx + s / 2, cy - s / 6, cx - s / 8, cy - s / 6, c);
}

static void drawBattery(int x, int y, int w, int h, const BattInfo &b) {
  if (!b.valid) return;
  if (!b.present) {
    drawText("USB", u8g2_font_helvB12_tf, x + w / 2, y + h - 2, COL_WHITE, 1);
    return;
  }
  gCanvas->drawRoundRect(x, y, w, h, 3, COL_WHITE);
  gCanvas->fillRect(x + w, y + h / 3, 3, h / 3, COL_WHITE);
  int pct = b.pct < 0 ? 0 : b.pct;
  int fw = (w - 4) * pct / 100;
  uint16_t c = pct <= 15 ? COL_RED : (pct <= 30 ? COL_ORANGE : COL_GREEN);
  if (fw > 0) gCanvas->fillRect(x + 2, y + 2, fw, h - 4, c);
  if (b.charging || (b.vbus && pct < 100)) drawBolt(x + w / 2, y + h / 2, h / 2 + 2, COL_YELLOW);
}

// --- Meteo -------------------------------------------------------------------------------------------------
enum WIcon { W_CLEAR, W_PARTLY, W_CLOUD, W_FOG, W_RAIN, W_SNOW, W_STORM };
static int wmoIcon(int c) {
  if (c == 0) return W_CLEAR;
  if (c == 1 || c == 2) return W_PARTLY;
  if (c == 3) return W_CLOUD;
  if (c == 45 || c == 48) return W_FOG;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return W_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return W_SNOW;
  if (c >= 95 && c <= 99) return W_STORM;  // 95, 96, 97 (nuovo nella tabella), 99
  return W_CLOUD;
}
static const char *wmoText(int c) {
  switch (c) {
    case 0: return "Sereno";
    case 1: return "Quasi sereno";
    case 2: return "Parz. nuvoloso";
    case 3: return "Coperto";
    case 45: return "Nebbia";
    case 48: return "Nebbia e brina";
    case 51: case 53: return "Pioviggine";
    case 55: return "Pioviggine forte";
    case 56: case 57: return "Pioviggine gelata";
    case 61: return "Pioggia debole";
    case 63: return "Pioggia";
    case 65: return "Pioggia forte";
    case 66: case 67: return "Pioggia gelata";
    case 71: return "Neve debole";
    case 73: return "Neve";
    case 75: return "Neve forte";
    case 77: return "Nevischio";
    case 80: return "Rovesci deboli";
    case 81: return "Rovesci";
    case 82: return "Rovesci violenti";
    case 85: case 86: return "Rovesci di neve";
    case 95: return "Temporale";
    case 96: case 99: return "Temporale e grandine";
    case 97: return "Temporale forte";
    default: return "Variabile";
  }
}

static void drawCloud(int cx, int cy, int s, uint16_t c) {
  gCanvas->fillCircle(cx - s * 18 / 100, cy + s * 4 / 100, s * 17 / 100, c);
  gCanvas->fillCircle(cx + s * 3 / 100, cy - s * 7 / 100, s * 22 / 100, c);
  gCanvas->fillCircle(cx + s * 22 / 100, cy + s * 6 / 100, s * 15 / 100, c);
  gCanvas->fillRoundRect(cx - s * 35 / 100, cy + s * 4 / 100, s * 72 / 100, s * 17 / 100, s * 8 / 100, c);
}
static void drawSun(int cx, int cy, int s) {
  for (int i = 0; i < 8; i++) {
    float a = i * (float)M_PI / 4;
    int x0 = cx + (int)(cosf(a) * s * 0.30f), y0 = cy + (int)(sinf(a) * s * 0.30f);
    int x1 = cx + (int)(cosf(a) * s * 0.44f), y1 = cy + (int)(sinf(a) * s * 0.44f);
    gCanvas->drawLine(x0, y0, x1, y1, COL_YELLOW);
    gCanvas->drawLine(x0 + 1, y0, x1 + 1, y1, COL_YELLOW);
    gCanvas->drawLine(x0, y0 + 1, x1, y1 + 1, COL_YELLOW);
  }
  gCanvas->fillCircle(cx, cy, s * 22 / 100, COL_YELLOW);
}
static void drawMoon(int cx, int cy, int s) {
  gCanvas->fillCircle(cx, cy, s * 28 / 100, RGB565(230, 230, 200));
  gCanvas->fillCircle(cx + s * 14 / 100, cy - s * 9 / 100, s * 24 / 100, COL_BLACK);
}
static void drawWeatherIcon(int cx, int cy, int s, int code, bool day) {
  int ic = wmoIcon(code);
  const uint16_t cloudC = RGB565(210, 215, 225), darkC = RGB565(130, 135, 150);
  switch (ic) {
    case W_CLEAR: if (day) drawSun(cx, cy, s); else drawMoon(cx, cy, s); break;
    case W_PARTLY:
      if (day) drawSun(cx - s / 6, cy - s / 6, s * 3 / 4); else drawMoon(cx - s / 6, cy - s / 6, s * 3 / 4);
      drawCloud(cx + s / 10, cy + s / 8, s * 3 / 4, cloudC);
      break;
    case W_CLOUD: drawCloud(cx, cy, s, cloudC); break;
    case W_FOG:
      for (int i = 0; i < 4; i++) gCanvas->fillRoundRect(cx - s * 36 / 100 + (i & 1) * s / 12, cy - s / 4 + i * s * 15 / 100, s * 66 / 100, s / 14 + 1, 2, cloudC);
      break;
    case W_RAIN:
      drawCloud(cx, cy - s / 8, s, cloudC);
      for (int i = 0; i < 3; i++) {
        int x = cx - s / 4 + i * s / 4;
        gCanvas->drawLine(x, cy + s / 5, x - s / 10, cy + s * 2 / 5, COL_BLUE);
        gCanvas->drawLine(x + 1, cy + s / 5, x - s / 10 + 1, cy + s * 2 / 5, COL_BLUE);
      }
      break;
    case W_SNOW:
      drawCloud(cx, cy - s / 8, s, cloudC);
      for (int i = 0; i < 3; i++) gCanvas->fillCircle(cx - s / 4 + i * s / 4, cy + s * 3 / 10 + (i & 1) * s / 12, s / 18 + 1, COL_WHITE);
      break;
    case W_STORM:
      drawCloud(cx, cy - s / 8, s, darkC);
      drawBolt(cx, cy + s * 3 / 10, s / 6, COL_YELLOW);
      break;
  }
}

// --- Occhi -------------------------------------------------------------------------------------------------
static void drawEye(int cx, int cy, int w, int h, int mood, bool left, uint16_t col) {
  if (w < 4 || h < 2) return;
  int r = (w < h ? w : h) / 3;
  if (r > h / 2) r = h / 2;
  gCanvas->fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, col);
  if (mood == EYES_SPEAK) {
    // occhi sorridenti: un'ellisse nera "morde" la parte bassa
    gCanvas->fillEllipse(cx, cy + h / 2 + h / 10, w * 62 / 100, h * 55 / 100, COL_BLACK);
  } else if (mood == EYES_ERROR) {
    // palpebre tristi: abbassate verso l'esterno
    int x0 = cx - w / 2 - 2, x1 = cx + w / 2 + 2, y0 = cy - h / 2 - 2;
    if (left) gCanvas->fillTriangle(x0, y0, x1, y0, x0, cy + h / 10, COL_BLACK);
    else gCanvas->fillTriangle(x1, y0, x0, y0, x1, cy + h / 10, COL_BLACK);
  } else if (mood == EYES_THINK) {
    gCanvas->fillRect(cx - w / 2 - 1, cy - h / 2 - 1, w + 2, h / 5, COL_BLACK);
  }
}

// Occhi chiusi felici "∩" (radio)
static void drawHappyClosed(int cx, int cy, uint16_t col) {
  gCanvas->fillEllipse(cx, cy, 46, 36, col);
  gCanvas->fillEllipse(cx, cy + 10, 34, 28, COL_BLACK);
  gCanvas->fillRect(cx - 50, cy + 4, 100, 40, COL_BLACK);
}

// --- Stato condiviso letto dal task ui -----------------------------------------------------------------------------
static BattInfo gBatt;  // aggiornata solo dal task ui ogni 5 s
static std::atomic<int> gBattPct{-1};

struct EyeAnim {
  float gx = 0, gy = 0, tgx = 0, tgy = 0, scale = 1.0f;
  uint32_t nextSaccade = 0, blinkStart = 0, nextBlink = 0;
};
static EyeAnim gEA;

// Righe dei sottotitoli precalcolate
struct SubLines {
  char text[SUB_MAX];
  uint16_t start[SUB_LINES], len[SUB_LINES], chars[SUB_LINES];
  int n = 0;
  uint32_t totalChars = 0;
};
static SubLines *gSL = nullptr;  // in PSRAM
static char *gSubCopy = nullptr; // copia di lavoro del task ui (PSRAM)

static void wrapSubtitle(const char *text) {
  SubLines &L = *gSL;
  safeCopy(L.text, sizeof(L.text), text);
  L.n = 0;
  L.totalChars = utf8Len(L.text);
  const int maxW = 312;
  gCanvas->setFont(u8g2_font_helvR18_tf);
  size_t pos = 0, len = strlen(L.text);
  char line[200];
  while (pos < len && L.n < SUB_LINES) {
    while (pos < len && L.text[pos] == ' ') pos++;
    size_t lineStart = pos, lastFit = pos;
    size_t p = pos;
    while (p <= len) {
      size_t q = p;
      while (q < len && L.text[q] != ' ') q++;
      size_t candLen = q - lineStart;
      if (candLen >= sizeof(line)) candLen = sizeof(line) - 1;
      memcpy(line, L.text + lineStart, candLen);
      line[candLen] = 0;
      if (textWidth(line, u8g2_font_helvR18_tf) > maxW) {
        if (lastFit == lineStart) {  // parola singola troppo lunga: taglio a caratteri
          size_t cut = lineStart;
          while (cut < q) {
            size_t nx = cut + 1;
            while (nx < q && ((uint8_t)L.text[nx] & 0xC0) == 0x80) nx++;
            size_t cl = nx - lineStart;
            if (cl >= sizeof(line)) break;
            memcpy(line, L.text + lineStart, cl);
            line[cl] = 0;
            if (textWidth(line, u8g2_font_helvR18_tf) > maxW && cut > lineStart) break;
            cut = nx;
          }
          lastFit = cut > lineStart ? cut : q;
        }
        break;
      }
      lastFit = q;
      if (q >= len) break;
      p = q + 1;
    }
    if (lastFit <= lineStart) lastFit = (len > lineStart) ? len : lineStart + 1;
    L.start[L.n] = (uint16_t)lineStart;
    L.len[L.n] = (uint16_t)(lastFit - lineStart);
    char tmp[200];
    size_t cl = L.len[L.n] < sizeof(tmp) - 1 ? L.len[L.n] : sizeof(tmp) - 1;
    memcpy(tmp, L.text + lineStart, cl);
    tmp[cl] = 0;
    L.chars[L.n] = (uint16_t)utf8Len(tmp);
    L.n++;
    pos = lastFit;
  }
}

// --- Schermata assistente --------------------------------------------------------------------------------------
static uint32_t bandHash[8];
static bool bandDirty(int i, uint32_t h) {
  if (bandHash[i] == h) return false;
  bandHash[i] = h;
  return true;
}
static void bandsInvalidate() {
  for (auto &h : bandHash) h = 0xFFFFFFFF;
}

// Stato batteria per gli hash delle barre di stato: drawBattery dipende anche da vbus (fulmine)
static uint32_t battHash(uint32_t h) {
  return hmix(hmix(h, (uint32_t)(gBatt.pct + 3)), (gBatt.vbus << 3) | (gBatt.charging << 2) | (gBatt.present << 1) | gBatt.valid);
}

static int rssiBars() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  int r = WiFi.RSSI();
  return r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : r >= -85 ? 1 : 0;
}

static bool localTime(struct tm &t) {
  time_t now = time(nullptr);
  LockGuard g(uiLock, 50);  // il loop cambia TZ sotto uiLock
  localtime_r(&now, &t);
  return t.tm_year >= 124;
}

static void drawStatusSmall(int bars, const struct tm &t, bool tv) {
  clearBand(0, 46);
  drawWifi(66, 16, bars < 0 ? 0 : bars, bars >= 0, 1);
  char hm[8];
  if (tv) snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  else strcpy(hm, "--:--");
  drawText(hm, u8g2_font_helvB12_tf, LCD_W / 2, 32, COL_WHITE, 1);
  drawBattery(262, 20, 26, 13, gBatt);
  if (gBatt.valid && gBatt.present && gBatt.pct >= 0) {
    char p[16];
    snprintf(p, sizeof(p), "%d%%", gBatt.pct);
    drawText(p, u8g2_font_helvR10_tf, 256, 31, COL_WHITE, 2);
  }
}

static void drawToastBand(int y0, int y1, const char *toast) {
  clearBand(y0, y1);
  if (!toast[0]) return;
  char t[100];
  fitText(toast, t, sizeof(t), u8g2_font_helvR14_tf, 300);
  int w = textWidth(t, u8g2_font_helvR14_tf) + 28;
  int yc = (y0 + y1) / 2;
  gCanvas->fillRoundRect(LCD_W / 2 - w / 2, yc - 16, w, 32, 16, COL_DARK);
  drawText(t, u8g2_font_helvR14_tf, LCD_W / 2, yc + 7, COL_WHITE, 1);
}

static void renderAssist(uint32_t now, bool full) {
  const int eyes = gEyes.load();
  struct tm t;
  bool tv = localTime(t);
  int bars = rssiBars();
  // --- barra di stato
  uint32_t h = battHash(hmix(hmix(1, tv ? t.tm_hour * 60 + t.tm_min : 9999), bars + 2));
  if (bandDirty(0, h) || full) { drawStatusSmall(bars, t, tv); flushBand(0, 46); }
  // --- avviso
  char toast[100] = "";
  {
    LockGuard g(uiLock, 20);
    if (g.ok && (int32_t)(gToastUntil - now) > 0) safeCopy(toast, sizeof(toast), gToast);
  }
  if (bandDirty(1, hstr(7, toast)) || full) { drawToastBand(46, 96, toast); flushBand(46, 96); }

  // --- occhi
  EyeAnim &A = gEA;
  float tScale = eyes == EYES_LISTEN ? 1.12f : 1.0f;
  A.scale += (tScale - A.scale) * 0.25f;
  if (eyes == EYES_IDLE || eyes == EYES_SPEAK) {
    if ((int32_t)(now - A.nextSaccade) >= 0) {
      A.tgx = (float)(esp_random() % 29) - 14.0f;
      A.tgy = (float)(esp_random() % 17) - 8.0f;
      if (esp_random() % 3 == 0) { A.tgx = 0; A.tgy = 0; }
      A.nextSaccade = now + 1500 + esp_random() % 2500;
    }
  } else if (eyes == EYES_THINK) { A.tgx = 10; A.tgy = -18; }
  else { A.tgx = 0; A.tgy = 0; }
  A.gx += (A.tgx - A.gx) * 0.3f;
  A.gy += (A.tgy - A.gy) * 0.3f;
  float blink = 0;
  if (eyes == EYES_IDLE || eyes == EYES_LISTEN || eyes == EYES_THINK) {
    if (A.nextBlink == 0) A.nextBlink = now + 2500;
    if ((int32_t)(now - A.nextBlink) >= 0) { A.blinkStart = now; A.nextBlink = now + 2500 + esp_random() % 3500; }
    uint32_t bt = now - A.blinkStart;
    if (bt < 180) blink = bt < 90 ? bt / 90.0f : (180 - bt) / 90.0f;
  }
  int ew = (int)(100 * A.scale), eh = (int)(124 * A.scale * (1.0f - 0.9f * blink));
  if (eyes == EYES_SLEEP) eh = 12;
  int gx = (int)lroundf(A.gx), gy = (int)lroundf(A.gy);
  int lvlMic = gMicLevel.load(), lvlPlay = gPlayLevel.load();
  int phase = (int)(now / 300) % 3;
  uint32_t fh = hmix(hmix(hmix(hmix(hmix(2, eyes), ew * 1000 + eh), (gx + 100) * 1000 + gy + 100), eyes == EYES_LISTEN ? lvlMic / 8 : 0),
                     eyes == EYES_SPEAK ? lvlPlay / 8 : (eyes == EYES_THINK ? phase : 0));
  if (eyes == EYES_LISTEN) fh = hmix(fh, (now / 80));  // barre del microfono sempre vive
  if (bandDirty(2, fh) || full) {
    clearBand(96, 306);
    const int cy = 196 + gy, lx = 112 + gx, rx = 256 + gx;
    uint16_t col = eyes == EYES_LISTEN ? COL_EYE_HI : (eyes == EYES_ERROR ? COL_EYE_SAD : COL_EYE);
    if (eyes == EYES_SLEEP) {
      gCanvas->fillRoundRect(lx - 45, 196, 90, 12, 6, COL_DIM);
      gCanvas->fillRoundRect(rx - 45, 196, 90, 12, 6, COL_DIM);
      drawText("z", u8g2_font_helvB18_tf, 300, 150, COL_DIM, 1);
      drawText("z", u8g2_font_helvB12_tf, 318, 128, COL_DIM, 1);
    } else {
      drawEye(lx, cy, ew, eh, eyes, true, col);
      drawEye(rx, cy, ew, eh, eyes, false, col);
    }
    if (eyes == EYES_LISTEN) {
      static const float wgt[9] = {0.35f, 0.55f, 0.75f, 0.92f, 1.0f, 0.92f, 0.75f, 0.55f, 0.35f};
      for (int i = 0; i < 9; i++) {
        int bh = 6 + (int)(lvlMic * 0.16f * wgt[i]) + (int)(esp_random() % 4);
        if (bh > 32) bh = 32;  // 274..306: dentro la fascia degli occhi (fino a 306)
        gCanvas->fillRoundRect(LCD_W / 2 - 64 + i * 14, 290 - bh / 2, 8, bh, 3, COL_EYE_HI);
      }
    } else if (eyes == EYES_THINK) {
      for (int i = 0; i < 3; i++) gCanvas->fillCircle(LCD_W / 2 - 26 + i * 26, 290, i == phase ? 8 : 5, i == phase ? COL_WHITE : COL_DIM);
    } else if (eyes == EYES_SPEAK) {
      int mw = 44 + lvlPlay / 4, mh = 8 + lvlPlay / 12;
      gCanvas->fillRoundRect(LCD_W / 2 - mw / 2, 286 - mh / 2, mw, mh, mh / 2, col);
    }
    flushBand(96, 306);
  }

  // --- sottotitoli / suggerimento
  uint32_t subVer, startF, rate, startMs, totalF;
  int mode;
  static uint32_t wrappedVer = 0xFFFFFFFF;
  static uint32_t lastReveal = 0;
  bool rewrap = false;
  {
    LockGuard g(uiLock, 20);
    if (!g.ok) return;
    subVer = gSubVer; mode = gSubMode; startF = gSubStartFrames; rate = gSubRate; startMs = gSubStartMs; totalF = gSubTotalFrames;
    if (subVer != wrappedVer) { safeCopy(gSubCopy, SUB_MAX, gSubText); rewrap = true; }
  }
  if (rewrap) { wrapSubtitle(gSubCopy); wrappedVer = subVer; lastReveal = 0; }  // misura del testo fuori dal lock
  uint32_t reveal = 0;
  if (mode == SUB_SYNC) {
    const uint32_t played = gPlayedFrames.load() - startF;
    // durata nota (voce tutta in coda): avanzamento proporzionale; prima, stima a 14 caratteri/s
    if (totalF) reveal = (uint32_t)((uint64_t)played * gSL->totalChars / totalF) + 2;
    else reveal = (uint32_t)((uint64_t)played * 14 / rate) + 2;
    if (reveal < lastReveal) reveal = lastReveal;  // mai nascondere testo già mostrato
  } else if (mode == SUB_TIME) reveal = (now - startMs) * 14 / 1000 + 2;
  else if (mode == SUB_ALL) reveal = gSL->totalChars;
  if (reveal > gSL->totalChars) reveal = gSL->totalChars;
  lastReveal = reveal;
  uint32_t sh = hmix(hmix(hmix(3, subVer), reveal), mode);
  if (mode == SUB_NONE || gSL->n == 0) sh = hmix(sh, eyes);
  if (bandDirty(3, sh) || full) {
    clearBand(306, LCD_H);
    if (mode != SUB_NONE && gSL->n > 0) {
      // ultima riga (parzialmente) rivelata e le due precedenti
      int last = 0;
      uint32_t acc = 0;
      for (int i = 0; i < gSL->n; i++) {
        if (acc < reveal || i == 0) last = i;
        acc += gSL->chars[i];
      }
      int first = last - 2 < 0 ? 0 : last - 2;
      uint32_t before = 0;
      for (int i = 0; i < first; i++) before += gSL->chars[i];
      gCanvas->setFont(u8g2_font_helvR18_tf);
      for (int i = first; i <= last; i++) {
        char ln[200];
        size_t L = gSL->len[i] < sizeof(ln) - 1 ? gSL->len[i] : sizeof(ln) - 1;
        memcpy(ln, gSL->text + gSL->start[i], L);
        ln[L] = 0;
        // posizione dalla riga INTERA: la parte rivelata cresce verso destra senza ricentrarsi
        int16_t bx, by;
        uint16_t bw, bh;
        gCanvas->getTextBounds(ln, 0, 0, &bx, &by, &bw, &bh);
        const int x = LCD_W / 2 - (int)bw / 2 - bx;
        uint32_t show = reveal > before ? reveal - before : 0;
        if (show < gSL->chars[i]) ln[utf8Offset(ln, show)] = 0;
        drawText(ln, u8g2_font_helvR18_tf, x, 336 + (i - first) * 32, COL_WHITE, 0);
        before += gSL->chars[i];
      }
    } else {
      const char *hint = eyes == EYES_LISTEN ? "Ti ascolto..." : eyes == EYES_THINK ? "Sto pensando..." : "Di' «Jarvis» o tieni premuto";
      drawText(hint, u8g2_font_helvR14_tf, LCD_W / 2, 424, COL_DIM, 1);
    }
    flushBand(306, LCD_H);
  }
}

// --- Schermata orologio-meteo --------------------------------------------------------------------------------------
static const char *const WDAY[] = {"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"};
static const char *const WDAY3[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
static const char *const MONTH[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};

static void renderClock(uint32_t now, bool full) {
  struct tm t;
  bool tv = localTime(t);
  int bars = rssiBars();
  // barra di stato grande
  uint32_t h = battHash(hmix(11, bars + 2));
  if (bandDirty(0, h) || full) {
    clearBand(0, 56);
    drawWifi(58, 18, bars < 0 ? 0 : bars, bars >= 0, 1);
    if (gBatt.valid && gBatt.present) {
      drawBattery(270, 22, 40, 20, gBatt);
      if (gBatt.pct >= 0) {
        char p[16];
        snprintf(p, sizeof(p), "%d%%", gBatt.pct);
        drawText(p, u8g2_font_helvB12_tf, 262, 39, COL_WHITE, 2);
      }
    } else if (gBatt.valid) {
      drawText("USB", u8g2_font_helvB12_tf, 290, 39, COL_WHITE, 1);
    }
    flushBand(0, 56);
  }
  // ora con due punti lampeggianti
  bool colon = (now / 500) % 2 == 0;
  uint32_t th = hmix(hmix(12, tv ? t.tm_hour * 60 + t.tm_min : 99999), colon);
  if (bandDirty(1, th) || full) {
    clearBand(56, 178);
    char hh[4], mm[4];
    if (tv) { snprintf(hh, sizeof(hh), "%02d", t.tm_hour); snprintf(mm, sizeof(mm), "%02d", t.tm_min); }
    else { strcpy(hh, "--"); strcpy(mm, "--"); }
    // logisoso92_tn ha cifre (e '-') a passo fisso: impaginazione sugli avanzamenti del font
    // (U8g2 2.36.19: 59 px per cifra, 34 px per ':'), non sulla larghezza dell'inchiostro.
    // Inchiostro centrato (56..312) e 17-18 px fra i due punti e le cifre, per ogni ora.
    const int ADV = 59, CADV = 34;
    const int x0 = LCD_W / 2 - (4 * ADV + CADV) / 2;
    drawText(hh, u8g2_font_logisoso92_tn, x0, 166, COL_WHITE, 0);
    if (colon || !tv) drawText(":", u8g2_font_logisoso92_tn, x0 + 2 * ADV, 166, COL_EYE, 0);
    drawText(mm, u8g2_font_logisoso92_tn, x0 + 2 * ADV + CADV, 166, COL_WHITE, 0);
    flushBand(56, 178);
  }
  // data o avviso
  char toast[100] = "";
  Weather w;
  uint32_t wver;
  {
    LockGuard g(uiLock, 20);
    if (g.ok) {
      if ((int32_t)(gToastUntil - now) > 0) safeCopy(toast, sizeof(toast), gToast);
      w = gWeather;
      wver = gWeatherVer;
    } else {
      wver = 0;
    }
  }
  uint32_t dh = hmix(hstr(13, toast), tv ? t.tm_yday + 1000 * t.tm_year : 0);
  if (bandDirty(2, dh) || full) {
    if (toast[0]) drawToastBand(178, 216, toast);
    else {
      clearBand(178, 216);
      char d[48];
      if (tv) {
        snprintf(d, sizeof(d), "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
        d[0] = (char)toupper((uint8_t)d[0]);
      } else {
        strcpy(d, "In attesa dell'ora...");
      }
      drawText(d, u8g2_font_helvB18_tf, LCD_W / 2, 206, COL_WHITE, 1);
    }
    flushBand(178, 216);
  }
  // meteo
  uint32_t wh2 = hmix(hmix(14, wver), w.ok);
  if (bandDirty(3, wh2) || full) {
    clearBand(216, LCD_H);
    if (!w.ok) {
      drawText(WiFi.status() == WL_CONNECTED ? "Meteo in arrivo..." : "Meteo non disponibile", u8g2_font_helvR14_tf, LCD_W / 2, 300, COL_DIM, 1);
    } else {
      drawWeatherIcon(102, 262, 84, w.code, w.isDay);
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "%d°", (int)lroundf(w.temp));
      drawText(tmp, u8g2_font_logisoso42_tf, 160, 284, COL_WHITE, 0);
      char desc[48];
      fitText(wmoText(w.code), desc, sizeof(desc), u8g2_font_helvR14_tf, 170);
      drawText(desc, u8g2_font_helvR14_tf, 160, 306, COL_DIM, 0);
      char city[40];
      fitText(w.city, city, sizeof(city), u8g2_font_helvR12_tf, 140);
      snprintf(tmp, sizeof(tmp), "%s · %d%% · %d km/h", city, w.hum, (int)lroundf(w.wind));
      char line[64];
      fitText(tmp, line, sizeof(line), u8g2_font_helvR12_tf, 300);
      drawText(line, u8g2_font_helvR12_tf, LCD_W / 2, 336, COL_WHITE, 1);
      for (int i = 0; i < 3; i++) {
        int cx = 104 + i * 80;
        drawText(WDAY3[w.dWday[i] % 7], u8g2_font_helvB12_tf, cx, 360, COL_WHITE, 1);
        drawWeatherIcon(cx, 380, 34, w.dCode[i], true);
        snprintf(tmp, sizeof(tmp), "%d° %d°", (int)lroundf(w.dMax[i]), (int)lroundf(w.dMin[i]));
        drawText(tmp, u8g2_font_helvR12_tf, cx, 414, COL_DIM, 1);
      }
    }
    flushBand(216, LCD_H);
  }
}

// --- Radio ----------------------------------------------------------------------------------------------------------
static void renderRadio(uint32_t now, bool full) {
  struct tm t;
  bool tv = localTime(t);
  int bars = rssiBars();
  uint32_t h = battHash(hmix(hmix(21, tv ? t.tm_hour * 60 + t.tm_min : 9999), bars + 2));
  if (bandDirty(0, h) || full) { drawStatusSmall(bars, t, tv); flushBand(0, 46); }
  char st[64];
  uint32_t sv;
  {
    LockGuard g(uiLock, 20);
    if (!g.ok) return;
    safeCopy(st, sizeof(st), gStation);
    sv = gStationVer;
  }
  if (bandDirty(1, hmix(22, sv)) || full) {
    clearBand(46, 236);
    drawHappyClosed(112, 170, COL_EYE);
    drawHappyClosed(256, 170, COL_EYE);
    flushBand(46, 236);
    clearBand(330, LCD_H);
    char name[64];
    fitText(st[0] ? st : "Radio", name, sizeof(name), u8g2_font_helvB18_tf, 300);
    drawText(name, u8g2_font_helvB18_tf, LCD_W / 2, 372, COL_WHITE, 1);
    drawText("Tocca lo schermo per fermare", u8g2_font_helvR14_tf, LCD_W / 2, 412, COL_DIM, 1);
    flushBand(330, LCD_H);
  }
  // equalizzatore animato con il livello audio
  static float bh[12];
  int lvl = gPlayLevel.load();
  clearBand(236, 330);
  for (int i = 0; i < 12; i++) {
    float target = 4 + lvl * (0.18f + 0.12f * (float)((esp_random() >> 8) % 100) / 100.0f);
    bh[i] += (target - bh[i]) * 0.5f;
    int hh = (int)bh[i];
    if (hh > 84) hh = 84;
    if (hh < 3) hh = 3;
    uint16_t c = RGB565(80 + i * 14, 215 - i * 10, 255);
    gCanvas->fillRoundRect(56 + i * 22, 326 - hh, 14, hh, 4, c);
  }
  flushBand(236, 330);
  (void)now;
}

// --- Messaggi (avvio, connessione, standby, errori) e setup -------------------------------------------------------------
static void renderMessage(bool full) {
  char t[48], a[100], b[100];
  uint32_t v;
  {
    LockGuard g(uiLock, 20);
    if (!g.ok) return;
    safeCopy(t, sizeof(t), gMsgTitle);
    safeCopy(a, sizeof(a), gMsg1);
    safeCopy(b, sizeof(b), gMsg2);
    v = gMsgVer;
  }
  if (!bandDirty(0, hmix(31, v)) && !full) return;
  gCanvas->fillScreen(COL_BLACK);
  gCanvas->fillRoundRect(112 - 45, 180, 90, 12, 6, COL_DIM);
  gCanvas->fillRoundRect(256 - 45, 180, 90, 12, 6, COL_DIM);
  char l[100];
  fitText(t, l, sizeof(l), u8g2_font_helvB18_tf, 310);
  drawText(l, u8g2_font_helvB18_tf, LCD_W / 2, 280, COL_WHITE, 1);
  fitText(a, l, sizeof(l), u8g2_font_helvR14_tf, 320);
  drawText(l, u8g2_font_helvR14_tf, LCD_W / 2, 318, COL_WHITE, 1);
  fitText(b, l, sizeof(l), u8g2_font_helvR12_tf, 300);
  drawText(l, u8g2_font_helvR12_tf, LCD_W / 2, 350, COL_DIM, 1);
  flushBand(0, LCD_H);
}

static char gApName[33] = "";
static void renderSetup(bool full) {
  char toast[100] = "";
  {
    LockGuard g(uiLock, 20);
    if (g.ok && (int32_t)(gToastUntil - millis()) > 0) safeCopy(toast, sizeof(toast), gToast);
  }
  if (!bandDirty(0, hstr(hstr(41, gApName), toast)) && !full) return;
  gCanvas->fillScreen(COL_BLACK);
  drawText("Configurazione", u8g2_font_helvB18_tf, LCD_W / 2, 92, COL_EYE, 1);
  drawText("1. Collegati alla rete Wi-Fi:", u8g2_font_helvR14_tf, LCD_W / 2, 150, COL_WHITE, 1);
  drawText(gApName, u8g2_font_helvB18_tf, LCD_W / 2, 186, COL_YELLOW, 1);
  drawText("2. Apri nel browser:", u8g2_font_helvR14_tf, LCD_W / 2, 238, COL_WHITE, 1);
  drawText("http://192.168.4.1", u8g2_font_helvB18_tf, LCD_W / 2, 274, COL_YELLOW, 1);
  if (gSetupNeedsPass) {  // configurazione partita da sola (Wi-Fi non raggiungibile): pagina protetta
    drawText("Pagina protetta: utente admin", u8g2_font_helvR12_tf, LCD_W / 2, 322, COL_DIM, 1);
    drawText("Password dimenticata? Tieni BOOT 5 s", u8g2_font_helvR12_tf, LCD_W / 2, 344, COL_DIM, 1);
  } else {
    drawText("Su molti telefoni la pagina", u8g2_font_helvR12_tf, LCD_W / 2, 322, COL_DIM, 1);
    drawText("si apre da sola.", u8g2_font_helvR12_tf, LCD_W / 2, 344, COL_DIM, 1);
  }
  if (toast[0]) drawToastBand(370, 410, toast);
  flushBand(0, LCD_H);
}

static void renderOta(bool full) {
  int p = gOtaPct.load();
  if (!bandDirty(0, hmix(51, p + 2)) && !full) return;
  gCanvas->fillScreen(COL_BLACK);
  drawText("Aggiornamento firmware", u8g2_font_helvB18_tf, LCD_W / 2, 180, COL_WHITE, 1);
  gCanvas->drawRoundRect(64, 214, 240, 22, 11, COL_WHITE);
  int fw = p < 0 ? 0 : 236 * (p > 100 ? 100 : p) / 100;
  if (fw > 0) gCanvas->fillRoundRect(66, 216, fw, 18, 9, COL_EYE);
  char s[32];
  if (p >= 0) snprintf(s, sizeof(s), "%d%%", p);
  else strcpy(s, "In attesa...");
  drawText(s, u8g2_font_helvR14_tf, LCD_W / 2, 270, COL_WHITE, 1);
  drawText("Non spegnere Jarvis", u8g2_font_helvR12_tf, LCD_W / 2, 310, COL_DIM, 1);
  flushBand(0, LCD_H);
}

// --- Task ui (core 1, prio 1) ------------------------------------------------------------------------------------------
static void uiTask(void *) {
  int lastScreen = -1;
  uint32_t lastBatt = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    if (gUiSleepReq.load()) {
      if (!gUiSleepAck.load()) {
        gPanel->setBrightness(0);
        gPanel->displayOff();
        gUiSleepAck.store(true);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    uint32_t now = millis();
    if (now - lastBatt > 5000 || lastBatt == 0) {
      gBatt = axpRead();
      gBattPct.store(gBatt.present ? gBatt.pct : -1);
      lastBatt = now;
    }
    int scr = gScreen.load();
    bool full = scr != lastScreen;
    if (full) {  // le funzioni di disegno coprono tutte le bande dello schermo
      bandsInvalidate();
      gCanvas->fillScreen(COL_BLACK);
      lastScreen = scr;
    }
    uint32_t period = 100;
    switch (scr) {
      case SCR_ASSIST: renderAssist(now, full); period = 33; break;
      case SCR_CLOCK: renderClock(now, full); period = 100; break;
      case SCR_RADIO: renderRadio(now, full); period = 50; break;
      case SCR_SETUP: renderSetup(full); period = 200; break;
      case SCR_OTA: renderOta(full); period = 200; break;
      default: renderMessage(full); period = 100; break;
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(period));
    if ((int32_t)(xTaskGetTickCount() - wake) > 0) wake = xTaskGetTickCount();  // evita rincorse
  }
}

static bool uiInit() {
  uiLock = xSemaphoreCreateRecursiveMutex();
  gSL = (SubLines *)psAlloc(sizeof(SubLines));
  gSubText = (char *)psAlloc(SUB_MAX);
  gSubCopy = (char *)psAlloc(SUB_MAX);
  if (!uiLock || !gSL || !gSubText || !gSubCopy) return false;
  memset(gSL, 0, sizeof(SubLines));
  gSubText[0] = gSubCopy[0] = gToast[0] = gMsgTitle[0] = gMsg1[0] = gMsg2[0] = gStation[0] = 0;
  gCanvas->setUTF8Print(true);
  gCanvas->setTextWrap(false);
  bandsInvalidate();
  return true;
}
