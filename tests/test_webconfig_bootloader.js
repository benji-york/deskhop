/* Exercise the production Web Config bootloader click without HID hardware. */

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function eventTarget() {
  const listeners = new Map();
  return {
    style: {},
    addEventListener(type, listener) {
      assert(!listeners.has(type), `unexpected duplicate ${type} listener`);
      listeners.set(type, listener);
    },
    dispatchEvent(event) {
      assert(listeners.has(event.type), `missing production ${event.type} listener`);
      listeners.get(event.type).call(this, event);
    },
  };
}

function loadWebConfig() {
  const menu = eventTarget();
  const elements = {
    'menu-buttons': menu,
    submitButton: eventTarget(),
    warning: eventTarget(),
  };
  const document = {
    getElementById(id) {
      assert(elements[id], `unexpected element ${id}`);
      return elements[id];
    },
    querySelector() { return null; },
    querySelectorAll() { return []; },
  };
  const context = vm.createContext({
    ...eventTarget(),
    console,
    document,
    navigator: {},
    Uint8Array,
    ArrayBuffer,
    DataView,
  });
  // Browser function declarations are properties of window.
  context.window = context;
  const filename = path.join(__dirname, '../webconfig/templates/script.js');
  vm.runInContext(fs.readFileSync(filename, 'utf8'), context, {filename});
  context.dispatchEvent({type: 'load'});

  const productionHandler = context.enterBootloaderHandler;
  assert.equal(typeof productionHandler, 'function');
  let pending;
  // The real delegated click listener discards the promise. Capture it without
  // replacing the production handler's behavior, so async failures fail Node.
  context.enterBootloaderHandler = function (...args) {
    pending = productionHandler.apply(this, args);
    return pending;
  };
  return {
    setDevice(device) { context.device = device; },
    clickBootloader() {
      pending = undefined;
      menu.dispatchEvent({
        type: 'click',
        target: {dataset: {handler: 'enterBootloaderHandler'}},
      });
      assert(pending && typeof pending.then === 'function',
             'menu click must invoke the actual async bootloader handler');
      return pending;
    },
  };
}

/* Polynomial division is independent of the production CRC shift register. */
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

function checkReport(report, proxy) {
  assert.equal(report.id, 6, 'management HID report ID');
  assert.equal(report.bytes.length, 12, 'fixed HID report size');
  const header = proxy ? [0xaa, 0x55, 23, 4] : [0xaa, 0x55, 4];
  const body = [...header, ...Array(11 - header.length).fill(0)];
  assert.deepEqual(report.bytes.slice(0, 11), body,
                   `${proxy ? 'peer proxy' : 'local'} firmware-upgrade command with zero payload`);
  assert.equal(report.bytes[11], crc8Oracle(body), 'independently computed CRC8');
}

function recordReport(reports, id, bytes) {
  reports.push({id, bytes: Array.from(bytes)});
}

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return {promise, resolve};
}

async function main() {
  assert.equal(crc8Oracle(Buffer.from('123456789')), 0xf4, 'CRC8 standard check vector');
  const page = loadWebConfig();
  const reports = [];

  page.setDevice(undefined);
  await page.clickBootloader();
  page.setDevice({
    opened: false,
    sendReport(id, bytes) { recordReport(reports, id, bytes); },
  });
  await page.clickBootloader();
  assert.equal(reports.length, 0, 'absent or closed devices must not send');

  page.setDevice({
    opened: true,
    async sendReport(id, bytes) { recordReport(reports, id, bytes); },
  });
  try {
    await page.clickBootloader();
  } catch (error) {
    console.error(`Bootloader click rejected after ${reports.length} HID sends.`);
    throw error;
  }
  assert.equal(reports.length, 2, 'one peer proxy and one local report only');
  checkReport(reports[0], true);
  checkReport(reports[1], false);

  reports.length = 0;
  const peer = deferred();
  const local = deferred();
  page.setDevice({
    opened: true,
    sendReport(id, bytes) {
      recordReport(reports, id, bytes);
      assert(reports.length <= 2, 'no duplicate bootloader sends');
      return reports.length === 1 ? peer.promise : local.promise;
    },
  });
  const operation = page.clickBootloader();
  let settled = false;
  operation.then(() => { settled = true; }, () => { settled = true; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(reports.length, 1, 'local reboot must wait for peer send completion');
  checkReport(reports[0], true);
  assert(!settled, 'handler must await the pending peer send');
  peer.resolve();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(reports.length, 2, 'local report follows completed peer send');
  checkReport(reports[1], false);
  assert(!settled, 'handler must also await the local send');
  local.resolve();
  await operation;
  assert.equal(reports.length, 2);

  reports.length = 0;
  const peerFailure = new Error('simulated peer HID send failure');
  page.setDevice({
    opened: true,
    async sendReport(id, bytes) {
      recordReport(reports, id, bytes);
      throw peerFailure;
    },
  });
  await assert.rejects(page.clickBootloader(), error => error === peerFailure,
                       'peer send rejection must propagate to the handler');
  assert.equal(reports.length, 1, 'failed peer send must prevent the local reboot');
  checkReport(reports[0], true);

  console.log('webconfig bootloader tests passed (DOM click, CRC8, peer-first ordering, device guards, send failure)');
}

// A pending promise alone does not keep Node alive. Keep a bounded watchdog so
// a never-settled handler cannot silently exit successfully or hang the suite.
let timeout;
Promise.race([
  main(),
  new Promise((resolve, reject) => {
    timeout = setTimeout(() => reject(new Error('bootloader handler did not finish within 3 seconds')), 3000);
  }),
]).catch(error => {
  console.error(error);
  process.exitCode = 1;
}).finally(() => clearTimeout(timeout));
