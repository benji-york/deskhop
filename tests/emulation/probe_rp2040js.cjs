#!/usr/bin/env node
/* An executable capability probe, not a DeskHop firmware emulation test.
 * Pass the directory of an unpacked rp2040js npm package. No npm install needed.
 */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const packageDir = path.resolve(process.argv[2] || 'node_modules/rp2040js');
const { RP2040 } = require(path.join(packageDir, 'dist/cjs/index.js'));
const { SimulationClock } = require(path.join(packageDir, 'dist/cjs/clock/simulation-clock.js'));
const version = JSON.parse(fs.readFileSync(path.join(packageDir, 'package.json'))).version;
const rp = new RP2040();
const warnings = [];
rp.logger = { debug() {}, info() {}, warn(...args) { warnings.push(args.join(' ')); }, error(...args) { warnings.push(args.join(' ')); } };
// Real ARMv6-M instructions: movs r0,#42; adds r0,#1; bkpt #0.
rp.sram.set([0x2a, 0x20, 0x01, 0x30, 0x00, 0xbe]);
rp.core.PC = 0x20000000;
rp.core.executeInstruction();
rp.core.executeInstruction();
assert.equal(rp.core.registers[0], 43, 'ARM instructions execute');
// RP2040 multicore launch relies on SIO FIFO_ST (VLD/RDY), FIFO_WR and FIFO_RD.
const originalWarn = console.warn;
console.warn = (...args) => warnings.push(args.join(' '));
const fifoBefore = rp.readUint32(0xd0000050);
rp.writeUint32(0xd0000054, 0x12345678);
const fifoAfter = rp.readUint32(0xd0000050);
const fifoRead = rp.readUint32(0xd0000058);
console.warn = originalWarn;
const cores = Object.entries(rp).filter(([, v]) => v && v.constructor?.name === 'CortexM0Core').map(([key]) => key);
const report = {
  package: 'rp2040js', version, armInstructions: 2, r0: rp.core.registers[0],
  coreInstances: cores, cpuid: rp.readUint32(0xd0000000),
  multicoreFifo: { before: fifoBefore, after: fifoAfter, read: fifoRead },
  pioBlocks: rp.pio.length, uartBlocks: rp.uart.length,
  usbControllerPresent: !!rp.usbCtrl,
  completeDeskHopEligible: cores.length === 2 && fifoBefore !== 0xffffffff,
  warnings,
};
// A second, positive feasibility experiment: two emulator instances exchange
// bytes through their UART-register models, both driven by ARM instructions.
const clock = new SimulationClock();
const boards = [new RP2040(clock), new RP2040(clock)];
const linkTrace = [];
const wireBaud = 3686400;
const wireByteNs = Math.ceil(10 * 1e9 / wireBaud);
const propagationNs = 1000; // Chosen model parameter, not measured isolator delay.
function installUartProgram(board, byte) {
  // ldr r1,[pc,#12]; movs r0,#byte; str r0,[r1]; ldr r2,[r1]; bkpt #0
  // Literal at SRAM+16 is UART0_DR (0x40034000).
  board.sram.set([0x03,0x49, byte,0x20, 0x08,0x60, 0x0a,0x68, 0x00,0xbe,
                 0,0,0,0,0,0, 0x00,0x40,0x03,0x40]);
  board.core.PC = 0x20000000;
  board.writeUint32(0x40034030, 0x301); // UARTEN, TXE, RXE.
  board.writeUint32(0x4003402c, 0x70); // 8-bit characters and FIFOs.
}
function step(board) {
  const cycles = board.core.executeInstruction();
  clock.tick(cycles * 8); // Emulator CPU cycle accounting at its 125 MHz default.
}
boards.forEach((board, index) => {
  installUartProgram(board, index === 0 ? 0x41 : 0x42);
  board.uart[0].onByte = byte => {
    linkTrace.push({ at_ns: clock.nanos, event: 'tx', from: index, byte });
    clock.createAlarm(() => {
      boards[1-index].uart[0].feedByte(byte);
      linkTrace.push({ at_ns: clock.nanos, event: 'rx', to: 1-index, byte });
    }).schedule(wireByteNs + propagationNs);
  };
});
for (let instruction = 0; instruction < 3; instruction++) {
  step(boards[0]);
  step(boards[1]);
}
assert.equal(boards[0].uart[0].flags & 0x10, 0x10, 'A waits for link arrival');
assert.equal(boards[1].uart[0].flags & 0x10, 0x10, 'B waits for link arrival');
clock.tick(wireByteNs + propagationNs);
step(boards[0]);
step(boards[1]);
assert.equal(boards[0].core.registers[2], 0x42, 'ARM UART read on A gets B byte');
assert.equal(boards[1].core.registers[2], 0x41, 'ARM UART read on B gets A byte');
report.pairedUart = {
  instances: 2,
  cpuCountPerInstance: boards.map(board => Object.values(board).filter(
    value => value && value.constructor?.name === 'CortexM0Core').length),
  armInstructionsPerInstance: 4,
  receivedInArmRegisterR2: boards.map(board => board.core.registers[2]),
  idealLinkBaud: wireBaud, wireByteNs, chosenPropagationNs: propagationNs,
  trace: linkTrace,
};
console.log(JSON.stringify(report, null, 2));
// Stop with success when the documented missing capability is reproduced.
// If upgraded, fail visibly so the architecture decision is reassessed.
assert.equal(cores.length, 1, 'RP2040js changed: reassess multicore support');
assert.equal(fifoBefore, 0xffffffff, 'RP2040js changed: reassess FIFO support');
assert.equal(fifoRead, 0xffffffff, 'RP2040js changed: reassess FIFO support');
