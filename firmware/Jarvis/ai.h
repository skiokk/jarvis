#pragma once
// ai.h - client HTTPS (sempre connessione nuova), Groq Whisper (multipart WAV), Groq chat
// con fallback di modello, Azure TTS in streaming nella coda audio, Open-Meteo (meteo e geocoding).
// Tutte le funzioni girano nel loop task (stack 20 KB) e non toccano cfg in scrittura.

#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include "settings.h"
#include "audio.h"
#include "ui.h"

#define MP_BOUNDARY "----Jarvis7d1f3a9c5e2b"

// --- Sink per le risposte -----------------------------------------------------------------------------------
// HTTPClient::writeToStream (core 3.3.12) gira finché la connessione è aperta e non ha un timeout
// complessivo. Con TLS readBytes() non aspetta: se non ci sono dati restituisce subito 0 e la sink
// riceve write(buf, 0), anche durante un trasferimento normale. Le sink contano quindi il tempo
// dall'ultimo byte utile e, se il server tace troppo a lungo (o l'utente interrompe), impostano
// l'errore di scrittura: writeToStream esce con HTTPC_ERROR_STREAM_WRITE.
// Limite noto: con una risposta senza Content-Length e non chunked la sink non viene chiamata.

// Accumula al massimo cap byte in PSRAM; accetta sempre tutto (writeToStream fallirebbe con -10).
class BufSink : public Stream {
 public:
  explicit BufSink(size_t capacity, uint32_t stallTimeoutMs = 15000, bool stopOnInterrupt = false)
      : stallMs(stallTimeoutMs), interruptible(stopOnInterrupt) {
    buf = (char *)psAlloc(capacity + 1);
    cap = buf ? capacity : 0;
    if (buf) buf[0] = 0;
  }
  ~BufSink() { if (buf) heap_caps_free(buf); }
  BufSink(const BufSink &) = delete;
  BufSink &operator=(const BufSink &) = delete;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *d, size_t n) override {
    if (!started) { started = true; lastData = millis(); }  // il conteggio parte con il corpo della risposta
    if (interruptible && gInterrupt.load()) { setWriteError(); return 0; }
    if (n == 0) {
      if (millis() - lastData > stallMs) { stalled = true; setWriteError(); }
      else vTaskDelay(1);  // niente attesa attiva mentre arrivano i dati
      return 0;
    }
    lastData = millis();
    size_t k = n < cap - len ? n : cap - len;
    if (k) { memcpy(buf + len, d, k); len += k; buf[len] = 0; }
    if (k < n) truncated = true;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  bool ok() const { return buf != nullptr; }
  const char *c_str() const { return buf ? buf : ""; }
  char *buf = nullptr;
  size_t cap = 0, len = 0;
  bool truncated = false, stalled = false;

 private:
  uint32_t stallMs;
  bool interruptible;
  bool started = false;
  uint32_t lastData = 0;
};

// PCM 16 bit mono dal TTS -> coda stereo. Blocca finché c'è spazio; gestisce il byte spezzato
// fra due blocchi chunked. Errore di scrittura (writeToStream si ferma) se l'utente interrompe
// o se Azure non manda dati per più di stallMs.
class TtsSink : public Stream {
 public:
  explicit TtsSink(uint32_t stallTimeoutMs) : stallMs(stallTimeoutMs) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *d, size_t n) override {
    if (!started) { started = true; lastData = millis(); }
    if (aborted || gInterrupt.load()) { aborted = true; setWriteError(); return 0; }
    if (n == 0) {
      if (millis() - lastData > stallMs) { stalled = true; setWriteError(); }
      else vTaskDelay(1);
      return 0;
    }
    int16_t tmp[256];
    uint32_t k = 0;
    size_t i = 0;
    if (hasCarry && n > 0) { tmp[k++] = (int16_t)(carry | ((uint16_t)d[0] << 8)); i = 1; hasCarry = false; }
    while (i + 1 < n) {
      tmp[k++] = (int16_t)(d[i] | ((uint16_t)d[i + 1] << 8));
      i += 2;
      if (k == 256) {
        if (!ringPushMono(tmp, k, 30000)) { aborted = true; setWriteError(); return 0; }
        k = 0;
      }
    }
    if (i < n) { carry = d[i]; hasCarry = true; }
    if (k && !ringPushMono(tmp, k, 30000)) { aborted = true; setWriteError(); return 0; }
    bytes += n;
    lastData = millis();  // dopo l'eventuale attesa di spazio nella coda
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  uint32_t bytes = 0;
  bool aborted = false, stalled = false;
 private:
  uint32_t stallMs, lastData = 0;
  bool started = false;
  uint8_t carry = 0;
  bool hasCarry = false;
};

// --- Helper HTTPS -----------------------------------------------------------------------------------------------
static void tlsSetup(NetworkClientSecure &tls, uint32_t timeoutMs) {
  tls.setInsecure();               // semplificazione nota: nessuna verifica del certificato
  tls.setHandshakeTimeout(12);     // secondi
  tls.setTimeout(timeoutMs);
  if (freeInternal() < 45000) LOGW("RAM interna bassa prima di TLS: %u byte (blocco max %u)", (unsigned)freeInternal(), (unsigned)largestInternal());
}

static String httpErrText(int code) {
  if (code > 0) return String("HTTP ") + code;
  return HTTPClient::errorToString(code);
}

// Estrae "error.message" da una risposta JSON di errore (per il log)
static String apiErrorMessage(const char *body) {
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc(&gJsonAlloc);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return String();
  const char *m = doc["error"]["message"] | "";
  String s(m);
  if (s.length() > 160) s = s.substring(0, 160);
  return s;
}

// GET con risposta JSON in sink. Restituisce il codice HTTP (o errore negativo).
static int httpsGet(const String &url, BufSink &sink, uint16_t timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_NOT_CONNECTED;
  if (!sink.ok()) return HTTPC_ERROR_TOO_LESS_RAM;
  NetworkClientSecure tls;
  tlsSetup(tls, timeoutMs);
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(8000);
  http.setTimeout(timeoutMs);
  http.setUserAgent(USER_AGENT);
  if (!http.begin(tls, url)) { tls.stop(); return HTTPC_ERROR_CONNECTION_REFUSED; }
  int code = http.GET();
  if (code > 0) {
    int r = http.writeToStream(&sink);
    if (r < 0 && code == 200) code = sink.stalled ? HTTPC_ERROR_READ_TIMEOUT : r;
  }
  http.end();
  tls.stop();
  return code;
}

// --- Groq Whisper -------------------------------------------------------------------------------------------------
static void wavHeader(uint8_t *h, uint32_t pcmBytes) {
  auto le32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  auto le16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
  memcpy(h, "RIFF", 4); le32(4, 36 + pcmBytes); memcpy(h + 8, "WAVEfmt ", 8);
  le32(16, 16); le16(20, 1); le16(22, 1); le32(24, SR_RATE); le32(28, SR_RATE * 2); le16(32, 2); le16(34, 16);
  memcpy(h + 36, "data", 4); le32(40, pcmBytes);
}

// Trascrizione della registrazione in gRecBuf (un solo POST: [intestazione][WAV][PCM][chiusura]).
// In caso di limite/errore del modello prova l'altro Whisper (i limiti Groq sono per modello).
static bool groqTranscribe(uint32_t pcmBytes, const Settings &s, String &text, String &err) {
  text = "";
  if (!gRecBuf || pcmBytes == 0) { err = "nessun audio"; return false; }
  if (s.groqKey.length() == 0) { err = "manca la chiave Groq"; return false; }
  uint8_t *wav = gRecBuf + REC_PREFIX;
  wavHeader(wav, pcmBytes);
  const char tail[] = "\r\n--" MP_BOUNDARY "--\r\n";
  memcpy(wav + REC_WAV_HDR + pcmBytes, tail, sizeof(tail) - 1);
  String models[2] = {s.sttModel, s.sttModel == "whisper-large-v3" ? "whisper-large-v3-turbo" : "whisper-large-v3"};
  for (int m = 0; m < 2; m++) {
    if (gInterrupt.load()) { err = "interrotto"; return false; }  // tocco durante la trascrizione
    String pre;
    pre.reserve(400);
    pre += "--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    pre += models[m];
    pre += "\r\n--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nit";
    pre += "\r\n--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson";
    pre += "\r\n--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0";
    pre += "\r\n--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"domanda.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    if (pre.length() > REC_PREFIX) { err = "intestazione troppo lunga"; return false; }
    uint8_t *body = wav - pre.length();  // intestazione allineata a destra, subito prima del WAV
    memcpy(body, pre.c_str(), pre.length());
    size_t bodyLen = pre.length() + REC_WAV_HDR + pcmBytes + sizeof(tail) - 1;

    if (WiFi.status() != WL_CONNECTED) { err = "Wi-Fi non connesso"; return false; }
    NetworkClientSecure tls;
    tlsSetup(tls, 20000);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(8000);
    http.setTimeout(20000);
    http.setUserAgent(USER_AGENT);
    if (!http.begin(tls, "https://api.groq.com/openai/v1/audio/transcriptions")) { err = "connessione"; return false; }
    http.addHeader("Authorization", "Bearer " + s.groqKey);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" MP_BOUNDARY);
    uint32_t t0 = millis();
    int code = http.POST(body, bodyLen);
    BufSink sink(16384, 20000, true);
    if (code > 0) {
      int r = http.writeToStream(&sink);
      if (r < 0 && code == 200) code = sink.stalled ? HTTPC_ERROR_READ_TIMEOUT : r;
    }
    http.end();
    tls.stop();
    LOGI("Whisper %s: %d in %u ms (%u byte inviati)", models[m].c_str(), code, (unsigned)(millis() - t0), (unsigned)bodyLen);
    if (gInterrupt.load()) { err = "interrotto"; return false; }
    if (code == 200) {
      JsonDocument filter;
      filter["text"] = true;
      JsonDocument doc(&gJsonAlloc);
      DeserializationError e = deserializeJson(doc, sink.c_str(), DeserializationOption::Filter(filter));
      if (e) { err = String("risposta Whisper non valida: ") + e.c_str(); return false; }
      text = String(doc["text"] | "");
      text.trim();
      return true;
    }
    String msg = apiErrorMessage(sink.c_str());
    if (msg.length()) LOGW("Groq STT: %s", msg.c_str());
    if (code == 401) { err = "chiave Groq non valida"; return false; }
    if (code < 0 && code != HTTPC_ERROR_READ_TIMEOUT) { err = "connessione a Groq fallita (" + httpErrText(code) + ")"; return false; }
    err = "trascrizione fallita (" + httpErrText(code) + ")";
    if (!(code == 400 || code == 403 || code == 404 || code == 429 || code == 498 || code >= 500 || code == HTTPC_ERROR_READ_TIMEOUT)) return false;
  }
  return false;
}

// Filtra le "allucinazioni" tipiche di Whisper su audio quasi muto
static bool whisperHallucination(const String &t) {
  String l = lowerIt(t);
  return l.indexOf("amara.org") >= 0 || l.indexOf("sottotitoli creati") >= 0 || l.indexOf("sottotitoli a cura") >= 0 ||
         l.indexOf("sottotitolato da") >= 0 || l.indexOf("iscriviti al canale") >= 0 || l.indexOf("www.") >= 0;
}

// --- Groq chat ------------------------------------------------------------------------------------------------------
struct ChatMsg {
  bool user;
  String text;
};
static ChatMsg gHist[8];
static int gHistN = 0;
static uint32_t gHistLast = 0;

static void historyReset() {
  for (auto &m : gHist) m.text = String();
  gHistN = 0;
}
static void historyAdd(bool user, const String &t) {
  if (gHistN == 8) {
    for (int i = 1; i < 8; i++) gHist[i - 1] = gHist[i];
    gHistN = 7;
  }
  gHist[gHistN].user = user;
  // al massimo 700 byte, tagliati su un confine di carattere: un UTF-8 spezzato renderebbe
  // non valido il JSON di tutte le richieste successive
  size_t cut = t.length();
  if (cut > 700) {
    cut = 700;
    while (cut > 0 && ((uint8_t)t[cut] & 0xC0) == 0x80) cut--;
  }
  gHist[gHistN].text = t.substring(0, cut);
  gHistN++;
  gHistLast = millis();
}

static String stripThink(String s) {
  for (;;) {
    int a = s.indexOf("<think>");
    if (a < 0) break;
    int b = s.indexOf("</think>", a);
    if (b < 0) { s = s.substring(0, a); break; }
    s = s.substring(0, a) + s.substring(b + 8);
  }
  int c = s.indexOf("</think>");  // a volte arriva solo la chiusura
  if (c >= 0) s = s.substring(c + 8);
  s.trim();
  return s;
}

// Una richiesta al modello indicato. 0 = ok, altrimenti codice HTTP/errore.
static int groqChatOnce(const String &model, const Settings &s, const String &question, const String &context, String &answer) {
  const bool gptoss = model.startsWith("openai/gpt-oss");
  const bool qwen = model.indexOf("qwen3") >= 0;
  JsonDocument req(&gJsonAlloc);
  req["model"] = model;
  req["temperature"] = 0.7;
  if (gptoss) {
    // GPT-OSS: reasoning_format non supportato; i token di ragionamento consumano il budget
    req["reasoning_effort"] = "low";
    req["include_reasoning"] = false;
    req["max_completion_tokens"] = 600;
  } else if (qwen) {
    req["reasoning_effort"] = "none";
    req["max_completion_tokens"] = 400;
  } else {
    req["max_completion_tokens"] = 400;
  }
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = s.prompt + "\n" + context;
  for (int i = 0; i < gHistN; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"] = gHist[i].user ? "user" : "assistant";
    m["content"] = gHist[i].text;
  }
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  u["content"] = question;
  if (req.overflowed()) return HTTPC_ERROR_TOO_LESS_RAM;
  size_t n = measureJson(req);
  char *body = (char *)psAlloc(n + 1);
  if (!body) return HTTPC_ERROR_TOO_LESS_RAM;
  serializeJson(req, body, n + 1);
  req.clear();

  int code = HTTPC_ERROR_NOT_CONNECTED;
  BufSink sink(32768, 25000, true);
  if (WiFi.status() == WL_CONNECTED) {
    NetworkClientSecure tls;
    tlsSetup(tls, 25000);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(8000);
    http.setTimeout(25000);
    http.setUserAgent(USER_AGENT);
    if (http.begin(tls, "https://api.groq.com/openai/v1/chat/completions")) {
      http.addHeader("Authorization", "Bearer " + s.groqKey);
      http.addHeader("Content-Type", "application/json");
      uint32_t t0 = millis();
      code = http.POST((uint8_t *)body, n);
      if (code > 0) {
        int r = http.writeToStream(&sink);
        if (r < 0 && code == 200) code = sink.stalled ? HTTPC_ERROR_READ_TIMEOUT : r;
      }
      LOGI("Groq chat %s: %d in %u ms", model.c_str(), code, (unsigned)(millis() - t0));
      http.end();
    } else {
      code = HTTPC_ERROR_CONNECTION_REFUSED;
    }
    tls.stop();
  }
  heap_caps_free(body);
  if (code != 200) {
    String msg = apiErrorMessage(sink.c_str());
    if (msg.length()) LOGW("Groq: %s", msg.c_str());
    return code;
  }
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["choices"][0]["finish_reason"] = true;
  JsonDocument doc(&gJsonAlloc);
  DeserializationError e = deserializeJson(doc, sink.c_str(), DeserializationOption::Filter(filter));
  if (e) { LOGW("Groq: JSON non valido (%s)", e.c_str()); return HTTPC_ERROR_ENCODING; }
  answer = stripThink(String(doc["choices"][0]["message"]["content"] | ""));
  const char *fr = doc["choices"][0]["finish_reason"] | "";
  if (answer.length() == 0) { LOGW("Groq %s: risposta vuota (finish_reason=%s)", model.c_str(), fr); return 204; }
  return 0;
}

// Chiede al modello principale, poi alle riserve (400/403/404/408/429/498/5xx, timeout, risposta vuota).
static bool groqChat(const Settings &s, const String &question, const String &context, String &answer, String &err) {
  answer = "";
  if (s.groqKey.length() == 0) { err = "manca la chiave Groq"; return false; }
  if (millis() - gHistLast > 10UL * 60 * 1000) historyReset();  // storia azzerata dopo 10 minuti
  String list[3] = {s.chatModel, s.fbModel, DEF_FB2_MODEL};
  for (int i = 0; i < 3; i++) {
    if (list[i].length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) if (list[j] == list[i]) dup = true;
    if (dup) continue;
    if (gInterrupt.load()) { err = "interrotto"; return false; }
    int r = groqChatOnce(list[i], s, question, context, answer);
    if (r == 0) {
      historyAdd(true, question);
      historyAdd(false, answer);
      return true;
    }
    if (r == 401) { err = "chiave Groq non valida"; return false; }
    if (r == HTTPC_ERROR_NOT_CONNECTED || r == HTTPC_ERROR_CONNECTION_REFUSED || r == HTTPC_ERROR_TOO_LESS_RAM) {
      err = "connessione a Groq fallita (" + httpErrText(r) + ")";
      return false;
    }
    err = "risposta non disponibile (" + httpErrText(r) + ")";
    LOGW("Modello %s non disponibile, provo la riserva", list[i].c_str());
  }
  return false;
}

// --- Azure TTS in streaming ------------------------------------------------------------------------------------------
// La voce va direttamente nella coda di riproduzione (16 kHz mono -> stereo).
// startFrames: contatore frame all'inizio; totalFrames: durata della voce in frame, nota quando
// tutto l'audio è in coda (per i sottotitoli). false se fallita o interrotta.
static bool azureSpeak(const Settings &s, const String &text, uint32_t &startFrames, uint32_t &totalFrames, String &err) {
  totalFrames = 0;
  if (s.azureKey.length() == 0) { err = "manca la chiave Azure"; return false; }
  if (text.length() == 0) return true;
  if (!setAudioRate(SR_RATE)) { err = "audio non disponibile"; return false; }
  String lang = "it-IT";
  if (s.voice.length() > 5 && s.voice[2] == '-' && s.voice[5] == '-') lang = s.voice.substring(0, 5);
  String ssml;
  ssml.reserve(text.length() + 200);
  ssml = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='" + lang + "'><voice name='" + xmlEscape(s.voice) + "'>";
  ssml += xmlEscape(text);
  ssml += "</voice></speak>";
  String url = "https://" + s.azureRegion + ".tts.speech.microsoft.com/cognitiveservices/v1";
  for (int attempt = 0; attempt < 2; attempt++) {
    if (WiFi.status() != WL_CONNECTED) { err = "Wi-Fi non connesso"; return false; }
    NetworkClientSecure tls;
    tlsSetup(tls, 15000);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
    if (!http.begin(tls, url)) { err = "connessione ad Azure"; return false; }
    http.addHeader("Ocp-Apim-Subscription-Key", s.azureKey);
    http.addHeader("Content-Type", "application/ssml+xml");
    http.addHeader("X-Microsoft-OutputFormat", "raw-16khz-16bit-mono-pcm");
    uint32_t t0 = millis();
    int code = http.POST(ssml);
    if (code == 200) {
      startFrames = playBegin(SR_RATE * 3 / 10);  // prefill 0,3 s
      TtsSink sink(15000);
      int r = http.writeToStream(&sink);  // gestisce sia chunked sia Content-Length
      playEnd();
      http.end();
      tls.stop();
      LOGI("Azure TTS: %u byte audio in %u ms (r=%d)", (unsigned)sink.bytes, (unsigned)(millis() - t0), r);
      if (sink.aborted) { playStop(); err = "interrotto"; return false; }
      if (sink.stalled) LOGW("Azure TTS: nessun dato da 15 s, voce troncata");
      if (r < 0 && sink.bytes == 0) { err = "voce non ricevuta (" + httpErrText(sink.stalled ? HTTPC_ERROR_READ_TIMEOUT : r) + ")"; return false; }
      totalFrames = sink.bytes / 2;  // mono 16 bit: un frame stereo per campione
      return true;
    }
    http.end();
    tls.stop();
    LOGW("Azure TTS: %s", httpErrText(code).c_str());
    if (code == 401 || code == 403) { err = "chiave o regione Azure non valida"; return false; }
    if (code == 400) { err = "richiesta TTS rifiutata (voce non valida?)"; return false; }
    if (code == 429 && attempt == 0) { vTaskDelay(pdMS_TO_TICKS(1500)); continue; }
    err = "sintesi vocale fallita (" + httpErrText(code) + ")";
    return false;
  }
  return false;
}

// --- Open-Meteo -----------------------------------------------------------------------------------------------------------
static int weekdayOf(int y, int m, int d) {  // 0 = domenica (Sakamoto)
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static bool weatherFetch(float lat, float lon, const String &city, Weather &w, String &err) {
  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4",
           lat, lon);
  BufSink sink(8192);
  int code = httpsGet(url, sink, 12000);
  if (code != 200) { err = "meteo: " + httpErrText(code); return false; }
  JsonDocument doc(&gJsonAlloc);
  DeserializationError e = deserializeJson(doc, sink.c_str());
  if (e) { err = String("meteo: JSON ") + e.c_str(); return false; }
  JsonObject cur = doc["current"];
  JsonObject day = doc["daily"];
  if (cur.isNull() || day.isNull()) { err = "meteo: risposta incompleta"; return false; }
  w = Weather();
  w.temp = cur["temperature_2m"] | 0.0f;
  w.hum = cur["relative_humidity_2m"] | 0;
  w.code = cur["weather_code"] | 0;
  w.isDay = (cur["is_day"] | 1) != 0;
  w.wind = cur["wind_speed_10m"] | 0.0f;
  for (int i = 0; i < 3; i++) {
    w.dCode[i] = day["weather_code"][i + 1] | 0;
    w.dMax[i] = day["temperature_2m_max"][i + 1] | 0.0f;
    w.dMin[i] = day["temperature_2m_min"][i + 1] | 0.0f;
    const char *d = day["time"][i + 1] | "";
    int yy = 0, mm = 0, dd = 0;
    if (sscanf(d, "%d-%d-%d", &yy, &mm, &dd) == 3 && mm >= 1 && mm <= 12) w.dWday[i] = weekdayOf(yy, mm, dd);
  }
  safeCopy(w.city, sizeof(w.city), cleanText(city, false).c_str());  // font Latin-1
  w.ok = true;
  return true;
}

// Geocoding della città. found=false se non esiste (risposta senza "results").
static bool geocode(const String &city, float &lat, float &lon, String &name, bool &found, String &err) {
  found = false;
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=it&format=json&name=" + urlEncode(city);
  BufSink sink(8192);
  int code = httpsGet(url, sink, 12000);
  if (code != 200) { err = "geocoding: " + httpErrText(code); return false; }
  JsonDocument doc(&gJsonAlloc);
  if (deserializeJson(doc, sink.c_str())) { err = "geocoding: JSON non valido"; return false; }
  JsonObject r = doc["results"][0];
  if (r.isNull()) return true;  // nessun risultato
  lat = r["latitude"] | NAN;
  lon = r["longitude"] | NAN;
  name = String(r["name"] | city.c_str());
  found = !isnan(lat) && !isnan(lon);
  return true;
}
