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
  const stored = new Map();
  const status = {style: {}, addEventListener() {}};
  const behavior = {rejectWrites: false, omitReadback: false, holdReadback: false, reads: [], failCommand: null};
  const document = {
    querySelector(selector) {
      const match = selector.match(/data-key="(\d+)"/);
      return match ? elements.get(Number(match[1])) : null;
    },
    querySelectorAll() { return [...elements.values()]; },
    getElementById(id) { return id === 'auto-start-jitter' ? null : status; },
  };
  const context = vm.createContext({
    console, document, navigator: {}, Uint8Array, ArrayBuffer, DataView, setTimeout, clearTimeout,
    Event: function Event(type) { this.type = type; },
    addEventListener() {},
  });
  context.window = context;
  vm.runInContext(script, context);
  context.device = {
    opened: true,
    async sendReport(id, bytes) {
      reports.push({id, bytes: [...bytes]});
      if (bytes[2] === behavior.failCommand)
        throw new Error('Simulated HID send failure');
      if (bytes[2] === 21 && !behavior.rejectWrites)
        stored.set(bytes[3], littleEndian([...bytes].slice(4, 11)));
      if (bytes[2] === 20 && !behavior.omitReadback) {
        if (behavior.holdReadback)
          behavior.reads.push(() => read(bytes[3], stored.get(bytes[3])));
        else
          read(bytes[3], stored.get(bytes[3]));
      }
    },
  };
  function read(key, value) {
    stored.set(key, value);
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
  return {context, elements, reports, read, stored, status, behavior};
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

async function asynchronousReadbackTests() {
  const {context, elements, reports, read, stored, status, behavior} = page();
  const first = elements.get(21);
  const second = elements.get(22);
  read(21, 7200000000n);
  first.value = '7200.000001';
  behavior.rejectWrites = true;
  assert.equal(await context.valueChangedHandler(first), false, 'rejected RAM change is not marked applied');
  assert.equal(first.getAttribute('fetched-value'), '7200');
  assert.match(status.textContent, /rejected/);
  assert(!reports.some(report => report.bytes[2] === 18), 'failed edit never persists');
  behavior.rejectWrites = false;

  for (const command of [23, 21, 20]) {
    reports.length = 0;
    behavior.failCommand = command;
    assert.equal(await context.valueChangedHandler(first), false, `command ${command} failure is handled`);
    assert.equal(first.getAttribute('fetched-value'), '7200', 'send failure never advances fetched state');
    assert.match(status.textContent, /HID send failure/);
    assert.equal(reports.at(-1).bytes[2], command, 'no following commands after a send failure');
  }
  behavior.failCommand = null;

  // Trigger the production timeout directly: no real sleep or shortened source deadline.
  behavior.omitReadback = true;
  const deadlines = new Map();
  let timerId = 0;
  context.setTimeout = (callback, ms) => {
    assert.equal(ms, 1500, 'readback keeps its bounded production deadline');
    deadlines.set(++timerId, callback);
    return timerId;
  };
  context.clearTimeout = id => deadlines.delete(id);
  const absent = context.valueChangedHandler(first);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(deadlines.size, 1);
  [...deadlines.values()][0]();
  assert.equal(await absent, false, 'missing timer readback fails closed');
  assert.equal(deadlines.size, 0, 'timeout cleans its pending read');
  assert.equal(first.getAttribute('fetched-value'), '7200');
  assert.match(status.textContent, /no readback/);
  behavior.omitReadback = false;
  context.setTimeout = setTimeout;
  context.clearTimeout = clearTimeout;

  // Two change handlers may fire while a device reply is outstanding. The next
  // field must not be sent until the first field's actual readback completes.
  reports.length = 0;
  behavior.holdReadback = true;
  second.value = '2.5';
  const one = context.valueChangedHandler(first);
  const two = context.valueChangedHandler(second);
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(reports.map(report => report.bytes[2]), [23, 21, 20]);
  assert.equal(behavior.reads.length, 1);
  behavior.reads.shift()();
  assert.equal(await one, true);
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(reports.map(report => report.bytes[2]), [23, 21, 20, 23, 21, 20]);
  assert.equal(behavior.reads.length, 1);
  behavior.reads.shift()();
  assert.equal(await two, true);
  assert.equal(first.getAttribute('fetched-value'), '7200.000001');
  assert.equal(second.getAttribute('fetched-value'), '2.5');
  behavior.holdReadback = false;

  read(21, readMax);
  first.value = '-1';
  assert.equal(await context.valueChangedHandler(first), false);
  first.value = seconds(readMax);
  const count = reports.length;
  assert.equal(await context.valueChangedHandler(first), true);
  assert.equal(first.validityMessage, '', 'restoring unchanged large value clears stale validation error');
  assert.equal(reports.length, count, 'restoring large timer never emits a SET');

  reports.length = 0;
  second.value = '3';
  assert.equal(await context.saveHandler(), true);
  assert.deepEqual(reports.filter(report => report.bytes[2] === 21).map(report => report.bytes[3]), [22]);
  assert.equal(stored.get(21), readMax, 'another field Save does not narrow the legacy-large timer');
  assert.match(status.textContent, /requested/);
  assert.match(status.textContent, /RAM/);
  assert.match(status.textContent, /not acknowledged/);
}

async function main() {
  await asynchronousReadbackTests();
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
  assert.equal(reports.length, 8, 'two peer/local timer SETs, two local GETs, and peer/local SAVE');
  assert.equal(littleEndian(reports[0].bytes.slice(5, 11)), 4294967296n);
  assert.equal(littleEndian(reports[1].bytes.slice(4, 11)), 4294967296n);
  assert.equal(reports[2].bytes[2], 20, 'first timer is checked before the next write');
  assert.equal(littleEndian(reports[3].bytes.slice(5, 11)), 0n, 'zero/unlimited preserved');
  assert.equal(littleEndian(reports[4].bytes.slice(4, 11)), 0n);
  assert.equal(reports[5].bytes[2], 20, 'second timer is checked before Save');
  assert.equal(first.getAttribute('fetched-value'), '4294.967296');
  assert.equal(last.getAttribute('fetched-value'), '0');
  reports.length = 0;

  // Other field types/units and read-only version/checksum formatting stay intact.
  const timeout = element(83, 'uint32', null);
  timeout.value = '1800';
  assert.equal(littleEndian([...context.packValue(timeout, 83, 'uint32')].slice(1)), 1800n);
  const border = element(14, 'int32', null);
  border.value = '-100';
  assert.throws(() => context.packValue(border, 14, 'int32'), {name: 'RangeError'});
  border.value = '100';
  assert.deepEqual([...context.packValue(border, 14, 'int32')], [14, 100, 0, 0, 0, 0, 0, 0]);
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
  assert.equal((html.match(/> Idle Time \(seconds\)<\/label>/g) || []).length, 2);
  assert.equal((html.match(/> Max Time \(seconds; 0 = unlimited\)<\/label>/g) || []).length, 2);
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
