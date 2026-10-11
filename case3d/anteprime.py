#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Jarvis - anteprime del case (PNG) con VTK in modalita' offscreen.
Legge gli STL "in posizione di montaggio" generati da jarvis_case.py (cartella stl/montaggio)
e salva le immagini in anteprime/.

Uso:  python anteprime.py
Richiede: vtk, numpy, pillow (tutti nel venv usato per CadQuery). Serve un display grafico (X11);
in alternativa VTK compilato con OSMesa/EGL.
"""
import math
import os

import numpy as np
import vtk
from vtk.util import numpy_support
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

QUI = os.path.dirname(os.path.abspath(__file__))
DIR_M = os.path.join(QUI, "stl", "montaggio")
DIR_OUT = os.path.join(QUI, "anteprime")
LARG, ALT = 1400, 1400
Y_SUOLO = -77.5

# colori (RGB 0-1)
BIANCO = (0.95, 0.95, 0.93)
NERO = (0.07, 0.07, 0.08)
GRIGIO = (0.62, 0.64, 0.68)
COLORI = {
    "01_guscio_anteriore": BIANCO, "02_guscio_posteriore": (0.93, 0.93, 0.91),
    "03_visiera": NERO, "04_telaio_scheda": GRIGIO,
    "05_braccio_destro": BIANCO, "06_braccio_sinistro": BIANCO,
    "07_piede_destro": (0.88, 0.89, 0.90), "08_piede_sinistro": (0.88, 0.89, 0.90),
    "09_tastino": BIANCO, "09_tastino_pwr": BIANCO, "11_perno_braccio": BIANCO, "11_perno_braccio_sx": BIANCO,
    "10_anello_altoparlante40": (0.45, 0.47, 0.50),
    "rif_scheda": (0.16, 0.33, 0.70), "rif_vetro": (0.02, 0.02, 0.03),
    "rif_batteria_103040": (0.93, 0.55, 0.15), "rif_batteria_waveshare": (0.93, 0.55, 0.15),
    "12_inserto_batteria_waveshare": (0.55, 0.57, 0.61),
    "rif_altoparlante40_ingombro": (0.80, 0.68, 0.30), "rif_altoparlante_di_serie": (0.80, 0.68, 0.30),
    "rif_max98357a_ingombro": (0.55, 0.30, 0.65),
}
PARTI_CASE = ["01_guscio_anteriore", "02_guscio_posteriore", "03_visiera", "04_telaio_scheda",
              "05_braccio_destro", "06_braccio_sinistro", "07_piede_destro", "08_piede_sinistro",
              "09_tastino", "09_tastino_pwr", "10_anello_altoparlante40", "11_perno_braccio", "11_perno_braccio_sx",
              "12_inserto_batteria_waveshare"]
RIF = ["rif_scheda", "rif_vetro", "rif_batteria_waveshare", "rif_altoparlante40_ingombro",
       "rif_altoparlante_di_serie", "rif_max98357a_ingombro"]

# spostamenti per la vista esplosa (mm): dal davanti (z+) al retro (z-)
ESPLOSO = {
    "03_visiera": (0, 0, 100), "01_guscio_anteriore": (0, 0, 52),
    "09_tastino": (34, 0, 52), "09_tastino_pwr": (34, 0, 52),
    "05_braccio_destro": (26, 0, 52), "06_braccio_sinistro": (-26, 0, 52),
    "11_perno_braccio": (14, 0, 52), "11_perno_braccio_sx": (-14, 0, 52),
    "rif_altoparlante_di_serie": (-16, 0, 52),
    "10_anello_altoparlante40": (0, -40, 30), "rif_altoparlante40_ingombro": (0, -40, 14),
    "rif_scheda": (0, 0, 2), "rif_vetro": (0, 0, 2),
    "04_telaio_scheda": (0, 0, -38), "rif_batteria_waveshare": (0, 0, -54),
    "12_inserto_batteria_waveshare": (0, 0, -70), "rif_max98357a_ingombro": (0, 0, -90),
    "02_guscio_posteriore": (0, 0, -116),
    "07_piede_destro": (0, -30, -116), "08_piede_sinistro": (0, -30, -116),
}
NOMI_BREVI = {
    "03_visiera": "03 visiera (nera)", "01_guscio_anteriore": "01 guscio anteriore", "09_tastino": "09 tastini x2",
    "05_braccio_destro": "05/06 braccia", "rif_scheda": "scheda Waveshare", "04_telaio_scheda": "04 telaio scheda",
    "rif_batteria_waveshare": "batteria Waveshare", "12_inserto_batteria_waveshare": "12 inserto batteria",
    "rif_max98357a_ingombro": "MAX98357A (upgrade)",
    "02_guscio_posteriore": "02 guscio posteriore", "07_piede_destro": "07/08 piedi",
    "10_anello_altoparlante40": "10 anello (upgrade)", "rif_altoparlante40_ingombro": "altoparlante 40 mm (upgrade)",
    "rif_altoparlante_di_serie": "altoparlante di serie",
}


def leggi(nome):
    p = os.path.join(DIR_M, nome + ".stl")
    if not os.path.exists(p):
        return None
    r = vtk.vtkSTLReader(); r.SetFileName(p); r.Update()
    return r.GetOutput()


def normali(pd, angolo=35.0):
    n = vtk.vtkPolyDataNormals(); n.SetInputData(pd); n.SetFeatureAngle(angolo)
    n.SplittingOn(); n.ConsistencyOn(); n.AutoOrientNormalsOff(); n.Update()
    return n.GetOutput()


def attore(pd, colore, spec=0.25, opac=1.0, trasla=(0, 0, 0), lucido=20):
    m = vtk.vtkPolyDataMapper(); m.SetInputData(normali(pd))
    a = vtk.vtkActor(); a.SetMapper(m)
    pr = a.GetProperty(); pr.SetColor(*colore); pr.SetSpecular(spec); pr.SetSpecularPower(lucido)
    pr.SetDiffuse(0.85); pr.SetAmbient(0.14); pr.SetOpacity(opac)
    pr.SetInterpolationToPhong()
    a.SetPosition(*trasla)
    return a


def taglia(pd, origine, normale):
    """Sezione chiusa (con tappo) di una mesh: nel taglio si vede il materiale pieno."""
    piani = vtk.vtkPlaneCollection(); p = vtk.vtkPlane(); p.SetOrigin(*origine); p.SetNormal(*normale)
    piani.AddItem(p)
    c = vtk.vtkClipClosedSurface(); c.SetInputData(pd); c.SetClippingPlanes(piani)
    c.SetGenerateFaces(1); c.Update()
    return c.GetOutput()


def scena(attori, cam_pos, cam_foc, cam_up=(0, 1, 0), parallela=False, scala=None,
          sfondo=((0.99, 0.99, 1.0), (0.84, 0.87, 0.91)), angolo=26.0, dim=(1400, 1400)):
    """Render offscreen. Restituisce (immagine PIL, funzione mondo->pixel, maschera dell'oggetto)."""
    global LARG, ALT
    LARG, ALT = dim
    ren = vtk.vtkRenderer()
    for a in attori:
        ren.AddActor(a)
    ren.GradientBackgroundOn(); ren.SetBackground(*sfondo[1]); ren.SetBackground2(*sfondo[0])
    lk = vtk.vtkLightKit(); lk.SetKeyLightIntensity(0.95); lk.SetKeyToFillRatio(2.4); lk.SetKeyToHeadRatio(3.0)
    lk.SetKeyLightElevation(55); lk.SetKeyLightAzimuth(20)
    lk.AddLightsToRenderer(ren)
    cam = ren.GetActiveCamera()
    cam.SetPosition(*cam_pos); cam.SetFocalPoint(*cam_foc); cam.SetViewUp(*cam_up); cam.SetViewAngle(angolo)
    if parallela:
        cam.ParallelProjectionOn()
        if scala:
            cam.SetParallelScale(scala)
    ren.ResetCameraClippingRange()
    ren.UseFXAAOn()
    w = vtk.vtkRenderWindow(); w.SetOffScreenRendering(1); w.AddRenderer(ren); w.SetSize(LARG, ALT)
    w.Render()
    f = vtk.vtkWindowToImageFilter(); f.SetInput(w); f.SetInputBufferTypeToRGB(); f.ReadFrontBufferOff(); f.Update()
    img = f.GetOutput(); dims = img.GetDimensions()
    arr = numpy_support.vtk_to_numpy(img.GetPointData().GetScalars()).reshape(dims[1], dims[0], 3)[::-1].copy()
    fz = vtk.vtkWindowToImageFilter(); fz.SetInput(w); fz.SetInputBufferTypeToZBuffer(); fz.ReadFrontBufferOff(); fz.Update()
    zb = numpy_support.vtk_to_numpy(fz.GetOutput().GetPointData().GetScalars()).reshape(dims[1], dims[0])[::-1]
    maschera = Image.fromarray(((zb < 0.99999) * 255).astype(np.uint8))
    vm = cam.GetCompositeProjectionTransformMatrix(ren.GetTiledAspectRatio(), -1, 1)
    M = np.array([[vm.GetElement(i, j) for j in range(4)] for i in range(4)])

    def proietta(p):
        v = M @ np.array([p[0], p[1], p[2], 1.0])
        return (v[0] / v[3] + 1) / 2 * LARG, (1 - (v[1] / v[3] + 1) / 2) * ALT
    w.Finalize()
    return Image.fromarray(arr), proietta, maschera


def ombra(img, maschera, proietta, centri, rx=34.0, rz=34.0, forza=0.55):
    """Ombra morbida sul piano del tavolo (solo dove non c'e' l'oggetto)."""
    ombra = Image.new("L", img.size, 0); d = ImageDraw.Draw(ombra)
    for (cx, cz, sx, sz) in centri:
        pts = [proietta((cx + sx * math.cos(t), Y_SUOLO, cz + sz * math.sin(t))) for t in np.linspace(0, 2 * math.pi, 72)]
        d.polygon(pts, fill=255)
    ombra = ombra.filter(ImageFilter.GaussianBlur(30)).point(lambda v: int(v * forza))
    scuro = Image.new("RGB", img.size, (60, 66, 78))
    fondo = Image.composite(scuro, img, ombra)
    m = maschera.filter(ImageFilter.GaussianBlur(0.8))
    return Image.composite(img, fondo, m)


def carica(nomi, trasl=None, taglio=None, colori=None):
    att = []
    for n in nomi:
        pd = leggi(n)
        if pd is None:
            continue
        if taglio is not None:
            pd = taglia(pd, *taglio)
            if pd.GetNumberOfPoints() == 0:
                continue
        t = (trasl or {}).get(n, (0, 0, 0))
        col = (colori or {}).get(n, COLORI.get(n, (0.7, 0.7, 0.7)))
        lucida = n in ("03_visiera", "rif_vetro")
        att.append(attore(pd, col, spec=0.5 if lucida else 0.2, trasla=t, lucido=70 if lucida else 15))
    return att


def interno_scuro():
    """SOLO PER IL RENDER: disco scuro dietro la griglia della pancia, simula l'ombra dentro il corpo
    (senza ombre portate VTK illuminerebbe l'interno e i fori sembrerebbero chiari). Non e' una parte."""
    c = vtk.vtkCylinderSource(); c.SetRadius(16.0); c.SetHeight(0.4); c.SetResolution(64); c.Update()
    t = vtk.vtkTransform(); t.Translate(0, -49.0, -1.8); t.RotateX(90)
    tf = vtk.vtkTransformPolyDataFilter(); tf.SetInputData(c.GetOutput()); tf.SetTransform(t); tf.Update()
    a = attore(tf.GetOutput(), (0.30, 0.30, 0.32), spec=0.0)
    a.GetProperty().SetAmbient(0.0); a.GetProperty().SetDiffuse(0.4)
    return a


def font(dim):
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, dim)
    return ImageFont.load_default()


def titolo(img, testo, sotto=None):
    d = ImageDraw.Draw(img)
    d.text((40, 30), testo, fill=(30, 34, 40), font=font(40))
    if sotto:
        d.text((40, 82), sotto, fill=(80, 86, 96), font=font(24))
    return img


def etichetta(img, punto, testo, verso=(80, -60)):
    d = ImageDraw.Draw(img)
    x, y = punto
    tx, ty = x + verso[0], y + verso[1]
    d.line([(x, y), (tx, ty)], fill=(40, 40, 40), width=3)
    d.ellipse([x - 6, y - 6, x + 6, y + 6], fill=(40, 40, 40))
    f = font(25)
    bb = d.textbbox((0, 0), testo, font=f)
    wbox, hbox = bb[2] - bb[0], bb[3] - bb[1]
    bx = tx if verso[0] >= 0 else tx - wbox - 16
    d.rounded_rectangle([bx - 2, ty - hbox / 2 - 10, bx + wbox + 14, ty + hbox / 2 + 10], 8,
                        fill=(255, 255, 255), outline=(40, 40, 40), width=2)
    d.text((bx + 6, ty - hbox / 2 - 5), testo, fill=(20, 20, 20), font=f)


def occhi_glow(img, proietta, sguardo=(0.0, 0.0)):
    """Due occhi azzurri luminosi sul vetro, come li disegna il firmware: rettangoli arrotondati
    ~100x124 px a 44 px di distanza su uno schermo 368x448 (28,7 x 34,9 mm)."""
    px = 28.70 / 368.0
    ew, eh, gap = 100 * px, 124 * px, 44 * px
    cy = 0.49 + 1.5 + sguardo[1]
    maschera = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(maschera)
    for s in (-1, 1):
        cx = s * (gap / 2 + ew / 2) + sguardo[0]
        pts, r = [], 3.2
        for (ax, ay, a0) in ((cx + ew / 2 - r, cy + eh / 2 - r, 0), (cx - ew / 2 + r, cy + eh / 2 - r, 90),
                             (cx - ew / 2 + r, cy - eh / 2 + r, 180), (cx + ew / 2 - r, cy - eh / 2 + r, 270)):
            for k in range(9):
                a = math.radians(a0 + k * 90 / 8)
                pts.append(proietta((ax + r * math.cos(a), ay + r * math.sin(a), 0.05)))
        d.polygon(pts, fill=255)
    nero = Image.new("RGB", img.size, (0, 0, 0))
    alone = maschera.filter(ImageFilter.GaussianBlur(30)).point(lambda v: min(255, int(v * 1.7)))
    alone2 = maschera.filter(ImageFilter.GaussianBlur(8))
    base = ImageChops.screen(img, Image.composite(Image.new("RGB", img.size, (40, 200, 255)), nero, alone))
    base = ImageChops.screen(base, Image.composite(Image.new("RGB", img.size, (110, 235, 255)), nero, alone2))
    base.paste(Image.new("RGB", img.size, (210, 250, 255)), (0, 0), maschera)
    return base


def centro_parte(nome, trasl=(0, 0, 0)):
    pd = leggi(nome)
    b = pd.GetBounds()
    return ((b[0] + b[1]) / 2 + trasl[0], (b[2] + b[3]) / 2 + trasl[1], (b[4] + b[5]) / 2 + trasl[2])


PIEDI_OMBRA = [(12.5, -18.5, 10, 28), (-12.5, -18.5, 10, 28), (0, -16, 26, 20)]


def main():
    os.makedirs(DIR_OUT, exist_ok=True)
    tutti = PARTI_CASE + RIF
    visibili = PARTI_CASE + ["rif_vetro"]

    # 1) vista frontale 3/4 con occhi accesi
    img, pr, m = scena(carica(visibili) + [interno_scuro()], cam_pos=(185, 85, 300), cam_foc=(0, -21, -14))
    img = ombra(img, m, pr, PIEDI_OMBRA)
    img = occhi_glow(img, pr, sguardo=(0.6, 0.0))
    titolo(img, "Jarvis - vista frontale 3/4", "case a capsula per Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1 e V2)")
    etichetta(img, pr((28.0, 0.0, -6.2)), "USB-C", (90, 20))
    etichetta(img, pr((28.6, 10.5, -4.5)), "BOOT", (90, -40))
    etichetta(img, pr((28.6, -10.5, -4.5)), "PWR", (90, 60))
    img.save(os.path.join(DIR_OUT, "01_vista_frontale_3-4.png"))

    # 2) vista posteriore 3/4
    img, pr, m = scena(carica(visibili), cam_pos=(-200, 80, -290), cam_foc=(0, -19, -14))
    img = ombra(img, m, pr, PIEDI_OMBRA)
    titolo(img, "Vista posteriore 3/4", "3 viti M2x8 nelle torrette (testa + spalle); fianco sinistro: altoparlante e microfono")
    etichetta(img, pr((-28.0, 8.0, -8.5)), "feritoie altoparlante di serie", (-90, -70))
    etichetta(img, pr((-28.0, -13.6, -10.0)), "foro microfono", (-90, 40))
    img.save(os.path.join(DIR_OUT, "02_vista_posteriore_3-4.png"))

    # 3) vista esplosa (diagonale: il davanti in basso a sinistra, il retro in alto a destra)
    img, pr, m = scena(carica(tutti, trasl=ESPLOSO), cam_pos=(360, 210, 330), cam_foc=(0, -24, -12),
                       angolo=36, dim=(1800, 1400))
    for n, t in (("03_visiera", (-110, -90)), ("01_guscio_anteriore", (-260, 20)), ("rif_scheda", (-60, -250)),
                 ("04_telaio_scheda", (-90, -270)), ("rif_batteria_waveshare", (-10, -330)),
                 ("12_inserto_batteria_waveshare", (300, -250)),
                 ("rif_max98357a_ingombro", (250, -70)), ("02_guscio_posteriore", (190, -20)),
                 ("07_piede_destro", (160, 100)), ("rif_altoparlante40_ingombro", (120, 160)),
                 ("10_anello_altoparlante40", (-260, 150)),
                 ("05_braccio_destro", (140, 60)), ("09_tastino", (120, -40)), ("rif_altoparlante_di_serie", (-200, -60))):
        etichetta(img, pr(centro_parte(n, ESPLOSO.get(n, (0, 0, 0)))), NOMI_BREVI[n], t)
    titolo(img, "Vista esplosa", "dal davanti al retro: visiera, guscio anteriore, scheda, telaio, batteria, guscio posteriore + piedi")
    img.save(os.path.join(DIR_OUT, "03_vista_esplosa.png"))

    # 4) sezione verticale a x = -3 mm (si guarda dal lato destro, il davanti a sinistra)
    img, pr, m = scena(carica(tutti, taglio=((-3.0, 0, 0), (-1, 0, 0))), cam_pos=(320, -20, -15),
                       cam_foc=(0, -20, -15), parallela=True, scala=76)
    for p3, txt, t in (((0, 0, 0.5), "vetro + visiera", (-150, -60)), ((-3, -3, -6.8), "scheda (PCB)", (-170, -150)),
                       ((-3, 20, -12.2), "telaio scheda", (170, -150)), ((-3, 6, -15.6), "batteria Waveshare + inserto", (170, -115)),
                       ((-3, 10, -30.5), "spazio MAX98357A", (150, 40)), ((-3, -48, -10), "altoparlante 40 mm (opz.)", (190, 40)),
                       ((-3, -49, 0), "griglia", (-120, 0)), ((-3, -73, -20), "piede", (150, 50)),
                       ((-3, -20.3, -7.0), "antenna (bordo inf.)", (-200, 60))):
        etichetta(img, pr(p3), txt, t)
    titolo(img, "Sezione verticale (x = -3 mm)", "posizione di scheda, batteria e altoparlante; niente metallo vicino all'antenna")
    img.save(os.path.join(DIR_OUT, "04_sezione.png"))

    # 5) vista frontale: aspetto finale con occhi luminosi
    img, pr, m = scena(carica(visibili) + [interno_scuro()], cam_pos=(0, -22, 400), cam_foc=(0, -22, 0),
                       parallela=True, scala=76)
    img = occhi_glow(img, pr)
    titolo(img, "Aspetto finale", "visiera nera + AMOLED: gli occhi brillano sul nero vero")
    img.save(os.path.join(DIR_OUT, "05_frontale_occhi.png"))
    # 6) piatto di stampa: ogni parte come esce dallo script (orientata per la stampa, senza supporti)
    piatto(os.path.join(DIR_OUT, "06_parti_orientate_per_la_stampa.png"))
    print("anteprime salvate in", DIR_OUT)


def piatto(uscita):
    nomi = ["01_guscio_anteriore", "02_guscio_posteriore", "03_visiera", "04_telaio_scheda", "00_prova_misure",
            "07_piede_destro", "08_piede_sinistro", "05_braccio_destro", "06_braccio_sinistro",
            "10_anello_altoparlante40", "09_tastino", "11_perno_braccio", "12_inserto_batteria_waveshare"]
    pos = {"01_guscio_anteriore": (-95, 25), "02_guscio_posteriore": (-30, 25), "03_visiera": (40, 55),
           "04_telaio_scheda": (40, -15), "00_prova_misure": (110, 50),
           "07_piede_destro": (95, -20), "08_piede_sinistro": (118, -20), "05_braccio_destro": (-95, -65),
           "06_braccio_sinistro": (-70, -65), "10_anello_altoparlante40": (-25, -62), "09_tastino": (20, -62),
           "11_perno_braccio": (40, -62), "12_inserto_batteria_waveshare": (75, -62)}
    att = []
    for n in nomi:
        p = os.path.join(QUI, "stl", n + ".stl")
        if not os.path.exists(p):
            continue
        r = vtk.vtkSTLReader(); r.SetFileName(p); r.Update()
        col = COLORI.get(n, (0.6, 0.62, 0.66))
        att.append(attore(r.GetOutput(), col, spec=0.2, trasla=(*pos[n], 0)))
    # piano d'appoggio (solo grafico)
    pl = vtk.vtkPlaneSource(); pl.SetOrigin(-150, -100, -0.3); pl.SetPoint1(150, -100, -0.3); pl.SetPoint2(-150, 100, -0.3)
    pl.Update()
    att.append(attore(pl.GetOutput(), (0.80, 0.82, 0.86), spec=0.0))
    img, pr, m = scena(att, cam_pos=(0, -330, 330), cam_foc=(0, 0, 5), cam_up=(0, 0, 1), angolo=36, dim=(1800, 1300))
    for n, (x, y) in pos.items():
        etichetta(img, pr((x, y, 0)), n.split("_", 1)[0], (0, -0))
    titolo(img, "Parti orientate per la stampa", "come escono in stl/: faccia piatta sul piatto, nessun supporto (fasce a 45 gradi sui bordi dei gusci)")
    img.save(uscita)


if __name__ == "__main__":
    main()
