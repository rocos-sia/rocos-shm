import { commandDetails, commandStatus } from './diagnostics.mjs';

export function parseUnsignedInput(text, max) {
  const source = String(text).trim();
  if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(source)) throw new Error('请输入十进制或 0x 十六进制整数');
  const n = Number(source);
  if (!Number.isSafeInteger(n) || n < 0 || n > max) throw new Error(`数值范围为 0–${max}`);
  return n;
}
export function sdoBody(type, position, index, sub, size, data) {
  const t = parseUnsignedInput(type, 2);
  if (t !== 1 && t !== 2) throw new Error('请选择 SDO 读取或写入');
  const p = parseUnsignedInput(position, 49);
  const i = parseUnsignedInput(index, 65535);
  const s = parseUnsignedInput(sub, 255);
  const n = parseUnsignedInput(size, 64);
  if (!i || !n) throw new Error('索引和长度不能为零');
  const bytes = t === 2 ? data.replace(/\s/g, '') : '-';
  if (t === 2 && (!/^[0-9a-f]+$/i.test(bytes) || bytes.length !== n * 2)) throw new Error('发送数据须为指定长度的十六进制字节（按传输顺序）');
  return [t, p, i, s, n, bytes].join(' ');
}

export function createControls({ $, onChange }) {
  let snapshot = { available: false };
  let session = '';
  let savedInstance = '';
  let ownerId = '';
  let busy = false;
  try { [session, savedInstance, ownerId] = JSON.parse(sessionStorage.getItem('rocos-shm-writer') || '["",""]'); } catch {}
  const save = () => { try { sessionStorage.setItem('rocos-shm-writer', JSON.stringify([session, savedInstance, ownerId])); } catch {} };
  const owns = () => Boolean(session && snapshot.available && snapshot.read_only === false && snapshot.write_owner === ownerId && snapshot.header?.instance_id === savedInstance);
  const writable = () => owns() && !busy && !snapshot.command_pending && !snapshot.recovery_required && snapshot.cmd_box?.status === 0;
  const headers = () => ({ 'Content-Type': 'text/plain', 'X-Rocos-Write': '1', 'X-Rocos-Session': session, 'X-Rocos-Instance': snapshot.header?.instance_id || '' });
  async function post(path, body) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 5000);
    try {
      const response = await fetch(path, { method: 'POST', headers: headers(), body, signal: controller.signal });
      const reply = await response.json();
      if (!response.ok) throw new Error(reply.error || '操作被拒绝');
      return reply;
    } catch (error) {
      if (error.name === 'AbortError' || error instanceof TypeError) throw new Error('网络中断或超时，结果未确认；请查看命令结果与实时值，不要盲目重发。');
      throw error;
    } finally { clearTimeout(timer); }
  }
  function render() {
    const owner = owns();
    const enabled = writable();
    $('write-mode').textContent = owner ? '切回只读' : '启用写入';
    $('write-mode').disabled = busy || !snapshot.available || (!owner && (snapshot.read_only === false || snapshot.recovery_required || snapshot.command_pending));
    $('write-mode').setAttribute('aria-pressed', String(owner));
    $('sole-writer').disabled = owner || busy;
    $('access-status').textContent = snapshot.recovery_required ? '结果未确认：写入已锁定，需协调停止两端后重启'
      : owner ? (snapshot.command_pending ? '可写 · 命令在途，等待最终结果' : '可写 · 当前页面持有写权限')
      : snapshot.read_only === false ? '只读 · 另一页面持有写权限' : '只读 · 写入未启用';
    const stateEnabled = enabled && Boolean(snapshot.state_control_supported);
    $('state-submit').disabled = !stateEnabled;
    $('state-target').disabled = !stateEnabled;
    $('state-support').textContent = snapshot.state_control_supported ? '提交状态请求后，以当前状态为准。' : '当前后端不支持主站状态控制。';
    const type = Number($('sdo-type').value);
    const supported = Boolean((snapshot.supported_cmds || 0) & (1 << type));
    $('sdo-submit').disabled = !enabled || !supported || !snapshot.slaves?.length;
    $('sdo-support').textContent = supported ? '长度 1–64 B；字节按传输顺序填写。后端检查设备身份及 AL 状态。' : '当前后端不支持所选 SDO 命令。';
    $('sdo-data').disabled = type !== 2 || !enabled;
    $('reset-stats').disabled = !enabled || !((snapshot.supported_cmds || 0) & 8);
    $('command-pending').textContent = snapshot.command_pending ? '命令在途 / 结果未确认；保持原事务等待，禁止自动重发。' : `邮箱：${commandStatus(snapshot.cmd_box?.status)}`;
    $('command-result').textContent = commandDetails(snapshot.last_command);
    $('command-request').textContent = snapshot.last_request ? commandDetails(snapshot.last_request) : '尚无提交记录';
    onChange(enabled);
  }
  async function run(action) {
    if (busy) return;
    busy = true; render();
    try { await action(); }
    catch (error) { $('control-result').textContent = error.message; }
    finally { busy = false; render(); }
  }
  $('write-mode').addEventListener('click', () => run(async () => {
    if (owns()) {
      await post('/api/mode', 'disable');
      session = ''; savedInstance = ''; ownerId = ''; save();
      snapshot = { ...snapshot, read_only: true };
      $('control-result').textContent = '已切回只读；已提交的命令仍由后台收取最终结果。';
    } else {
      if (!$('sole-writer').checked && !snapshot.demo) throw new Error('请先停止其他可写 App，并勾选唯一写入者确认。');
      const reply = await post('/api/mode', 'enable sole-writer');
      session = reply.session; savedInstance = snapshot.header.instance_id; ownerId = reply.owner; save();
      snapshot = { ...snapshot, read_only: false, write_owner: ownerId };
      $('control-result').textContent = '已启用写入。';
    }
  }));
  $('state-form').addEventListener('submit', event => {
    event.preventDefault();
    if (!writable()) return;
    run(async () => {
      await post('/api/state', $('state-target').value);
      $('control-result').textContent = '已提交主站状态请求；请观察当前状态，不代表切换已完成。';
    });
  });
  async function command(body) {
    const reply = await post('/api/command', body);
    snapshot = { ...snapshot, command_pending: ![3, 4, 5].includes(reply.command.status), last_command: reply.command };
    $('control-result').textContent = '命令已提交；最终状态、返回字节及错误码显示在下方。';
  }
  $('sdo-form').addEventListener('submit', event => {
    event.preventDefault();
    if (!writable()) return;
    run(() => command(sdoBody($('sdo-type').value, $('sdo-slave').value, $('sdo-index').value, $('sdo-sub').value, $('sdo-size').value, $('sdo-data').value)));
  });
  $('sdo-type').addEventListener('change', render);
  $('reset-stats').addEventListener('click', () => { if (writable()) run(() => command('3 0 0 0 0 -')); });
  return {
    headers, writable,
    update(bus) {
      snapshot = bus;
      if (bus.available && savedInstance && savedInstance !== bus.header?.instance_id) { session = ''; savedInstance = ''; ownerId = ''; save(); }
      const select = $('sdo-slave');
      const schema = JSON.stringify((bus.slaves || []).map(s => [s.position, s.id, s.name]));
      if (select.dataset.schema !== schema) {
        const selected = select.value;
        select.replaceChildren(...(bus.slaves || []).map(s => {
          const option = document.createElement('option');
          option.value = s.position;
          option.textContent = `#${s.id} ${s.name}（位置 ${s.position}）`;
          return option;
        }));
        if ([...select.options].some(o => o.value === selected)) select.value = selected;
        select.dataset.schema = schema;
      }
      render();
    },
    disconnected() { snapshot = { ...snapshot, available: false }; render(); },
  };
}
