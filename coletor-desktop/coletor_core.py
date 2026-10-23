"""Núcleo do Coletor do Biodigestor (protocolo de coleta v1, ver ESPEC_COLETA.md).

Só biblioteca padrão. Nada aqui depende de tkinter: tudo pode ser usado/testado sem interface.

Partes:
  - configuração e estado (pasta de dados por usuário, gravação atômica)
  - ClienteESP     : API HTTP do ESP32 (info, hora, dados, eventos, coletado)
  - ArmazemLocal   : CSVs mensais locais no formato da nuvem, sem duplicatas (serie+n)
  - coletar()      : coleta completa, retomável
  - ClienteGitHub / enviar() : envio pela API de conteúdo do GitHub, com mescla e index.json
  - GerenciadorWifi: troca de rede (Windows netsh / Linux nmcli / macOS networksetup), melhor esforço
  - ModoAutomatico : laço de 30 s que coleta quando o AP aparece e envia quando há internet
"""
import base64
import copy
import csv
import datetime
import io
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

VERSAO = "1.0.0"
PROTO = 1
LOTE_MAX = 2000                 # linhas por pedido a /api/dados (spec: padrão 2000)
TS_MINIMO = 1577836800          # 2020-01-01: ts menor que isso é tratado como hora desconhecida
COLS_ESP_PADRAO = ["n", "ts", "ts_ok", "boot", "up", "t_dig", "t_dig2", "t_med", "t_amb", "ur_amb", "mq2_rel",
                   "mq7_rel", "p_pa", "vol_nl", "vazao_nlh", "rele", "alarmes", "sim"]
COLS_NUVEM = ["ts", "t_dig", "t_dig2", "t_med", "t_amb", "ur_amb", "mq2_rel", "mq7_rel", "p_pa", "vol_nl",
              "vazao_nlh", "rele", "alarmes", "sim", "ts_ok", "serie", "n"]
COLS_EVENTOS = ["ts", "serie", "n", "tipo", "detalhe"]
ARQ_SEM_HORA = "sem_hora.csv"
ARQ_EVENTOS = "eventos.csv"
RE_MES = re.compile(r"^\d{4}-\d{2}\.csv$")
PREFIXO_AP = "Biodigestor-"

CONFIG_PADRAO = {
    "esp_url": "http://192.168.4.1",
    "esp_token": "",
    "ap_ssid": "",                 # vazio = qualquer rede que comece com "Biodigestor-"
    "ap_senha": "biogas123",
    "gh_dono": "lribeiro770-debug",
    "gh_repo": "biodigestor-painel",
    "gh_ramo": "main",
    "gh_token": "",
    "gh_api": "https://api.github.com",
    "coletor": "",
    "auto_intervalo_min": 60,
    "acertar_hora": True,
    "trocar_wifi": True,
    "timeout_esp_s": 15,
}
CHAVES_SECRETAS = ("esp_token", "gh_token", "ap_senha")


# ============================================================================
# Erros
# ============================================================================
class ErroColetor(Exception):
    """Erro com mensagem já pronta para o usuário (em português)."""


class ErroESP(ErroColetor):
    pass


class ErroGitHub(ErroColetor):
    pass


class SemInternet(ErroColetor):
    pass


class Interrompido(ErroColetor):
    pass


class ConflitoSha(Exception):
    pass


# ============================================================================
# Pasta de dados, gravação atômica, configuração, estado, registro
# ============================================================================
def pasta_padrao() -> Path:
    if os.environ.get("COLETOR_BIODIGESTOR_DIR"):
        return Path(os.environ["COLETOR_BIODIGESTOR_DIR"])
    if sys.platform.startswith("win") and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "ColetorBiodigestor"
    return Path.home() / ".coletor_biodigestor"


def gravar_atomico(caminho, dados) -> None:
    """Grava num temporário na mesma pasta e troca de nome (os.replace é atômico)."""
    caminho = Path(caminho)
    caminho.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(dados, str):
        dados = dados.encode("utf-8")
    fd, tmp = tempfile.mkstemp(prefix="." + caminho.name + ".", suffix=".tmp", dir=str(caminho.parent))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(dados)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, str(caminho))
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _ler_json(caminho, padrao):
    try:
        with open(caminho, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return copy.deepcopy(padrao)
    except (OSError, ValueError):
        # arquivo corrompido: guarda uma cópia e recomeça do padrão
        try:
            shutil.copy2(str(caminho), str(caminho) + ".corrompido")
        except OSError:
            pass
        return copy.deepcopy(padrao)


def nome_padrao_coletor() -> str:
    for v in ("USERNAME", "USER", "LOGNAME"):
        if os.environ.get(v):
            return os.environ[v]
    return platform.node() or "coletor"


class Config(dict):
    """Configuração salva em <pasta>/config.json."""

    def __init__(self, base):
        super().__init__(CONFIG_PADRAO)
        self.base = Path(base)
        self.caminho = self.base / "config.json"
        dados = _ler_json(self.caminho, {})
        if isinstance(dados, dict):
            for k, v in dados.items():
                if k in CONFIG_PADRAO:
                    self[k] = v
        if not self.get("coletor"):
            self["coletor"] = nome_padrao_coletor()

    def salvar(self):
        gravar_atomico(self.caminho, json.dumps(dict(self), indent=2, ensure_ascii=False))
        try:
            os.chmod(self.caminho, 0o600)   # sem efeito no Windows; lá a pasta já é do usuário
        except OSError:
            pass

    def mascarado(self) -> dict:
        d = dict(self)
        for k in CHAVES_SECRETAS:
            if d.get(k):
                d[k] = "***"
        return d


ESTADO_PADRAO = {
    "esp": {},                 # serie -> {"ultimo_n", "ultimo_ev"}  (já baixado do ESP)
    "nuvem": {},               # serie -> {"ultimo_n", "ultimo_ev"}  (já enviado)
    "pendente": False,
    "arquivos_pendentes": [],  # arquivos locais alterados desde o último envio
    "sem_hora": {},            # serie -> {boot: [n_min, n_max]} linhas guardadas sem ts
    "dispositivo": "",
    "ultima_serie": "",
    "ultima_coleta": 0,
    "ultimo_envio": 0,
    "ultimo_info": {},
    "ultima_hora": "",
}


class Estado(dict):
    """estado.json (spec §7) + campos auxiliares do app."""

    def __init__(self, base):
        super().__init__(copy.deepcopy(ESTADO_PADRAO))
        self.caminho = Path(base) / "estado.json"
        dados = _ler_json(self.caminho, {})
        if isinstance(dados, dict):
            self.update(dados)
        for k, v in ESTADO_PADRAO.items():
            self.setdefault(k, copy.deepcopy(v))

    def salvar(self):
        gravar_atomico(self.caminho, json.dumps(dict(self), indent=2, ensure_ascii=False))

    def serie(self, chave, serie):
        return self[chave].setdefault(serie, {"ultimo_n": 0, "ultimo_ev": 0})

    def marcar_pendentes(self, nomes):
        atuais = set(self["arquivos_pendentes"])
        atuais.update(nomes)
        self["arquivos_pendentes"] = sorted(atuais)
        if nomes:
            self["pendente"] = True

    def linhas_pendentes(self) -> int:
        tot = 0
        for s, v in self["esp"].items():
            tot += max(0, int(v.get("ultimo_n", 0)) - int(self["nuvem"].get(s, {}).get("ultimo_n", 0)))
        return tot


class Registro:
    """Log com data/hora, em arquivo e em 'ouvintes' (GUI, terminal). Mascara segredos."""

    def __init__(self, base=None, cfg=None):
        self.cfg = cfg
        self.arquivo = Path(base) / "coletor.log" if base else None
        self.ouvintes = []
        self.lock = threading.Lock()

    def _mascarar(self, msg: str) -> str:
        if self.cfg:
            for k in CHAVES_SECRETAS:
                v = self.cfg.get(k)
                if v and len(str(v)) >= 4:
                    msg = msg.replace(str(v), "***")
        return re.sub(r"(github_pat_|ghp_)[A-Za-z0-9_]+", r"\1***", msg)

    def __call__(self, msg: str):
        msg = self._mascarar(str(msg))
        linha = time.strftime("%Y-%m-%d %H:%M:%S") + "  " + msg
        with self.lock:
            if self.arquivo:
                try:
                    self.arquivo.parent.mkdir(parents=True, exist_ok=True)
                    if self.arquivo.exists() and self.arquivo.stat().st_size > 2_000_000:
                        os.replace(str(self.arquivo), str(self.arquivo) + ".1")
                    with open(self.arquivo, "a", encoding="utf-8") as f:
                        f.write(linha + "\n")
                except OSError:
                    pass
            for o in list(self.ouvintes):
                try:
                    o(linha)
                except Exception:
                    pass


# trava global: coleta e envio nunca rodam ao mesmo tempo
OPERACAO = threading.Lock()


# ============================================================================
# HTTP
# ============================================================================
_SEM_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))
_COM_PROXY = urllib.request.build_opener()


def _endereco_local(url) -> bool:
    host = (urllib.parse.urlparse(url).hostname or "").lower()
    if host in ("localhost", "::1") or host.endswith(".local"):
        return True
    m = re.match(r"^(\d+)\.(\d+)\.\d+\.\d+$", host)
    if not m:
        return False
    a, b = int(m.group(1)), int(m.group(2))
    return a in (10, 127) or (a == 192 and b == 168) or (a == 172 and 16 <= b <= 31) or (a == 169 and b == 254)


def http(metodo, url, corpo=None, cabecalhos=None, timeout=15):
    """Faz a requisição e devolve (status, cabeçalhos em minúsculas, corpo em bytes).
    Erros HTTP (4xx/5xx) voltam como status; falhas de rede levantam OSError."""
    if isinstance(corpo, (dict, list)):
        corpo = json.dumps(corpo).encode("utf-8")
        cabecalhos = dict(cabecalhos or {}, **{"Content-Type": "application/json"})
    req = urllib.request.Request(url, data=corpo, method=metodo, headers=dict(cabecalhos or {}))
    req.add_header("User-Agent", "ColetorBiodigestor/" + VERSAO)
    abridor = _SEM_PROXY if _endereco_local(url) else _COM_PROXY
    try:
        with abridor.open(req, timeout=timeout) as r:
            return r.status, {k.lower(): v for k, v in r.headers.items()}, r.read()
    except urllib.error.HTTPError as e:
        try:
            b = e.read()
        except Exception:
            b = b""
        return e.code, {k.lower(): v for k, v in (e.headers or {}).items()}, b
    except urllib.error.URLError as e:
        raise OSError(str(e.reason)) from e


def _json(b):
    try:
        return json.loads(b.decode("utf-8") or "{}")
    except (ValueError, UnicodeDecodeError):
        return {}


# ============================================================================
# CSV e conversões
# ============================================================================
def ler_csv_texto(texto: str):
    texto = texto.lstrip("﻿")
    r = csv.DictReader(io.StringIO(texto))
    cols = r.fieldnames or []
    linhas = [l for l in r if any((v or "").strip() for v in l.values() if isinstance(v, str))]
    return cols, linhas


def csv_para_texto(linhas, colunas) -> str:
    s = io.StringIO()
    w = csv.DictWriter(s, fieldnames=colunas, extrasaction="ignore", lineterminator="\n")
    w.writeheader()
    for l in linhas:
        w.writerow({c: ("" if l.get(c) is None else l.get(c)) for c in colunas})
    return s.getvalue()


def ts_valido(ts):
    """Devolve o epoch inteiro, ou None se o ts estiver vazio/inválido."""
    try:
        t = int(float(ts))
    except (TypeError, ValueError):
        return None
    return t if t >= TS_MINIMO else None


def mes_de_ts(ts):
    t = ts_valido(ts)
    if t is None:
        return None
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m")


def arquivo_da_amostra(linha) -> str:
    m = mes_de_ts(linha.get("ts"))
    return (m + ".csv") if m else ARQ_SEM_HORA


def amostra_para_nuvem(linha, serie) -> dict:
    d = {c: (linha.get(c) or "").strip() for c in COLS_NUVEM}
    d["serie"] = serie
    d["n"] = str(int(float(linha["n"])))
    if ts_valido(d["ts"]) is None:
        d["ts"] = ""
        if d["ts_ok"] not in ("", "0"):
            d["ts_ok"] = "0"
    else:
        d["ts"] = str(ts_valido(d["ts"]))
    return d


def evento_para_nuvem(linha, serie) -> dict:
    t = ts_valido(linha.get("ts"))
    return {"ts": "" if t is None else str(t), "serie": serie, "n": str(int(float(linha["n"]))),
            "tipo": (linha.get("tipo") or "").strip(), "detalhe": (linha.get("detalhe") or "").strip()}


def chave(l):
    return (l["serie"], int(l["n"]))


def _ordem_amostra(l):
    t = ts_valido(l.get("ts"))
    return (0 if t is not None else 1, t or 0, l["serie"], int(l["n"]))


def serializar(nome, linhas_dict) -> str:
    linhas = sorted(linhas_dict.values(), key=_ordem_amostra)
    return csv_para_texto(linhas, COLS_EVENTOS if nome == ARQ_EVENTOS else COLS_NUVEM)


def interpretar(nome, texto) -> dict:
    """Texto CSV (formato da nuvem) -> {(serie, n): linha}. Linhas inválidas são ignoradas."""
    cols_ok = COLS_EVENTOS if nome == ARQ_EVENTOS else COLS_NUVEM
    _, linhas = ler_csv_texto(texto)
    out = {}
    for l in linhas:
        try:
            d = {c: (l.get(c) or "") for c in cols_ok}
            out[chave(d)] = d
        except (KeyError, ValueError, TypeError):
            continue
    return out


# ============================================================================
# Armazém local (pasta dados/ com os mesmos arquivos da nuvem)
# ============================================================================
class ArmazemLocal:
    def __init__(self, base):
        self.pasta = Path(base) / "dados"
        self.pasta.mkdir(parents=True, exist_ok=True)

    def arquivos(self):
        return sorted(p.name for p in self.pasta.glob("*.csv"))

    def meses(self):
        return sorted(n[:-4] for n in self.arquivos() if RE_MES.match(n))

    def ler(self, nome) -> dict:
        p = self.pasta / nome
        if not p.exists():
            return {}
        return interpretar(nome, p.read_text(encoding="utf-8", errors="replace"))

    def gravar(self, nome, linhas_dict):
        p = self.pasta / nome
        if not linhas_dict:
            if p.exists():
                p.unlink()
            return
        gravar_atomico(p, serializar(nome, linhas_dict))

    def mesclar_amostras(self, linhas):
        """Mescla amostras (formato nuvem). Devolve {arquivo: linhas_novas} dos arquivos alterados.
        Linha que chega com ts sai de sem_hora.csv (se estava lá)."""
        grupos = {}
        for l in linhas:
            grupos.setdefault(arquivo_da_amostra(l), []).append(l)
        cache, alterados = {}, {}
        for nome, ls in grupos.items():
            atual = cache.setdefault(nome, self.ler(nome))
            novas = 0
            for l in ls:
                k = chave(l)
                if k not in atual:
                    novas += 1
                if atual.get(k) != l:
                    atual[k] = l
                    alterados.setdefault(nome, 0)
            if nome in alterados:
                alterados[nome] = novas
        com_ts = [chave(l) for n, ls in grupos.items() if n != ARQ_SEM_HORA for l in ls]
        if com_ts and (self.pasta / ARQ_SEM_HORA).exists():
            sh = cache.setdefault(ARQ_SEM_HORA, self.ler(ARQ_SEM_HORA))
            movidas = [k for k in com_ts if sh.pop(k, None) is not None]
            if movidas:
                alterados.setdefault(ARQ_SEM_HORA, 0)
        for nome in alterados:
            self.gravar(nome, cache[nome])
        return alterados

    def mesclar_eventos(self, linhas) -> int:
        atual = self.ler(ARQ_EVENTOS)
        novas = 0
        for l in linhas:
            k = chave(l)
            if k not in atual:
                novas += 1
            atual[k] = l
        if linhas:
            self.gravar(ARQ_EVENTOS, atual)
        return novas

    def chaves_com_ts(self) -> set:
        s = set()
        for m in self.meses():
            s.update(self.ler(m + ".csv").keys())
        return s

    def ultimas_amostras(self, k=12):
        """Últimas k amostras com hora (mais recentes primeiro)."""
        out = []
        for m in reversed(self.meses()):
            linhas = sorted(self.ler(m + ".csv").values(), key=_ordem_amostra)
            out = linhas[-(k - len(out)):] + out
            if len(out) >= k:
                break
        return list(reversed(out[-k:]))

    def ultimo_ts(self) -> int:
        for m in reversed(self.meses()):
            ts = [ts_valido(l["ts"]) for l in self.ler(m + ".csv").values()]
            ts = [t for t in ts if t]
            if ts:
                return max(ts)
        return 0


# ============================================================================
# Cliente do ESP
# ============================================================================
class ClienteESP:
    def __init__(self, url, token, timeout=15):
        self.url = (url or CONFIG_PADRAO["esp_url"]).rstrip("/")
        if not re.match(r"^https?://", self.url):
            self.url = "http://" + self.url
        self.token = token or ""
        self.timeout = timeout

    def _req(self, metodo, caminho, corpo=None, auth=True, timeout=None):
        cab = {"X-Token": self.token} if auth else {}
        try:
            st, hd, b = http(metodo, self.url + caminho, corpo, cab, timeout or self.timeout)
        except (OSError, ValueError) as e:
            raise ErroESP(f"ESP não alcançável em {self.url} ({e}). Você está conectado à rede Biodigestor-XXXX?")
        if st == 401:
            raise ErroESP("O ESP recusou o token (401). Confira o token do ESP em Configurações.")
        if st != 200:
            raise ErroESP(f"O ESP respondeu {st} em {caminho}: {b[:200].decode('utf-8', 'replace')}")
        return hd, b

    def info(self, timeout=None) -> dict:
        _, b = self._req("GET", "/api/info", auth=False, timeout=timeout)
        d = _json(b)
        if "serie" not in d or "n_ultimo" not in d:
            raise ErroESP("Resposta inesperada de /api/info (isto é mesmo o ESP do biodigestor?)")
        if not re.match(r"^[0-9A-Za-z_-]{1,32}$", str(d["serie"])):
            raise ErroESP("Série do cartão inválida em /api/info: " + str(d["serie"])[:40])
        return d

    def alcancavel(self, timeout=3):
        try:
            return self.info(timeout=timeout)
        except ErroESP:
            return None

    def hora(self, agora: int) -> dict:
        _, b = self._req("POST", "/api/hora", {"agora": int(agora)})
        return _json(b)

    def _lote(self, rota, desde, maximo):
        hd, b = self._req("GET", f"{rota}?desde={int(desde)}&max={int(maximo)}")
        cols, linhas = ler_csv_texto(b.decode("utf-8", "replace"))
        if "n" not in cols:
            raise ErroESP(f"CSV sem coluna 'n' em {rota}")
        try:
            ultimo = int(hd.get("x-ultimo-n") or 0)
        except ValueError:
            ultimo = 0
        if linhas:
            ultimo = max(ultimo, max(int(float(l["n"])) for l in linhas))
        return cols, linhas, ultimo, hd.get("x-mais", "0").strip() == "1", hd.get("x-serie", "").strip()

    def dados(self, desde, maximo=LOTE_MAX):
        return self._lote("/api/dados", desde, maximo)

    def eventos(self, desde, maximo=LOTE_MAX):
        return self._lote("/api/eventos", desde, maximo)

    def coletado(self, ate, coletor) -> dict:
        _, b = self._req("POST", "/api/coletado", {"ate": int(ate), "coletor": coletor})
        return _json(b)


# ============================================================================
# Coleta (spec §7: info -> hora -> dados -> eventos -> coletado -> salvar)
# ============================================================================
def _parar(evento):
    if evento is not None and evento.is_set():
        raise Interrompido("Operação interrompida. Os dados já baixados foram guardados; a próxima coleta continua daí.")


def _baixar_amostras(esp, armazem, estado, serie, desde, ate, log, parar, rotulo):
    """Baixa amostras com n > desde (e n <= ate, se ate) em lotes, gravando a cada lote.
    Devolve (linhas_novas, ultimo_n, arquivos_alterados)."""
    novas_tot, alterados_tot = 0, set()
    while True:
        _parar(parar)
        _, linhas, ultimo, mais, serie_h = esp.dados(desde, LOTE_MAX)
        if serie_h and serie_h != serie:
            raise ErroESP("A série do cartão mudou durante a coleta. Faça uma nova coleta.")
        if ate is not None:
            linhas = [l for l in linhas if int(float(l["n"])) <= ate]
        conv = [amostra_para_nuvem(l, serie) for l in linhas]
        if conv:
            alt = armazem.mesclar_amostras(conv)
            novas_tot += sum(alt.values())
            alterados_tot.update(alt)
            estado.marcar_pendentes(alt.keys())
            # lembra trechos sem hora por partida (boot), para tentar recuperar depois
            sh = estado["sem_hora"].setdefault(serie, {})
            for l, c in zip(linhas, conv):
                if not c["ts"]:
                    b = str(l.get("boot") or "?")
                    n = int(c["n"])
                    r = sh.setdefault(b, [n, n])
                    r[0], r[1] = min(r[0], n), max(r[1], n)
        if ultimo <= desde and mais:
            raise ErroESP("O ESP indicou que há mais dados mas não avançou a numeração.")
        desde = max(desde, ultimo)
        if ate is None:
            estado.serie("esp", serie)["ultimo_n"] = desde
        estado.salvar()
        if linhas:
            log(f"{rotulo}: {len(linhas)} linhas recebidas (n até {desde}).")
        if not mais or (ate is not None and desde >= ate):
            return novas_tot, desde, alterados_tot


def _recuperar_sem_hora(esp, armazem, estado, serie, boot_atual, log, parar):
    """Linhas guardadas sem ts na partida atual podem ganhar ts depois que o ESP recebe a hora
    (âncora do mesmo boot, spec §4). Baixa de novo só esse trecho. Partidas antigas não têm
    mais como receber âncora e são esquecidas (as linhas continuam em sem_hora.csv)."""
    sh = estado["sem_hora"].get(serie, {})
    for b in list(sh):
        if b != str(boot_atual) and b.isdigit() and int(b) < int(boot_atual):
            del sh[b]
    faixa = sh.get(str(boot_atual))
    if not faixa:
        return 0
    a, z = faixa
    log(f"Tentando recuperar a hora de linhas sem hora (n {a}–{z}).")
    antes = sum(1 for k in armazem.ler(ARQ_SEM_HORA) if k[0] == serie and a <= k[1] <= z)
    # apaga a faixa; _baixar_amostras a recria só com as linhas que continuarem sem hora
    del sh[str(boot_atual)]
    _baixar_amostras(esp, armazem, estado, serie, a - 1, z, log, parar, "Recuperação de hora")
    restantes = sum(1 for k in armazem.ler(ARQ_SEM_HORA) if k[0] == serie and a <= k[1] <= z)
    estado.salvar()
    log(f"Hora recuperada para {max(0, antes - restantes)} linhas; {restantes} continuam sem hora.")
    return max(0, antes - restantes)


def coletar(cfg, base, log=print, parar=None, acertar_hora=None, esp=None) -> dict:
    """Coleta completa a partir do ESP. Retomável: cada lote é gravado e o estado é salvo."""
    base = Path(base)
    esp = esp or ClienteESP(cfg["esp_url"], cfg["esp_token"], int(cfg.get("timeout_esp_s") or 15))
    if not esp.token:
        raise ErroESP("Configure o token do ESP em Configurações antes de coletar.")
    acertar = cfg.get("acertar_hora", True) if acertar_hora is None else acertar_hora
    armazem, estado = ArmazemLocal(base), Estado(base)
    resumo = {"serie": "", "novas": 0, "eventos": 0, "hora": "", "n_ultimo_esp": 0, "ultimo_n": 0,
              "sem_hora": 0, "recuperadas": 0, "arquivos": []}

    # 1) info
    info = esp.info()
    serie = str(info["serie"])
    resumo.update(serie=serie, n_ultimo_esp=int(info.get("n_ultimo") or 0))
    if int(info.get("proto") or 1) != PROTO:
        log(f"Aviso: o ESP usa protocolo {info.get('proto')} e este app o {PROTO}; tentando assim mesmo.")
    estado["dispositivo"] = str(info.get("id") or estado.get("dispositivo") or "")
    estado["ultimo_info"] = {k: info.get(k) for k in ("id", "fw", "serie", "boot", "n_primeiro", "n_ultimo",
                                                       "ev_ultimo", "sd_ok", "sd_livre_mb", "hora_ok", "intervalo_s")}
    if estado.get("ultima_serie") and estado["ultima_serie"] != serie:
        log(f"Cartão novo detectado (série {serie}, antes {estado['ultima_serie']}): começando uma série nova.")
    estado["ultima_serie"] = serie
    log(f"ESP {info.get('id', '?')} (fw {info.get('fw', '?')}), série {serie}, "
        f"n {info.get('n_primeiro')}–{info.get('n_ultimo')}, cartão {'ok' if info.get('sd_ok', True) else 'COM ERRO'}.")

    # 2) hora
    hora_ok = bool(info.get("hora_ok"))
    dif = (int(info.get("agora") or 0) - time.time()) if hora_ok else None
    if not acertar:
        resumo["hora"] = "ok" if hora_ok else "desconhecida (acerto desativado)"
    elif not hora_ok or abs(dif) > 2:
        r = esp.hora(int(round(time.time())))
        aj = r.get("ajuste_s")
        resumo["hora"] = "ajustada" + (f" ({aj:+d} s)" if isinstance(aj, int) else " (estava sem hora)")
        hora_ok = True
        log(f"Hora do ESP {resumo['hora']} pelo relógio deste computador.")
    else:
        resumo["hora"] = f"ok (diferença {dif:+.0f} s)"
    estado["ultima_hora"] = resumo["hora"]
    _parar(parar)

    # 3) dados em lotes
    st = estado.serie("esp", serie)
    desde = int(st.get("ultimo_n", 0))
    if resumo["n_ultimo_esp"] < desde:
        log(f"Aviso: o ESP tem n até {resumo['n_ultimo_esp']}, mas já havíamos coletado até {desde} "
            "nesta série. Baixando tudo de novo (sem duplicar).")
        desde = 0
    if desde:
        log(f"Continuando da linha n={desde}.")
    novas, ultimo, alt = _baixar_amostras(esp, armazem, estado, serie, desde, None, log, parar, "Dados")
    resumo["novas"], resumo["ultimo_n"] = novas, ultimo
    arquivos = set(alt)
    if hora_ok:
        resumo["recuperadas"] = _recuperar_sem_hora(esp, armazem, estado, serie, info.get("boot", 0), log, parar)

    # 4) eventos
    _parar(parar)
    ev_desde = int(estado.serie("esp", serie).get("ultimo_ev", 0))
    if int(info.get("ev_ultimo") or 0) < ev_desde:
        ev_desde = 0
    while True:
        _parar(parar)
        _, evs, ev_ult, mais, _s = esp.eventos(ev_desde, LOTE_MAX)
        if evs:
            resumo["eventos"] += armazem.mesclar_eventos([evento_para_nuvem(e, serie) for e in evs])
            estado.marcar_pendentes([ARQ_EVENTOS])
            arquivos.add(ARQ_EVENTOS)
        if ev_ult <= ev_desde and mais:
            raise ErroESP("O ESP indicou mais eventos mas não avançou a numeração.")
        ev_desde = max(ev_desde, ev_ult)
        estado.serie("esp", serie)["ultimo_ev"] = ev_desde
        estado.salvar()
        if not mais:
            break

    # 5) coletado (só auditoria; falha aqui não invalida a coleta)
    try:
        esp.coletado(ultimo, cfg.get("coletor") or nome_padrao_coletor())
    except ErroESP as e:
        log(f"Aviso: não foi possível registrar a coleta no ESP ({e}).")

    # 6) estado final
    estado["ultima_coleta"] = int(time.time())
    estado.salvar()
    resumo["sem_hora"] = sum(1 for k in armazem.ler(ARQ_SEM_HORA) if k[0] == serie)
    resumo["arquivos"] = sorted(arquivos)
    log(f"Coleta concluída: {novas} linhas novas, {resumo['eventos']} eventos novos, "
        f"último n {ultimo} de {resumo['n_ultimo_esp']}"
        + (f", {resumo['sem_hora']} linhas ainda sem hora" if resumo["sem_hora"] else "") + ".")
    return resumo


# ============================================================================
# GitHub
# ============================================================================
def tem_internet(api_base=None, timeout=6) -> bool:
    try:
        http("HEAD", (api_base or CONFIG_PADRAO["gh_api"]).rstrip("/") + "/", timeout=timeout)
        return True
    except (OSError, ValueError):
        return False


class ClienteGitHub:
    def __init__(self, api, dono, repo, ramo, token, timeout=60):
        self.api = (api or CONFIG_PADRAO["gh_api"]).rstrip("/")
        self.dono, self.repo, self.ramo, self.token = dono, repo, ramo or "main", token
        self.timeout = timeout

    def _req(self, metodo, caminho, corpo=None, accept="application/vnd.github+json", url=None):
        cab = {"Accept": accept, "X-GitHub-Api-Version": "2022-11-28"}
        if self.token and url is None:
            cab["Authorization"] = "Bearer " + self.token
        try:
            return http(metodo, url or (self.api + caminho), corpo, cab, self.timeout)
        except (OSError, ValueError) as e:
            raise SemInternet(f"Falha de conexão com o GitHub ({e}).")

    def _erro(self, st, hd, b, contexto):
        msg = _json(b).get("message", "") if b else ""
        if st == 401:
            return ErroGitHub("Token do GitHub inválido ou expirado (401). Crie um novo token e cole em Configurações.")
        if st == 403 and hd.get("x-ratelimit-remaining") == "0":
            return ErroGitHub("Limite de requisições do GitHub atingido (403). Tente de novo em alguns minutos.")
        if st == 403:
            return ErroGitHub("O token não tem permissão (403). Ele precisa de acesso ao repositório "
                              f"{self.dono}/{self.repo} com 'Contents: Read and write'.")
        if st == 404:
            return ErroGitHub(f"Repositório {self.dono}/{self.repo} não encontrado, ou o token não tem acesso a ele (404). "
                              "Confira dono/repositório em Configurações e o acesso do token.")
        return ErroGitHub(f"Erro do GitHub {st} ao {contexto}: {msg[:200]}")

    def _conteudo(self, caminho):
        return f"/repos/{self.dono}/{self.repo}/contents/" + urllib.parse.quote(caminho)

    def verificar_acesso(self):
        st, hd, b = self._req("GET", f"/repos/{self.dono}/{self.repo}")
        if st != 200:
            raise self._erro(st, hd, b, "acessar o repositório")
        perm = _json(b).get("permissions")
        if isinstance(perm, dict) and perm.get("push") is False:
            raise ErroGitHub("O token só pode ler o repositório. Dê a permissão 'Contents: Read and write'.")

    def obter(self, caminho):
        """Devolve (bytes, sha) ou (None, None) se o arquivo não existe."""
        q = "?ref=" + urllib.parse.quote(self.ramo)
        st, hd, b = self._req("GET", self._conteudo(caminho) + q)
        if st == 404:
            return None, None
        if st != 200:
            raise self._erro(st, hd, b, f"ler {caminho}")
        d = _json(b)
        sha = d.get("sha")
        if d.get("encoding") == "base64" and d.get("content"):
            return base64.b64decode(d["content"]), sha
        if d.get("size", 0) == 0 and d.get("encoding") == "base64":
            return b"", sha
        # arquivo > 1 MB: conteúdo não vem no JSON; pede o tipo "raw"
        st2, hd2, b2 = self._req("GET", self._conteudo(caminho) + q, accept="application/vnd.github.raw")
        if st2 == 200:
            return b2, sha
        if d.get("download_url"):
            st3, hd3, b3 = self._req("GET", None, url=d["download_url"], accept="*/*")
            if st3 == 200:
                return b3, sha
        raise self._erro(st2, hd2, b2, f"baixar {caminho} (arquivo grande)")

    def gravar(self, caminho, dados: bytes, sha, mensagem):
        corpo = {"message": mensagem, "content": base64.b64encode(dados).decode("ascii"), "branch": self.ramo}
        if sha:
            corpo["sha"] = sha
        st, hd, b = self._req("PUT", self._conteudo(caminho), corpo)
        if st in (200, 201):
            return _json(b).get("content", {}).get("sha")
        if st == 409 or (st == 422 and "sha" in (_json(b).get("message", "") or "")):
            raise ConflitoSha(caminho)
        raise self._erro(st, hd, b, f"gravar {caminho}")


def _ordem_envio(nome):
    return (0 if RE_MES.match(nome) else (1 if nome == ARQ_SEM_HORA else 2), nome)


def _gravar_com_retentativa(gh, caminho, montar, log, tentativas=5):
    """montar(remoto_bytes) -> (bytes_novos ou None se nada muda, mensagem). Refaz em conflito de sha."""
    for i in range(tentativas):
        remoto, sha = gh.obter(caminho)
        novo, msg = montar(remoto)
        if novo is None:
            return False
        try:
            gh.gravar(caminho, novo, sha, msg)
            return True
        except ConflitoSha:
            log(f"{caminho} mudou na nuvem durante o envio; baixando de novo e mesclando (tentativa {i + 2}).")
            time.sleep(min(2.0, 0.3 * (i + 1)))
    raise ErroGitHub(f"Não foi possível gravar {caminho}: o arquivo continua mudando na nuvem. Tente de novo.")


def enviar(cfg, base, log=print, parar=None, gh=None) -> dict:
    """Envia ao GitHub os arquivos locais alterados, mesclando com o que já está lá, e atualiza index.json."""
    base = Path(base)
    if not cfg.get("gh_token"):
        raise ErroGitHub("Cole o token do GitHub em Configurações antes de enviar.")
    if not tem_internet(cfg.get("gh_api")):
        raise SemInternet("Sem internet: não foi possível alcançar o GitHub. Os dados continuam guardados neste computador.")
    gh = gh or ClienteGitHub(cfg.get("gh_api"), cfg["gh_dono"], cfg["gh_repo"], cfg.get("gh_ramo"), cfg["gh_token"])
    armazem, estado = ArmazemLocal(base), Estado(base)
    coletor = cfg.get("coletor") or nome_padrao_coletor()
    resumo = {"arquivos": [], "linhas": 0}

    # estado antigo sem lista de arquivos, mas com linhas pendentes: reenvia tudo (a mescla evita duplicar)
    if not estado["arquivos_pendentes"] and estado.linhas_pendentes() > 0:
        estado.marcar_pendentes(armazem.arquivos())
    pendentes = sorted(set(estado["arquivos_pendentes"]), key=_ordem_envio)
    if not pendentes:
        log("Nada pendente para enviar.")
        estado["pendente"] = False
        estado.salvar()
        return resumo

    gh.verificar_acesso()
    log(f"Enviando {len(pendentes)} arquivo(s) para {cfg['gh_dono']}/{cfg['gh_repo']}…")
    chaves_ts = None
    for nome in pendentes:
        _parar(parar)
        local = armazem.ler(nome)
        if nome == ARQ_SEM_HORA and chaves_ts is None:
            chaves_ts = armazem.chaves_com_ts()
        contagem = {}

        def montar(remoto_b, nome=nome, local=local, contagem=contagem):
            remoto = interpretar(nome, remoto_b.decode("utf-8", "replace")) if remoto_b is not None else {}
            mescla = dict(remoto)
            if nome == ARQ_SEM_HORA:
                for k in chaves_ts:   # linhas que já ganharam hora saem de sem_hora.csv
                    mescla.pop(k, None)
            novas = [k for k in local if k not in remoto]
            mescla.update(local)
            texto = serializar(nome, mescla).encode("utf-8")
            if remoto_b is not None and texto == remoto_b:
                return None, ""
            if remoto_b is None and not mescla:
                return None, ""
            ns = [k[1] for k in novas] or [k[1] for k in local] or [0]
            contagem["novas"] = len(novas)
            return texto, f"dados: +{len(novas)} linhas ({coletor}, n {min(ns)}–{max(ns)})"

        mudou = _gravar_com_retentativa(gh, "dados/" + nome, montar, log)
        if mudou:
            log(f"dados/{nome}: +{contagem.get('novas', 0)} linhas enviadas.")
            resumo["linhas"] += contagem.get("novas", 0)
            resumo["arquivos"].append(nome)
        else:
            log(f"dados/{nome}: já estava atualizado na nuvem.")
        estado["arquivos_pendentes"] = [x for x in estado["arquivos_pendentes"] if x != nome]
        estado.salvar()

    # index.json
    _parar(parar)
    atualizar_index(gh, armazem, estado, coletor, log)
    estado["nuvem"] = copy.deepcopy(estado["esp"])
    estado["pendente"] = bool(estado["arquivos_pendentes"])
    estado["ultimo_envio"] = int(time.time())
    estado.salvar()
    log(f"Envio concluído: {resumo['linhas']} linhas novas em {len(resumo['arquivos'])} arquivo(s).")
    return resumo


def montar_index(remoto: dict, armazem, estado) -> dict:
    meses = set(m for m in (remoto.get("meses") or []) if isinstance(m, str) and re.match(r"^\d{4}-\d{2}$", m))
    meses.update(armazem.meses())
    series = dict(remoto.get("series") or {}) if isinstance(remoto.get("series"), dict) else {}
    for s, v in estado["esp"].items():
        r = series.get(s) if isinstance(series.get(s), dict) else {}
        series[s] = {"ultimo_n": max(int(r.get("ultimo_n") or 0), int(v.get("ultimo_n") or 0)),
                     "ultimo_ev": max(int(r.get("ultimo_ev") or 0), int(v.get("ultimo_ev") or 0))}
    try:
        ult_remoto = int(remoto.get("ultimo_ts") or 0)
    except (TypeError, ValueError):
        ult_remoto = 0
    return {"proto": PROTO, "dispositivo": estado.get("dispositivo") or remoto.get("dispositivo") or "",
            "meses": sorted(meses), "series": dict(sorted(series.items())),
            "ultimo_ts": max(ult_remoto, armazem.ultimo_ts()), "atualizado": int(time.time()),
            "colunas": list(COLS_NUVEM)}


def atualizar_index(gh, armazem, estado, coletor, log):
    def montar(remoto_b):
        remoto = {}
        if remoto_b:
            try:
                remoto = json.loads(remoto_b.decode("utf-8"))
                if not isinstance(remoto, dict):
                    remoto = {}
            except (ValueError, UnicodeDecodeError):
                log("Aviso: dados/index.json na nuvem estava inválido; será recriado.")
        idx = montar_index(remoto, armazem, estado)
        return (json.dumps(idx, indent=1, ensure_ascii=False) + "\n").encode("utf-8"), \
            f"dados: atualiza index.json ({coletor})"
    _gravar_com_retentativa(gh, "dados/index.json", montar, log)
    log("dados/index.json atualizado.")


# ============================================================================
# Wi-Fi (melhor esforço)
# ============================================================================
def _escapar_xml(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;").replace("'", "&apos;"))


def perfil_wlan_xml(ssid, senha) -> str:
    """Perfil WPA2-Pessoal (AES) para 'netsh wlan add profile'. connectionMode manual: o Windows
    não volta sozinho para a rede do ESP (que não tem internet)."""
    s, p = _escapar_xml(ssid), _escapar_xml(senha)
    return ('<?xml version="1.0"?>\n'
            '<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">\n'
            f'  <name>{s}</name>\n'
            f'  <SSIDConfig><SSID><name>{s}</name></SSID></SSIDConfig>\n'
            '  <connectionType>ESS</connectionType>\n'
            '  <connectionMode>manual</connectionMode>\n'
            '  <MSM><security>\n'
            '    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption>'
            '<useOneX>false</useOneX></authEncryption>\n'
            f'    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{p}</keyMaterial></sharedKey>\n'
            '  </security></MSM>\n'
            '</WLANProfile>\n')


def interpretar_netsh_redes(texto):
    return [m.group(1).strip() for m in re.finditer(r"^\s*SSID\s+\d+\s*:\s*(.*?)\s*$", texto, re.M) if m.group(1).strip()]


def interpretar_netsh_interface(texto):
    """Devolve (ssid, perfil) da primeira interface conectada (saída em inglês ou português)."""
    ssid = perfil = None
    for linha in texto.splitlines():
        m = re.match(r"^\s*(SSID|Profile|Perfil)\s*:\s*(.*?)\s*$", linha)
        if m:
            if m.group(1) == "SSID" and ssid is None:
                ssid = m.group(2)
            elif m.group(1) in ("Profile", "Perfil") and perfil is None:
                perfil = m.group(2)
    return ssid or None, perfil or ssid or None


def _dividir_nmcli(linha):
    return [p.replace("\\:", ":").replace("\\\\", "\\") for p in re.split(r"(?<!\\):", linha)]


class GerenciadorWifi:
    """Detecta a rede do ESP, conecta e depois volta para a rede anterior."""

    def __init__(self, log=print, executar=None):
        self.log = log
        self.sistema = platform.system()
        self._executar = executar or self._executar_padrao
        self._mac_dev = None

    @staticmethod
    def _executar_padrao(args, timeout=30):
        kw = {}
        if sys.platform.startswith("win"):
            kw["creationflags"] = 0x08000000   # CREATE_NO_WINDOW: sem janela preta piscando
        try:
            p = subprocess.run(args, capture_output=True, timeout=timeout, **kw)
        except (OSError, subprocess.TimeoutExpired) as e:
            return 1, str(e)
        enc = "utf-8"
        if sys.platform.startswith("win"):
            try:
                import ctypes
                enc = "cp%d" % ctypes.windll.kernel32.GetOEMCP()
            except Exception:
                enc = "cp850"
        return p.returncode, (p.stdout + p.stderr).decode(enc, "replace")

    def disponivel(self) -> bool:
        prog = {"Windows": "netsh", "Linux": "nmcli", "Darwin": "networksetup"}.get(self.sistema)
        return bool(prog and shutil.which(prog))

    # --- macOS: descobre o dispositivo Wi-Fi (en0, en1...)
    def _dispositivo_mac(self):
        if self._mac_dev:
            return self._mac_dev
        rc, out = self._executar(["networksetup", "-listallhardwareports"])
        m = re.search(r"Hardware Port:\s*(Wi-Fi|AirPort)\s*\nDevice:\s*(\S+)", out)
        self._mac_dev = m.group(2) if m else "en0"
        return self._mac_dev

    def redes_visiveis(self):
        """Lista de SSIDs visíveis, ou None se não for possível saber."""
        if self.sistema == "Windows":
            rc, out = self._executar(["netsh", "wlan", "show", "networks"])
            return interpretar_netsh_redes(out) if rc == 0 else None
        if self.sistema == "Linux":
            rc, out = self._executar(["nmcli", "-t", "-f", "SSID", "dev", "wifi", "list", "--rescan", "yes"])
            if rc != 0:
                rc, out = self._executar(["nmcli", "-t", "-f", "SSID", "dev", "wifi", "list"])
            return [_dividir_nmcli(l)[0] for l in out.splitlines() if l.strip()] if rc == 0 else None
        if self.sistema == "Darwin":
            airport = ("/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport")
            if os.path.exists(airport):
                rc, out = self._executar([airport, "-s"])
                if rc == 0 and out.strip():
                    return [l[:32].strip() for l in out.splitlines()[1:] if l.strip()]
            return None
        return None

    def rede_atual(self):
        """(ssid, identificador para voltar) da rede Wi-Fi atual, ou (None, None)."""
        if self.sistema == "Windows":
            rc, out = self._executar(["netsh", "wlan", "show", "interfaces"])
            return interpretar_netsh_interface(out) if rc == 0 else (None, None)
        if self.sistema == "Linux":
            rc, out = self._executar(["nmcli", "-t", "-f", "NAME,TYPE", "con", "show", "--active"])
            nome = None
            if rc == 0:
                for l in out.splitlines():
                    p = _dividir_nmcli(l)
                    if len(p) >= 2 and p[1] == "802-11-wireless":
                        nome = p[0]
                        break
            rc, out = self._executar(["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"])
            ssid = None
            if rc == 0:
                for l in out.splitlines():
                    p = _dividir_nmcli(l)
                    if len(p) >= 2 and p[0] in ("yes", "sim"):
                        ssid = p[1]
                        break
            return ssid, nome or ssid
        if self.sistema == "Darwin":
            rc, out = self._executar(["networksetup", "-getairportnetwork", self._dispositivo_mac()])
            m = re.search(r":\s*(.+)$", out.strip()) if rc == 0 and "not associated" not in out else None
            s = m.group(1).strip() if m else None
            return s, s
        return None, None

    def encontrar_ap(self, ssid_cfg=""):
        redes = self.redes_visiveis()
        if redes is None:
            return ssid_cfg or None     # não dá para listar: tenta direto se o nome for conhecido
        if ssid_cfg:
            return ssid_cfg if ssid_cfg in redes else None
        for r in redes:
            if r.startswith(PREFIXO_AP):
                return r
        return None

    def conectar(self, ssid, senha) -> bool:
        if self.sistema == "Windows":
            fd, tmp = tempfile.mkstemp(suffix=".xml")
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    f.write(perfil_wlan_xml(ssid, senha))
                rc, out = self._executar(["netsh", "wlan", "add", "profile", f"filename={tmp}", "user=current"])
            finally:
                try:
                    os.unlink(tmp)          # o arquivo tem a senha
                except OSError:
                    pass
            if rc != 0:
                self.log(f"Não foi possível criar o perfil Wi-Fi: {out.strip()[:200]}")
                return False
            rc, out = self._executar(["netsh", "wlan", "connect", f"name={ssid}", f"ssid={ssid}"])
        elif self.sistema == "Linux":
            rc, out = self._executar(["nmcli", "dev", "wifi", "connect", ssid, "password", senha], timeout=60)
        elif self.sistema == "Darwin":
            rc, out = self._executar(["networksetup", "-setairportnetwork", self._dispositivo_mac(), ssid, senha], 60)
            if rc == 0 and ("Could not" in out or "Error" in out):
                rc = 1
        else:
            return False
        if rc != 0:
            self.log(f"Falha ao conectar em {ssid}: {out.strip()[:200]}")
            return False
        return True

    def restaurar(self, ident) -> bool:
        if not ident:
            return False
        if self.sistema == "Windows":
            rc, out = self._executar(["netsh", "wlan", "connect", f"name={ident}"])
        elif self.sistema == "Linux":
            rc, out = self._executar(["nmcli", "con", "up", "id", ident], timeout=60)
        elif self.sistema == "Darwin":
            rc, out = self._executar(["networksetup", "-setairportnetwork", self._dispositivo_mac(), ident], 60)
        else:
            return False
        if rc != 0:
            self.log(f"Não consegui voltar para a rede {ident}: {out.strip()[:200]}. Reconecte manualmente.")
            return False
        self.log(f"De volta à rede {ident}.")
        return True


def coletar_com_wifi(cfg, base, log=print, parar=None, wifi=None, esp=None, espera_s=40) -> dict:
    """Coleta usando a rede atual se o ESP já responde; senão (se permitido) conecta na rede
    Biodigestor-XXXX, coleta e volta para a rede anterior."""
    esp = esp or ClienteESP(cfg["esp_url"], cfg["esp_token"], int(cfg.get("timeout_esp_s") or 15))
    if esp.alcancavel(timeout=3):
        return coletar(cfg, base, log, parar, esp=esp)
    if not cfg.get("trocar_wifi", True):
        raise ErroESP("ESP não alcançável. Conecte o computador à rede Wi-Fi Biodigestor-XXXX "
                      "(senha padrão biogas123) e tente de novo.")
    wifi = wifi or GerenciadorWifi(log)
    if not wifi.disponivel():
        raise ErroESP("ESP não alcançável e não sei trocar de rede neste sistema. "
                      "Conecte-se manualmente à rede Biodigestor-XXXX e tente de novo.")
    ssid = wifi.encontrar_ap(cfg.get("ap_ssid", ""))
    if not ssid:
        raise ErroESP("Rede do biodigestor não encontrada. Aproxime-se da caixa (até ~30 m) ou "
                      "conecte-se manualmente à rede Biodigestor-XXXX.")
    _ssid_ant, anterior = wifi.rede_atual()
    if _ssid_ant == ssid:
        anterior = None
    log(f"Conectando à rede {ssid}…")
    if not wifi.conectar(ssid, cfg.get("ap_senha") or CONFIG_PADRAO["ap_senha"]):
        raise ErroESP(f"Não consegui conectar automaticamente em {ssid}. Conecte-se manualmente "
                      "(senha padrão biogas123) e clique em Coletar agora.")
    try:
        limite = time.time() + espera_s
        while not esp.alcancavel(timeout=3):
            _parar(parar)
            if time.time() > limite:
                raise ErroESP(f"Conectado em {ssid}, mas o ESP não respondeu em {esp.url}.")
            time.sleep(2)
        return coletar(cfg, base, log, parar, esp=esp)
    finally:
        if anterior:
            log(f"Voltando para a rede {anterior}…")
            wifi.restaurar(anterior)


# ============================================================================
# Situação e modo automático
# ============================================================================
def situacao(base) -> dict:
    estado, armazem = Estado(base), ArmazemLocal(base)
    serie = estado.get("ultima_serie") or ""
    return {
        "serie": serie,
        "coletado_n": int(estado["esp"].get(serie, {}).get("ultimo_n", 0)) if serie else 0,
        "esp_n": int((estado.get("ultimo_info") or {}).get("n_ultimo") or 0),
        "hora": estado.get("ultima_hora") or "",
        "ultima_coleta": int(estado.get("ultima_coleta") or 0),
        "ultimo_envio": int(estado.get("ultimo_envio") or 0),
        "linhas_pendentes": estado.linhas_pendentes(),
        "arquivos_pendentes": list(estado["arquivos_pendentes"]),
        "meses": armazem.meses(),
        "pasta": str(Path(base)),
    }


def formatar_data(ts) -> str:
    if not ts:
        return "nunca"
    return datetime.datetime.fromtimestamp(int(ts)).strftime("%d/%m/%Y %H:%M")


class ModoAutomatico:
    """A cada 30 s: se a rede do ESP estiver por perto e a última coleta for mais antiga que
    N minutos, coleta; se houver internet e dados pendentes, envia."""

    def __init__(self, obter_cfg, base, log=print, ao_terminar=None, intervalo_s=30, wifi=None):
        self.obter_cfg, self.base, self.log = obter_cfg, Path(base), log
        self.ao_terminar = ao_terminar or (lambda: None)
        self.intervalo_s = intervalo_s
        self.wifi = wifi
        self._parar = threading.Event()
        self._thread = None

    @property
    def ativo(self):
        return self._thread is not None and self._thread.is_alive()

    def iniciar(self):
        if self.ativo:
            return
        self._parar.clear()
        self._thread = threading.Thread(target=self._laco, name="modo-automatico", daemon=True)
        self._thread.start()
        self.log("Modo automático ligado.")

    def parar(self):
        self._parar.set()
        self.log("Modo automático desligado.")

    def _laco(self):
        while not self._parar.is_set():
            try:
                self.ciclo()
            except Exception as e:  # nunca derruba o laço
                self.log(f"Modo automático: erro inesperado: {e}")
            self._parar.wait(self.intervalo_s)

    def ciclo(self):
        if not OPERACAO.acquire(blocking=False):
            return   # outra operação em andamento
        try:
            cfg = self.obter_cfg()
            estado = Estado(self.base)
            atraso = max(1, int(cfg.get("auto_intervalo_min") or 60)) * 60
            if time.time() - int(estado.get("ultima_coleta") or 0) >= atraso and cfg.get("esp_token"):
                esp = ClienteESP(cfg["esp_url"], cfg["esp_token"], int(cfg.get("timeout_esp_s") or 15))
                tentar = esp.alcancavel(timeout=3) is not None
                wifi = None
                if not tentar and cfg.get("trocar_wifi", True):
                    wifi = self.wifi or GerenciadorWifi(self.log)
                    tentar = wifi.disponivel() and bool(wifi.encontrar_ap(cfg.get("ap_ssid", "")))
                if tentar:
                    self.log("Modo automático: ESP por perto, coletando.")
                    try:
                        coletar_com_wifi(cfg, self.base, self.log, self._parar, wifi=wifi, esp=esp)
                    except ErroColetor as e:
                        self.log(f"Modo automático: {e}")
                    self.ao_terminar()
            estado = Estado(self.base)
            if (estado.get("pendente") or estado["arquivos_pendentes"]) and cfg.get("gh_token") \
                    and tem_internet(cfg.get("gh_api")):
                self.log("Modo automático: há internet e dados pendentes, enviando.")
                try:
                    enviar(cfg, self.base, self.log, self._parar)
                except ErroColetor as e:
                    self.log(f"Modo automático: {e}")
                self.ao_terminar()
        finally:
            OPERACAO.release()
