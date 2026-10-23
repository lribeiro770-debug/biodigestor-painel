// Núcleo do Coletor do Biodigestor — lógica pura (sem Capacitor), testável no Node.
// Segue coleta/ESPEC_COLETA.md (protocolo 1). Tudo de E/S é injetado:
//   http(pedido)  -> {status, cabecalhos (chaves minúsculas), texto}
//   armazem       -> {ler(caminho) -> string|null, escrever(caminho, texto), listar(pasta) -> [nomes]}
//   agora()       -> epoch em segundos
//   log(msg, nivel)  nivel: 'info' | 'ok' | 'aviso' | 'erro'

export const PROTO = 1;
export const COLUNAS_NUVEM = ['ts', 't_dig', 't_dig2', 't_med', 't_amb', 'ur_amb', 'mq2_rel', 'mq7_rel', 'p_pa',
  'vol_nl', 'vazao_nlh', 'rele', 'alarmes', 'sim', 'ts_ok', 'serie', 'n'];
export const COLUNAS_EVENTOS = ['ts', 'serie', 'n', 'tipo', 'detalhe'];
export const PASTA_DADOS = 'dados';
export const ARQ_ESTADO = 'estado.json';
export const ARQ_SEM_HORA = 'dados/sem_hora.csv';
export const ARQ_EVENTOS = 'dados/eventos.csv';
export const ARQ_INDICE = 'dados/index.json';
const RE_MES = /^\d{4}-\d{2}\.csv$/;

export const CONFIG_PADRAO = {
  espUrl: 'http://192.168.4.1',
  espToken: '',
  apSsid: 'Biodigestor-XXXX',
  apSenha: 'biogas123',
  ghApi: 'https://api.github.com',
  ghDono: 'lribeiro770-debug',
  ghRepo: 'biodigestor-painel',
  ghRamo: 'main',
  ghToken: '',
  coletor: '',
  autoEnviar: true,
  lote: 5000,
  tempoEspMs: 15000,
  tempoGhMs: 60000,
};

/** Erro com mensagem em português pronta para mostrar ao usuário. */
export class ErroColetor extends Error {
  constructor(mensagem, codigo = 'erro', status = 0) {
    super(mensagem);
    this.name = 'ErroColetor';
    this.codigo = codigo;
    this.status = status;
  }
}

// ---------------------------------------------------------------- CSV

/** Lê CSV (RFC 4180 simples, aceita CRLF). Devolve {cabecalho, linhas:[objeto]}. */
export function lerCsv(texto) {
  const regs = [];
  let campo = '', reg = [], aspas = false, i = 0;
  const s = String(texto ?? '').replace(/^﻿/, '');
  while (i < s.length) {
    const c = s[i];
    if (aspas) {
      if (c === '"') {
        if (s[i + 1] === '"') { campo += '"'; i += 2; continue; }
        aspas = false; i++; continue;
      }
      campo += c; i++; continue;
    }
    if (c === '"') { aspas = true; i++; continue; }
    if (c === ',') { reg.push(campo); campo = ''; i++; continue; }
    if (c === '\r') { i++; continue; }
    if (c === '\n') { reg.push(campo); regs.push(reg); reg = []; campo = ''; i++; continue; }
    campo += c; i++;
  }
  if (campo !== '' || reg.length) { reg.push(campo); regs.push(reg); }
  const uteis = regs.filter((r) => !(r.length === 1 && r[0] === ''));
  if (!uteis.length) return { cabecalho: [], linhas: [] };
  const cabecalho = uteis[0].map((x) => x.trim());
  const linhas = uteis.slice(1).map((r) => {
    const o = {};
    cabecalho.forEach((k, j) => { o[k] = r[j] ?? ''; });
    return o;
  });
  return { cabecalho, linhas };
}

function campoCsv(v) {
  const s = v === null || v === undefined ? '' : String(v);
  return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

/** Gera CSV com cabeçalho e "\n" no fim. */
export function escreverCsv(colunas, linhas) {
  const out = [colunas.join(',')];
  for (const l of linhas) out.push(colunas.map((c) => campoCsv(l[c])).join(','));
  return out.join('\n') + '\n';
}

// ---------------------------------------------------------------- utilidades

export const chave = (l) => `${l.serie}:${l.n}`;
const temTs = (l) => l.ts !== undefined && l.ts !== null && String(l.ts).trim() !== '';

/** Caminho do arquivo de destino de uma linha: mês UTC do ts, ou sem_hora.csv. */
export function arquivoDaLinha(l) {
  if (!temTs(l)) return ARQ_SEM_HORA;
  const d = new Date(Number(l.ts) * 1000);
  const m = String(d.getUTCMonth() + 1).padStart(2, '0');
  return `${PASTA_DADOS}/${d.getUTCFullYear()}-${m}.csv`;
}

function ordenar(linhas) {
  return linhas.sort((a, b) => {
    const ta = temTs(a) ? Number(a.ts) : Infinity, tb = temTs(b) ? Number(b.ts) : Infinity;
    if (ta !== tb) return ta < tb ? -1 : 1;
    if (a.serie !== b.serie) return String(a.serie) < String(b.serie) ? -1 : 1;
    return Number(a.n) - Number(b.n);
  });
}

/**
 * Une linhas existentes com novas pela chave serie+n (a nova prevalece), remove as chaves em `remover`
 * e ordena por ts. Devolve {linhas, novas} (novas = chaves que não existiam antes).
 */
export function mesclar(existentes, novas, remover = null) {
  const mapa = new Map();
  for (const l of existentes) mapa.set(chave(l), l);
  let contagem = 0;
  for (const l of novas) {
    const k = chave(l);
    if (!mapa.has(k)) contagem++;
    mapa.set(k, l);
  }
  if (remover) for (const k of remover) mapa.delete(k);
  return { linhas: ordenar([...mapa.values()]), novas: contagem };
}

/** Base64 de texto UTF-8 (funciona no Node e no WebView). */
export function paraBase64(texto) {
  const bytes = new TextEncoder().encode(texto);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(bin);
}

export function deBase64(b64) {
  const bin = atob(String(b64).replace(/\s/g, ''));
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new TextDecoder().decode(bytes);
}

const espera = (ms) => new Promise((r) => setTimeout(r, ms));
const semBarra = (u) => String(u || '').trim().replace(/\/+$/, '');
const nada = () => {};

/** Adaptador http baseado em fetch (Node 18+ e navegador). */
export function criarHttpFetch(fetchFn = globalThis.fetch) {
  return async ({ metodo = 'GET', url, cabecalhos = {}, corpo, tempoMs = 15000 }) => {
    const r = await fetchFn(url, { method: metodo, headers: cabecalhos, body: corpo, signal: AbortSignal.timeout(tempoMs) });
    const cab = {};
    r.headers.forEach((v, k) => { cab[k.toLowerCase()] = v; });
    return { status: r.status, cabecalhos: cab, texto: await r.text() };
  };
}

/** Armazém em memória (testes e pré-visualização no navegador). */
export class ArmazemMemoria {
  constructor() { this.arquivos = new Map(); }
  async ler(c) { return this.arquivos.has(c) ? this.arquivos.get(c) : null; }
  async escrever(c, t) { this.arquivos.set(c, t); }
  async listar(p) {
    const pre = semBarra(p) + '/';
    return [...this.arquivos.keys()].filter((k) => k.startsWith(pre) && !k.slice(pre.length).includes('/')).map((k) => k.slice(pre.length));
  }
}

// ---------------------------------------------------------------- estado local (§7)

export function estadoVazio() {
  return { esp: {}, nuvem: {}, pendente: false, arquivos_pendentes: [], dispositivo: '' };
}

export async function carregarEstado(armazem) {
  const t = await armazem.ler(ARQ_ESTADO);
  if (!t) return estadoVazio();
  try {
    const e = JSON.parse(t);
    return { ...estadoVazio(), ...e, esp: e.esp || {}, nuvem: e.nuvem || {}, arquivos_pendentes: e.arquivos_pendentes || [] };
  } catch {
    return estadoVazio();
  }
}

export async function salvarEstado(armazem, estado) {
  await armazem.escrever(ARQ_ESTADO, JSON.stringify(estado, null, 1));
}

/** Linhas coletadas e ainda não enviadas (pela diferença de n, que é contínuo por série). */
export function linhasPendentes(estado) {
  let total = 0;
  for (const [s, e] of Object.entries(estado.esp || {})) {
    const nv = (estado.nuvem || {})[s]?.ultimo_n || 0;
    total += Math.max(0, (e.ultimo_n || 0) - nv);
  }
  return total;
}

export function eventosPendentes(estado) {
  let total = 0;
  for (const [s, e] of Object.entries(estado.esp || {})) {
    total += Math.max(0, (e.ultimo_ev || 0) - ((estado.nuvem || {})[s]?.ultimo_ev || 0));
  }
  return total;
}

async function mesclarArquivoLocal(armazem, caminho, colunas, novas, remover) {
  const atual = await armazem.ler(caminho);
  const existentes = atual ? lerCsv(atual).linhas : [];
  const r = mesclar(existentes, novas, remover);
  if (atual === null && r.linhas.length === 0) return 0;
  await armazem.escrever(caminho, escreverCsv(colunas, r.linhas));
  return r.novas;
}

/** Grava linhas (formato nuvem) nos CSV locais: mês UTC ou sem_hora; linhas com ts saem do sem_hora. */
export async function gravarLocal(armazem, estado, linhas, eventos = []) {
  const grupos = new Map();
  const comHora = new Set();
  for (const l of linhas) {
    const a = arquivoDaLinha(l);
    if (!grupos.has(a)) grupos.set(a, []);
    grupos.get(a).push(l);
    if (a !== ARQ_SEM_HORA) comHora.add(chave(l));
  }
  if (comHora.size && !grupos.has(ARQ_SEM_HORA) && (await armazem.ler(ARQ_SEM_HORA)) !== null) grupos.set(ARQ_SEM_HORA, []);
  let novas = 0;
  const tocados = new Set(estado.arquivos_pendentes || []);
  for (const [arq, ls] of grupos) {
    novas += await mesclarArquivoLocal(armazem, arq, COLUNAS_NUVEM, ls, arq === ARQ_SEM_HORA ? comHora : null);
    tocados.add(arq);
  }
  if (eventos.length) {
    await mesclarArquivoLocal(armazem, ARQ_EVENTOS, COLUNAS_EVENTOS, eventos, null);
    tocados.add(ARQ_EVENTOS);
  }
  estado.arquivos_pendentes = [...tocados].sort();
  return novas;
}

// ---------------------------------------------------------------- ESP (§5)

function mensagemSemEsp(base, e) {
  return `ESP não alcançável em ${base}. Conecte o celular à rede Wi-Fi "Biodigestor-XXXX" (senha padrão biogas123) e tente de novo.` +
    (e && e.message ? ` (detalhe: ${e.message})` : '');
}

async function pedirEsp(ctx, metodo, caminho, corpo, comToken = true) {
  const base = semBarra(ctx.config.espUrl);
  const cab = {};
  if (comToken) cab['X-Token'] = ctx.config.espToken || '';
  if (corpo !== undefined) cab['Content-Type'] = 'application/json';
  let r;
  try {
    r = await ctx.http({ metodo, url: base + caminho, cabecalhos: cab, corpo: corpo === undefined ? undefined : JSON.stringify(corpo), tempoMs: ctx.config.tempoEspMs || 15000 });
  } catch (e) {
    throw new ErroColetor(mensagemSemEsp(base, e), 'esp_inalcancavel');
  }
  if (r.status === 401) throw new ErroColetor('Token do ESP incorreto (401). Confira o "Token do ESP" em Configurações (é o mesmo token de comando do MQTT).', 'esp_token', 401);
  if (r.status !== 200) throw new ErroColetor(`O ESP respondeu com erro ${r.status} em ${caminho}.`, 'esp_http', r.status);
  return r;
}

function jsonOuErro(r, onde) {
  try { return JSON.parse(r.texto); } catch { throw new ErroColetor(`Resposta inválida do ESP em ${onde} (não é JSON). O endereço do ESP está certo?`, 'esp_formato'); }
}

/** GET /api/info — sem token. */
export async function lerInfo(ctx) {
  const info = jsonOuErro(await pedirEsp(ctx, 'GET', '/api/info', undefined, false), '/api/info');
  if (info.proto !== PROTO) throw new ErroColetor(`Versão de protocolo do ESP (${info.proto}) diferente da do app (${PROTO}). Atualize o app ou o firmware.`, 'esp_proto');
  if (!/^[0-9a-fA-F]{8}$/.test(String(info.serie || ''))) throw new ErroColetor('O ESP não informou a "série" do cartão. O cartão microSD está presente?', 'esp_serie');
  return info;
}

/** Normaliza a leitura de /api/agora (telemetria MQTT) ou de uma linha do CSV. */
export function resumoLeitura(o) {
  if (!o) return null;
  const num = (v) => (v === '' || v === null || v === undefined || Number.isNaN(Number(v)) ? null : Number(v));
  let al = o.alarmes;
  if (Array.isArray(al)) al = al.join('|');
  return {
    ts: num(o.ts) || null,
    t_med: num(o.t_med), t_dig: num(o.t_dig), t_dig2: num(o.t_dig2), t_amb: num(o.t_amb), ur_amb: num(o.ur_amb), p_pa: num(o.p_pa),
    alarmes: String(al || '').split('|').map((x) => x.trim()).filter(Boolean),
    sim: o.sim === true || String(o.sim) === '1',
  };
}

function linhaParaNuvem(l, serie) {
  const o = {};
  for (const c of COLUNAS_NUVEM) o[c] = c === 'serie' ? serie : (l[c] ?? '');
  return o;
}

/**
 * Coleta completa (§7): info → hora → dados em lotes → eventos → coletado → salvar local.
 * Cada lote é gravado no aparelho antes do próximo pedido: se a coleta cair, a próxima retoma de onde parou.
 */
export async function coletar(ctx) {
  const log = ctx.log || nada;
  const progresso = ctx.aoProgresso || nada;
  const agora = ctx.agora || (() => Math.floor(Date.now() / 1000));
  const lote = Math.min(5000, Math.max(1, Number(ctx.config.lote) || 2000));

  log('Lendo informações do ESP…');
  const info = await lerInfo(ctx);
  const serie = info.serie.toLowerCase();
  const estado = await carregarEstado(ctx.armazem);
  if (info.id) estado.dispositivo = info.id;
  const anterior = estado.esp[serie];
  if (!anterior) {
    const conhecidas = Object.keys(estado.esp);
    if (conhecidas.length) log(`Série nova no cartão (${serie}); a numeração recomeçou. Série anterior: ${conhecidas.join(', ')}.`, 'aviso');
    estado.esp[serie] = { ultimo_n: 0, ultimo_ev: 0 };
  }
  log(`ESP ${info.id || ''} · série ${serie} · n ${info.n_primeiro}–${info.n_ultimo} · fw ${info.fw || '?'}`);

  // 1) hora
  const res = { serie, info, dispositivo: info.id || '', linhasNovas: 0, eventosNovos: 0, horaAjustada: false, ajusteHora: null, ultimaLinha: null, semHora: 0, avisos: [] };
  const dif = agora() - Number(info.agora || 0);
  if (!info.hora_ok || Math.abs(dif) > 2) {
    const rh = jsonOuErro(await pedirEsp(ctx, 'POST', '/api/hora', { agora: agora() }), '/api/hora');
    res.horaAjustada = true;
    res.ajusteHora = rh.ajuste_s ?? null;
    log(info.hora_ok ? `Hora do ESP acertada pelo celular (diferença de ${Math.round(dif)} s).` : 'ESP estava sem hora; hora acertada pelo celular.', 'ok');
  } else {
    res.ajusteHora = Math.round(dif);
    log(`Hora do ESP já estava certa (diferença de ${Math.round(dif)} s).`);
  }

  // 2) dados em lotes
  let desde = estado.esp[serie].ultimo_n || 0;
  if (Number(info.n_ultimo || 0) < desde) {
    const m = `O ESP informa n até ${info.n_ultimo}, menor que o já coletado (${desde}). Recomeçando a contagem desta série.`;
    res.avisos.push(m); log(m, 'aviso');
    desde = 0;
  }
  if (info.n_primeiro > desde + 1) {
    const m = `Linhas n ${desde + 1}–${info.n_primeiro - 1} não existem mais no cartão (apagadas ou perdidas).`;
    res.avisos.push(m); log(m, 'aviso');
    desde = Number(info.n_primeiro) - 1;   // pula direto para o que existe
  }
  const totalEsperado = Math.max(0, Number(info.n_ultimo || 0) - desde);
  if (totalEsperado === 0) log('Nenhuma linha nova no ESP.');
  let mais = totalEsperado > 0;
  let baixadas = 0;
  while (mais) {
    const r = await pedirEsp(ctx, 'GET', `/api/dados?desde=${desde}&max=${lote}`);
    const serieCab = (r.cabecalhos['x-serie'] || serie).toLowerCase();
    if (serieCab !== serie) throw new ErroColetor('A série do cartão mudou durante a coleta (cartão trocado ou formatado). Toque em "Coletar agora" de novo.', 'esp_serie_mudou');
    const { linhas } = lerCsv(r.texto);
    const validas = linhas.filter((l) => /^\d+$/.test(String(l.n)) && Number(l.n) > desde);
    const ultimoCab = parseInt(r.cabecalhos['x-ultimo-n'], 10);
    // X-Ultimo-N = maior n coberto pela janela (pode vir sem linhas, se houver buraco no cartão)
    const ultimo = Math.max(desde, validas.length ? Number(validas[validas.length - 1].n) : 0, Number.isFinite(ultimoCab) ? ultimoCab : 0);
    mais = r.cabecalhos['x-mais'] === '1' && ultimo > desde;
    if (validas.length) {
      const nuvem = validas.map((l) => linhaParaNuvem(l, serie));
      res.linhasNovas += await gravarLocal(ctx.armazem, estado, nuvem);
      res.semHora += nuvem.filter((l) => !temTs(l)).length;
      res.ultimaLinha = nuvem[nuvem.length - 1];
      estado.esp[serie].ultimo_n = ultimo;
      estado.pendente = true;
      await salvarEstado(ctx.armazem, estado);
      baixadas += validas.length;
      progresso({ etapa: 'dados', feito: baixadas, total: Math.max(totalEsperado, baixadas) });
      log(`Lote recebido: n ${validas[0].n}–${ultimo} (${validas.length} linhas).`);
    } else if (ultimo > desde) {
      estado.esp[serie].ultimo_n = ultimo;
      await salvarEstado(ctx.armazem, estado);
      log(`Janela n ${desde + 1}–${ultimo} vazia no cartão (buraco); seguindo.`, 'aviso');
    }
    desde = ultimo;
  }

  // 3) eventos
  let desdeEv = estado.esp[serie].ultimo_ev || 0;
  for (let volta = 0; volta < 100; volta++) {
    const r = await pedirEsp(ctx, 'GET', `/api/eventos?desde=${desdeEv}&max=${lote}`);
    const evs = lerCsv(r.texto).linhas.filter((e) => /^\d+$/.test(String(e.n)) && Number(e.n) > desdeEv);
    const ultEvCab = parseInt(r.cabecalhos['x-ultimo-n'], 10);
    if (!evs.length) {
      if (r.cabecalhos['x-mais'] === '1' && Number.isFinite(ultEvCab) && ultEvCab > desdeEv) { desdeEv = ultEvCab; continue; }
      break;
    }
    const nuvem = evs.map((e) => ({ ts: e.ts ?? '', serie, n: e.n, tipo: e.tipo ?? '', detalhe: e.detalhe ?? '' }));
    await gravarLocal(ctx.armazem, estado, [], nuvem);
    desdeEv = Math.max(...evs.map((e) => Number(e.n)));
    estado.esp[serie].ultimo_ev = desdeEv;
    estado.pendente = true;
    await salvarEstado(ctx.armazem, estado);
    res.eventosNovos += evs.length;
    if (r.cabecalhos['x-mais'] !== '1') break;
  }
  if (res.eventosNovos) log(`${res.eventosNovos} evento(s) novo(s) do ESP.`);

  // 4) registro de auditoria no ESP (falha aqui não invalida a coleta)
  try {
    await pedirEsp(ctx, 'POST', '/api/coletado', { ate: estado.esp[serie].ultimo_n, coletor: ctx.config.coletor || 'android' });
  } catch (e) {
    log(`Não foi possível registrar a coleta no ESP: ${e.message}`, 'aviso');
  }

  // 5) leitura ao vivo (opcional)
  try {
    const ra = await pedirEsp(ctx, 'GET', '/api/agora', undefined, false);
    res.leitura = resumoLeitura(JSON.parse(ra.texto));
  } catch {
    res.leitura = null;
  }
  if (!res.leitura && res.ultimaLinha) res.leitura = resumoLeitura(res.ultimaLinha);

  await salvarEstado(ctx.armazem, estado);
  res.ateN = estado.esp[serie].ultimo_n;
  res.estado = estado;
  res.pendentes = linhasPendentes(estado);
  log(`Coleta concluída: ${res.linhasNovas} linha(s) nova(s)` + (res.semHora ? `, ${res.semHora} sem hora` : '') + `; último n = ${res.ateN}.`, 'ok');
  return res;
}

// ---------------------------------------------------------------- GitHub (§6)

function ctxGh(ctx) {
  const c = ctx.config;
  const faltam = [];
  if (!c.ghDono) faltam.push('dono do repositório');
  if (!c.ghRepo) faltam.push('repositório');
  if (!c.ghToken) faltam.push('token do GitHub');
  if (faltam.length) throw new ErroColetor(`Configure em Configurações: ${faltam.join(', ')}.`, 'gh_config');
  return { api: semBarra(c.ghApi || 'https://api.github.com'), dono: c.ghDono.trim(), repo: c.ghRepo.trim(), ramo: (c.ghRamo || 'main').trim(), token: c.ghToken.trim() };
}

const caminhoUrl = (p) => p.split('/').map(encodeURIComponent).join('/');

async function pedirGh(ctx, g, metodo, urlOuCaminho, corpo, { aceitar404 = false } = {}) {
  const url = /^https?:\/\//.test(urlOuCaminho) ? urlOuCaminho : g.api + urlOuCaminho;
  const cab = { Authorization: `Bearer ${g.token}`, Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28' };
  if (corpo !== undefined) cab['Content-Type'] = 'application/json';
  let r;
  try {
    r = await ctx.http({ metodo, url, cabecalhos: cab, corpo: corpo === undefined ? undefined : JSON.stringify(corpo), tempoMs: ctx.config.tempoGhMs || 60000 });
  } catch (e) {
    throw new ErroColetor(`Sem acesso ao GitHub (${g.api}). O celular está com internet? (detalhe: ${e.message})`, 'sem_internet');
  }
  if (r.status >= 200 && r.status < 300) return r;
  if (r.status === 404 && aceitar404) return r;
  let msg = '';
  try { msg = JSON.parse(r.texto).message || ''; } catch { /* corpo não-JSON */ }
  if (r.status === 401) throw new ErroColetor('Token do GitHub inválido ou expirado (401). Gere um novo token e cole em Configurações.', 'gh_401', 401);
  if (r.status === 403 || r.status === 429) {
    if (/rate limit/i.test(msg) || r.cabecalhos['x-ratelimit-remaining'] === '0') throw new ErroColetor('Limite de requisições do GitHub atingido. Aguarde alguns minutos e envie de novo.', 'gh_limite', r.status);
    throw new ErroColetor(`Sem permissão no GitHub (403). O token precisa ter acesso ao repositório ${g.dono}/${g.repo} com a permissão "Contents: Read and write".` + (msg ? ` (${msg})` : ''), 'gh_403', 403);
  }
  if (r.status === 404) throw new ErroColetor(`Repositório ou ramo não encontrado (404): ${g.dono}/${g.repo} @ ${g.ramo}. Confira dono, nome e ramo, e se o token tem acesso a esse repositório.`, 'gh_404', 404);
  if (r.status === 409 || r.status === 422) throw new ErroColetor(`Conflito ao gravar no GitHub (${r.status}).` + (msg ? ` (${msg})` : ''), 'gh_conflito', r.status);
  throw new ErroColetor(`Erro ${r.status} do GitHub.` + (msg ? ` (${msg})` : ''), 'gh_http', r.status);
}

/** Baixa um arquivo do repositório. Devolve {texto, sha} ou null se não existe. */
async function lerArquivoGh(ctx, g, caminho) {
  const r = await pedirGh(ctx, g, 'GET', `/repos/${g.dono}/${g.repo}/contents/${caminhoUrl(caminho)}?ref=${encodeURIComponent(g.ramo)}`, undefined, { aceitar404: true });
  if (r.status === 404) return null;
  const j = JSON.parse(r.texto);
  if (Array.isArray(j)) throw new ErroColetor(`"${caminho}" é uma pasta no repositório, não um arquivo.`, 'gh_formato');
  if (j.content && j.encoding === 'base64') return { texto: deBase64(j.content), sha: j.sha };
  if (Number(j.size) === 0) return { texto: '', sha: j.sha };
  // Arquivo > 1 MB: a API não manda o conteúdo. Usa o blob pelo sha (exato, sem cache) e, se falhar, o download_url.
  const rb = await pedirGh(ctx, g, 'GET', `/repos/${g.dono}/${g.repo}/git/blobs/${j.sha}`, undefined, { aceitar404: true });
  if (rb.status === 200) {
    const b = JSON.parse(rb.texto);
    if (b.content !== undefined) return { texto: b.encoding === 'base64' ? deBase64(b.content) : b.content, sha: j.sha };
  }
  if (j.download_url) {
    const rd = await pedirGh(ctx, g, 'GET', j.download_url);
    return { texto: rd.texto, sha: j.sha };
  }
  throw new ErroColetor(`Não foi possível baixar "${caminho}" do GitHub.`, 'gh_formato');
}

async function gravarArquivoGh(ctx, g, caminho, texto, sha, mensagem) {
  const corpo = { message: mensagem, content: paraBase64(texto), branch: g.ramo };
  if (sha) corpo.sha = sha;
  const r = await pedirGh(ctx, g, 'PUT', `/repos/${g.dono}/${g.repo}/contents/${caminhoUrl(caminho)}`, corpo);
  try { return JSON.parse(r.texto).content?.sha || null; } catch { return null; }
}

/** Lê-mescla-grava um arquivo no GitHub, repetindo em conflito de sha (409/422). */
async function atualizarArquivoGh(ctx, g, caminho, gerar, log) {
  for (let tentativa = 1; ; tentativa++) {
    const atual = await lerArquivoGh(ctx, g, caminho);
    const novo = gerar(atual ? atual.texto : null);
    if (!novo || (atual && novo.texto === atual.texto)) return { ...(novo || {}), alterado: false };
    try {
      await gravarArquivoGh(ctx, g, caminho, novo.texto, atual?.sha, novo.mensagem);
      return { ...novo, alterado: true };
    } catch (e) {
      if (e.codigo === 'gh_conflito' && tentativa < 4) {
        log(`Conflito ao gravar ${caminho} (alguém enviou ao mesmo tempo); tentando de novo…`, 'aviso');
        await espera(400 * tentativa);
        continue;
      }
      throw e;
    }
  }
}

/** Confere acesso ao repositório (botão "Testar GitHub"). */
export async function testarGithub(ctx) {
  const g = ctxGh(ctx);
  const r = await pedirGh(ctx, g, 'GET', `/repos/${g.dono}/${g.repo}`);
  const j = JSON.parse(r.texto);
  if (j.permissions && j.permissions.push === false) {
    throw new ErroColetor(`O token lê ${g.dono}/${g.repo}, mas não pode gravar. Dê a permissão "Contents: Read and write".`, 'gh_403', 403);
  }
  return { repo: j.full_name || `${g.dono}/${g.repo}`, privado: !!j.private, ramoPadrao: j.default_branch || '' };
}

function faixaN(ls) {
  const ns = ls.map((l) => Number(l.n));
  return [Math.min(...ns), Math.max(...ns)];
}

/**
 * Envio à nuvem (§6/§7): manda tudo com n > nuvem.ultimo_n por série, mesclando com o que já existe
 * no repositório, e atualiza dados/index.json. Pode ser repetido sem duplicar (chave serie+n).
 */
export async function enviar(ctx) {
  const log = ctx.log || nada;
  const progresso = ctx.aoProgresso || nada;
  const agora = ctx.agora || (() => Math.floor(Date.now() / 1000));
  const coletor = ctx.config.coletor || 'android';
  const g = ctxGh(ctx);
  const estado = await carregarEstado(ctx.armazem);
  const fotoEsp = JSON.parse(JSON.stringify(estado.esp));
  const limite = {};
  for (const s of Object.keys(fotoEsp)) limite[s] = { n: estado.nuvem[s]?.ultimo_n || 0, ev: estado.nuvem[s]?.ultimo_ev || 0 };
  const res = { linhasEnviadas: 0, eventosEnviados: 0, arquivos: [], nada: false };

  if (linhasPendentes(estado) === 0 && eventosPendentes(estado) === 0) {
    log('Nada pendente para enviar.');
    res.nada = true;
    estado.pendente = false;
    await salvarEstado(ctx.armazem, estado);
    return res;
  }

  log(`Conectando ao GitHub (${g.dono}/${g.repo})…`);
  await pedirGh(ctx, g, 'GET', `/repos/${g.dono}/${g.repo}`);

  // junta as linhas locais pendentes, agrupadas pelo arquivo de destino
  let arquivosLocais = (estado.arquivos_pendentes || []).filter((a) => a !== ARQ_EVENTOS && (RE_MES.test(a.split('/').pop()) || a === ARQ_SEM_HORA));
  if (!estado.arquivos_pendentes || !estado.arquivos_pendentes.length) {
    arquivosLocais = (await ctx.armazem.listar(PASTA_DADOS)).filter((n) => RE_MES.test(n) || n === 'sem_hora.csv').map((n) => `${PASTA_DADOS}/${n}`);
  }
  const grupos = new Map();
  for (const arq of arquivosLocais) {
    const t = await ctx.armazem.ler(arq);
    if (!t) continue;
    for (const l of lerCsv(t).linhas) {
      const lim = limite[l.serie];
      if (!lim || Number(l.n) <= lim.n) continue;
      const destino = arquivoDaLinha(l);
      if (!grupos.has(destino)) grupos.set(destino, []);
      grupos.get(destino).push(l);
    }
  }
  const comHora = new Set();
  for (const [arq, ls] of grupos) if (arq !== ARQ_SEM_HORA) ls.forEach((l) => comHora.add(chave(l)));
  const arqs = [...grupos.keys()].sort();
  const meses = arqs.filter((a) => a !== ARQ_SEM_HORA).map((a) => a.slice(PASTA_DADOS.length + 1, -4));
  let maxTs = 0;
  for (const k of arqs) for (const l of grupos.get(k)) if (temTs(l)) maxTs = Math.max(maxTs, Number(l.ts));
  const totalLinhas = arqs.reduce((s, k) => s + grupos.get(k).length, 0);

  let feitos = 0;
  for (const arq of arqs) {
    const ls = grupos.get(arq);
    const [de, ate] = faixaN(ls);
    log(`Enviando ${arq} (${ls.length} linhas)…`);
    const r = await atualizarArquivoGh(ctx, g, arq, (textoAtual) => {
      const existentes = textoAtual ? lerCsv(textoAtual).linhas : [];
      const m = mesclar(existentes, ls, arq === ARQ_SEM_HORA ? comHora : null);
      return { texto: escreverCsv(COLUNAS_NUVEM, m.linhas), novas: m.novas, mensagem: `dados: +${m.novas} linhas (${coletor}, n ${de}–${ate})` };
    }, log);
    res.linhasEnviadas += r.novas || 0;
    res.arquivos.push(arq);
    feitos += ls.length;
    progresso({ etapa: 'envio', feito: feitos, total: totalLinhas });
    log(r.alterado ? `${arq}: +${r.novas} linha(s) nova(s) na nuvem.` : `${arq}: já estava atualizado.`, 'ok');
  }
  // linhas que ganharam hora saem do sem_hora.csv da nuvem também
  if (comHora.size && !grupos.has(ARQ_SEM_HORA)) {
    await atualizarArquivoGh(ctx, g, ARQ_SEM_HORA, (textoAtual) => {
      if (textoAtual === null) return null;
      const m = mesclar(lerCsv(textoAtual).linhas, [], comHora);
      return { texto: escreverCsv(COLUNAS_NUVEM, m.linhas), mensagem: `dados: linhas com hora reconstruída saem de sem_hora (${coletor})` };
    }, log);
  }

  // eventos
  const evLocal = await ctx.armazem.ler(ARQ_EVENTOS);
  const evs = evLocal ? lerCsv(evLocal).linhas.filter((e) => limite[e.serie] && Number(e.n) > limite[e.serie].ev) : [];
  if (evs.length) {
    const [de, ate] = faixaN(evs);
    const r = await atualizarArquivoGh(ctx, g, ARQ_EVENTOS, (textoAtual) => {
      const existentes = textoAtual ? lerCsv(textoAtual).linhas : [];
      const m = mesclar(existentes, evs);
      return { texto: escreverCsv(COLUNAS_EVENTOS, m.linhas), novas: m.novas, mensagem: `dados: +${m.novas} eventos (${coletor}, n ${de}–${ate})` };
    }, log);
    res.eventosEnviados = r.novas || 0;
    res.arquivos.push(ARQ_EVENTOS);
  }

  // índice
  await atualizarArquivoGh(ctx, g, ARQ_INDICE, (textoAtual) => {
    let idx = {};
    try { idx = textoAtual ? JSON.parse(textoAtual) : {}; } catch { idx = {}; }
    const series = { ...(idx.series || {}) };
    for (const [s, e] of Object.entries(fotoEsp)) {
      const ant = series[s] || {};
      series[s] = { ultimo_n: Math.max(ant.ultimo_n || 0, e.ultimo_n || 0), ultimo_ev: Math.max(ant.ultimo_ev || 0, e.ultimo_ev || 0) };
    }
    const novo = {
      ...idx,
      proto: PROTO,
      dispositivo: estado.dispositivo || idx.dispositivo || '',
      meses: [...new Set([...(idx.meses || []), ...meses])].sort(),
      series,
      ultimo_ts: Math.max(Number(idx.ultimo_ts) || 0, maxTs) || null,
      atualizado: agora(),
      colunas: COLUNAS_NUVEM,
    };
    return { texto: JSON.stringify(novo, null, 1) + '\n', mensagem: `dados: índice atualizado (${coletor})` };
  }, log);

  // marca como enviado (só o que foi fotografado no início)
  const atual = await carregarEstado(ctx.armazem);
  for (const [s, e] of Object.entries(fotoEsp)) atual.nuvem[s] = { ultimo_n: e.ultimo_n || 0, ultimo_ev: e.ultimo_ev || 0 };
  atual.pendente = linhasPendentes(atual) > 0 || eventosPendentes(atual) > 0;
  if (!atual.pendente) atual.arquivos_pendentes = [];
  atual.ultimo_envio = agora();
  await salvarEstado(ctx.armazem, atual);
  res.estado = atual;
  log(`Envio concluído: ${res.linhasEnviadas} linha(s) e ${res.eventosEnviados} evento(s) novos na nuvem.`, 'ok');
  return res;
}
