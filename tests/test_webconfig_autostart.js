/* Native smoke tests for the Web Config auto-start Jitter aggregate control. */

const fs = require('fs');
const vm = require('vm');
const zlib = require('zlib');

function check(condition, message) {
  if (!condition)
    throw new Error(message);
}

function modeElement(value, key = null, dataType = 'uint8') {
  return {
    value,
    type: 'number',
    listeners: {},
    attributes: {'fetched-value': String(value), 'data-key': key, 'data-type': dataType},
    setCustomValidity(message) { this.validityMessage = message; },
    reportValidity() { return !this.validityMessage; },
    getAttribute(name) {
      return this.attributes[name];
    },
    hasAttribute(name) {
      return Object.hasOwn(this.attributes, name);
    },
    setAttribute(name, newValue) {
      this.attributes[name] = String(newValue);
    },
    addEventListener(name, listener) {
      this.listeners[name] = listener;
    },
    dispatchEvent(event) {
      if (this.listeners[event.type])
        this.listeners[event.type]();
    },
  };
}

const checkbox = {checked: false, indeterminate: false};
const modes = {
  19: modeElement(0, 19),
  49: modeElement(0, 49),
};
const inertElement = {style: {}, addEventListener() {}};
let apiElements = [];
let loadHandler;
const documentStub = {
  getElementById(id) {
    return id === 'auto-start-jitter' ? checkbox : inertElement;
  },
  querySelector(selector) {
    const match = selector.match(/data-key="(\d+)"/);
    return match ? modes[match[1]] : null;
  },
  querySelectorAll() {
    return apiElements;
  },
};
const windowStub = {
  document: documentStub,
  addEventListener(name, listener) {
    if (name === 'load')
      loadHandler = listener;
  },
};

const context = vm.createContext({
  console,
  Event: function Event(type) { this.type = type; },
  navigator: {},
  window: windowStub,
  document: documentStub,
  Uint8Array,
  ArrayBuffer,
  DataView,
  setTimeout,
  clearTimeout,
});

const script = fs.readFileSync(process.env.WEB_CONFIG_SCRIPT || 'webconfig/templates/script.js', 'utf8');
vm.runInContext(script, context);
loadHandler.call(windowStub);

function run(source) {
  return vm.runInContext(source, context);
}

run('updateAutoStartJitter()');
check(!checkbox.checked && !checkbox.indeterminate,
      'both Disabled should display unchecked');

modes[19].value = 2;
run('updateAutoStartJitter()');
check(checkbox.checked && checkbox.indeterminate,
      'mixed modes should display indeterminate');

modes[49].value = 2;
run('updateAutoStartJitter()');
check(checkbox.checked && !checkbox.indeterminate,
      'both Jitter should display checked');

modes[19].value = 0;
modes[19].dispatchEvent({type: 'change'});
check(checkbox.checked && checkbox.indeterminate,
      'changing an individual Mode should refresh the aggregate checkbox');
modes[19].value = 2;

checkbox.checked = false;
run('autoStartJitterChanged(document.getElementById("auto-start-jitter"))');
check(modes[19].value === 0 && modes[49].value === 0,
      'unchecking should disable both startup modes');

checkbox.checked = true;
run('autoStartJitterChanged(document.getElementById("auto-start-jitter"))');
check(modes[19].value === 2 && modes[49].value === 2,
      'checking should select Jitter for both startup modes');

console.log('webconfig auto-start Jitter tests passed');

function rejected(action, name = 'RangeError') {
  try { action(); }
  catch (error) { return error.name === name; }
  return false;
}

function packed(key, type, value) {
  context.testElement = modeElement(String(value), key, type);
  return run(`packValue(testElement, ${key}, '${type}')`);
}

/* Reproduces the predecessor's setUint32 truncation with the production
   script. WEB_CONFIG_SCRIPT permits the identical witness on git-show output. */
const twoHours = 7200000000n;
const timeOracle = Buffer.alloc(8);
timeOracle.writeBigUInt64LE(twoHours);
check(Buffer.from(packed(21, 'uint64', twoHours).slice(1)).equals(timeOracle.subarray(0, 7)),
      'two-hour microsecond timeout must not wrap at 32 bits');

const timeMax = 281474976710655n;
for (const key of [21, 22, 51, 52]) {
  for (const value of [0n, 4294967295n, 4294967296n, twoHours, timeMax]) {
    const payload = packed(key, 'uint64', value);
    const expected = Buffer.alloc(8);
    expected.writeBigUInt64LE(value);
    check(payload[0] === key && Buffer.from(payload.slice(1)).equals(expected.subarray(0, 7)),
          `field ${key}: all seven value bytes encode ${value} exactly`);
    context.testPayload = payload;
    for (const proxy of [false, true]) {
      const report = run(`makeReport(21, testPayload, ${proxy})`);
      const start = proxy ? 5 : 4;
      check(Buffer.from(report.slice(start, start + 6)).equals(expected.subarray(0, 6)),
            `field ${key}: direct/proxy six-byte exact value`);
    }
    const response = run('makeReport(20, testPayload)');
    context.testData = new DataView(response.buffer, response.byteOffset, response.byteLength);
    check(run("unpackValue('uint64', testData)") === String(value),
          `field ${key}: production response decoder preserves ${value}`);
  }
  for (const value of [-1n, timeMax + 1n, 72057594037927935n, 18446744073709551615n])
    check(rejected(() => packed(key, 'uint64', value)), `field ${key}: rejects ${value}`);
}

const fieldCases = [
  ...[71, 72, 73, 75, 76].map(key => [key, 'uint8', 0n, 1n]),
  [74, 'uint8', 0n, 255n], [77, 'uint16', 0n, 65535n], [83, 'uint32', 0n, 4294967295n]
];
for (const base of [10, 40]) {
  fieldCases.push(
    [base + 1, 'uint32', 1n, 2147483647n],
    [base + 2, 'int32', 1n, 128n], [base + 3, 'int32', 1n, 128n],
    [base + 4, 'int32', 0n, 32766n], [base + 5, 'int32', 1n, 32767n],
    [base + 6, 'uint8', 1n, 255n, [1n, 2n, 3n, 4n, 255n]],
    [base + 7, 'uint8', 1n, 2n], [base + 8, 'uint8', 0n, 3n, [0n, 1n, 3n]],
    [base + 9, 'uint8', 0n, 2n], [base + 10, 'uint8', 0n, 1n],
    [base + 11, 'uint64', 0n, timeMax], [base + 12, 'uint64', 0n, timeMax]
  );
}
for (const [key, type, low, high, allowed] of fieldCases) {
  for (const value of allowed || [low, high])
    check(packed(key, type, value)[0] === key, `field ${key}: accepts boundary ${value}`);
  for (const value of [low - 1n, high + 1n])
    check(rejected(() => packed(key, type, value)), `field ${key}: rejects boundary ${value}`);
  if (allowed)
    for (let value = low; value <= high; value++)
      if (!allowed.includes(value))
        check(rejected(() => packed(key, type, value)), `field ${key}: rejects enum hole ${value}`);
}
for (const key of [0, 1, 2, 3, 10, 40, 70, 78, 79, 80, 81, 82, 99, 255, 256])
  check(rejected(() => packed(key, 'uint32', 1)), `read-only/unknown field ${key} rejected`);
for (const value of ['', 'NaN', 'Infinity', '1.5', '1e6', '0x10', '1,000', '--1'])
  check(rejected(() => packed(21, 'uint64', value)), `invalid decimal ${value} rejected`);
check(rejected(() => packed(83, 'unknown', 1), 'TypeError'), 'unknown datatype rejected');
check(rejected(() => packed(83, 'uint8', 256)), 'datatype narrowing cannot truncate');
for (const payload of [[-1], [256], [1.2], ['1']]) {
  context.testPayload = payload;
  check(rejected(() => run('makeReport(21, testPayload)')), 'invalid payload byte rejected');
}

function borderPair(top, bottom, oldTop, oldBottom, base = 10) {
  const pair = [modeElement(top, base + 4, 'int32'), modeElement(bottom, base + 5, 'int32')];
  pair[0].setAttribute('fetched-value', oldTop);
  pair[1].setAttribute('fetched-value', oldBottom);
  return pair;
}
for (const base of [10, 40]) {
  for (const [top, bottom, oldTop, oldBottom, expected] of [
    [200, 300, 0, 100, [base + 5, base + 4]],
    [0, 100, 200, 300, [base + 4, base + 5]],
    [25, 75, 0, 100, [base + 4, base + 5]],
    [0, 32767, 25, 75, [base + 4, base + 5]]
  ]) {
    context.editElements = borderPair(top, bottom, oldTop, oldBottom, base);
    const order = run('planConfigurationEdits(editElements).map(edit => edit.key)');
    check(String(order) === String(expected), 'interval edits expand before shrinking');
    let liveTop = oldTop, liveBottom = oldBottom;
    for (const key of order) {
      if (key === base + 4) liveTop = top;
      else liveBottom = bottom;
      check(liveTop < liveBottom, 'each planned intermediate interval is valid');
    }
  }
  for (const [top, bottom] of [[16384, 16384], [200, 100], [-1, 10], [0, 32768]]) {
    context.editElements = borderPair(top, bottom, 0, 32767, base);
    check(rejected(() => run('planConfigurationEdits(editElements)')), 'invalid final interval rejected');
  }
}
console.log(`webconfig numeric validation passed (${fieldCases.length} writable fields, exact 48-bit times, both border orders)`);

if (!process.env.WEB_CONFIG_SCRIPT) {
  const packedPage = fs.readFileSync('webconfig/config.htm');
  const generated = fs.readFileSync('webconfig/config-unpacked.htm');
  const compressed = packedPage.toString().match(/atob\('([A-Za-z0-9+/=]+)'\)/);
  check(compressed && zlib.inflateRawSync(Buffer.from(compressed[1], 'base64')).equals(generated),
        'compressed page exactly expands to generated HTML');
  check(generated.toString().includes(script.trim()), 'generated HTML contains current production script');
  check(generated.length < 100000, 'generated HTML fits the shipped decompressor buffer');

  /* Independent FAT12 reader verifies the actual embedded artifact without
     depending on mtools or mounted disks in ordinary native test runs. */
  const disk = fs.readFileSync('disk/disk.img');
  check(disk.length === 65536, 'embedded disk remains exactly 64 KiB');
  const sectorBytes = disk.readUInt16LE(11);
  const clusterBytes = sectorBytes * disk[13];
  const fatOffset = sectorBytes * disk.readUInt16LE(14);
  const fatBytes = sectorBytes * disk.readUInt16LE(22);
  const rootOffset = fatOffset + disk[16] * fatBytes;
  const rootBytes = Math.ceil(disk.readUInt16LE(17) * 32 / sectorBytes) * sectorBytes;
  const dataOffset = rootOffset + rootBytes;
  let entry;
  for (let offset = rootOffset; offset < rootOffset + rootBytes; offset += 32)
    if (disk.toString('ascii', offset, offset + 11).toUpperCase() === 'CONFIG  HTM')
      entry = offset;
  check(entry !== undefined, 'embedded FAT has a config.htm directory entry');
  let cluster = disk.readUInt16LE(entry + 26);
  const fileBytes = disk.readUInt32LE(entry + 28);
  const parts = [];
  const visited = new Set();
  while (cluster < 0xff8) {
    check(cluster >= 2 && !visited.has(cluster), 'embedded FAT chain is valid and acyclic');
    visited.add(cluster);
    const start = dataOffset + (cluster - 2) * clusterBytes;
    check(start + clusterBytes <= disk.length, 'embedded file stays in 64 KiB image');
    parts.push(disk.subarray(start, start + clusterBytes));
    const entryOffset = fatOffset + Math.floor(cluster * 3 / 2);
    const word = disk.readUInt16LE(entryOffset);
    cluster = cluster & 1 ? word >> 4 : word & 0xfff;
  }
  check(Buffer.concat(parts).subarray(0, fileBytes).equals(packedPage),
        'embedded FAT config.htm exactly matches regenerated page');
  console.log('webconfig generated page and embedded FAT parity passed');
}

/* Independent polynomial-division oracle, rather than copying the
 * production shift-register routine, covers every bit of USB reports. */
function crc8Oracle(bytes) {
  const bits = [];
  for (const byte of bytes)
    for (let bit = 7; bit >= 0; bit--)
      bits.push((byte >> bit) & 1);
  bits.push(...Array(8).fill(0));
  const polynomial = [1, 0, 0, 0, 0, 0, 1, 1, 1]; // x^8 + x^2 + x + 1
  for (let offset = 0; offset < bits.length - 8; offset++)
    if (bits[offset])
      for (let bit = 0; bit < polynomial.length; bit++)
        bits[offset + bit] ^= polynomial[bit];
  return bits.slice(-8).reduce((value, bit) => value * 2 + bit, 0);
}
check(crc8Oracle(Buffer.from('123456789')) === 0xf4, 'CRC8 standard check vector');

for (const proxy of [false, true]) {
  for (const type of [9, 10, 18, 19, 20, 21, 22]) {
    const report = run(`makeReport(${type}, [83,123,0,0,0,0,0,0], ${proxy})`);
    check(report.length === 12 && report[0] === 0xaa && report[1] === 0x55,
          'USB report retains descriptor length and preamble');
    check(report[2] === (proxy ? 23 : type) && (!proxy || report[3] === type),
          'normal/proxy command is included in payload layout');
    check(report[11] === crc8Oracle(report.slice(0, 11)), 'independent USB CRC8 oracle');
  }
}
check(run('makeReport(22)[11]') === crc8Oracle(run('makeReport(22)').slice(0, 11)),
      'payload omitted still calculates the checksum');
let oversizedRejected = false;
try { run('makeReport(21, [0,0,0,0,0,0,0,1], true)'); }
catch (error) { oversizedRejected = error.name === 'RangeError'; }
check(oversizedRejected, 'nonzero proxy overflow must be explicit');

let updates = 0;
context.captureUpdate = key => { check(key === 83, 'validated field key'); updates++; };
run('var productionUpdateElement = updateElement');
run('updateElement = captureUpdate');
function receive(bytes, reportId = 6, prefix = 0) {
  const buffer = new Uint8Array(bytes.length + prefix + 3);
  buffer.set(bytes, prefix);
  context.reportEvent = {reportId, data: new DataView(buffer.buffer, prefix, bytes.length)};
  run('handleInputReport(reportEvent)');
}
const valid = run('makeReport(20, [83,123,0,0,0,0,0,0])');
receive(valid, 6, 5);
check(updates === 1, 'input handler respects DataView offset and length');
for (let position = 0; position < valid.length * 8; position++) {
  const corrupt = Uint8Array.from(valid);
  corrupt[position >> 3] ^= 1 << (position & 7);
  receive(corrupt);
}
check(updates === 1, 'all 96 single-bit errors rejected before touching UI');
receive(valid.slice(0, 11));
receive(new Uint8Array([...valid, 0]));
receive(valid, 5);
const wrongType = run('makeReport(10, [83])');
receive(wrongType);
const oldXor = Uint8Array.from(valid);
oldXor[11] = oldXor.slice(3, 11).reduce((a, b) => a ^ b, 0);
receive(oldXor);
check(updates === 1, 'malformed length, report ID/type and old XOR response rejected');
console.log('webconfig transport tests passed (CRC8 oracle, command/proxy encoding, 96 bit positions, strict input reports)');

/* Execute real handlers against an independent in-memory USB endpoint. The
   endpoint implements only byte storage/GET replies, not the JS validator. */
run('updateElement = productionUpdateElement');
const sent = [];
const stored = new Map();
let rejectWrites = false;
let omitReadback = false;
context.fakeDevice = {
  opened: true,
  async sendReport(reportId, bytes) {
    check(reportId === 6 && bytes.length === 12, 'real write handler preserves report format');
    sent.push(Uint8Array.from(bytes));
    const command = bytes[2];
    const key = bytes[3];
    if (command === 21 && !rejectWrites)
      stored.set(key, Uint8Array.from(bytes.slice(4, 11)));
    if (command === 20 && !omitReadback) {
      const response = new Uint8Array([0xaa, 0x55, 20, key, ...(stored.get(key) || [0, 0, 0, 0, 0, 0, 0]), 0]);
      response[11] = crc8Oracle(response.slice(0, 11));
      receive(response);
    }
  }
};
run('device = fakeDevice');

(async () => {
  const element = modeElement('7200', 21, 'uint64');
  element.setAttribute('data-scale', '1000000');
  element.setAttribute('fetched-value', '0');
  modes[21] = element;
  context.editElement = element;
  check(await run('valueChangedHandler(editElement)'), 'confirmed change succeeds');
  check(element.getAttribute('fetched-value') === '7200', 'only confirmed seconds become fetched');
  check(sent[0][2] === 23 && sent[1][2] === 21 && sent[2][2] === 20,
        'handler sends protected proxy SET, local SET, then local GET');

  rejectWrites = true;
  element.value = '7200.000001';
  check(!await run('valueChangedHandler(editElement)'), 'readback mismatch rejects the edit');
  check(element.getAttribute('fetched-value') === '7200', 'rejected edit retains old fetched value');
  check(inertElement.textContent.includes('rejected'), 'rejected value is visible to the user');
  rejectWrites = false;

  element.value = '281474976.710656';
  let before = sent.length;
  check(!await run('valueChangedHandler(editElement)'), 'invalid changed value is caught');
  check(sent.length === before, 'invalid edit sends no report');

  const borders = borderPair(200, 300, 0, 100);
  for (const border of borders) modes[border.getAttribute('data-key')] = border;
  apiElements = borders;
  context.editElement = borders[0];
  before = sent.length;
  check(await run('valueChangedHandler(editElement)'), 'border is staged in UI');
  check(sent.length === before, 'single border change waits for Save');
  check(await run('saveHandler()'), 'valid interval saves');
  const changedKeys = sent.slice(before).filter(bytes => bytes[2] === 21).map(bytes => bytes[3]);
  check(String(changedKeys) === '15,14', 'real save executes safe bottom-first interval order');
  check(sent.at(-2)[2] === 23 && sent.at(-2)[3] === 18 && sent.at(-1)[2] === 18,
        'save requests follow successful readbacks');

  const validEdit = modeElement('2', 11, 'uint32');
  validEdit.setAttribute('fetched-value', '1');
  apiElements = [validEdit, ...borderPair(16384, 16384, 0, 32767)];
  before = sent.length;
  check(!await run('saveHandler()'), 'bad final border blocks save');
  check(sent.length === before, 'all fields preflight before any SET or SAVE');

  omitReadback = true;
  element.value = '7200.000002';
  context.editElement = element;
  // Keep the real timeout path; accelerate only its virtual timer in this harness.
  context.setTimeout = callback => setTimeout(callback, 0);
  check(!await run('valueChangedHandler(editElement)'), 'missing readback is explicit failure');
  check(element.getAttribute('fetched-value') === '7200', 'timeout leaves fetched value unchanged');
  check(inertElement.textContent.includes('no readback'), 'readback timeout is visible');
  omitReadback = false;
  context.setTimeout = setTimeout;

  const oversizedResponse = new Uint8Array([0xaa, 0x55, 20, 21, 0, 0, 0, 0, 0, 0, 1, 0]);
  oversizedResponse[11] = crc8Oracle(oversizedResponse.slice(0, 11));
  receive(oversizedResponse);
  check(element.getAttribute('fetched-value') === '281474976.710656', '56-bit read beyond writable range is retained exactly');
  console.log('webconfig real handler tests passed (preflight, safe border save, local readback, rejection, timeout)');
})().catch(error => { console.error(error); process.exitCode = 1; });
