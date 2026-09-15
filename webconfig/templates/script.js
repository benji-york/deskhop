const mgmtReportId = 6;
const configReportLength = 12;
const jitterMode = 2;
const disabledMode = 0;
const screensaverModeKeys = [19, 49];
var device;

const packetType = {
  keyboardReportMsg: 1, mouseReportMsg: 2, outputSelectMsg: 3, firmwareUpgradeMsg: 4, switchLockMsg: 7,
  syncBordersMsg: 8, flashLedMsg: 9, wipeConfigMsg: 10, readConfigMsg: 16, writeConfigMsg: 17, saveConfigMsg: 18,
  rebootMsg: 19, getValMsg: 20, setValMsg: 21, getValAllMsg: 22, proxyPacketMsg: 23
};

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

function packValue(element, key, dataType, buffer) {
  const dataOffset = 1;
  var buffer = new ArrayBuffer(8);
  var view = new DataView(buffer);

  const methods = {
    "uint32": view.setUint32,
    "uint64": view.setUint32, /* Yes, I know. :-| */
    "int32": view.setInt32,
    "uint16": view.setUint16,
    "uint8": view.setUint8,
    "int16": view.setInt16,
    "int8": view.setInt8
  };

  if (dataType in methods) {
    const method = methods[dataType];
    if (element.type === 'checkbox')
      view.setUint8(dataOffset, element.checked ? 1 : 0, true);
    else
      method.call(view, dataOffset, element.value, true);
  }

  view.setUint8(0, key);
  return new Uint8Array(buffer);
}

window.addEventListener('load', function () {
  if (!("hid" in navigator)) {
    document.getElementById('warning').style.display = 'block';
  }

  for (const key of screensaverModeKeys) {
    const element = document.querySelector(`[data-key="${key}"]`);
    if (element)
      element.addEventListener('change', updateAutoStartJitter);
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

  device = devices[0];
  device.open().then(async () => {
    device.addEventListener('inputreport', handleInputReport);
    document.querySelectorAll('.online').forEach(element => { element.style.opacity = 1.0; });
    await readHandler();
  });
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


function updateElement(key, event) {
  var dataOffset = 4;
  var element = document.querySelector(`[data-key="${key}"]`);

  if (!element)
    return;

  const methods = {
    "uint32": event.data.getUint32,
    "uint64": event.data.getUint32, /* Yes, I know. :-| */
    "int32": event.data.getInt32,
    "uint16": event.data.getUint16,
    "uint8": event.data.getUint8,
    "int16": event.data.getInt16,
    "int8": event.data.getInt8
  };

  dataType = element.getAttribute('data-type');

  if (dataType in methods) {
    var value = methods[dataType].call(event.data, dataOffset, true);
    setValue(element, value);

    if (element.hasAttribute('data-hex'))
      setValue(element, parseInt(value).toString(16));

    if (element.hasAttribute('data-fw-ver')) {
      /* u16 version = major * 1000 + minor + 100; */
      const major = Math.floor((value - 100) / 1000);
      const minor = (value - 100) % 1000;
      setValue(element, `v${major}.${minor}`);
    }
  }
}

async function readHandler() {
  if (!device || !device.opened)
    await connectHandler();

  await sendReport(packetType.getValAllMsg);
}

async function handleInputReport(event) {
  const data = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
  if (event.reportId !== mgmtReportId || data.length !== configReportLength
      || data[0] !== 0xaa || data[1] !== 0x55 || data[2] !== packetType.getValMsg
      || data[data.length - 1] !== calcChecksum(data))
    return;
  updateElement(data[3], event);
}

async function rebootHandler() {
  await sendReport(packetType.rebootMsg);
}

async function enterBootloaderHandler() {
  await sendReport(packetType.firmwareUpgradeMsg, true, true);
}

async function valueChangedHandler(element) {
  var key = element.getAttribute('data-key');
  var dataType = element.getAttribute('data-type');

  var origValue = element.getAttribute('fetched-value');
  var newValue = getValue(element);

  if (origValue != newValue) {
    uintBuffer = packValue(element, key, dataType);

    /* Send to both devices */
    await sendReport(packetType.setValMsg, uintBuffer, true);

    /* Set this as the current value */
    element.setAttribute('fetched-value', newValue);
  }

  if (screensaverModeKeys.includes(Number(key)))
    updateAutoStartJitter();
}

async function saveHandler() {
  const elements = document.querySelectorAll('.api');

  if (!device || !device.opened)
    return;

  for (const element of elements) {
    var origValue = element.getAttribute('fetched-value')

    if (element.hasAttribute('readonly'))
      continue;

    if (origValue != getValue(element))
      await valueChangedHandler(element);
  }
  await sendReport(packetType.saveConfigMsg, [], true);
}

async function wipeConfigHandler() {
  await sendReport(packetType.wipeConfigMsg, [], true);
}
