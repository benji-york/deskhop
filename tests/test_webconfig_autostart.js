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
