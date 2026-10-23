"""Gera icone.ico (quadrado azul com uma gota branca, como o ícone do painel). Precisa do Pillow.
Uso: python gerar_icone.py   -> cria icone.ico ao lado deste arquivo."""
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Pillow não instalado; icone.ico não foi gerado (o .exe sai com o ícone padrão).")
    sys.exit(1)

L = 1024                       # desenha grande e reduz (bordas suaves)
AZUL = (43, 123, 214, 255)
img = Image.new("RGBA", (L, L), AZUL)
d = ImageDraw.Draw(img)
cx, cy, r = L / 2, L * 0.60, L * 0.25          # círculo da base da gota
topo = (cx, L * 0.16)                          # ponta da gota
d.ellipse((cx - r, cy - r, cx + r, cy + r), fill="white")
# laterais da gota: retas da ponta até os pontos de tangência do círculo
cos_a = r / (cy - topo[1])
sin_a = (1 - cos_a ** 2) ** 0.5
d.polygon([topo, (cx - r * sin_a, cy - r * cos_a), (cx + r * sin_a, cy - r * cos_a)], fill="white")
# brilho em arco, como no ícone do painel
d.arc((cx - r * 0.62, cy - r * 0.62, cx + r * 0.62, cy + r * 0.62), start=20, end=80, fill=AZUL, width=int(L * 0.018))

destino = Path(__file__).resolve().parent / "icone.ico"
img.save(destino, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(destino.with_suffix(".png"))
print(f"Ícone gerado: {destino}")
