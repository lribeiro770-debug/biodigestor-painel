// Interface do Coletor do Biodigestor (Capacitor, Android). A lógica fica em nucleo.js.
import {
  CONFIG_PADRAO, coletar, enviar, lerInfo, testarGithub, carregarEstado, linhasPendentes, eventosPendentes,
  criarHttpFetch, ErroColetor, PASTA_DADOS,
} from './nucleo.js';
import { VERSAO } from './versao.js';

// ------------------------------------------------------------ plataforma
const cap = window.capacitorExports;
const nativo = !!cap?.Capacitor?.isNativePlatform?.();
const { Preferences } = window.capacitorPreferences;
const { Filesystem, Directory, Encoding } = window.capacitorFilesystem;
const { Network } = window.capacitorNetwork;
const { App } = window.capacitorApp;
const { CapacitorHttp } = cap;

// Plugin nativo próprio (android/app/src/main/java/.../RedeLocalPlugin.java); no navegador vira no-op.
const RedeLocal = cap.registerPlugin('RedeLocal', {
  web: () => ({
    estado: async () => ({ wifi: false, wifiTemInternet: false, vinculado: false, apConectadoPeloApp: false, android: 0 }),
    vincularWifi: async () => ({ ok: false }),
    desvincular: async () => ({ ok: true }),
    conectarAp: async () => { throw new Error('Disponível só no Android.'); },
    desconectarAp: async () => ({ ok: true }),
    abrirConfigWifi: async () => ({ ok: false }),
    addListener: async () => ({ remove: async () => {} }),
  }),
});

/** Adaptador http pelo HTTP nativo (sem CORS/mixed content; respeita bindProcessToNetwork). */
async function httpNativo({ metodo = 'GET', url, cabecalhos = {}, corpo, tempoMs = 15000 }) {
  const opcoes = { method: metodo, url, headers: cabecalhos, connectTimeout: tempoMs, readTimeout: tempoMs, responseType: 'text' };
  if (corpo !== undefined) {
    const ehJson = /json/i.test(cabecalhos['Content-Type'] || '');
    opcoes.data = ehJson ? JSON.parse(corpo) : corpo;
  }
  const r = await CapacitorHttp.request(opcoes);
  const cab = {};
  for (const [k, v] of Object.entries(r.headers || {})) cab[k.toLowerCase()] = String(v);
  const texto = typeof r.data === 'string' ? r.data : (r.data === null || r.data === undefined ? '' : JSON.stringify(r.data));
  return { status: r.status, cabecalhos: cab, texto };
}
const http = nativo ? httpNativo : criarHttpFetch();

/** Armazém no diretório privado do app (Directory.Data). */
const armazem = {
  async ler(caminho) {
    try {
      const r = await Filesystem.readFile({ path: caminho, directory: Directory.Data, encoding: Encoding.UTF8 });
      return typeof r.data === 'string' ? r.data : await r.data.text();
    } catch {
      return null;
    }
  },
  async escrever(caminho, texto) {
    await Filesystem.writeFile({ path: caminho, data: texto, directory: Directory.Data, encoding: Encoding.UTF8, recursive: true });
  },
  async listar(pasta) {
    try {
      const r = await Filesystem.readdir({ path: pasta, directory: Directory.Data });
      return r.files.map((f) => (typeof f === 'string' ? f : f.name));
    } catch {
      return [];
    }
  },
};

// ------------------------------------------------------------ estado da interface
let config = { ...CONFIG_PADRAO };
let status = { esp: null, leitura: null, ultimaColeta: null };
let registro = [];
let ocupado = false;
let conectadoPeloApp = false;
let ultimaTentativaAuto = 0;

const $ = (id) => document.getElementById(id);
const fmtN = (v) => (v === null || v === undefined || v === '' ? '—' : Number(v).toLocaleString('pt-BR'));
const fmtDec = (v, casas = 1) => (v === null || v === undefined ? '—' : Number(v).toLocaleString('pt-BR', { minimumFractionDigits: casas, maximumFractionDigits: casas }));
const fmtData = (ts) => (ts ? new Date(ts * 1000).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' }) : '—');
const agoraS = () => Math.floor(Date.now() / 1000);

function haQuanto(ts) {
  if (!ts) return '';
  const s = agoraS() - ts;
  if (s < 60) return 'agora há pouco';
  if (s < 3600) return `há ${Math.floor(s / 60)} min`;
  if (s < 86400) return `há ${Math.floor(s / 3600)} h`;
  return `há ${Math.floor(s / 86400)} dia(s)`;
}

async function lerPref(chave, padrao) {
  try {
    const { value } = await Preferences.get({ key: chave });
    return value ? JSON.parse(value) : padrao;
  } catch {
    return padrao;
  }
}
const gravarPref = (chave, valor) => Preferences.set({ key: chave, value: JSON.stringify(valor) });

// ------------------------------------------------------------ registro (log)
function registrar(msg, nivel = 'info') {
  registro.push({ t: agoraS(), msg, nivel });
  if (registro.length > 300) registro = registro.slice(-300);
  desenharLog();
  gravarPref('log', registro).catch(() => {});
}

function desenharLog() {
  const ol = $('log');
  ol.replaceChildren(...registro.slice(-120).reverse().map((e) => {
    const li = document.createElement('li');
    li.className = e.nivel;
    const t = document.createElement('time');
    t.textContent = new Date(e.t * 1000).toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    const s = document.createElement('span');
    s.textContent = e.msg;
    li.append(t, s);
    return li;
  }));
  if (!registro.length) {
    const li = document.createElement('li');
    li.innerHTML = '<span class="discreto">Nada registrado ainda.</span>';
    ol.append(li);
  }
}

let timerToast;
function toast(msg, erro = false) {
  const el = $('toast');
  el.textContent = msg;
  el.className = 'toast' + (erro ? ' erro' : '');
  el.hidden = false;
  clearTimeout(timerToast);
  timerToast = setTimeout(() => { el.hidden = true; }, erro ? 7000 : 3500);
}

// ------------------------------------------------------------ desenho do estado
async function desenhar() {
  const est = await carregarEstado(armazem);
  const e = status.esp;
  const pill = $('pill-esp');
  if (e && e.alcancavel) { pill.textContent = 'alcançável'; pill.className = 'pill ok'; }
  else if (e && e.alcancavel === false) { pill.textContent = 'não alcançável'; pill.className = 'pill erro'; }
  else { pill.textContent = 'sem contato'; pill.className = 'pill'; }

  const serie = e?.info?.serie || Object.keys(est.esp).slice(-1)[0];
  $('v-serie').textContent = serie || '—';
  $('v-n-esp').textContent = fmtN(e?.info?.n_ultimo);
  $('v-n-col').textContent = fmtN(serie ? est.esp[serie]?.ultimo_n : null);
  if (e?.horaQuando) {
    $('v-hora').textContent = e.horaAjustada ? `ajustada ${haQuanto(e.horaQuando)}` : `certa (±${Math.abs(e.ajuste ?? 0)} s)`;
  } else {
    $('v-hora').textContent = e?.info ? (e.info.hora_ok ? 'com hora' : 'sem hora') : '—';
  }
  $('v-esp-quando').textContent = e?.quando ? `${e.alcancavel ? 'Último contato' : 'Última tentativa'} ${haQuanto(e.quando)}${e.info?.id ? ' · ' + e.info.id : ''}` : 'Nenhum contato ainda.';
  $('sub-dispositivo').textContent = est.dispositivo ? `${est.dispositivo} · coleta offline` : 'Coleta offline · UTFPR';

  const pend = linhasPendentes(est), pendEv = eventosPendentes(est);
  $('v-pend').textContent = fmtN(pend);
  $('v-pend-ev').textContent = fmtN(pendEv);
  const pn = $('pill-nuvem');
  if (pend || pendEv) { pn.textContent = `${fmtN(pend)} pendente(s)`; pn.className = 'pill aviso'; }
  else if (est.ultimo_envio) { pn.textContent = 'em dia'; pn.className = 'pill ok'; }
  else { pn.textContent = '—'; pn.className = 'pill'; }
  $('v-envio').textContent = est.ultimo_envio ? `${fmtData(est.ultimo_envio)} (${haQuanto(est.ultimo_envio)})` : 'nunca';
  $('v-repo').textContent = config.ghDono && config.ghRepo ? `${config.ghDono}/${config.ghRepo} @ ${config.ghRamo || 'main'}` : 'repositório não configurado';

  const l = status.leitura;
  $('l-tmed').textContent = fmtDec(l?.t_med, 1);
  $('l-tamb').textContent = fmtDec(l?.t_amb, 1);
  $('l-ur').textContent = fmtDec(l?.ur_amb, 0);
  $('l-pa').textContent = fmtDec(l?.p_pa, 0);
  $('v-leitura-quando').textContent = l ? (l.ts ? fmtData(l.ts) : `coletada ${haQuanto(status.ultimaColeta)}`) + (l.sim ? ' · simulação' : '') : '—';
  const al = $('l-alarmes');
  if (l?.alarmes?.length) {
    al.replaceChildren(...l.alarmes.map((a) => { const s = document.createElement('span'); s.className = 'chip'; s.textContent = a; return s; }));
  } else {
    al.innerHTML = `<span class="discreto">${l ? 'Sem alarmes.' : 'Nenhuma leitura ainda.'}</span>`;
  }

  $('aviso-config').hidden = !!config.espToken;
  $('txt-conectar').textContent = conectadoPeloApp ? 'Desconectar da rede do biodigestor' : 'Conectar à rede do biodigestor';
  for (const id of ['bt-conectar', 'bt-coletar', 'bt-enviar', 'bt-verificar', 'bt-testar-esp', 'bt-testar-gh']) $(id).disabled = ocupado;
}

async function desenharRede() {
  const faixa = $('faixa-rede');
  try {
    const n = await Network.getStatus();
    const r = await RedeLocal.estado();
    let txt, cls;
    if (conectadoPeloApp) { txt = 'Conectado à rede do biodigestor pelo app'; cls = 'local'; }
    else if (r.wifi && !r.wifiTemInternet && nativo) { txt = 'Wi-Fi sem internet (provável rede do biodigestor)'; cls = 'local'; }
    else if (n.connected) { txt = n.connectionType === 'wifi' ? 'Wi-Fi com internet' : n.connectionType === 'cellular' ? 'Dados móveis' : 'Com rede'; cls = 'ok'; }
    else { txt = 'Sem rede'; cls = 'off'; }
    $('txt-rede').textContent = txt;
    faixa.className = 'faixa ' + cls;
  } catch {
    $('txt-rede').textContent = 'Estado da rede desconhecido';
    faixa.className = 'faixa';
  }
}

function progresso(p) {
  const box = $('progresso');
  if (!p) { box.hidden = true; return; }
  box.hidden = false;
  if (p.total) {
    box.classList.remove('indet');
    $('barra-int').style.width = `${Math.min(100, Math.round((100 * p.feito) / p.total))}%`;
    $('txt-progresso').textContent = `${p.etapa === 'envio' ? 'Enviando' : 'Baixando'}: ${fmtN(p.feito)} de ${fmtN(p.total)} linhas`;
  } else {
    box.classList.add('indet');
    $('txt-progresso').textContent = p.texto || 'Aguarde…';
  }
}

// ------------------------------------------------------------ operações
function ctx() {
  return { http, armazem, config, agora: agoraS, log: registrar, aoProgresso: progresso };
}

async function comOcupado(texto, fn) {
  if (ocupado) return;
  ocupado = true;
  progresso({ texto });
  await desenhar();
  try {
    return await fn();
  } catch (e) {
    const msg = e instanceof ErroColetor ? e.message : `Erro inesperado: ${e?.message || e}`;
    registrar(msg, 'erro');
    toast(msg, true);
    return undefined;
  } finally {
    ocupado = false;
    progresso(null);
    await desenhar();
    desenharRede();
  }
}

/** Prende o tráfego do app ao Wi-Fi durante fn (evita que o Android use os dados móveis). */
async function noWifi(fn) {
  let vinculado = false;
  if (nativo) {
    try {
      const r = await RedeLocal.vincularWifi();
      vinculado = !!r.ok;
      if (vinculado && !r.temInternet) registrar('Tráfego do app preso ao Wi-Fi (dados móveis ignorados durante a coleta).');
    } catch (e) {
      registrar(`${e.message || e} Tentando mesmo assim…`, 'aviso');
    }
  }
  try {
    return await fn();
  } finally {
    if (vinculado) await RedeLocal.desvincular().catch(() => {});
  }
}

function marcarEsp(parcial) {
  status.esp = { ...(status.esp || {}), ...parcial, quando: agoraS() };
  gravarPref('status', status).catch(() => {});
}

async function acaoConectar() {
  if (conectadoPeloApp) {
    await RedeLocal.desconectarAp().catch(() => {});
    conectadoPeloApp = false;
    registrar('Desconectado da rede do biodigestor.');
    await desenhar(); desenharRede();
    return;
  }
  if (!nativo) {
    toast('No navegador, conecte o computador à rede do biodigestor manualmente.');
    return;
  }
  const info = await RedeLocal.estado().catch(() => ({ android: 0 }));
  if (info.android >= 29) {
    await comOcupado('Conectando… confirme na janela do Android', async () => {
      try {
        registrar(`Pedindo ao Android para conectar em "${config.apSsid || 'Biodigestor-…'}". Confirme na janela do sistema.`);
        await RedeLocal.conectarAp({ ssid: config.apSsid, senha: config.apSenha, tempoMs: 45000 });
        conectadoPeloApp = true;
        registrar('Conectado à rede do biodigestor.', 'ok');
        await verificarEsp(true);
        return;
      } catch (e) {
        registrar(`Conexão automática não deu certo: ${e.message || e}`, 'aviso');
      }
      registrar('Abrindo o Wi-Fi do Android: escolha a rede "Biodigestor-XXXX", conecte e volte ao app.');
      await RedeLocal.abrirConfigWifi().catch(() => {});
    });
  } else {
    registrar('Abrindo o Wi-Fi do Android: escolha a rede "Biodigestor-XXXX", conecte e volte ao app.');
    await RedeLocal.abrirConfigWifi().catch(() => {});
  }
}

async function verificarEsp(silencioso = false) {
  try {
    const info = await noWifi(() => lerInfo(ctx()));
    marcarEsp({ alcancavel: true, info });
    if (!silencioso) registrar(`ESP alcançável: ${info.id || ''} · série ${info.serie} · último n ${info.n_ultimo}.`, 'ok');
    return info;
  } catch (e) {
    marcarEsp({ alcancavel: false });
    if (!silencioso) throw e;
    return null;
  } finally {
    await desenhar();
  }
}

async function acaoColetar() {
  if (!config.espToken) { toast('Preencha o token do ESP em Configurações.', true); abrirConfig(); return; }
  const r = await comOcupado('Contatando o ESP…', () => noWifi(async () => {
    try {
      const res = await coletar(ctx());
      marcarEsp({ alcancavel: true, info: res.info, horaAjustada: res.horaAjustada, ajuste: res.ajusteHora, horaQuando: agoraS() });
      if (res.leitura) status.leitura = res.leitura;
      status.ultimaColeta = agoraS();
      await gravarPref('status', status);
      toast(`Coleta concluída: ${fmtN(res.linhasNovas)} linha(s) nova(s).`);
      return res;
    } catch (e) {
      if (e.codigo === 'esp_inalcancavel') marcarEsp({ alcancavel: false });
      throw e;
    }
  }));
  if (r) setTimeout(() => talvezEnviarAuto(), 5000);
}

async function acaoEnviar(automatico = false) {
  if (!config.ghToken) {
    if (!automatico) { toast('Preencha o token do GitHub em Configurações.', true); abrirConfig(); }
    return;
  }
  await comOcupado('Enviando para o GitHub…', async () => {
    try {
      const r = await enviar(ctx());
      if (!r.nada) toast(`Envio concluído: ${fmtN(r.linhasEnviadas)} linha(s) novas na nuvem.`);
      else if (!automatico) toast('Nada pendente para enviar.');
    } catch (e) {
      // no automático, falta de internet é esperada: só registra, sem alarde
      if (automatico && e.codigo === 'sem_internet') { registrar('Envio automático adiado: sem internet.'); return; }
      throw e;
    }
  });
}

async function talvezEnviarAuto() {
  if (!config.autoEnviar || ocupado || !config.ghToken) return;
  if (Date.now() - ultimaTentativaAuto < 120000) return;
  const est = await carregarEstado(armazem);
  if (!linhasPendentes(est) && !eventosPendentes(est)) return;
  const n = await Network.getStatus().catch(() => ({ connected: false }));
  if (!n.connected) return;
  ultimaTentativaAuto = Date.now();
  registrar('Internet disponível: enviando automaticamente…');
  await acaoEnviar(true);
}

// ------------------------------------------------------------ configurações
const CAMPOS = ['coletor', 'espUrl', 'espToken', 'apSsid', 'apSenha', 'ghDono', 'ghRepo', 'ghRamo', 'ghToken', 'ghApi'];

function abrirConfig() {
  const f = $('form-config');
  for (const c of CAMPOS) f.elements[c].value = config[c] ?? '';
  f.elements.autoEnviar.checked = !!config.autoEnviar;
  $('tela-principal').hidden = true;
  $('tela-config').hidden = false;
  window.scrollTo(0, 0);
  listarArquivos();
}

function fecharConfig() {
  $('tela-config').hidden = true;
  $('tela-principal').hidden = false;
  desenhar();
}

function lerFormulario() {
  const f = $('form-config');
  const novo = { ...config };
  for (const c of CAMPOS) novo[c] = f.elements[c].value.trim();
  novo.autoEnviar = f.elements.autoEnviar.checked;
  if (!novo.espUrl) novo.espUrl = CONFIG_PADRAO.espUrl;
  if (!novo.ghApi) novo.ghApi = CONFIG_PADRAO.ghApi;
  if (!novo.ghRamo) novo.ghRamo = 'main';
  if (!novo.apSsid) novo.apSsid = CONFIG_PADRAO.apSsid;
  if (!/^https?:\/\//i.test(novo.espUrl)) novo.espUrl = 'http://' + novo.espUrl;
  return novo;
}

async function salvarConfig(ev) {
  ev?.preventDefault();
  config = lerFormulario();
  await gravarPref('config', config);
  let host = '';
  try { host = new URL(config.espUrl).hostname; } catch { /* inválido */ }
  if (nativo && config.espUrl.startsWith('http:') && !['192.168.4.1', 'biodigestor.local'].includes(host)) {
    toast('Atenção: o Android só permite HTTP para 192.168.4.1 e biodigestor.local.', true);
  } else {
    toast('Configurações salvas.');
  }
  registrar('Configurações salvas.');
}

async function listarArquivos() {
  const ul = $('lista-arquivos');
  const nomes = (await armazem.listar(PASTA_DADOS)).sort();
  if (!nomes.length) { ul.innerHTML = '<li class="discreto">Nenhum arquivo ainda.</li>'; return; }
  const itens = [];
  for (const n of nomes) {
    let tam = '';
    try {
      const s = await Filesystem.stat({ path: `${PASTA_DADOS}/${n}`, directory: Directory.Data });
      tam = s.size > 1048576 ? `${(s.size / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(s.size / 1024))} kB`;
    } catch { /* sem tamanho */ }
    const li = document.createElement('li');
    const a = document.createElement('span'); a.textContent = `${PASTA_DADOS}/${n}`;
    const b = document.createElement('span'); b.textContent = tam;
    li.append(a, b);
    itens.push(li);
  }
  ul.replaceChildren(...itens);
}

// ------------------------------------------------------------ início
async function iniciar() {
  config = { ...CONFIG_PADRAO, ...(await lerPref('config', {})) };
  status = { ...status, ...(await lerPref('status', {})) };
  registro = await lerPref('log', []);
  $('txt-versao').textContent = `Coletor do Biodigestor v${VERSAO} · protocolo 1${nativo ? '' : ' · modo navegador'}`;
  desenharLog();

  $('bt-config').addEventListener('click', abrirConfig);
  $('bt-voltar').addEventListener('click', fecharConfig);
  $('form-config').addEventListener('submit', salvarConfig);
  $('bt-conectar').addEventListener('click', acaoConectar);
  $('bt-coletar').addEventListener('click', acaoColetar);
  $('bt-enviar').addEventListener('click', () => acaoEnviar(false));
  $('bt-verificar').addEventListener('click', () => comOcupado('Verificando o ESP…', () => verificarEsp(false)));
  $('bt-limpar-log').addEventListener('click', () => { registro = []; gravarPref('log', registro); desenharLog(); });
  $('bt-testar-esp').addEventListener('click', async () => {
    await salvarConfig();
    await comOcupado('Testando o ESP…', async () => { const i = await verificarEsp(false); if (i) toast(`ESP OK: ${i.id || ''}, série ${i.serie}.`); });
  });
  $('bt-testar-gh').addEventListener('click', async () => {
    await salvarConfig();
    await comOcupado('Testando o GitHub…', async () => {
      const r = await testarGithub(ctx());
      registrar(`GitHub OK: ${r.repo}${r.privado ? ' (privado)' : ''}, gravação permitida.`, 'ok');
      toast(`GitHub OK: ${r.repo}`);
    });
  });
  document.querySelectorAll('.ver').forEach((b) => b.addEventListener('click', () => {
    const inp = $('form-config').elements[b.dataset.alvo];
    const mostrar = inp.type === 'password';
    inp.type = mostrar ? 'text' : 'password';
    b.textContent = mostrar ? 'ocultar' : 'mostrar';
  }));

  Network.addListener('networkStatusChange', () => { desenharRede(); setTimeout(talvezEnviarAuto, 3000); });
  RedeLocal.addListener('apPerdido', () => { conectadoPeloApp = false; registrar('A conexão com a rede do biodigestor caiu.', 'aviso'); desenhar(); desenharRede(); });
  App.addListener('appStateChange', ({ isActive }) => {
    if (!isActive) return;
    desenharRede();
    desenhar();
    if (!ocupado && config.espToken) verificarEsp(true);
    setTimeout(talvezEnviarAuto, 2000);
  });
  App.addListener('backButton', () => {
    if (!$('tela-config').hidden) fecharConfig();
    else App.exitApp();
  });

  if (nativo) {
    const r = await RedeLocal.estado().catch(() => null);
    conectadoPeloApp = !!r?.apConectadoPeloApp;
  }
  await desenhar();
  await desenharRede();
  if (!config.espToken) registrar('Bem-vindo! Abra Configurações (engrenagem) para preencher os tokens.');
  setInterval(() => { if (!ocupado) desenhar(); }, 30000);
  setTimeout(talvezEnviarAuto, 2500);
}

iniciar().catch((e) => {
  const d = document.createElement('div');
  d.className = 'aviso';
  d.textContent = `Falha ao iniciar o app: ${e?.message || e}`;
  document.body.prepend(d);
});
