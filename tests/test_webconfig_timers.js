/* Upstream #364 seconds UI, adapted to the existing seven-byte GET and
 * six-byte proxy SET limits. No HID hardware or browser packages required. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');

const root = path.join(__dirname, '..');
const script = fs.readFileSync(path.join(root, 'webconfig/templates/script.js'), 'utf8');
const timerKeys = [21, 22, 51, 52];
const writeMax = (1n << 48n) - 1n;
const readMax = (1n << 56n) - 1n;

function element(key, type = 'uint64', scale = '1000000') {
  const attributes = {'data-key': String(key), 'data-type': type};
  if (scale !== null) attributes['data-scale'] = scale;
  return {
    type: 'text', value: '', validityMessage: '', validityReports: 0,
    getAttribute(name) { return attributes[name] ?? null; },
    hasAttribute(name) { return Object.hasOwn(attributes, name); },
    setAttribute(name, value) { attributes[name] = String(value); },
    setCustomValidity(message) { this.validityMessage = message; },
    reportValidity() { this.validityReports++; return !this.validityMessage; },
    dispatchEvent() {},
  };
}

function page() {
  const elements = new Map(timerKeys.map(key => [key, element(key)]));
  const reports = [];
  const document = {
    querySelector(selector) {
      const match = selector.match(/data-key="(\d+)"/);
      return match ? elements.get(Number(match[1])) : null;
    },
    querySelectorAll() { return [...elements.values()]; },
    getElementById() { return {addEventListener() {}}; },
  };
  const context = vm.createContext({
    console, document, navigator: {}, Uint8Array, ArrayBuffer, DataView,
    Event: function Event(type) { this.type = type; },
    addEventListener() {},
  });
  context.window = context;
  vm.runInContext(script, context);
  context.device = {
    opened: true,
    async sendReport(id, bytes) { reports.push({id, bytes: [...bytes]}); },
  };
  function read(key, value) {
    // The eighth value byte does not exist: byte 11 is the packet CRC.
    const bytes = new Uint8Array(12);
    for (let offset = 4; offset < 11; offset++) {
      bytes[offset] = Number(value & 0xffn);
      value >>= 8n;
    }
    bytes[11] = 0xff;
    context.updateElement(key, {data: new DataView(bytes.buffer)});
  }
  for (const key of timerKeys) read(key, 0n);
  return {context, elements, reports, read};
}

// Independent decimal/byte oracles do not use the production conversion.
function seconds(value) {
  const digits = value.toString().padStart(7, '0');
  const fraction = digits.slice(-6).replace(/0+$/, '');
  return digits.slice(0, -6) + (fraction ? '.' + fraction : '');
}
function littleEndian(bytes) {
  return bytes.reduce((value, byte, offset) => value + (BigInt(byte) << BigInt(offset * 8)), 0n);
}

async function main() {
  const {context, elements, reports, read} = page();
  for (const key of timerKeys) {
    const field = elements.get(key);
    for (const raw of [0n, 1n, 1234567n, 240000000n, 4294967295n, 4294967296n, writeMax]) {
      read(key, raw);
      assert.equal(field.value, seconds(raw), `seconds read at ${key}/${raw}`);
      assert.equal(field.getAttribute('fetched-value'), seconds(raw));
      assert.equal(await context.valueChangedHandler(field), true);
      assert.equal(reports.length, 0, 'unchanged read must not send SET');
      const packed = context.packValue(field, key, 'uint64');
      assert.equal(packed[0], key);
      assert.equal(littleEndian([...packed].slice(1)), raw, 'exact microsecond roundtrip');
      assert.equal(packed[7], 0, 'six-byte proxy capacity is respected');
      const local = context.makeReport(21, packed, false);
      const proxy = context.makeReport(21, packed, true);
      assert.equal(littleEndian([...local].slice(4, 11)), raw, 'local SET payload');
      assert.equal(littleEndian([...proxy].slice(5, 11)), raw, 'peer SET payload');
    }
    for (const raw of [writeMax + 1n, (1n << 53n) + 123n, readMax]) {
      read(key, raw);
      assert.equal(field.value, seconds(raw), 'seven-byte read has no Number precision loss');
      await context.valueChangedHandler(field);
      assert.equal(reports.length, 0, 'unchanged large read remains untouched');
    }
  }

  // All four read-only-in-practice large timers survive Save without any SET.
  await context.saveHandler();
  assert.equal(reports.length, 2, 'only peer/local SAVE is sent for unchanged values');
  assert.deepEqual(reports.map(report => report.bytes.slice(2, 4)), [[23, 18], [18, 0]]);
  reports.length = 0;

  const first = elements.get(21);
  const last = elements.get(52);
  first.value = '4294.967296';
  last.value = '281474976.710656';
  await context.saveHandler();
  assert.equal(reports.length, 0, 'preflight rejects later invalid timer before earlier valid SET');
  assert.match(last.validityMessage, /maximum writable/);
  assert.equal(first.getAttribute('fetched-value'), seconds(readMax), 'rejected Save retains fetched values');

  for (const invalid of ['', ' ', '-1', '+1', 'NaN', 'Infinity', '1e6', '0.0000001',
                         '1.1234567', '1.2.3', '1,5', '281474976.710656', '18446744073709.551615']) {
    last.value = invalid;
    assert.throws(() => context.packValue(last, 52, 'uint64'), {name: 'RangeError'});
    assert.equal(await context.valueChangedHandler(last), false);
    assert.equal(reports.length, 0, `${invalid}: no local/peer update`);
    assert(last.validityMessage, 'invalid input is visible through native validity UI');
  }

  last.value = '0';
  await context.saveHandler();
  assert.equal(last.validityMessage, '', 'corrected value clears validation error');
  assert.equal(reports.length, 6, 'two changed timers and SAVE each go to peer/local');
  assert.equal(littleEndian(reports[0].bytes.slice(5, 11)), 4294967296n);
  assert.equal(littleEndian(reports[1].bytes.slice(4, 11)), 4294967296n);
  assert.equal(littleEndian(reports[2].bytes.slice(5, 11)), 0n, 'zero/unlimited preserved');
  assert.equal(littleEndian(reports[3].bytes.slice(4, 11)), 0n);
  assert.equal(first.getAttribute('fetched-value'), '4294.967296');
  assert.equal(last.getAttribute('fetched-value'), '0');
  reports.length = 0;

  // Other field types/units and read-only version/checksum formatting stay intact.
  const timeout = element(83, 'uint32', null);
  timeout.value = '1800';
  assert.equal(littleEndian([...context.packValue(timeout, 83, 'uint32')].slice(1)), 1800n);
  const border = element(14, 'int32', null);
  border.value = '-100';
  assert.deepEqual([...context.packValue(border, 14, 'int32')], [14, 156, 255, 255, 255, 0, 0, 0]);
  const led = element(73, 'uint8', null);
  led.type = 'checkbox'; led.checked = true;
  assert.deepEqual([...context.packValue(led, 73, 'uint8')], [73, 1, 0, 0, 0, 0, 0, 0]);
  const version = element(78, 'uint16', null);
  version.setAttribute('data-fw-ver', '');
  elements.set(78, version);
  read(78, 207n);
  assert.equal(version.value, 'v0.107');

  const html = fs.readFileSync(path.join(root, 'webconfig/config-unpacked.htm'), 'utf8');
  const packed = fs.readFileSync(path.join(root, 'webconfig/config.htm'), 'utf8');
  assert(html.includes(script), 'generated page contains current script');
  const scaledKeys = [...html.matchAll(/data-key="(\d+)" data-scale="1000000"/g)].map(match => Number(match[1]));
  assert.deepEqual(scaledKeys, timerKeys, 'only four screensaver timers are scaled');
  assert.equal((html.match(/Idle Time \(seconds\)/g) || []).length, 2);
  assert.equal((html.match(/Max Time \(seconds; 0 = unlimited\)/g) || []).length, 2);
  assert(html.includes('281474976.710655 seconds'), 'writable protocol limit is visible');
  for (const control of ['auto-start-jitter', 'System idle timeout (seconds; 0 = unlimited)',
                         'KBD LED as Indicator', 'enterBootloaderHandler', 'submitButton'])
    assert(html.includes(control), `custom control retained: ${control}`);
  const compressed = packed.match(/atob\('([^']+)'\)/);
  assert(compressed, 'packed page contains compressed payload');
  assert.equal(zlib.inflateRawSync(Buffer.from(compressed[1], 'base64')).toString('utf8'), html,
               'packed and unpacked configuration pages agree exactly');
  console.log('webconfig timer tests passed (exact seconds, 48-bit writes/56-bit reads, bounds, no partial invalid saves, generated assets)');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
