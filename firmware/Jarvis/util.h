#pragma once
// util.h - strumenti comuni a più moduli: versione, log, PSRAM, lock RAII, testo.

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

#define FW_NAME "Jarvis"
#define FW_VERSION "1.0.0"
#define USER_AGENT FW_NAME "/" FW_VERSION " (ESP32-S3)"

// ---------------------------------------------------------------------------
// Log su USB CDC. Una sola printf per riga: le righe di task diversi non si mescolano.
#define LOGI(fmt, ...) Serial.printf("[%7lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
#define LOGW(fmt, ...) Serial.printf("[%7lu] ATTENZIONE: " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
#define LOGE(fmt, ...) Serial.printf("[%7lu] ERRORE: " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

static inline uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static inline uint32_t freePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static inline uint32_t largestInternal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

// Allocazione in PSRAM (nullptr se manca): il chiamante controlla SEMPRE il risultato.
static inline void *psAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static inline void *inAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

// Allocatore ArduinoJson su PSRAM (i documenti grandi non consumano RAM interna).
struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static SpiRamAllocator gJsonAlloc;

// ---------------------------------------------------------------------------
// Mutex ricorsivo con guardia RAII: rilascio garantito su ogni percorso.
struct LockGuard {
  SemaphoreHandle_t m;
  bool ok;
  LockGuard(SemaphoreHandle_t mtx, uint32_t ms) : m(mtx), ok(false) {
    ok = (m != nullptr) && xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(ms)) == pdTRUE;
  }
  ~LockGuard() {
    if (ok) xSemaphoreGiveRecursive(m);
  }
  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;
};

// ---------------------------------------------------------------------------
// Eventi verso il loop (coda FreeRTOS)
enum AppEvent : uint8_t {
  EV_NONE = 0,
  EV_TAP,         // tocco breve
  EV_LONG,        // tocco lungo (1 s)
  EV_PWR,         // PWR breve
  EV_BOOT,        // BOOT breve
  EV_BOOT_LONG,   // BOOT tenuto 5 s (rilasciato)
  EV_WAKE,        // «Jarvis» riconosciuto
};
static QueueHandle_t gEvtQueue = nullptr;
// false se la coda è piena: chi pubblica eventi "con stato" (EV_WAKE) deve gestirlo
static inline bool postEvent(AppEvent e) {
  if (!gEvtQueue) return false;
  uint8_t v = (uint8_t)e;
  return xQueueSend(gEvtQueue, &v, 0) == pdTRUE;
}

// Richiesta di interruzione (tocco/PWR durante ascolto, risposta o radio)
static std::atomic<bool> gInterrupt{false};
// Il loop è occupato (ascolto/pensiero/voce/radio): il tocco interrompe invece di cambiare modalità
static std::atomic<bool> gAppBusy{false};
// Richiesta di riavvio (dal web: salvataggio o OTA) con istante di esecuzione
static std::atomic<uint32_t> gRestartAt{0};

// ---------------------------------------------------------------------------
// Testo

// Copia limitata con terminatore garantito. Se il testo viene troncato, il taglio non lascia
// in fondo una sequenza UTF-8 incompleta (Arduino_GFX conserva lo stato del decoder UTF-8 fra
// una stringa e l'altra: un byte iniziale isolato rovinerebbe il testo disegnato dopo).
static inline void safeCopy(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = 0; return; }
  if (strlcpy(dst, src, cap) < cap) return;
  size_t len = cap - 1, i = len;
  while (i > 0 && ((uint8_t)dst[i - 1] & 0xC0) == 0x80) i--;  // inizio dell'ultimo carattere
  if (i == 0) return;
  const uint8_t lead = (uint8_t)dst[i - 1];
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (len - (i - 1) < need) dst[i - 1] = 0;
}

// Lunghezza in caratteri di una stringa UTF-8
static inline size_t utf8Len(const char *s) {
  size_t n = 0;
  for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++;
  return n;
}

// Byte offset dell'n-esimo carattere UTF-8
static inline size_t utf8Offset(const char *s, size_t chars) {
  size_t i = 0, c = 0;
  while (s[i]) {
    if (((uint8_t)s[i] & 0xC0) != 0x80) {
      if (c == chars) return i;
      c++;
    }
    i++;
  }
  return i;
}

// Normalizza il testo per font Latin-1 e sintesi vocale:
// virgolette/apostrofi tipografici, trattini, puntini, NBSP, euro; toglie emoji e (per la voce) markdown.
// speech = false: solo visualizzazione (nomi di stazioni, SSID, città): * # _ ~ ` restano.
static String cleanText(const String &in, bool speech = true) {
  String out;
  out.reserve(in.length() + 8);
  const uint8_t *s = (const uint8_t *)in.c_str();
  size_t n = in.length();
  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = 0x10000; len = 4; }  // emoji e simili: scartati
    else { i++; continue; }  // byte non valido
    i += len;
    switch (cp) {
      case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += '\''; continue;
      case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += '"'; continue;
      case 0x2013: case 0x2014: case 0x2212: out += '-'; continue;
      case 0x2026: out += "..."; continue;
      case 0x00A0: case 0x2009: case 0x202F: case 0x200B: out += ' '; continue;
      case 0x20AC: out += "euro"; continue;
      case 0x2022: out += '-'; continue;
      case '*': case '#': case '`': case '_': case '~':  // markdown
        if (speech) continue;
        break;
      case '\r': continue;
      case '\n': case '\t': out += ' '; continue;
      default: break;
    }
    if (cp < 0x20) continue;
    if (cp >= 0x100) continue;  // fuori dal Latin-1 dei font: scartato
    if (cp < 0x80) out += (char)cp;
    else { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  }
  // spazi multipli -> uno
  String res;
  res.reserve(out.length());
  bool sp = true;
  for (size_t i = 0; i < out.length(); i++) {
    char ch = out[i];
    if (ch == ' ') { if (!sp) res += ' '; sp = true; }
    else { res += ch; sp = false; }
  }
  res.trim();
  return res;
}

// Escape HTML (pagina web) e XML (SSML)
static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += c;
    }
  }
  return o;
}
static String xmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&apos;"); break;
      default: o += c;
    }
  }
  return o;
}

// URL-encode (query string)
static String urlEncode(const String &s) {
  static const char hex[] = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// Minuscolo ASCII + lettere accentate Latin-1 in UTF-8 (À..Þ -> à..þ)
static String lowerIt(const String &s) {
  String o = s;
  for (size_t i = 0; i < o.length(); i++) {
    uint8_t c = (uint8_t)o[i];
    if (c >= 'A' && c <= 'Z') o.setCharAt(i, (char)(c + 32));
    else if (c == 0xC3 && i + 1 < o.length()) {
      uint8_t d = (uint8_t)o[i + 1];
      if (d >= 0x80 && d <= 0x9E && d != 0x97) o.setCharAt(i + 1, (char)(d + 0x20));
      i++;
    }
  }
  return o;
}

// Ultime 4 cifre di una chiave (per "salvata, finisce con XXXX")
static String keyTail(const String &k) {
  if (k.length() == 0) return String();
  if (k.length() <= 4) return String("****");
  return k.substring(k.length() - 4);
}
