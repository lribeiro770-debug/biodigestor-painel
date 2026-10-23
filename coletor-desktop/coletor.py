#!/usr/bin/env python3
"""Coletor do Biodigestor — baixa os dados do ESP32 pela rede Wi-Fi dele e envia ao GitHub.

Sem argumentos abre a janela. Linha de comando:
  python coletor.py coletar  [--esp URL] [--token TOKEN] [--sem-wifi] [--sem-acertar-hora]
  python coletor.py enviar   [--api-github URL]
  python coletor.py status
  python coletor.py auto     [--intervalo-min N]
  python coletor.py config   [--definir chave=valor ...]
Opções gerais: --dados PASTA (pasta de dados), --coletor NOME.
Opções passadas na linha de comando valem só para aquela execução (exceto em "config --definir").
"""
import argparse
import json
import sys
import threading

import coletor_core as core


def _cfg_da_linha(a):
    base = core.Path(a.dados) if a.dados else core.pasta_padrao()
    cfg = core.Config(base)
    if getattr(a, "esp", None):
        cfg["esp_url"] = a.esp
    if getattr(a, "token", None):
        cfg["esp_token"] = a.token
    if getattr(a, "coletor", None):
        cfg["coletor"] = a.coletor
    if getattr(a, "api_github", None):
        cfg["gh_api"] = a.api_github
    if getattr(a, "intervalo_min", None):
        cfg["auto_intervalo_min"] = a.intervalo_min
    if getattr(a, "sem_wifi", False):
        cfg["trocar_wifi"] = False
    return base, cfg


def _converter(valor_txt, padrao):
    if isinstance(padrao, bool):
        return valor_txt.strip().lower() in ("1", "true", "sim", "s", "yes", "on")
    if isinstance(padrao, int):
        return int(valor_txt)
    return valor_txt


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    # no .exe "--windowed" não existe console: stdout/stderr vêm como None
    if sys.stdout is None or sys.stderr is None:
        import os
        nulo = open(os.devnull, "w", encoding="utf-8")
        sys.stdout = sys.stdout or nulo
        sys.stderr = sys.stderr or nulo
    if not argv or argv[0] == "gui":
        import coletor_gui
        return coletor_gui.main()

    ap = argparse.ArgumentParser(prog="coletor", description="Coletor do Biodigestor (protocolo de coleta v1)")
    ap.add_argument("--dados", help="pasta de dados do app (padrão: %%APPDATA%%/ColetorBiodigestor ou ~/.coletor_biodigestor)")
    ap.add_argument("--coletor", help="nome de quem está coletando (vai no registro do ESP e no commit)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("coletar", help="baixa os dados novos do ESP")
    c.add_argument("--esp", help="endereço do ESP (padrão http://192.168.4.1)")
    c.add_argument("--token", help="token do ESP (de preferência salve com 'config')")
    c.add_argument("--sem-wifi", action="store_true", help="não tentar trocar de rede Wi-Fi")
    c.add_argument("--sem-acertar-hora", action="store_true", help="não acertar o relógio do ESP")
    e = sub.add_parser("enviar", help="envia os dados pendentes para o GitHub")
    e.add_argument("--api-github", help="endereço da API (padrão https://api.github.com)")
    sub.add_parser("status", help="mostra a situação local")
    au = sub.add_parser("auto", help="modo automático (Ctrl+C para sair)")
    au.add_argument("--esp")
    au.add_argument("--token")
    au.add_argument("--api-github")
    au.add_argument("--intervalo-min", type=int, help="minutos entre coletas (padrão 60)")
    au.add_argument("--sem-wifi", action="store_true")
    cf = sub.add_parser("config", help="mostra ou altera a configuração salva")
    cf.add_argument("--definir", nargs="*", default=[], metavar="CHAVE=VALOR")
    a = ap.parse_args(argv)

    base, cfg = _cfg_da_linha(a)
    log = core.Registro(base, cfg)
    log.ouvintes.append(lambda linha: print(linha, flush=True))

    try:
        if a.cmd == "coletar":
            with core.OPERACAO:
                if a.sem_acertar_hora:
                    cfg["acertar_hora"] = False
                r = core.coletar_com_wifi(cfg, base, log)
            print(json.dumps(r, ensure_ascii=False, indent=1))
        elif a.cmd == "enviar":
            with core.OPERACAO:
                r = core.enviar(cfg, base, log)
            print(json.dumps(r, ensure_ascii=False, indent=1))
        elif a.cmd == "status":
            s = core.situacao(base)
            print(f"Pasta de dados:      {s['pasta']}")
            print(f"Série atual:         {s['serie'] or '-'}")
            print(f"Coletado até n:      {s['coletado_n']} (ESP tinha {s['esp_n']} na última coleta)")
            print(f"Hora do ESP:         {s['hora'] or '-'}")
            print(f"Última coleta:       {core.formatar_data(s['ultima_coleta'])}")
            print(f"Último envio:        {core.formatar_data(s['ultimo_envio'])}")
            print(f"Linhas pendentes:    {s['linhas_pendentes']}  arquivos: {', '.join(s['arquivos_pendentes']) or '-'}")
            print(f"Meses guardados:     {', '.join(s['meses']) or '-'}")
            info = core.ClienteESP(cfg["esp_url"], cfg["esp_token"]).alcancavel(timeout=3)
            print(f"ESP ({cfg['esp_url']}): " + (f"alcançável, n_ultimo={info.get('n_ultimo')}" if info else "não alcançável"))
            print("Internet: " + ("sim" if core.tem_internet(cfg.get("gh_api")) else "não"))
        elif a.cmd == "auto":
            auto = core.ModoAutomatico(lambda: cfg, base, log)
            auto.iniciar()
            try:
                threading.Event().wait()
            except KeyboardInterrupt:
                auto.parar()
        elif a.cmd == "config":
            salvo = core.Config(base)
            for item in a.definir:
                if "=" not in item:
                    raise core.ErroColetor(f"Use chave=valor (recebi '{item}').")
                k, v = item.split("=", 1)
                if k not in core.CONFIG_PADRAO:
                    raise core.ErroColetor(f"Chave desconhecida: {k}. Válidas: {', '.join(core.CONFIG_PADRAO)}")
                salvo[k] = _converter(v, core.CONFIG_PADRAO[k])
            if a.definir:
                salvo.salvar()
                print("Configuração salva.")
            print(json.dumps(salvo.mascarado(), ensure_ascii=False, indent=1))
    except core.ErroColetor as e:
        log(f"ERRO: {e}")
        return 1
    except ValueError as e:
        log(f"ERRO: valor inválido ({e}).")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
