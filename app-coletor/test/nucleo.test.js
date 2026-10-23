// Testes do núcleo do Coletor (node --test). Rodam contra servidores falsos em memória e,
// se estiverem no ar, contra o ESP simulado em Python (127.0.0.1:8088) e o github_falso.py (127.0.0.1:8089).
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  lerCsv, escreverCsv, mesclar, arquivoDaLinha, paraBase64, deBase64, criarHttpFetch, ArmazemMemoria,
  coletar, enviar, testarGithub, carregarEstado, linhasPendentes, ErroColetor, COLUNAS_NUVEM, CONFIG_PADRAO, resumoLeitura,
} from '../www/js/nucleo.js';
import { iniciarEspFalso, iniciarGithubFalso } from './falsos.js';

const http = criarHttpFetch();
const silencio = () => {};
const AGORA = 1792000000; // 2026-10-15 UTC (aprox.)

function ctx(config, armazem = new ArmazemMemoria(), extra = {}) {
  return { http, armazem, config: { ...CONFIG_PADRAO, coletor: 'teste', ...config }, agora: () => AGORA, log: silencio, ...extra };
}

async function linhasLocais(armazem) {
  const nomes = await armazem.listar('dados');
  const todas = [];
  for (const n of nomes.filter((x) => /^\d{4}-\d{2}\.csv$|^sem_hora\.csv$/.test(x))) todas.push(...lerCsv(await armazem.ler(`dados/${n}`)).linhas);
  return todas;
}

async function noAr(url) {
  try { const r = await fetch(url, { signal: AbortSignal.timeout(1500) }); return r.status > 0; } catch { return false; }
}

test('CSV: ida e volta com aspas, vírgulas e CRLF', () => {
  const linhas = [{ a: '1', b: 'x,y', c: 'diz "oi"' }, { a: '2', b: '', c: 'linha\nquebrada' }];
  const t = escreverCsv(['a', 'b', 'c'], linhas);
  assert.deepEqual(lerCsv(t).linhas, linhas);
  assert.deepEqual(lerCsv('a,b\r\n1,2\r\n\r\n').linhas, [{ a: '1', b: '2' }]);
  assert.deepEqual(lerCsv('').linhas, []);
});

test('Divisão por mês em UTC e linhas sem hora', () => {
  assert.equal(arquivoDaLinha({ ts: String(Date.UTC(2026, 9, 31, 23, 59, 59) / 1000) }), 'dados/2026-10.csv');
  assert.equal(arquivoDaLinha({ ts: String(Date.UTC(2026, 10, 1, 0, 0, 0) / 1000) }), 'dados/2026-11.csv');
  assert.equal(arquivoDaLinha({ ts: '' }), 'dados/sem_hora.csv');
});

test('Mesclagem: sem duplicatas por serie+n, ordenada por ts, remoção de chaves', () => {
  const a = [{ serie: 's', n: '2', ts: '200' }, { serie: 's', n: '1', ts: '100' }];
  const b = [{ serie: 's', n: '2', ts: '200', t_med: 'novo' }, { serie: 't', n: '1', ts: '150' }];
  const m = mesclar(a, b, new Set(['s:1']));
  assert.equal(m.novas, 1);
  assert.deepEqual(m.linhas.map((l) => `${l.serie}:${l.n}`), ['t:1', 's:2']);
  assert.equal(m.linhas[1].t_med, 'novo');
});

test('Base64 UTF-8 (acentos e travessão)', () => {
  const s = 'dados: +3 linhas (Lúcia, n 1–3) ✓';
  assert.equal(deBase64(paraBase64(s)), s);
  assert.equal(paraBase64('ç'), Buffer.from('ç').toString('base64'));
});

test('Leitura resumida aceita alarmes em lista (telemetria) ou texto com |', () => {
  assert.deepEqual(resumoLeitura({ t_med: 30, alarmes: ['A', 'B'] }).alarmes, ['A', 'B']);
  assert.deepEqual(resumoLeitura({ t_med: '30.5', alarmes: 'A|B' }).alarmes, ['A', 'B']);
  assert.equal(resumoLeitura({ t_med: '' }).t_med, null);
});

test('Coleta: lotes, hora, eventos, coletado, divisão por mês, sem_hora e só o que é novo', async () => {
  // 3000 linhas a partir de 31/10 20:00 UTC → cruzam para novembro; as 10 primeiras sem hora
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 3000, inicio: Date.UTC(2026, 9, 31, 20) / 1000, semHoraAte: 10 });
  try {
    const arm = new ArmazemMemoria();
    const c = ctx({ espUrl: esp.url + '/', espToken: 'tk', lote: 700 }, arm);
    const r = await coletar(c);
    assert.equal(r.linhasNovas, 3000);
    assert.equal(r.ateN, 3000);
    assert.equal(r.semHora, 10);
    assert.equal(r.horaAjustada, true);
    assert.deepEqual(esp.st.horaPosts, [AGORA]);
    assert.equal(esp.st.pedidosDados, 5);
    assert.deepEqual(esp.st.coletados, [{ ate: 3000, coletor: 'teste' }]);
    assert.deepEqual(r.leitura.alarmes, ['GAS_COMBUSTIVEL']);
    const nomes = (await arm.listar('dados')).sort();
    assert.deepEqual(nomes, ['2026-10.csv', '2026-11.csv', 'eventos.csv', 'sem_hora.csv']);
    const out = lerCsv(await arm.ler('dados/2026-10.csv'));
    assert.deepEqual(out.cabecalho, COLUNAS_NUVEM);
    assert.equal(out.linhas[0].serie, 'aabbccdd');
    assert.equal(lerCsv(await arm.ler('dados/sem_hora.csv')).linhas.length, 10);
    assert.equal((await linhasLocais(arm)).length, 3000);
    const alarme = (await linhasLocais(arm)).find((l) => l.n === '500');
    assert.equal(alarme.alarmes, 'GAS_COMBUSTIVEL|T_DIG_BAIXA');
    const est = await carregarEstado(arm);
    assert.deepEqual(est.esp.aabbccdd, { ultimo_n: 3000, ultimo_ev: 1 });
    assert.equal(est.pendente, true);
    assert.equal(linhasPendentes(est), 3000);

    // segunda coleta: hora já certa, nada novo nos dados; o evento "coleta" anterior é baixado
    const r2 = await coletar(c);
    assert.equal(r2.linhasNovas, 0);
    assert.equal(r2.horaAjustada, false);
    assert.equal(r2.eventosNovos, 1);
    const ev = lerCsv(await arm.ler('dados/eventos.csv')).linhas;
    assert.equal(ev[1].detalhe, 'teste ate 3000, via app');

    // chegam 5 linhas novas: só elas são baixadas
    esp.st.gerar(5, Date.UTC(2026, 10, 3) / 1000);
    const antes = esp.st.pedidosDados;
    const r3 = await coletar(c);
    assert.equal(r3.linhasNovas, 5);
    assert.equal(esp.st.pedidosDados - antes, 1);
    assert.equal((await linhasLocais(arm)).length, 3005);
  } finally { await esp.fechar(); }
});

test('Coleta interrompida retoma de onde parou, sem duplicar', async () => {
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 2500, inicio: 1791000000, falharDadosNoPedido: 3 });
  try {
    const arm = new ArmazemMemoria();
    const c = ctx({ espUrl: esp.url, espToken: 'tk', lote: 1000, tempoEspMs: 3000 }, arm);
    await assert.rejects(coletar(c), (e) => e instanceof ErroColetor && e.codigo === 'esp_inalcancavel');
    assert.equal((await carregarEstado(arm)).esp.aabbccdd.ultimo_n, 2000);
    const r = await coletar(c);
    assert.equal(r.linhasNovas, 500);
    const ns = (await linhasLocais(arm)).map((l) => Number(l.n)).sort((a, b) => a - b);
    assert.equal(ns.length, 2500);
    assert.equal(new Set(ns).size, 2500);
  } finally { await esp.fechar(); }
});

test('Troca de série (cartão formatado) vira série nova, sem colidir com a antiga', async () => {
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 100, inicio: 1791000000 });
  try {
    const arm = new ArmazemMemoria();
    const avisos = [];
    const c = ctx({ espUrl: esp.url, espToken: 'tk' }, arm, { log: (m, n) => { if (n === 'aviso') avisos.push(m); } });
    await coletar(c);
    esp.st.trocarSerie('11223344');
    esp.st.gerar(40, 1791100000);
    const r = await coletar(c);
    assert.equal(r.serie, '11223344');
    assert.equal(r.linhasNovas, 40);
    assert.ok(avisos.some((m) => m.includes('Série nova')));
    const est = await carregarEstado(arm);
    assert.equal(est.esp.aabbccdd.ultimo_n, 100);
    assert.equal(est.esp['11223344'].ultimo_n, 40);
    assert.equal((await linhasLocais(arm)).length, 140);
  } finally { await esp.fechar(); }
});

test('Erros do ESP em português: token errado e ESP fora do ar', async () => {
  const esp = await iniciarEspFalso({ token: 'certo', linhas: 3 });
  try {
    await assert.rejects(coletar(ctx({ espUrl: esp.url, espToken: 'errado' })), (e) => e.codigo === 'esp_token' && /Token do ESP incorreto/.test(e.message));
  } finally { await esp.fechar(); }
  await assert.rejects(coletar(ctx({ espUrl: 'http://127.0.0.1:9', tempoEspMs: 2000 })), (e) => e.codigo === 'esp_inalcancavel' && /Biodigestor-XXXX/.test(e.message));
});

test('Envio ao GitHub: cria arquivos, mescla com a nuvem, index.json, mensagens e idempotência', async () => {
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 1200, inicio: Date.UTC(2026, 9, 31, 12) / 1000, semHoraAte: 3 });
  const gh = await iniciarGithubFalso({ dono: 'lab', limiteConteudo: 4000 });
  try {
    // a nuvem já tem uma linha de outro coletor (série antiga) e um índice
    gh.st.por('dados/2026-10.csv', escreverCsv(COLUNAS_NUVEM, [{ ts: '1790000000', t_med: '29', serie: '99999999', n: '7', ts_ok: '1' }]));
    gh.st.por('dados/index.json', JSON.stringify({ proto: 1, dispositivo: 'biodig-teste', meses: ['2026-09'], series: { '99999999': { ultimo_n: 7, ultimo_ev: 0 } }, ultimo_ts: 1790000000 }));
    const arm = new ArmazemMemoria();
    const cfg = { espUrl: esp.url, espToken: 'tk', ghApi: gh.url, ghDono: 'lab', ghRepo: 'biodigestor-painel', ghRamo: 'main', ghToken: 'teste-gh', coletor: 'Ana' };
    await coletar(ctx(cfg, arm));
    assert.deepEqual(await testarGithub(ctx(cfg, arm)), { repo: 'lab/biodigestor-painel', privado: false, ramoPadrao: 'main' });

    const r = await enviar(ctx(cfg, arm));
    assert.equal(r.linhasEnviadas, 1200);
    assert.equal(r.eventosEnviados, 1);
    const out = lerCsv(gh.st.arquivos.get('dados/2026-10.csv').texto);
    assert.deepEqual(out.cabecalho, COLUNAS_NUVEM);
    assert.ok(out.linhas.some((l) => l.serie === '99999999' && l.n === '7'), 'linha antiga preservada');
    const tss = out.linhas.map((l) => Number(l.ts));
    assert.deepEqual(tss, [...tss].sort((a, b) => a - b));
    assert.ok(gh.st.arquivos.has('dados/2026-11.csv'));
    assert.equal(lerCsv(gh.st.arquivos.get('dados/sem_hora.csv').texto).linhas.length, 3);
    const total = ['dados/2026-10.csv', 'dados/2026-11.csv', 'dados/sem_hora.csv'].reduce((s, a) => s + lerCsv(gh.st.arquivos.get(a).texto).linhas.length, 0);
    assert.equal(total, 1201);
    const idx = JSON.parse(gh.st.arquivos.get('dados/index.json').texto);
    assert.deepEqual(idx.meses, ['2026-09', '2026-10', '2026-11']);
    assert.deepEqual(idx.series.aabbccdd, { ultimo_n: 1200, ultimo_ev: 1 });
    assert.deepEqual(idx.series['99999999'], { ultimo_n: 7, ultimo_ev: 0 });
    assert.equal(idx.dispositivo, 'biodig-teste');
    assert.equal(idx.atualizado, AGORA);
    assert.deepEqual(idx.colunas, COLUNAS_NUVEM);
    assert.equal(idx.ultimo_ts, Date.UTC(2026, 9, 31, 12) / 1000 + 1199 * 60);
    assert.ok(gh.st.commits.some((c) => /^dados: \+\d+ linhas \(Ana, n \d+–\d+\)$/.test(c.mensagem) && c.caminho === 'dados/2026-10.csv'));
    assert.ok(gh.st.commits.every((c) => c.ramo === 'main'));

    const est = await carregarEstado(arm);
    assert.equal(est.pendente, false);
    assert.equal(linhasPendentes(est), 0);
    assert.deepEqual(est.nuvem.aabbccdd, { ultimo_n: 1200, ultimo_ev: 1 });

    // repetir não muda nada
    const commitsAntes = gh.st.commits.length;
    const r2 = await enviar(ctx(cfg, arm));
    assert.equal(r2.nada, true);
    assert.equal(gh.st.commits.length, commitsAntes);

    // nova coleta + envio: mescla com os arquivos > limite (lidos via blob)
    esp.st.gerar(30, Date.UTC(2026, 10, 2) / 1000);
    await coletar(ctx(cfg, arm));
    gh.st.pedidos.length = 0;
    const r3 = await enviar(ctx(cfg, arm));
    assert.equal(r3.linhasEnviadas, 30);
    assert.ok(gh.st.pedidos.some((p) => p.includes('/git/blobs/')), 'usou a API de blobs para arquivo > 1 MB');
    assert.equal(lerCsv(gh.st.arquivos.get('dados/2026-11.csv').texto).linhas.length, lerCsv(await arm.ler('dados/2026-11.csv')).linhas.length);
  } finally { await esp.fechar(); await gh.fechar(); }
});

test('Envio: conflito de sha (409) é repetido e não perde dados', async () => {
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 50, inicio: 1791000000 });
  const gh = await iniciarGithubFalso({ dono: 'lab', conflitos: 2 });
  try {
    const arm = new ArmazemMemoria();
    const cfg = { espUrl: esp.url, espToken: 'tk', ghApi: gh.url, ghDono: 'lab', ghToken: 'teste-gh' };
    await coletar(ctx(cfg, arm));
    const avisos = [];
    await enviar(ctx(cfg, arm, { log: (m, n) => { if (n === 'aviso') avisos.push(m); } }));
    assert.equal(avisos.filter((m) => m.includes('Conflito')).length, 2);
    const mes = [...gh.st.arquivos.keys()].find((k) => /\d{4}-\d{2}\.csv$/.test(k));
    assert.equal(lerCsv(gh.st.arquivos.get(mes).texto).linhas.length, 50);
  } finally { await esp.fechar(); await gh.fechar(); }
});

test('Envio: mensagens claras para 401, 403, 404 e configuração faltando', async () => {
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 5, inicio: 1791000000 });
  const gh = await iniciarGithubFalso({ dono: 'lab' });
  const gh403 = await iniciarGithubFalso({ dono: 'lab', proibido: true });
  try {
    const arm = new ArmazemMemoria();
    const cfg = { espUrl: esp.url, espToken: 'tk', ghApi: gh.url, ghDono: 'lab', ghToken: 'teste-gh' };
    await coletar(ctx(cfg, arm));
    await assert.rejects(enviar(ctx({ ...cfg, ghToken: 'ruim' }, arm)), (e) => e.status === 401 && /inválido ou expirado/.test(e.message));
    await assert.rejects(enviar(ctx({ ...cfg, ghApi: gh403.url }, arm)), (e) => e.status === 403 && /Contents: Read and write/.test(e.message));
    await assert.rejects(enviar(ctx({ ...cfg, ghDono: 'outro' }, arm)), (e) => e.status === 404 && /não encontrado/.test(e.message));
    await assert.rejects(enviar(ctx({ ...cfg, ghToken: '' }, arm)), (e) => e.codigo === 'gh_config' && /token do GitHub/.test(e.message));
    await assert.rejects(enviar(ctx({ ...cfg, ghApi: 'http://127.0.0.1:9', tempoGhMs: 2000 }, arm)), (e) => e.codigo === 'sem_internet');
    assert.equal(linhasPendentes(await carregarEstado(arm)), 5, 'nada marcado como enviado após erro');
  } finally { await esp.fechar(); await gh.fechar(); await gh403.fechar(); }
});

test('Integração: ESP simulado em Python (127.0.0.1:8088), com âncoras', async (t) => {
  const url = process.env.ESP_SIMULADO_URL || 'http://127.0.0.1:8088';
  if (!(await noAr(url + '/api/info'))) return t.skip('ESP simulado fora do ar');
  const arm = new ArmazemMemoria();
  const gh = await iniciarGithubFalso({ dono: 'lab' });
  try {
    const cfg = { espUrl: url, espToken: process.env.ESP_SIMULADO_TOKEN || 'teste123', lote: 2000, ghApi: gh.url, ghDono: 'lab', ghToken: 'teste-gh' };
    const c = { ...ctx(cfg, arm), agora: () => Math.floor(Date.now() / 1000) };
    const r = await coletar(c);
    const linhas = await linhasLocais(arm);
    assert.equal(linhas.length, r.ateN);
    assert.equal(r.serie, '5e1a0c3f');
    const oks = new Set(linhas.map((l) => l.ts_ok));
    assert.ok(oks.has('1') && oks.has('2'), 'há linhas medidas com hora e reconstruídas por âncora');
    for (const l of linhas) assert.equal(l.ts === '', l.ts_ok === '0');
    const e = await enviar(c);
    assert.equal(e.linhasEnviadas, linhas.length);
    const idx = JSON.parse(gh.st.arquivos.get('dados/index.json').texto);
    assert.equal(idx.series['5e1a0c3f'].ultimo_n, r.ateN);
  } finally { await gh.fechar(); }
});

test('Integração: github_falso.py (127.0.0.1:8089), se estiver no ar', async (t) => {
  const url = process.env.GITHUB_FALSO_URL || 'http://127.0.0.1:8089';
  if (!(await noAr(url))) return t.skip('github_falso.py fora do ar');
  const esp = await iniciarEspFalso({ token: 'tk', linhas: 300, inicio: Date.UTC(2026, 9, 20) / 1000 });
  try {
    const arm = new ArmazemMemoria();
    const cfg = { espUrl: esp.url, espToken: 'tk', ghApi: url, ghDono: 'lribeiro770-debug', ghRepo: 'biodigestor-painel', ghToken: 'teste-gh', coletor: 'android-teste-' + Date.now() };
    await coletar(ctx(cfg, arm));
    const r = await enviar(ctx(cfg, arm));
    assert.ok(r.linhasEnviadas >= 0);
    assert.equal(linhasPendentes(await carregarEstado(arm)), 0);
  } finally { await esp.fechar(); }
});

test('Integração: buraco no cartão não trava a coleta (ESP_BURACO_URL, simulado com --buraco)', async (t) => {
  const url = process.env.ESP_BURACO_URL || 'http://127.0.0.1:8090';
  if (!(await noAr(url + '/api/info'))) return t.skip('ESP simulado com buraco fora do ar');
  const arm = new ArmazemMemoria();
  const c = { ...ctx({ espUrl: url, espToken: 'teste123', lote: 5000 }, arm), agora: () => Math.floor(Date.now() / 1000) };
  const r = await coletar(c);
  const info = await (await fetch(url + '/api/info')).json();
  assert.equal(r.ateN, info.n_ultimo, 'chegou ao fim apesar da janela vazia');
  const linhas = await linhasLocais(arm);
  assert.ok(linhas.every((l) => Number(l.n) < 1200 || Number(l.n) > 7300));
  assert.ok(linhas.some((l) => Number(l.n) > 7300));
});
