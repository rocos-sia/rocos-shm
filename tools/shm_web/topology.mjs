import { slaveDetails } from './diagnostics.mjs';
// Logical membership only: ABI v2 does not expose physical port adjacency.
const AL_STATES = { 1: 'INIT', 2: 'PREOP', 3: 'BOOT', 4: 'SAFEOP', 8: 'OP' };

function alStatus(value) {
  const label = AL_STATES[value & 0x0f];
  if (value & 0x10) return { tone: 'bad', label: `${label || 'AL'} · 错误` };
  if (!label) return { tone: 'muted', label: '状态未知' };
  return { tone: value === 8 ? 'ok' : 'warn', label };
}

export function masterStatus(bus) {
  if (!bus.available) return { tone: 'muted', label: '未连接' };
  const valid = bus.status_valid || 0;
  if ((valid & 2) && !bus.link_up) return { tone: 'bad', label: '链路断开' };
  if ((valid & 4) && bus.wc_state === 2) return { tone: 'bad', label: 'WKC 异常' };
  if ((valid & 1) && bus.connected_slave_num < bus.slave_num) return { tone: 'warn', label: '从站未全部在线' };
  return alStatus(bus.current_state || 0);
}

export function slaveStatus(slave) {
  const valid = slave.runtime_valid || 0;
  if ((valid & 1) && !slave.online) return { tone: 'bad', label: '离线' };
  if (!(valid & 2)) return { tone: 'muted', label: valid & 1 ? '在线 · AL 未知' : '状态未知' };
  const state = alStatus(slave.al_state);
  if (!(valid & 1)) return { tone: state.tone === 'bad' ? 'bad' : 'muted', label: `${state.label} · 在线状态未知` };
  return state;
}

export function topologyModel(bus, stale = false) {
  const state = stale ? { tone: 'muted', label: '数据已过期' } : masterStatus(bus);
  const master = { id: 'master', name: `主站 ${bus.master_id ?? '—'}`, role: '主站', ...state };
  const slaves = bus.available ? (bus.slaves || []).map(slave => ({
    id: `slave-${slave.id}`, slaveId: slave.id, name: `#${slave.id} ${slave.name}`,
    role: '从站', ...(stale ? { tone: 'muted', label: '数据已过期' } : slaveStatus(slave)),
  })) : [];
  return { nodes: [master, ...slaves], edges: slaves.map(node => ({ id: `edge-${node.id}`, source: master.id, target: node.id })) };
}

export function createTopologyView({ container, picker, detail, notice, onSelect }) {
  let cy = null;
  let snapshot = { available: false };
  let stale = false;
  let schema = '';
  let model = topologyModel(snapshot);
  const tooltip = document.createElement('div');
  tooltip.className = 'topology-tooltip';
  tooltip.hidden = true;
  tooltip.setAttribute('role', 'tooltip');
  document.body.append(tooltip);
  let hoverId = null;
  let hideTimer;
  function hideTooltip() { hoverId = null; tooltip.hidden = true; }
  function tooltipText(id) {
    const slave = snapshot.slaves?.find(s => `slave-${s.id}` === id);
    return slave ? `${stale ? '数据已过期\n' : ''}${slaveDetails(slave, true).map(([k, v]) => `${k}: ${v}`).join('\n')}` : '';
  }
  function showTooltip(id, point) {
    clearTimeout(hideTimer);
    hoverId = id;
    tooltip.textContent = tooltipText(id);
    tooltip.hidden = !tooltip.textContent;
    if (point) {
      const rect = container.getBoundingClientRect();
      tooltip.style.left = `${Math.max(8, Math.min(rect.left + point.x + 12, window.innerWidth - tooltip.offsetWidth - 8))}px`;
      tooltip.style.top = `${Math.max(8, Math.min(rect.top + point.y + 12, window.innerHeight - tooltip.offsetHeight - 8))}px`;
    }
  }
  tooltip.addEventListener('mouseenter', () => clearTimeout(hideTimer));
  tooltip.addEventListener('mouseleave', hideTooltip);
  document.addEventListener('keydown', event => { if (event.key === 'Escape') hideTooltip(); });
  const text = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const nodeLabel = node => `${Array.from(node.name).slice(0, 26).join('')}${Array.from(node.name).length > 26 ? '…' : ''}\n${node.role} · ${node.label}`;
  const narrow = () => container.clientWidth < 600;
  function styles() {
    return [
      { selector: 'node', style: {
        shape: 'round-rectangle', width: 224, height: 76,
        'background-color': text('--panel'), 'border-width': 2,
        'border-color': text('--muted'), color: text('--text'),
        label: 'data(label)', 'font-family': 'AlibabaPuHuiTi', 'font-size': 14, 'font-weight': 500,
        'text-valign': 'center', 'text-halign': 'center', 'text-wrap': 'wrap', 'text-max-width': 204,
        'text-overflow-wrap': 'anywhere', 'line-height': 1.5,
      } },
      ...['ok', 'warn', 'bad', 'muted'].map(tone => ({ selector: `node[tone = "${tone}"]`, style: { 'border-color': text(`--${tone}`) } })),
      { selector: '#master', style: { 'border-width': 4, 'font-weight': 600 } },
      { selector: 'node:selected', style: { 'background-color': text('--panel-deep'), 'overlay-opacity': 0 } },
      { selector: 'edge', style: { width: 1.5, 'line-color': text('--border-strong'), 'line-style': 'dashed', 'curve-style': 'taxi', 'taxi-direction': 'downward', 'taxi-turn': 40, 'taxi-turn-min-distance': 12 } },
    ];
  }
  function showSelection(id) {
    const node = model.nodes.find(item => item.id === id) || model.nodes[0];
    picker.value = node.id;
    detail.textContent = `${node.name} · ${node.role} · ${node.label}\n${tooltipText(node.id)}`;
    onSelect(node.slaveId, stale || !snapshot.available);
  }
  function fit() {
    if (!cy || !cy.nodes().length) return;
    cy.resize();
    cy.fit(cy.elements(), 32);
    if (cy.zoom() > 1) { cy.zoom(1); cy.center(); }
  }
  function render() {
    model = topologyModel(snapshot, stale);
    if (hoverId) showTooltip(hoverId);
    const selected = picker.value || 'master';
    const nextSchema = JSON.stringify([narrow(), snapshot.master_id, model.nodes.map(node => [node.id, node.name])]);
    if (schema !== nextSchema) {
      schema = nextSchema;
      picker.replaceChildren(...model.nodes.map(node => {
        const option = document.createElement('option');
        option.value = node.id;
        return option;
      }));
      if (cy) {
        // A membership fan, never a fabricated slave-to-slave cable chain.
        const rows = Math.max(1, model.nodes.length - 1);
        cy.style(styles());
        cy.elements().remove();
        cy.add([
          ...model.nodes.map((node, i) => ({ data: { ...node, label: nodeLabel(node) },
            position: narrow() ? { x: 0, y: i * 140 }
              : i === 0 ? { x: (Math.min(rows, 5) - 1) * 250 / 2, y: 0 } : { x: (i - 1) * 250, y: 140 } })),
          ...model.edges.map(data => ({ data })),
        ]);
        // Keep labels readable for large buses; Fit can show the complete graph.
        if (rows <= (narrow() ? 1 : 5)) fit();
        else { cy.zoom(1); cy.center(cy.nodes().slice(0, narrow() ? 2 : 5)); }
      }
    }
    if (cy) cy.batch(() => {
      for (const node of model.nodes) {
        const element = cy.getElementById(node.id);
        const label = nodeLabel(node);
        if (element.data('label') !== label || element.data('tone') !== node.tone) element.data({ label, tone: node.tone });
      }
    });
    for (const option of picker.options) {
      const node = model.nodes.find(item => item.id === option.value);
      const label = `${node.name} · ${node.label}`;
      if (option.textContent !== label) option.textContent = label;
    }
    const active = model.nodes.some(node => node.id === selected) ? selected : 'master';
    if (cy && !cy.getElementById(active).selected()) cy.getElementById(active).select();
    showSelection(active);
    notice.textContent = stale ? '连接中断，图中状态已过期。' : snapshot.available
      ? `${snapshot.demo ? '演示数据 · ' : ''}已加载 ${model.nodes.length - 1} 个从站 · 虚线表示归属，不代表实际布线。`
      : '等待共享内存；尚无可用的从站拓扑。';
  }
  picker.addEventListener('change', () => {
    if (cy) { cy.$(':selected').unselect(); cy.getElementById(picker.value).select(); cy.center(cy.getElementById(picker.value)); }
    showSelection(picker.value);
  });
  function zoom(factor) {
    if (cy) cy.zoom({ level: cy.zoom() * factor, renderedPosition: { x: cy.width() / 2, y: cy.height() / 2 } });
  }
  container.addEventListener('keydown', event => {
    if (!cy) return;
    const delta = { ArrowLeft: { x: 40, y: 0 }, ArrowRight: { x: -40, y: 0 }, ArrowUp: { x: 0, y: 40 }, ArrowDown: { x: 0, y: -40 } }[event.key];
    if (delta) cy.panBy(delta);
    else if (event.key === '+' || event.key === '=') zoom(1.25);
    else if (event.key === '-') zoom(.8);
    else if (event.key === 'Home') fit();
    else return;
    event.preventDefault();
  });
  const ready = document.fonts ? Promise.all([400, 500, 600].map(weight => document.fonts.load(`${weight} 14px AlibabaPuHuiTi`))).catch(() => {}) : Promise.resolve();
  ready.then(() => {
    if (typeof window.cytoscape !== 'function') {
      container.textContent = '拓扑图库加载失败，请刷新页面。仍可使用节点列表查看状态。';
      return;
    }
    cy = window.cytoscape({ container, elements: [], style: styles(), layout: { name: 'preset' },
      minZoom: .05, maxZoom: 2, boxSelectionEnabled: false, autoungrabify: true,
      userZoomingEnabled: true, wheelSensitivity: .25 });
    cy.on('tap', 'node', event => showSelection(event.target.id()));
    cy.on('mouseover mousemove', 'node', event => showTooltip(event.target.id(), event.renderedPosition || event.target.renderedPosition()));
    cy.on('mouseout', 'node', () => { hideTimer = setTimeout(hideTooltip, 180); });
    cy.on('pan zoom', hideTooltip);
    new ResizeObserver(() => { cy.resize(); render(); }).observe(container);
    schema = '';
    render();
  });
  return {
    update(bus) { snapshot = bus; stale = false; render(); },
    markStale() { stale = true; render(); },
    theme() { if (cy) cy.style(styles()); },
    fit, zoom,
  };
}
