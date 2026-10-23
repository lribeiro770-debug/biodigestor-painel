#!/usr/bin/env python3
"""ESP32 simulado (protocolo de coleta v1). Uso: python esp_simulado.py [--porta 8088] [--token teste123] [--linhas 5000]
Implementa /api/info, /api/agora, /api/dados, /api/eventos, /api/hora, /api/coletado e uma página / simples."""
import argparse, json, math, random, time, threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

ap = argparse.ArgumentParser(); ap.add_argument("--porta", type=int, default=8088); ap.add_argument("--token", default="teste123")
ap.add_argument("--linhas", type=int, default=5000); ap.add_argument("--buraco", default="", help="faixa de n ausente no cartão, ex.: 1200-7300"); ap.add_argument("--intervalo", type=int, default=60)
a = ap.parse_args()
COLS = ["n","ts","ts_ok","boot","up","t_dig","t_dig2","t_med","t_amb","ur_amb","mq2_rel","mq7_rel","p_pa","vol_nl","vazao_nlh","rele","alarmes","sim"]
SERIE = "5e1a0c3f"; lock = threading.Lock()
rows, eventos, ancoras = [], [], []   # ancoras: (boot, up, epoch)
agora0 = int(time.time()); t_inicio = agora0 - a.linhas * a.intervalo
# gera histórico: boot 1 (com hora), boot 2 (queda de energia; sem hora até uma âncora), boot 3 (sem âncora: hora perdida)
corte1, corte2 = int(a.linhas * 0.6), int(a.linhas * 0.85)
boot_start = {}
for i in range(a.linhas):
    n = i + 1; ts = t_inicio + i * a.intervalo
    boot = 1 if i < corte1 else (2 if i < corte2 else 3)
    boot_start.setdefault(boot, ts)
    up = ts - boot_start[boot]
    h = (ts % 86400) / 3600
    t1 = 30 + 1.2 * math.sin((h - 15) / 24 * 2 * math.pi) + random.uniform(-.1, .1); t2 = t1 - 1.0
    rel2 = 1 - (0.55 if 3000 < i % 4000 < 3030 else 0) + random.uniform(-.03, .03)
    known = (boot == 1)
    rows.append({"n": n, "ts_real": ts, "ts": ts if known else None, "boot": boot, "up": up, "t_dig": round(t1, 2), "t_dig2": round(t2, 2), "t_med": round((t1 + t2) / 2, 2),
                 "t_amb": round(24 + 6 * math.sin((h - 15) / 24 * 2 * math.pi), 1), "ur_amb": round(70 - 15 * math.sin((h - 15) / 24 * 2 * math.pi)),
                 "mq2_rel": round(rel2, 3), "mq7_rel": round(1 + random.uniform(-.03, .03), 3), "p_pa": round(300 + 200 * math.sin(i / 50)), "vol_nl": round(i * 0.03, 3),
                 "vazao_nlh": round(1.8 + .3 * math.sin(i / 300), 3), "rele": 0, "alarmes": "GAS_COMBUSTIVEL" if rel2 < .5 else "", "sim": 1})
# âncora no boot 2 (hora acertada no meio dele)
mid2 = rows[(corte1 + corte2) // 2]; ancoras.append((2, mid2["up"], mid2["ts_real"]))
for b in (1, 2, 3):
    r = next(x for x in rows if x["boot"] == b); eventos.append({"n": len(eventos) + 1, "ts": r["ts"], "boot": b, "up": 0, "tipo": "boot", "detalhe": f"partida {b}"})
if a.buraco:
    _b0, _b1 = map(int, a.buraco.split("-")); rows = [r for r in rows if not (_b0 <= r["n"] <= _b1)]
boot_atual = 3; up0 = time.time() - (rows[-1]["up"])   # relógio do boot atual
hora_ok = False; offset = 0  # epoch = time.time()+offset quando hora_ok

def up_now(): return int(time.time() - up0)
def epoch_now(): return int(time.time() + offset) if hora_ok else None
def ts_of(r):
    if r["ts"] is not None: return r["ts"], 1
    for (b, u, e) in ancoras:
        if b == r["boot"]: return e - (u - r["up"]), 2
    return None, 0
def fmt(v): return "" if v is None else (repr(v) if isinstance(v, float) else str(v))
def gerador():
    global rows
    while True:
        time.sleep(5)  # acelera: 1 amostra a cada 5 s reais
        with lock:
            last = rows[-1]; n = last["n"] + 1; e = epoch_now()
            r = dict(last); r.update(n=n, up=up_now(), ts=e, ts_real=e or 0, t_dig=round(last["t_dig"] + random.uniform(-.05, .05), 2)); rows.append(r)
threading.Thread(target=gerador, daemon=True).start()

class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json", extra=None):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b)))
        self.send_header("Access-Control-Allow-Origin", "*"); self.send_header("Access-Control-Expose-Headers", "X-Ultimo-N, X-Mais, X-Serie")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def _auth(self, q):
        tok = self.headers.get("X-Token") or (q.get("token") or [""])[0]
        if tok != a.token: self._send(401, json.dumps({"erro": "token"})); return False
        return True
    def do_OPTIONS(self):
        self.send_response(204); self.send_header("Access-Control-Allow-Origin", "*"); self.send_header("Access-Control-Allow-Headers", "X-Token, Content-Type"); self.send_header("Access-Control-Allow-Methods", "GET, POST"); self.end_headers()
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        with lock:
            if u.path == "/api/info":
                return self._send(200, json.dumps({"proto": 1, "id": "biodig-utfpr-cm-01", "fw": "3.0.0-sim", "serie": SERIE, "agora": epoch_now() or 0, "hora_ok": hora_ok,
                    "boot": boot_atual, "up": up_now(), "n_primeiro": rows[0]["n"], "n_ultimo": rows[-1]["n"], "intervalo_s": a.intervalo, "sd_ok": True, "sd_livre_mb": 14800,
                    "ev_ultimo": eventos[-1]["n"], "colunas": COLS}))
            if u.path == "/api/agora":
                r = rows[-1]; return self._send(200, json.dumps({k: r[k] for k in r if k != "ts_real"}))
            if u.path == "/api/dados":
                if not self._auth(q): return
                desde = int((q.get("desde") or ["0"])[0]); mx = min(5000, int((q.get("max") or ["2000"])[0]))
                total = rows[-1]["n"]; ate = max(desde, min(total, desde + mx))   # mesma janela do firmware: (desde, ate]
                lote = [r for r in rows if desde < r["n"] <= ate]
                out = [",".join(COLS)]
                for r in lote:
                    ts, ok = ts_of(r); rr = dict(r, ts=ts, ts_ok=ok); out.append(",".join(fmt(rr[c]) for c in COLS))
                return self._send(200, "\n".join(out) + "\n", "text/csv; charset=utf-8", {"X-Ultimo-N": str(ate), "X-Mais": "1" if total > ate else "0", "X-Serie": SERIE})
            if u.path == "/api/eventos":
                if not self._auth(q): return
                desde = int((q.get("desde") or ["0"])[0]); sel = [e for e in eventos if e["n"] > desde]
                out = ["n,ts,ts_ok,boot,up,tipo,detalhe"] + [f'{e["n"]},{fmt(e["ts"])},{1 if e["ts"] else 0},{e["boot"]},{e["up"]},{e["tipo"]},{e["detalhe"]}' for e in sel]
                return self._send(200, "\n".join(out) + "\n", "text/csv; charset=utf-8", {"X-Ultimo-N": str(sel[-1]["n"] if sel else desde), "X-Mais": "0", "X-Serie": SERIE})
            if u.path == "/":
                return self._send(200, "<h1>Biodigestor (simulado)</h1><p>Use /api/info</p>", "text/html; charset=utf-8")
        self._send(404, json.dumps({"erro": "rota"}))
    def do_POST(self):
        global hora_ok, offset
        u = urlparse(self.path); q = parse_qs(u.query); ln = int(self.headers.get("Content-Length") or 0)
        try: body = json.loads(self.rfile.read(ln) or b"{}")
        except Exception: return self._send(400, json.dumps({"erro": "json"}))
        if not self._auth(q): return
        with lock:
            if u.path == "/api/hora":
                ag = int(body.get("agora", 0)); atual = epoch_now(); d = (ag - atual) if atual else None
                if not hora_ok or abs(d) > 2:
                    offset = ag - time.time(); hora_ok = True; ancoras.append((boot_atual, up_now(), ag))
                    eventos.append({"n": len(eventos) + 1, "ts": ag, "boot": boot_atual, "up": up_now(), "tipo": "hora", "detalhe": "acertada pelo coletor"})
                return self._send(200, json.dumps({"ok": True, "ajuste_s": d}))
            if u.path == "/api/coletado":
                eventos.append({"n": len(eventos) + 1, "ts": epoch_now(), "boot": boot_atual, "up": up_now(), "tipo": "coleta", "detalhe": f'{body.get("coletor","?")} ate {body.get("ate")}'})
                return self._send(200, json.dumps({"ok": True}))
        self._send(404, json.dumps({"erro": "rota"}))
    def log_message(self, *x): pass

print(f"ESP simulado em http://127.0.0.1:{a.porta}  token={a.token}  linhas={len(rows)}  serie={SERIE}")
ThreadingHTTPServer(("0.0.0.0", a.porta), H).serve_forever()
