// Exercise the page's real download-to-write flow without accessing a serial device.
import { readFileSync } from 'node:fs';
import { webcrypto, createHash } from 'node:crypto';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const source = readFileSync(new URL('../src/pages/install.astro', import.meta.url), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('let transport, loader, md5;', 'let transport, loader = fixtureLoader, md5;')
  .replace('Promise.all([import("esptool-js"), import("spark-md5")])', 'Promise.resolve(fixtureModules)');
const image = new Uint8Array([1, 2, 3, 4]);
const sha256 = createHash('sha256').update(image).digest('hex');

function page(fetch, { checksum = sha256, writeFailure = false, copyFailure = false, cancelConnection = false, connectionFailure = false } = {}) {
  const elements = new Map();
  const el = (name) => {
    if (!elements.has(name)) elements.set(name, {
      hidden: false, textContent: '', value: '', style: {},
      parentElement: { setAttribute() {} }, handlers: {},
      addEventListener(type, handler) { this.handlers[type] = handler; },
      focus() {}, select() { this.selected = true; },
    });
    return elements.get(name);
  };
  let writes = 0, copied;
  const fixtureLoader = { async after() {}, async writeFlash() {
    writes++;
    if (writeFailure) throw new Error('Serial failure');
  } };
  const fixtureModules = [{
    ESPLoader: class {
      constructor() {
        return Object.assign(fixtureLoader, {
          IS_STUB: true, chip: { CHIP_NAME: 'ESP32-S3' },
          async main() { if (connectionFailure) throw new Error('Connection failed'); },
          async detectFlashSize() { return '16MB'; },
          async getSecurityInfo() { return { flashCryptCnt: 0, parsedFlags: { SECURE_BOOT_EN: false } }; },
          async writeReg() {},
        });
      }
    },
    Transport: class { async disconnect() {} },
  }, { default: { ArrayBuffer: { hash() { return 'md5'; } } } }];
  const card = {
    dataset: { url: 'https://user:secret@example.com/firmware.bin?token=private#fragment', version: '0.3.0', sha256: checksum, recovery: '恢复说明' },
    querySelector: (selector) => el(selector.match(/data-el="([^"]+)"/)[1]),
    querySelectorAll: () => [],
  };
  const context = {
    document: { querySelector: () => card, querySelectorAll: () => [] },
    navigator: { serial: { async requestPort() {
      if (cancelConnection) throw new Error('Chooser cancelled');
      return {};
    } }, userAgent: 'Test browser', clipboard: { async writeText(text) {
      if (copyFailure) throw new Error('Clipboard unavailable');
      copied = text;
    } } },
    fixtureLoader, fixtureModules,
    window: { dispatchEvent() {} }, addEventListener() {}, CustomEvent: class {},
    fetch, crypto: webcrypto, performance, URL, Uint8Array,
    console: { error() {} },
  };
  vm.runInNewContext(script, context);
  return { el, card, install: () => el('install').handlers.click(),
    reconnect: () => el('retry').handlers.click(),
    copy: () => el('copy-diagnostics').handlers.click(),
    get writes() { return writes; }, get copied() { return copied; } };
}
const response = (status, body = image) => new Response(body, { status,
  headers: { 'x-cos-request-id': 'test-request-123' } });
const report = (p) => p.el('diagnostic-text').value;

for (const [status, message] of [[403, '被拒绝'], [429, '过于频繁'], [404, '未找到'], [503, '服务暂不可用'], [418, '下载未成功']]) {
  const p = page(async () => response(status));
  await p.install();
  assert.equal(p.writes, 0);
  assert.match(p.el('failure-title').textContent, new RegExp(message));
  assert.match(p.el('failure-message').textContent, /尚未擦除或写入/);
  assert.equal(p.el('failure-code').textContent, `DOWNLOAD_HTTP_${status}`);
  assert.equal(p.el('status').textContent, '下载未完成');
  assert.equal(p.el('connection-help').hidden, true);
  assert.match(report(p), new RegExp(`DOWNLOAD_HTTP_${status}`));
  assert.match(report(p), /test-request-123/);
  assert.match(report(p), /固件版本: 0.3.0/);
  assert.doesNotMatch(report(p), /https?:|example\.com|firmware\.bin|下载地址|secret|token=|private|fragment|user@/);
  await p.copy();
  assert.equal(p.copied, report(p));
  assert.match(p.el('copy-status').textContent, /已复制/);
}

const unreachable = page(async () => { throw new TypeError('Failed to fetch'); }, { copyFailure: true });
await unreachable.install();
assert.equal(unreachable.writes, 0);
assert.match(report(unreachable), /DOWNLOAD_UNREACHABLE/);
assert.match(report(unreachable), /未取得响应/);
assert.doesNotMatch(unreachable.el('failure-title').textContent, /被拒绝|防盗刷/);
await unreachable.copy();
assert.equal(unreachable.el('diagnostic-details').open, true);
assert.equal(unreachable.el('diagnostic-text').selected, true);

const interrupted = page(async () => ({ ok: true, status: 200, headers: new Headers(),
  async arrayBuffer() { throw new TypeError('Connection reset'); } }));
await interrupted.install();
assert.equal(interrupted.writes, 0);
assert.match(report(interrupted), /DOWNLOAD_INTERRUPTED/);
assert.match(report(interrupted), /HTTP 状态: 200/);

const mismatch = page(async () => response(200), { checksum: '0'.repeat(64) });
await mismatch.install();
assert.equal(mismatch.writes, 0);
assert.match(report(mismatch), /VERIFY_MISMATCH/);
assert.match(report(mismatch), new RegExp(sha256));

const brokenWrite = page(async () => response(200), { writeFailure: true });
await brokenWrite.install();
assert.equal(brokenWrite.writes, 1);
assert.match(report(brokenWrite), /FLASH_FAILED/);
assert.match(report(brokenWrite), /已进入擦写流程: 是/);
assert.doesNotMatch(brokenWrite.el('failure-message').textContent, /尚未擦除/);
assert.equal(brokenWrite.el('connection-help').hidden, false);

const cancelled = page(async () => response(403), { cancelConnection: true });
await cancelled.install();
const previousReport = report(cancelled);
await cancelled.reconnect();
assert.equal(cancelled.card.dataset.phase, 'failed');
assert.equal(report(cancelled), previousReport);
assert.equal(cancelled.writes, 0);

const brokenConnection = page(async () => response(200), { connectionFailure: true });
await brokenConnection.reconnect();
assert.equal(brokenConnection.el('failure-code').textContent, 'CONNECT_FAILED');
assert.equal(brokenConnection.el('connection-help').hidden, false);
assert.equal(brokenConnection.writes, 0);

const unsafeHeader = page(async () => new Response('', { status: 403,
  headers: { 'x-request-id': 'https://example.com/firmware.bin?token=private' } }));
await unsafeHeader.install();
assert.doesNotMatch(report(unsafeHeader), /example\.com|token=|x-request-id/);

let attempt = 0;
const retry = page(async () => response(++attempt === 1 ? 403 : 200));
await retry.install();
assert.equal(retry.el('diagnostics').hidden, false);
await retry.reconnect();
assert.equal(retry.card.dataset.phase, 'connected');
assert.equal(retry.writes, 0);
assert.equal(attempt, 1);
assert.equal(retry.el('confirm').checked, false);
assert.equal(retry.el('install').disabled, true);
retry.el('confirm').checked = true;
retry.el('confirm').handlers.change();
assert.equal(retry.el('install').disabled, false);
await retry.install();
assert.equal(retry.card.dataset.phase, 'done');
assert.equal(retry.el('diagnostics').hidden, true);
assert.equal(report(retry), '');
console.log('PASS: installer HTTP, network, body, digest, flash, copy and retry regressions');
