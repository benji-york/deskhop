/* Independent browser/HID double for confirmed configuration operations. */
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

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
    addEventListener() {}, dispatchEvent() {},
  };
}

function crc8(bytes) {
  const bits = bytes.flatMap(byte => Array.from({length: 8}, (_, bit) => (byte >> (7 - bit)) & 1));
  bits.push(...Array(8).fill(0));
  const polynomial = [1, 0, 0, 0, 0, 0, 1, 1, 1];
  for (let offset = 0; offset < bits.length - 8; offset++)
    if (bits[offset])
      for (let bit = 0; bit < polynomial.length; bit++) bits[offset + bit] ^= polynomial[bit];
  return bits.slice(-8).reduce((value, bit) => value * 2 + bit, 0);
}

function littleEndian(bytes) {
  return [...bytes].reduce((value, byte, offset) => value + (BigInt(byte) << BigInt(offset * 8)), 0n);
}

function word(value) {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32LE(Number(value) >>> 0);
  return [...bytes];
}

function page(fields = [21, 22, 51, 52].map(key => element(key))) {
  const elements = new Map(fields.map(field => [Number(field.getAttribute('data-key')), field]));
  const reports = [], commands = [], held = [], deadlines = new Map(), listeners = new Map();
  const ram = [new Map(), new Map()], flash = [new Map(), new Map()];
  const staged = new Map();
  const behavior = {reject: () => 0, drop: () => false, hold: () => false, beforeExecute() {},
                    sendFailure: () => false, ackTransform: frames => frames, reverse: false};
  const status = {style: {}, addEventListener() {}};
  let token = 100, timer = 0;
  const document = {
    querySelector(selector) {
      const match = selector.match(/data-key="(\d+)"/);
      return match ? elements.get(Number(match[1])) : null;
    },
    querySelectorAll(selector) { return selector === '.online' ? [] : [...elements.values()]; },
    getElementById(id) { return id === 'auto-start-jitter' ? null : status; },
  };
  const context = vm.createContext({
    console, document, navigator: {hid: {addEventListener: (type, fn) => listeners.set(type, fn)}},
    Uint8Array, Uint32Array, ArrayBuffer, DataView,
    crypto: {getRandomValues(array) { array[0] = ++token; return array; }},
    setTimeout(callback, ms) { deadlines.set(++timer, {callback, ms}); return timer; },
    clearTimeout(id) { deadlines.delete(id); },
    Event: function Event(type) { this.type = type; },
    addEventListener(type, fn) { if (type === 'load') listeners.set('load', fn); },
  });
  context.window = context;
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../webconfig/templates/script.js'), 'utf8'), context);
  listeners.get('load').call(context);

  function receive(type, payload, source = context.device) {
    const bytes = [0xaa, 0x55, type, ...payload];
    while (bytes.length < 11) bytes.push(0);
    bytes.push(crc8(bytes));
    deliver(Uint8Array.from(bytes), source);
  }
  function deliver(bytes, source = context.device, reportId = 6) {
    return context.handleInputReport({device: source, reportId,
      data: new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)});
  }
  function read(key, value, both = true) {
    ram[0].set(key, value);
    if (both) ram[1].set(key, value);
    const bytes = [];
    for (let index = 0; index < 7; index++) bytes.push(Number((value >> BigInt(index * 8)) & 255n));
    receive(20, [key, ...bytes]);
  }
  function digest(role) {
    let hash = 2166136261;
    const text = [...ram[role]].sort(([a], [b]) => a - b).map(([key, value]) => `${key}:${value}`).join(';');
    for (const char of text) hash = Math.imul(hash ^ char.charCodeAt(0), 16777619) >>> 0;
    return hash;
  }
  function acknowledgements(request, code, result) {
    const payloads = [[60, [...word(request.token), request.role, request.op, request.key, code]],
                      [61, [...word(request.token), ...word(result)]]];
    const frames = payloads.map(([type, payload]) => {
      const bytes = [0xaa, 0x55, type, ...payload];
      return Uint8Array.from([...bytes, crc8(bytes)]);
    });
    if (behavior.reverse) frames.reverse();
    return behavior.ackTransform(frames, request);
  }
  context.device = {
    opened: true,
    async sendReport(id, bytes) {
      reports.push({id, bytes: [...bytes]});
      if (behavior.sendFailure(bytes)) throw new Error('Simulated HID send failure');
      if (id !== 6 || bytes.length !== 12 || bytes[11] !== crc8([...bytes].slice(0, 11)))
        throw new Error('Malformed production HID frame');
      const type = bytes[2], nonce = Number(littleEndian(bytes.slice(3, 7)));
      if (type === 56) staged.set(nonce, {token: nonce, role: bytes[7], op: bytes[8], key: bytes[9]});
      else if (type === 57 || type === 58) {
        const request = staged.get(nonce);
        if (!request) throw new Error('Value frame without META');
        request[type === 57 ? 'lo' : 'hi'] = littleEndian(bytes.slice(7, 11));
      } else if (type === 59) {
        const request = staged.get(nonce);
        if (!request || request.lo === undefined || request.hi === undefined)
          throw new Error('EXEC without complete value');
        request.value = request.lo | (request.hi << 32n);
        commands.push(request);
        behavior.beforeExecute(request);
        let code = behavior.reject(request), result = 0;
        if (!code) {
          if (request.op === 1) result = 1;
          else if (request.op === 2) { ram[request.role].set(request.key, request.value); result = digest(request.role); }
          else if (request.op === 3) {
            const base = request.key === 0 ? 10 : 40;
            if (request.lo >= request.hi || request.hi > 32767n) code = 1;
            else {
              ram[request.role].set(base + 4, request.lo);
              ram[request.role].set(base + 5, request.hi);
              result = digest(request.role);
            }
          } else if (request.op === 4) result = digest(request.role);
          else if (request.op === 5) {
            result = digest(request.role);
            if (BigInt(result) !== request.value) code = 3;
            else flash[request.role] = new Map(ram[request.role]);
          } else if (request.op === 6) {
            result = digest(request.role);
            if (!ram[request.role].has(request.key)) code = 1;
            else if (ram[request.role].get(request.key) !== request.value) code = 3;
          } else code = 1;
        }
        const frames = acknowledgements(request, code, result);
        const release = () => frames.forEach(frame => deliver(frame));
        if (behavior.hold(request)) held.push({request, release, frames});
        else if (!behavior.drop(request)) release();
      } else if (type === 22) {
        for (const [key, value] of ram[0]) read(key, value, false);
      }
    },
  };
  for (const key of elements.keys()) read(key, key % 30 === 15 ? 32767n : 0n);
  return {context, elements, reports, commands, ram, flash, status, behavior, deadlines, held,
          read, receive, deliver, digest, listeners,
          disconnect() { context.device.opened = false; listeners.get('disconnect')({device: context.device}); },
          expire() { for (const {callback} of [...deadlines.values()]) callback(); }};
}

const tick = () => new Promise(resolve => setImmediate(resolve));
module.exports = {element, page, crc8, littleEndian, word, tick};
