import assert from 'node:assert/strict';
import test from 'node:test';
import { masterStatus, slaveStatus, topologyModel } from './topology.mjs';

const bus = { available: true, master_id: 123456, current_state: 8, status_valid: 7,
  link_up: 1, wc_state: 1, connected_slave_num: 2, slave_num: 2,
  slaves: [
    { id: 2, name: 'Axis X', online: 1, al_state: 8, runtime_valid: 3 },
    { id: 9, name: 'IO', online: 1, al_state: 4, runtime_valid: 3 },
  ] };

test('master diagnostics respect each validity bit and alarm precedence', () => {
  assert.equal(masterStatus(bus).tone, 'ok');
  assert.equal(masterStatus({ ...bus, link_up: 0 }).label, '链路断开');
  assert.equal(masterStatus({ ...bus, wc_state: 2 }).label, 'WKC 异常');
  assert.equal(masterStatus({ ...bus, connected_slave_num: 1 }).tone, 'warn');
  assert.equal(masterStatus({ ...bus, status_valid: 0, wc_state: 2, link_up: 0, connected_slave_num: 0 }).tone, 'ok');
  assert.equal(masterStatus({ ...bus, current_state: 0 }).tone, 'muted');
  assert.equal(masterStatus({ ...bus, available: false }).label, '未连接');
});

test('slave colors distinguish OP, preparation, offline, AL error and unknown', () => {
  const slave = bus.slaves[0];
  for (const al_state of [1, 2, 3, 4]) assert.equal(slaveStatus({ ...slave, al_state }).tone, 'warn');
  assert.equal(slaveStatus(slave).tone, 'ok');
  assert.equal(slaveStatus({ ...slave, online: 0 }).label, '离线');
  assert.equal(slaveStatus({ ...slave, al_state: 0x14 }).tone, 'bad');
  assert.equal(slaveStatus({ ...slave, al_state: 0x14, runtime_valid: 1 }).tone, 'muted');
  assert.equal(slaveStatus({ ...slave, runtime_valid: 2 }).tone, 'muted');
  assert.equal(slaveStatus({ ...slave, online: 0, runtime_valid: 0 }).label, '状态未知');
  assert.equal(slaveStatus({ ...slave, al_state: 0 }).tone, 'muted');
});

test('edges encode membership for nonconsecutive IDs, never fabricated cable order', () => {
  const graph = topologyModel(bus);
  assert.equal(graph.nodes.length, 3);
  assert.deepEqual(graph.edges.map(edge => [edge.source, edge.target]), [['master', 'slave-2'], ['master', 'slave-9']]);
  assert.deepEqual(graph.nodes.map(node => node.tone), ['ok', 'ok', 'warn']);
  const empty = topologyModel({ ...bus, slave_num: 0, slaves: [] });
  assert.equal(empty.nodes.length, 1);
  assert.equal(empty.edges.length, 0);
});

test('disconnect and unavailable mappings never retain green current states', () => {
  const stale = topologyModel(bus, true);
  assert.equal(stale.nodes.length, 3);
  assert.ok(stale.nodes.every(node => node.tone === 'muted' && node.label === '数据已过期'));
  const gone = topologyModel({ ...bus, available: false });
  assert.equal(gone.nodes.length, 1);
  assert.equal(gone.edges.length, 0);
  assert.equal(gone.nodes[0].label, '未连接');
});

test('maximum ABI capacity and long names are preserved in the accessible model', () => {
  const name = '完整设备名'.repeat(16);
  const slaves = Array.from({ length: 50 }, (_, id) => ({ ...bus.slaves[0], id, name }));
  const graph = topologyModel({ ...bus, slave_num: 50, slaves });
  assert.equal(graph.nodes.length, 51);
  assert.equal(new Set(graph.nodes.map(node => node.id)).size, 51);
  assert.equal(graph.nodes[50].name, `#49 ${name}`);
});
