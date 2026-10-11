#pragma once
// web.h - pagina impostazioni (task dedicato), captive portal in setup, OTA, mDNS, avvio AP.
// Il task web NON scrive cfg: prepara pendingCfg sotto cfgLock; il loop applica e salva.
// Basic Auth (utente "admin") su ogni endpoint, upload OTA compreso, quando è impostata una password.

#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include "settings.h"
#include "ui.h"

static WebServer server(80);
static DNSServer dns;
static bool gSetupMode = false;
// Configurazione aperta (senza password): primo avvio senza SSID salvato, oppure richiesta con
// BOOT 5 s (presenza fisica). Se parte da sola perché il Wi-Fi non risponde (es. router lento
// dopo un blackout) la pagina resta protetta dalla password scelta. Scritto prima del task web.
static bool gSetupOpen = false;
static std::atomic<uint32_t> gWebActivity{0};
static std::atomic<bool> gOtaActive{false};
static bool gOtaAuthOk = false, gOtaError = false, gOtaEnded = false;  // usati solo dal task web
static String gOtaMsg;
static char gLastErr[100] = "";  // ultimo errore (protetto da uiLock), mostrato nella pagina
static String gHostIp;           // indirizzo mostrato (scritto prima dell'avvio del task web)
static TaskHandle_t gWebTask = nullptr;

static void setLastError(const String &e) {
  LockGuard g(uiLock, 100);
  if (g.ok) safeCopy(gLastErr, sizeof(gLastErr), e.c_str());
}

// --- Autenticazione -------------------------------------------------------------------------------------------
static String webPassword() {
  LockGuard g(cfgLock, 1000);
  return g.ok ? cfg.webPass : String("\x01");  // lock non preso: nessun accesso
}

static bool authOk() {
  String p = webPassword();
  if (p.length() == 0) return true;
  if (gSetupMode && gSetupOpen) return true;  // configurazione aperta: solo al primo avvio o dopo BOOT 5 s
  return server.authenticate("admin", p.c_str());
}

static bool checkAuth() {
  gWebActivity.store(millis());
  if (authOk()) return true;
  server.requestAuthentication(BASIC_AUTH, "Jarvis", "Autenticazione richiesta");
  return false;
}

// --- HTML ---------------------------------------------------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Jarvis</title><style>
body{font-family:system-ui,sans-serif;background:#111418;color:#e8eaed;margin:0;padding:16px;max-width:680px;margin:auto}
h1{font-size:1.4em;color:#50d7ff;margin:.2em 0}h2{font-size:1.05em;color:#50d7ff;margin:0 0 .5em}
fieldset{border:1px solid #2c323a;border-radius:10px;margin:14px 0;padding:12px 14px}legend{display:none}
label{display:block;margin:.6em 0 .2em;font-size:.92em;color:#b8bec6}
input,select,textarea{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #3a414b;background:#1b1f25;color:#e8eaed;font-size:1em}
textarea{min-height:110px}input[type=checkbox]{width:auto;margin-right:8px}
.row{display:flex;gap:10px}.row>div{flex:1}.note{font-size:.82em;color:#8b939e;margin:.3em 0}
button,.btn{background:#50d7ff;color:#111;border:0;border-radius:10px;padding:12px 18px;font-size:1em;font-weight:600;margin-top:10px;cursor:pointer;text-decoration:none;display:inline-block}
.st{font-size:.85em;color:#b8bec6;line-height:1.6}.ok{color:#5ddc78}.err{color:#ff7a6b}a{color:#50d7ff}
progress{width:100%;height:18px}</style></head><body>)HTML";

static const char PAGE_TAIL[] PROGMEM = "</body></html>";

static void sendStart() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent_P(PAGE_HEAD);
}

static void sendEnd() {
  server.sendContent_P(PAGE_TAIL);
  server.sendContent("");  // fine della risposta chunked
}

static void addInput(String &h, const char *label, const char *name, const String &value, const char *type, const char *extra) {
  h += "<label for='"; h += name; h += "'>"; h += label; h += "</label><input id='"; h += name; h += "' name='"; h += name;
  h += "' type='"; h += type; h += "' value='"; h += htmlEscape(value); h += "' "; h += extra; h += ">";
}

static void addKeyInput(String &h, const char *label, const char *name, const String &current) {
  String ph = current.length() ? "salvata, finisce con " + keyTail(current) : String("non impostata");
  h += "<label for='"; h += name; h += "'>"; h += label; h += "</label><input id='"; h += name; h += "' name='"; h += name;
  h += "' type='password' autocomplete='off' value='' placeholder='"; h += htmlEscape(ph); h += "'>";
}

static void addSelectInt(String &h, const char *label, const char *name, int cur, const int *vals, const char *const *texts, int n) {
  h += "<label for='"; h += name; h += "'>"; h += label; h += "</label><select id='"; h += name; h += "' name='"; h += name; h += "'>";
  for (int i = 0; i < n; i++) {
    h += "<option value='"; h += vals[i]; h += "'"; if (vals[i] == cur) h += " selected"; h += ">"; h += texts[i]; h += "</option>";
  }
  h += "</select>";
}

static void addDatalist(String &h, const char *id, const char *const *opts, int n) {
  h += "<datalist id='"; h += id; h += "'>";
  for (int i = 0; i < n; i++) { h += "<option value='"; h += opts[i]; h += "'>"; }
  h += "</datalist>";
}

static void handleRoot() {
  if (!checkAuth()) return;
  Settings s = cfgSnapshot();
  {
    LockGuard g(cfgLock, 1000);
    if (g.ok && pendingCfgReady) s = pendingCfg;  // modifiche salvate ma non ancora applicate
  }
  char lastErr[100];
  {
    LockGuard g(uiLock, 100);
    safeCopy(lastErr, sizeof(lastErr), g.ok ? gLastErr : "");
  }
  if (server.hasArg("scan")) { WiFi.scanDelete(); WiFi.scanNetworks(true); }
  sendStart();
  String h;
  h.reserve(4096);
  h += "<h1>Jarvis</h1><div class='st'>";
  if (gSetupMode) h += "<b>Configurazione iniziale</b>: compila Wi-Fi e chiavi, poi salva. Jarvis si riavvia.<br>";
  h += "Firmware " FW_VERSION " &middot; scheda V"; h += gBoardRev; h += gBoardRev == 2 ? " (CO5300/CST820)" : " (SH8601/FT3168)";
  h += "<br>Indirizzo: "; h += htmlEscape(gHostIp);
  if (!gSetupMode) { h += " &middot; Wi-Fi "; h += WiFi.RSSI(); h += " dBm"; }
  int bp = gBattPct.load();
  h += "<br>Batteria: "; if (bp >= 0) { h += bp; h += "%"; } else h += "non rilevata (USB)";
  h += " &middot; RAM libera "; h += (unsigned)(freeInternal() / 1024); h += " KB, PSRAM "; h += (unsigned)(freePsram() / 1024); h += " KB";
  h += " &middot; acceso da "; h += (unsigned)(millis() / 60000); h += " min";
  if (lastErr[0]) { h += "<br><span class='err'>Ultimo errore: "; h += htmlEscape(lastErr); h += "</span>"; }
  if (!gSetupMode && s.webPass.length() == 0) h += "<br><span class='err'>Pagina senza password: chiunque sulla rete può modificarla.</span>";
  h += "</div><form method='post' action='/save'>";
  server.sendContent(h);

  // Wi-Fi
  h = "<fieldset><h2>Wi-Fi</h2>";
  addInput(h, "Nome rete (SSID)", "wifiSsid", s.wifiSsid, "text", "maxlength='32' list='nets' autocomplete='off'");
  h += "<datalist id='nets'>";
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED && gSetupMode) WiFi.scanNetworks(true);
  for (int i = 0; i < n && i < 30; i++) { h += "<option value='"; h += htmlEscape(WiFi.SSID(i)); h += "'>"; }
  h += "</datalist><p class='note'>";
  h += n == WIFI_SCAN_RUNNING ? "Ricerca reti in corso... " : "";
  h += "<a href='/?scan=1'>Cerca reti</a></p>";
  h += "<label for='wifiPass'>Password Wi-Fi</label><input id='wifiPass' name='wifiPass' type='password' maxlength='63' autocomplete='off' placeholder='";
  h += s.wifiPass.length() ? "invariata" : "nessuna";
  h += "'><p class='note'>Vuota = invariata (se cambi rete senza password: rete aperta).</p></fieldset>";
  // Pagina
  h += "<fieldset><h2>Protezione della pagina</h2>";
  h += "<label for='webPass'>Password (utente <b>admin</b>)</label><input id='webPass' name='webPass' type='password' maxlength='64' autocomplete='new-password' placeholder='";
  h += s.webPass.length() ? "impostata (vuota = invariata)" : "nessuna";
  h += "'><label><input type='checkbox' name='webPassClear' value='1'>Rimuovi la password</label></fieldset>";
  server.sendContent(h);

  // Groq / Azure
  static const char *const chatModels[] = {"qwen/qwen3.8-27b", "openai/gpt-oss-120b", "openai/gpt-oss-20b"};
  h = "<fieldset><h2>Groq (trascrizione e risposte)</h2>";
  addKeyInput(h, "Chiave API Groq", "groqKey", s.groqKey);
  addInput(h, "Modello chat", "chatModel", s.chatModel, "text", "maxlength='64' list='cm'");
  addInput(h, "Modello di riserva", "fbModel", s.fbModel, "text", "maxlength='64' list='cm'");
  addDatalist(h, "cm", chatModels, 3);
  {
    static const int v[] = {0, 1};
    static const char *const t[] = {"whisper-large-v3-turbo (veloce)", "whisper-large-v3 (più preciso)"};
    addSelectInt(h, "Modello di trascrizione", "sttModel", s.sttModel == "whisper-large-v3" ? 1 : 0, v, t, 2);
  }
  h += "<p class='note'>Piano gratuito Groq: 30 richieste/min e 1.000/giorno per modello.</p></fieldset>";
  static const char *const voices[] = {"it-IT-ElsaNeural", "it-IT-IsabellaNeural", "it-IT-DiegoNeural", "it-IT-GiuseppeMultilingualNeural", "it-IT-FiammaNeural", "it-IT-BenignoNeural"};
  h += "<fieldset><h2>Azure Speech (voce)</h2>";
  addKeyInput(h, "Chiave Azure Speech", "azureKey", s.azureKey);
  addInput(h, "Regione", "azureRegion", s.azureRegion, "text", "maxlength='32' placeholder='westeurope'");
  addInput(h, "Voce", "voice", s.voice, "text", "maxlength='80' list='vv'");
  addDatalist(h, "vv", voices, 6);
  h += "</fieldset>";
  server.sendContent(h);

  h = "<fieldset><h2>Personalità</h2><label for='prompt'>Istruzioni per l'assistente</label><textarea id='prompt' name='prompt' maxlength='1500'>";
  h += htmlEscape(s.prompt);
  h += "</textarea></fieldset><fieldset><h2>Meteo e ora</h2>";
  addInput(h, "Città", "city", s.city, "text", "maxlength='64'");
  addInput(h, "Fuso orario (POSIX)", "tz", s.tz, "text", "maxlength='64'");
  h += "</fieldset>";
  server.sendContent(h);

  // Audio
  h = "<fieldset><h2>Audio e voce</h2>";
  addInput(h, "Volume (0-100)", "volume", String(s.volume), "number", "min='0' max='100'");
  {
    static const int v[] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const char *const t[] = {"0 (0 dB)", "1 (+6 dB)", "2 (+12 dB)", "3 (+18 dB)", "4 (+24 dB)", "5 (+30 dB)", "6 (+36 dB)", "7 (+42 dB)"};
    addSelectInt(h, "Guadagno microfono", "micGain", s.micGain, v, t, 8);
    static const int w[] = {0, 1};
    static const char *const wt[] = {"Normale", "Più sensibile (più falsi allarmi)"};
    addSelectInt(h, "Sensibilità «Jarvis» (riavvio)", "wakeSens", s.wakeSens, w, wt, 2);
  }
  addInput(h, "Radio preferita", "radioFav", s.radioFav, "text", "maxlength='64'");
  h += "<p class='note'>Solo stazioni MP3 (es. Radio 105, Virgin Radio Italia, Rai Radio 1). Radio Deejay trasmette solo in AAC/HLS.</p></fieldset>";
  server.sendContent(h);

  // Amplificatore esterno, scheda, avvio
  h = "<fieldset><h2>Amplificatore esterno MAX98357A (riavvio)</h2><label><input type='checkbox' name='ampExt' value='1'";
  if (s.ampExt) h += " checked";
  h += ">Usa l'amplificatore esterno</label>";
  {
    static const int v[] = {-1, 17, 18, 38, 39, 40, 41, 42};
    static const char *const t[] = {"-", "GPIO17", "GPIO18", "GPIO38", "GPIO39", "GPIO40", "GPIO41", "GPIO42"};
    h += "<div class='row'><div>"; addSelectInt(h, "BCLK", "ampBclk", s.ampBclk, v, t, 8);
    h += "</div><div>"; addSelectInt(h, "LRC", "ampLrc", s.ampLrc, v, t, 8);
    h += "</div><div>"; addSelectInt(h, "DIN", "ampDin", s.ampDin, v, t, 8);
    h += "</div></div><p class='note'>Connettore di espansione, passo 1,27 mm (va saldato). 5 V solo con USB collegata.</p></fieldset>";
  }
  h += "<fieldset><h2>Scheda e batteria</h2>";
  {
    static const int v[] = {0, 1, 2};
    static const char *const t[] = {"Automatica", "V1 (SH8601 + FT3168)", "V2 (CO5300 + CST820)"};
    addSelectInt(h, "Revisione scheda (riavvio)", "boardRev", s.boardRev, v, t, 3);
    static const int c[] = {0, 100, 150, 200, 300, 400, 500};
    static const char *const ct[] = {"Non modificare", "100 mA", "150 mA", "200 mA", "300 mA", "400 mA", "500 mA"};
    addSelectInt(h, "Corrente di ricarica", "chgMa", s.chgMa, c, ct, 7);
    h += "<p class='note'>Al massimo metà della capacità: 400 mAh &rarr; 200 mA, 1000 mAh &rarr; 500 mA.</p>";
    static const int m[] = {0, 1};
    static const char *const mt[] = {"Orologio", "Assistente"};
    addSelectInt(h, "Modalità all'accensione", "startMode", s.startMode, m, mt, 2);
  }
  h += "</fieldset><button type='submit'>Salva</button></form><p><a href='/update'>Aggiornamento firmware (OTA)</a></p>";
  server.sendContent(h);
  sendEnd();
}

// --- Salvataggio -------------------------------------------------------------------------------------------------------
static bool printableUtf8(const String &s, bool allowNewline) {
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '\n' && allowNewline) continue;
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}
static bool charsetOk(const String &s, const char *extra) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!isalnum((uint8_t)c) && !strchr(extra, c)) return false;
  }
  return true;
}

static void handleSave() {
  if (!checkAuth()) return;
  String errs;
  Settings n;
  {
    LockGuard g(cfgLock, 2000);
    if (!g.ok) { server.send(503, "text/plain", "Occupato, riprova"); return; }
    n = pendingCfgReady ? pendingCfg : cfg;
  }
  const Settings old = n;
  auto arg = [](const char *k) { String v = server.arg(k); v.trim(); return v; };

  String v = arg("wifiSsid");
  if (server.hasArg("wifiSsid")) {
    if (v.length() <= 32 && printableUtf8(v, false)) n.wifiSsid = v; else errs += "SSID non valido. ";
  }
  String wp = server.arg("wifiPass");  // la password può avere spazi: niente trim
  if (wp.length()) {
    if ((wp.length() >= 8 && wp.length() <= 63) && printableUtf8(wp, false)) n.wifiPass = wp; else errs += "Password Wi-Fi: 8-63 caratteri. ";
  } else if (n.wifiSsid != old.wifiSsid) {
    n.wifiPass = "";  // rete nuova senza password = rete aperta
  }
  String pw = server.arg("webPass");
  if (server.hasArg("webPassClear")) n.webPass = "";
  else if (pw.length()) {
    if (pw.length() <= 64 && printableUtf8(pw, false)) n.webPass = pw; else errs += "Password pagina non valida (max 64 caratteri). ";
  }
  v = arg("groqKey");
  if (v.length()) { if (v.length() >= 20 && v.length() <= 200 && charsetOk(v, "_-.")) n.groqKey = v; else errs += "Chiave Groq non valida. "; }
  v = arg("azureKey");
  if (v.length()) { if (v.length() >= 20 && v.length() <= 200 && charsetOk(v, "_-.")) n.azureKey = v; else errs += "Chiave Azure non valida. "; }
  v = arg("chatModel");
  if (v.length()) { if (v.length() <= 64 && charsetOk(v, "._/:-")) n.chatModel = v; else errs += "Modello chat non valido. "; }
  v = arg("fbModel");
  if (v.length()) { if (v.length() <= 64 && charsetOk(v, "._/:-")) n.fbModel = v; else errs += "Modello di riserva non valido. "; }
  if (server.hasArg("sttModel")) n.sttModel = arg("sttModel") == "1" ? "whisper-large-v3" : "whisper-large-v3-turbo";
  v = arg("azureRegion");
  if (v.length()) { v.toLowerCase(); if (v.length() <= 32 && charsetOk(v, "")) n.azureRegion = v; else errs += "Regione Azure non valida. "; }
  v = arg("voice");
  if (v.length()) { if (v.length() <= 80 && charsetOk(v, "-:")) n.voice = v; else errs += "Voce non valida. "; }
  if (server.hasArg("prompt")) {
    String p = server.arg("prompt");
    p.replace("\r", "");
    p.trim();
    if (p.length() == 0) p = DEF_PROMPT;
    if (p.length() <= 1500 && printableUtf8(p, true)) n.prompt = p; else errs += "Istruzioni troppo lunghe o non valide. ";
  }
  v = arg("city");
  if (v.length()) { if (v.length() <= 64 && printableUtf8(v, false)) n.city = v; else errs += "Città non valida. "; }
  v = arg("tz");
  if (v.length()) { if (v.length() <= 64 && printableUtf8(v, false) && v.indexOf(' ') < 0) n.tz = v; else errs += "Fuso orario non valido. "; }
  v = arg("radioFav");
  if (v.length()) { if (v.length() <= 64 && printableUtf8(v, false)) n.radioFav = v; else errs += "Radio preferita non valida. "; }
  auto argInt = [&](const char *k, int lo, int hi, int &dst, const char *msg) {
    if (!server.hasArg(k)) return;
    String s = arg(k);
    char *end = nullptr;
    long x = strtol(s.c_str(), &end, 10);
    if (s.length() && end && *end == 0 && x >= lo && x <= hi) dst = (int)x;
    else { errs += msg; errs += ' '; }
  };
  argInt("volume", 0, 100, n.volume, "Volume 0-100.");
  argInt("micGain", 0, 7, n.micGain, "Guadagno 0-7.");
  argInt("wakeSens", 0, 1, n.wakeSens, "Sensibilità non valida.");
  argInt("startMode", 0, 1, n.startMode, "Modalità non valida.");
  argInt("boardRev", 0, 2, n.boardRev, "Revisione non valida.");
  int chg = n.chgMa;
  argInt("chgMa", 0, 500, chg, "Corrente non valida.");
  if (chgMaValid(chg)) n.chgMa = chg;
  n.ampExt = server.hasArg("ampExt");
  int b = n.ampBclk, l = n.ampLrc, d = n.ampDin;
  argInt("ampBclk", -1, 48, b, "Pin BCLK non valido.");
  argInt("ampLrc", -1, 48, l, "Pin LRC non valido.");
  argInt("ampDin", -1, 48, d, "Pin DIN non valido.");
  n.ampBclk = ampPinValid(b) ? b : -1;
  n.ampLrc = ampPinValid(l) ? l : -1;
  n.ampDin = ampPinValid(d) ? d : -1;
  if (n.ampExt && (n.ampBclk < 0 || n.ampLrc < 0 || n.ampDin < 0 || n.ampBclk == n.ampLrc || n.ampBclk == n.ampDin || n.ampLrc == n.ampDin)) {
    errs += "Amplificatore: servono tre pin diversi (disattivato). ";
    n.ampExt = false;
  }
  settingsClamp(n);
  bool restart = gSetupMode || n.wifiSsid != old.wifiSsid || n.wifiPass != old.wifiPass || n.wakeSens != old.wakeSens ||
                 n.ampExt != old.ampExt || n.ampBclk != old.ampBclk || n.ampLrc != old.ampLrc || n.ampDin != old.ampDin ||
                 n.boardRev != old.boardRev;
  {
    LockGuard g(cfgLock, 2000);
    if (!g.ok) { server.send(503, "text/plain", "Occupato, riprova"); return; }
    pendingCfg = n;
    pendingCfgRestart = pendingCfgRestart || restart;
    pendingCfgReady = true;
  }
  LOGI("Web: impostazioni ricevute%s%s", restart ? " (riavvio)" : "", errs.length() ? ", con errori" : "");
  sendStart();
  String h = "<h1>Impostazioni salvate</h1>";
  if (errs.length()) { h += "<p class='err'>Alcuni campi non sono stati accettati: "; h += htmlEscape(errs); h += "</p>"; }
  if (restart) h += "<p>Jarvis si riavvia per applicare le modifiche. Se hai cambiato rete Wi-Fi, ricollegati alla tua rete e apri <b>http://jarvis.local</b> o l'indirizzo mostrato sul display.</p>";
  else h += "<p class='ok'>Modifiche applicate.</p><p><a class='btn' href='/'>Torna alle impostazioni</a></p>";
  server.sendContent(h);
  sendEnd();
}

// --- OTA -----------------------------------------------------------------------------------------------------------------------
static const char OTA_FORM[] PROGMEM = R"HTML(<h1>Aggiornamento firmware</h1>
<p class='st'>Scegli il file <b>Jarvis_aggiornamento_OTA.bin</b>. Non spegnere durante il caricamento.</p>
<form id='f' method='post' action='/update' enctype='multipart/form-data'>
<input type='file' name='firmware' accept='.bin' required><button type='submit'>Carica</button></form>
<progress id='p' max='100' value='0' hidden></progress><p id='m' class='st'></p><p><a href='/'>Indietro</a></p>
<script>
document.getElementById('f').onsubmit=function(e){e.preventDefault();var x=new XMLHttpRequest(),p=document.getElementById('p'),m=document.getElementById('m');
p.hidden=false;x.upload.onprogress=function(ev){if(ev.lengthComputable)p.value=ev.loaded*100/ev.total;};
x.onload=function(){m.innerHTML=x.status==200?'<span class=ok>Completato: riavvio in corso...</span>':'<span class=err>Errore: '+x.responseText.replace(/</g,'&lt;')+'</span>';};
x.onerror=function(){m.innerHTML='<span class=err>Connessione interrotta</span>';};
x.open('POST','/update');x.send(new FormData(this));};
</script>)HTML";

static void handleUpdateForm() {
  if (!checkAuth()) return;
  sendStart();
  server.sendContent_P(OTA_FORM);
  sendEnd();
}

// Callback di upload: gira PRIMA dell'handler della rotta, quindi l'autenticazione va verificata qui.
static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  gWebActivity.store(millis());
  if (up.status == UPLOAD_FILE_START) {
    gOtaError = false;
    gOtaEnded = false;
    gOtaMsg = "";
    gOtaAuthOk = authOk();
    if (!gOtaAuthOk) { LOGW("OTA rifiutato: autenticazione mancante"); return; }
    gOtaActive.store(true);
    gOtaPct.store(0);
    LOGI("OTA: inizio (%s)", up.filename.c_str());
    if (Update.isRunning()) Update.abort();  // residuo di un caricamento precedente
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { gOtaError = true; gOtaMsg = Update.errorString(); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!gOtaAuthOk || gOtaError) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      gOtaError = true;
      gOtaMsg = Update.errorString();
      Update.abort();
      return;
    }
    int total = server.clientContentLength();
    if (total > 0) gOtaPct.store((int)((uint64_t)up.totalSize * 100 / (uint32_t)total));
  } else if (up.status == UPLOAD_FILE_END) {
    if (!gOtaAuthOk || gOtaError) return;
    if (!Update.end(true) || !Update.isFinished()) { gOtaError = true; gOtaMsg = Update.errorString(); }
    else { gOtaEnded = true; LOGI("OTA: %u byte scritti e verificati", (unsigned)up.totalSize); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // Dopo un abort il WebServer NON chiama handleUpdateDone: lo stato va chiuso qui.
    if (gOtaAuthOk && gOtaEnded && !gOtaError) {
      // file completo e verificato (partizione di avvio già cambiata da Update.end): è mancata
      // solo la chiusura del modulo, il client si è scollegato. Si riavvia sulla nuova versione.
      LOGW("OTA: client scollegato dopo la fine del file: aggiornamento valido, riavvio");
      gOtaPct.store(100);
      gRestartAt.store(millis() + 1500);
      return;
    }
    if (gOtaAuthOk && Update.isRunning()) Update.abort();
    gOtaError = true;
    gOtaMsg = "caricamento interrotto";
    LOGW("OTA: caricamento interrotto");
    if (gOtaAuthOk) setLastError("OTA: caricamento interrotto");
    gOtaActive.store(false);
    gOtaPct.store(-1);
  }
}

// Rete di sicurezza (loop): OTA "in corso" ma nessuna attività del client da 30 s.
// Il WebServer chiude da sé i caricamenti fermi (HTTP_MAX_POST_WAIT), quindi qui si azzerano
// solo i flag che bloccherebbero loop ed eventi; Update resta al task web.
static void otaWatchdog() {
  if (!gOtaActive.load() || gRestartAt.load() != 0) return;
  if (millis() - gWebActivity.load() < 30000) return;
  LOGW("OTA: nessun dato da 30 s, aggiornamento annullato");
  setLastError("OTA: caricamento interrotto");
  gOtaActive.store(false);
  gOtaPct.store(-1);
}

static void handleUpdateDone() {
  if (!checkAuth()) { gOtaActive.store(false); gOtaPct.store(-1); return; }
  if (!gOtaAuthOk || gOtaError || !Update.isFinished()) {
    String m = gOtaMsg.length() ? gOtaMsg : String("file non ricevuto");
    if (Update.isRunning()) Update.abort();
    LOGE("OTA fallito: %s", m.c_str());
    setLastError("OTA: " + m);
    gOtaActive.store(false);
    gOtaPct.store(-1);
    server.send(500, "text/plain; charset=utf-8", m);
    return;
  }
  gOtaPct.store(100);
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain; charset=utf-8", "OK, riavvio");
  gRestartAt.store(millis() + 1500);
}

// --- Captive portal e altre rotte -------------------------------------------------------------------------------------------
static void redirectHome() {
  server.sendHeader("Location", gSetupMode ? String("http://192.168.4.1/") : String("/"), true);
  server.send(302, "text/plain", "");
}

static void handleCaptive() {
  if (!gSetupMode && !checkAuth()) return;
  gWebActivity.store(millis());
  redirectHome();
}

static void handleNotFound() {
  if (!gSetupMode && !checkAuth()) return;
  redirectHome();
}

static void webTask(void *) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void webStart() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, handleUpdateForm);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  // sonde captive portal di Android, Apple, Windows, Firefox
  static const char *const probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                                       "/ncsi.txt", "/fwlink", "/redirect", "/canonical.html", "/success.txt", "/chat"};
  for (const char *p : probes) server.on(p, HTTP_ANY, handleCaptive);
  server.onNotFound(handleNotFound);
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, &gWebTask, 1);
}

// Nome AP stabile dal MAC
static String apName() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char n[24];
  snprintf(n, sizeof(n), "Jarvis-%02X%02X", mac[4], mac[5]);
  return String(n);
}

static bool startSetupAP() {
  WiFi.disconnect(true);
  WiFi.setAutoReconnect(false);
  delay(100);
  WiFi.mode(WIFI_AP_STA);  // STA serve per la scansione delle reti
  String n = apName();
  safeCopy(gApName, sizeof(gApName), n.c_str());
  IPAddress ip(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, ip, mask);
  bool ok = WiFi.softAP(n.c_str());  // rete aperta
  delay(200);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());  // in 3.x risponde in modo asincrono (AsyncUDP)
  WiFi.scanNetworks(true);
  gHostIp = "192.168.4.1 (rete " + n + ")";
  LOGI("Setup: AP '%s' %s, IP %s", n.c_str(), ok ? "attivo" : "NON avviato", WiFi.softAPIP().toString().c_str());
  return ok;
}

// --- Rollback OTA ---------------------------------------------------------------------------------------------------------------
// Il core segnerebbe valida la nuova immagine prima di setup(): rimandiamo la conferma a quando
// display, audio e pagina web sono partiti. Il bootloader torna alla versione precedente solo a un
// RIAVVIO con l'immagine non confermata: se il nuovo firmware va in crash/watchdog prima della conferma,
// oppure se si ferma in uno degli arresti di setup() (fatalHalt chiede il rollback esplicitamente).
extern "C" bool verifyRollbackLater(void) { return true; }

static bool gOtaValidMarked = false;
static void markOtaValid() {
  if (gOtaValidMarked) return;
  gOtaValidMarked = true;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) LOGI("OTA: nuova versione confermata");
    else LOGE("OTA: conferma della nuova versione fallita");
  }
}

static void startMdns() {
  if (MDNS.begin("jarvis")) {
    MDNS.addService("http", "tcp", 80);
    LOGI("mDNS: http://jarvis.local");
  } else {
    LOGW("mDNS non avviato");
  }
}
