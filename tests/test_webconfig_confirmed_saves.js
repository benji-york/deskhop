/* Real page handlers against an independent two-board acknowledged endpoint. */
const assert = require('node:assert/strict');
const {page, element, crc8, tick} = require('./webconfig_test_helpers');

const ops = p => p.commands.map(request => [request.op, request.role]);
const sets = p => p.commands.filter(request => request.op === 2 || request.op === 3);
const saves = p => p.commands.filter(request => request.op === 5);

async function capabilityAndSuccess() {
  const p = page();
  p.elements.get(21).value = '7200';
  p.behavior.hold = request => request.op === 1 && request.role === 1;
  const saving = p.context.saveHandler();
  await tick();
  assert.deepEqual(ops(p), [[1, 0], [1, 1]]);
  assert.equal(sets(p).length, 0, 'both capability responses precede any mutation');
  p.held.shift().release();
  assert.equal(await saving, true);
  assert.deepEqual(ops(p), [[1, 0], [1, 1], [2, 0], [2, 1], [4, 0], [4, 1], [6, 0], [6, 1], [5, 0], [5, 1]]);
  assert.equal(new Set(p.commands.map(request => request.token)).size, p.commands.length);
  assert(p.commands.every(request => request.token > 0));
  for (let index = 0; index < p.reports.length; index += 4)
    assert.deepEqual(p.reports.slice(index, index + 4).map(report => report.bytes[2]), [56, 57, 58, 59]);
  assert(p.reports.every(report => ![18, 21, 23].includes(report.bytes[2])), 'never falls back to legacy writes');
  assert.equal(p.flash[0].get(21), 7200000000n);
  assert.equal(p.flash[1].get(21), 7200000000n);
  assert.match(p.status.textContent, /A: saved to flash and verified; B: saved to flash and verified/);
  assert.match(p.status.textContent, /digests match/);
  assert.equal(p.deadlines.size, 0);

  for (const reject of [false, true]) {
    const old = page();
    old.elements.get(21).value = '2';
    old.behavior.drop = request => !reject && request.op === 1 && request.role === 1;
    old.behavior.reject = request => reject && request.op === 1 && request.role === 1 ? 1 : 0;
    const operation = old.context.saveHandler();
    await tick();
    if (!reject) {
      assert.equal([...old.deadlines.values()][0].ms, 3000);
      old.expire();
    }
    assert.equal(await operation, false);
    assert.deepEqual(ops(old), [[1, 0], [1, 1]]);
    assert.match(old.status.textContent, /No configuration writes sent/);
    assert.match(old.status.textContent, reject ? /B: rejected/ : /B: unknown/);
  }
  const future = page();
  future.behavior.ackTransform = (frames, request) => {
    if (request.op === 1) { frames[1][7] = 2; frames[1][11] = crc8([...frames[1]].slice(0, 11)); }
    return frames;
  };
  assert.equal(await future.context.saveHandler(), false);
  assert.equal(sets(future).length, 0);
  assert.match(future.status.textContent, /unsupported confirmation protocol/);
}

async function partialApplyRetry() {
  const p = page();
  const field = p.elements.get(21);
  field.value = '3';
  p.behavior.reject = request => request.op === 2 && request.role === 1 ? 1 : 0;
  assert.equal(await p.context.valueChangedHandler(field), false);
  assert.equal(p.ram[0].get(21), 3000000n);
  assert.equal(p.ram[1].get(21), 0n);
  assert.equal(field.getAttribute('fetched-value'), '0');
  assert.match(p.status.textContent, /A: applied in RAM; B: rejected/);
  assert.equal(saves(p).length, 0);
  await p.context.readHandler();
  assert.equal(field.value, '3', 'local read must retain desired unconfirmed value');
  assert.equal(field.getAttribute('fetched-value'), '0', 'local read cannot confirm peer');
  p.behavior.reject = () => 0;
  assert.equal(await p.context.saveHandler(), true);
  assert.deepEqual(sets(p).map(request => [request.role, request.value]),
                   [[0, 3000000n], [1, 3000000n], [0, 3000000n], [1, 3000000n]]);
  assert.equal(field.getAttribute('fetched-value'), '3');
  assert.equal(p.flash[1].get(21), 3000000n);

  const reverted = page();
  const restored = reverted.elements.get(21);
  restored.value = '1';
  reverted.behavior.reject = request => request.op === 2 && request.role === 1 ? 2 : 0;
  assert.equal(await reverted.context.valueChangedHandler(restored), false);
  restored.value = '0'; // Equal to old fetched value, but A already applied one second.
  reverted.behavior.reject = () => 0;
  assert.equal(await reverted.context.saveHandler(), true);
  assert.equal(reverted.flash[0].get(21), 0n);
  assert.equal(reverted.flash[1].get(21), 0n);
  assert.equal(sets(reverted).length, 4, 'retry dirty marker survives reverting to old local baseline');
}

async function persistenceResults() {
  const p = page();
  p.elements.get(21).value = '1';
  p.behavior.drop = request => request.op === 5 && request.role === 1;
  const saving = p.context.saveHandler();
  await tick(); p.expire();
  assert.equal(await saving, false);
  assert.equal(p.flash[1].get(21), 1000000n, 'lost response can follow successful physical save');
  assert.match(p.status.textContent, /A: saved to flash and verified; B: unknown/);
  assert(!/B: saved/.test(p.status.textContent));
  const before = sets(p).length;
  p.behavior.drop = () => false;
  assert.equal(await p.context.saveHandler(), true);
  assert.equal(sets(p).length, before + 2, 'RAM acknowledgements are not persistence; retry reapplies desired values');
  assert.equal(saves(p).length, 4, 'explicit Save retry uses fresh confirmation requests');

  const rebooted = page();
  const previousPeerSettings = new Map(rebooted.ram[1]);
  rebooted.elements.get(21).value = '1';
  rebooted.behavior.reject = request => request.op === 5 && request.role === 1 ? 2 : 0;
  assert.equal(await rebooted.context.saveHandler(), false);
  assert.equal(rebooted.elements.get(21).getAttribute('fetched-value'), '1', 'both RAM ACKs were received');
  rebooted.ram[1] = new Map(previousPeerSettings); // Peer reboot loses its unpersisted RAM edit.
  rebooted.behavior.reject = () => 0;
  assert.equal(await rebooted.context.saveHandler(), true);
  assert.equal(sets(rebooted).length, 4, 'retry reapplies values despite unchanged RAM-confirmed UI baseline');
  assert.equal(rebooted.flash[0].get(21), 1000000n);
  assert.equal(rebooted.flash[1].get(21), 1000000n);
  assert.match(rebooted.status.textContent, /Both settings digests match/);
  assert(!rebooted.status.textContent.includes('pre-existing differences'));

  const live = page();
  live.elements.get(21).value = '1';
  assert.equal(await live.context.valueChangedHandler(live.elements.get(21)), true);
  live.ram[1].set(21, 0n); // Same risk between a live edit and its first Save.
  assert.equal(await live.context.saveHandler(), true);
  assert.equal(live.flash[1].get(21), 1000000n);

  const beforeQuery = page();
  beforeQuery.elements.get(21).value = '1';
  beforeQuery.behavior.beforeExecute = request => {
    if (request.op === 4 && request.role === 1) beforeQuery.ram[1].set(21, 0n);
  };
  assert.equal(await beforeQuery.context.saveHandler(), false);
  assert.equal(saves(beforeQuery).length, 0, 'changed post-apply digest stops before either flash save');
  assert.match(beforeQuery.status.textContent, /B: error \(RAM_CHANGED_AFTER_APPLY/);
  assert(!beforeQuery.status.textContent.includes('pre-existing differences'));
  beforeQuery.behavior.beforeExecute = () => {};
  assert.equal(await beforeQuery.context.saveHandler(), true);
  assert.equal(beforeQuery.flash[1].get(21), 1000000n, 'digest conflict retains desired value for retry');

  const unrelatedDifference = page();
  unrelatedDifference.ram[1].set(77, 9n);
  unrelatedDifference.elements.get(21).value = '1';
  assert.equal(await unrelatedDifference.context.saveHandler(), true);
  assert.equal(unrelatedDifference.flash[1].get(21), 1000000n);
  assert.equal(unrelatedDifference.flash[1].get(77), 9n);
  assert.match(unrelatedDifference.status.textContent, /settings differ between Picos/);

  const betweenEdits = page();
  const oldPeer = new Map(betweenEdits.ram[1]);
  betweenEdits.elements.get(21).value = '1';
  betweenEdits.elements.get(22).value = '2';
  betweenEdits.behavior.beforeExecute = request => {
    if (request.op === 2 && request.role === 1 && request.key === 22)
      betweenEdits.ram[1] = new Map(oldPeer); // Reboot loses field21 before SET22's ACK digest.
  };
  assert.equal(await betweenEdits.context.saveHandler(), false);
  assert.equal(saves(betweenEdits).length, 0, 'lost earlier edit is detected even when last SET and QUERY digests match');
  assert.match(betweenEdits.status.textContent, /B: error \(CONFLICT/);
  betweenEdits.behavior.beforeExecute = () => {};
  assert.equal(await betweenEdits.context.saveHandler(), true);
  for (const role of [0, 1]) {
    assert.equal(betweenEdits.flash[role].get(21), 1000000n);
    assert.equal(betweenEdits.flash[role].get(22), 2000000n);
  }

  const duringCheck = page();
  duringCheck.elements.get(21).value = '1';
  duringCheck.behavior.beforeExecute = request => {
    if (request.op === 6 && request.role === 1) duringCheck.ram[1].set(77, 1n);
  };
  assert.equal(await duringCheck.context.saveHandler(), false);
  assert.equal(saves(duringCheck).length, 0, 'CHECK digest also binds unchanged fields to the queried snapshot');
  assert.match(duringCheck.status.textContent, /RAM_CHANGED_DURING_CHECK/);

  for (const code of [1, 2, 3, 4, 5, 6]) {
    const rejected = page();
    rejected.behavior.reject = request => request.op === 5 && request.role === 1 ? code : 0;
    assert.equal(await rejected.context.saveHandler(), false);
    assert.match(rejected.status.textContent, /A: saved to flash and verified/);
    assert.match(rejected.status.textContent, code === 5 ? /B: unknown \(EXPIRED; operation may have completed\)/
      : [3, 4].includes(code) ? /B: error/ : /B: rejected/);
  }
  const query = page();
  query.behavior.reject = request => request.op === 4 && request.role === 1 ? 2 : 0;
  assert.equal(await query.context.saveHandler(), false);
  assert.equal(saves(query).length, 0, 'failed digest snapshot prevents any flash request');

  const conflict = page();
  conflict.behavior.beforeExecute = request => {
    if (request.op === 5 && request.role === 1) conflict.ram[1].set(21, 123n);
  };
  assert.equal(await conflict.context.saveHandler(), false);
  assert.match(conflict.status.textContent, /B: error \(CONFLICT;/);
  assert.equal(conflict.flash[1].size, 0);

  const different = page();
  different.ram[1].set(77, 9n);
  assert.equal(await different.context.saveHandler(), true);
  assert.match(different.status.textContent, /settings differ between Picos/);
  assert.equal(different.flash[1].get(77), 9n, 'untouched peer divergence is not silently overwritten');

  const expired = page();
  expired.behavior.ackTransform = (frames, request) => {
    if (request.op === 5 && request.role === 1) {
      // The remote SAVE succeeded, but only the bridge's EXPIRED response arrived.
      frames[0][10] = 5;
      frames[0][11] = crc8([...frames[0]].slice(0, 11));
    }
    return frames;
  };
  assert.equal(await expired.context.saveHandler(), false);
  assert(expired.flash[1].size > 0, 'peer really committed before response loss');
  assert.match(expired.status.textContent, /B: unknown \(EXPIRED; operation may have completed\)/);
  assert(!expired.status.textContent.includes('B: rejected'));
}

async function responseFaults() {
  for (const mutation of ['token', 'role', 'op', 'key', 'status', 'crc', 'short', 'meta-only', 'value-only']) {
    const p = page();
    p.behavior.ackTransform = (frames, request) => {
      if (request.role !== 1) return frames;
      const altered = frames.map(frame => Uint8Array.from(frame));
      if (mutation === 'token') { altered[0][3] ^= 1; altered[1][3] ^= 1; }
      if (mutation === 'role') altered[0][7] = 0;
      if (mutation === 'op') altered[0][8] = 5;
      if (mutation === 'key') altered[0][9] = 83;
      if (mutation === 'status') altered[0][10] = 7;
      for (const frame of altered) frame[11] = crc8([...frame].slice(0, 11));
      if (mutation === 'crc') altered[0][11] ^= 1;
      if (mutation === 'short') return [altered[0].slice(0, 11), altered[1]];
      if (mutation === 'meta-only') return [altered[0]];
      if (mutation === 'value-only') return [altered[1]];
      return altered;
    };
    const saving = p.context.saveHandler();
    await tick(); p.expire();
    assert.equal(await saving, false, mutation);
    assert.equal(sets(p).length, 0, mutation + ' cannot grant capability');
  }
  const reversed = page();
  reversed.behavior.reverse = true;
  assert.equal(await reversed.context.saveHandler(), true, 'ACK value may arrive before ACK metadata');

  const p = page();
  p.behavior.hold = request => request.op === 1 && request.role === 1;
  const old = p.context.saveHandler();
  await tick();
  const stale = p.held.shift();
  p.expire(); assert.equal(await old, false);
  const retry = p.context.saveHandler();
  await tick();
  const fresh = p.held.shift();
  assert.notEqual(fresh.request.token, stale.request.token);
  stale.release(); await tick();
  assert.equal(saves(p).length, 0, 'late ACK cannot satisfy the new token');
  fresh.release(); assert.equal(await retry, true);

  const mismatch = page();
  mismatch.behavior.ackTransform = (frames, request) => {
    if (request.op === 5 && request.role === 1) {
      frames[1][7] ^= 1;
      frames[1][11] = crc8([...frames[1]].slice(0, 11));
    }
    return frames;
  };
  assert.equal(await mismatch.context.saveHandler(), false);
  assert.match(mismatch.status.textContent, /saved digest did not match/);
}

async function serializationAndDisconnect() {
  const p = page();
  const first = p.elements.get(21), second = p.elements.get(22);
  first.value = '1'; second.value = '2';
  p.behavior.hold = request => request.op === 2 && request.role === 0;
  const one = p.context.valueChangedHandler(first);
  const two = p.context.valueChangedHandler(second);
  await tick();
  assert.deepEqual(ops(p), [[1, 0], [1, 1], [2, 0]]);
  p.held.shift().release();
  assert.equal(await one, true);
  await tick();
  assert.deepEqual(sets(p).map(request => request.key), [21, 21, 22]);
  p.held.shift().release();
  assert.equal(await two, true);

  const gone = page();
  gone.elements.get(21).value = '1';
  gone.behavior.hold = request => request.op === 2;
  const changing = gone.context.valueChangedHandler(gone.elements.get(21));
  const queued = gone.context.saveHandler();
  await tick();
  const reportCount = gone.reports.length;
  gone.disconnect();
  assert.equal(await changing, false);
  assert.equal(await queued, false);
  assert.equal(gone.reports.length, reportCount, 'disconnect cancels outstanding and queued writes');
  gone.held.shift().release();
  assert.equal(gone.elements.get(21).getAttribute('fetched-value'), '0');
  assert.equal(gone.deadlines.size, 0);

  const failed = page();
  failed.elements.get(21).value = '1';
  failed.behavior.sendFailure = bytes => bytes[2] === 58;
  assert.equal(await failed.context.valueChangedHandler(failed.elements.get(21)), false);
  assert.equal(sets(failed).length, 0);
  assert.match(failed.status.textContent, /HID send failure/);

  const stalled = page();
  const originalSend = stalled.context.device.sendReport;
  let releaseSend;
  let intercepted = false;
  stalled.context.device.sendReport = async function (id, bytes) {
    if (!intercepted && bytes[2] === 57) {
      intercepted = true;
      await new Promise(resolve => { releaseSend = resolve; });
    }
    return originalSend.call(this, id, bytes);
  };
  const blocked = stalled.context.saveHandler();
  await tick(); stalled.expire();
  assert.equal(await blocked, false, 'ACK deadline bounds a blocked HID send, not only reply waiting');
  const finishedCount = stalled.commands.length;
  releaseSend(); await tick();
  assert.equal(stalled.commands.length, finishedCount, 'late send completion cannot continue the expired staged operation');
  assert.equal(saves(stalled).length, 0);

  const maintenance = page();
  maintenance.elements.get(21).value = '1';
  maintenance.behavior.hold = request => request.op === 2;
  const applying = maintenance.context.valueChangedHandler(maintenance.elements.get(21));
  const laterSave = maintenance.context.saveHandler();
  await tick();
  await maintenance.context.rebootHandler();
  assert.equal(await applying, false);
  assert.equal(await laterSave, false);
  assert.equal(sets(maintenance).length, 1, 'maintenance prevents the remaining peer apply and queued Save');
  assert.equal(saves(maintenance).length, 0);

  const unavailable = page();
  const desired = unavailable.elements.get(21);
  desired.value = '1';
  unavailable.behavior.reject = request => request.op === 1 && request.role === 1 ? 2 : 0;
  assert.equal(await unavailable.context.saveHandler(), false);
  await unavailable.context.readHandler();
  assert.equal(desired.value, '1', 'capability failure must not discard retry intent on Read');
  unavailable.disconnect();
  unavailable.context.device.opened = true;
  unavailable.read(21, 0n); // Automatic local read on reconnect.
  assert.equal(desired.value, '1', 'reconnect read retains edits staged before capabilities');
  unavailable.behavior.reject = () => 0;
  assert.equal(await unavailable.context.saveHandler(), true);
  assert.equal(unavailable.flash[0].get(21), 1000000n);
  assert.equal(unavailable.flash[1].get(21), 1000000n);

  const restoredLarge = page();
  const largeValue = (1n << 56n) - 1n;
  restoredLarge.read(21, largeValue);
  const largeField = restoredLarge.elements.get(21);
  const originalDisplay = largeField.value;
  largeField.value = '1';
  restoredLarge.behavior.reject = request => request.op === 1 && request.role === 1 ? 2 : 0;
  assert.equal(await restoredLarge.context.saveHandler(), false);
  largeField.value = originalDisplay;
  restoredLarge.behavior.reject = () => 0;
  assert.equal(await restoredLarge.context.saveHandler(), true);
  assert.equal(sets(restoredLarge).length, 0, 'reverting provably unsent intent preserves an unwritable legacy timer');
  assert.equal(restoredLarge.flash[0].get(21), largeValue);
  assert.equal(restoredLarge.flash[1].get(21), largeValue);
}

async function atomicBordersAndPreflight() {
  for (const base of [10, 40]) {
    const p = page([element(base + 4, 'int32', null), element(base + 5, 'int32', null)]);
    p.read(base + 4, 0n); p.read(base + 5, 100n);
    p.ram[1].set(base + 4, 600n); p.ram[1].set(base + 5, 700n);
    p.elements.get(base + 4).value = '200'; p.elements.get(base + 5).value = '300';
    assert.equal(await p.context.valueChangedHandler(p.elements.get(base + 4)), true);
    assert.equal(p.reports.length, 0, 'individual border changes remain deferred');
    assert.equal(await p.context.saveHandler(), true);
    assert.deepEqual(sets(p).map(request => [request.op, request.key, request.lo, request.hi]),
                     [[3, base === 10 ? 0 : 1, 200n, 300n], [3, base === 10 ? 0 : 1, 200n, 300n]]);
    assert.equal(p.flash[1].get(base + 4), 200n);
    assert.equal(p.flash[1].get(base + 5), 300n);
    assert.deepEqual(p.commands.filter(request => request.op === 6).map(request => [request.role, request.key, request.value]),
                     [[0, base + 4, 200n], [1, base + 4, 200n], [0, base + 5, 300n], [1, base + 5, 300n]],
                     'atomic border pair expands to checks of both intended fields on each board');
  }
  const invalid = page([element(21), element(14, 'int32', null), element(15, 'int32', null)]);
  invalid.elements.get(21).value = '2';
  invalid.elements.get(14).value = '100'; invalid.elements.get(15).value = '100';
  assert.equal(await invalid.context.saveHandler(), false);
  assert.equal(invalid.reports.length, 0, 'all validation precedes even capability traffic');
}

async function main() {
  await capabilityAndSuccess(); await partialApplyRetry(); await persistenceResults();
  await responseFaults(); await serializationAndDisconnect(); await atomicBordersAndPreflight();
  console.log('webconfig confirmed saves passed (both capabilities, strict ACKs, partial apply/save, retry, races, atomic borders)');
}
let timeout;
Promise.race([main(), new Promise((_, reject) => {
  timeout = setTimeout(() => reject(new Error('confirmed-save tests did not settle')), 5000);
})]).catch(error => { console.error(error); process.exitCode = 1; }).finally(() => clearTimeout(timeout));
