const mgmtReportId = 6;
const configReportLength = 12;
const jitterMode = 2;
const disabledMode = 0;
const screensaverModeKeys = [19, 49];
const configTimeMax = 0xffffffffffffn; // Six value bytes fit the peer SET route.
const configFieldRules = {
  71: [0n, 1n], 72: [0n, 1n], 73: [0n, 1n], 74: [0n, 255n],
  75: [0n, 1n], 76: [0n, 1n], 77: [0n, 65535n], 83: [0n, 4294967295n]
};
for (const base of [10, 40]) {
  const rules = {
    1: [1n, 2147483647n], 2: [1n, 128n], 3: [1n, 128n],
    4: [0n, 32766n], 5: [1n, 32767n],
    6: [1n, 255n, [1n, 2n, 3n, 4n, 255n]], 7: [1n, 2n],
    8: [0n, 3n, [0n, 1n, 3n]], 9: [0n, 2n], 10: [0n, 1n],
    11: [0n, configTimeMax], 12: [0n, configTimeMax]
  };
  for (const [offset, rule] of Object.entries(rules))
    configFieldRules[base + Number(offset)] = rule;
}
const borderKeys = [14, 15, 44, 45];
/* Desired edits stay pending through RAM acknowledgements: an unpersisted
   Pico can reboot and lose them before a later explicit Save retry. */
const unconfirmedConfigEdits = new Set();
const attemptedConfigEdits = new Set();
const configTokens = new Set();
const configAckTimeout = 3000; // Firmware staging expires after two seconds.
const configOp = {capability: 1, set: 2, borderPair: 3, query: 4, save: 5, check: 6};
const configResults = ['OK', 'INVALID', 'BUSY', 'CONFLICT', 'FLASH_MISMATCH', 'EXPIRED', 'INCOMPLETE'];
let pendingConfigOperation;
let configConnectionGeneration = 0;
let activeConfigGeneration;
let configWriteQueue = Promise.resolve();
var device;

const packetType = {
  keyboardReportMsg: 1, mouseReportMsg: 2, outputSelectMsg: 3, firmwareUpgradeMsg: 4, switchLockMsg: 7,
  syncBordersMsg: 8, flashLedMsg: 9, wipeConfigMsg: 10, readConfigMsg: 16, writeConfigMsg: 17, saveConfigMsg: 18,
  rebootMsg: 19, getValMsg: 20, setValMsg: 21, getValAllMsg: 22, proxyPacketMsg: 23,
  configMeta: 56, configLow: 57, configHigh: 58, configExecute: 59,
  configAckMeta: 60, configAckValue: 61
};

function cancelConfigOperation(message) {
  configConnectionGeneration++;
  pendingConfigOperation?.reject(new Error(message));
}

function configToken() {
  if (!globalThis.crypto?.getRandomValues)
    throw new Error('Secure random tokens are unavailable; configuration writes are disabled.');
  let token;
  do { token = globalThis.crypto.getRandomValues(new Uint32Array(1))[0]; }
  while (!token || configTokens.has(token));
  configTokens.add(token);
  return token;
}

function configWord(value) {
  return [0, 8, 16, 24].map(shift => Number((BigInt(value) >> BigInt(shift)) & 255n));
}

async function confirmedConfigOperation(role, operation, key = 0, value = 0n) {
  const connected = device;
  const generation = configConnectionGeneration;
  const unknown = error => ({role, state: 'unknown', message: error.message});
  if (activeConfigGeneration !== undefined && activeConfigGeneration !== generation)
    return unknown(new Error('operation cancelled by connection or maintenance change'));
  if (!connected?.opened)
    return unknown(new Error('disconnected'));
  if (pendingConfigOperation)
    throw new Error('Configuration operations must be serialized');
  let timeout;
  let pending;
  try {
    const token = configToken();
    const reply = new Promise((resolve, reject) => {
      pending = {token, role, operation, key, connected, generation, resolve, reject};
      pendingConfigOperation = pending;
      timeout = setTimeout(() => reject(new Error('timeout; outcome unknown')), configAckTimeout);
    });
    const tokenBytes = configWord(token);
    const frames = [
      [packetType.configMeta, [...tokenBytes, role, operation, key, 0]],
      [packetType.configLow, [...tokenBytes, ...configWord(value)]],
      [packetType.configHigh, [...tokenBytes, ...configWord(value >> 32n)]],
      [packetType.configExecute, [...tokenBytes, 0, 0, 0, 0]]
    ];
    const sends = (async () => {
      for (const [type, payload] of frames) {
        if (pendingConfigOperation !== pending || device !== connected || !connected.opened
            || generation !== configConnectionGeneration)
          throw new Error('disconnected or operation cancelled; outcome unknown');
        await connected.sendReport(mgmtReportId, makeReport(type, payload));
      }
      return reply;
    })();
    return await Promise.race([reply, sends]);
  } catch (error) {
    return unknown(error);
  } finally {
    clearTimeout(timeout);
    if (pendingConfigOperation === pending)
      pendingConfigOperation = undefined;
  }
}

function configAcknowledgement(data) {
  const pending = pendingConfigOperation;
  const word = offset => new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset, true);
  if (!pending || device !== pending.connected || !device.opened
      || pending.generation !== configConnectionGeneration || word(3) !== pending.token)
    return;
  if (data[2] === packetType.configAckMeta) {
    if (data[7] !== pending.role || data[8] !== pending.operation || data[9] !== pending.key
        || data[10] >= configResults.length)
      return;
    if (pending.status !== undefined && pending.status !== data[10]) {
      pending.reject(new Error('conflicting acknowledgement; outcome unknown'));
      return;
    }
    pending.status = data[10];
  } else {
    const value = word(7);
    if (pending.value !== undefined && pending.value !== value) {
      pending.reject(new Error('conflicting acknowledgement value; outcome unknown'));
      return;
    }
    pending.value = value;
  }
  if (pending.status !== undefined && pending.value !== undefined) {
    /* A bridge can expire after the peer committed but its ACK was lost.
       Conflict/mismatch also do not prove that flash remained untouched. */
    const state = pending.status === 0 ? 'ok' : pending.status === 5 ? 'unknown'
      : [3, 4].includes(pending.status) ? 'error' : 'rejected';
    pending.resolve({role: pending.role, state, code: configResults[pending.status],
                     value: pending.value, message: 'EXPIRED; operation may have completed'});
  }
}

function configBoardStatus(results, action) {
  return results.map(result => `${result.role === 0 ? 'A' : 'B'}: ${result.state === 'ok'
    ? action : result.state === 'rejected' ? `rejected (${result.code})`
      : result.state === 'error' ? `error (${result.code}; requested state not confirmed)`
        : `unknown (${result.message})`}`).join('; ');
}

async function requireConfigCapabilities() {
  const results = [];
  for (const role of [0, 1]) {
    const result = await confirmedConfigOperation(role, configOp.capability);
    if (result.state === 'ok' && result.value !== 1)
      results.push({role, state: 'rejected', code: 'unsupported confirmation protocol'});
    else
      results.push(result);
  }
  if (results.some(result => result.state !== 'ok'))
    throw new Error(`No configuration writes sent. ${configBoardStatus(results, 'confirmed-save protocol ready')}. Both Picos need compatible firmware.`);
}

function calcChecksum(report) {
  /* CRC-8/ATM, polynomial 0x07, init/xorout 0, no reflection. Keep the
     12-byte USB contract independent of the protected UART wire format. */
  let crc = 0;
  for (let i = 0; i < configReportLength - 1; i++) {
    crc ^= report[i];
    for (let bit = 0; bit < 8; bit++)
      crc = ((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0)) & 0xff;
  }
  return crc;
}

async function sendReport(type, payload = [], sendBoth = false) {
  if (!device || !device.opened)
    return;

  /* First send this one, if the first one gets e.g. rebooted */
  if (sendBoth) {
    var reportProxy = makeReport(type, payload, true);
    await device.sendReport(mgmtReportId, reportProxy);
    }

    var report = makeReport(type, payload, false);
    await device.sendReport(mgmtReportId, report);
}

function makeReport(type, payload, proxy=false) {
  const dataOffset = proxy ? 4 : 3;
  const bytes = payload ? [...payload] : [];
  const capacity = proxy ? 7 : 8;
  if (bytes.some(value => !Number.isInteger(value) || value < 0 || value > 255))
    throw new RangeError('Configuration payload must contain bytes');
  /* The proxy consumes one of the payload bytes for its command. Existing
     packValue returns eight bytes, so only zero padding may be discarded. */
  if (bytes.slice(capacity).some(value => value !== 0))
    throw new RangeError('Configuration payload exceeds report capacity');
  const report = new Uint8Array(configReportLength);
  report.set([0xaa, 0x55, proxy ? packetType.proxyPacketMsg : type]);
  if (proxy)
    report[3] = type;
  report.set(bytes.slice(0, capacity), dataOffset);
  report[report.length - 1] = calcChecksum(report);
  return report;
}

/* Adapted from upstream PR #364: display seconds, retain microseconds on the
   wire. Decimal strings and BigInt avoid both uint32 wrap and Number rounding.
   GET carries seven value bytes, but the peer-proxy SET carries only six.
   The stored uint64's highest byte is not observable through this API. Do not
   change the wire protocol or silently truncate values to fit either limit. */
function isScaledTimer(element) {
  return element.getAttribute('data-type') === 'uint64'
    && element.getAttribute('data-scale') === '1000000';
}

function timerMicroseconds(seconds) {
  const parts = String(seconds).trim().match(/^(\d+)(?:\.(\d{0,6}))?$/);
  if (!parts)
    throw new RangeError('Enter nonnegative seconds with at most 6 decimal places.');
  const value = BigInt(parts[1]) * 1000000n + BigInt((parts[2] || '').padEnd(6, '0'));
  if (value > configTimeMax)
    throw new RangeError('The maximum writable timer is 281474976.710655 seconds.');
  return value;
}

function timerSeconds(microseconds) {
  const fraction = (microseconds % 1000000n).toString().padStart(6, '0').replace(/0+$/, '');
  return (microseconds / 1000000n).toString() + (fraction ? '.' + fraction : '');
}

function configurationInteger(value) {
  const text = String(value).trim();
  if (!/^-?\d+$/.test(text))
    throw new RangeError('Enter a whole decimal number');
  return BigInt(text);
}

function configurationLabel(key) {
  const element = document.querySelector(`.api[data-key="${key}"]`);
  const name = element?.getAttribute('aria-label') || 'Configuration value';
  const output = key >= 11 && key <= 22 ? 'Output A: ' : key >= 41 && key <= 52 ? 'Output B: ' : '';
  return output + name;
}

function validateConfigurationValue(key, value) {
  const rule = configFieldRules[key];
  if (!rule)
    throw new RangeError('Unknown or read-only configuration value');
  if (value < rule[0] || value > rule[1] || (rule[2] && !rule[2].includes(value)))
    throw new RangeError(`${configurationLabel(key)}: enter ${rule[2] ? rule[2].join(', ') : `${rule[0]} to ${rule[1]}`}`);
  return value;
}

function configurationValue(element) {
  return isScaledTimer(element) ? timerMicroseconds(element.value) : configurationInteger(getValue(element));
}

function packValue(element, key, dataType) {
  const widths = {uint8: 1, int8: 1, uint16: 2, int16: 2, uint32: 4, int32: 4, uint64: 7};
  if (!Object.hasOwn(widths, dataType))
    throw new TypeError('Unknown configuration data type');
  const value = validateConfigurationValue(Number(key), configurationValue(element));
  const bits = BigInt(widths[dataType] * 8);
  const signed = dataType.startsWith('int');
  const low = signed ? -(1n << (bits - 1n)) : 0n;
  const high = (1n << (signed ? bits - 1n : bits)) - 1n;
  if (value < low || value > high)
    throw new RangeError('Value does not fit its configuration data type');
  const bytes = new Uint8Array(8);
  bytes[0] = Number(key);
  let remaining = BigInt.asUintN(Number(bits), value);
  for (let i = 1; i <= widths[dataType]; i++) {
    bytes[i] = Number(remaining & 255n);
    remaining >>= 8n;
  }
  return bytes;
}

window.addEventListener('load', function () {
  if (!("hid" in navigator)) {
    document.getElementById('warning').style.display = 'block';
  }
  navigator.hid?.addEventListener?.('disconnect', event => {
    if (event.device === device) {
      cancelConfigOperation('disconnected; outcome unknown');
      showConfigStatus('Disconnected. Outstanding configuration outcomes are unknown; reconnect and retry Save.', true);
    }
  });

  for (const key of screensaverModeKeys) {
    const element = document.querySelector(`[data-key="${key}"]`);
    if (element)
      element.addEventListener('change', updateAutoStartJitter);
  }

  for (const element of document.querySelectorAll('.api')) {
    const rule = configFieldRules[element.getAttribute('data-key')];
    if (rule && element.type === 'number') {
      element.setAttribute('min', rule[0]);
      element.setAttribute('max', rule[1]);
      element.setAttribute('step', '1');
    }
  }

  this.document.getElementById('menu-buttons').addEventListener('click', function (event) {
    window[event.target.dataset.handler]();
  })
});

document.getElementById('submitButton').addEventListener('click', async () => { await saveHandler(); });

async function connectHandler() {
  if (device && device.opened)
    return;

  var devices = await navigator.hid.requestDevice({
    filters: [{ vendorId: 0x2e8a, productId: 0x107c, usagePage: 0xff00, usage: 0x10 }]
  });

  if (!devices.length)
    return;
  cancelConfigOperation('device connection changed; outcome unknown');
  device = devices[0];
  await device.open();
  device.addEventListener('inputreport', handleInputReport);
  document.querySelectorAll('.online').forEach(element => { element.style.opacity = 1.0; });
  await sendReport(packetType.getValAllMsg);
}

async function blinkHandler() {
  await sendReport(packetType.flashLedMsg, []);
}

async function blinkBothHandler() {
  await sendReport(packetType.flashLedMsg, [], true);
}

function getValue(element) {
  if (element.type === 'checkbox')
    return element.checked ? 1 : 0;
  else
    return element.value;
}

function setValue(element, value) {
  element.setAttribute('fetched-value', value);

  if (element.type === 'checkbox') {
    element.checked = value;
  }
  else {
    element.value = value;
    element.dispatchEvent(new Event('input', { bubbles: true }));
  }

  if (screensaverModeKeys.includes(Number(element.getAttribute('data-key'))))
    updateAutoStartJitter();
}

/* The per-output screensaver modes are already persisted and loaded at boot.
   Present a single convenience control without introducing a second source of
   truth: checked means both startup modes are Jitter, and unchecked writes
   Disabled to both. A mixed configuration is shown indeterminate. */
function updateAutoStartJitter() {
  const checkbox = document.getElementById('auto-start-jitter');
  const modes = screensaverModeKeys.map(
    key => document.querySelector(`[data-key="${key}"]`));

  if (!checkbox || modes.some(element => !element || !element.hasAttribute('fetched-value')))
    return;

  const jitterCount = modes.filter(element => Number(element.value) === jitterMode).length;
  checkbox.checked = jitterCount > 0;
  checkbox.indeterminate = jitterCount > 0 && jitterCount < modes.length;
}

function autoStartJitterChanged(checkbox) {
  checkbox.indeterminate = false;
  const mode = checkbox.checked ? jitterMode : disabledMode;

  for (const key of screensaverModeKeys) {
    const element = document.querySelector(`[data-key="${key}"]`);
    if (element)
      element.value = mode;
  }
}


function unpackValue(dataType, data) {
  const dataOffset = 4;
  if (dataType === 'uint64') {
    let value = 0n;
    for (let offset = dataOffset + 6; offset >= dataOffset; offset--)
      value = (value << 8n) | BigInt(data.getUint8(offset));
    // GET exposes 56 bits, even though SET to both outputs can carry only 48.
    return value.toString();
  }
  const methods = {
    uint32: 'getUint32', int32: 'getInt32', uint16: 'getUint16',
    uint8: 'getUint8', int16: 'getInt16', int8: 'getInt8'
  };
  if (!Object.hasOwn(methods, dataType))
    throw new TypeError('Unknown configuration data type');
  return data[methods[dataType]](dataOffset, true);
}

function updateElement(key, event) {
  const element = document.querySelector(`.api[data-key="${key}"]`);
  if (!element)
    return;
  try {
    const value = unpackValue(element.getAttribute('data-type'), event.data);
    if (configFieldRules[key] && !isScaledTimer(element))
      validateConfigurationValue(key, configurationInteger(value));
    /* An uncorrelated local GET must not erase a partially applied desired
       value or make retry Save skip the unconfirmed peer. */
    if (unconfirmedConfigEdits.has(key))
      return;
    setValue(element, isScaledTimer(element) ? timerSeconds(BigInt(value)) : value);
    element.setCustomValidity('');

    if (element.hasAttribute('data-hex'))
      setValue(element, parseInt(value).toString(16));

    if (element.hasAttribute('data-fw-ver')) {
      /* u16 version = major * 1000 + minor + 100; */
      const major = Math.floor((value - 100) / 1000);
      const minor = (value - 100) % 1000;
      setValue(element, `v${major}.${minor}`);
    }
  } catch (error) {
    showConfigStatus(error.message, true);
  }
}

async function readHandler() {
  if (!device || !device.opened)
    return connectHandler();
  return enqueueConfigurationWrite(async () => {
    await sendReport(packetType.getValAllMsg);
    if (unconfirmedConfigEdits.size)
      showConfigStatus('Read connected-device values; unconfirmed edits retained for retry Save. Peer values are not confirmed.');
  });
}

async function handleInputReport(event) {
  const data = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
  if (event.reportId !== mgmtReportId || data.length !== configReportLength
      || data[0] !== 0xaa || data[1] !== 0x55
      || (event.device && event.device !== device)
      || data[data.length - 1] !== calcChecksum(data))
    return;
  if (data[2] === packetType.getValMsg)
    updateElement(data[3], event);
  else if (data[2] === packetType.configAckMeta || data[2] === packetType.configAckValue)
    configAcknowledgement(data);
}

async function rebootHandler() {
  cancelConfigOperation('reboot requested; configuration outcome unknown');
  await sendReport(packetType.rebootMsg);
}

async function enterBootloaderHandler() {
  cancelConfigOperation('bootloader requested; configuration outcome unknown');
  await sendReport(packetType.firmwareUpgradeMsg, [], true);
}

function showConfigStatus(message, isError = false) {
  const status = document.getElementById('config-status');
  status.textContent = message;
  status.style.color = isError ? '#a00' : '';
}

function configurationEdit(element) {
  try {
    const key = Number(element.getAttribute('data-key'));
    const payload = packValue(element, key, element.getAttribute('data-type'));
    element.setCustomValidity('');
    return {element, key, payload, value: configurationValue(element),
            expected: configurationValue(element).toString()};
  } catch (error) {
    element.setCustomValidity(error.message);
    element.reportValidity();
    throw error;
  }
}

function planConfigurationEdits(elements) {
  const editable = [...elements].filter(element => !element.hasAttribute('readonly'));
  for (const element of editable) {
    forgetUnsentRevertedEdit(element);
    if (element.getAttribute('fetched-value') == getValue(element))
      element.setCustomValidity('');
  }
  /* Do not pack unchanged timers: fetched 56-bit values can exceed the 48-bit
     SET limit. Leaving those fields unsent preserves the stored full uint64. */
  const edits = editable.filter(element => unconfirmedConfigEdits.has(Number(element.getAttribute('data-key')))
                                || element.getAttribute('fetched-value') != getValue(element))
                        .map(configurationEdit);
  const result = edits.filter(edit => !borderKeys.includes(edit.key));
  for (const base of [10, 40]) {
    const pair = [base + 4, base + 5].map(key => editable.find(element => Number(element.getAttribute('data-key')) === key));
    const changed = edits.filter(edit => edit.key === base + 4 || edit.key === base + 5);
    if (!changed.length)
      continue;
    if (pair.some(element => !element || !element.hasAttribute('fetched-value')))
      throw new Error('Read both borders before saving calibration');
    const [top, bottom] = pair.map(element => validateConfigurationValue(
      Number(element.getAttribute('data-key')), configurationInteger(getValue(element))));
    if (top >= bottom)
      throw new RangeError('Border Top must be less than Border Bottom');
    /* Both bounds are applied atomically on each Pico, independently of its
       previous interval. This is not an atomic transaction across Picos. */
    result.push({operation: configOp.borderPair, key: base === 10 ? 0 : 1,
                 value: top | (bottom << 32n), elements: pair,
                 expected: [top.toString(), bottom.toString()]});
  }
  return result;
}

async function writeConfigurationEdit(edit) {
  if (!device || !device.opened)
    throw new Error('Connect before applying configuration');
  const elements = edit.elements || [edit.element];
  const expected = edit.elements ? edit.expected : [edit.expected];
  for (const element of elements) {
    const key = Number(element.getAttribute('data-key'));
    unconfirmedConfigEdits.add(key);
    attemptedConfigEdits.add(key);
  }
  const results = [];
  for (const role of [0, 1])
    results.push(await confirmedConfigOperation(role, edit.operation || configOp.set, edit.key, edit.value));
  if (results.some(result => result.state !== 'ok'))
    throw new Error(`${configBoardStatus(results, 'applied in RAM')}. Not saved to flash. Retry Save to reapply unconfirmed edits.`);
  for (let index = 0; index < elements.length; index++) {
    const element = elements[index];
    element.setAttribute('fetched-value', isScaledTimer(element) ? timerSeconds(BigInt(expected[index])) : expected[index]);
  }
  return results;
}

function rememberConfigurationEdits(edits) {
  for (const edit of edits)
    for (const element of edit.elements || [edit.element])
      unconfirmedConfigEdits.add(Number(element.getAttribute('data-key')));
}

function forgetUnsentRevertedEdit(element) {
  const key = Number(element.getAttribute('data-key'));
  /* A failed capability probe sends no mutation. Reverting that unsent edit
     may restore a legacy timer too wide to write; leave its stored bytes alone.
     Once a write was attempted, equality with local baseline is not enough. */
  if (!attemptedConfigEdits.has(key) && element.getAttribute('fetched-value') == getValue(element))
    unconfirmedConfigEdits.delete(key);
}

function enqueueConfigurationWrite(operation) {
  const generation = configConnectionGeneration;
  configWriteQueue = configWriteQueue.then(() => {
    if (generation !== configConnectionGeneration)
      throw new Error('Connection or maintenance state changed; queued configuration cancelled. Retry explicitly.');
    activeConfigGeneration = generation;
    return Promise.resolve().then(operation).finally(() => { activeConfigGeneration = undefined; });
  }).catch(error => {
    showConfigStatus(error.message, true);
    return false;
  });
  return configWriteQueue;
}

async function valueChangedHandler(element) {
  if (element.hasAttribute('readonly'))
    return false;
  if (borderKeys.includes(Number(element.getAttribute('data-key')))) {
    showConfigStatus('Border edits apply together when you Save. Top must be less than Bottom.');
    return true;
  }
  return enqueueConfigurationWrite(async () => {
    forgetUnsentRevertedEdit(element);
    if (unconfirmedConfigEdits.has(Number(element.getAttribute('data-key')))
        || element.getAttribute('fetched-value') != getValue(element)) {
      const edit = configurationEdit(element);
      rememberConfigurationEdits([edit]);
      await requireConfigCapabilities();
      await writeConfigurationEdit(edit);
    }
    else {
      element.setCustomValidity('');
      showConfigStatus('No change to connected-device values. Save to confirm both Picos and persist.');
      return true;
    }
    updateAutoStartJitter();
    showConfigStatus('A: applied in RAM; B: applied in RAM. Not saved to flash; Save to persist.');
    return true;
  });
}

async function saveHandler() {
  if (!device || !device.opened)
    return false;
  return enqueueConfigurationWrite(async () => {
    if (!device || !device.opened)
      throw new Error('Connect before saving configuration');
    const edits = planConfigurationEdits(document.querySelectorAll('.api'));
    rememberConfigurationEdits(edits);
    await requireConfigCapabilities();
    let applied;
    for (const edit of edits)
      applied = await writeConfigurationEdit(edit);
    const snapshots = [];
    for (const role of [0, 1]) {
      const snapshot = await confirmedConfigOperation(role, configOp.query);
      /* A reboot or another writer can discard a just-applied edit before
         QUERY. Do not relabel that as an old, untouched board difference. */
      if (snapshot.state === 'ok' && applied && snapshot.value !== applied[role].value)
        snapshots.push({role, state: 'error', code: 'RAM_CHANGED_AFTER_APPLY'});
      else
        snapshots.push(snapshot);
    }
    if (snapshots.some(result => result.state !== 'ok'))
      throw new Error(`No flash saves sent. ${configBoardStatus(snapshots, 'RAM digest read')}. Retry Save.`);
    /* A Pico could have rebooted between two SETs, losing an earlier edit
       before the last SET's digest was returned. Confirm every intended field
       against the same post-apply snapshot before allowing either flash save. */
    for (const edit of edits) {
      const elements = edit.elements || [edit.element];
      const expected = edit.elements ? edit.expected : [edit.expected];
      for (let index = 0; index < elements.length; index++) {
        const key = Number(elements[index].getAttribute('data-key'));
        const checked = [];
        for (const role of [0, 1]) {
          const result = await confirmedConfigOperation(role, configOp.check, key, BigInt(expected[index]));
          checked.push(result.state === 'ok' && result.value !== snapshots[role].value
            ? {role, state: 'error', code: 'RAM_CHANGED_DURING_CHECK'} : result);
        }
        if (checked.some(result => result.state !== 'ok'))
          throw new Error(`No flash saves sent. ${configurationLabel(key)}: ${configBoardStatus(checked, 'intended value checked in RAM')}. Retry Save.`);
      }
    }
    const saved = [];
    for (const snapshot of snapshots) {
      const result = await confirmedConfigOperation(snapshot.role, configOp.save, 0, BigInt(snapshot.value));
      if (result.state === 'ok' && result.value !== snapshot.value)
        saved.push({role: snapshot.role, state: 'unknown', message: 'saved digest did not match requested RAM digest'});
      else
        saved.push(result);
    }
    updateAutoStartJitter();
    if (saved.some(result => result.state !== 'ok'))
      throw new Error(`${configBoardStatus(saved, 'saved to flash and verified')}. Retry Save; this is not an atomic two-Pico save.`);
    for (const edit of edits)
      for (const element of edit.elements || [edit.element]) {
        const key = Number(element.getAttribute('data-key'));
        unconfirmedConfigEdits.delete(key);
        attemptedConfigEdits.delete(key);
      }
    showConfigStatus(`A: saved to flash and verified; B: saved to flash and verified. ${snapshots[0].value === snapshots[1].value
      ? 'Both settings digests match.' : 'Warning: settings differ between Picos; unchanged pre-existing differences were not synchronized.'}`);
    return true;
  });
}

async function wipeConfigHandler() {
  cancelConfigOperation('wipe requested; configuration outcome unknown');
  await sendReport(packetType.wipeConfigMsg, [], true);
}
