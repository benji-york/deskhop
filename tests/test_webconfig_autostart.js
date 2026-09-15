/* Native smoke tests for the Web Config auto-start Jitter aggregate control. */

const fs = require('fs');
const vm = require('vm');

function check(condition, message) {
  if (!condition)
    throw new Error(message);
}

function modeElement(value) {
  return {
    value,
    listeners: {},
    attributes: {'fetched-value': String(value)},
    getAttribute(name) {
      return name === 'data-key' ? null : this.attributes[name];
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
  19: modeElement(0),
  49: modeElement(0),
};
const inertElement = {style: {}, addEventListener() {}};
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
    return [];
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
});

const script = fs.readFileSync('webconfig/templates/script.js', 'utf8');
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
