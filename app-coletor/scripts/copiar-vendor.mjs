// Copia os bundles IIFE do Capacitor (core + plugins) para www/vendor, sem bundler.
// Também gera www/js/versao.js a partir do package.json.
// Cada um define uma global: capacitorExports, capacitorPreferences, capacitorFilesystem, capacitorNetwork, capacitorApp.
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const raiz = join(dirname(fileURLToPath(import.meta.url)), '..');
const destino = join(raiz, 'www', 'vendor');
mkdirSync(destino, { recursive: true });
const arquivos = {
  'capacitor.js': '@capacitor/core/dist/capacitor.js',
  'preferences.js': '@capacitor/preferences/dist/plugin.js',
  'filesystem.js': '@capacitor/filesystem/dist/plugin.js',
  'network.js': '@capacitor/network/dist/plugin.js',
  'app.js': '@capacitor/app/dist/plugin.js',
};
for (const [nome, origem] of Object.entries(arquivos)) {
  copyFileSync(join(raiz, 'node_modules', origem), join(destino, nome));
  console.log(`vendor/${nome}  <-  ${origem}`);
}

const { version } = JSON.parse(readFileSync(join(raiz, 'package.json'), 'utf8'));
writeFileSync(join(raiz, 'www', 'js', 'versao.js'), `// Gerado por scripts/copiar-vendor.mjs — não edite.\nexport const VERSAO = '${version}';\n`);
console.log(`js/versao.js  <-  package.json (${version})`);
