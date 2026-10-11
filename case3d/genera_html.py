#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Jarvis - genera anteprima_3d.html: visualizzatore 3D interattivo, autonomo (funziona da file://).
Gli STL "in posizione di montaggio" (stl/montaggio/*.stl) vengono incorporati in base64.
three.js 0.160.0 viene caricato da cdn.jsdelivr.net (serve la connessione internet solo per la libreria).

Uso:  python genera_html.py      (dopo jarvis_case.py)
"""
import base64
import json
import os

QUI = os.path.dirname(os.path.abspath(__file__))
DIR_M = os.path.join(QUI, "stl", "montaggio")
USCITA = os.path.join(QUI, "anteprima_3d.html")

# id file, nome mostrato, colore, gruppo, spostamento vista esplosa (mm), visibile all'avvio
PARTI = [
    ("01_guscio_anteriore", "01 Guscio anteriore", "#f3f2ee", "case", (0, 0, 52), True),
    ("02_guscio_posteriore", "02 Guscio posteriore", "#ecebe6", "case", (0, 0, -116), True),
    ("03_visiera", "03 Visiera (nera)", "#141416", "case", (0, 0, 100), True),
    ("04_telaio_scheda", "04 Telaio scheda", "#8e949d", "case", (0, 0, -38), True),
    ("05_braccio_destro", "05 Braccio destro", "#f3f2ee", "case", (26, 0, 52), True),
    ("06_braccio_sinistro", "06 Braccio sinistro", "#f3f2ee", "case", (-26, 0, 52), True),
    ("07_piede_destro", "07 Piede destro", "#e2e3e5", "case", (0, -30, -116), True),
    ("08_piede_sinistro", "08 Piede sinistro", "#e2e3e5", "case", (0, -30, -116), True),
    ("09_tastino", "09 Tastino BOOT", "#f3f2ee", "case", (34, 0, 52), True),
    ("09_tastino_pwr", "09 Tastino PWR", "#f3f2ee", "case", (34, 0, 52), True),
    ("10_anello_altoparlante40", "10 Anello altoparlante (opz.)", "#6f747b", "case", (0, -40, 30), True),
    ("11_perno_braccio", "11 Perno braccio destro", "#f3f2ee", "case", (14, 0, 52), True),
    ("11_perno_braccio_sx", "11 Perno braccio sinistro", "#f3f2ee", "case", (-14, 0, 52), True),
    ("rif_vetro", "Vetro del display", "#050506", "rif", (0, 0, 2), True),
    ("rif_scheda", "Scheda Waveshare (semplificata)", "#2f5fc4", "rif", (0, 0, 2), False),
    ("12_inserto_batteria_waveshare", "12 Inserto batteria Waveshare", "#8e949d", "case", (0, 0, -70), True),
    ("rif_batteria_waveshare", "Batteria Waveshare (con la scheda)", "#ec8c26", "rif", (0, 0, -54), False),
    ("rif_batteria_103040", "Batteria 103040 (alternativa, senza inserto)", "#ec8c26", "rif", (0, 0, -66), False),
    ("rif_altoparlante40_ingombro", "Altoparlante 40 mm (ingombro)", "#c9ab4f", "rif", (0, -40, 14), False),
    ("rif_altoparlante_di_serie", "Altoparlante di serie", "#c9ab4f", "rif", (-16, 0, 52), False),
    ("rif_max98357a_ingombro", "MAX98357A (ingombro)", "#8a4fa6", "rif", (0, 0, -86), False),
]

TEMPLATE = r"""<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Jarvis case</title>
<meta name="description" content="Visualizzatore 3D del case a capsula di Jarvis per Waveshare ESP32-S3-Touch-AMOLED-1.8">
<style>
  :root {
    --sfondo: #eef1f5; --pannello: #ffffff; --testo: #1d2229; --tenue: #5b6470; --bordo: #d7dce3;
    --accento: #1b8fb3; --accento-testo: #ffffff; --scena: #e6eaf0;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --sfondo: #15181d; --pannello: #1e232a; --testo: #e7ebf0; --tenue: #9aa4b1; --bordo: #323943;
      --accento: #3fc1e6; --accento-testo: #0d1014; --scena: #1a1e24;
    }
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; background: var(--sfondo); color: var(--testo);
    font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, "DejaVu Sans", sans-serif; }
  header { padding: 14px 16px 6px; }
  header h1 { margin: 0; font-size: 20px; font-weight: 650; letter-spacing: .2px; }
  header p { margin: 2px 0 0; color: var(--tenue); font-size: 13.5px; }
  main { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 12px; padding: 8px 16px 16px; }
  #scena { position: relative; min-height: 70vh; border-radius: 14px; overflow: hidden; background: var(--scena);
    border: 1px solid var(--bordo); }
  #scena canvas { display: block; width: 100%; height: 100%; touch-action: none; }
  #carica { position: absolute; inset: 0; display: grid; place-items: center; color: var(--tenue); font-size: 14px; }
  aside { background: var(--pannello); border: 1px solid var(--bordo); border-radius: 14px; padding: 12px;
    display: flex; flex-direction: column; gap: 10px; align-self: start; }
  aside h2 { font-size: 13px; text-transform: uppercase; letter-spacing: .6px; color: var(--tenue); margin: 4px 0 0; }
  .bottoni { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
  button { font: inherit; font-size: 13.5px; padding: 7px 8px; border-radius: 9px; border: 1px solid var(--bordo);
    background: var(--sfondo); color: var(--testo); cursor: pointer; }
  button[aria-pressed="true"] { background: var(--accento); color: var(--accento-testo); border-color: var(--accento); }
  button:focus-visible, input:focus-visible { outline: 2px solid var(--accento); outline-offset: 2px; }
  label.parte { display: flex; align-items: center; gap: 8px; font-size: 13.5px; cursor: pointer; padding: 1px 0; }
  .campione { width: 14px; height: 14px; border-radius: 4px; border: 1px solid var(--bordo); flex: none; }
  .riga { display: flex; align-items: center; gap: 8px; font-size: 13.5px; }
  input[type=range] { flex: 1; accent-color: var(--accento); }
  .nota { font-size: 12.5px; color: var(--tenue); margin: 0; }
  @media (max-width: 820px) {
    main { grid-template-columns: 1fr; }
    #scena { min-height: 62vh; }
  }
</style>
<script type="importmap">
{ "imports": {
    "three": "https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js",
    "three/addons/": "https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/"
} }
</script>
</head>
<body>
<header>
  <h1>Jarvis &mdash; case a capsula</h1>
  <p>Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1/V2) &middot; corpo 56 &times; 114 &times; 35,5 mm, alto 118 mm con i piedi &middot; trascina per ruotare, rotella per lo zoom, tasto destro per spostare</p>
</header>
<main>
  <div id="scena"><div id="carica">Caricamento di three.js e dei modelli&hellip;</div></div>
  <aside>
    <div class="bottoni">
      <button id="b-esplosa" aria-pressed="false">Vista esplosa</button>
      <button id="b-occhi" aria-pressed="true">Occhi accesi</button>
      <button id="b-interni" aria-pressed="false">Componenti</button>
      <button id="b-reset">Vista iniziale</button>
    </div>
    <div class="riga"><label for="taglio">Sezione</label><input id="taglio" type="range" min="-30" max="30" step="0.5" value="30" aria-label="Piano di sezione (asse X)"></div>
    <h2>Parti</h2>
    <div id="lista"></div>
    <p class="nota">Gli oggetti &laquo;Componenti&raquo; (scheda, batteria, altoparlanti, MAX98357A) sono solo ingombri di riferimento e non si stampano. Le parti 10 e l&rsquo;altoparlante 40 mm sono per l&rsquo;upgrade.</p>
  </aside>
</main>
<script>window.PARTI = __DATI__;</script>
<script type="module">
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { STLLoader } from 'three/addons/loaders/STLLoader.js';
import { toCreasedNormals } from 'three/addons/utils/BufferGeometryUtils.js';

const contenitore = document.getElementById('scena');
const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.localClippingEnabled = true;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 0.92;
contenitore.appendChild(renderer.domElement);
document.getElementById('carica').remove();

const scena = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(30, 1, 1, 3000);
const BERSAGLIO = new THREE.Vector3(0, -19, -15);
const POS_INIZIALE = new THREE.Vector3(150, 62, 235);
camera.position.copy(POS_INIZIALE);
const controlli = new OrbitControls(camera, renderer.domElement);
controlli.target.copy(BERSAGLIO);
controlli.enableDamping = true;
controlli.update();

scena.add(new THREE.HemisphereLight(0xffffff, 0x7d8592, 1.05));
const luce1 = new THREE.DirectionalLight(0xffffff, 2.4); luce1.position.set(170, 240, 200); scena.add(luce1);
const luce2 = new THREE.DirectionalLight(0xd6e2ff, 0.7); luce2.position.set(-230, 60, -60); scena.add(luce2);
const luce3 = new THREE.DirectionalLight(0xffffff, 0.5); luce3.position.set(-60, -40, 260); scena.add(luce3);

// ombra morbida sul tavolo
{
  const c = document.createElement('canvas'); c.width = c.height = 256;
  const g = c.getContext('2d'); const gr = g.createRadialGradient(128, 128, 10, 128, 128, 128);
  gr.addColorStop(0, 'rgba(0,0,0,0.38)'); gr.addColorStop(1, 'rgba(0,0,0,0)');
  g.fillStyle = gr; g.fillRect(0, 0, 256, 256);
  const piano = new THREE.Mesh(new THREE.PlaneGeometry(120, 110),
    new THREE.MeshBasicMaterial({ map: new THREE.CanvasTexture(c), transparent: true, depthWrite: false }));
  piano.rotation.x = -Math.PI / 2; piano.position.set(0, -77.45, -18);
  scena.add(piano);
}

const piano_taglio = new THREE.Plane(new THREE.Vector3(-1, 0, 0), 30);
const loader = new STLLoader();
const oggetti = [];
function b64(s) { const bin = atob(s); const u = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i); return u.buffer; }

const lista = document.getElementById('lista');
for (const p of window.PARTI) {
  let geo = loader.parse(b64(p.stl));
  geo = toCreasedNormals(geo, THREE.MathUtils.degToRad(32));
  const lucida = p.id === '03_visiera' || p.id === 'rif_vetro';
  const mat = new THREE.MeshStandardMaterial({ color: p.colore, roughness: lucida ? 0.22 : 0.5,
    metalness: 0.0, clippingPlanes: [piano_taglio], clipShadows: true, side: THREE.DoubleSide });
  const m = new THREE.Mesh(geo, mat);
  m.userData = { esplosa: new THREE.Vector3(...p.esplosa), gruppo: p.gruppo };
  m.visible = p.visibile;
  scena.add(m);
  oggetti.push({ p, m });
  const etichetta = document.createElement('label'); etichetta.className = 'parte';
  etichetta.dataset.gruppo = p.gruppo;
  const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = p.visibile;
  cb.addEventListener('change', () => { m.visible = cb.checked; });
  const cam = document.createElement('span'); cam.className = 'campione'; cam.style.background = p.colore;
  etichetta.append(cb, cam, document.createTextNode(p.nome));
  lista.appendChild(etichetta);
  p.cb = cb;
}

// occhi luminosi disegnati sul vetro (come nel firmware: 100x124 px, distanza 44 px, schermo 368x448)
const tela = document.createElement('canvas'); tela.width = 368; tela.height = 448;
const ctx = tela.getContext('2d');
const tex = new THREE.CanvasTexture(tela); tex.colorSpace = THREE.SRGBColorSpace;
const occhi = new THREE.Mesh(new THREE.PlaneGeometry(28.7, 34.94),
  new THREE.MeshBasicMaterial({ map: tex, transparent: true, blending: THREE.AdditiveBlending, depthWrite: false,
                                clippingPlanes: [piano_taglio] }));
occhi.position.set(0, 0.49, 0.05);
scena.add(occhi);
let palpebra = 1, sguardo = 0, sguardoMeta = 0, prossimoBattito = 2, prossimoSguardo = 1.5;
function rr(x, y, w, h, r) { ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r); ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath(); }
function disegnaOcchi() {
  ctx.clearRect(0, 0, 368, 448);
  const h = Math.max(6, 124 * palpebra), w = 100;
  for (const s of [-1, 1]) {
    const cx = 184 + s * (22 + 50) + sguardo, cy = 205;
    ctx.save(); ctx.shadowColor = 'rgba(40,200,255,0.95)'; ctx.shadowBlur = 38;
    ctx.fillStyle = '#c9f6ff'; rr(cx - w / 2, cy - h / 2, w, h, 30); ctx.fill(); ctx.restore();
  }
  tex.needsUpdate = true;
}

const centro = new THREE.Vector3();
function adatta() {
  const w = contenitore.clientWidth, h = contenitore.clientHeight;
  renderer.setSize(w, h, false); camera.aspect = w / h; camera.updateProjectionMatrix();
}
new ResizeObserver(adatta).observe(contenitore);
adatta();

let esplosa = 0, esplosaMeta = 0;
const bEsplosa = document.getElementById('b-esplosa');
// la camera si allontana (o torna) quando si apre/chiude la vista esplosa
let volo = 0; const camDa = new THREE.Vector3(), camA = new THREE.Vector3(), tgDa = new THREE.Vector3(), tgA = new THREE.Vector3();
function vola(pos, tg) { camDa.copy(camera.position); tgDa.copy(controlli.target); camA.copy(pos); tgA.copy(tg); volo = 1e-6; }
bEsplosa.addEventListener('click', () => {
  esplosaMeta = esplosaMeta ? 0 : 1; bEsplosa.setAttribute('aria-pressed', String(!!esplosaMeta));
  if (esplosaMeta) vola(new THREE.Vector3(360, 200, 330), new THREE.Vector3(0, -24, -12));
  else vola(POS_INIZIALE, BERSAGLIO);
});
const bOcchi = document.getElementById('b-occhi');
bOcchi.addEventListener('click', () => { occhi.visible = !occhi.visible; bOcchi.setAttribute('aria-pressed', String(occhi.visible)); });
const bInterni = document.getElementById('b-interni');
bInterni.addEventListener('click', () => {
  const on = bInterni.getAttribute('aria-pressed') !== 'true';
  bInterni.setAttribute('aria-pressed', String(on));
  for (const { p, m } of oggetti) if (p.gruppo === 'rif' && p.id !== 'rif_vetro') { m.visible = on; p.cb.checked = on; }
});
document.getElementById('b-reset').addEventListener('click', () => { camera.position.copy(POS_INIZIALE); controlli.target.copy(BERSAGLIO); controlli.update(); });
document.getElementById('taglio').addEventListener('input', e => { piano_taglio.constant = parseFloat(e.target.value); });

// opzioni dall'indirizzo: #esplosa #componenti #sezione #spento (utili per condividere una vista)
{
  const h = location.hash.toLowerCase();
  if (h.includes('esplosa')) bEsplosa.click();
  if (h.includes('componenti')) bInterni.click();
  if (h.includes('spento')) bOcchi.click();
  if (h.includes('sezione')) { const r = document.getElementById('taglio'); r.value = -3; r.dispatchEvent(new Event('input')); }
}
const orologio = new THREE.Clock();
function ciclo() {
  const dt = Math.min(orologio.getDelta(), 0.05), t = orologio.elapsedTime;
  esplosa += (esplosaMeta - esplosa) * Math.min(1, dt * 5);
  if (volo > 0) {
    volo = Math.min(1, volo + dt * 1.6); const k = volo * volo * (3 - 2 * volo);
    camera.position.lerpVectors(camDa, camA, k); controlli.target.lerpVectors(tgDa, tgA, k);
    if (volo >= 1) volo = 0;
  }
  for (const { m } of oggetti) m.position.copy(m.userData.esplosa).multiplyScalar(esplosa);
  occhi.position.set(0, 0.49, 0.05 + 2 * esplosa);
  // battito di ciglia e sguardi casuali
  if (t > prossimoBattito) { prossimoBattito = t + 2.5 + Math.random() * 3; }
  const fase = prossimoBattito - t; palpebra = fase < 0.16 ? Math.abs(fase - 0.08) / 0.08 : 1;
  if (t > prossimoSguardo) { sguardoMeta = (Math.random() - 0.5) * 50; prossimoSguardo = t + 1.5 + Math.random() * 2.5; }
  sguardo += (sguardoMeta - sguardo) * Math.min(1, dt * 6);
  disegnaOcchi();
  controlli.update();
  renderer.render(scena, camera);
  requestAnimationFrame(ciclo);
}
ciclo();
</script>
</body>
</html>
"""


def main():
    dati = []
    for (pid, nome, col, gruppo, esp, vis) in PARTI:
        p = os.path.join(DIR_M, pid + ".stl")
        if not os.path.exists(p):
            print("manca", p)
            continue
        dati.append({"id": pid, "nome": nome, "colore": col, "gruppo": gruppo, "esplosa": list(esp),
                     "visibile": vis, "stl": base64.b64encode(open(p, "rb").read()).decode("ascii")})
    html = TEMPLATE.replace("__DATI__", json.dumps(dati, ensure_ascii=False))
    open(USCITA, "w", encoding="utf-8").write(html)
    print(f"scritto {USCITA} ({os.path.getsize(USCITA) / 1e6:.2f} MB, {len(dati)} oggetti)")


if __name__ == "__main__":
    main()
