#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Jarvis - case 3D "robottino a capsula"
per Waveshare ESP32-S3-Touch-AMOLED-1.8 (revisione V1 SH8601/FT3168 e V2 CO5300/CST820).

Script PARAMETRICO CadQuery: tutte le misure principali sono nella sezione PARAMETRI qui sotto.
Uso:
    python jarvis_case.py            -> genera STL (stl/), STEP assemblato, verifica mesh e interferenze
    python jarvis_case.py --veloce   -> come sopra ma con mesh STL piu' grossolane (prove rapide)

Sistema di riferimento (identico al modello STEP ufficiale Waveshare della scheda):
    origine = centro del vetro del display
    +X = destra guardando il display (lato USB-C e tasti), +Y = alto (lato fori del connettore a 15 pin)
    Z = 0 superficie del vetro, Z negativo verso il retro del robot.
Unita': millimetri.

Provenienza delle misure della scheda:
    modello STEP ufficiale Waveshare "ESP32-S3-Touch-AMOLED-1.8-3D.zip" (CREO, 2025-03-04, scheda V1),
    contorni del vetro e del PCB estratti per sezione del modello. La V2 ha lo stesso layout secondo le
    foto Waveshare, ma NON esiste uno STEP V2: le differenze meccaniche V1/V2 NON sono verificate.
    Le misure marcate "STIMATO" non sono nel modello STEP e vanno controllate sul pezzo reale
    (vedi LEGGIMI_case.md e il pezzo di prova 00_prova_misure).
"""
import math
import os
import sys
import json

import cadquery as cq

# =====================================================================================
#  PARAMETRI
# =====================================================================================

# ---------- Tolleranze di stampa ----------
GIOCO = 0.25            # gioco tra scheda/vetro e plastica (mm per lato)
GIOCO_INCASTRO = 0.15   # gioco tra due parti stampate che si incastrano (mm per lato)
SPESSORE = 2.0          # spessore delle pareti del corpo (>= 2 mm)

# ---------- Corpo a capsula (guscio anteriore + guscio posteriore) ----------
LARGHEZZA = 56.0        # larghezza del corpo (diametro delle due cupole)
Y_CUPOLA_SUP = 12.0     # centro della cupola superiore (rispetto al centro del vetro)
Y_CUPOLA_INF = -46.0    # centro della cupola inferiore
Z_FRONTE = 1.0          # superficie frontale del corpo: il vetro (z=0) resta 1 mm piu' indietro
Z_RETRO = -34.5         # superficie posteriore del corpo
R_FRONTE = 5.0          # raccordo dei bordi frontali
R_RETRO = 12.0          # raccordo dei bordi posteriori (schiena tonda)
Z_DIVISIONE = -22.0     # piano dove si separano guscio anteriore e posteriore
LINGUETTA_H = 2.5       # altezza del bordo di centraggio del guscio posteriore
LINGUETTA_S = 1.2       # spessore del bordo di centraggio

# ---------- Scheda Waveshare (dal modello STEP ufficiale) ----------
PCB_Z_FRONTE = -6.20    # faccia anteriore del PCB (6,20 mm sotto la superficie del vetro)
PCB_Z_RETRO = -7.40     # faccia posteriore del PCB (PCB spesso 1,2 mm)
FORI_M2 = [(-12.10, 17.97), (11.90, 17.97), (-12.10, -18.03), (11.90, -18.03)]  # fori M2 (Ø2,12)
FORO_M2_D = 2.12
USB_Y, USB_Z = 0.0, -6.18          # centro della presa USB-C (bordo destro)
USB_X_FACCIA = 17.40               # faccia della presa USB-C (sporge oltre il PCB)
USB_SPINA = (13.2, 7.4)            # foro per la spina USB-C (larghezza y, altezza z) - STIMATO:
                                   # va bene per spine con plastica fino a ~12,8 x 7 mm
TASTO_BOOT_Y, TASTO_PWR_Y = 10.5, -10.5   # pulsantini laterali (BOOT in alto, PWR in basso)
TASTO_Z = -5.3                     # centro in Z del pulsantino (2,0 x 1,0 mm)
TASTO_X_PUNTA = 16.70              # punta del pulsantino
TASTINO_Z_ASSE = TASTO_Z + 0.8     # asse della guida del tastino (la punta del tastino e' centrata sul pulsantino)
MIC_Y = -13.61                     # microfono MEMS sul retro del PCB (porta acustica STIMATA verso il retro)
MIC_FORO_Z = -10.0                 # quota del foro laterale del microfono (dentro il condotto del telaio)
ANTENNA = (-5.2, 2.5, -20.3, -18.5)  # antenna chip, bordo inferiore (x0, x1, y0, y1) - STIMATO; usata per
                                     # controllare la distanza delle parti metalliche (viti, batteria, altoparlante)

# ---------- Visiera scura (parte separata) ----------
VISIERA_BORDO_X = 4.45  # bordo scuro ai lati del vetro (visiera larga 43 mm)
VISIERA_BORDO_Y = 4.15  # bordo scuro sopra/sotto il vetro (visiera alta 50 mm)
VISIERA_R = 14.0        # raggio degli angoli della visiera (forma morbida da "casco")
VISIERA_Z_RETRO = -2.5  # faccia posteriore della flangia della visiera (qui premono i tubi del telaio)
SMUSSO_VETRO = 1.0      # smusso a 45 gradi attorno al vetro (il vetro e' 1 mm sotto la superficie)
VISIERA_SPALLA = 1.2    # spalla a 45 gradi + flangia dietro la parete: centra la visiera e la trattiene

# ---------- Telaio scheda (parte interna) ----------
TELAIO_Z = -11.4        # faccia anteriore della piastra del telaio (0,5 mm dietro il connettore batteria)
TELAIO_S = 1.6          # spessore della piastra
# Colonnine delle viti del telaio: calcolate attorno agli angoli della visiera (vedi colonnine())
COLONNINE_ANGOLI = (70.0, -65.5)   # direzione (gradi) dal centro dell'angolo della visiera: alto, basso
COLONNINA_D = 5.0
PERNO_D = 1.75          # perni di centraggio nei fori M2 del PCB
APPOGGIO_D = 3.8        # appoggi del PCB attorno ai fori
GANCIO_T = 0.9          # spessore dei ganci elastici che trattengono il PCB
GANCIO_SOVRAPP = 0.6    # quanto il dente del gancio copre il bordo del PCB

# ---------- Batteria LiPo (vano parametrico) ----------
# Scelta: 103040 = 10 x 30 x 40 mm, ~1200 mAh (formato molto comune). Alternative compatibili
# cambiando i 3 valori: 103035 (~1000 mAh), 803040 (~1000 mAh, 8 mm). Una 603450 (6x34x50) NON
# entra nel vano senza ridisegnarlo (50 mm di lunghezza).
BATT_S, BATT_L, BATT_A = 10.0, 30.0, 40.0   # spessore (z), larghezza (x), altezza (y)
BATT_CENTRO = (0.0, 10.0)                   # centro della batteria (x, y)
BATT_Z_FRONTE = -13.3                       # faccia anteriore della batteria (0,3 mm dietro il telaio)
BATT_GIOCO = 0.5

# Batteria in uso: quella fornita con la scheda Waveshare (spina MX1.25, ~350-400 mAh, polarita' gia'
# giusta). Va nell'inserto 12, che riempie il vano della 103040: per passare a una 103040 basta
# togliere l'inserto (e mettere USA_BATTERIA_WAVESHARE = False).
# Misure = formato consigliato da Waveshare per il suo case (3,85 x 24 x 28 mm) - STIMATO:
# misurare la batteria vera e correggere questi tre valori.
USA_BATTERIA_WAVESHARE = True
BATT_WS_S, BATT_WS_L, BATT_WS_A = 3.85, 24.0, 28.0   # spessore (z), larghezza (x), altezza (y)
BATT_WS_GIOCO = 0.4

# ---------- Altoparlanti ----------
ALTOP_D = 40.0          # upgrade: altoparlante 40 mm 4 ohm 3 W (es. Adafruit 3968: Ø40 x 20 mm)
ALTOP_H = 20.0
ALTOP_Y = -49.0         # centro dell'altoparlante 40 mm (pancia)
ALTOP_BORDO = 2.5       # spessore del bordo (flangia) dell'altoparlante - STIMATO
ANELLO_H = 2.5          # altezza dell'anello di fissaggio
ANELLO_D_INT = 35.0     # foro dell'anello: deve stare sulla flangia, fuori dal cestello - STIMATO
ALTOP_CESTELLO_D = 34.0 # diametro massimo del cestello/magnete dietro la flangia - STIMATO
GRIGLIA_R = 14.5        # raggio della zona fori della griglia frontale
GRIGLIA_FORO = 2.0
GRIGLIA_PASSO = 3.0
SPK_PICCOLO = (3.0, 12.0, 10.0)   # altoparlante di serie 12x10 mm, spessore 3 mm STIMATO (x, y, z)
SPK_PICCOLO_Y = 8.0               # centro della tasca sul fianco sinistro

# ---------- MAX98357A (upgrade, solo spazio riservato) ----------
MAX_DIM = (18.0, 20.0, 5.0)       # ingombro con fili saldati - STIMATO
MAX_CENTRO = (0.0, 10.0)

# ---------- Braccia e piedi ----------
Y_BRACCIO, Z_BRACCIO = -32.0, -12.0   # centro del perno della spalla
BRACCIO_MANO = (-52.0, -7.0)          # centro della "mano" (y, z): braccio leggermente in avanti
BRACCIO_R = 5.6                       # raggio della "mano"
BRACCIO_R_SPALLA = 4.4                # raggio alla spalla
BRACCIO_S = 7.5                       # spessore del braccio
PERNO_BRACCIO_D = 5.0                 # perno separato: stretto nel braccio, gira nel corpo (braccia in posa)
PERNO_NEL_BRACCIO = 5.5               # profondita' del foro cieco nel braccio
PERNO_NEL_CORPO = 6.0                 # quanto il perno entra nel fianco del corpo
FORO_PERNO_CORPO = 5.3                # foro nel corpo (gioco: il braccio ruota con attrito)
PIEDE_X = 12.5                        # posizione dei piedi (centro in x)
PIEDE_Z0, PIEDE_Z1 = 8.0, -45.0       # punta e tallone (lungo z): il tallone sporge dietro per non ribaltarsi al tocco
PIEDE_L = 16.0                        # larghezza del piede (x)
PIEDE_H = 9.0                         # altezza della "scarpa"
Y_SUOLO = -77.5                       # piano d'appoggio sul tavolo
PIOLI_PIEDE_Z = -26.0                 # due pioli affiancati per piede, nel guscio posteriore
PIOLI_PIEDE_DX = (-3.0, 3.0)          # posizione dei pioli rispetto al centro del piede (x)
PIOLO_PIEDE_D = 3.6

# ---------- Stampabilita' ----------
SMUSSO_STAMPA_RETRO = True   # fascia a 45 gradi dove il raccordo della schiena tocca il piatto (niente supporti)
SMUSSO_STAMPA_FRONTE = True  # idem sul bordo frontale (fascia di ~1,5 mm, quasi invisibile)

# ---------- Scritta sulla schiena ----------
TESTO_RETRO = "JARVIS"        # scritta incisa sulla schiena ("" = nessuna scritta)
TESTO_ALTEZZA = 6.0
TESTO_Y = -8.0
TESTO_PROFONDITA = 0.6

# ---------- Viti ----------
VITE_FORO_PILOTA = 1.6   # foro per vite autofilettante M2 (per plastica)
VITE_FORO_PASSANTE = 2.5
VITE_TESTA_D = 4.4
VITI_GUSCI = [(0.0, 35.0), (-23.75, Y_BRACCIO - 5.0), (23.75, Y_BRACCIO - 5.0)]  # 3 viti M2x8: testa + spalle
Z_TESTA_VITE_GUSCI = -24.2   # appoggio della testa delle viti dei gusci (dentro le torrette)

# ---------- Esportazione ----------
QUI = os.path.dirname(os.path.abspath(__file__))
DIR_STL = os.path.join(QUI, "stl")
TOLL_STL = 0.03
TOLL_ANG = 0.2

# =====================================================================================
#  DATI DELLA SCHEDA (contorni dal modello STEP ufficiale, semplificati a 0,015 mm)
# =====================================================================================
VETRO = [(15.226, -18.149), (15.815, -17.315), (16.123, -16.680), (16.312, -16.148), (16.659, -14.488), (16.800, -12.000), (16.800, 12.000), (16.742, 13.668), (16.641, 14.619), (16.555, 15.151), (16.193, 16.499), (16.123, 16.680), (15.661, 17.568), (15.226, 18.149), (14.880, 18.516), (13.994, 19.224), (13.868, 19.305), (12.636, 19.909), (12.485, 19.964), (11.109, 20.327), (10.680, 20.399), (9.289, 20.537), (8.531, 20.572), (6.000, 20.600), (-6.000, 20.600), (-7.778, 20.590), (-9.371, 20.532), (-10.680, 20.399), (-11.309, 20.288), (-12.485, 19.964), (-12.727, 19.874), (-13.661, 19.428), (-13.994, 19.224), (-14.494, 18.859), (-15.226, 18.149), (-15.815, 17.315), (-16.123, 16.680), (-16.312, 16.148), (-16.659, 14.488), (-16.800, 12.000), (-16.800, -12.000), (-16.742, -13.668), (-16.641, -14.619), (-16.555, -15.151), (-16.193, -16.499), (-16.123, -16.680), (-15.661, -17.568), (-15.226, -18.149), (-14.880, -18.516), (-13.994, -19.224), (-13.868, -19.305), (-12.636, -19.909), (-12.485, -19.964), (-11.109, -20.327), (-10.680, -20.399), (-9.289, -20.537), (-8.531, -20.572), (-6.000, -20.600), (6.000, -20.600), (7.778, -20.590), (9.371, -20.532), (10.680, -20.399), (11.309, -20.288), (12.485, -19.964), (12.727, -19.874), (13.661, -19.428), (13.994, -19.224), (14.494, -18.859)]
PCB = [(14.599, -18.318), (15.104, -17.742), (15.530, -17.105), (15.869, -16.418), (16.115, -15.692), (16.265, -14.940), (16.302, -14.128), (15.700, -13.526), (15.700, -7.526), (16.400, -6.826), (16.400, -4.477), (10.601, -4.477), (10.601, 4.523), (16.400, 4.523), (16.400, 6.774), (15.700, 7.474), (15.700, 13.474), (16.310, 14.076), (16.274, 14.723), (16.162, 15.379), (15.978, 16.019), (15.723, 16.634), (15.401, 17.217), (15.016, 17.760), (14.572, 18.256), (14.076, 18.700), (13.443, 19.128), (12.570, 19.541), (11.661, 19.866), (10.724, 20.101), (9.768, 20.243), (9.077, 20.274), (-6.807, 20.287), (-9.039, 20.235), (-9.888, 20.198), (-10.862, 20.070), (-11.821, 19.857), (-12.759, 19.562), (-13.185, 19.345), (-13.490, 19.096), (-13.740, 18.791), (-13.926, 18.444), (-14.040, 18.067), (-14.079, 17.674), (-14.040, 17.282), (-13.926, 16.905), (-13.740, 16.558), (-13.490, 16.253), (-13.185, 16.003), (-12.838, 15.818), (-12.461, 15.703), (-12.069, 15.674), (-6.750, 15.682), (-6.456, 15.653), (-6.173, 15.567), (-5.913, 15.428), (-5.684, 15.240), (-5.497, 15.012), (-5.357, 14.751), (-5.272, 14.469), (-5.250, 14.174), (-5.243, 2.174), (-5.272, 1.880), (-5.357, 1.598), (-5.497, 1.337), (-5.684, 1.109), (-5.913, 0.921), (-6.173, 0.782), (-6.456, 0.696), (-6.750, 0.674), (-15.600, 0.683), (-15.861, 0.649), (-16.104, 0.548), (-16.313, 0.388), (-16.474, 0.179), (-16.574, -0.065), (-16.600, -0.326), (-16.611, -12.379), (-16.567, -13.499), (-16.433, -14.629), (-16.211, -15.745), (-16.055, -16.316), (-15.675, -17.125), (-15.252, -17.759), (-14.749, -18.332), (-14.176, -18.835), (-13.543, -19.258), (-12.859, -19.595), (-12.137, -19.840), (-11.537, -19.981), (-10.314, -20.201), (-9.078, -20.334), (7.787, -20.338), (9.048, -20.293), (10.302, -20.158), (11.543, -19.934), (11.973, -19.834), (12.698, -19.588), (13.386, -19.249), (14.023, -18.823)]
PANNELLO = [(13.479, 17.801), (12.610, 18.346), (11.645, 18.700), (10.593, 18.884), (9.486, 18.941), (-9.756, 18.949), (-10.732, 18.875), (-11.382, 18.764), (-12.273, 18.499), (-13.032, 18.117), (-13.286, 17.951), (-14.099, 17.187), (-14.621, 16.423), (-14.809, 16.038), (-15.031, 15.409), (-15.245, 14.234), (-15.306, 12.444), (-15.306, -12.550), (-15.271, -14.044), (-15.157, -14.968), (-14.988, -15.656), (-14.530, -16.690), (-14.220, -17.155), (-13.844, -17.587), (-13.060, -18.208), (-12.161, -18.647), (-11.775, -18.772), (-10.463, -19.002), (-8.806, -19.050), (9.628, -19.041), (10.134, -19.025), (11.088, -18.923), (12.239, -18.610), (12.848, -18.327), (13.287, -18.052), (14.002, -17.410), (14.562, -16.629), (14.990, -15.615), (15.216, -14.534), (15.269, -13.427), (15.298, -12.550), (15.298, 12.444), (15.269, 13.849), (15.216, 14.429), (15.065, 15.241), (14.697, 16.267), (14.173, 17.101)]

# Ganci elastici che trattengono il PCB: (x, y) punto sul bordo del PCB, angolo della normale esterna
# (gradi), larghezza del gancio. Posizioni scelte dove il lato anteriore del PCB e' libero da componenti
# (verificato sul modello STEP): due sul bordo inferiore (fuori dalla zona antenna), uno sul lobo in alto
# a sinistra (attorno al foro M2) e uno sull'angolo in alto a destra.
GANCI = [(-8.5, -20.334, -90.0, 4.0), (5.0, -20.336, -90.0, 4.0),
         (-14.079, 17.674, 180.0, 2.6), (14.80, 18.00, 42.0, 3.6)]

# =====================================================================================
#  FUNZIONI DI BASE
# =====================================================================================

def scatola(x0, x1, y0, y1, z0, z1):
    """Parallelepipedo dati gli estremi (in qualsiasi ordine)."""
    xa, xb = sorted((x0, x1)); ya, yb = sorted((y0, y1)); za, zb = sorted((z0, z1))
    return cq.Workplane("XY").box(xb - xa, yb - ya, zb - za).translate(((xa + xb) / 2, (ya + yb) / 2, (za + zb) / 2))


def cil_z(x, y, d, z0, z1):
    za, zb = sorted((z0, z1))
    return cq.Workplane("XY").workplane(offset=za).center(x, y).circle(d / 2).extrude(zb - za)


def cil_x(y, z, d, x0, x1):
    xa, xb = sorted((x0, x1))
    return cq.Workplane("YZ").workplane(offset=xa).center(y, z).circle(d / 2).extrude(xb - xa)


def cil_y(x, z, d, y0, y1):
    ya, yb = sorted((y0, y1))
    # il piano XZ ha la normale verso -Y: costruisco lungo Z e ruoto
    c = cq.Workplane("XY").workplane(offset=0).center(x, -z).circle(d / 2).extrude(yb - ya)
    return c.rotate((0, 0, 0), (1, 0, 0), -90).translate((0, ya, 0))


def rett_arr(w, h, r, z0, z1, cx=0.0, cy=0.0):
    """Rettangolo arrotondato estruso tra z0 e z1."""
    za, zb = sorted((z0, z1))
    sk = cq.Sketch().rect(w, h).vertices().fillet(r)
    return cq.Workplane("XY").workplane(offset=za).center(cx, cy).placeSketch(sk).extrude(zb - za)


def poligono(pts, z0, z1, offset=0.0):
    """Poligono chiuso (contorno della scheda) estruso, eventualmente allargato di 'offset'."""
    za, zb = sorted((z0, z1))
    w = cq.Workplane("XY").workplane(offset=za).polyline(pts).close()
    if abs(offset) > 1e-6:
        w = w.offset2D(offset, kind="arc")
    return w.extrude(zb - za)


def stadio_2d(w, z0, z1, y0=Y_CUPOLA_INF, y1=Y_CUPOLA_SUP):
    """Profilo a capsula (stadio verticale) di larghezza w estruso tra z0 e z1."""
    za, zb = sorted((z0, z1))
    return (cq.Workplane("XY").workplane(offset=za).center(0, (y0 + y1) / 2)
            .slot2D((y1 - y0) + w, w, angle=90).extrude(zb - za))


def corpo(t=0.0):
    """Corpo a capsula PIENO, rientrato di t verso l'interno (t<0 = allargato).
    L'offset di una superficie raccordata e' ancora raccordata con raggio R-t: cosi' il guscio cavo
    e' esatto (spessore costante) senza usare la funzione shell di OpenCascade."""
    w = LARGHEZZA - 2 * t
    zf, zb = Z_FRONTE - t, Z_RETRO + t
    rf, rb = R_FRONTE - t, R_RETRO - t
    s = stadio_2d(w, zb, zf)
    if rf > 0.05:
        s = s.faces(">Z").edges().fillet(rf)
    if rb > 0.05:
        s = s.faces("<Z").edges().fillet(rb)
    return s


def semispazio_z(z, sopra=True, ampio=300.0):
    return scatola(-ampio, ampio, -ampio, ampio, z, z + ampio if sopra else z - ampio)


def raggio_elevazione(z, t=0.0):
    """Raggio della capsula vista di fronte alla quota z (tiene conto dei raccordi fronte/retro)."""
    r = LARGHEZZA / 2 - t
    zf, zb = Z_FRONTE - t, Z_RETRO + t
    rf, rb = R_FRONTE - t, R_RETRO - t
    if z > zf - rf:
        d = z - (zf - rf); return r - (rf - math.sqrt(max(rf * rf - d * d, 0.0)))
    if z < zb + rb:
        d = (zb + rb) - z; return r - (rb - math.sqrt(max(rb * rb - d * d, 0.0)))
    return r


def y_fondo(x, z, t=0.0):
    """Quota y della superficie inferiore del corpo nel punto (x, z)."""
    r = raggio_elevazione(z, t)
    return Y_CUPOLA_INF - math.sqrt(max(r * r - x * x, 0.0))


def unisci(lista):
    out = None
    for s in lista:
        if s is None:
            continue
        out = s if out is None else out.union(s)
    return out


def visiera_contorno(z0, z1, rientro=0.0):
    """Contorno visibile della visiera (rettangolo arrotondato attorno al vetro)."""
    mx, my = visiera_semiassi()
    return rett_arr(2 * (mx - rientro), 2 * (my - rientro), VISIERA_R - rientro, z0, z1)


def visiera_semiassi():
    mx = max(abs(p[0]) for p in VETRO) + GIOCO + VISIERA_BORDO_X
    my = max(abs(p[1]) for p in VETRO) + GIOCO + VISIERA_BORDO_Y
    return mx, my


def colonnine():
    """Le 4 colonnine stanno a 4 mm dal bordo della visiera, cosi' il tubo del telaio (Ø8) preme proprio
    sulla flangia della visiera e resta dentro la zona piana della parete frontale."""
    mx, my = visiera_semiassi()
    cx, cy = mx - VISIERA_R, my - VISIERA_R
    d = (VISIERA_R - GIOCO_INCASTRO) + 4.0
    out = []
    for ang in COLONNINE_ANGOLI:
        a = math.radians(ang)
        x, y = cx + d * math.cos(a), cy * (1 if ang > 0 else -1) + d * math.sin(a)
        out += [(-round(x, 2), round(y, 2)), (round(x, 2), round(y, 2))]
    return out


def visiera_spalla(z_alto, rientro, verso_basso=True):
    """Tronco di piramide a 45 gradi che parte dal contorno della visiera alla quota z_alto e si allarga
    di VISIERA_SPALLA andando verso il retro."""
    mx, my = visiera_semiassi()
    sk = cq.Sketch().rect(2 * (mx - rientro), 2 * (my - rientro)).vertices().fillet(VISIERA_R - rientro)
    wp = cq.Workplane("XY", origin=(0, 0, z_alto)).transformed(rotate=(180, 0, 0))
    return wp.placeSketch(sk).extrude(VISIERA_SPALLA, taper=-45)


def anello_stadio(w_est, w_int, z0, z1):
    return stadio_2d(w_est, z0, z1).cut(stadio_2d(w_int, z0 - 1, z1 + 1))


# =====================================================================================
#  PARTI
# =====================================================================================

def guscio_cavo():
    return corpo(0.0).cut(corpo(SPESSORE))


def ingombro_linguetta(extra=0.0):
    """Zona occupata dal bordo di centraggio del guscio posteriore (dentro il guscio anteriore)."""
    w_est = LARGHEZZA - 2 * SPESSORE
    w_int = LARGHEZZA - 2 * (SPESSORE + GIOCO_INCASTRO + LINGUETTA_S + extra)
    return anello_stadio(w_est + 0.02, w_int, Z_DIVISIONE - 0.01, Z_DIVISIONE + LINGUETTA_H + 0.3)


def guscio_anteriore():
    g = guscio_cavo().intersect(semispazio_z(Z_DIVISIONE, sopra=True))
    if SMUSSO_STAMPA_FRONTE:
        g = g.union(riempimento_smusso(Z_FRONTE, R_FRONTE, verso_alto=False))
    zi = Z_FRONTE - SPESSORE          # faccia interna della parete frontale (zona piana)
    z_in = zi + 0.5                   # le aggiunte partono dentro la parete per fondersi bene

    feat = []
    # colonnine per le viti del telaio scheda
    for (x, y) in colonnine():
        feat.append(cil_z(x, y, COLONNINA_D, z_in, TELAIO_Z + 0.3))
    # colonna per la vite che chiude i due gusci in alto (le altre due sono nei blocchi delle spalle)
    for (x, y) in VITI_GUSCI[:1]:
        feat.append(cil_z(x, y, 6.5, z_in, Z_DIVISIONE))
    # blocchi delle spalle (perno del braccio + vite di chiusura in basso)
    for s in (-1, 1):
        feat.append(scatola(s * 21.5, s * 30.0, Y_BRACCIO - 8.0, Y_BRACCIO + 6.0, z_in, Z_DIVISIONE))
    # blocco lato destro: guida della spina USB-C e dei due tastini
    feat.append(scatola(22.0, 30.0, -14.0, 14.0, z_in, -12.0))
    # tasca dell'altoparlante di serie (fianco sinistro, rivolto verso le feritoie)
    sx, sy, sz = SPK_PICCOLO
    y0, y1 = SPK_PICCOLO_Y - sy / 2 - 0.3, SPK_PICCOLO_Y + sy / 2 + 0.3
    x_int = -(LARGHEZZA / 2 - SPESSORE) + sx + 0.6          # faccia interna della tasca
    z_tasca0 = TASTO_Z + 1.5                                 # fondo della tasca (altoparlante da -3.8 in giu')
    tasca = unisci([
        scatola(-30.0, x_int + 1.2, y0 - 1.2, y0, z_in, z_tasca0 - sz - 1.0),
        scatola(-30.0, x_int + 1.2, y1, y1 + 1.2, z_in, z_tasca0 - sz - 1.0),
        scatola(x_int, x_int + 1.2, y0 - 1.2, y1 + 1.2, z_in, z_tasca0 - sz - 1.0),
        scatola(-30.0, x_int + 1.2, y0 - 1.2, y1 + 1.2, z_in, z_tasca0),
    ])
    tasca = tasca.cut(scatola(x_int - 0.5, x_int + 2.0, y0 + 2.0, y1 - 2.0, z_tasca0 - 2.0, z_tasca0 - sz + 1.5))
    feat.append(tasca)
    # collare dell'altoparlante 40 mm
    collare_h = ALTOP_BORDO + ANELLO_H
    feat.append(cil_z(0, ALTOP_Y, ALTOP_D + 0.6 + 2.4, z_in, zi - collare_h)
                .cut(cil_z(0, ALTOP_Y, ALTOP_D + 0.6, z_in + 1, zi - collare_h - 1)))

    feat = unisci(feat)
    feat = feat.cut(ingombro_linguetta(0.25))
    # spazio libero per la flangia della visiera (nessuna nervatura deve toccarla)
    feat = feat.cut(visiera_contorno(VISIERA_Z_RETRO - 0.3, zi + 0.01, rientro=-(VISIERA_SPALLA + 0.12)))
    feat = feat.intersect(corpo(0.01)).intersect(semispazio_z(Z_DIVISIONE, sopra=True))
    g = g.union(feat)

    tagli = []
    # apertura della visiera + smusso a 45 gradi sul bordo interno (sede della spalla della visiera)
    tagli.append(visiera_contorno(zi - 0.6, Z_FRONTE + 1.0))
    tagli.append(visiera_spalla(zi + VISIERA_SPALLA, 0.0).union(visiera_contorno(zi - 0.6, zi + 0.01, rientro=-VISIERA_SPALLA)))
    # griglia frontale dell'altoparlante 40 mm (fori a nido d'ape)
    punti = []
    passo_y = GRIGLIA_PASSO * math.sqrt(3) / 2
    n = int(GRIGLIA_R / passo_y) + 1
    for j in range(-n, n + 1):
        yy = j * passo_y
        off = (GRIGLIA_PASSO / 2) if (j % 2) else 0.0
        m = int(GRIGLIA_R / GRIGLIA_PASSO) + 2
        for i in range(-m, m + 1):
            xx = i * GRIGLIA_PASSO + off
            if math.hypot(xx, yy) <= GRIGLIA_R - GRIGLIA_FORO / 2:
                punti.append((xx, ALTOP_Y + yy))
    tagli.append(cq.Workplane("XY").workplane(offset=zi - 1.0).pushPoints(punti)
                 .circle(GRIGLIA_FORO / 2).extrude(SPESSORE + 3.0))
    # foro per la spina USB-C (asola) - attraversa parete e blocco
    w, h = USB_SPINA
    tagli.append(cq.Workplane("YZ").workplane(offset=21.0).center(USB_Y, USB_Z)
                 .slot2D(w, h, angle=0).extrude(10.0))
    # fori quadri dei tastini (BOOT e PWR)
    for ty in (TASTO_BOOT_Y, TASTO_PWR_Y):
        tagli.append(scatola(21.0, 31.0, ty - 1.7, ty + 1.7, TASTINO_Z_ASSE - 1.7, TASTINO_Z_ASSE + 1.7))
    # feritoie dell'altoparlante di serie (fianco sinistro)
    for k in range(5):
        yy = SPK_PICCOLO_Y - 4.0 + 2.0 * k
        tagli.append(scatola(-31.0, -LARGHEZZA / 2 + SPESSORE - 0.01, yy - 0.6, yy + 0.6, -5.0, -12.5))
    # foro del microfono (fianco sinistro, allineato al condotto del telaio)
    tagli.append(cil_x(MIC_Y, MIC_FORO_Z, 1.8, -31.0, -LARGHEZZA / 2 + SPESSORE - 0.01))
    # sedi dei perni delle braccia
    for s in (-1, 1):
        tagli.append(cil_x(Y_BRACCIO, Z_BRACCIO, FORO_PERNO_CORPO, s * 21.0, s * 31.0))
    # fori pilota: viti del telaio (nelle colonnine) e viti dei gusci
    for (x, y) in colonnine():
        tagli.append(cil_z(x, y, VITE_FORO_PILOTA, TELAIO_Z + 0.29, zi - 1.5))
    for (x, y) in VITI_GUSCI:
        tagli.append(cil_z(x, y, VITE_FORO_PILOTA, Z_DIVISIONE - 0.01, Z_DIVISIONE + 8.5))
    for t in tagli:
        g = g.cut(t)
    return g


def guscio_posteriore():
    g = guscio_cavo().intersect(semispazio_z(Z_DIVISIONE, sopra=False))
    if SMUSSO_STAMPA_RETRO:
        g = g.union(riempimento_smusso(Z_RETRO, R_RETRO, verso_alto=True))
    zi_r = Z_RETRO + SPESSORE          # faccia interna della schiena (zona piana)
    feat = []
    # bordo di centraggio (linguetta) + base che lo unisce alla parete
    w_est = LARGHEZZA - 2 * (SPESSORE + GIOCO_INCASTRO)
    w_int = LARGHEZZA - 2 * (SPESSORE + GIOCO_INCASTRO + LINGUETTA_S)
    feat.append(anello_stadio(w_est, w_int, Z_DIVISIONE - 0.01, Z_DIVISIONE + LINGUETTA_H))
    # base del bordo a 45 gradi (niente sbalzo orizzontale in stampa)
    base = stadio_2d(LARGHEZZA - 2 * SPESSORE + 0.5, Z_DIVISIONE - 3.0, Z_DIVISIONE)
    foro = (cq.Workplane("XY", origin=(0, (Y_CUPOLA_INF + Y_CUPOLA_SUP) / 2, Z_DIVISIONE + 0.01))
            .transformed(rotate=(180, 0, 0))
            .slot2D((Y_CUPOLA_SUP - Y_CUPOLA_INF) + w_int, w_int, angle=90).extrude(3.2, taper=-45))
    feat.append(base.cut(foro))
    # torrette delle viti
    for (x, y) in VITI_GUSCI:
        feat.append(cil_z(x, y, 7.5, Z_RETRO - 1.0, Z_DIVISIONE - 0.2))
    # vano batteria: pareti + appoggi posteriori
    bx, by = BATT_CENTRO
    lx, ly = BATT_L / 2 + BATT_GIOCO, BATT_A / 2 + BATT_GIOCO
    z_top = BATT_Z_FRONTE - 1.8
    z_app = BATT_Z_FRONTE - BATT_S - 0.3
    pareti = scatola(bx - lx - 1.2, bx + lx + 1.2, by - ly - 1.2, by + ly + 1.2, zi_r - 1.0, z_top).cut(
        scatola(bx - lx, bx + lx, by - ly, by + ly, zi_r - 2.0, z_top + 1.0))
    # finestra in basso per il cavo della batteria
    pareti = pareti.cut(scatola(bx - 12.0, bx - 2.0, by - ly - 2.0, by - ly + 0.5, z_top + 1, z_app + 0.5))
    feat.append(pareti)
    for xx in (bx - 13.0, bx + 13.0):
        feat.append(scatola(xx - 0.6, xx + 0.6, by - ly, by + ly, zi_r - 1.0, z_app))
    # angolari per la schedina MAX98357A (upgrade) dietro la batteria
    mx, my = MAX_CENTRO
    hx, hy = MAX_DIM[0] / 2 + 0.6, MAX_DIM[1] / 2 + 0.6
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = mx + sx * hx, my + sy * hy
            # squadretta a L attorno all'angolo della schedina (due barrette che si sovrappongono nell'angolo)
            feat.append(unisci([scatola(cx, cx + sx * 1.0, cy - sy * 4.0, cy + sy * 1.0, zi_r - 1.0, zi_r + 2.5),
                                scatola(cx - sx * 4.0, cx + sx * 1.0, cy, cy + sy * 1.0, zi_r - 1.0, zi_r + 2.5)]))
    # blocchi dei pioli dei piedi
    for s in (-1, 1):
        feat.append(scatola(s * PIEDE_X - 6.0, s * PIEDE_X + 6.0, -90.0, -60.0, Z_DIVISIONE - 0.5, Z_RETRO - 1.0))
    feat = unisci(feat)
    feat = feat.intersect(corpo(0.01))
    g = g.union(feat)
    tagli = []
    for (x, y) in VITI_GUSCI:
        tagli.append(cil_z(x, y, VITE_TESTA_D, Z_RETRO - 2.0, Z_TESTA_VITE_GUSCI))
        tagli.append(cil_z(x, y, VITE_FORO_PASSANTE, Z_TESTA_VITE_GUSCI - 0.1, Z_DIVISIONE + 1.0))
    for s in (-1, 1):
        for dx in PIOLI_PIEDE_DX:
            xx, zz = s * PIEDE_X + dx, PIOLI_PIEDE_Z
            yb = y_fondo(xx, zz)
            tagli.append(cil_y(xx, zz, PIOLO_PIEDE_D + 0.3, yb - 3.0, yb + 6.5))
    if TESTO_RETRO:
        # testo specchiato: si legge correttamente guardando il robot da dietro
        tx = (cq.Workplane("XY").workplane(offset=Z_RETRO - 1.0)
              .text(TESTO_RETRO, TESTO_ALTEZZA, 1.0 + TESTO_PROFONDITA, font="DejaVu Sans", kind="bold",
                    halign="center", valign="center")
              .mirror("YZ").translate((0, TESTO_Y, 0)))
        tagli.append(tx)
    for t in tagli:
        g = g.cut(t)
    return g


def riempimento_smusso(z_piatto, r, verso_alto=True, pieno=False, gioco=0.0):
    """Smusso di stampa: dove un raccordo di raggio r tocca il piatto, la parte piu' bassa del raccordo e'
    quasi orizzontale (sbalzo fino a 90 gradi). Si riempie lo spazio sotto il raccordo con una fascia a
    45 gradi tangente al raccordo: la forma cambia di pochissimo e il pezzo si stampa senza supporti.
    Restituisce il solido da UNIRE al guscio (gia' svuotato all'interno); con pieno=True restituisce il
    tronco intero allargato di 'gioco' (serve per scavare i piedi)."""
    s45 = math.sin(math.pi / 4)
    d = r * (2 * s45 - 1.0)                    # dove la retta a 45 gradi tocca il piatto (oltre la zona piana)
    h = r * (1.0 - s45)                        # altezza del punto di tangenza
    w0 = LARGHEZZA - 2 * (r - d) + 2 * gioco
    yc, L = (Y_CUPOLA_INF + Y_CUPOLA_SUP) / 2, (Y_CUPOLA_SUP - Y_CUPOLA_INF)
    wp = cq.Workplane("XY", origin=(0, yc, z_piatto))
    if not verso_alto:
        wp = wp.transformed(rotate=(180, 0, 0))
    tronco = wp.slot2D(L + w0, w0, angle=90).extrude(h, taper=-45)
    return tronco if pieno else tronco.cut(corpo(SPESSORE))


def visiera():
    """Visiera scura: tappo che riempie l'apertura della parete frontale + spalla a 45 gradi + flangia.
    Si inserisce DA DENTRO il guscio anteriore; il telaio scheda (con i suoi 4 tubi) la stringe contro
    la parete. Tutto il vetro resta scoperto (l'apertura e' il contorno del vetro + GIOCO)."""
    zi = Z_FRONTE - SPESSORE
    v = visiera_contorno(zi + VISIERA_SPALLA, Z_FRONTE, rientro=GIOCO_INCASTRO)
    try:
        v = v.faces(">Z").edges().fillet(0.6)
    except Exception:
        pass
    v = v.union(visiera_spalla(zi + VISIERA_SPALLA + 0.01, GIOCO_INCASTRO))
    v = v.union(visiera_contorno(VISIERA_Z_RETRO, zi + 0.01, rientro=GIOCO_INCASTRO - VISIERA_SPALLA))
    # apertura del vetro (tutto il vetro resta scoperto) + smusso a 45 gradi verso l'esterno
    v = v.cut(poligono(VETRO, VISIERA_Z_RETRO - 1.0, Z_FRONTE + 1.0, offset=GIOCO))
    smusso = (cq.Workplane("XY").workplane(offset=Z_FRONTE - SMUSSO_VETRO).polyline(VETRO).close()
              .offset2D(GIOCO, kind="arc").extrude(SMUSSO_VETRO + 0.5, taper=-45))
    v = v.cut(smusso)
    return v


def gancio(px, py, ang, larg, profondita_tasca=True):
    """Gancio elastico che si aggancia sopra il bordo anteriore del PCB. Restituisce (gancio, tasca)."""
    g, t = 0.15, GANCIO_T
    ov = GANCIO_SOVRAPP
    z_rad = TELAIO_Z - TELAIO_S + 0.8          # radice del gancio (dentro una tasca della piastra)
    z_dente0 = PCB_Z_FRONTE + 0.05
    z_dente1 = z_dente0 + 0.8
    z_punta = z_dente1 + (g + t + ov)          # rampa a 45 gradi per l'inserimento
    prof = [(g, z_rad), (g + t, z_rad), (g + t, z_punta), (-ov, z_dente1), (-ov, z_dente0), (g, z_dente0)]
    w = cq.Workplane("XZ").polyline(prof).close().extrude(larg / 2, both=True)
    tasca = scatola(g - 0.5, g + 8.0, -larg / 2 - 0.5, larg / 2 + 0.5, z_rad, TELAIO_Z + 0.01)
    w = w.rotate((0, 0, 0), (0, 0, 1), ang).translate((px, py, 0))
    tasca = tasca.rotate((0, 0, 0), (0, 0, 1), ang).translate((px, py, 0))
    return w, tasca


def appoggi_e_ganci():
    """Appoggi con perni nei 4 fori M2 + 4 ganci: tengono il PCB con ~0 gioco senza viti."""
    pezzi, tasche = [], []
    for (x, y) in FORI_M2:
        pezzi.append(cil_z(x, y, APPOGGIO_D, TELAIO_Z - 0.01, PCB_Z_RETRO))
        perno = (cq.Workplane("XY").workplane(offset=PCB_Z_RETRO).center(x, y).circle(PERNO_D / 2)
                 .extrude(0.6).faces(">Z").workplane().circle(PERNO_D / 2).workplane(offset=0.3)
                 .circle(PERNO_D / 2 - 0.3).loft())
        pezzi.append(perno)
    for (px, py, ang, larg) in GANCI:
        gg, tt = gancio(px, py, ang, larg)
        pezzi.append(gg); tasche.append(tt)
    return pezzi, tasche


def telaio():
    z0, z1 = TELAIO_Z, TELAIO_Z - TELAIO_S
    piastra = unisci([
        rett_arr(36.4, 44.8, 4.0, z1, z0, 0.0, 0.1),
        # alette verso le colonnine + ponti
        *[cil_z(x, y, 2 * 4.2, z1, z0) for (x, y) in colonnine()],
        *[scatola(x - 3.0, x + 3.0, y, 20.0 if y > 0 else -20.0, z1, z0) for (x, y) in colonnine()],
        # pavimento del condotto del microfono fino al fianco sinistro
        scatola(-LARGHEZZA / 2 + SPESSORE + 0.4, -16.0, MIC_Y - 2.5, MIC_Y + 2.5, z1, z0),
    ])
    # accesso alla spina della batteria (si innesta dal basso verso l'alto) + passaggio dei fili
    # verso il retro, a destra del condotto del microfono e sotto il bordo inferiore della batteria
    piastra = piastra.cut(scatola(-17.6, -6.8, -10.3, -2.8, z1 - 1, z0 + 1))
    piastra = piastra.cut(scatola(-10.9, -4.8, -15.6, -10.0, z1 - 1, z0 + 1))
    # feritoia per i fili verso il connettore a 15 pin (upgrade MAX98357A)
    piastra = piastra.cut(scatola(-10.0, 10.0, 18.6, 21.4, z1 - 1, z0 + 1))
    pezzi, tasche = appoggi_e_ganci()
    for t in tasche:
        piastra = piastra.cut(t)
    t = unisci([piastra] + pezzi)
    # tubi che premono la flangia della visiera contro la parete frontale
    for (x, y) in colonnine():
        t = t.union(cil_z(x, y, 8.0, z0 - 0.01, VISIERA_Z_RETRO))
        t = t.cut(cil_z(x, y, COLONNINA_D + 0.6, z0, VISIERA_Z_RETRO + 1))
        t = t.cut(cil_z(x, y, VITE_FORO_PASSANTE + 0.1, z1 - 1, z0 + 1))
    # condotto del microfono: due pareti + tetto fuori dal PCB, porta il suono dal foro laterale al MEMS
    x_parete = -LARGHEZZA / 2 + SPESSORE + 0.4
    cond = unisci([
        scatola(x_parete, -11.3, MIC_Y - 2.5, MIC_Y - 1.5, z0 - 0.01, -7.9),
        scatola(x_parete, -11.3, MIC_Y + 1.5, MIC_Y + 2.5, z0 - 0.01, -7.9),
        scatola(-11.9, -11.3, MIC_Y - 2.5, MIC_Y + 2.5, z0 - 0.01, -7.9),
        scatola(x_parete, -16.95, MIC_Y - 2.5, MIC_Y + 2.5, -8.7, -7.9),
    ])
    t = t.union(cond)
    return t



def tastino(y_tasto):
    """Tastino a pressione (stampare 2 pezzi). Profilo piatto: si stampa sdraiato senza supporti."""
    zb = TASTO_Z - 0.7                     # faccia inferiore (sul piatto di stampa)
    x_punta = TASTO_X_PUNTA + 0.15
    x_flangia0, x_flangia1 = 20.9, 21.9    # la flangia batte contro il blocco (x=22) e non fa uscire il tastino
    x_fuori = LARGHEZZA / 2 + 0.8          # sporge 0,8 mm dal fianco
    punta = scatola(x_punta, x_flangia0 + 0.01, y_tasto - 1.0, y_tasto + 1.0, zb, zb + 1.4)
    flangia = scatola(x_flangia0, x_flangia1, y_tasto - 2.6, y_tasto + 2.6, zb, zb + 3.0)
    asta = scatola(x_flangia1 - 0.01, x_fuori, y_tasto - 1.5, y_tasto + 1.5, zb, zb + 3.0)
    t = unisci([punta, flangia, asta])
    try:
        t = t.faces(">X").edges().chamfer(0.4)
    except Exception:
        pass
    return t


def anello_altoparlante():
    zi = Z_FRONTE - SPESSORE
    z0 = zi - ALTOP_BORDO
    d_est = ALTOP_D + 0.6 - 0.1
    a = cil_z(0, ALTOP_Y, d_est, z0, z0 - ANELLO_H).cut(cil_z(0, ALTOP_Y, ANELLO_D_INT, z0 + 1, z0 - ANELLO_H - 1))
    # 6 nervature schiacciabili per l'incastro a pressione nel collare
    for k in range(6):
        a_ = math.radians(k * 60 + 30)
        r = d_est / 2
        a = a.union(cil_z(r * math.cos(a_), ALTOP_Y + r * math.sin(a_), 0.8, z0 - 0.3, z0 - ANELLO_H))
    return a


def braccio(lato=1):
    """Braccio a "salsicciotto" (lato=+1 destro, -1 sinistro): capsula che si allarga verso la mano,
    faccia interna piatta (si stampa su quella), esterno tutto arrotondato. Nella faccia piatta c'e' un foro
    cieco per il perno separato (parte 11): il perno e' stretto nel braccio e gira nel corpo, cosi' il
    braccio si puo' mettere in posa."""
    x_in = LARGHEZZA / 2 + 0.15
    (ys, zs), (yh, zh) = (Y_BRACCIO, Z_BRACCIO), BRACCIO_MANO
    sk = (cq.Sketch().push([(ys, zs)]).circle(BRACCIO_R_SPALLA).push([(yh, zh)]).circle(BRACCIO_R)
          .reset().hull())
    b = cq.Workplane("YZ").workplane(offset=x_in).placeSketch(sk).extrude(BRACCIO_S)
    # raggio < raggio della spalla, altrimenti la faccia esterna sparisce e la mesh si rovina
    for r in (min(BRACCIO_R_SPALLA - 0.4, BRACCIO_S * 0.55), 3.0, 2.5):
        try:
            b = b.faces(">X").edges().fillet(r)
            break
        except Exception:
            continue
    b = b.cut(cil_x(ys, zs, PERNO_BRACCIO_D, x_in - 1.0, x_in + PERNO_NEL_BRACCIO))
    if lato < 0:
        b = b.mirror("YZ")
    return b


def perno_braccio(lato=1):
    """Perno separato del braccio (stampare 2 pezzi): profilo a D, si stampa sdraiato sulla parte piatta."""
    x_in = LARGHEZZA / 2 + 0.15
    x0, x1 = x_in - PERNO_NEL_CORPO, x_in + PERNO_NEL_BRACCIO - 0.3
    p = cil_x(Y_BRACCIO, Z_BRACCIO, PERNO_BRACCIO_D, x0, x1)
    p = p.cut(scatola(x0 - 1, x1 + 1, Y_BRACCIO - 5, Y_BRACCIO + 5, Z_BRACCIO - 5, Z_BRACCIO - PERNO_BRACCIO_D / 2 + 0.5))
    try:
        p = p.faces("<X or >X").chamfer(0.45)
    except Exception:
        pass
    if lato < 0:
        p = p.mirror("YZ")
    return p


def piede(lato=1):
    """Piede a scarpetta: suola piatta, bordi molto arrotondati; la parte alta e' scavata con la forma
    del corpo (il robot ci si appoggia dentro). Due pioli entrano nel guscio posteriore."""
    x = lato * PIEDE_X
    zc, L = (PIEDE_Z0 + PIEDE_Z1) / 2, abs(PIEDE_Z0 - PIEDE_Z1)
    s = (cq.Workplane("XY").center(x, -zc).slot2D(L, PIEDE_L, angle=90).extrude(PIEDE_H)
         .rotate((0, 0, 0), (1, 0, 0), -90).translate((0, Y_SUOLO, 0)))
    for r in (PIEDE_H * 0.55, 3.0, 2.0):
        try:
            s = s.faces(">Y").edges().fillet(r)
            break
        except Exception:
            continue
    p = s.cut(corpo(-GIOCO_INCASTRO))
    if SMUSSO_STAMPA_FRONTE:
        p = p.cut(riempimento_smusso(Z_FRONTE, R_FRONTE, verso_alto=False, pieno=True, gioco=0.3))
    if SMUSSO_STAMPA_RETRO:
        p = p.cut(riempimento_smusso(Z_RETRO, R_RETRO, verso_alto=True, pieno=True, gioco=0.3))
    for dx in PIOLI_PIEDE_DX:
        xx, zz = x + dx, PIOLI_PIEDE_Z
        yb = y_fondo(xx, zz)
        piolo = cil_y(xx, zz, PIOLO_PIEDE_D, min(yb, Y_SUOLO + PIEDE_H) - 2.0, yb + 5.5)
        piolo = piolo.faces(">Y").chamfer(0.4)
        p = p.union(piolo)
    return p


def prova_misure():
    """Pezzo di PROVA (stampa veloce): stesso appoggio/ganci del telaio + blocco laterale con il foro
    USB-C e le guide dei tastini nella posizione definitiva + anello per provare il vetro nella visiera."""
    z0, z1 = TELAIO_Z, TELAIO_Z - TELAIO_S
    piastra = unisci([rett_arr(36.4, 44.8, 4.0, z1, z0, 0.0, 0.1), scatola(17.0, 25.0, -14.0, 14.0, z1, z0)])
    piastra = piastra.cut(rett_arr(22.0, 26.0, 3.0, z1 - 1, z0 + 1, -2.0, 0.0))
    pezzi, tasche = appoggi_e_ganci()
    for t in tasche:
        piastra = piastra.cut(t)
    p = unisci([piastra] + pezzi)
    blocco = scatola(22.0, 25.0, -14.0, 14.0, z1, Z_FRONTE - SPESSORE)
    w, h = USB_SPINA
    blocco = blocco.cut(cq.Workplane("YZ").workplane(offset=21.0).center(USB_Y, USB_Z)
                        .slot2D(w, h, angle=0).extrude(10.0))
    for ty in (TASTO_BOOT_Y, TASTO_PWR_Y):
        blocco = blocco.cut(scatola(21.0, 31.0, ty - 1.7, ty + 1.7, TASTINO_Z_ASSE - 1.7, TASTINO_Z_ASSE + 1.7))
    p = p.union(blocco)
    # anello di prova del vetro (stessa apertura della visiera), unito alla piastra da una barretta
    dx = -47.0
    anello = poligono([(x + dx, y) for (x, y) in VETRO], z1, z1 + 1.6, offset=GIOCO + 2.5).cut(
        poligono([(x + dx, y) for (x, y) in VETRO], z1 - 1, z1 + 3, offset=GIOCO))
    barra = scatola(dx + 16.8 + GIOCO + 1.5, -17.0, -1.5, 1.5, z1, z1 + 1.2)
    return p.union(anello).union(barra)


def inserto_quote():
    """Ingombro dell'inserto nel vano della 103040 e posizione della tasca della batteria Waveshare."""
    bx, by = BATT_CENTRO
    lx = BATT_L / 2 + BATT_GIOCO - 0.3       # 0,3 mm di gioco per lato dentro il vano
    ly = BATT_A / 2 + BATT_GIOCO - 0.3
    z0 = BATT_Z_FRONTE - BATT_S - 0.2        # appoggia sulle due guide del guscio posteriore
    z1 = BATT_Z_FRONTE - 0.1                 # 0,4 mm dietro il telaio della scheda
    tx = BATT_WS_L / 2 + BATT_WS_GIOCO
    ty = BATT_WS_A / 2 + BATT_WS_GIOCO
    cy = by - ly + 2.0 + ty                  # tasca in basso: il cavo esce corto verso la finestra del vano
    return bx, by, lx, ly, z0, z1, tx, ty, cy


def inserto_batteria():
    """Inserto per la batteria Waveshare: riempie il vano della 103040, tasca aperta verso il telaio,
    canale per il cavo verso la finestra in basso e incavo per il dito per estrarre la batteria."""
    bx, by, lx, ly, z0, z1, tx, ty, cy = inserto_quote()
    blocco = scatola(bx - lx, bx + lx, by - ly, by + ly, z0, z1)
    try:
        blocco = blocco.edges("|Z").fillet(1.0)
    except Exception:
        pass
    prof = BATT_WS_S + 0.45
    tasca = scatola(bx - tx, bx + tx, cy - ty, cy + ty, z1 - prof, z1 + 1.0)
    canale = scatola(bx - 11.0, bx - 3.0, by - ly - 1.0, cy - ty + 0.5, z1 - 2.5, z1 + 1.0)
    dito = cil_z(bx, cy + ty, 12.0, z1 - prof, z1 + 1.0)
    return blocco.cut(tasca).cut(canale).cut(dito)


# ------------------------- oggetti di riferimento (NON da stampare) -------------------------

def rif_scheda():
    pcb = poligono(PCB, PCB_Z_RETRO, PCB_Z_FRONTE)
    for (x, y) in FORI_M2:
        pcb = pcb.cut(cil_z(x, y, FORO_M2_D, PCB_Z_RETRO - 1, PCB_Z_FRONTE + 1))
    pannello = poligono(PANNELLO, -2.52, -1.5)
    usb = scatola(10.2, USB_X_FACCIA, -4.47, 4.47, -7.76, -4.60)
    tasti = unisci([scatola(13.05, TASTO_X_PUNTA, s * 8.22, s * 12.78, -6.6, -4.4) for s in (1, -1)])
    sd = scatola(-15.6, -0.6, -15.7, 0.4, PCB_Z_FRONTE, -4.35)
    btb = scatola(-4.7, 4.7, 9.1, 13.95, -4.27, PCB_Z_FRONTE)
    conn_batt = scatola(-16.09, -8.44, -5.47, -0.27, -10.9, PCB_Z_RETRO)
    comp_retro = scatola(-7.3, -1.8, -12.0, -5.0, -9.4, PCB_Z_RETRO).union(
        scatola(0.97, 9.44, -14.03, 13.52, -8.4, PCB_Z_RETRO))
    mic = scatola(-15.54, -12.14, -14.91, -12.31, -8.5, PCB_Z_RETRO)
    return unisci([pcb, pannello, usb, tasti, sd, btb, conn_batt, comp_retro, mic])


def rif_vetro():
    return poligono(VETRO, -1.5, 0.0)


def rif_batteria():
    bx, by = BATT_CENTRO
    b = scatola(bx - BATT_L / 2, bx + BATT_L / 2, by - BATT_A / 2, by + BATT_A / 2,
                BATT_Z_FRONTE - BATT_S, BATT_Z_FRONTE)
    try:
        b = b.edges("|Z").fillet(1.5)
    except Exception:
        pass
    return b


def rif_batteria_waveshare():
    bx, by, lx, ly, z0, z1, tx, ty, cy = inserto_quote()
    zf = z1 - 0.3
    return scatola(bx - BATT_WS_L / 2, bx + BATT_WS_L / 2, cy - BATT_WS_A / 2, cy + BATT_WS_A / 2,
                   zf - BATT_WS_S, zf)


def rif_altoparlante40():
    """Ingombro dell'altoparlante 40 mm: flangia Ø40 + cestello/magnete (dimensioni STIMATE)."""
    zi = Z_FRONTE - SPESSORE
    return cil_z(0, ALTOP_Y, ALTOP_D, zi, zi - ALTOP_BORDO).union(
        cil_z(0, ALTOP_Y, ALTOP_CESTELLO_D, zi - ALTOP_BORDO + 0.01, zi - ALTOP_H))


def rif_altoparlante_piccolo():
    sx, sy, sz = SPK_PICCOLO
    x0 = -(LARGHEZZA / 2 - SPESSORE) + 0.3
    z0 = TASTO_Z + 1.5 - 0.2
    return scatola(x0, x0 + sx, SPK_PICCOLO_Y - sy / 2, SPK_PICCOLO_Y + sy / 2, z0, z0 - sz)


def rif_max98357():
    mx, my = MAX_CENTRO
    zi_r = Z_RETRO + SPESSORE
    return scatola(mx - MAX_DIM[0] / 2, mx + MAX_DIM[0] / 2, my - MAX_DIM[1] / 2, my + MAX_DIM[1] / 2,
                   zi_r + 0.2, zi_r + 0.2 + MAX_DIM[2])


# =====================================================================================
#  ORIENTAMENTO PER LA STAMPA ED ESPORTAZIONE
# =====================================================================================

def orienta(shape, rot):
    """rot = lista di (asse, gradi). Poi centra in XY e appoggia sul piano z=0."""
    s = shape
    for asse, gradi in rot:
        s = s.rotate((0, 0, 0), asse, gradi)
    bb = s.val().BoundingBox()
    return s.translate((-(bb.xmin + bb.xmax) / 2, -(bb.ymin + bb.ymax) / 2, -bb.zmin))


X_AX, Y_AX = (1, 0, 0), (0, 1, 0)

# nome file, funzione, rotazioni per la stampa, colore (per STEP/anteprime), quantita', note
PARTI = [
    ("00_prova_misure", prova_misure, [], "gray", 1, "pezzo di prova: stampare per primo"),
    ("01_guscio_anteriore", guscio_anteriore, [(X_AX, 180)], "white", 1, "faccia frontale sul piatto"),
    ("02_guscio_posteriore", guscio_posteriore, [], "white", 1, "schiena sul piatto"),
    ("03_visiera", visiera, [(X_AX, 180)], "black", 1, "faccia frontale sul piatto, filamento NERO"),
    ("04_telaio_scheda", telaio, [], "gray", 1, "piastra sul piatto"),
    ("05_braccio_destro", lambda: braccio(1), [(Y_AX, -90)], "white", 1, "faccia piatta interna sul piatto"),
    ("06_braccio_sinistro", lambda: braccio(-1), [(Y_AX, 90)], "white", 1, "faccia piatta interna sul piatto"),
    ("07_piede_destro", lambda: piede(1), [(X_AX, 90)], "white", 1, "suola sul piatto"),
    ("08_piede_sinistro", lambda: piede(-1), [(X_AX, 90)], "white", 1, "suola sul piatto"),
    ("09_tastino", lambda: tastino(TASTO_BOOT_Y), [], "white", 2, "sdraiato, stampare 2 pezzi"),
    ("10_anello_altoparlante40", anello_altoparlante, [], "gray", 1, "opzionale (upgrade)"),
    ("11_perno_braccio", lambda: perno_braccio(1), [], "white", 2, "sdraiato sulla parte piatta, stampare 2 pezzi"),
    ("12_inserto_batteria_waveshare", inserto_batteria, [], "gray", 1,
     "retro sul piatto, tasca verso l'alto (solo con la batteria Waveshare)"),
]

RIFERIMENTI = [
    ("rif_scheda", rif_scheda, "royalblue"),
    ("rif_vetro", rif_vetro, "black"),
    ("rif_batteria_103040", rif_batteria, "orange"),
    ("rif_batteria_waveshare", rif_batteria_waveshare, "orange"),
    ("rif_altoparlante40_ingombro", rif_altoparlante40, "goldenrod"),
    ("rif_altoparlante_di_serie", rif_altoparlante_piccolo, "goldenrod"),
    ("rif_max98357a_ingombro", rif_max98357, "purple"),
]


def costruisci_assemblato(parti):
    """Parti nella posizione di montaggio (il secondo tastino va sul PWR). Il pezzo di prova e' escluso."""
    ass = {k: v for k, v in parti.items() if not k.startswith("00_")}
    if not USA_BATTERIA_WAVESHARE:
        ass.pop("12_inserto_batteria_waveshare", None)
    ass["09_tastino_pwr"] = tastino(TASTO_PWR_Y)
    ass["11_perno_braccio_sx"] = perno_braccio(-1)
    return ass


def esporta_stl(shape, percorso, veloce=False):
    cq.exporters.export(shape, percorso, tolerance=(0.08 if veloce else TOLL_STL),
                        angularTolerance=(0.4 if veloce else TOLL_ANG))


def verifica_mesh(percorso):
    import trimesh
    m = trimesh.load(percorso, force="mesh")
    info = {"file": os.path.basename(percorso), "triangoli": int(len(m.faces)),
            "watertight": bool(m.is_watertight), "winding_ok": bool(m.is_winding_consistent),
            "volume_mm3": round(float(m.volume), 1),
            "bbox_mm": [round(float(v), 2) for v in (m.extents)],
            "corpi": int(len(m.split(only_watertight=False)))}
    try:
        import manifold3d
        mm = manifold3d.Manifold(manifold3d.Mesh(vert_properties=m.vertices.astype("float32"),
                                                 tri_verts=m.faces.astype("uint32")))
        info["manifold3d"] = str(mm.status()).split(".")[-1]
    except Exception as e:  # pragma: no cover
        info["manifold3d"] = f"non verificato ({e})"
    return info


def interferenze(ass, rif):
    """Volume di sovrapposizione tra coppie di parti montate (deve essere ~0)."""
    nomi = list(ass.keys()) + list(rif.keys())
    tutti = {**ass, **rif}
    # contatti VOLUTI: nervature schiacciabili dei perni delle braccia e dell'anello (incastro a pressione)
    escludi = {("rif_altoparlante40_ingombro", "rif_altoparlante_di_serie"),
               ("01_guscio_anteriore", "05_braccio_destro"), ("01_guscio_anteriore", "06_braccio_sinistro"),
               ("01_guscio_anteriore", "10_anello_altoparlante40"),
               # le due batterie sono alternative: l'inserto occupa il posto della 103040
               ("12_inserto_batteria_waveshare", "rif_batteria_103040")}
    # (il perno e' a misura nel foro cieco del braccio: contatto voluto, volume ~0)
    righe = []
    for i in range(len(nomi)):
        for j in range(i + 1, len(nomi)):
            a, b = nomi[i], nomi[j]
            if a.startswith("rif_") and b.startswith("rif_"):
                continue
            if (a, b) in escludi:
                continue
            sa, sb = tutti[a].val(), tutti[b].val()
            ba, bb = sa.BoundingBox(), sb.BoundingBox()
            if (ba.xmax < bb.xmin or bb.xmax < ba.xmin or ba.ymax < bb.ymin or bb.ymax < ba.ymin
                    or ba.zmax < bb.zmin or bb.zmax < ba.zmin):
                continue
            try:
                v = sa.intersect(sb).Volume()
            except Exception:
                v = float("nan")
            if not (v < 0.05):
                righe.append((a, b, round(v, 3)))
    return righe


def stabilita(ass, rif):
    """Stima di massa, baricentro e forza che fa ribaltare il robot all'indietro premendo il centro dello
    schermo (PLA 1,24 g/cm3 al ~80% di riempimento; masse dei componenti STIMATE)."""
    masse_rif = {"rif_scheda": 15.0, "rif_vetro": 5.0, "rif_altoparlante_di_serie": 1.5}
    if USA_BATTERIA_WAVESHARE:
        masse_rif["rif_batteria_waveshare"] = 8.0     # ~350-400 mAh, STIMATO
    else:
        masse_rif["rif_batteria_103040"] = 23.0
    extra = {"rif_altoparlante40_ingombro": 27.3, "rif_max98357a_ingombro": 2.0}
    def calcola(con_upgrade):
        M, mom = 0.0, [0.0, 0.0, 0.0]
        voci = [(n, s, s.val().Volume() / 1000 * 1.24 * 0.8) for n, s in ass.items()]
        if not con_upgrade:
            voci = [v for v in voci if v[0] != "10_anello_altoparlante40"]
        voci += [(n, rif[n], m) for n, m in masse_rif.items()]
        if con_upgrade:
            voci += [(n, rif[n], m) for n, m in extra.items()]
        for n, s, m in voci:
            c = s.val().Center()
            M += m; mom = [mom[0] + m * c.x, mom[1] + m * c.y, mom[2] + m * c.z]
        g = [v / M for v in mom]
        tallone = min(PIEDE_Z0, PIEDE_Z1)
        braccio_peso = g[2] - tallone            # distanza orizzontale baricentro -> tallone
        leva = 0.0 - Y_SUOLO                      # altezza del centro dello schermo dal tavolo
        F = M / 1000 * 9.81 * braccio_peso / leva
        return {"massa_g": round(M, 1), "baricentro_mm": [round(v, 1) for v in g],
                "forza_ribaltamento_N": round(F, 2), "equivale_a_grammi": round(F / 9.81 * 1000)}
    return {"base": calcola(False), "con_upgrade_altoparlante40": calcola(True)}


def distanze_antenna(rif):
    """Distanza minima (mm) fra la zona dell'antenna e le parti metalliche: viti, batteria, altoparlanti."""
    x0, x1, y0, y1 = ANTENNA
    ant = scatola(x0, x1, y0, y1, PCB_Z_RETRO - 1.1, PCB_Z_RETRO).val()
    viti = {}
    for i, (x, y) in enumerate(colonnine()):
        viti[f"vite_telaio_{i + 1}"] = cil_z(x, y, 2.0, TELAIO_Z - TELAIO_S, TELAIO_Z - TELAIO_S + 8.0)
    for i, (x, y) in enumerate(VITI_GUSCI):
        viti[f"vite_guscio_{i + 1}"] = cil_z(x, y, 2.0, Z_TESTA_VITE_GUSCI, Z_TESTA_VITE_GUSCI + 8.0)
    out = {k: round(ant.distance(v.val()), 1) for k, v in viti.items()}
    for k in ("rif_batteria_103040", "rif_batteria_waveshare", "rif_altoparlante40_ingombro",
              "rif_altoparlante_di_serie"):
        out[k] = round(ant.distance(rif[k].val()), 1)
    return out


def main():
    veloce = "--veloce" in sys.argv
    os.makedirs(DIR_STL, exist_ok=True)
    print("Costruzione delle parti ...")
    parti = {}
    for nome, f, rot, col, q, nota in PARTI:
        parti[nome] = f()
        print(f"  {nome}: ok")
    rif = {nome: f() for (nome, f, col) in RIFERIMENTI}

    # STL orientati per la stampa
    report = []
    for nome, f, rot, col, q, nota in PARTI:
        p = os.path.join(DIR_STL, nome + ".stl")
        esporta_stl(orienta(parti[nome], rot), p, veloce)
        info = verifica_mesh(p)
        info.update({"quantita": q, "colore": col, "orientamento": nota})
        report.append(info)
        print(f"  {info['file']}: watertight={info['watertight']} manifold={info['manifold3d']} "
              f"volume={info['volume_mm3']} mm3 bbox={info['bbox_mm']} corpi={info['corpi']}")

    # STL nella posizione di montaggio (per le anteprime) + STEP assemblato
    dir_mont = os.path.join(QUI, "stl", "montaggio")
    os.makedirs(dir_mont, exist_ok=True)
    ass = costruisci_assemblato(parti)
    colori = {n: c for (n, f, r, c, q, no) in PARTI}
    colori["09_tastino_pwr"] = "white"
    colori["11_perno_braccio_sx"] = "white"
    for nome, s in list(ass.items()) + list(rif.items()):
        cq.exporters.export(s, os.path.join(dir_mont, nome + ".stl"), tolerance=(0.08 if veloce else 0.04),
                            angularTolerance=(0.4 if veloce else 0.25))
    A = cq.Assembly(name="Jarvis")
    for nome, s in ass.items():
        A.add(s, name=nome, color=cq.Color(colori[nome]))
    for nome, f, col in RIFERIMENTI:
        alternativa = "rif_batteria_103040" if USA_BATTERIA_WAVESHARE else "rif_batteria_waveshare"
        if nome == alternativa:
            continue
        A.add(rif[nome], name=nome, color=cq.Color(col))
    A.save(os.path.join(QUI, "jarvis_assemblato.step"))
    print("  STEP assemblato: jarvis_assemblato.step")

    print("Controllo interferenze (parti montate) ...")
    righe = interferenze(ass, rif)
    for a, b, v in righe:
        print(f"  INTERFERENZA {a} <-> {b}: {v} mm3")
    if not righe:
        print("  nessuna interferenza")

    st = stabilita(ass, rif)
    print("Stabilita' (stima):", st)
    da = distanze_antenna(rif)
    print("Distanza dall'antenna (mm):", da)
    json.dump({"parti": report, "interferenze": righe, "stabilita": st, "distanze_antenna_mm": da},
              open(os.path.join(QUI, "stl", "verifica.json"), "w"), indent=1, ensure_ascii=False)
    print("Fatto.")


if __name__ == "__main__":
    main()
