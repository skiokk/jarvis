#pragma once
// radio.h - ricerca stazioni su radio-browser.info, riconoscimento del comando vocale,
// stream HTTP/HTTPS (anche ICY) con redirect, decodifica MP3 con il decoder Helix del core.

#include "ai.h"
#include "mp3dec.h"  // chmorgan esp-libhelix-mp3, incluso e linkato dal core 3.3.12

struct Station {
  String name, url, uuid, country;
  int bitrate = 0;
};

static void loopServiceLight();  // definita nello sketch: applica le impostazioni durante la radio

// --- Ricerca stazione ----------------------------------------------------------------------------------------
static String normName(const String &s) {
  String l = lowerIt(s);
  String o;
  o.reserve(l.length());
  bool sp = true;
  for (size_t i = 0; i < l.length(); i++) {
    char c = l[i];
    if (c == ' ' || c == '-' || c == '_' || c == '\t') { if (!sp) o += ' '; sp = true; }
    else { o += c; sp = false; }
  }
  o.trim();
  return o;
}

// Nome normalizzato senza la parola "radio" ("deejay fox radio" -> "deejay fox")
static String stripRadioWord(const String &n) {
  String s = " " + n + " ";
  while (s.indexOf(" radio ") >= 0) s.replace(" radio ", " ");
  s.trim();
  return s;
}

// Sottocanale tematico: "Radio 105 - Hits", "Virgin Radio Rockstar: Rolling Stones"
static bool isSubChannel(const String &name) {
  return name.indexOf(" - ") >= 0 || name.indexOf(" \xE2\x80\x93 ") >= 0 || name.indexOf(" \xE2\x80\x94 ") >= 0 ||
         name.indexOf(" | ") >= 0 || name.indexOf(": ") >= 0;
}

// Come confrontare il nome della stazione con la richiesta
enum RbMatch : int {
  RB_CONTAINS = 0,  // nome che contiene la richiesta (la API filtra già per "contiene")
  RB_EXACT,         // solo nome identico (ricerca fuori dall'Italia)
  RB_NO_RADIO,      // nome identico senza la parola "radio" (secondo tentativo, richiesta ridotta)
};

// Una ricerca (limit=10) con filtro MP3; restituisce la stazione migliore secondo il nome.
static bool rbSearch(const String &query, bool italy, RbMatch match, Station &best, String &err) {
  static const char *const hosts[] = {"all.api.radio-browser.info", "de1.api.radio-browser.info"};
  String q = normName(query);
  for (const char *host : hosts) {
    if (gInterrupt.load()) { err = "interrotto"; return false; }
    String url = String("https://") + host + "/json/stations/search?limit=10&hidebroken=true&order=votes&reverse=true&codec=MP3&name=" + urlEncode(query);
    if (italy) url += "&countrycode=IT";
    BufSink sink(65536, 12000, true);
    int code = httpsGet(url, sink, 12000);
    if (code != 200) {
      err = String("radio-browser ") + host + ": " + httpErrText(code);
      LOGW("%s", err.c_str());
      continue;
    }
    JsonDocument filter;
    filter[0]["name"] = true;
    filter[0]["url_resolved"] = true;
    filter[0]["stationuuid"] = true;
    filter[0]["countrycode"] = true;
    filter[0]["bitrate"] = true;
    filter[0]["hls"] = true;
    JsonDocument doc(&gJsonAlloc);
    if (deserializeJson(doc, sink.c_str(), DeserializationOption::Filter(filter))) { err = "radio-browser: JSON non valido"; return false; }
    int bestScore = 0;
    size_t bestLen = 9999;
    for (JsonObject st : doc.as<JsonArray>()) {
      String name = String(st["name"] | "");
      String u = String(st["url_resolved"] | "");
      int hls = st["hls"] | 0;
      if (name.length() == 0 || u.length() < 8 || hls) continue;
      if (!(u.startsWith("http://") || u.startsWith("https://")) || u.endsWith(".m3u8")) continue;
      String n = normName(name);
      int score = n == q ? 3 : (n.startsWith(q) ? 2 : (n.indexOf(q) >= 0 ? 1 : 0));
      if (match == RB_EXACT && score < 3) continue;
      if (match == RB_NO_RADIO) {
        // la richiesta è già stata ridotta ("radio deejay" -> "deejay"): niente omonimi
        // ("Deejay Fox Radio"), solo lo stesso nome a meno della parola "radio"
        if (stripRadioWord(n) != q) continue;
        score = 3;
      }
      if (score == 0) score = 1;  // la API ha già filtrato per "contiene"
      // un sottocanale ("Radio 105 - Hits") non batte la stazione principale ("Radio 105 Network")
      if (score == 2 && isSubChannel(name)) score = 1;
      // a parità di punteggio: nome più corto (il canale principale), poi più voti (ordine API)
      if (score > bestScore || (score == bestScore && n.length() < bestLen)) {
        bestScore = score;
        bestLen = n.length();
        best.name = name;
        best.url = u;
        best.uuid = String(st["stationuuid"] | "");
        best.country = String(st["countrycode"] | "");
        best.bitrate = st["bitrate"] | 0;
      }
    }
    return bestScore > 0;
  }
  return false;
}

static bool radioFind(const String &query, Station &st, String &err) {
  String q = query;
  q.trim();
  if (rbSearch(q, true, RB_CONTAINS, st, err)) return true;
  String nq = normName(q);
  if (nq.startsWith("radio ") && nq.length() > 7) {  // "radio rtl 102.5 best" -> "rtl 102.5 best"
    if (gInterrupt.load()) { err = "interrotto"; return false; }
    if (rbSearch(nq.substring(6), true, RB_NO_RADIO, st, err)) return true;
  }
  // fuori dall'Italia solo con nome identico (evita omonimi, es. "Radio Deejay HR")
  if (gInterrupt.load()) { err = "interrotto"; return false; }
  if (rbSearch(q, false, RB_EXACT, st, err)) return true;
  if (err.length() == 0) err = "nessuna stazione MP3 trovata";
  return false;
}

// --- Comando vocale ----------------------------------------------------------------------------------------------
static bool radioCommand(const String &text, const String &fav, String &query) {
  String t = lowerIt(text);
  // punteggiatura -> spazio (il punto resta fra due cifre: "102.5")
  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    bool dotNum = c == '.' && i > 0 && i + 1 < t.length() && isdigit((uint8_t)t[i - 1]) && isdigit((uint8_t)t[i + 1]);
    if ((strchr(",.!?;:\"()", c) && !dotNum) || c == '\'') t.setCharAt(i, ' ');
  }
  t.replace("«", " ");
  t.replace("»", " ");
  t = " " + normName(t) + " ";
  if (t.indexOf("radio") < 0) return false;
  static const char *const verbs[] = {" metti ", " mettimi ", " accendi ", " fammi sentire ", " fammi ascoltare ", " ascolta ",
                                      " suona ", " avvia ", " fai partire ", " voglio sentire ", " voglio ascoltare "};
  bool verb = false;
  for (const char *v : verbs) if (t.indexOf(v) >= 0) verb = true;
  if (!verb) return false;
  static const char *const drop[] = {"jarvis", "metti", "mettimi", "accendi", "fammi", "sentire", "ascoltare", "ascolta", "suona",
                                     "avvia", "fai", "partire", "voglio", "per", "favore", "piacere", "ti", "prego", "pure",
                                     "la", "il", "lo", "l", "un", "una", "po", "di", "mi", "su", "sulla", "sul", "stazione",
                                     "canale", "adesso", "ora", "subito", "grazie", "dai", "e", "allora", "ok", "okay"};
  String out;
  int start = 0;
  String s = t;
  s.trim();
  while (start <= (int)s.length()) {
    int sp = s.indexOf(' ', start);
    if (sp < 0) sp = s.length();
    String w = s.substring(start, sp);
    start = sp + 1;
    if (w.length() == 0) continue;
    bool skip = false;
    for (const char *d : drop) if (w == d) { skip = true; break; }
    if (skip) continue;
    if (w == "uno") w = "1";
    else if (w == "due") w = "2";
    else if (w == "tre") w = "3";
    if (out.length()) out += ' ';
    out += w;
  }
  if (out.length() == 0 || out == "radio") query = fav;
  else query = out;
  return true;
}

// --- Stream HTTP/HTTPS ---------------------------------------------------------------------------------------------------
struct UrlParts {
  bool https = false;
  String host, path;
  uint16_t port = 80;
};

static bool parseUrl(const String &url, UrlParts &u) {
  int p;
  if (url.startsWith("http://")) { u.https = false; p = 7; u.port = 80; }
  else if (url.startsWith("https://")) { u.https = true; p = 8; u.port = 443; }
  else return false;
  int slash = url.indexOf('/', p);
  String hp = slash < 0 ? url.substring(p) : url.substring(p, slash);
  u.path = slash < 0 ? String("/") : url.substring(slash);
  int at = hp.lastIndexOf('@');
  if (at >= 0) hp = hp.substring(at + 1);  // credenziali nell'URL: ignorate
  int colon = hp.lastIndexOf(':');
  if (colon > 0 && hp.indexOf(']') < colon) {
    long port = hp.substring(colon + 1).toInt();
    if (port <= 0 || port > 65535) return false;
    u.port = (uint16_t)port;
    hp = hp.substring(0, colon);
  }
  u.host = hp;
  return u.host.length() > 0;
}

// Location relativa o assoluta
static String resolveUrl(const UrlParts &base, const String &loc) {
  if (loc.startsWith("http://") || loc.startsWith("https://")) return loc;
  String origin = String(base.https ? "https://" : "http://") + base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) origin += ":" + String(base.port);
  if (loc.startsWith("//")) return String(base.https ? "https:" : "http:") + loc;
  if (loc.startsWith("/")) return origin + loc;
  String dir = base.path;
  int q = dir.indexOf('?');
  if (q >= 0) dir = dir.substring(0, q);
  int ls = dir.lastIndexOf('/');
  dir = ls >= 0 ? dir.substring(0, ls + 1) : String("/");
  return origin + dir + loc;
}

class RadioStream {
 public:
  ~RadioStream() { close(); }
  void close() {
    if (c) c->stop();
    c = nullptr;
  }
  NetworkClient *client() { return c; }

  // Apre lo stream seguendo fino a 4 redirect (anche fra host e verso HTTPS).
  bool open(const String &startUrl, String &err, String &ctype) {
    String url = startUrl;
    for (int hop = 0; hop <= 4; hop++) {
      close();
      UrlParts u;
      if (!parseUrl(url, u)) { err = "URL non valido"; return false; }
      if (u.https) {
        tls.setInsecure();
        tls.setHandshakeTimeout(10);
        c = &tls;
      } else {
        c = &plain;
      }
      c->setTimeout(10000);
      if (!c->connect(u.host.c_str(), u.port, 8000)) { err = "connessione a " + u.host + " fallita"; close(); return false; }
      // HTTP/1.0 senza Icy-MetaData: niente metadati intercalati nell'audio
      String req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host + "\r\nUser-Agent: " USER_AGENT "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
      if (c->write((const uint8_t *)req.c_str(), req.length()) != req.length()) { err = "invio richiesta fallito"; close(); return false; }
      char line[512];
      if (!readLine(line, sizeof(line), 10000)) { err = "nessuna risposta dal server"; close(); return false; }
      int code = 0;
      if (!strncmp(line, "HTTP/", 5)) { const char *sp = strchr(line, ' '); code = sp ? atoi(sp + 1) : 0; }
      else if (!strncmp(line, "ICY ", 4)) code = atoi(line + 4);
      String location;
      ctype = "";
      for (int i = 0; i < 64; i++) {
        if (!readLine(line, sizeof(line), 10000)) { err = "intestazioni incomplete"; close(); return false; }
        if (!line[0]) break;
        if (!strncasecmp(line, "location:", 9)) { location = String(line + 9); location.trim(); }
        else if (!strncasecmp(line, "content-type:", 13)) { ctype = String(line + 13); ctype.trim(); ctype.toLowerCase(); }
      }
      if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && location.length()) {
        url = resolveUrl(u, location);
        LOGI("Radio: redirect %d -> %s", code, url.c_str());
        continue;
      }
      if (code != 200) { err = "risposta " + String(code); close(); return false; }
      return true;
    }
    err = "troppi redirect";
    close();
    return false;
  }

 private:
  NetworkClient plain;
  NetworkClientSecure tls;
  NetworkClient *c = nullptr;

  bool readLine(char *buf, size_t cap, uint32_t timeoutMs) {
    size_t n = 0;
    uint32_t t0 = millis();
    for (;;) {
      if (c->available() > 0) {
        int ch = c->read();
        if (ch < 0) continue;
        if (ch == '\n') break;
        if (ch != '\r' && n + 1 < cap) buf[n++] = (char)ch;
        t0 = millis();
      } else {
        if (!c->connected()) { buf[n] = 0; return n > 0; }
        if (millis() - t0 > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    buf[n] = 0;
    return true;
  }
};

static bool rateSupported(int r) {  // frequenze presenti nella tabella coefficienti es8311.c con MCLK 256·fs
  return r == 8000 || r == 11025 || r == 12000 || r == 16000 || r == 22050 || r == 24000 || r == 32000 || r == 44100 || r == 48000;
}

// Riproduce finché: interruzione, fine stream, errore. Ritorna false con err se non è mai partita.
static bool radioPlay(const Station &st, String &err) {
  const size_t IN_CAP = 8192;
  uint8_t *in = (uint8_t *)inAlloc(IN_CAP);
  if (!in) in = (uint8_t *)psAlloc(IN_CAP);
  int16_t *pcm = (int16_t *)inAlloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * sizeof(int16_t));
  HMP3Decoder dec = MP3InitDecoder();
  if (!in || !pcm || !dec) {
    err = "memoria insufficiente per il decoder";
    if (dec) MP3FreeDecoder(dec);
    heap_caps_free(in);
    heap_caps_free(pcm);
    return false;
  }
  RadioStream rs;
  String ctype;
  bool everPlayed = false;
  int reconnects = 0;
  uint32_t playStart = 0;
  int curRate = 0;  // frequenza dell'audio in coda: resta valida anche dopo una riconnessione
  for (;;) {
    if (!rs.open(st.url, err, ctype)) break;
    LOGI("Radio: %s (%s), %s", st.name.c_str(), ctype.c_str(), st.url.c_str());
    if (ctype.indexOf("mpegurl") >= 0 || ctype.indexOf("aac") >= 0 || ctype.indexOf("ogg") >= 0 || ctype.indexOf("flac") >= 0 || ctype.indexOf("text/html") >= 0) {
      err = "formato non supportato (" + ctype + ")";
      break;
    }
    int bytesLeft = 0, badFrames = 0;
    uint8_t *rp = in;
    uint32_t lastData = millis();
    bool streamEnded = false;
    NetworkClient *c = rs.client();
    while (!gInterrupt.load()) {
      loopServiceLight();
      // riempimento del buffer d'ingresso
      if (bytesLeft < (int)IN_CAP / 2 && c) {
        if (rp != in && bytesLeft > 0) memmove(in, rp, bytesLeft);
        rp = in;
        int av = c->available();
        if (av > 0) {
          int want = (int)IN_CAP - bytesLeft;
          if (av < want) want = av;
          int n = c->read(in + bytesLeft, want);
          if (n > 0) { bytesLeft += n; lastData = millis(); }
        } else if (!c->connected()) {
          streamEnded = true;
        } else if (millis() - lastData > 10000) {
          err = "nessun dato da 10 s";
          streamEnded = true;
        }
      }
      if (bytesLeft < 2048 && !streamEnded) {  // poco materiale: attende la rete
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      if (bytesLeft <= 0) break;
      int off = MP3FindSyncWord(rp, bytesLeft);
      if (off < 0) { bytesLeft = 0; continue; }
      rp += off;
      bytesLeft -= off;
      int left = bytesLeft;
      unsigned char *p = rp;
      int e = MP3Decode(dec, &p, &left, pcm, 0);
      if (e == ERR_MP3_INDATA_UNDERFLOW) {
        if (streamEnded) break;
        if (bytesLeft >= (int)IN_CAP / 2) {  // con 4 KB un frame intero c'è sempre: dati corrotti
          rp++;
          bytesLeft--;
          if (++badFrames > 300) { err = "lo stream non sembra MP3"; break; }
          continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      rp = p;
      bytesLeft = left;
      if (e == ERR_MP3_MAINDATA_UNDERFLOW) continue;  // normale all'avvio: servono i frame precedenti
      if (e != ERR_MP3_NONE) {
        if (bytesLeft > 0) { rp++; bytesLeft--; }
        if (++badFrames > 300) { err = "lo stream non sembra MP3"; break; }
        continue;
      }
      badFrames = 0;
      MP3FrameInfo fi;
      MP3GetLastFrameInfo(dec, &fi);
      if (fi.samprate != curRate) {
        if (!rateSupported(fi.samprate)) { err = "frequenza " + String(fi.samprate) + " Hz non supportata"; break; }
        if (curRate) {  // la coda contiene audio alla vecchia frequenza: lo lascia finire
          playEnd();    // senza fine-stream playWait arriverebbe sempre al timeout (underrun)
          playWait(3000);
        }
        playStop();
        if (!setAudioRate(fi.samprate)) { err = "cambio frequenza fallito"; break; }
        playBegin(fi.samprate / 2);  // prefill 0,5 s
        curRate = fi.samprate;
        if (!everPlayed) { playStart = millis(); LOGI("Radio: MP3 %d Hz, %d canali, %d kbps", fi.samprate, fi.nChans, fi.bitrate / 1000); }
        everPlayed = true;
      }
      bool ok;
      if (fi.nChans == 2) {
        // Il DAC dell'ES8311 è mono e prende solo il canale sinistro (REG09 SDP_IN_SEL = 0);
        // anche il MAX98357A è mono: downmix (L+R)/2, altrimenti il canale destro andrebbe perso.
        const int n = fi.outputSamps / 2;
        for (int i = 0; i < n; i++) pcm[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) >> 1);
        ok = ringPushMono(pcm, n, 4000);
      } else {
        ok = ringPushMono(pcm, fi.outputSamps, 4000);
      }
      if (!ok) {
        if (!gInterrupt.load()) err = "riproduzione bloccata";
        break;
      }
    }
    rs.close();
    if (gInterrupt.load() || !everPlayed) break;
    // stream caduto dopo aver suonato: riconnessione (max 3). Niente attesa: l'audio ancora in coda
    // continua a suonare mentre ci si riconnette; se la frequenza non cambia la coda non viene fermata.
    if (millis() - playStart > 20000 && reconnects < 3) {
      reconnects++;
      LOGW("Radio: stream interrotto (%s), riconnessione %d/3", err.c_str(), reconnects);
      err = "";
      MP3FreeDecoder(dec);  // decoder nuovo: il bit reservoir del vecchio stream darebbe un frame sporco
      dec = MP3InitDecoder();
      if (!dec) { err = "memoria insufficiente per il decoder"; break; }
      continue;
    }
    break;
  }
  if (!gInterrupt.load()) playEnd();
  if (dec) MP3FreeDecoder(dec);
  heap_caps_free(in);
  heap_caps_free(pcm);
  return everPlayed;
}
