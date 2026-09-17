/* Exact seconds, BigInt bounds and unchanged full-width configuration timers. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const {page, element, littleEndian} = require('./webconfig_test_helpers');
const root = path.join(__dirname, '..');
const script = fs.readFileSync(path.join(root, 'webconfig/templates/script.js'), 'utf8');
const timerKeys = [21, 22, 51, 52];
const writeMax = (1n << 48n) - 1n;
const readMax = (1n << 56n) - 1n;

// Independent decimal/byte oracles do not use production conversion.
function seconds(value) {
  const digits = value.toString().padStart(7, '0');
  const fraction = digits.slice(-6).replace(/0+$/, '');
  return digits.slice(0, -6) + (fraction ? '.' + fraction : '');
}

async function main() {
  const p = page();
  const {context, elements, reports, read, commands, status} = p;
  for (const key of timerKeys) {
    const field = elements.get(key);
    for (const raw of [0n, 1n, 1234567n, 240000000n, 4294967295n, 4294967296n, writeMax]) {
      read(key, raw);
      assert.equal(field.value, seconds(raw), `seconds read at ${key}/${raw}`);
      assert.equal(field.getAttribute('fetched-value'), seconds(raw));
      assert.equal(await context.valueChangedHandler(field), true);
      assert.equal(reports.length, 0, 'unchanged read must not send a write or claim peer confirmation');
      const packed = context.packValue(field, key, 'uint64');
      assert.equal(packed[0], key);
      assert.equal(littleEndian([...packed].slice(1)), raw, 'exact microsecond roundtrip');
      assert.equal(packed[7], 0, 'legacy six-byte write limit is retained');
      const local = context.makeReport(21, packed, false);
      const proxy = context.makeReport(21, packed, true);
      assert.equal(littleEndian([...local].slice(4, 11)), raw);
      assert.equal(littleEndian([...proxy].slice(5, 11)), raw);
    }
    for (const raw of [writeMax + 1n, (1n << 53n) + 123n, readMax]) {
      read(key, raw);
      assert.equal(field.value, seconds(raw), 'seven-byte read has no Number precision loss');
      await context.valueChangedHandler(field);
      assert.equal(reports.length, 0, 'unchanged large read remains untouched');
    }
  }

  // The inaccessible eighth byte must survive too: GET only exposes low56.
  const storedFull = (0xaan << 56n) | readMax;
  for (const role of [0, 1])
    for (const key of timerKeys) p.ram[role].set(key, storedFull);
  assert.equal(await context.saveHandler(), true);
  assert.deepEqual(commands.map(request => request.op), [1, 1, 4, 4, 5, 5]);
  assert(!commands.some(request => request.op === 2), 'unchanged timers are not narrowed by confirmed save');
  for (const role of [0, 1])
    for (const key of timerKeys) assert.equal(p.flash[role].get(key), storedFull);
  assert.match(status.textContent, /saved to flash and verified/);
  reports.length = 0; commands.length = 0;

  const first = elements.get(21), last = elements.get(52);
  first.value = '4294.967296';
  last.value = '281474976.710656';
  assert.equal(await context.saveHandler(), false);
  assert.equal(reports.length, 0, 'later invalid timer prevents even earlier valid SET');
  assert.match(last.validityMessage, /maximum writable/);
  assert.equal(first.getAttribute('fetched-value'), seconds(readMax));

  for (const invalid of ['', ' ', '-1', '+1', 'NaN', 'Infinity', '1e6', '0.0000001',
                         '1.1234567', '1.2.3', '1,5', '281474976.710656', '18446744073709.551615']) {
    last.value = invalid;
    assert.throws(() => context.packValue(last, 52, 'uint64'), {name: 'RangeError'});
    assert.equal(await context.valueChangedHandler(last), false);
    assert.equal(reports.length, 0, `${invalid}: no local/peer mutation`);
    assert(last.validityMessage);
  }
  last.value = seconds(readMax);
  assert.equal(await context.valueChangedHandler(last), true);
  assert.equal(last.validityMessage, '', 'restoring unchanged large timer clears stale validation error');
  assert.equal(reports.length, 0);
  last.value = '0';
  assert.equal(await context.saveHandler(), true);
  const edits = commands.filter(request => request.op === 2);
  assert.deepEqual(edits.map(request => [request.role, request.key, request.value]),
                   [[0, 21, 4294967296n], [1, 21, 4294967296n], [0, 52, 0n], [1, 52, 0n]]);
  assert.equal(first.getAttribute('fetched-value'), '4294.967296');
  assert.equal(last.getAttribute('fetched-value'), '0');
  for (const role of [0, 1])
    for (const key of [22, 51]) assert.equal(p.flash[role].get(key), storedFull);

  // Other field types/units and read-only version formatting stay intact.
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
  assert.deepEqual(scaledKeys, timerKeys);
  assert.equal((html.match(/> Idle Time \(seconds\)<\/label>/g) || []).length, 2);
  assert.equal((html.match(/> Max Time \(seconds; 0 = unlimited\)<\/label>/g) || []).length, 2);
  assert(html.includes('281474976.710655 seconds'));
  for (const control of ['auto-start-jitter', 'System idle timeout (seconds; 0 = unlimited)',
                         'KBD LED as Indicator', 'enterBootloaderHandler', 'submitButton'])
    assert(html.includes(control), `custom control retained: ${control}`);
  const compressed = packed.match(/atob\('([^']+)'\)/);
  assert(compressed);
  assert.equal(zlib.inflateRawSync(Buffer.from(compressed[1], 'base64')).toString('utf8'), html);
  console.log('webconfig timers passed (exact seconds, 48-bit UI writes/56-bit reads, full uint64 preservation, confirmed save, generated assets)');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
