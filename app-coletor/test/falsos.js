// Servidores falsos para os testes: ESP (protocolo 1) e API de conteúdo do GitHub, em memória.
import http from 'node:http';
import crypto from 'node:crypto';

const COLS_ESP = ['n', 'ts', 'ts_ok', 'boot', 'up', 't_dig', 't_dig2', 't_med', 't_amb', 'ur_amb', 'mq2_rel', 'mq7_rel', 'p_pa', 'vol_nl', 'vazao_nlh', 'rele', 'alarmes', 'sim'];

function ouvir(servidor) {
  return new Promise((ok) => servidor.listen(0, '127.0.0.1', () => ok(`http://127.0.0.1:${servidor.address().port}`)));
}

function lerCorpo(req) {
  return new Promise((ok) => { let b = ''; req.on('data', (c) => { b += c; }); req.on('end', () => ok(b)); });
}

/**
 * ESP falso. opcoes: {token, linhas, inicio (epoch), semHoraAte (n até o qual ts é vazio), falharDadosNoPedido}
 */
export async function iniciarEspFalso(opcoes = {}) {
  const st = {
    token: opcoes.token || 'tk',
    serie: opcoes.serie || 'aabbccdd',
    horaOk: opcoes.horaOk ?? false,
    relogio: opcoes.relogio ?? 1000,
    linhas: [], eventos: [], horaPosts: [], coletados: [], pedidosDados: 0,
    falharDadosNoPedido: opcoes.falharDadosNoPedido || 0,
  };
  st.gerar = (qtd, inicio, semHoraAte = 0) => {
    const base = st.linhas.length ? st.linhas[st.linhas.length - 1].n : 0;
    for (let i = 1; i <= qtd; i++) {
      const n = base + i, ts = inicio + (i - 1) * 60;
      st.linhas.push({ n, ts: n <= semHoraAte ? '' : String(ts), ts_ok: n <= semHoraAte ? '0' : '1', boot: '1', up: String(i * 60),
        t_dig: '30.5', t_dig2: '29.5', t_med: '30', t_amb: '24.1', ur_amb: '70', mq2_rel: '1.01', mq7_rel: '0.99', p_pa: '350',
        vol_nl: (n * 0.03).toFixed(3), vazao_nlh: '1.8', rele: '0', alarmes: n % 500 === 0 ? 'GAS_COMBUSTIVEL|T_DIG_BAIXA' : '', sim: '1' });
    }
  };
  st.trocarSerie = (nova) => { st.serie = nova; st.linhas = []; st.eventos = []; };
  st.evento = (tipo, detalhe) => st.eventos.push({ n: st.eventos.length + 1, ts: '', ts_ok: 0, boot: 1, up: 0, tipo, detalhe });
  if (opcoes.linhas) st.gerar(opcoes.linhas, opcoes.inicio || 1790000000, opcoes.semHoraAte || 0);
  st.evento('boot', 'partida 1');

  const srv = http.createServer(async (req, res) => {
    const u = new URL(req.url, 'http://x');
    const env = (cod, corpo, tipo = 'application/json', extra = {}) => { res.writeHead(cod, { 'Content-Type': tipo, ...extra }); res.end(corpo); };
    const autorizado = () => (req.headers['x-token'] || u.searchParams.get('token')) === st.token;
    if (u.pathname === '/api/info') {
      return env(200, JSON.stringify({ proto: 1, id: 'biodig-teste', fw: '3.0.0-t', serie: st.serie, agora: st.horaOk ? st.relogio : 0, hora_ok: st.horaOk, boot: 1, up: 10,
        n_primeiro: st.linhas[0]?.n || 0, n_ultimo: st.linhas[st.linhas.length - 1]?.n || 0, intervalo_s: 60, sd_ok: true, sd_livre_mb: 100, ev_ultimo: st.eventos.length, colunas: COLS_ESP }));
    }
    if (u.pathname === '/api/agora') return env(200, JSON.stringify({ t_med: 30.1, t_amb: 22.5, ur_amb: 65, p_pa: 310, alarmes: ['GAS_COMBUSTIVEL'], sim: true }));
    if (!autorizado()) return env(401, JSON.stringify({ erro: 'token' }));
    if (u.pathname === '/api/dados') {
      st.pedidosDados++;
      if (st.falharDadosNoPedido && st.pedidosDados === st.falharDadosNoPedido) { req.socket.destroy(); return; }
      const desde = Number(u.searchParams.get('desde') || 0), mx = Math.min(5000, Number(u.searchParams.get('max') || 2000));
      const sel = st.linhas.filter((l) => l.n > desde), lote = sel.slice(0, mx);
      const csv = [COLS_ESP.join(','), ...lote.map((l) => COLS_ESP.map((c) => l[c]).join(','))].join('\n') + '\n';
      return env(200, csv, 'text/csv; charset=utf-8', { 'X-Ultimo-N': String(lote.length ? lote[lote.length - 1].n : desde), 'X-Mais': sel.length > mx ? '1' : '0', 'X-Serie': st.serie });
    }
    if (u.pathname === '/api/eventos') {
      const desde = Number(u.searchParams.get('desde') || 0), sel = st.eventos.filter((e) => e.n > desde);
      const csv = ['n,ts,ts_ok,boot,up,tipo,detalhe', ...sel.map((e) => [e.n, e.ts, e.ts_ok, e.boot, e.up, e.tipo, e.detalhe.includes(',') ? `"${e.detalhe}"` : e.detalhe].join(','))].join('\n') + '\n';
      return env(200, csv, 'text/csv; charset=utf-8', { 'X-Ultimo-N': String(sel.length ? sel[sel.length - 1].n : desde), 'X-Mais': '0', 'X-Serie': st.serie });
    }
    if (req.method === 'POST' && u.pathname === '/api/hora') {
      const b = JSON.parse(await lerCorpo(req)); st.horaPosts.push(b.agora);
      const d = st.horaOk ? b.agora - st.relogio : null; st.horaOk = true; st.relogio = b.agora;
      return env(200, JSON.stringify({ ok: true, ajuste_s: d }));
    }
    if (req.method === 'POST' && u.pathname === '/api/coletado') {
      const b = JSON.parse(await lerCorpo(req)); st.coletados.push(b);
      st.evento('coleta', `${b.coletor} ate ${b.ate}, via app`);
      return env(200, JSON.stringify({ ok: true }));
    }
    env(404, JSON.stringify({ erro: 'rota' }));
  });
  const url = await ouvir(srv);
  return { url, st, fechar: () => new Promise((ok) => srv.close(ok)) };
}

const shaGit = (texto) => { const b = Buffer.from(texto, 'utf8'); return crypto.createHash('sha1').update(`blob ${b.length}\0`).update(b).digest('hex'); };

/**
 * GitHub falso (contents API + blobs + download_url). opcoes: {token, limiteConteudo (bytes), proibido, conflitos (nº de PUTs que dão 409)}
 */
export async function iniciarGithubFalso(opcoes = {}) {
  const st = { token: opcoes.token || 'teste-gh', dono: opcoes.dono || 'dono', repo: opcoes.repo || 'biodigestor-painel', arquivos: new Map(), commits: [],
    limiteConteudo: opcoes.limiteConteudo ?? 1024 * 1024, proibido: !!opcoes.proibido, conflitos: opcoes.conflitos || 0, pedidos: [] };
  st.por = (caminho, texto) => st.arquivos.set(caminho, { texto, sha: shaGit(texto) });
  let base = '';
  const srv = http.createServer(async (req, res) => {
    const u = new URL(req.url, 'http://x');
    st.pedidos.push(`${req.method} ${u.pathname}`);
    const env = (cod, obj, tipo = 'application/json') => { res.writeHead(cod, { 'Content-Type': tipo }); res.end(typeof obj === 'string' ? obj : JSON.stringify(obj)); };
    if (req.headers.authorization !== `Bearer ${st.token}`) return env(401, { message: 'Bad credentials' });
    if (st.proibido) return env(403, { message: 'Resource not accessible by personal access token' });
    const prefixo = `/repos/${st.dono}/${st.repo}`;
    if (u.pathname.startsWith('/raw/')) {
      const c = decodeURIComponent(u.pathname.slice(`/raw/${st.dono}/${st.repo}/main/`.length));
      const a = st.arquivos.get(c); return a ? env(200, a.texto, 'text/plain') : env(404, '404: Not Found', 'text/plain');
    }
    if (!u.pathname.startsWith(prefixo)) return env(404, { message: 'Not Found' });
    const resto = u.pathname.slice(prefixo.length);
    if (resto === '' && req.method === 'GET') return env(200, { full_name: `${st.dono}/${st.repo}`, private: false, default_branch: 'main', permissions: { push: true } });
    if (resto.startsWith('/git/blobs/')) {
      const sha = resto.slice('/git/blobs/'.length);
      const a = [...st.arquivos.values()].find((x) => x.sha === sha);
      return a ? env(200, { sha, size: Buffer.byteLength(a.texto), encoding: 'base64', content: Buffer.from(a.texto).toString('base64').replace(/(.{60})/g, '$1\n') }) : env(404, { message: 'Not Found' });
    }
    if (resto.startsWith('/contents/')) {
      const caminho = decodeURIComponent(resto.slice('/contents/'.length));
      const a = st.arquivos.get(caminho);
      if (req.method === 'GET') {
        if (!a) return env(404, { message: 'Not Found' });
        const tam = Buffer.byteLength(a.texto), grande = tam > st.limiteConteudo;
        return env(200, { type: 'file', path: caminho, sha: a.sha, size: tam, encoding: grande ? 'none' : 'base64', content: grande ? '' : Buffer.from(a.texto).toString('base64').replace(/(.{60})/g, '$1\n'),
          download_url: `${base}/raw/${st.dono}/${st.repo}/main/${caminho}` });
      }
      if (req.method === 'PUT') {
        const b = JSON.parse(await lerCorpo(req));
        if (st.conflitos > 0) {
          st.conflitos--;
          // simula outro coletor gravando no mesmo instante
          st.por(caminho, a ? a.texto : '');
          st.arquivos.get(caminho).sha = shaGit(`${caminho}#${st.conflitos}#${Date.now()}`);
          return env(409, { message: `${caminho} does not match ${b.sha || ''}` });
        }
        if (a && b.sha !== a.sha) return env(a && !b.sha ? 422 : 409, { message: 'sha mismatch' });
        if (!a && b.sha) return env(422, { message: 'sha given for new file' });
        const texto = Buffer.from(b.content, 'base64').toString('utf8');
        st.por(caminho, texto); st.commits.push({ caminho, mensagem: b.message, ramo: b.branch });
        return env(a ? 200 : 201, { content: { path: caminho, sha: st.arquivos.get(caminho).sha } });
      }
    }
    env(404, { message: 'Not Found' });
  });
  base = await ouvir(srv);
  return { url: base, st, fechar: () => new Promise((ok) => srv.close(ok)) };
}
