#pragma once
// audio.h - I2S duplex (ES8311), coda di riproduzione SPSC in PSRAM, task play/feed/fetch,
// esp-sr AFE (wake word «Jarvis» + VAD), modello nella partizione "model".

#include <ESP_I2S.h>
#include <esp_partition.h>
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_wn_iface.h"
#include "esp_vad.h"
#include "model_path.h"
#include "board.h"
#include "settings.h"
#include "sr_model_jarvis.h"

#define SR_RATE 16000u
#define RING_FRAMES 144000u          // frame stereo: 3 s a 48 kHz, 9 s a 16 kHz (576 KB PSRAM)
#define PLAY_CHUNK 256u              // frame per scrittura I2S
#define END_SILENCE_MS 700
#define REC_MAX_MS 10000
#define REC_NOSPEECH_MS 5000
#define REC_DISCARD_MS 250           // eco del bip
#define REC_PREFIX 512u              // spazio per l'intestazione multipart (scritta allineata a destra)
#define REC_WAV_HDR 44u
#define REC_PCM_MAX ((uint32_t)REC_MAX_MS * SR_RATE / 1000u * 2u)  // 320000 byte
#define REC_TAIL_MAX 256u
#define REC_BUF_SIZE (REC_PREFIX + REC_WAV_HDR + REC_PCM_MAX + REC_TAIL_MAX)

// --- Stato I2S / codec -----------------------------------------------------------------
static I2SClass i2s(I2S_NUM_0);       // ES8311 (duplex)
static I2SClass *gAmp = nullptr;      // MAX98357A esterno (I2S_NUM_1, solo TX)
static bool gI2sOk = false;
static bool gCodecOk = false;
static std::atomic<uint32_t> gAudioRate{SR_RATE};  // scritto solo dal loop
static std::atomic<int32_t> gSoftGainQ15{32767};  // volume software (solo amplificatore esterno)

// Handshake per il cambio frequenza: i task impostano busy PRIMA di controllare paused.
static std::atomic<bool> i2sPaused{false};
static std::atomic<bool> feedBusy{false};
static std::atomic<bool> playBusy{false};
static std::atomic<bool> gMicEnabled{true};  // false durante la radio (LRCK comune ADC/DAC)

// --- Coda di riproduzione SPSC (produttore: loop; consumatore: play) ---------------------
// Posizioni in [0, 2*RING_FRAMES): pieno e vuoto si distinguono senza contatori extra.
static int16_t *gRing = nullptr;
static std::atomic<uint32_t> gRingWr{0};
static std::atomic<uint32_t> gRingRd{0};
static std::atomic<bool> gPlayEos{false};      // il produttore ha finito
static std::atomic<bool> gPlayFlush{false};    // richiesta di svuotamento (ack: torna false)
static std::atomic<bool> gPlayActive{false};   // il consumatore sta riproducendo
static std::atomic<uint32_t> gPlayPrefill{4800};
static std::atomic<uint32_t> gPlayedFrames{0}; // frame scritti su I2S (per i sottotitoli)
static std::atomic<uint8_t> gPlayLevel{0};     // livello audio 0-255 (bocca, equalizzatore)
static std::atomic<uint32_t> gUnderruns{0};

static inline uint32_t ringUsed() {
  uint32_t w = gRingWr.load(), r = gRingRd.load();
  return w >= r ? w - r : w + 2 * RING_FRAMES - r;
}
static inline uint32_t ringFree() { return RING_FRAMES - ringUsed(); }

// --- Registrazione / AFE -----------------------------------------------------------------------
enum AfeMode : uint32_t { AFE_IDLE = 0, AFE_WAKE = 1, AFE_RECORD = 2 };
enum RecState : int { REC_NONE = 0, REC_RUNNING, REC_OK, REC_NOSPEECH, REC_ABORTED };

static const esp_afe_sr_iface_t *afe = nullptr;
static esp_afe_sr_data_t *afeData = nullptr;
static int afeFeedChunk = 0, afeFetchChunk = 0;
static std::atomic<uint32_t> gAfeReq{0};        // (generazione << 4) | modo, scritto solo dal loop
static std::atomic<int> gRecState{REC_NONE};
static std::atomic<uint32_t> gRecPcmBytes{0};
static std::atomic<uint8_t> gMicLevel{0};
static std::atomic<int> gMicChannel{-1};        // -1 da decidere, 0 = L, 1 = R
static uint8_t *gRecBuf = nullptr;              // [prefisso multipart][WAV][PCM][chiusura]
static bool gAfeOk = false;

static inline void afeSetMode(AfeMode m) {
  uint32_t gen = (gAfeReq.load() >> 4) + 1;
  gAfeReq.store((gen << 4) | (uint32_t)m);
}

// --- Volume ------------------------------------------------------------------------------------
static void audioSetVolume(int vol) {
  vol = constrain(vol, 0, 100);
  if (gAmp) {
    // amplificatore esterno: -40..0 dB in software (niente guadagno oltre 0 dB)
    float g = vol == 0 ? 0.0f : powf(10.0f, (-40.0f + (vol - 1) * (40.0f / 99.0f)) / 20.0f);
    gSoftGainQ15.store((int32_t)(g * 32767.0f));
  } else if (gCodecOk) {
    codecSetVolume(vol);
  }
}

// --- Cambio frequenza (chiamato solo dal loop: TTS a 16 kHz, radio alla frequenza dello stream) ---
static bool setAudioRate(uint32_t rate) {
  if (!gI2sOk) return false;
  if (rate == gAudioRate) return true;
  i2sPaused.store(true);
  uint32_t t0 = millis();
  while ((feedBusy.load() || playBusy.load()) && millis() - t0 < 1500) vTaskDelay(1);
  if (feedBusy.load() || playBusy.load()) {
    LOGE("Cambio frequenza: task audio bloccati, annullato");
    i2sPaused.store(false);
    return false;
  }
  // Speaker integrato: DAC muto mentre MCLK/BCLK/LRCK si fermano e ripartono (il NS4150B può
  // essere ancora acceso, fino a 4 s dopo l'ultimo suono). La coda è vuota e l'I2S invia zeri
  // (auto_clear), quindi mute e unmute avvengono sul silenzio.
  const bool muteDac = gCodecOk && !gAmp;
  if (muteDac) codecMute(true);
  bool ok = i2s.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  // in duplex l'RX è slave del clock TX: riconfigurare entrambi, TX prima
  ok = ok && i2s.configureRX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!ok) {
    LOGW("configureTX/RX a %u Hz fallito (errore %d): riavvio I2S", (unsigned)rate, i2s.lastError());
    i2s.end();
    i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
    ok = i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
    i2s.setTimeout(200);
    if (!ok) {
      LOGE("I2S non riavviabile a %u Hz", (unsigned)rate);
      gI2sOk = false;
      if (muteDac) codecMute(false);
      i2sPaused.store(false);
      return false;
    }
  }
  gAudioRate = rate;
  if (gCodecOk && !codecSetRate(rate)) LOGE("ES8311: frequenza %u Hz non impostata", (unsigned)rate);
  if (gAmp && !gAmp->configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH))
    LOGE("Amplificatore esterno: frequenza %u Hz non impostata", (unsigned)rate);
  if (muteDac) {
    vTaskDelay(pdMS_TO_TICKS(10));  // clock e divisori del codec stabili prima di togliere il mute
    codecMute(false);
  }
  i2sPaused.store(false);
  LOGI("Audio a %u Hz", (unsigned)rate);
  return true;
}

// --- API del produttore (solo loop) ------------------------------------------------------------
// Svuota la coda e attende che il task play confermi.
static void playStop() {
  gPlayEos.store(true);
  gPlayFlush.store(true);
  uint32_t t0 = millis();
  while (gPlayFlush.load() && millis() - t0 < 1000) vTaskDelay(1);
  gPlayLevel.store(0);
}

// Inizio di una nuova riproduzione (coda vuota). Restituisce il contatore frame di partenza.
static uint32_t playBegin(uint32_t prefillFrames) {
  if (ringUsed() != 0) playStop();
  gPlayPrefill.store(prefillFrames);
  gPlayEos.store(false);
  return gPlayedFrames.load();
}

static void playEnd() { gPlayEos.store(true); }

static bool playIsDone() { return ringUsed() == 0 && !gPlayActive.load(); }

// Attende la fine della riproduzione (false se interrotta o scaduta)
static bool playWait(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!playIsDone()) {
    if (gInterrupt.load()) return false;
    if (millis() - t0 > timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// Scrive frame stereo nella coda; attende spazio. false se interrotta/scaduta.
static bool ringPush(const int16_t *stereo, uint32_t frames, uint32_t timeoutMs) {
  if (!gRing) return false;
  uint32_t t0 = millis();
  while (frames) {
    uint32_t fr = ringFree();
    if (fr == 0) {
      if (gInterrupt.load() || millis() - t0 > timeoutMs) return false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    uint32_t n = frames < fr ? frames : fr;
    uint32_t w = gRingWr.load();
    uint32_t idx = w % RING_FRAMES;
    uint32_t first = n < RING_FRAMES - idx ? n : RING_FRAMES - idx;
    memcpy(gRing + idx * 2, stereo, first * 4);
    if (n > first) memcpy(gRing, stereo + first * 2, (n - first) * 4);
    gRingWr.store((w + n) % (2 * RING_FRAMES));  // pubblicazione dopo la copia
    stereo += n * 2;
    frames -= n;
    t0 = millis();
  }
  return true;
}

// Mono -> stereo (L = R) a blocchi
static bool ringPushMono(const int16_t *mono, uint32_t n, uint32_t timeoutMs) {
  int16_t tmp[256 * 2];
  while (n) {
    uint32_t k = n > 256 ? 256 : n;
    for (uint32_t i = 0; i < k; i++) tmp[2 * i] = tmp[2 * i + 1] = mono[i];
    if (!ringPush(tmp, k, timeoutMs)) return false;
    mono += k;
    n -= k;
  }
  return true;
}

// Bip di ascolto (due toni brevi) alla frequenza corrente
static void playBeep(bool rising) {
  const uint32_t rate = gAudioRate;
  const int ms = 70;
  int16_t tmp[256 * 2];
  playBegin(0);
  for (int tone = 0; tone < 2; tone++) {
    float f = rising ? (tone ? 1318.5f : 987.8f) : (tone ? 784.0f : 1046.5f);
    uint32_t total = rate * ms / 1000, done = 0;
    float ph = 0, dph = 2.0f * (float)M_PI * f / rate;
    while (done < total) {
      uint32_t k = total - done > 256 ? 256 : total - done;
      for (uint32_t i = 0; i < k; i++) {
        uint32_t j = done + i;
        float env = 1.0f;
        uint32_t fade = rate / 200;  // 5 ms
        if (j < fade) env = (float)j / fade;
        else if (total - j < fade) env = (float)(total - j) / fade;
        int16_t v = (int16_t)(sinf(ph) * 9000.0f * env);
        ph += dph;
        if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
        tmp[2 * i] = tmp[2 * i + 1] = v;
      }
      if (!ringPush(tmp, k, 500)) { playEnd(); return; }
      done += k;
    }
  }
  playEnd();
}

// --- Task play (core 1, prio 6) -------------------------------------------------------------------
static void playTask(void *) {
  static int16_t buf[PLAY_CHUNK * 2];
  enum { PS_IDLE, PS_PREFILL, PS_PLAY } state = PS_IDLE;
  uint32_t idleSince = millis();
  bool paOn = false;
  uint32_t writeErr = 0;
  for (;;) {
    if (gPlayFlush.load()) {
      gRingRd.store(gRingWr.load());
      state = PS_IDLE;
      gPlayActive.store(false);
      gPlayLevel.store(0);
      idleSince = millis();
      gPlayFlush.store(false);
    }
    uint32_t used = ringUsed();
    if (state == PS_IDLE) {
      if (used == 0) {
        gPlayActive.store(false);
        if (paOn && millis() - idleSince > 4000) { paSet(false); paOn = false; }
        vTaskDelay(pdMS_TO_TICKS(4));
        continue;
      }
      gPlayActive.store(true);
      state = PS_PREFILL;
    }
    if (state == PS_PREFILL) {
      if (used < gPlayPrefill.load() && !gPlayEos.load()) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }
      if (!gAmp && gCodecOk && !paOn) {
        paSet(true);
        paOn = true;
        vTaskDelay(pdMS_TO_TICKS(15));  // avvio NS4150B
      }
      state = PS_PLAY;
    }
    if (used == 0) {
      if (gPlayEos.load()) { state = PS_IDLE; idleSince = millis(); gPlayLevel.store(0); }
      else { state = PS_PREFILL; gUnderruns.fetch_add(1); }
      continue;
    }
    uint32_t n = used < PLAY_CHUNK ? used : PLAY_CHUNK;
    uint32_t rd = gRingRd.load();
    uint32_t idx = rd % RING_FRAMES;
    uint32_t first = n < RING_FRAMES - idx ? n : RING_FRAMES - idx;
    memcpy(buf, gRing + idx * 2, first * 4);
    if (n > first) memcpy(buf + first * 2, gRing, (n - first) * 4);

    int32_t peak = 0;
    const int32_t g = gSoftGainQ15.load();
    for (uint32_t i = 0; i < n * 2; i++) {
      int32_t v = buf[i];
      if (gAmp) {
        v = (v * g) >> 15;
        buf[i] = (int16_t)v;
      }
      int32_t a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    // livello su scala logaritmica (circa -50..0 dBFS -> 0..255)
    int lvl = 0;
    if (peak > 100) lvl = (int)((20.0f * log10f(peak / 32768.0f) + 50.0f) * 5.1f);
    gPlayLevel.store((uint8_t)constrain(lvl, 0, 255));

    playBusy.store(true);
    if (i2sPaused.load()) {
      playBusy.store(false);
      vTaskDelay(1);
      continue;  // dati non consumati: riprova dopo il cambio frequenza
    }
    size_t wr = 0;
    if (gAmp) wr = gAmp->write((uint8_t *)buf, n * 4);
    else if (gI2sOk) wr = i2s.write((uint8_t *)buf, n * 4);
    playBusy.store(false);
    if (wr != n * 4) {
      if ((writeErr++ % 200) == 0) LOGW("I2S write: %u/%u byte", (unsigned)wr, (unsigned)(n * 4));
      vTaskDelay(1);
    }
    gRingRd.store((rd + n) % (2 * RING_FRAMES));
    gPlayedFrames.fetch_add(n);
  }
}

// --- Task feed (core 0, prio 5): I2S -> canale microfono -> AFE ---------------------------------
static void feedTask(void *) {
  const int chunk = afeFeedChunk;
  int16_t *stereo = (int16_t *)inAlloc(chunk * 2 * sizeof(int16_t));
  int16_t *mono = (int16_t *)inAlloc(chunk * sizeof(int16_t));
  if (!stereo || !mono) {
    LOGE("feed: memoria insufficiente per i buffer (%d campioni)", chunk);
    free(stereo);
    free(mono);
    vTaskDelete(nullptr);
    return;
  }
  uint64_t eL = 0, eR = 0;
  int detChunks = 0;
  const int detNeeded = (int)(2000 * SR_RATE / 1000 / chunk);  // ~2 s
  uint32_t readErr = 0;
  for (;;) {
    if (!gMicEnabled.load() || gAudioRate.load() != SR_RATE) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    feedBusy.store(true);
    if (i2sPaused.load()) {
      feedBusy.store(false);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t got = gI2sOk ? i2s.readBytes((char *)stereo, chunk * 4) : 0;
    feedBusy.store(false);
    if (got != (size_t)chunk * 4) {
      if ((readErr++ % 100) == 0) LOGW("I2S read: %u/%u byte", (unsigned)got, (unsigned)(chunk * 4));
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    int ch = gMicChannel.load();
    if (ch < 0) {
      // canale del microfono rilevato confrontando l'energia di L e R per ~2 s
      for (int i = 0; i < chunk; i++) {
        int32_t l = stereo[2 * i], r = stereo[2 * i + 1];
        eL += (uint64_t)(l * l);
        eR += (uint64_t)(r * r);
        mono[i] = (int16_t)((l + r) / 2);
      }
      if (++detChunks >= detNeeded) {
        ch = eR > eL ? 1 : 0;
        gMicChannel.store(ch);
        LOGI("Microfono sul canale %s (energia L=%llu R=%llu)", ch ? "R" : "L", (unsigned long long)eL, (unsigned long long)eR);
        if (eL == 0 && eR == 0) LOGW("Microfono muto: controllare ES8311/guadagno");
      }
    } else {
      for (int i = 0; i < chunk; i++) mono[i] = stereo[2 * i + ch];
    }
    if (afe && afeData) afe->feed(afeData, mono);
  }
}

// --- Task fetch (core 0, prio 5): risultati AFE, wake word, registrazione ----------------------
// UNICO task che chiama enable/disable_wakenet.
static void fetchTask(void *) {
  uint32_t lastGen = 0xFFFFFFFF;
  uint32_t mode = AFE_IDLE;
  bool wakeOn = true;  // stato iniziale dopo create_from_config
  uint32_t recMs = 0, speechMs = 0, silenceMs = 0, pcmBytes = 0;
  bool speechSeen = false;
  uint8_t *pcm = gRecBuf ? gRecBuf + REC_PREFIX + REC_WAV_HDR : nullptr;
  uint32_t lowRb = 0;

  for (;;) {
    uint32_t req = gAfeReq.load();
    if ((req >> 4) != lastGen) {
      lastGen = req >> 4;
      uint32_t want = req & 0x0F;
      if (mode == AFE_RECORD && gRecState.load() == REC_RUNNING) gRecState.store(REC_ABORTED);
      mode = want;
      if (mode == AFE_WAKE && !wakeOn) { afe->enable_wakenet(afeData); wakeOn = true; }
      if (mode != AFE_WAKE && wakeOn) { afe->disable_wakenet(afeData); wakeOn = false; }
      if (mode == AFE_RECORD) {
        recMs = speechMs = silenceMs = pcmBytes = 0;
        speechSeen = false;
        gRecPcmBytes.store(0);
        gRecState.store(pcm ? REC_RUNNING : REC_ABORTED);
        if (!pcm) mode = AFE_IDLE;
      }
    }
    if (!gMicEnabled.load()) {
      if (mode == AFE_RECORD && gRecState.load() == REC_RUNNING) gRecState.store(REC_ABORTED);
      if (mode == AFE_RECORD) mode = AFE_IDLE;
      gMicLevel.store(0);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    afe_fetch_result_t *res = afe->fetch_with_delay ? afe->fetch_with_delay(afeData, pdMS_TO_TICKS(100)) : afe->fetch(afeData);
    if (!res || res->ret_value == ESP_FAIL || !res->data) continue;

    int lvl = (int)((res->data_volume + 65.0f) * 5.1f);  // ~ -65..-15 dBFS -> 0..255
    gMicLevel.store((uint8_t)constrain(lvl, 0, 255));
    if (res->ringbuff_free_pct < 0.1f && (lowRb++ % 200) == 0) LOGW("AFE in ritardo (ring libero %.0f%%)", res->ringbuff_free_pct * 100);

    if (mode == AFE_WAKE && res->wakeup_state == WAKENET_DETECTED) {
      afe->disable_wakenet(afeData);  // niente doppi rilevamenti finché il loop non riarma
      wakeOn = false;
      mode = AFE_IDLE;
      LOGI("«Jarvis» rilevato (modello %d, parola %d)", res->wakenet_model_index, res->wake_word_index);
      if (!postEvent(EV_WAKE)) {
        // coda piena: il loop non riceverà l'evento e non riarmerebbe mai la wake word
        LOGW("Coda eventi piena: «Jarvis» ignorato");
        afe->enable_wakenet(afeData);
        wakeOn = true;
        mode = AFE_WAKE;
      }
      continue;
    }
    if (mode != AFE_RECORD) continue;

    const uint32_t bytes = (uint32_t)res->data_size;
    const uint32_t frameMs = bytes / 2 * 1000 / SR_RATE;
    recMs += frameMs;
    if (recMs <= REC_DISCARD_MS) {  // eco del bip
      // Il VAD gira a ogni fetch e torna "silenzio" solo dopo vad_min_noise_ms (300 ms) di rumore:
      // l'eco del bip lo lascerebbe in "parlato" oltre la finestra scartata. Alla fine della
      // finestra si azzera il trigger (vad_reset_trigger), così il primo frame tenuto parte da silenzio.
      if (recMs + frameMs > REC_DISCARD_MS && afe->reset_vad) afe->reset_vad(afeData);
      continue;
    }
    uint32_t room = REC_PCM_MAX - pcmBytes;
    uint32_t n = bytes < room ? bytes : room;
    if (n) {
      memcpy(pcm + pcmBytes, res->data, n);
      pcmBytes += n;
      gRecPcmBytes.store(pcmBytes);
    }
    if (res->vad_state == VAD_SPEECH) {
      speechMs += frameMs;
      silenceMs = 0;
      if (speechMs >= 96) speechSeen = true;
    } else {
      silenceMs += frameMs;
    }
    int result = REC_RUNNING;
    if (speechSeen && silenceMs >= END_SILENCE_MS) result = REC_OK;
    else if (pcmBytes >= REC_PCM_MAX) result = speechSeen ? REC_OK : REC_NOSPEECH;
    else if (!speechSeen && recMs >= REC_NOSPEECH_MS + REC_DISCARD_MS) result = REC_NOSPEECH;
    if (result != REC_RUNNING) {
      gRecPcmBytes.store(pcmBytes);
      gRecState.store(result);
      mode = AFE_IDLE;
      LOGI("Registrazione finita: %s, %u ms audio, parlato %u ms", result == REC_OK ? "ok" : "nessun parlato",
           (unsigned)(pcmBytes / 32), (unsigned)speechMs);
    }
  }
}

// --- Modello wake word nella partizione "model" ---------------------------------------------------
// Va scritto PRIMA del primo esp_srmodel_init (che mappa la partizione e mette in cache la lista).
static bool ensureModel() {
  const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (!p) { LOGE("Partizione 'model' assente: serve il partitions.csv dello sketch"); return false; }
  if (p->size < SR_MODEL_JARVIS_LEN) { LOGE("Partizione 'model' troppo piccola (%u)", (unsigned)p->size); return false; }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) { LOGE("ensureModel: memoria"); return false; }
  auto compare = [&]() -> bool {
    for (uint32_t off = 0; off < SR_MODEL_JARVIS_LEN; off += 4096) {
      uint32_t n = SR_MODEL_JARVIS_LEN - off < 4096 ? SR_MODEL_JARVIS_LEN - off : 4096;
      if (esp_partition_read(p, off, buf, n) != ESP_OK) return false;
      if (memcmp(buf, SR_MODEL_JARVIS + off, n) != 0) return false;
    }
    return true;
  };
  bool ok = compare();
  if (ok) {
    LOGI("Modello «Jarvis» già presente nella partizione (%u byte)", (unsigned)SR_MODEL_JARVIS_LEN);
  } else {
    LOGI("Scrittura del modello «Jarvis» nella partizione (%u byte)...", (unsigned)SR_MODEL_JARVIS_LEN);
    uint32_t eraseLen = (SR_MODEL_JARVIS_LEN + 4095) & ~4095u;
    ok = esp_partition_erase_range(p, 0, eraseLen) == ESP_OK;
    for (uint32_t off = 0; ok && off < SR_MODEL_JARVIS_LEN; off += 4096) {
      uint32_t n = SR_MODEL_JARVIS_LEN - off < 4096 ? SR_MODEL_JARVIS_LEN - off : 4096;
      memcpy(buf, SR_MODEL_JARVIS + off, n);  // copia in RAM interna: scrittura flash a blocchi da 4 KB
      ok = esp_partition_write(p, off, buf, n) == ESP_OK;
    }
    ok = ok && compare();
    LOGI("Modello %s", ok ? "scritto e verificato" : "NON scritto (errore flash)");
  }
  heap_caps_free(buf);
  return ok;
}

static bool afeInit(int wakeSens) {
  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models || models->num <= 0) { LOGE("esp-sr: nessun modello nella partizione"); return false; }
  for (int i = 0; i < models->num; i++) LOGI("esp-sr modello %d: %s", i, models->model_name[i]);
  afe_config_t *ac = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!ac) { LOGE("afe_config_init fallito"); return false; }
  ac->aec_init = false;
  ac->se_init = false;
  ac->ns_init = false;
  ac->vad_init = true;               // WebRTC VAD (nessun modello vadnet)
  ac->vad_min_noise_ms = 300;
  ac->wakenet_init = true;
  ac->wakenet_mode = wakeSens ? DET_MODE_95 : DET_MODE_90;
  ac->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  ac->afe_perferred_core = 0;
  ac->afe_perferred_priority = 5;
  if (!ac->wakenet_model_name) { LOGE("Modello wakenet non trovato"); afe_config_free(ac); return false; }
  LOGI("AFE: wakenet %s, modalità %s", ac->wakenet_model_name, wakeSens ? "DET_MODE_95 (sensibile)" : "DET_MODE_90");
  afe = esp_afe_handle_from_config(ac);
  if (!afe) { afe_config_free(ac); LOGE("esp_afe_handle_from_config fallito"); return false; }
  afeData = afe->create_from_config(ac);
  afe_config_free(ac);
  if (!afeData) { LOGE("AFE create_from_config fallito"); return false; }
  afeFeedChunk = afe->get_feed_chunksize(afeData);
  afeFetchChunk = afe->get_fetch_chunksize(afeData);
  int nch = afe->get_feed_channel_num(afeData);
  LOGI("AFE: feed %d campioni x %d canali, fetch %d campioni, %d Hz", afeFeedChunk, nch, afeFetchChunk, afe->get_samp_rate(afeData));
  if (nch != 1 || afeFeedChunk <= 0 || afeFeedChunk > 4096) { LOGE("AFE: formato inatteso"); return false; }
  return true;
}

// --- Inizializzazione audio (setup) ---------------------------------------------------------------
static bool audioInit(const Settings &s) {
  gRing = (int16_t *)psAlloc(RING_FRAMES * 4);
  gRecBuf = (uint8_t *)psAlloc(REC_BUF_SIZE);
  if (!gRing || !gRecBuf) LOGE("PSRAM: coda audio %s, buffer domanda %s", gRing ? "ok" : "NO", gRecBuf ? "ok" : "NO");

  pinMode(PIN_PA, OUTPUT);
  paSet(false);
  // Ordine come l'esempio Waveshare 15_ES8311: prima I2S (MCLK attivo), poi il codec.
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  gI2sOk = i2s.begin(I2S_MODE_STD, SR_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  i2s.setTimeout(200);
  gAudioRate = SR_RATE;
  if (!gI2sOk) LOGE("I2S: begin fallito");
  gCodecOk = gI2sOk && codecInit(SR_RATE, s.micGain);
  if (!gCodecOk) LOGE("ES8311: inizializzazione fallita");

  if (s.ampExt) {
    gAmp = new I2SClass(I2S_NUM_1);
    gAmp->setPins(s.ampBclk, s.ampLrc, s.ampDin, -1, -1);
    if (!gAmp->begin(I2S_MODE_STD, SR_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
      LOGE("Amplificatore esterno: I2S1 non avviato (BCLK %d, LRC %d, DIN %d): uso lo speaker integrato", s.ampBclk, s.ampLrc, s.ampDin);
      delete gAmp;
      gAmp = nullptr;
    } else {
      gAmp->setTimeout(200);
      LOGI("Amplificatore esterno su BCLK %d, LRC %d, DIN %d", s.ampBclk, s.ampLrc, s.ampDin);
    }
  }
  if (gCodecOk) codecMute(gAmp != nullptr);  // con l'esterno: DAC integrato muto e PA spento
  audioSetVolume(s.volume);
  return gI2sOk && gRing != nullptr;
}
