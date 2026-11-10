"""Gráfico no tempo do teste de adaptação (CSV gerado por `adaptation_follows_the_network`).
Uso: python grafico-adaptacao.py adaptacao-nitidez.csv [saida.png]
Só depende do Pillow. Três painéis: bitrate (limite da rede, estimativa, enviado), FPS e resolução, latência e maior intervalo."""
import csv
import sys
from PIL import Image, ImageDraw, ImageFont

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
rows = list(csv.DictReader(open(src, encoding='utf-8')))
T = [float(r['t_s']) for r in rows]
W, H, L, R = 1400, 980, 90, 150
img = Image.new('RGB', (W, H), (18, 6, 26))
d = ImageDraw.Draw(img)
try:
    font = ImageFont.truetype('segoeui.ttf', 15)
    small = ImageFont.truetype('segoeui.ttf', 13)
    big = ImageFont.truetype('segoeuib.ttf', 19)
except OSError:
    font = small = big = ImageFont.load_default()


def panel(top, height, title, series, ymax, unit, log=False):
    """series: [(rótulo, cor, valores, tracejado)]"""
    x0, x1, y0, y1 = L, W - R, top + 30, top + height - 26
    d.text((x0, top + 2), title, fill=(237, 241, 247), font=big)
    d.rectangle([x0, y0, x1, y1], outline=(70, 45, 95))
    tmax = max(T)

    def X(t): return x0 + (x1 - x0) * t / tmax

    def Y(v): return y1 - (y1 - y0) * min(max(v, 0), ymax) / ymax

    for i in range(5):
        v = ymax * i / 4
        d.line([x0, Y(v), x1, Y(v)], fill=(45, 28, 62))
        d.text((x0 - 8, Y(v) - 8), f'{v:g}', fill=(165, 146, 184), font=small, anchor='ra')
    d.text((8, y0 - 2), unit, fill=(165, 146, 184), font=small)
    # faixas dos degraus da rede
    last = None
    for r in rows:
        key = (r['limite_mbps'], r['perda_injetada_pct'])
        if key != last:
            x = X(float(r['t_s']) - 1)
            d.line([x, y0, x, y1], fill=(110, 80, 150))
            d.text((x + 4, y1 + 4), f"{float(key[0]):g} Mbps, perda {float(key[1]):g}%", fill=(165, 146, 184), font=small)
            last = key
    for k, (label, color, vals, dash) in enumerate(series):
        pts = [(X(t), Y(v)) for t, v in zip(T, vals) if v is not None]
        for a, b in zip(pts, pts[1:]):
            if dash:
                mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
                d.line([a, (mx, my)], fill=color, width=2)
            else:
                d.line([a, b], fill=color, width=3)
        d.line([x1 + 12, y0 + 12 + k * 22, x1 + 36, y0 + 12 + k * 22], fill=color, width=3)
        d.text((x1 + 42, y0 + 3 + k * 22), label, fill=(237, 241, 247), font=small)


def col(name, scale=1.0, neg=None):
    out = []
    for r in rows:
        v = float(r[name]) * scale
        out.append(neg if v < 0 else v)
    return out


mb = 1 / 1000
panel(40, 330, 'Bitrate', [
    ('limite da rede', (233, 85, 85), [min(float(r['limite_mbps']), 13) for r in rows], True),
    ('estimativa', (245, 185, 66), col('estimativa_kbps', mb), False),
    ('enviado', (87, 210, 139), col('enviado_kbps', mb), False),
], 13, 'Mbps')
panel(370, 300, 'Quadros por segundo e resolução', [
    ('FPS alvo', (139, 92, 246), col('fps_alvo'), True),
    ('FPS recebido', (87, 210, 139), col('fps_recebido'), False),
    ('altura ÷ 36', (86, 170, 255), col('altura', 1 / 36), False),
], 32, 'qps')
panel(670, 300, 'Latência (captura → quadro pronto) e maior intervalo sem quadro novo', [
    ('latência p50', (87, 210, 139), col('latencia_p50_ms', 1.0, None), False),
    ('maior intervalo', (233, 85, 85), col('maior_intervalo_ms'), False),
], 5000, 'ms')
d.text((L, 8), src.replace('\\', '/').rsplit('/', 1)[-1], fill=(165, 146, 184), font=font)
img.save(out)
print(out)
