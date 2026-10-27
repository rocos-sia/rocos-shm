import assert from 'node:assert/strict';
import test from 'node:test';
import { busDetails, slaveDetails, rowsHtml, dcFresh, commandDetails } from './diagnostics.mjs';
import { parseUnsignedInput, sdoBody } from './controls.mjs';

test('identity, firmware and runtime fields honor independent validity bits', () => {
  const s = { id: 17, position: 0, name: '<img src=x>', vendor_id: 0x1234, product_code: 0x5678,
    firmware_version: 'fw-1', online: 0, al_status_code: 0x1234, info_valid: 1, runtime_valid: 4,
    identity_source: 2, input_vars: [{ name: 'Status', index: '6041', sub_index: 0, offset: 0, size: 2, bytes: '2700' }] };
  const text = slaveDetails(s, true).map(row => row.join(': ')).join('\n');
  assert.match(text, /Vendor ID.*0x00001234/);
  assert.match(text, /Product ID.*未知/);
  assert.match(text, /firmware_version.*未知/);
  assert.match(text, /online.*未知/);
  assert.match(text, /al_status_code.*0x1234/);
  assert.match(text, /配置（未验证实物）/);
  assert.match(text, /TxPDO\[0\].*offset=0.*bytes=2700/);
  assert.doesNotMatch(rowsHtml(slaveDetails(s)), /<img/);
  assert.match(rowsHtml(slaveDetails(s)), /&lt;img/);
});

test('64-bit DC freshness is exact and invalid diagnostics are not normal zeroes', () => {
  const b = { sample_mono_ns: '18446744073709551615', dc_sample_mono_ns: '18446744070709551615', dc_diff_ns: '-9007199254740993', dc_metric_kind: 1, status_valid: 0, link_up: 0, wc_state: 1 };
  assert.equal(dcFresh(b), true);
  assert.equal(dcFresh({ ...b, dc_sample_mono_ns: '18446744070709551614' }), false);
  assert.equal(dcFresh({ ...b, dc_sample_mono_ns: '0' }), false);
  const text = busDetails(b).map(row => row.join(': ')).join('\n');
  assert.match(text, /-9007199254740993 ns/);
  assert.match(text, /WKC.*未知/);
  assert.match(text, /link_up.*未知/);
});

test('mailbox result distinguishes unknown outcomes and invalid error codes', () => {
  assert.match(commandDetails(null), /只读取邮箱 status/);
  const text = commandDetails({ status: 5, result_code: 10, error_valid: 2, backend_error: 123, sdo_abort_code: 0x06020000 });
  assert.match(text, /结果未知/);
  assert.match(text, /后端错误 未知/);
  assert.match(text, /SDO abort 0x06020000/);
});

test('SDO wire encoding uses array position, bounded capacity and strict raw bytes', () => {
  assert.equal(sdoBody('1', '0', '0x1000', '0', '64', ''), '1 0 4096 0 64 -');
  assert.equal(sdoBody('2', '0', '0x607A', '0', '4', '78 56 34 12'), '2 0 24698 0 4 78563412');
  for (const args of [ ['2','0','0x607A','0','4','ff'], ['1','50','4096','0','4',''], ['1','0','4096','0','65',''], ['1','0','4096','256','4',''] ]) assert.throws(() => sdoBody(...args));
  for (const input of ['1e2', '-1', '1.5', '0x', 'NaN', '12tail']) assert.throws(() => parseUnsignedInput(input, 65535));
});
