#!/usr/bin/env python3
"""Gera ícones e telas de abertura do app (manômetro branco sobre cobre). Uso: python3 scripts/gerar_icones.py"""
import math, os
from PIL import Image, ImageDraw

RES = os.path.join(os.path.dirname(__file__), '..', 'android', 'app', 'src', 'main', 'res')
COBRE, BRANCO, FUNDO = (184, 105, 47, 255), (255, 255, 255, 255), (243, 242, 240, 255)
SS = 4  # superamostragem para bordas suaves

def manometro(d, cx, cy, r, cor, cor_ponteiro):
    w = max(2, int(r * 0.13))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=cor, width=w)
    ri = r * 0.68  # marcas da escala (arco de 225° a -45°)
    for i in range(9):
        a = math.radians(225 - i * 270 / 8)
        p1 = (cx + ri * math.cos(a), cy - ri * math.sin(a)); p2 = (cx + (ri - r * 0.14) * math.cos(a), cy - (ri - r * 0.14) * math.sin(a))
        d.line([p1, p2], fill=cor, width=max(1, int(w * 0.55)))
    a = math.radians(40)  # ponteiro
    d.line([(cx, cy), (cx + r * 0.6 * math.cos(a), cy - r * 0.6 * math.sin(a))], fill=cor_ponteiro, width=int(w * 1.1))
    rc = r * 0.12
    d.ellipse([cx - rc, cy - rc, cx + rc, cy + rc], fill=cor_ponteiro)

def icone(tam, forma):
    T = tam * SS
    im = Image.new('RGBA', (T, T), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    if forma == 'redondo': d.ellipse([0, 0, T - 1, T - 1], fill=COBRE)
    elif forma == 'quadrado': d.rounded_rectangle([0, 0, T - 1, T - 1], radius=int(T * 0.18), fill=COBRE)
    escala = 0.30 if forma == 'frente' else 0.34  # frente adaptativa: área segura de 66/108
    manometro(d, T / 2, T / 2, T * escala, BRANCO, BRANCO)
    return im.resize((tam, tam), Image.LANCZOS)

def splash(w, h):
    W, H = w * 2, h * 2
    im = Image.new('RGBA', (W, H), FUNDO); d = ImageDraw.Draw(im)
    r = min(W, H) * 0.12
    d.ellipse([W / 2 - r * 1.35, H / 2 - r * 1.35, W / 2 + r * 1.35, H / 2 + r * 1.35], fill=COBRE)
    manometro(d, W / 2, H / 2, r, BRANCO, BRANCO)
    return im.resize((w, h), Image.LANCZOS).convert('RGB')

for pasta in os.listdir(RES):
    for nome in os.listdir(os.path.join(RES, pasta)):
        cam = os.path.join(RES, pasta, nome)
        if not nome.endswith('.png'): continue
        w, h = Image.open(cam).size
        if nome == 'ic_launcher.png': icone(w, 'quadrado').save(cam)
        elif nome == 'ic_launcher_round.png': icone(w, 'redondo').save(cam)
        elif nome == 'ic_launcher_foreground.png': icone(w, 'frente').save(cam)
        elif nome == 'splash.png': splash(w, h).save(cam)
        else: continue
        print('gerado', os.path.relpath(cam, RES))
