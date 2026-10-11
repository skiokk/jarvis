#pragma once
// settings.h - impostazioni in NVS (namespace "jarvis").
// Un solo scrittore: il loop. La pagina web prepara pendingCfg (sotto cfgLock),
// il loop la applica e la salva. Preferences è usato solo dal loop (e in setup()).

#include <Preferences.h>
#include "util.h"

#define CFG_VERSION 1  // versione dello schema delle impostazioni (per eventuali migrazioni future)

// Modelli Groq disponibili nel piano gratuito a ottobre 2026 (vedi docs/SPECIFICA.md)
#define DEF_CHAT_MODEL "qwen/qwen3.8-27b"
#define DEF_FB_MODEL "openai/gpt-oss-120b"
#define DEF_FB2_MODEL "openai/gpt-oss-20b"
#define DEF_STT_MODEL "whisper-large-v3-turbo"
#define DEF_RADIO_FAV "Radio 105 Network"  // nome esatto su radio-browser: "Radio 105" sceglieva un sottocanale

static const char DEF_PROMPT[] =
  "Sei Jarvis, un piccolo robot assistente da scrivania a forma di capsula. "
  "Hai una personalità simpatica e un po' ironica. "
  "Rispondi sempre in italiano, in modo cordiale e diretto, con al massimo 3 frasi brevi. "
  "Non usare markdown, elenchi, emoji, simboli o URL: il testo verrà letto ad alta voce.";

struct Settings {
  String wifiSsid, wifiPass, webPass;
  String groqKey, chatModel, fbModel, sttModel;
  String azureKey, azureRegion, voice;
  String prompt;
  String city;
  float lat = 41.8919f, lon = 12.5113f;
  String geoCity;
  String tz;
  int volume = 70;    // 0-100
  int micGain = 4;    // 0-7 (ES8311, 6 dB/passo)
  int wakeSens = 0;   // 0 normale (DET_MODE_90), 1 più sensibile (DET_MODE_95)
  String radioFav;
  bool ampExt = false;
  int ampBclk = -1, ampLrc = -1, ampDin = -1;
  int startMode = 1;  // 0 orologio, 1 assistente
  bool forceSetup = false;
  int boardRev = 0;   // 0 automatico, 1 = V1 (SH8601/FT3168), 2 = V2 (CO5300/CST820)
  int chgMa = 200;    // corrente di ricarica AXP2101 in mA (0 = non modificare)
};

static Settings cfg;                 // scritta SOLO dal loop (lettura libera nel loop)
static Settings pendingCfg;          // preparata dal task web sotto cfgLock
static bool pendingCfgReady = false; // protetta da cfgLock
static bool pendingCfgRestart = false;
static SemaphoreHandle_t cfgLock = nullptr;

// GPIO ammessi per l'amplificatore esterno (connettore di espansione, passo 1,27 mm)
static const int AMP_PINS[] = {17, 18, 38, 39, 40, 41, 42};
static inline bool ampPinValid(int p) {
  for (int v : AMP_PINS) if (v == p) return true;
  return false;
}
static inline bool chgMaValid(int v) {
  return v == 0 || v == 100 || v == 125 || v == 150 || v == 175 || v == 200 || v == 300 || v == 400 || v == 500;
}

static void settingsDefaults(Settings &s) {
  s = Settings();
  s.chatModel = DEF_CHAT_MODEL;
  s.fbModel = DEF_FB_MODEL;
  s.sttModel = DEF_STT_MODEL;
  s.azureRegion = "westeurope";
  s.voice = "it-IT-ElsaNeural";
  s.prompt = DEF_PROMPT;
  s.city = "Roma";
  s.tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  s.radioFav = DEF_RADIO_FAV;
}

static void settingsClamp(Settings &s) {
  s.volume = constrain(s.volume, 0, 100);
  s.micGain = constrain(s.micGain, 0, 7);
  s.wakeSens = constrain(s.wakeSens, 0, 1);
  s.startMode = constrain(s.startMode, 0, 1);
  s.boardRev = constrain(s.boardRev, 0, 2);
  if (!chgMaValid(s.chgMa)) s.chgMa = 200;
  if (!ampPinValid(s.ampBclk)) s.ampBclk = -1;
  if (!ampPinValid(s.ampLrc)) s.ampLrc = -1;
  if (!ampPinValid(s.ampDin)) s.ampDin = -1;
  if (s.ampExt && (s.ampBclk < 0 || s.ampLrc < 0 || s.ampDin < 0 || s.ampBclk == s.ampLrc || s.ampBclk == s.ampDin || s.ampLrc == s.ampDin)) s.ampExt = false;
  if (isnan(s.lat) || s.lat < -90 || s.lat > 90) s.lat = 41.8919f;
  if (isnan(s.lon) || s.lon < -180 || s.lon > 180) s.lon = 12.5113f;
  if (s.chatModel.length() == 0) s.chatModel = DEF_CHAT_MODEL;
  if (s.fbModel.length() == 0) s.fbModel = DEF_FB_MODEL;
  if (s.sttModel.length() == 0) s.sttModel = DEF_STT_MODEL;
  if (s.azureRegion.length() == 0) s.azureRegion = "westeurope";
  if (s.voice.length() == 0) s.voice = "it-IT-ElsaNeural";
  if (s.prompt.length() == 0) s.prompt = DEF_PROMPT;
  if (s.city.length() == 0) s.city = "Roma";
  if (s.tz.length() == 0) s.tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  if (s.radioFav.length() == 0) s.radioFav = DEF_RADIO_FAV;
}

static void settingsLoad(Settings &s) {
  settingsDefaults(s);
  Preferences p;
  if (!p.begin("jarvis", true)) {  // namespace assente al primo avvio
    LOGI("NVS: nessuna impostazione salvata, uso i valori predefiniti");
    return;
  }
  s.wifiSsid = p.getString("wifiSsid", "");
  s.wifiPass = p.getString("wifiPass", "");
  s.webPass = p.getString("webPass", "");
  s.groqKey = p.getString("groqKey", "");
  s.chatModel = p.getString("chatModel", s.chatModel);
  s.fbModel = p.getString("fbModel", s.fbModel);
  s.sttModel = p.getString("sttModel", s.sttModel);
  s.azureKey = p.getString("azureKey", "");
  s.azureRegion = p.getString("azureRegion", s.azureRegion);
  s.voice = p.getString("voice", s.voice);
  s.prompt = p.getString("prompt", s.prompt);
  s.city = p.getString("city", s.city);
  s.lat = p.getFloat("lat", s.lat);
  s.lon = p.getFloat("lon", s.lon);
  s.geoCity = p.getString("geoCity", "");
  s.tz = p.getString("tz", s.tz);
  s.volume = p.getInt("volume", s.volume);
  s.micGain = p.getInt("micGain", s.micGain);
  s.wakeSens = p.getInt("wakeSens", s.wakeSens);
  s.radioFav = p.getString("radioFav", s.radioFav);
  s.ampExt = p.getBool("ampExt", s.ampExt);
  s.ampBclk = p.getInt("ampBclk", s.ampBclk);
  s.ampLrc = p.getInt("ampLrc", s.ampLrc);
  s.ampDin = p.getInt("ampDin", s.ampDin);
  s.startMode = p.getInt("startMode", s.startMode);
  s.forceSetup = p.getBool("forceSetup", false);
  s.boardRev = p.getInt("boardRev", s.boardRev);
  s.chgMa = p.getInt("chgMa", s.chgMa);
  p.end();
  settingsClamp(s);
}

// Salva solo le chiavi cambiate (meno usura della flash). old può essere nullptr.
static bool settingsSave(const Settings &s, const Settings *old) {
  Preferences p;
  if (!p.begin("jarvis", false)) {
    LOGE("NVS: impossibile aprire il namespace in scrittura");
    return false;
  }
#define SAVE_STR(k) if (!old || old->k != s.k) p.putString(#k, s.k)
#define SAVE_INT(k) if (!old || old->k != s.k) p.putInt(#k, s.k)
#define SAVE_BOOL(k) if (!old || old->k != s.k) p.putBool(#k, s.k)
#define SAVE_FLT(k) if (!old || old->k != s.k) p.putFloat(#k, s.k)
  SAVE_STR(wifiSsid); SAVE_STR(wifiPass); SAVE_STR(webPass);
  SAVE_STR(groqKey); SAVE_STR(chatModel); SAVE_STR(fbModel); SAVE_STR(sttModel);
  SAVE_STR(azureKey); SAVE_STR(azureRegion); SAVE_STR(voice);
  SAVE_STR(prompt); SAVE_STR(city); SAVE_FLT(lat); SAVE_FLT(lon); SAVE_STR(geoCity); SAVE_STR(tz);
  SAVE_INT(volume); SAVE_INT(micGain); SAVE_INT(wakeSens);
  SAVE_STR(radioFav); SAVE_BOOL(ampExt); SAVE_INT(ampBclk); SAVE_INT(ampLrc); SAVE_INT(ampDin);
  SAVE_INT(startMode); SAVE_BOOL(forceSetup); SAVE_INT(boardRev); SAVE_INT(chgMa);
#undef SAVE_STR
#undef SAVE_INT
#undef SAVE_BOOL
#undef SAVE_FLT
  p.putInt("cfgVer", CFG_VERSION);
  p.end();
  return true;
}

// Copia coerente delle impostazioni per i task diversi dal loop
static Settings cfgSnapshot() {
  LockGuard g(cfgLock, 1000);
  Settings s = cfg;
  return s;
}
