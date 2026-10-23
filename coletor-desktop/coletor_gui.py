"""Janela (tkinter) do Coletor do Biodigestor. Toda a lógica está em coletor_core."""
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

import coletor_core as core

AZUL = "#2b7bd6"
AZUL_ESCURO = "#1f5fa8"
VERDE = "#1e8e3e"
VERMELHO = "#c5221f"
CINZA = "#5f6368"
FUNDO = "#f4f6f9"

AJUDA_TOKEN = """Faça isto uma vez só, num computador com internet.

1. Entre em github.com com a conta DONA do repositório (lribeiro770-debug). Tokens "fine-grained" só enxergam repositórios do próprio dono.

2. Clique na sua foto (canto superior direito) → Settings.

3. No fim do menu à esquerda: Developer settings → Personal access tokens → Fine-grained tokens → Generate new token.

4. Token name: "Coletor biodigestor". Expiration: escolha uma data (ex.: 1 ano) e anote; depois dela é preciso criar outro token.

5. Repository access: "Only select repositories" → biodigestor-painel.

6. Permissions → Repository permissions → Contents: "Read and write". (Metadata: Read-only é marcado automaticamente.)

7. Clique em Generate token, copie o token (começa com github_pat_) e cole no campo "Token do GitHub". O GitHub não mostra o token de novo.

O token fica salvo só neste computador. Não o compartilhe nem o coloque em arquivos do repositório."""


def _abrir_pasta(caminho):
    try:
        if sys.platform.startswith("win"):
            os.startfile(caminho)  # type: ignore[attr-defined]
        elif sys.platform == "darwin":
            subprocess.Popen(["open", caminho])
        else:
            subprocess.Popen(["xdg-open", caminho])
    except OSError as e:
        messagebox.showerror("Abrir pasta", f"Não foi possível abrir a pasta:\n{caminho}\n\n{e}")


class App:
    def __init__(self, root, base=None):
        self.root = root
        self.base = core.Path(base) if base else core.pasta_padrao()
        self.base.mkdir(parents=True, exist_ok=True)
        self.cfg = core.Config(self.base)
        self.log = core.Registro(self.base, self.cfg)
        self.fila = queue.Queue()
        self.log.ouvintes.append(lambda linha: self.fila.put(("log", linha)))
        self.parar = threading.Event()
        self.ocupado = False
        self.esp_info = None
        self.internet = None
        self.auto = core.ModoAutomatico(lambda: dict(self.cfg), self.base, self.log,
                                        ao_terminar=lambda: self.fila.put(("atualizar", None)))
        self._estilos()
        self._montar()
        self.atualizar_tela()
        self.root.after(100, self._processar_fila)
        threading.Thread(target=self._sondagem, daemon=True).start()
        self.root.protocol("WM_DELETE_WINDOW", self.fechar)
        self.log(f"Coletor do Biodigestor {core.VERSAO}. Dados em {self.base}")
        if not self.cfg.get("esp_token") or not self.cfg.get("gh_token"):
            self.root.after(400, lambda: self.log("Primeiro uso: abra Configurações e preencha o token do ESP "
                                                  "e o token do GitHub."))

    # ------------------------------------------------------------------ visual
    def _estilos(self):
        self.root.title("Coletor do Biodigestor")
        self.root.geometry("1180x700")
        self.root.minsize(960, 600)
        self.root.configure(bg=FUNDO)
        st = ttk.Style(self.root)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        base = ("Segoe UI", 10) if sys.platform.startswith("win") else ("TkDefaultFont", 10)
        st.configure(".", font=base, background=FUNDO)
        st.configure("TFrame", background=FUNDO)
        st.configure("TLabelframe", background=FUNDO, padding=10)
        st.configure("TLabelframe.Label", background=FUNDO, foreground=AZUL_ESCURO, font=(base[0], 11, "bold"))
        st.configure("TLabel", background=FUNDO)
        st.configure("TCheckbutton", background=FUNDO, font=(base[0], 11))
        st.configure("Titulo.TLabel", font=(base[0], 18, "bold"), foreground=AZUL_ESCURO)
        st.configure("Sub.TLabel", foreground=CINZA)
        st.configure("Chave.TLabel", foreground=CINZA)
        st.configure("Valor.TLabel", font=(base[0], 10, "bold"))
        st.configure("Ok.TLabel", font=(base[0], 10, "bold"), foreground=VERDE)
        st.configure("Ruim.TLabel", font=(base[0], 10, "bold"), foreground=VERMELHO)
        st.configure("Grande.TButton", font=(base[0], 13, "bold"), padding=(18, 12), foreground="white",
                     background=AZUL, borderwidth=0)
        st.map("Grande.TButton", background=[("disabled", "#a7c4e8"), ("active", AZUL_ESCURO)],
               foreground=[("disabled", "#eef3fa")])
        st.configure("TButton", padding=(10, 6))
        st.configure("Treeview", rowheight=24, font=base)
        st.configure("Treeview.Heading", font=(base[0], 10, "bold"))

    def _linha(self, pai, r, chave):
        ttk.Label(pai, text=chave, style="Chave.TLabel").grid(row=r, column=0, sticky="w", pady=2, padx=(0, 12))
        v = ttk.Label(pai, text="—", style="Valor.TLabel")
        v.grid(row=r, column=1, sticky="w", pady=2)
        return v

    def _montar(self):
        topo = ttk.Frame(self.root, padding=(16, 12, 16, 4))
        topo.pack(fill="x")
        ttk.Label(topo, text="Coletor do Biodigestor", style="Titulo.TLabel").pack(side="left")
        ttk.Label(topo, text="   UTFPR · coleta offline do ESP32 e envio ao painel", style="Sub.TLabel").pack(side="left", pady=(8, 0))
        ttk.Button(topo, text="Configurações", command=self.abrir_config).pack(side="right")
        ttk.Button(topo, text="Abrir pasta de dados", command=lambda: _abrir_pasta(str(self.base))).pack(side="right", padx=8)

        st = ttk.Frame(self.root, padding=(16, 4))
        st.pack(fill="x")
        st.columnconfigure(0, weight=1)
        st.columnconfigure(1, weight=1)
        fe = ttk.LabelFrame(st, text="Biodigestor (ESP)")
        fe.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        self.v_esp = self._linha(fe, 0, "Situação")
        self.v_serie = self._linha(fe, 1, "Série do cartão")
        self.v_n = self._linha(fe, 2, "Último n no ESP / coletado")
        self.v_hora = self._linha(fe, 3, "Hora do ESP")
        self.v_coleta = self._linha(fe, 4, "Última coleta")
        fn = ttk.LabelFrame(st, text="Nuvem (GitHub)")
        fn.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        self.v_internet = self._linha(fn, 0, "Internet")
        self.v_pend = self._linha(fn, 1, "Pendentes de envio")
        self.v_envio = self._linha(fn, 2, "Último envio")
        self.v_repo = self._linha(fn, 3, "Repositório")
        self.v_token = self._linha(fn, 4, "Token do GitHub")

        ac = ttk.Frame(self.root, padding=(16, 10))
        ac.pack(fill="x")
        self.b_coletar = ttk.Button(ac, text="Coletar agora", style="Grande.TButton", command=self.coletar)
        self.b_coletar.pack(side="left")
        self.b_enviar = ttk.Button(ac, text="Enviar para a nuvem", style="Grande.TButton", command=self.enviar)
        self.b_enviar.pack(side="left", padx=12)
        self.var_auto = tk.BooleanVar(value=False)
        ttk.Checkbutton(ac, text="Modo automático", variable=self.var_auto, command=self.alternar_auto).pack(side="left", padx=8)
        self.b_parar = ttk.Button(ac, text="Interromper", command=self.interromper, state="disabled")
        self.b_parar.pack(side="right")
        self.lbl_op = ttk.Label(ac, text="", style="Sub.TLabel")
        self.lbl_op.pack(side="right", padx=8)
        self.prog = ttk.Progressbar(ac, mode="indeterminate", length=140)
        self.prog.pack(side="right")

        pw = ttk.PanedWindow(self.root, orient="horizontal")
        pw.pack(fill="both", expand=True, padx=16, pady=(0, 14))
        ft = ttk.LabelFrame(pw, text="Últimas leituras guardadas")
        cols = ("hora", "t_med", "t_amb", "ur_amb", "p_pa", "alarmes")
        tit = ("Data/hora", "T méd °C", "T amb °C", "UR %", "Pressão Pa", "Alarmes")
        larg = (96, 66, 66, 48, 76, 110)
        self.tabela = ttk.Treeview(ft, columns=cols, show="headings", height=12)
        for c, t, w in zip(cols, tit, larg):
            self.tabela.heading(c, text=t)
            self.tabela.column(c, width=w, anchor="w" if c in ("hora", "alarmes") else "e", stretch=True,
                               minwidth=w if c == "hora" else 40)
        self.tabela.tag_configure("alarme", foreground=VERMELHO)
        sb = ttk.Scrollbar(ft, orient="vertical", command=self.tabela.yview)
        self.tabela.configure(yscrollcommand=sb.set)
        self.tabela.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        fl = ttk.LabelFrame(pw, text="Registro")
        self.txt = ScrolledText(fl, height=12, wrap="word", font=("Consolas", 9) if sys.platform.startswith("win")
                                else ("TkFixedFont", 9), relief="flat", background="white")
        self.txt.pack(fill="both", expand=True)
        self.txt.configure(state="disabled")
        self.txt.tag_configure("erro", foreground=VERMELHO)
        pw.add(ft, weight=3)
        pw.add(fl, weight=2)
        # tabela com ~55% da largura no início (o usuário pode arrastar a divisória)
        self.root.after(200, lambda: pw.sashpos(0, max(480, int(pw.winfo_width() * 0.55))))

    # ------------------------------------------------------------- atualização
    def _escrever_log(self, linha):
        self.txt.configure(state="normal")
        self.txt.insert("end", linha + "\n", ("erro",) if "ERRO" in linha else ())
        n = int(self.txt.index("end-1c").split(".")[0])
        if n > 2000:
            self.txt.delete("1.0", f"{n - 2000}.0")
        self.txt.see("end")
        self.txt.configure(state="disabled")

    def _processar_fila(self):
        try:
            while True:
                tipo, dado = self.fila.get_nowait()
                if tipo == "log":
                    self._escrever_log(dado)
                elif tipo == "atualizar":
                    self.atualizar_tela()
                elif tipo == "fim":
                    self._fim_operacao(*dado)
                elif tipo == "chamar":
                    dado()
        except queue.Empty:
            pass
        self.root.after(100, self._processar_fila)

    def _sondagem(self):
        """Em segundo plano: vê se o ESP responde (a cada 10 s) e se há internet (a cada 60 s)."""
        ultimo_net = 0
        while True:
            if not self.ocupado:
                info = core.ClienteESP(self.cfg["esp_url"], self.cfg["esp_token"]).alcancavel(timeout=2)
                self.esp_info = info
                if time.time() - ultimo_net > 60:
                    self.internet = core.tem_internet(self.cfg.get("gh_api"), timeout=5)
                    ultimo_net = time.time()
                self.fila.put(("atualizar", None))
            time.sleep(10)

    def atualizar_tela(self):
        try:
            s = core.situacao(self.base)
        except Exception as e:  # estado ilegível não deve fechar a janela
            self.log(f"ERRO ao ler o estado local: {e}")
            return
        info = self.esp_info
        if info:
            self.v_esp.configure(text=f"alcançável ({info.get('id', '?')}, fw {info.get('fw', '?')})", style="Ok.TLabel")
            self.v_serie.configure(text=str(info.get("serie")))
            col = int(core.Estado(self.base)["esp"].get(str(info.get("serie")), {}).get("ultimo_n", 0))
            falta = max(0, int(info.get("n_ultimo") or 0) - col)
            self.v_n.configure(text=f"{info.get('n_ultimo')} / {col}" + (f"  ({falta} novas)" if falta else "  (em dia)"))
            self.v_hora.configure(text=("certa" if info.get("hora_ok") else "SEM HORA (será acertada na coleta)"),
                                  style="Valor.TLabel" if info.get("hora_ok") else "Ruim.TLabel")
        else:
            self.v_esp.configure(text="não alcançável (conecte-se à rede Biodigestor-XXXX)", style="Ruim.TLabel")
            self.v_serie.configure(text=s["serie"] or "—")
            self.v_n.configure(text=f"{s['esp_n'] or '—'} / {s['coletado_n']}  (na última coleta)")
            self.v_hora.configure(text=s["hora"] or "—", style="Valor.TLabel")
        self.v_coleta.configure(text=core.formatar_data(s["ultima_coleta"]))
        self.v_internet.configure(text={None: "verificando…", True: "disponível", False: "sem internet"}[self.internet],
                                  style={True: "Ok.TLabel", False: "Ruim.TLabel"}.get(self.internet, "Valor.TLabel"))
        pend = s["linhas_pendentes"]
        arqs = len(s["arquivos_pendentes"])
        self.v_pend.configure(text=(f"{pend} linhas em {arqs} arquivo(s)" if arqs else "nada pendente"),
                              style="Ruim.TLabel" if arqs else "Ok.TLabel")
        self.v_envio.configure(text=core.formatar_data(s["ultimo_envio"]))
        self.v_repo.configure(text=f"{self.cfg['gh_dono']}/{self.cfg['gh_repo']} ({self.cfg['gh_ramo']})")
        self.v_token.configure(text="configurado" if self.cfg.get("gh_token") else "FALTANDO (veja Configurações)",
                               style="Valor.TLabel" if self.cfg.get("gh_token") else "Ruim.TLabel")
        self._tabela()

    def _tabela(self):
        # só relê os CSV quando algum arquivo mudou (um mês tem ~45 mil linhas)
        armazem = core.ArmazemLocal(self.base)
        try:
            marca = tuple((n, (armazem.pasta / n).stat().st_mtime_ns) for n in armazem.arquivos())
            if marca == getattr(self, "_marca_tabela", None):
                return
            linhas = armazem.ultimas_amostras(30)
        except OSError:
            return
        self._marca_tabela = marca
        self.tabela.delete(*self.tabela.get_children())
        for l in linhas:
            al = (l.get("alarmes") or "").replace("|", ", ")
            self.tabela.insert("", "end", tags=("alarme",) if al else (), values=(
                time.strftime("%d/%m %H:%M", time.localtime(int(l["ts"]))) if l.get("ts") else "sem hora",
                l.get("t_med", ""), l.get("t_amb", ""), l.get("ur_amb", ""), l.get("p_pa", ""), al or "—"))

    # ---------------------------------------------------------------- operações
    def _rodar(self, rotulo, funcao):
        if self.ocupado:
            return
        if not core.OPERACAO.acquire(blocking=False):
            messagebox.showinfo("Aguarde", "O modo automático está fazendo uma operação. Tente em instantes.")
            return
        self.ocupado = True
        self.parar.clear()
        self.b_coletar.configure(state="disabled")
        self.b_enviar.configure(state="disabled")
        self.b_parar.configure(state="normal")
        self.lbl_op.configure(text=rotulo + "…")
        self.prog.start(12)

        def trabalho():
            erro = None
            try:
                funcao()
            except core.ErroColetor as e:
                erro = str(e)
            except Exception as e:  # erro inesperado: registra e segue
                erro = f"Erro inesperado: {e!r}"
            finally:
                core.OPERACAO.release()
            if erro:
                self.log("ERRO: " + erro)
            self.fila.put(("fim", (rotulo, erro)))

        threading.Thread(target=trabalho, daemon=True).start()

    def _fim_operacao(self, rotulo, erro):
        self.ocupado = False
        self.prog.stop()
        self.b_coletar.configure(state="normal")
        self.b_enviar.configure(state="normal")
        self.b_parar.configure(state="disabled")
        self.lbl_op.configure(text=f"{rotulo}: {'falhou' if erro else 'concluído'}")
        if erro and not self.var_auto.get():
            messagebox.showwarning(rotulo, erro)
        threading.Thread(target=self._sondar_uma_vez, daemon=True).start()
        self.atualizar_tela()

    def _sondar_uma_vez(self):
        self.esp_info = core.ClienteESP(self.cfg["esp_url"], self.cfg["esp_token"]).alcancavel(timeout=2)
        self.internet = core.tem_internet(self.cfg.get("gh_api"), timeout=5)
        self.fila.put(("atualizar", None))

    def coletar(self):
        if not self.cfg.get("esp_token"):
            messagebox.showinfo("Coletar", "Preencha o token do ESP em Configurações.")
            return self.abrir_config()
        self._rodar("Coleta", lambda: core.coletar_com_wifi(dict(self.cfg), self.base, self.log, self.parar))

    def enviar(self):
        if not self.cfg.get("gh_token"):
            messagebox.showinfo("Enviar", "Cole o token do GitHub em Configurações.")
            return self.abrir_config()
        self._rodar("Envio", lambda: core.enviar(dict(self.cfg), self.base, self.log, self.parar))

    def interromper(self):
        self.parar.set()
        self.log("Interrompendo depois do lote atual…")

    def alternar_auto(self):
        if self.var_auto.get():
            if not self.cfg.get("esp_token"):
                self.var_auto.set(False)
                messagebox.showinfo("Modo automático", "Preencha o token do ESP em Configurações.")
                return
            self.auto.iniciar()
            self.log(f"Coleta a cada {self.cfg.get('auto_intervalo_min')} min quando a rede do ESP estiver por "
                     "perto; envio sempre que houver internet e dados pendentes.")
        else:
            self.auto.parar()

    def fechar(self):
        if self.ocupado and not messagebox.askyesno(
                "Sair", "Há uma operação em andamento. Sair mesmo assim?\n"
                        "(Os dados já baixados ficam guardados; a próxima coleta continua daí.)"):
            return
        self.parar.set()
        self.auto.parar()
        self.root.destroy()

    def abrir_config(self):
        DialogoConfig(self)


class DialogoConfig:
    CAMPOS = [
        ("Biodigestor (ESP)", [
            ("esp_url", "Endereço do ESP", False),
            ("esp_token", "Token do ESP", True),
            ("ap_ssid", "Rede Wi-Fi do ESP (vazio = automático)", False),
            ("ap_senha", "Senha da rede Wi-Fi do ESP", True),
        ]),
        ("GitHub", [
            ("gh_dono", "Dono do repositório", False),
            ("gh_repo", "Repositório", False),
            ("gh_ramo", "Ramo", False),
            ("gh_token", "Token do GitHub", True),
            ("gh_api", "Endereço da API", False),
        ]),
        ("Coletor", [
            ("coletor", "Seu nome (vai no registro)", False),
            ("auto_intervalo_min", "Minutos entre coletas (automático)", False),
        ]),
    ]

    def __init__(self, app):
        self.app = app
        self.top = tk.Toplevel(app.root)
        self.top.title("Configurações")
        self.top.configure(bg=FUNDO)
        self.top.transient(app.root)
        self.top.geometry("980x640")
        self.top.minsize(860, 560)
        self.vars, self.entradas = {}, []
        corpo = ttk.Frame(self.top, padding=14)
        corpo.pack(fill="both", expand=True)
        corpo.columnconfigure(0, weight=1)
        corpo.columnconfigure(1, weight=1)
        corpo.rowconfigure(0, weight=1)
        esq = ttk.Frame(corpo)
        esq.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        esq.columnconfigure(0, weight=1)
        for i, (grupo, campos) in enumerate(self.CAMPOS):
            lf = ttk.LabelFrame(esq, text=grupo)
            lf.grid(row=i, column=0, sticky="ew", pady=(0, 8))
            lf.columnconfigure(0, minsize=250)
            lf.columnconfigure(1, weight=1)
            for r, (k, rot, secreto) in enumerate(campos):
                ttk.Label(lf, text=rot, style="Chave.TLabel").grid(row=r, column=0, sticky="w", pady=3, padx=(0, 8))
                v = tk.StringVar(value=str(app.cfg.get(k, "")))
                e = ttk.Entry(lf, textvariable=v, show="•" if secreto else "", width=34)
                e.grid(row=r, column=1, sticky="ew", pady=3)
                self.vars[k] = v
                if secreto:
                    self.entradas.append(e)
        op = ttk.Frame(esq)
        op.grid(row=3, column=0, sticky="ew")
        self.var_hora = tk.BooleanVar(value=bool(app.cfg.get("acertar_hora", True)))
        self.var_wifi = tk.BooleanVar(value=bool(app.cfg.get("trocar_wifi", True)))
        self.var_mostrar = tk.BooleanVar(value=False)
        ttk.Checkbutton(op, text="Acertar a hora do ESP pelo relógio deste computador", variable=self.var_hora).pack(anchor="w")
        ttk.Checkbutton(op, text="Trocar de rede Wi-Fi automaticamente (volta à rede anterior depois)",
                        variable=self.var_wifi).pack(anchor="w")
        ttk.Checkbutton(op, text="Mostrar tokens e senha", variable=self.var_mostrar, command=self._mostrar).pack(anchor="w")

        dir_ = ttk.LabelFrame(corpo, text="Como criar o token do GitHub")
        dir_.grid(row=0, column=1, sticky="nsew")
        t = tk.Text(dir_, wrap="word", relief="flat", background="white", padx=10, pady=8, height=20, width=48,
                    font=("Segoe UI", 10) if sys.platform.startswith("win") else ("TkDefaultFont", 10))
        t.insert("1.0", AJUDA_TOKEN)
        t.configure(state="disabled")
        t.pack(fill="both", expand=True)

        rod = ttk.Frame(self.top, padding=(14, 0, 14, 14))
        rod.pack(fill="x")
        self.lbl = ttk.Label(rod, text="", style="Sub.TLabel")
        self.lbl.pack(side="left")
        ttk.Button(rod, text="Cancelar", command=self.top.destroy).pack(side="right")
        ttk.Button(rod, text="Salvar", command=self.salvar).pack(side="right", padx=8)
        ttk.Button(rod, text="Testar GitHub", command=self.testar_github).pack(side="right")
        ttk.Button(rod, text="Testar ESP", command=self.testar_esp).pack(side="right", padx=8)
        self.top.grab_set()

    def _mostrar(self):
        for e in self.entradas:
            e.configure(show="" if self.var_mostrar.get() else "•")

    def _coletar_valores(self):
        novo = dict(self.app.cfg)
        for k, v in self.vars.items():
            novo[k] = v.get().strip()
        try:
            novo["auto_intervalo_min"] = max(1, int(novo["auto_intervalo_min"]))
        except ValueError:
            raise core.ErroColetor("Minutos entre coletas precisa ser um número inteiro.")
        if not novo["esp_url"]:
            novo["esp_url"] = core.CONFIG_PADRAO["esp_url"]
        if not novo["gh_api"]:
            novo["gh_api"] = core.CONFIG_PADRAO["gh_api"]
        if novo["ap_senha"] and len(novo["ap_senha"]) < 8:
            raise core.ErroColetor("A senha WPA2 da rede do ESP tem no mínimo 8 caracteres.")
        novo["acertar_hora"] = self.var_hora.get()
        novo["trocar_wifi"] = self.var_wifi.get()
        return novo

    def salvar(self):
        try:
            novo = self._coletar_valores()
        except core.ErroColetor as e:
            messagebox.showerror("Configurações", str(e), parent=self.top)
            return
        self.app.cfg.update(novo)
        try:
            self.app.cfg.salvar()
        except OSError as e:
            messagebox.showerror("Configurações", f"Não foi possível salvar: {e}", parent=self.top)
            return
        self.app.log("Configurações salvas.")
        self.app.atualizar_tela()
        self.top.destroy()

    def _teste(self, rotulo, funcao):
        try:
            cfg = self._coletar_valores()
        except core.ErroColetor as e:
            self.lbl.configure(text=str(e))
            return
        self.lbl.configure(text=f"Testando {rotulo}…")

        def rodar():
            try:
                msg = funcao(cfg)
            except core.ErroColetor as e:
                msg = f"{rotulo}: {e}"
            self.app.fila.put(("chamar", lambda: self.lbl.winfo_exists() and self.lbl.configure(text=msg[:140])))
            self.app.log(msg)
        threading.Thread(target=rodar, daemon=True).start()

    def testar_esp(self):
        def f(cfg):
            info = core.ClienteESP(cfg["esp_url"], cfg["esp_token"]).info(timeout=4)
            # pede 1 linha para validar o token
            core.ClienteESP(cfg["esp_url"], cfg["esp_token"]).dados(max(0, int(info["n_ultimo"]) - 1), 1)
            return f"ESP OK: {info.get('id')} série {info.get('serie')}, n até {info.get('n_ultimo')}, token aceito."
        self._teste("ESP", f)

    def testar_github(self):
        def f(cfg):
            if not cfg.get("gh_token"):
                raise core.ErroGitHub("cole o token primeiro.")
            if not core.tem_internet(cfg["gh_api"]):
                raise core.SemInternet("sem internet agora.")
            core.ClienteGitHub(cfg["gh_api"], cfg["gh_dono"], cfg["gh_repo"], cfg["gh_ramo"], cfg["gh_token"]).verificar_acesso()
            return f"GitHub OK: o token tem acesso de escrita a {cfg['gh_dono']}/{cfg['gh_repo']}."
        self._teste("GitHub", f)


def main(base=None):
    root = tk.Tk()
    try:
        icone = core.Path(getattr(sys, "_MEIPASS", core.Path(__file__).parent)) / "icone.ico"
        if sys.platform.startswith("win") and icone.exists():
            root.iconbitmap(str(icone))
    except tk.TclError:
        pass
    App(root, base)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
