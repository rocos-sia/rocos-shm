// ABI v2 display semantics. Preserve 64-bit protocol integers as decimal strings.
export const escapeHtml = value => String(value ?? '—').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
export const hex = (value, width = 8) => value == null ? '未知' : `0x${Number(value).toString(16).toUpperCase().padStart(width, '0')}`;
const val = value => value == null ? '未知' : String(value);
const valid = (mask, bit, value, format = val) => mask & (1 << bit) ? format(value) : `未知（原始值 ${val(value)}）`;
const named = (value, names) => `${names[value] ?? '未知'} (${val(value)})`;
export const alName = value => named(value, { 0: '未知', 1: 'INIT', 2: 'PREOP', 3: 'BOOT', 4: 'SAFEOP', 8: 'OP' });
export const commandStatus = value => named(value, { 0: '空闲', 1: '等待处理', 2: '处理中', 3: '成功', 4: '确认失败', 5: '结果未知' });
export const resultName = value => named(value, { 0: '成功', 1: '参数无效', 2: '不支持', 3: '忙', 4: '实例失效', 5: '从站离线', 6: '状态不匹配', 7: '缓冲区不足', 8: '提交前已过期', 9: '后端错误', 10: '结果未知' });
export function slaveDetails(s, includePdo = false) {
  const rows = [
    ['数组位置 / position', val(s.position)], ['从站 ID / id', val(s.id)], ['名称 / name', s.name],
    ['Vendor ID / vendor_id', valid(s.info_valid, 0, s.vendor_id, hex)],
    ['Product ID / product_code', valid(s.info_valid, 1, s.product_code, hex)],
    ['修订 / revision_number', valid(s.info_valid, 2, s.revision_number, hex)],
    ['序列号 / serial_number', valid(s.info_valid, 3, s.serial_number, hex)],
    ['固件 / firmware_version', valid(s.info_valid, 4, s.firmware_version)],
    ['身份有效位 / info_valid', hex(s.info_valid)],
    ['身份来源 / identity_source', named(s.identity_source, { 0: '未知', 1: '在线设备', 2: '配置（未验证实物）' })],
    ['在线 / online', valid(s.runtime_valid, 0, s.online, n => n ? '在线 (1)' : '离线 (0)')],
    ['AL 状态 / al_state', valid(s.runtime_valid, 1, s.al_state, alName)],
    ['AL 状态码 / al_status_code', valid(s.runtime_valid, 2, s.al_status_code, n => hex(n, 4))],
    ['运行有效位 / runtime_valid', hex(s.runtime_valid)],
    ['TxPDO 数量 / input_var_num', val(s.input_var_num)], ['RxPDO 数量 / output_var_num', val(s.output_var_num)],
  ];
  if (includePdo) for (const [key, label] of [['input_vars', 'TxPDO'], ['output_vars', 'RxPDO']]) {
    for (const [i, v] of (s[key] || []).entries()) rows.push([`${label}[${i}] ${v.name}`,
      `${v.index}:${v.sub_index} · offset=${v.offset} · size=${v.size} B · bytes=${v.bytes || '未知'}`]);
  }
  return rows;
}
export function dcFresh(bus) {
  try {
    const age = BigInt(bus.sample_mono_ns) - BigInt(bus.dc_sample_mono_ns);
    return BigInt(bus.dc_sample_mono_ns) > 0n && age >= 0n && age <= 3000000000n;
  } catch { return false; }
}
export function busDetails(b) {
  const fresh = dcFresh(b);
  return [
    ['后端 / backend_type', named(b.backend_type, { 0: '未知 / 演示', 1: 'EC-Master', 2: 'IgH' })],
    ['主站版本 / ecm_version', b.ecm_version || '未知'],
    ['当前状态 / current_state', alName(b.current_state)], ['请求状态 / request_state', alName(b.request_state)],
    ['下一状态 / next_expected_state', alName(b.next_expected_state)],
    ['状态控制 / state_control_supported', val(b.state_control_supported)],
    ['命令能力位 / supported_cmds', hex(b.supported_cmds)],
    ['心跳 / heartbeat_mono_ns', val(b.heartbeat_mono_ns)],
    ['在线从站 / connected_slave_num', valid(b.status_valid, 0, b.connected_slave_num)],
    ['链路 / link_up', valid(b.status_valid, 1, b.link_up, n => n ? '连接 (1)' : '断开 (0)')],
    ['WKC / wc_state', valid(b.status_valid, 2, b.wc_state, n => named(n, { 0: '未知', 1: '正常', 2: '异常' }))],
    ['状态有效位 / status_valid', hex(b.status_valid)],
    ['DC 同步 / dc_sync_status', `${fresh ? '' : '过期 / 未采样 · '}${named(b.dc_sync_status, { 0: '未启用', 1: '同步中', 2: '已同步', 3: '失锁', 4: '未知' })}`],
    ['DC 指标 / dc_metric_kind', named(b.dc_metric_kind, { 0: '无效', 1: 'DCM 有符号控制误差', 2: 'IgH 非负总线偏差' })],
    ['DC 偏差 / dc_diff_ns', `${fresh && [1, 2].includes(b.dc_metric_kind) ? '' : '未知 / 过期 · 原始值 '}${val(b.dc_diff_ns)} ns`],
    ['DC 采样 / dc_sample_mono_ns', val(b.dc_sample_mono_ns)],
    ['统计复位标记 / resetCycleTime', val(b.resetCycleTime)],
    ['邮箱状态 / cmd_box.status', commandStatus(b.cmd_box?.status)],
  ];
}
export function rowsHtml(rows) {
  return rows.map(([label, value]) => `<div><dt>${escapeHtml(label)}</dt><dd>${escapeHtml(value)}</dd></div>`).join('');
}
export function commandDetails(c) {
  if (!c) return '尚无本 Web 提交的命令。旁观者只读取邮箱 status，不读取无所有权的载荷。';
  return `${commandStatus(c.status)} · ${resultName(c.result_code)}\n命令 ID ${c.cmd_id} · 实例 ${c.instance_id}\n类型 ${c.cmd_type} · 从站数组位置 ${c.target_slave} · ${hex(c.sdo_index, 4)}:${c.sdo_subindex}\n截止时间 ${c.deadline_mono_ns} ns\n数据 (${c.data_size} B) ${c.data || '—'}\n后端错误 ${valid(c.error_valid, 0, c.backend_error, hex)} · SDO abort ${valid(c.error_valid, 1, c.sdo_abort_code, hex)} · 有效位 ${hex(c.error_valid)}`;
}
