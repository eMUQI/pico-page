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

function page(fetch, { checksum = sha256, eraseFailure = false, writeFailure = false, copyFailure = false, cancelConnection = false, connectionFailure = false,
  onErase = async () => {}, onWrite = async () => {}, onReset = async () => {} } = {}) {
  const elements = new Map();
  const el = (name) => {
    if (!elements.has(name)) elements.set(name, {
      hidden: false, textContent: '', value: '', style: {},
      parentElement: { dataset: {}, attributes: {},
        setAttribute(name, value) { this.attributes[name] = String(value); },
        removeAttribute(name) { delete this.attributes[name]; },
      }, handlers: {},
      addEventListener(type, handler) { this.handlers[type] = handler; },
      focus() {}, select() { this.selected = true; },
    });
    return elements.get(name);
  };
  let erases = 0, writes = 0, copied;
  const fixtureLoader = { async after() { await onReset(); }, async eraseFlash() {
    erases++;
    if (eraseFailure) throw new Error('Erase failed');
    await onErase();
  }, async writeFlash(options) {
    writes++;
    if (writeFailure) throw new Error('Serial failure');
    await onWrite(options);
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
    get erases() { return erases; }, get writes() { return writes; }, get copied() { return copied; } };
}
const response = (status, body = image) => new Response(body, { status,
  headers: { 'x-cos-request-id': 'test-request-123' } });
const report = (p) => p.el('diagnostic-text').value;

for (const [status, message] of [[403, '被拒绝'], [429, '过于频繁'], [404, '未找到'], [503, '服务暂不可用'], [418, '下载未成功']]) {
  const p = page(async () => response(status));
  await p.install();
  assert.equal(p.erases, 0);
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
assert.equal(unreachable.erases, 0);
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
assert.equal(interrupted.erases, 0);
assert.equal(interrupted.writes, 0);
assert.match(report(interrupted), /DOWNLOAD_INTERRUPTED/);
assert.match(report(interrupted), /HTTP 状态: 200/);

const mismatch = page(async () => response(200), { checksum: '0'.repeat(64) });
await mismatch.install();
assert.equal(mismatch.erases, 0);
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

const brokenErase = page(async () => response(200), { eraseFailure: true });
await brokenErase.install();
assert.equal(brokenErase.erases, 1);
assert.equal(brokenErase.writes, 0);
assert.match(report(brokenErase), /失败阶段: 擦除整片闪存/);
assert.match(report(brokenErase), /已进入擦写流程: 是/);
assert.equal(brokenErase.el('bar').parentElement.dataset.indeterminate, 'false');

const eraseStarted = Promise.withResolvers(), finishErase = Promise.withResolvers();
const writeStarted = Promise.withResolvers(), finishWrite = Promise.withResolvers();
const resetStarted = Promise.withResolvers(), finishReset = Promise.withResolvers();
const flashing = page(async () => response(200), {
  async onErase() { eraseStarted.resolve(); await finishErase.promise; },
  async onWrite(options) {
    assert.equal(options.eraseAll, false);
    assert.equal(flashing.el('stage').textContent, '正在准备写入');
    options.reportProgress(0, 0, 100);
    assert.equal(flashing.el('bar').parentElement.attributes['aria-valuenow'], '0');
    options.reportProgress(0, 42, 100);
    assert.equal(flashing.el('percent').textContent, '42%');
    assert.equal(flashing.el('bar').style.width, '42%');
    assert.equal(flashing.el('bar').parentElement.dataset.indeterminate, 'false');
    options.reportProgress(0, 100, 100);
    writeStarted.resolve();
    await finishWrite.promise;
  },
  async onReset() { resetStarted.resolve(); await finishReset.promise; },
});
function assertWaiting(stage) {
  assert.equal(flashing.card.dataset.phase, 'writing');
  assert.equal(flashing.el('stage').textContent, stage);
  assert.equal(flashing.el('percent').hidden, true);
  assert.equal(flashing.el('bar').parentElement.dataset.indeterminate, 'true');
  assert.equal(flashing.el('bar').parentElement.attributes['aria-valuenow'], undefined);
}
const installation = flashing.install();
assertWaiting('正在下载固件');
await eraseStarted.promise;
assertWaiting('正在擦除整片闪存');
assert.equal(flashing.writes, 0);
finishErase.resolve();
await writeStarted.promise;
assertWaiting('正在完成写入并校验');
finishWrite.resolve();
await resetStarted.promise;
assertWaiting('正在发送重启指令');
finishReset.resolve();
await installation;
assert.equal(flashing.erases, 1);
assert.equal(flashing.card.dataset.phase, 'done');
assert.equal(flashing.el('bar').parentElement.dataset.indeterminate, 'false');

const failedVerification = page(async () => response(200), { async onWrite(options) {
  options.reportProgress(0, 100, 100);
  throw new Error('Device checksum mismatch');
} });
await failedVerification.install();
assert.equal(failedVerification.card.dataset.phase, 'failed');
assert.match(report(failedVerification), /失败阶段: 完成写入并校验/);
assert.equal(failedVerification.el('bar').parentElement.dataset.indeterminate, 'false');

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
console.log('PASS: installer download safety, erase/write/verify/reset states, failure, copy and retry regressions');
