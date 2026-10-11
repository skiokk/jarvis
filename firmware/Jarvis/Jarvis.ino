// Jarvis - assistente vocale da scrivania per Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1 e V2).
// Documentazione: docs/SPECIFICA.md (progetto) e firmware/COMPILAZIONE.md (compilazione).
//
// Scheda: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, Partition Scheme "Custom" (usa il partitions.csv
// dello sketch: due slot app da 6 MB per l'OTA), USB CDC On Boot Enabled.
//
// Jarvis.ino: setup/loop, task comandi (touch/PWR/BOOT), flusso conversazione,
// radio, meteo, standby, applicazione delle impostazioni.

#include <Arduino.h>
#include <U8g2lib.h>  // prima di Arduino_GFX: abilita i font U8g2
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_system.h>
#include "util.h"
#include "settings.h"
#include "board.h"
#include "audio.h"
#include "ui.h"
#include "ai.h"
#include "radio.h"
#include "web.h"

SET_LOOP_TASK_STACK_SIZE(20 * 1024);  // TLS (mbedTLS) + JSON + decodifica MP3 nel loop

enum AppMode : int { MODE_CLOCK = 0, MODE_ASSIST = 1 };
static int gMode = MODE_ASSIST;
static uint32_t gNextWeather = 0;
static bool gWeatherOk = false;
static String gGeoFailCity;
static uint32_t gSubClearAt = 0;
static uint32_t gEyesIdleAt = 0;
static uint32_t gSetupStart = 0;
static uint32_t gNextStats = 0;
static bool gNtpStarted = false;
static TaskHandle_t hFeed = nullptr, hFetch = nullptr, hPlay = nullptr, hUi = nullptr, hInput = nullptr;
static std::atomic<bool> gTpIrq{false};
static std::atomic<bool> gReady{false};  // fine di setup(): prima i gesti non generano eventi

// ===================================================================================================
// Task comandi (core 1, prio 3): touch, PWR (espansore EXIO4), BOOT (GPIO0)
// ===================================================================================================
static void IRAM_ATTR tpIsr() { gTpIrq.store(true); }

// Durante l'avvio (fino a 20 s di connessione Wi-Fi) i tocchi verrebbero eseguiti tutti insieme
// a setup() finito: cambio modalità salvato, standby, ascolto. Passa solo BOOT 5 s (configurazione).
static void postInput(AppEvent e) {
  if (gReady.load() || e == EV_BOOT_LONG) postEvent(e);
}

// Interruzione dall'utente (ascolto, pensiero, voce, radio): riscontro immediato anche quando
// il loop è fermo in una richiesta HTTP che si chiuderà solo al prossimo controllo.
static void userInterrupt() {
  gInterrupt.store(true);
  const int e = gEyes.load();
  if (e == EYES_LISTEN || e == EYES_THINK) uiSetEyes(EYES_IDLE);
}

// BOOT 5 s ha effetto: sempre, tranne nella configurazione già aperta (senza password)
static bool bootLongAllowed() { return !gSetupMode || !gSetupOpen; }

static void inputTask(void *) {
  bool touchDown = false, longFired = false, touchArmed = false;  // dito già appoggiato all'avvio: ignorato
  uint32_t touchStart = 0, lastTouchOk = 0, lastPoll = 0;
  const uint32_t taskStart = millis();
  bool pwrDown = false, pwrArmed = false;  // armato solo dopo aver visto il tasto rilasciato
  uint32_t pwrStart = 0, lastPwrPoll = 0;
  bool bootDown = false, bootLong = false, bootArmed = false;
  uint32_t bootStart = 0;
  for (;;) {
    const uint32_t now = millis();
    // --- touch: lettura I2C su interrupt, mentre il dito è appoggiato, e comunque ogni 250 ms
    if (gTpIrq.exchange(false) || touchDown || now - lastPoll > 250) {
      lastPoll = now;
      int f = touchFingers();
      if (f == 0 || now - taskStart > 3000) touchArmed = true;
      if (f > 0 && !touchArmed) f = 0;
      if (f > 0) {
        lastTouchOk = now;
        if (!touchDown) { touchDown = true; touchStart = now; longFired = false; }
        else if (!longFired && now - touchStart >= 1000) {
          longFired = true;
          if (!gAppBusy.load()) postInput(EV_LONG);
        }
      } else if (touchDown && (f == 0 || now - lastTouchOk > 400)) {  // rilascio (o chip in sleep)
        touchDown = false;
        if (!longFired && now - touchStart < 1000) {
          if (gAppBusy.load()) userInterrupt();  // interrompe ascolto/risposta/radio
          else postInput(EV_TAP);
        }
      }
    }
    // --- PWR (ALTO = premuto). Tenuto 6 s spegne l'AXP2101 da solo.
    if (now - lastPwrPoll >= 30) {
      lastPwrPoll = now;
      int in = tcaReadInputs();
      if (in >= 0) {
        bool p = in & (1 << 4);
        if (!p) pwrArmed = true;
        if (p && !pwrDown && pwrArmed) { pwrDown = true; pwrStart = now; }
        else if (!p && pwrDown) {
          pwrDown = false;
          uint32_t d = now - pwrStart;
          if (d >= 40 && d < 2000) {
            if (gAppBusy.load()) userInterrupt();  // PWR ferma la radio / la risposta
            else postInput(EV_PWR);
          }
        }
      }
    }
    // --- BOOT (BASSO = premuto)
    bool b = digitalRead(PIN_BOOT) == LOW;
    if (!b) bootArmed = true;
    if (b && !bootDown && bootArmed) { bootDown = true; bootStart = now; bootLong = false; }
    else if (b && bootDown && !bootLong && now - bootStart >= 5000) {
      bootLong = true;
      if (bootLongAllowed()) {
        // ferma subito radio/risposta: il loop sarà libero al rilascio e l'avviso si vede
        if (gAppBusy.load()) userInterrupt();
        uiToast("Rilascia BOOT: configurazione", 6000);
      }
    } else if (!b && bootDown) {
      bootDown = false;
      uint32_t d = now - bootStart;
      if (bootLong) {  // riavvio solo dopo il rilascio (GPIO0 basso = download mode)
        if (bootLongAllowed()) postInput(EV_BOOT_LONG);
      } else if (d >= 40 && d < 1500) {
        if (gAppBusy.load()) userInterrupt();
        postInput(EV_BOOT);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ===================================================================================================
// Impostazioni, riavvio, OTA, statistiche (chiamata dal loop e durante la radio)
// ===================================================================================================
static void applyPendingCfg() {
  Settings old, neu;
  bool restart;
  {
    LockGuard g(cfgLock, 50);
    if (!g.ok || !pendingCfgReady) return;
    old = cfg;
    cfg = pendingCfg;
    neu = cfg;
    pendingCfgReady = false;
    restart = pendingCfgRestart;
    pendingCfgRestart = false;
  }
  settingsSave(neu, &old);
  LOGI("Impostazioni applicate e salvate%s", restart ? ": riavvio" : "");
  if (neu.volume != old.volume) audioSetVolume(neu.volume);
  if (neu.micGain != old.micGain) codecSetMicGain(neu.micGain);
  if (neu.chgMa != old.chgMa && neu.chgMa > 0 && gAxpOk) {
    uint8_t n = neu.chgMa <= 200 ? (uint8_t)(neu.chgMa / 25) : (uint8_t)(8 + (neu.chgMa - 200) / 100);
    i2cUpdateReg(ADDR_AXP2101, 0x62, 0x1F, n & 0x1F);
  }
  if (neu.tz != old.tz) {
    LockGuard g(uiLock, 200);
    setenv("TZ", neu.tz.c_str(), 1);
    tzset();
  }
  // subito: "adesso" (non 0, che dopo 24,8 giorni di accensione risulterebbe nel futuro)
  if (neu.city != old.city || neu.lat != old.lat || neu.lon != old.lon) { gNextWeather = millis(); gGeoFailCity = ""; }
  if (neu.groqKey != old.groqKey || neu.prompt != old.prompt) historyReset();
  if (restart) {
    uiMessage("Riavvio", "Applico le nuove impostazioni...", "");
    uiSetScreen(SCR_MESSAGE);
    gRestartAt.store(millis() + 1500);
  } else if (!gSetupMode) {
    uiToast("Impostazioni salvate", 3000);
  }
}

static void doRestart() {
  LOGI("Riavvio...");
  playStop();
  paSet(false);
  markOtaValid();  // la versione in uso funziona: niente rollback al prossimo avvio
  Serial.flush();
  delay(100);
  ESP.restart();
}

static void logStats() {
  LOGI("Stato: RAM interna %u (min %u, blocco %u), PSRAM %u, RSSI %d, underrun %u",
       (unsigned)freeInternal(), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)largestInternal(),
       (unsigned)freePsram(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, (unsigned)gUnderruns.load());
  auto hwm = [](TaskHandle_t h) { return h ? (unsigned)uxTaskGetStackHighWaterMark(h) : 0u; };
  LOGI("Stack liberi (byte): loop %u, feed %u, fetch %u, play %u, ui %u, input %u, web %u", (unsigned)uxTaskGetStackHighWaterMark(nullptr),
       hwm(hFeed), hwm(hFetch), hwm(hPlay), hwm(hUi), hwm(hInput), hwm(gWebTask));
}

// Lavori leggeri: anche dentro la radio (niente rete qui)
static void loopServiceLight() {
  applyPendingCfg();
  uint32_t ra = gRestartAt.load();
  if (ra && (int32_t)(millis() - ra) >= 0) doRestart();
  otaWatchdog();
  if (gOtaActive.load()) {
    gInterrupt.store(true);  // ferma radio/conversazione durante l'aggiornamento
    if (gScreen.load() != SCR_OTA) uiSetScreen(SCR_OTA);
  }
  if ((int32_t)(millis() - gNextStats) >= 0) {
    gNextStats = millis() + 60000;
    logStats();
  }
}

// ===================================================================================================
// Meteo e geocoding (ogni 15 min; ogni minuto finché non arriva il primo)
// ===================================================================================================
static void weatherService() {
  if (gSetupMode || WiFi.status() != WL_CONNECTED || (int32_t)(millis() - gNextWeather) < 0) return;
  gNextWeather = millis() + (gWeatherOk ? 15UL * 60 * 1000 : 60UL * 1000);
  if (cfg.city != cfg.geoCity && cfg.city != gGeoFailCity) {
    float lat = cfg.lat, lon = cfg.lon;
    String name, err;
    bool found = false;
    if (geocode(cfg.city, lat, lon, name, found, err)) {
      if (found) {
        Settings old = cfg;
        {
          LockGuard g(cfgLock, 1000);
          if (g.ok) { cfg.lat = lat; cfg.lon = lon; cfg.geoCity = cfg.city; }
        }
        settingsSave(cfg, &old);
        LOGI("Geocoding: %s -> %s (%.4f, %.4f)", cfg.city.c_str(), name.c_str(), lat, lon);
      } else {
        gGeoFailCity = cfg.city;
        LOGW("Geocoding: città '%s' non trovata, uso le coordinate precedenti", cfg.city.c_str());
        uiToast("Città non trovata: " + cfg.city, 6000);
      }
    } else {
      LOGW("%s", err.c_str());
    }
  }
  Weather w;
  String err;
  if (weatherFetch(cfg.lat, cfg.lon, cfg.city, w, err)) {
    uiSetWeather(w);
    if (!gWeatherOk) gNextWeather = millis() + 15UL * 60 * 1000;
    gWeatherOk = true;
    LOGI("Meteo: %.1f °C, codice %d, umidità %d%%", w.temp, w.code, w.hum);
  } else {
    LOGW("%s", err.c_str());
  }
}

// ===================================================================================================
// Conversazione
// ===================================================================================================
static void showError(const String &msg) {
  LOGE("%s", msg.c_str());
  setLastError(msg);
  uiSetEyes(EYES_ERROR);
  uiSubtitle(msg, SUB_ALL, 0, 0);
  gSubClearAt = millis() + 12000;
  gEyesIdleAt = millis() + 4000;
}

static void armWake() {
  bool on = gAfeOk && !gSetupMode && gMode == MODE_ASSIST && gMicEnabled.load();
  afeSetMode(on ? AFE_WAKE : AFE_IDLE);
}

static String chatContext() {
  String c = "Contesto: ";
  struct tm t;
  if (localTime(t)) {
    char b[96];
    snprintf(b, sizeof(b), "oggi è %s %d %s %d, ore %02d:%02d. ", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min);
    c += b;
  }
  c += "Città: " + cfg.city + ". ";
  Weather w;
  {
    LockGuard g(uiLock, 100);
    if (g.ok) w = gWeather;
  }
  if (w.ok) {
    char b[96];
    snprintf(b, sizeof(b), "Meteo attuale: %d gradi, %s, umidità %d%%.", (int)lroundf(w.temp), wmoText(w.code), w.hum);
    c += b;
  }
  return c;
}

// Pronuncia un testo con sottotitoli sincronizzati; se la voce non è disponibile mostra solo il testo.
static bool speak(const String &raw) {
  String text = cleanText(raw);
  if (text.length() == 0) return true;
  playStop();
  setAudioRate(SR_RATE);
  uiSetEyes(EYES_SPEAK);
  uiSubtitle(text, SUB_SYNC, gPlayedFrames.load(), SR_RATE);  // la coda è vuota: il primo frame è la voce
  uint32_t startFrames = 0, totalFrames = 0;
  String err;
  bool ok = azureSpeak(cfg, text, startFrames, totalFrames, err);
  // durata reale della voce nota: i sottotitoli seguono la voce invece della stima a 14 caratteri/s
  if (ok && totalFrames) uiSubtitleSync(startFrames, totalFrames);
  if (ok) ok = playWait(120000);
  if (!ok && !gInterrupt.load()) {
    if (err.length()) {
      LOGW("Voce non disponibile: %s", err.c_str());
      setLastError("Voce: " + err);
    }
    // solo testo: lettura a 14 caratteri/s
    uiSubtitle(text, SUB_TIME, 0, 0);
    uint32_t t0 = millis(), dur = 1500 + utf8Len(text.c_str()) * 1000 / 14;
    while (millis() - t0 < dur && !gInterrupt.load()) vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (gInterrupt.load()) playStop();
  uiSubtitleMode(SUB_ALL);
  return !gInterrupt.load();
}

static void radioFlow(const String &query);

// Su OGNI uscita riarma la wake word: dopo un «Jarvis» il task fetch l'ha spenta e aspetta il loop.
static void converse() {
  gSubClearAt = 0;  // un timer di una risposta precedente cancellerebbe il testo nuovo
  gMode = MODE_ASSIST;  // anche da orologio (tocco lungo): l'eventuale errore deve essere visibile
  uiSetScreen(SCR_ASSIST);
  if (!gAfeOk || !gRecBuf) { showError("Microfono non disponibile (vedi log)"); armWake(); return; }
  if (cfg.groqKey.length() == 0) { showError("Manca la chiave Groq: apri " + gHostIp); armWake(); return; }
  if (WiFi.status() != WL_CONNECTED) { showError("Wi-Fi non connesso"); armWake(); return; }
  gAppBusy.store(true);
  gInterrupt.store(false);
  uiSubtitleClear();
  uiSetEyes(EYES_LISTEN);
  afeSetMode(AFE_IDLE);
  playStop();
  setAudioRate(SR_RATE);
  playBeep(true);
  playWait(1500);
  String question, answer, err;
  if (gInterrupt.load()) goto done;
  // --- registrazione (VAD nel task fetch)
  gRecState.store(REC_NONE);
  afeSetMode(AFE_RECORD);
  {
    uint32_t t0 = millis();
    int st;
    for (;;) {
      st = gRecState.load();
      if (st != REC_NONE && st != REC_RUNNING) break;
      if (gInterrupt.load() || millis() - t0 > 14000) { afeSetMode(AFE_IDLE); st = REC_ABORTED; break; }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (st == REC_ABORTED) goto done;
    if (st == REC_NOSPEECH) {
      uiSetEyes(EYES_IDLE);
      uiToast("Non ho sentito nulla", 3000);
      goto done;
    }
  }
  playBeep(false);
  // --- trascrizione
  uiSetEyes(EYES_THINK);
  if (!groqTranscribe(gRecPcmBytes.load(), cfg, question, err)) {
    if (!gInterrupt.load()) showError("Non ho capito: " + err);  // interrotta dall'utente: niente errore
    goto done;
  }
  LOGI("Domanda: \"%s\"", question.c_str());
  if (question.length() < 2 || whisperHallucination(question)) {
    uiSetEyes(EYES_IDLE);
    uiToast("Non ho capito, riprova", 3000);
    goto done;
  }
  if (gInterrupt.load()) goto done;
  uiSubtitle(cleanText(question), SUB_ALL, 0, 0);
  {
    String q;
    if (radioCommand(question, cfg.radioFav, q)) {
      radioFlow(q);
      goto done;
    }
  }
  // --- risposta
  if (!groqChat(cfg, question, chatContext(), answer, err)) {
    if (!gInterrupt.load()) showError("Nessuna risposta: " + err);
    goto done;
  }
  LOGI("Risposta: \"%s\"", answer.c_str());
  if (gInterrupt.load()) goto done;
  speak(answer);
  gSubClearAt = millis() + 20000;
done:
  playStop();
  // ogni testo rimasto (radio non trovata, domanda interrotta...) sparisce, e torna il suggerimento
  if (!gSubClearAt) gSubClearAt = millis() + 15000;
  if (gEyes.load() != EYES_ERROR) uiSetEyes(EYES_IDLE);
  gInterrupt.store(false);
  gAppBusy.store(false);
  armWake();
}

// ===================================================================================================
// Radio
// ===================================================================================================
static void radioFlow(const String &query) {
  LOGI("Radio richiesta: \"%s\"", query.c_str());
  uiSetEyes(EYES_THINK);
  uiSubtitle("Cerco " + query + "...", SUB_ALL, 0, 0);
  Station st;
  String err;
  if (!radioFind(query, st, err)) {
    LOGW("Radio: %s", err.c_str());
    if (gInterrupt.load()) return;  // ricerca interrotta dall'utente
    speak("Non trovo " + query + " in un formato che posso riprodurre.");
    return;
  }
  LOGI("Radio scelta: %s [%s, %d kbps] %s", st.name.c_str(), st.country.c_str(), st.bitrate, st.url.c_str());
  if (gInterrupt.load()) return;
  if (!speak("Ecco " + st.name)) return;
  // durante la radio il microfono è spento (stessa LRCK per ADC e DAC)
  afeSetMode(AFE_IDLE);
  gMicEnabled.store(false);
  uiSubtitleClear();
  uiSetStation(st.name);
  uiSetScreen(SCR_RADIO);
  bool played = radioPlay(st, err);
  if (gInterrupt.load()) playStop();
  else playWait(4000);
  playStop();
  setAudioRate(SR_RATE);
  gMicEnabled.store(true);
  uiSetScreen(SCR_ASSIST);
  if (gOtaActive.load()) return;
  if (gInterrupt.load()) return;  // fermata dall'utente
  if (!played) showError("Radio non disponibile: " + err);
  else if (err.length()) {
    LOGW("Radio terminata: %s", err.c_str());
    uiToast("Radio interrotta: " + err, 5000);
  }
}

// ===================================================================================================
// Standby (deep sleep): risveglio con tocco (TP_INT basso) o BOOT. PWR non può risvegliare.
// ===================================================================================================
static void enterStandby() {
  LOGI("Standby");
  markOtaValid();  // altrimenti al risveglio il bootloader tornerebbe alla versione precedente
  afeSetMode(AFE_IDLE);
  playStop();
  gMicEnabled.store(false);
  uiMessage("Standby", "Tocca lo schermo o premi BOOT", "per riaccendere");
  uiSetScreen(SCR_MESSAGE);
  vTaskDelay(pdMS_TO_TICKS(1800));
  // attende che il dito si stacchi e che BOOT sia rilasciato
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    if (touchFingers() <= 0 && digitalRead(PIN_BOOT) == HIGH) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  paSet(false);
  codecSuspend();
  uiSleep();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  uint64_t mask = 1ULL << PIN_BOOT;
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  t0 = millis();
  while (digitalRead(PIN_TP_INT) == LOW && millis() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(20));
  if (digitalRead(PIN_TP_INT) == HIGH) {
    mask |= 1ULL << PIN_TP_INT;
    rtc_gpio_pullup_en(GPIO_NUM_21);
    rtc_gpio_pulldown_dis(GPIO_NUM_21);
  } else {
    LOGW("TP_INT resta basso: risveglio solo con BOOT");
  }
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  LOGI("Deep sleep (maschera risveglio 0x%llx)", (unsigned long long)mask);
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

// ===================================================================================================
// Eventi
// ===================================================================================================
static void setMode(int m) {
  gMode = m;
  uiSetScreen(m == MODE_ASSIST ? SCR_ASSIST : SCR_CLOCK);
  if (m == MODE_ASSIST) uiSetEyes(EYES_IDLE);
  armWake();
  if (cfg.startMode != m) {  // ricordata per il prossimo avvio
    Settings old = cfg;
    {
      LockGuard g(cfgLock, 500);
      if (g.ok) cfg.startMode = m;
    }
    settingsSave(cfg, &old);
  }
}

// BOOT 5 s: riavvio nella configurazione aperta (presenza fisica: niente password)
static void restartIntoSetup() {
  Settings old = cfg;
  {
    LockGuard g(cfgLock, 500);
    if (g.ok) cfg.forceSetup = true;
  }
  settingsSave(cfg, &old);
  uiMessage("Configurazione", "Riavvio in modalità configurazione", "");
  uiSetScreen(SCR_MESSAGE);
  gRestartAt.store(millis() + 1200);
}

static void handleEvent(uint8_t ev) {
  if (gOtaActive.load()) {
    if (ev == EV_WAKE) armWake();  // il task fetch ha spento la wake word e aspetta il riarmo
    return;
  }
  if (gSetupMode) {
    if (ev == EV_BOOT_LONG && !gSetupOpen) restartIntoSetup();  // configurazione protetta: BOOT 5 s la apre
    else if (ev == EV_BOOT_LONG || ev == EV_PWR) LOGI("In configurazione: comando ignorato");
    return;
  }
  switch (ev) {
    case EV_TAP:
      setMode(gMode == MODE_ASSIST ? MODE_CLOCK : MODE_ASSIST);
      break;
    case EV_LONG:
      converse();
      break;
    case EV_WAKE:
      if (gMode == MODE_ASSIST) converse();
      else armWake();
      break;
    case EV_PWR:
      enterStandby();
      break;
    case EV_BOOT:
      historyReset();
      uiToast("Nuova conversazione", 2500);
      break;
    case EV_BOOT_LONG:
      restartIntoSetup();
      break;
    default: break;
  }
}

// ===================================================================================================
// Avvio
// ===================================================================================================
static void fatalScreen(const char *l1, const char *l2) {
  LOGE("%s %s", l1, l2);
  if (!gPanel) return;
  gPanel->fillScreen(0x0000);
  gPanel->setFont(u8g2_font_helvB18_tf);
  gPanel->setUTF8Print(true);
  gPanel->setTextColor(COL_RED);
  gPanel->setCursor(30, 200);
  gPanel->print(l1);
  gPanel->setFont(u8g2_font_helvR14_tf);
  gPanel->setTextColor(COL_WHITE);
  gPanel->setCursor(30, 240);
  gPanel->print(l2);
}

// Avvio impossibile (display/PSRAM/UI). Se è un aggiornamento OTA non ancora confermato il bootloader
// tornerebbe al firmware precedente solo con un riavvio: lo chiede esplicitamente, dopo ~5 s di messaggio.
// Con un firmware già confermato resta fermo con il messaggio (niente cicli di riavvio a batteria).
static void fatalHalt(const char *why) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  for (int i = 0;; i++) {
    LOGE("Avvio interrotto: %s", why);
    if (pending && i >= 1) {
      LOGE("Nuovo firmware non avviabile: ritorno alla versione precedente");
      Serial.flush();
      esp_ota_mark_app_invalid_rollback_and_reboot();  // ritorna solo se non c'è una versione valida
      LOGE("Rollback non possibile");
      pending = false;
    }
    delay(5000);
  }
}

static bool connectWifi() {
  if (cfg.wifiSsid.length() == 0) return false;
  uiMessage("Connessione al Wi-Fi", cfg.wifiSsid, "");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("jarvis");
  WiFi.setSleep(false);  // niente risparmio energetico: latenza bassa per voce e radio
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(100));
  if (WiFi.status() != WL_CONNECTED) {
    LOGW("Wi-Fi '%s' non raggiungibile entro 20 s (stato %d)", cfg.wifiSsid.c_str(), (int)WiFi.status());
    return false;
  }
  LOGI("Wi-Fi connesso a '%s': IP %s, RSSI %d dBm", cfg.wifiSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(2);  // senza terminale aperto i log non bloccano i task
#endif
  delay(200);
  esp_log_level_set("i2c.master", ESP_LOG_NONE);  // NACK attesi: CST820 in auto-sleep, sonde di revisione
  LOGI("==== " FW_NAME " " FW_VERSION " ====");
  LOGI("Reset: %d, risveglio: %d, CPU %u MHz, flash %u MB", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
       (unsigned)getCpuFrequencyMhz(), (unsigned)(ESP.getFlashChipSize() >> 20));
  LOGI("PSRAM %u byte (libera %u), RAM interna libera %u", (unsigned)ESP.getPsramSize(), (unsigned)freePsram(), (unsigned)freeInternal());

  cfgLock = xSemaphoreCreateRecursiveMutex();
  i2cLock = xSemaphoreCreateRecursiveMutex();
  gEvtQueue = xQueueCreate(16, sizeof(uint8_t));
  settingsLoad(cfg);
  LOGI("Impostazioni: Wi-Fi '%s', chiavi Groq %s / Azure %s, modelli %s | %s", cfg.wifiSsid.c_str(), cfg.groqKey.length() ? "sì" : "no",
       cfg.azureKey.length() ? "sì" : "no", cfg.chatModel.c_str(), cfg.fbModel.c_str());

  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  pinMode(PIN_PA, OUTPUT);
  paSet(false);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.setTimeOut(50);
  if (!tcaInit()) LOGE("TCA9554 non risponde su 0x20");
  delay(150);  // come board_variant.c: attesa prima di sondare il touch
  int rev = boardDetect(cfg.boardRev);
  LOGI("Scheda V%d: %s%s", rev, rev == 2 ? "CO5300 + CST820" : "SH8601 + FT3168", cfg.boardRev ? " (forzata da impostazioni)" : "");
  touchConfigure();
  axpInit(cfg.chgMa);

  if (!displayCreate(rev)) {
    fatalScreen(psramFound() ? "Display non avviato" : "PSRAM non trovata", "Arduino IDE: PSRAM = OPI PSRAM");
    fatalHalt("display/PSRAM");
  }
  if (!uiInit()) {
    fatalScreen("Memoria insufficiente", "UI non avviata");
    fatalHalt("UI");
  }
  uiMessage("Avvio...", FW_NAME " " FW_VERSION, rev == 2 ? "Scheda V2" : "Scheda V1");
  xTaskCreatePinnedToCore(uiTask, "ui", 10240, nullptr, 1, &hUi, 1);

  // --- audio e riconoscimento vocale
  bool audioOk = audioInit(cfg);
  if (audioOk) xTaskCreatePinnedToCore(playTask, "play", 4096, nullptr, 6, &hPlay, 1);
  uiMessage("Avvio...", "Preparo il riconoscimento vocale", "");
  gAfeOk = audioOk && gRecBuf && ensureModel() && afeInit(cfg.wakeSens);
  if (gAfeOk) {
    xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 5, &hFetch, 0);
    xTaskCreatePinnedToCore(feedTask, "feed", 6144, nullptr, 5, &hFeed, 0);
  } else {
    LOGE("Wake word non disponibile");
    setLastError("Riconoscimento vocale non avviato");
  }
  attachInterrupt(PIN_TP_INT, tpIsr, FALLING);
  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 3, &hInput, 1);

  // --- rete
  bool force = cfg.forceSetup;
  if (force) {
    Settings old = cfg;
    cfg.forceSetup = false;  // una sola volta
    settingsSave(cfg, &old);
  }
  if (!force && connectWifi()) {
    gSetupMode = false;
    gHostIp = WiFi.localIP().toString();
    startMdns();
    configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");
    gNtpStarted = true;
  } else {
    gSetupMode = true;
    // aperta solo se voluta (BOOT 5 s) o al primo avvio; se il Wi-Fi salvato non risponde resta protetta
    gSetupOpen = force || cfg.wifiSsid.length() == 0;
    gSetupNeedsPass = !gSetupOpen && cfg.webPass.length() > 0;
    LOGI("Configurazione %s", gSetupOpen || cfg.webPass.length() == 0 ? "aperta" : "protetta dalla password della pagina");
    gSetupStart = millis();
    startSetupAP();
    setenv("TZ", cfg.tz.c_str(), 1);
    tzset();
  }
  webStart();
  markOtaValid();

  if (gSetupMode) {
    uiSetScreen(SCR_SETUP);
  } else {
    gMode = cfg.startMode;
    uiSetScreen(gMode == MODE_ASSIST ? SCR_ASSIST : SCR_CLOCK);
    uiSetEyes(EYES_IDLE);
    armWake();
    // l'avviso è largo al massimo 300 px (helvR14): l'indirizzo deve restare intero
    if (cfg.groqKey.length() == 0 || cfg.azureKey.length() == 0) uiToast("Configura: http://" + gHostIp, 15000);
    else uiToast("Pagina: http://" + gHostIp, 10000);
  }
  gNextWeather = millis() + 2000;
  gNextStats = millis() + 15000;
  gReady.store(true);
  LOGI("Avvio completato in %u ms", (unsigned)millis());
  logStats();
}

void loop() {
  loopServiceLight();
  uint8_t ev;
  if (xQueueReceive(gEvtQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) handleEvent(ev);
  const uint32_t now = millis();
  if (gSetupMode) {
    if (!gOtaActive.load() && gScreen.load() == SCR_OTA) uiSetScreen(SCR_SETUP);  // OTA fallito
    // senza salvataggi per 5 minuti (e nessuno sulla pagina): riavvio per ritentare il Wi-Fi
    uint32_t act = gWebActivity.load();
    uint32_t last = act > gSetupStart ? act : gSetupStart;
    if (cfg.wifiSsid.length() && now - last > 5UL * 60 * 1000 && WiFi.softAPgetStationNum() == 0) {
      LOGI("Setup: 5 minuti senza salvataggi, riavvio per ritentare il Wi-Fi");
      doRestart();
    }
    return;
  }
  if (gOtaActive.load()) return;
  if (gSubClearAt && (int32_t)(now - gSubClearAt) >= 0) { gSubClearAt = 0; uiSubtitleClear(); }
  if (gEyesIdleAt && (int32_t)(now - gEyesIdleAt) >= 0) {
    gEyesIdleAt = 0;
    if (gEyes.load() == EYES_ERROR) uiSetEyes(EYES_IDLE);
  }
  if (gScreen.load() == SCR_OTA) {  // OTA fallito
    uiSetScreen(gMode == MODE_ASSIST ? SCR_ASSIST : SCR_CLOCK);
    armWake();  // un «Jarvis» arrivato durante l'OTA aveva spento la wake word
  }
  weatherService();
}
