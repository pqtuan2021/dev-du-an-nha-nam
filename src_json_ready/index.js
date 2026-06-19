/* global uibuilder */
'use strict';

const RELAY_ACK_TIMEOUT_MS = 8000;
const CONFIG_ACK_TIMEOUT_MS = 9000;

const $ = (id) => document.getElementById(id);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

const trangThai = {
    cheDoDangNhap: 'login',
    tenDangNhap: localStorage.getItem('nongtrai_username') || '',
    danhSachKhuVuc: [],
    danhSachTu: [],
    danhSachCamBien: [],
    khuVucDangChon: localStorage.getItem('fuviair_khu_vuc_id') || localStorage.getItem('nongtrai_room_id') || '',
    tuDangChon: localStorage.getItem('fuviair_tu_id') || '',
    camBienDangChon: localStorage.getItem('nongtrai_device_id') || '',
    duLieuMoiNhat: {},
    duLieuTheoCamBien: {},
    trangThaiTuMoiNhat: {},
    cheDoChoAck: {},
    daMoKyThuat: sessionStorage.getItem('nongtrai_dev_unlocked') === '1',
    relay: Array(10).fill(0),
    relayChiTiet: Array.from({ length: 10 }, () => ({})),
    relayDangCho: {},
    lenhCaiDatDangCho: {},
    cheDoTu: '--',
    phienBanCauHinh: '--',
    logHeThong: [],
    uibReady: false,
    lichSu: [],
    thongKe: null,
    cauHinhTu: { timers: {}, sensorRules: {}, fan: {} },
    dangApDungCauHinhForm: false,
};

const thongTinTrang = {
    pageOverview: ['Tổng quan', 'Theo dõi nhanh tình trạng vận hành.'],
    pageRooms: ['Khu vực / thiết bị', 'Thiết lập khu vực, tủ điều khiển và cảm biến.'],
    pageControl: ['Điều khiển tủ', 'Vận hành ngõ ra, lịch chạy và điều kiện tự động.'],
    pageUsage: ['Điện & thống kê', 'Tổng điện năng và giá trị cảm biến theo ngày, tháng, năm.'],
    pageDev: ['Nhật ký kỹ thuật', 'Lệnh hệ thống, lịch sử và log dành cho kỹ thuật viên.'],
};

function nowText() {
    return new Date().toLocaleTimeString('vi-VN', { hour12: false });
}

function num(v, digits = 1) {
    const n = Number(v);
    if (!Number.isFinite(n)) return '--';
    return String(Math.round(n * 10 ** digits) / 10 ** digits);
}

function intVal(v, fallback = 0) {
    const n = parseInt(v, 10);
    return Number.isFinite(n) ? n : fallback;
}

function floatVal(v, fallback = 0) {
    const n = parseFloat(v);
    return Number.isFinite(n) ? n : fallback;
}

function escapeHtml(v) {
    return String(v ?? '').replace(/[&<>"']/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }[ch]));
}

function safeJson(obj) {
    try { return JSON.stringify(obj, null, 2); }
    catch { return String(obj); }
}

function normalizePayload(payload) {
    if (typeof payload === 'string') {
        try { return JSON.parse(payload); } catch { return { raw: payload }; }
    }
    if (payload && typeof payload === 'object') return payload;
    return {};
}

function showToast(text, type = 'ok') {
    const box = $('toastBox');
    if (!box) return;
    const el = document.createElement('div');
    el.className = `toast ${type}`;
    el.textContent = text;
    box.appendChild(el);
    setTimeout(() => el.remove(), 3600);
}

function ghiLog(title, data) {
    const line = `[${nowText()}] ${title}${data !== undefined ? '\n' + safeJson(data) : ''}`;
    trangThai.logHeThong.unshift(line);
    trangThai.logHeThong = trangThai.logHeThong.slice(0, 160);
    const logBox = $('logBox');
    if (logBox) logBox.textContent = trangThai.logHeThong.join('\n\n');
}

function send(topic, payload = {}) {
    if (!window.uibuilder || typeof uibuilder.send !== 'function') {
        showToast('Chưa kết nối uibuilder, không gửi được lệnh.', 'err');
        ghiLog('UI gửi thất bại: chưa có uibuilder', { topic, payload });
        return false;
    }
    const msg = { topic, payload };
    uibuilder.send(msg);
    ghiLog(`UI → Node-RED: ${topic}`, payload);
    return true;
}

function makeMsgId(prefix) {
    return `${prefix}_${Date.now()}_${Math.floor(Math.random() * 10000)}`;
}


function configStorageKey(maTu = getGatewayId()) {
    const user = trangThai.tenDangNhap || 'guest';
    return `fuviair_config_${user}_${maTu || 'default'}`;
}

function taoCauHinhRong() {
    return { timers: {}, sensorRules: {}, fan: {}, updatedAt: 0 };
}

function napCauHinhTuLocal() {
    try {
        const raw = localStorage.getItem(configStorageKey());
        trangThai.cauHinhTu = raw ? { ...taoCauHinhRong(), ...JSON.parse(raw) } : taoCauHinhRong();
        if (!trangThai.cauHinhTu.timers) trangThai.cauHinhTu.timers = {};
        if (!trangThai.cauHinhTu.sensorRules) trangThai.cauHinhTu.sensorRules = {};
        if (!trangThai.cauHinhTu.fan) trangThai.cauHinhTu.fan = {};
    } catch {
        trangThai.cauHinhTu = taoCauHinhRong();
    }
}

function luuCauHinhTuLocal() {
    try {
        trangThai.cauHinhTu.updatedAt = Date.now();
        localStorage.setItem(configStorageKey(), JSON.stringify(trangThai.cauHinhTu));
    } catch { /* bỏ qua khi trình duyệt không cho lưu localStorage */ }
}

function timerCfgKey(relay, index) {
    return `${intVal(relay, 1)}:${intVal(index, 1)}`;
}

function normalizeTimeText(v, fallback = '00:00') {
    const s = String(v ?? '').trim();
    if (/^\d{1,2}:\d{2}$/.test(s)) {
        const [h, m] = s.split(':');
        return `${String(Math.max(0, Math.min(23, intVal(h, 0)))).padStart(2, '0')}:${String(Math.max(0, Math.min(59, intVal(m, 0)))).padStart(2, '0')}`;
    }
    return fallback;
}

function luuTimerDaCai(cfg) {
    const relay = intVal(cfg.relay, 1);
    const index = intVal(cfg.index ?? cfg.schedule_index, 1);
    trangThai.cauHinhTu.timers[timerCfgKey(relay, index)] = {
        relay,
        index,
        enable: intVal(cfg.enable, 0) ? 1 : 0,
        on: normalizeTimeText(cfg.on ?? cfg.on_time, '00:00'),
        off: normalizeTimeText(cfg.off ?? cfg.off_time, '00:00'),
        cfgVersion: cfg.cfgVersion ?? trangThai.phienBanCauHinh,
        savedAt: Date.now(),
    };
}

function luuSensorRuleDaCai(cfg) {
    const relay = intVal(cfg.relay, 1);
    const logic = String(cfg.logic || 'ABOVE').toUpperCase();
    const onValue = cfg.onValue ?? cfg.onAbove ?? cfg.onBelow ?? 0;
    const offValue = cfg.offValue ?? cfg.offBelow ?? cfg.offAbove ?? 0;
    trangThai.cauHinhTu.sensorRules[String(relay)] = {
        relay,
        enable: intVal(cfg.enable, 1) ? 1 : 0,
        id_device: String(cfg.id_device || cfg.device_id || '').trim(),
        field: String(cfg.field || 'temperature'),
        logic: logic === 'BELOW' ? 'BELOW' : 'ABOVE',
        onValue: Number(onValue) || 0,
        offValue: Number(offValue) || 0,
        cfgVersion: cfg.cfgVersion ?? trangThai.phienBanCauHinh,
        savedAt: Date.now(),
    };
}

function capNhatCauHinhDaLuu(p) {
    if (!p || typeof p !== 'object') return false;
    let changed = false;
    let formChanged = false;

    const timerRules = p.timer_rules || p.timerRules || p.timers || p.data?.timer_rules || p.data?.timerRules;
    if (Array.isArray(timerRules)) {
        timerRules.forEach(rule => {
            const relay = intVal(rule.relay, 0);
            if (!relay) return;
            const schedules = rule.schedules || rule.schedule || rule.items;
            if (Array.isArray(schedules)) {
                schedules.forEach(sc => { luuTimerDaCai({ ...sc, relay, cfgVersion: p.cfgVersion ?? rule.cfgVersion }); changed = true; formChanged = true; });
            } else {
                luuTimerDaCai({ ...rule, relay, cfgVersion: p.cfgVersion ?? rule.cfgVersion }); changed = true; formChanged = true;
            }
        });
    }

    const schedules = p.schedules || p.data?.schedules;
    if (Array.isArray(schedules) && p.relay !== undefined) {
        schedules.forEach(sc => { luuTimerDaCai({ ...sc, relay: p.relay, cfgVersion: p.cfgVersion }); changed = true; formChanged = true; });
    }

    if (String(p.cmd || '').toUpperCase() === 'SET_TIMER' && p.relay !== undefined && (p.index !== undefined || p.on !== undefined || p.off !== undefined || p.on_time !== undefined)) {
        luuTimerDaCai(p); changed = true; formChanged = true;
    }

    const sensorRules = p.sensor_rules || p.sensorRules || p.rules || p.data?.sensor_rules || p.data?.sensorRules;
    if (Array.isArray(sensorRules)) {
        sensorRules.forEach(rule => { if (rule.relay !== undefined) { luuSensorRuleDaCai({ ...rule, cfgVersion: p.cfgVersion ?? rule.cfgVersion }); changed = true; formChanged = true; } });
    }
    if (String(p.cmd || '').toUpperCase() === 'SET_SENSOR_RULE' && p.relay !== undefined) {
        luuSensorRuleDaCai(p); changed = true; formChanged = true;
    }

    const fanMode = p.fanMode ?? p.fan_mode ?? p.data?.fanMode;
    if (fanMode !== undefined || p.fanOnTemp !== undefined || p.fanOffTemp !== undefined) {
        trangThai.cauHinhTu.fan = {
            ...trangThai.cauHinhTu.fan,
            fanMode: fanMode !== undefined ? Number(fanMode) : trangThai.cauHinhTu.fan.fanMode,
            fanOnTemp: p.fanOnTemp !== undefined ? Number(p.fanOnTemp) : trangThai.cauHinhTu.fan.fanOnTemp,
            fanOffTemp: p.fanOffTemp !== undefined ? Number(p.fanOffTemp) : trangThai.cauHinhTu.fan.fanOffTemp,
        };
        changed = true;
    }

    if (changed) {
        luuCauHinhTuLocal();
        if (formChanged) apDungCauHinhLenForm(false);
        else apDungCauHinhQuatLenForm();
        renderCauHinhDaLuu();
    }
    return changed;
}

function apDungCauHinhLichLenForm(resetIfMissing = false) {
    const relayEl = $('timerRelay'), indexEl = $('timerIndex');
    if (!relayEl || !indexEl) return;
    const key = timerCfgKey(relayEl.value, indexEl.value);
    const cfg = trangThai.cauHinhTu.timers?.[key];
    trangThai.dangApDungCauHinhForm = true;
    if (cfg) {
        if ($('timerEnable')) $('timerEnable').value = String(cfg.enable ? 1 : 0);
        if ($('timerOn')) $('timerOn').value = cfg.on || '00:00';
        if ($('timerOff')) $('timerOff').value = cfg.off || '00:00';
    } else if (resetIfMissing) {
        if ($('timerEnable')) $('timerEnable').value = '0';
        if ($('timerOn')) $('timerOn').value = '00:00';
        if ($('timerOff')) $('timerOff').value = '00:00';
    }
    trangThai.dangApDungCauHinhForm = false;
}

function apDungCauHinhCamBienLenForm(resetIfMissing = false) {
    const relayEl = $('sensorRelay');
    if (!relayEl) return;
    const cfg = trangThai.cauHinhTu.sensorRules?.[String(intVal(relayEl.value, 1))];
    trangThai.dangApDungCauHinhForm = true;
    if (cfg) {
        if ($('sensorEnable')) $('sensorEnable').value = String(cfg.enable ? 1 : 0);
        if ($('sensorDeviceSelect') && cfg.id_device) $('sensorDeviceSelect').value = cfg.id_device;
        if ($('sensorField')) $('sensorField').value = cfg.field || 'temperature';
        if ($('sensorLogic')) $('sensorLogic').value = cfg.logic || 'ABOVE';
        if ($('sensorOnValue')) $('sensorOnValue').value = String(cfg.onValue ?? 0);
        if ($('sensorOffValue')) $('sensorOffValue').value = String(cfg.offValue ?? 0);
    } else if (resetIfMissing) {
        if ($('sensorEnable')) $('sensorEnable').value = '0';
        if ($('sensorField')) $('sensorField').value = 'temperature';
        if ($('sensorLogic')) $('sensorLogic').value = 'ABOVE';
        if ($('sensorOnValue')) $('sensorOnValue').value = '30';
        if ($('sensorOffValue')) $('sensorOffValue').value = '25';
    }
    trangThai.dangApDungCauHinhForm = false;
}

function apDungCauHinhQuatLenForm() {
    const fan = trangThai.cauHinhTu.fan || {};
    if (fan.fanMode !== undefined && $('fanModeInput')) $('fanModeInput').value = String(fan.fanMode);
    if (fan.fanOnTemp !== undefined && $('fanOnTempInput')) $('fanOnTempInput').value = String(fan.fanOnTemp);
    if (fan.fanOffTemp !== undefined && $('fanOffTempInput')) $('fanOffTempInput').value = String(fan.fanOffTemp);
}

function apDungCauHinhLenForm(resetIfMissing = false) {
    apDungCauHinhLichLenForm(resetIfMissing);
    apDungCauHinhCamBienLenForm(resetIfMissing);
    apDungCauHinhQuatLenForm();
}

function thoiGianDaLuuText(ts) {
    if (!ts) return '';
    try { return new Date(ts).toLocaleString('vi-VN', { hour12: false }); }
    catch { return ''; }
}

function trangThaiBatTat(v) {
    return intVal(v, 0) ? 'Bật' : 'Tắt';
}

function logicLabel(logic) {
    return String(logic || 'ABOVE').toUpperCase() === 'BELOW'
        ? 'Bật khi chỉ số thấp hơn ngưỡng'
        : 'Bật khi chỉ số cao hơn ngưỡng';
}

function renderCauHinhDaLuu() {
    const timerBox = $('timerSavedList');
    const sensorBox = $('sensorRuleSavedList');
    const timers = Object.values(trangThai.cauHinhTu?.timers || {})
        .sort((a, b) => intVal(a.relay, 0) - intVal(b.relay, 0) || intVal(a.index, 0) - intVal(b.index, 0));
    const rules = Object.values(trangThai.cauHinhTu?.sensorRules || {})
        .sort((a, b) => intVal(a.relay, 0) - intVal(b.relay, 0));

    if (timerBox) {
        if (!timers.length) {
            timerBox.className = 'saved-config-list empty-list';
            timerBox.textContent = 'Chưa có lịch đã lưu.';
        } else {
            timerBox.className = 'saved-config-list';
            timerBox.innerHTML = timers.map(t => {
                const pending = Object.values(trangThai.lenhCaiDatDangCho || {}).some(x => x.type === 'timer' && intVal(x.relay, 0) === intVal(t.relay, 0) && intVal(x.index, 0) === intVal(t.index, 0));
                return `<div class="saved-config-item ${pending ? 'pending' : ''}">
          <div><strong>K${escapeHtml(t.relay)} · Lịch ${escapeHtml(t.index)}</strong><span>${escapeHtml(t.on)} → ${escapeHtml(t.off)} · ${trangThaiBatTat(t.enable)}${t.savedAt ? ' · ' + escapeHtml(thoiGianDaLuuText(t.savedAt)) : ''}</span></div>
          <button class="small-btn" type="button" data-load-timer="${escapeHtml(t.relay)}:${escapeHtml(t.index)}">Chọn</button>
        </div>`;
            }).join('');
        }
    }

    if (sensorBox) {
        if (!rules.length) {
            sensorBox.className = 'saved-config-list empty-list';
            sensorBox.textContent = 'Chưa có điều kiện đã lưu.';
        } else {
            sensorBox.className = 'saved-config-list';
            sensorBox.innerHTML = rules.map(r => {
                const pending = Object.values(trangThai.lenhCaiDatDangCho || {}).some(x => x.type === 'sensor' && intVal(x.relay, 0) === intVal(r.relay, 0));
                const unit = r.field === 'temperature' ? '°C' : r.field === 'humidity' ? '%' : r.field === 'co2' ? 'ppm' : '';
                return `<div class="saved-config-item ${pending ? 'pending' : ''}">
          <div><strong>K${escapeHtml(r.relay)} · ${escapeHtml(fieldLabel(r.field))}</strong><span>${escapeHtml(r.id_device || '--')} · ${escapeHtml(logicLabel(r.logic))} · bật ${escapeHtml(r.onValue)}${unit}, tắt ${escapeHtml(r.offValue)}${unit} · ${trangThaiBatTat(r.enable)}</span></div>
          <button class="small-btn" type="button" data-load-sensor-rule="${escapeHtml(r.relay)}">Chọn</button>
        </div>`;
            }).join('');
        }
    }
}

function loadTimerToForm(relay, index) {
    if ($('timerRelay')) $('timerRelay').value = String(relay);
    if ($('timerIndex')) $('timerIndex').value = String(index);
    apDungCauHinhLichLenForm(true);
}

function loadSensorRuleToForm(relay) {
    if ($('sensorRelay')) $('sensorRelay').value = String(relay);
    apDungCauHinhCamBienLenForm(true);
}

function setSaveStatus(kind, text, type = 'muted') {
    const el = kind === 'timer' ? $('timerAckStatus') : $('sensorAckStatus');
    if (!el) return;
    el.className = `save-status ${type}`;
    el.textContent = text;
}

function markConfigPending(msgId, data) {
    if (!msgId) return;
    trangThai.lenhCaiDatDangCho[msgId] = { ...data, start: Date.now() };
    renderCauHinhDaLuu();
}

function clearConfigPending(msgId, ok, message = '') {
    const p = trangThai.lenhCaiDatDangCho[msgId];
    if (!p) return false;
    delete trangThai.lenhCaiDatDangCho[msgId];
    const label = p.type === 'timer' ? `Lịch K${p.relay} - lịch ${p.index}` : `Điều kiện K${p.relay}`;
    setSaveStatus(p.type, ok ? `${label} đã được tủ xác nhận.` : `${label} chưa lưu được${message ? ': ' + message : '.'}`, ok ? 'ok' : 'err');
    renderCauHinhDaLuu();
    return true;
}

function kiemTraLenhCaiDatQuaHan() {
    const now = Date.now();
    let changed = false;
    Object.entries(trangThai.lenhCaiDatDangCho || {}).forEach(([msgId, p]) => {
        if (!p?.start || now - p.start <= CONFIG_ACK_TIMEOUT_MS) return;
        delete trangThai.lenhCaiDatDangCho[msgId];
        const label = p.type === 'timer' ? `lịch K${p.relay} - lịch ${p.index}` : `điều kiện K${p.relay}`;
        setSaveStatus(p.type, `Đã gửi ${label}, nhưng chưa nhận phản hồi xác nhận từ tủ.`, 'warn');
        changed = true;
    });
    if (changed) renderCauHinhDaLuu();
}

function requestConfigSync() {
    send('sync_request', { msg_id: makeMsgId('UI_SYNC'), gateway_id: getGatewayId(), control_id: getGatewayId(), ma_tu: getGatewayId(), ack_req: true });
}

function setBadge(online) {
    const badge = $('socketBadge');
    if (badge) {
        badge.className = `badge ${online ? 'good' : 'bad'}`;
        badge.textContent = online ? 'Online' : 'Offline';
    }
}

function setAuthMode(mode) {
    trangThai.cheDoDangNhap = mode;
    $('loginTab')?.classList.toggle('active', mode === 'login');
    $('registerTab')?.classList.toggle('active', mode === 'register');
    if ($('authSubmit')) $('authSubmit').textContent = mode === 'login' ? 'Đăng nhập' : 'Đăng ký';
    if ($('authStatus')) $('authStatus').textContent = mode === 'login' ? 'Nhập tài khoản để đăng nhập.' : 'Tạo tài khoản mới.';
}

function showApp() {
    $('authView')?.classList.add('hidden');
    $('appView')?.classList.remove('hidden');
    if ($('currentUserText')) $('currentUserText').textContent = trangThai.tenDangNhap || '--';
    buildRelayControls();
    buildRelayMiniGrid();
    requestDanhSachKhuVucTu();
    updateSelectedDisplay();
    renderThietBiHeThong();
    updateDevView();
    napCauHinhTuLocal();
    apDungCauHinhLenForm(false);
    requestStatus();
    setTimeout(requestConfigSync, 500);
}

function showAuth() {
    $('appView')?.classList.add('hidden');
    $('authView')?.classList.remove('hidden');
}

function handleAuthSubmit(ev) {
    ev.preventDefault();
    const username = $('authUsername').value.trim();
    const password = $('authPassword').value;
    if (!username || !password) return;
    const topic = trangThai.cheDoDangNhap === 'login' ? 'login_request' : 'register_request';
    if ($('authStatus')) $('authStatus').textContent = 'Đang gửi...';
    send(topic, { username, password });
}

function handleAuthResponse(payload) {
    const ok = payload?.status === 'success' || payload?.status === 'ok' || payload?.success === true;
    const message = payload?.message || (ok ? 'Thành công.' : 'Có lỗi.');
    if ($('authStatus')) $('authStatus').textContent = message;
    showToast(message, ok ? 'ok' : 'err');
    if (ok) {
        trangThai.tenDangNhap = payload.username || $('authUsername')?.value?.trim() || trangThai.tenDangNhap;
        localStorage.setItem('nongtrai_username', trangThai.tenDangNhap);
        showApp();
    }
}

function logout() {
    localStorage.removeItem('nongtrai_username');
    trangThai.tenDangNhap = '';
    showAuth();
}

function requestDanhSachKhuVucTu() {
    if (!trangThai.tenDangNhap) return;
    send('room_list_request', { username: trangThai.tenDangNhap, v: 'V10' });
    send('room_devices_list_request', { username: trangThai.tenDangNhap, v: 'V10' });
}

function layMaKhuVuc(row) {
    return String(row?.area_id ?? row?.khu_vuc_id ?? row?.id_area ?? row?.room_id ?? row?.id ?? row?.gateway_id ?? '').trim();
}

function layTenKhuVuc(row) {
    return String(row?.area_name || row?.ten_khu_vuc || row?.room_name || row?.name || row?.label || layMaKhuVuc(row) || 'Không tên').trim();
}

function layMaTu(row) {
    return String(row?.control_id || row?.ma_tu || row?.gateway_id || row?.gateway_device_id || row?.device_id || row?.gw_id || '').trim();
}

function layTenTu(row) {
    return String(row?.control_name || row?.ten_tu || row?.cabinet_name || row?.room_name || row?.name || layMaTu(row) || 'Không tên').trim();
}

function napDanhSachKhuVucTu(payload) {
    const p = normalizePayload(payload);
    const rows = Array.isArray(p) ? p : (Array.isArray(p.rows) ? p.rows : Array.isArray(p.control_units) ? p.control_units : Array.isArray(p.rooms) ? p.rooms : []);
    const areasIn = Array.isArray(p.areas) ? p.areas : [];
    const areaMap = new Map();
    const tuMap = new Map();

    for (const a of areasIn) {
        const areaId = layMaKhuVuc(a);
        if (!areaId) continue;
        areaMap.set(areaId, { id: areaId, area_id: areaId, area_name: layTenKhuVuc(a), note: a.note || a.ghi_chu || '' });
    }

    for (const r of rows) {
        const maTu = layMaTu(r);
        const areaId = layMaKhuVuc(r) || (maTu ? `KV_${maTu}` : '');
        if (areaId && !areaMap.has(areaId)) {
            areaMap.set(areaId, { id: areaId, area_id: areaId, area_name: layTenKhuVuc(r), note: r.note || r.ghi_chu || '' });
        }
        if (maTu) {
            tuMap.set(maTu, {
                id: String(r.control_unit_pk ?? r.control_unit_id ?? r.tu_id ?? r.id ?? maTu),
                area_id: areaId,
                area_name: areaMap.get(areaId)?.area_name || layTenKhuVuc(r),
                control_id: maTu,
                gateway_id: maTu,
                control_name: layTenTu(r),
                stm_id: r.stm_id || r.stm || maTu,
                device_key: r.device_key || '',
                created_at: r.created_at || '',
            });
        }
    }

    trangThai.danhSachKhuVuc = Array.from(areaMap.values());
    trangThai.danhSachTu = Array.from(tuMap.values());

    if (!trangThai.khuVucDangChon && trangThai.danhSachKhuVuc.length) {
        trangThai.khuVucDangChon = trangThai.danhSachKhuVuc[0].id;
    }
    if (!trangThai.tuDangChon) {
        const firstTu = layDanhSachTuTheoKhu(trangThai.khuVucDangChon)[0] || trangThai.danhSachTu[0];
        if (firstTu) trangThai.tuDangChon = firstTu.control_id;
    }
    luuLuaChon();
    renderTatCaDanhSach();
}

function napDanhSachCamBien(payload) {
    const p = normalizePayload(payload);
    const rows = Array.isArray(p) ? p : (Array.isArray(p.devices) ? p.devices : Array.isArray(p.rows) ? p.rows : []);
    trangThai.danhSachCamBien = rows.map(d => {
        const id = String(d.id_device || d.device_id || d.sensor_device_id || d.ma_cam_bien || '').trim();
        const maTu = String(d.control_id || d.gateway_id || d.ma_tu || '').trim();
        return {
            id,
            id_device: id,
            device_id: id,
            control_id: maTu,
            gateway_id: maTu,
            area_id: String(d.area_id || d.khu_vuc_id || '').trim(),
            device_name: d.device_name || d.sensor_name || d.name || id,
            fields_json: d.fields_json || '',
        };
    }).filter(d => d.id);

    const first = camBienTheoTu()[0] || trangThai.danhSachCamBien[0];
    if (!trangThai.camBienDangChon && first) trangThai.camBienDangChon = first.id;
    luuLuaChon();
    renderTatCaDanhSach();
}

function luuLuaChon() {
    if (trangThai.khuVucDangChon) localStorage.setItem('fuviair_khu_vuc_id', trangThai.khuVucDangChon);
    if (trangThai.tuDangChon) localStorage.setItem('fuviair_tu_id', trangThai.tuDangChon);
    if (trangThai.camBienDangChon) localStorage.setItem('nongtrai_device_id', trangThai.camBienDangChon);
}

function layKhuVucDangChon() {
    return trangThai.danhSachKhuVuc.find(a => String(a.id) === String(trangThai.khuVucDangChon)) || trangThai.danhSachKhuVuc[0] || null;
}

function layDanhSachTuTheoKhu(areaId = trangThai.khuVucDangChon) {
    const list = trangThai.danhSachTu.filter(t => String(t.area_id || '') === String(areaId || ''));
    return list.length ? list : (areaId ? [] : trangThai.danhSachTu);
}

function layTuDangChon() {
    return trangThai.danhSachTu.find(t => String(t.control_id) === String(trangThai.tuDangChon)) || layDanhSachTuTheoKhu()[0] || trangThai.danhSachTu[0] || null;
}

function getGatewayId() {
    const tu = layTuDangChon();
    const maTu = tu?.control_id || $('gatewayId')?.value?.trim();
    return maTu || 'ESP79';
}

function camBienTheoTu(maTu = getGatewayId()) {
    return trangThai.danhSachCamBien.filter(d => String(d.control_id || d.gateway_id || '') === String(maTu || ''));
}

function camBienThuocTuDangChon(deviceId) {
    if (!deviceId) return false;
    return camBienTheoTu().some(d => String(d.id) === String(deviceId));
}

function layDuLieuCamBienDangChon() {
    const id = String(trangThai.camBienDangChon || '').trim();
    if (!id) return null;
    return trangThai.duLieuTheoCamBien[id] || null;
}

function hienThiDuLieuCamBien(src = null) {
    const data = src || layDuLieuCamBienDangChon();
    if (!data) {
        if ($('tempVal')) $('tempVal').textContent = '--';
        if ($('humiVal')) $('humiVal').textContent = '--';
        if ($('co2Val')) $('co2Val').textContent = '--';
        if ($('lightVal')) $('lightVal').textContent = '--';
        if ($('voltageVal')) $('voltageVal').textContent = '--';
        if ($('currentVal')) $('currentVal').textContent = '--';
        if ($('powerVal')) $('powerVal').textContent = '--';
        if ($('frequencyVal')) $('frequencyVal').textContent = '--';
        return;
    }

    const temperature = data.temperature ?? data.temp ?? data.t ?? data.Temperature;
    const humidity = data.humidity ?? data.humi ?? data.h ?? data.Humidity;
    const co2 = data.co2 ?? data.CO2 ?? data.Co2;
    const light = data.light ?? data.lux ?? data.Light;
    const voltage = data.voltage ?? data.volt ?? data.v;
    const current = data.current ?? data.amp ?? data.a;
    const frequency = data.frequency ?? data.freq ?? data.f;
    const power = data.power ?? data.watt ?? data.w;

    if ($('tempVal')) $('tempVal').textContent = num(temperature);
    if ($('humiVal')) $('humiVal').textContent = num(humidity);
    if ($('co2Val')) $('co2Val').textContent = num(co2, 0);
    if ($('lightVal')) $('lightVal').textContent = num(light, 0);
    if ($('voltageVal')) $('voltageVal').textContent = num(voltage);
    if ($('currentVal')) $('currentVal').textContent = num(current, 2);
    if ($('powerVal')) $('powerVal').textContent = num(power, 1);
    if ($('frequencyVal')) $('frequencyVal').textContent = num(frequency, 1);
}

function chonKhuVuc(areaId, chonTuDau = true) {
    trangThai.khuVucDangChon = String(areaId || '');
    if (chonTuDau) {
        const dsTu = layDanhSachTuTheoKhu(trangThai.khuVucDangChon);
        trangThai.tuDangChon = dsTu[0]?.control_id || '';
        const cb = camBienTheoTu(trangThai.tuDangChon)[0];
        if (cb) trangThai.camBienDangChon = cb.id;
    }
    luuLuaChon();
    renderTatCaDanhSach();
    napCauHinhTuLocal();
    apDungCauHinhLenForm(false);
    requestStatus();
}


function chonTu(maTu) {
    trangThai.tuDangChon = String(maTu || '');
    const tu = layTuDangChon();
    if (tu?.area_id) trangThai.khuVucDangChon = tu.area_id;
    const cb = camBienTheoTu(trangThai.tuDangChon)[0];
    if (cb) trangThai.camBienDangChon = cb.id;
    if ($('gatewayId') && trangThai.tuDangChon) $('gatewayId').value = trangThai.tuDangChon;
    luuLuaChon();
    renderTatCaDanhSach();
    napCauHinhTuLocal();
    apDungCauHinhLenForm(false);
    requestStatus();
    setTimeout(requestConfigSync, 300);
    showToast(`Đã chọn tủ ${trangThai.tuDangChon || '--'}.`, 'ok');
}

function chonCamBien(id, rerender = true) {
    trangThai.camBienDangChon = id || '';
    if (id) localStorage.setItem('nongtrai_device_id', id);
    trangThai.duLieuMoiNhat = layDuLieuCamBienDangChon() || {};
    hienThiDuLieuCamBien(trangThai.duLieuMoiNhat);
    if (rerender) renderDeviceSelects();
    updateSelectedDisplay();
    renderThietBiHeThong();
}

function renderTatCaDanhSach() {
    renderAreaControlSelects();
    renderRoomList();
    renderDeviceSelects();
    updateSelectedDisplay();
    renderThietBiHeThong();
    renderCauHinhDaLuu();
}

function renderAreaControlSelects() {
    const areaSelects = [$('overviewAreaSelect'), $('controlAreaSelect'), $('sensorAreaSelect'), $('controlAreaSelectMain')].filter(Boolean);
    for (const sel of areaSelects) {
        sel.innerHTML = '';
        if (!trangThai.danhSachKhuVuc.length) {
            const opt = document.createElement('option'); opt.value = ''; opt.textContent = 'Chưa có khu vực'; sel.appendChild(opt); continue;
        }
        for (const a of trangThai.danhSachKhuVuc) {
            const opt = document.createElement('option');
            opt.value = a.id;
            opt.textContent = a.area_name || a.id;
            if (String(opt.value) === String(trangThai.khuVucDangChon)) opt.selected = true;
            sel.appendChild(opt);
        }
    }

    const tuSelectPairs = [
        [$('overviewControlSelect'), trangThai.khuVucDangChon],
        [$('controlUnitSelectMain'), trangThai.khuVucDangChon],
        [$('sensorControlSelect'), $('sensorAreaSelect')?.value || trangThai.khuVucDangChon],
    ].filter(p => p[0]);

    for (const [sel, areaId] of tuSelectPairs) {
        const list = layDanhSachTuTheoKhu(areaId);
        sel.innerHTML = '';
        if (!list.length) {
            const opt = document.createElement('option'); opt.value = ''; opt.textContent = 'Chưa có tủ điều khiển'; sel.appendChild(opt); continue;
        }
        for (const tu of list) {
            const opt = document.createElement('option');
            opt.value = tu.control_id;
            opt.textContent = `${tu.control_name || tu.control_id} (${tu.control_id})`;
            if (String(opt.value) === String(trangThai.tuDangChon)) opt.selected = true;
            sel.appendChild(opt);
        }
    }
}

function renderRoomList() {
    const box = $('roomList');
    if (!box) return;
    box.innerHTML = '';
    if (!trangThai.danhSachKhuVuc.length) {
        box.className = 'room-list empty-list';
        box.textContent = 'Chưa có khu vực.';
        return;
    }
    box.className = 'room-list';
    for (const area of trangThai.danhSachKhuVuc) {
        const dsTu = layDanhSachTuTheoKhu(area.id);
        const areaCard = document.createElement('div');
        areaCard.className = `room-card ${String(area.id) === String(trangThai.khuVucDangChon) ? 'active' : ''}`;
        const tuHtml = dsTu.length ? dsTu.map(tu => {
            const dsCamBien = camBienTheoTu(tu.control_id);
            const chips = dsCamBien.length ? `<div class="device-chip-list">${dsCamBien.map(d => `<span class="device-chip">${escapeHtml(d.id)}</span>`).join('')}</div>` : '<span class="room-meta">Chưa gán cảm biến.</span>';
            return `<div class="tu-nested ${String(tu.control_id) === String(trangThai.tuDangChon) ? 'active' : ''}"><b>${escapeHtml(tu.control_name || tu.control_id)}</b><span class="room-meta">Mã tủ: ${escapeHtml(tu.control_id)}</span>${chips}<button class="select-control" data-control="${escapeHtml(tu.control_id)}" type="button">Chọn tủ</button></div>`;
        }).join('') : '<span class="room-meta">Khu vực này chưa có tủ điều khiển.</span>';

        areaCard.innerHTML = `
      <div>
        <strong>${escapeHtml(area.area_name || area.id)}</strong>
        <span class="room-meta">${dsTu.length} tủ điều khiển</span>
        ${tuHtml}
      </div>
      <div class="room-actions">
        <button class="select-area" type="button">Chọn khu vực</button>
      </div>
    `;
        areaCard.querySelector('.select-area')?.addEventListener('click', () => chonKhuVuc(area.id));
        areaCard.querySelectorAll('.select-control').forEach(btn => btn.addEventListener('click', () => chonTu(btn.dataset.control)));
        box.appendChild(areaCard);
    }
}

function renderDeviceSelects() {
    const list = camBienTheoTu();
    const options = list.length ? list : trangThai.danhSachCamBien;
    if (!trangThai.camBienDangChon && options.length) trangThai.camBienDangChon = options[0].id;

    for (const sel of [$('overviewSensorSelect'), $('sensorDeviceSelect'), $('historyDeviceSelect'), $('usageDeviceSelect')]) {
        if (!sel) continue;
        sel.innerHTML = '';
        if (!options.length) {
            const opt = document.createElement('option'); opt.value = ''; opt.textContent = 'Chưa có thiết bị cảm biến'; sel.appendChild(opt); continue;
        }
        for (const d of options) {
            const opt = document.createElement('option');
            opt.value = d.id;
            opt.textContent = `${d.device_name || d.id} (${d.id})`;
            if (String(d.id) === String(trangThai.camBienDangChon)) opt.selected = true;
            sel.appendChild(opt);
        }
    }
}

function updateSelectedDisplay() {
    const area = layKhuVucDangChon();
    const tu = layTuDangChon();
    const dsCamBien = tu ? camBienTheoTu(tu.control_id) : [];
    if (tu && $('gatewayId')) $('gatewayId').value = tu.control_id;
    const camBien = dsCamBien.find(d => String(d.id) === String(trangThai.camBienDangChon)) || dsCamBien[0] || null;
    const text = area
        ? `${area.area_name || area.id} · ${tu?.control_name || '--'}${tu?.control_id ? ' (' + tu.control_id + ')' : ''} · ${dsCamBien.length} cảm biến${camBien ? ' · đang xem ' + camBien.id : ''}`
        : 'Chưa chọn khu vực.';
    if ($('overviewRoomInfo')) $('overviewRoomInfo').textContent = text;
    if ($('controlRoomInfo')) $('controlRoomInfo').textContent = text;
}

function renderThietBiHeThong() {
    const tu = layTuDangChon();
    const maTu = tu?.control_id || getGatewayId();
    const dsCamBien = camBienTheoTu(maTu);
    const last = trangThai.trangThaiTuMoiNhat || {};
    if ($('controlCabinetId')) $('controlCabinetId').textContent = maTu || '--';
    if ($('controlCabinetState')) {
        const txt = last.state ? `${last.state}${last.ip ? ' · IP ' + last.ip : ''}` : 'Chưa có dữ liệu.';
        $('controlCabinetState').textContent = txt;
    }
    if ($('fuviairDeviceCount')) $('fuviairDeviceCount').textContent = String(dsCamBien.length);
    if ($('fuviairDeviceList')) {
        if (!dsCamBien.length) $('fuviairDeviceList').textContent = 'Chưa có cảm biến.';
        else $('fuviairDeviceList').innerHTML = dsCamBien.map(d => {
            const online = trangThai.duLieuTheoCamBien[d.id] ? ' · đã có dữ liệu' : '';
            return `<span class="device-chip">${escapeHtml(d.id)}${escapeHtml(online)}</span>`;
        }).join(' ');
    }
}

function buildRelayMiniGrid() {
    const box = $('relayMiniGrid');
    if (!box) return;
    box.innerHTML = '';
    for (let i = 0; i < 10; i++) {
        const el = document.createElement('div');
        el.className = `relay-pill ${trangThai.relay[i] ? 'on' : 'off'} ${trangThai.relayDangCho[i + 1] ? 'pending' : ''}`;
        el.id = `relayPill${i + 1}`;
        el.innerHTML = `<span>K${i + 1}: ${trangThai.relay[i] ? 'BẬT' : 'TẮT'}</span><small>${escapeHtml(relayCaption(i))}</small>`;
        box.appendChild(el);
    }
}

function buildRelayControls() {
    const box = $('relayControlGrid');
    if (!box) return;
    box.innerHTML = '';
    for (let i = 0; i < 10; i++) {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = `relay-toggle ${trangThai.relay[i] ? 'on' : 'off'} ${trangThai.relayDangCho[i + 1] ? 'pending' : ''}`;
        btn.id = `relayBtn${i + 1}`;
        btn.innerHTML = `<span>K${i + 1}</span><strong>${trangThai.relay[i] ? 'BẬT' : 'TẮT'}</strong><small>${escapeHtml(relayCaption(i))}</small>`;
        btn.addEventListener('click', () => sendRelay(i + 1, trangThai.relay[i] ? 0 : 1));
        box.appendChild(btn);
    }
}

function updateRelayUi() {
    for (let i = 0; i < 10; i++) {
        const on = !!trangThai.relay[i];
        const pill = $(`relayPill${i + 1}`);
        const pending = !!trangThai.relayDangCho[i + 1];
        const cap = escapeHtml(relayCaption(i));
        if (pill) { pill.className = `relay-pill ${on ? 'on' : 'off'} ${pending ? 'pending' : ''}`; pill.innerHTML = `<span>K${i + 1}: ${on ? 'BẬT' : 'TẮT'}</span><small>${cap}</small>`; }
        const btn = $(`relayBtn${i + 1}`);
        if (btn) { btn.className = `relay-toggle ${on ? 'on' : 'off'} ${pending ? 'pending' : ''}`; btn.innerHTML = `<span>K${i + 1}</span><strong>${on ? 'BẬT' : 'TẮT'}</strong><small>${cap}</small>`; }
    }
    updateRunningInfo();
}

function modeLabel(mode) {
    const m = String(mode || '').toUpperCase();
    return { MANUAL: 'Thủ công', TIMER: 'Lịch trình', SENSOR: 'Theo cảm biến' }[m] || mode || '--';
}

function fieldLabel(field) {
    const f = String(field || '').toLowerCase();
    return { temperature: 'nhiệt độ', temp: 'nhiệt độ', humidity: 'độ ẩm', humi: 'độ ẩm', co2: 'CO₂', light: 'ánh sáng', lux: 'ánh sáng', voltage: 'điện áp', current: 'dòng điện', frequency: 'tần số', power: 'công suất' }[f] || f || 'chỉ số';
}

function relaySourceLabel(meta = {}) {
    const src = String(meta.source || meta.src || meta.cmd_source || '').toUpperCase();
    if (src === 'TIMER') return (meta.schedule_index || meta.si) ? `Lịch ${meta.schedule_index || meta.si}` : 'Lịch trình';
    if (src === 'SENSOR') {
        const dev = meta.id_device || meta.device_id || meta.id || meta.did || '';
        const val = Number.isFinite(Number(meta.value)) ? ` ${num(meta.value)}` : '';
        return `${fieldLabel(meta.field)}${dev ? ' · ' + dev : ''}${val}`;
    }
    if (src === 'MANUAL' || src === 'HMI' || src === 'ESP' || src === 'UI') return 'Thủ công';
    if (src === 'SYSTEM') return 'Hệ thống';
    return src ? src : 'Chưa rõ nguồn';
}

function relayCaption(idx) {
    const pending = trangThai.relayDangCho[idx + 1];
    if (pending) return pending.state ? 'Đang gửi lệnh bật...' : 'Đang gửi lệnh tắt...';
    if (!trangThai.relay[idx]) return 'Đã tắt';
    const meta = trangThai.relayChiTiet[idx] || {};
    return relaySourceLabel(meta);
}

function runningText() {
    const mode = String(trangThai.cheDoTu || '').toUpperCase();
    const active = [];
    for (let i = 0; i < 10; i++) {
        if (trangThai.relay[i]) active.push(`K${i + 1} ${relayCaption(i)}`);
    }
    if (active.length) return `${modeLabel(mode)} · ${active.join('; ')}`;
    if (mode === 'TIMER') return 'Đang ở chế độ lịch trình, chưa có ngõ ra nào trong khung giờ bật.';
    if (mode === 'SENSOR') return 'Đang ở chế độ theo cảm biến, chưa có ngõ ra nào đạt điều kiện bật.';
    if (mode === 'MANUAL') return 'Đang ở chế độ thủ công.';
    return '--';
}

function updateRunningInfo() {
    const text = runningText();
    if ($('runState')) $('runState').textContent = text;
    if ($('controlRunInfo')) $('controlRunInfo').textContent = text;
}

function setModeUi(mode) {
    if (!mode) return;
    const m = String(mode).trim().toUpperCase();
    trangThai.cheDoTu = m;
    if ($('modeState')) $('modeState').textContent = modeLabel(m);
    $$('.mode-btn').forEach(btn => btn.classList.toggle('active', String(btn.dataset.mode || '').toUpperCase() === m));
    updateRunningInfo();
}

function sendMode(mode) {
    const m = String(mode || '').toUpperCase();
    const msgId = makeMsgId('UI_MODE');
    trangThai.cheDoChoAck[msgId] = m;
    setModeUi(m);
    send('mode_request', { msg_id: msgId, gateway_id: getGatewayId(), control_id: getGatewayId(), ma_tu: getGatewayId(), mode: m, ack_req: true });
}

function sendRelay(relay, value) {
    const msgId = makeMsgId(`UI_RELAY_${relay}`);
    trangThai.relayDangCho[relay] = { msgId, state: value ? 1 : 0, oldState: trangThai.relay[relay - 1] ? 1 : 0, start: Date.now() };
    updateRelayUi();
    send('relay_request', { msg_id: msgId, gateway_id: getGatewayId(), control_id: getGatewayId(), ma_tu: getGatewayId(), relay, state: value ? 1 : 0, ack_req: true });
}

function kiemTraLenhRelayQuaHan() {
    const now = Date.now();
    let changed = false;
    Object.keys(trangThai.relayDangCho || {}).forEach(key => {
        const p = trangThai.relayDangCho[key];
        if (!p || !p.start) return;
        if (now - p.start > RELAY_ACK_TIMEOUT_MS) {
            const relay = Number(key);
            if (relay >= 1 && relay <= 10) {
                trangThai.relay[relay - 1] = p.oldState ? 1 : 0;
            }
            delete trangThai.relayDangCho[key];
            changed = true;
        }
    });
    if (changed) {
        updateRelayUi();
        showToast('Chưa nhận được phản hồi từ tủ. Nút điều khiển đã trở về trạng thái trước đó.', 'warn');
    }
}

function handleAreaForm(ev) {
    ev.preventDefault();
    send('area_create_request', {
        username: trangThai.tenDangNhap,
        area_name: $('areaName').value.trim(),
        ten_khu_vuc: $('areaName').value.trim(),
        note: $('areaNote')?.value?.trim() || '',
    });
}

function handleControlUnitForm(ev) {
    ev.preventDefault();
    const maTu = $('controlUnitId')?.value?.trim() || getGatewayId();
    if ($('gatewayId')) $('gatewayId').value = maTu;
    send('control_unit_create_request', {
        username: trangThai.tenDangNhap,
        area_id: $('controlAreaSelect')?.value || trangThai.khuVucDangChon,
        khu_vuc_id: $('controlAreaSelect')?.value || trangThai.khuVucDangChon,
        control_name: $('controlUnitName')?.value?.trim() || maTu,
        ten_tu: $('controlUnitName')?.value?.trim() || maTu,
        control_id: maTu,
        ma_tu: maTu,
        gateway_id: maTu,
        device_id: maTu,
        gateway_device_id: maTu,
        stm_id: maTu,
        device_key: $('controlDeviceKey')?.value?.trim() || '',
    });
}

function handleSensorDeviceForm(ev) {
    ev.preventDefault();
    const maTu = $('sensorControlSelect')?.value || getGatewayId();
    const id = $('sensorDeviceIdInput').value.trim();
    send('sensor_device_add_request', {
        username: trangThai.tenDangNhap,
        area_id: $('sensorAreaSelect')?.value || trangThai.khuVucDangChon,
        khu_vuc_id: $('sensorAreaSelect')?.value || trangThai.khuVucDangChon,
        control_id: maTu,
        gateway_id: maTu,
        ma_tu: maTu,
        device_id: id,
        id_device: id,
        sensor_device_id: id,
        ma_cam_bien: id,
        device_name: $('sensorDeviceNameInput').value.trim() || id,
        ten_cam_bien: $('sensorDeviceNameInput').value.trim() || id,
        fields: ['temperature', 'humidity', 'co2', 'light', 'voltage', 'current', 'frequency', 'power'],
    });
}

function handleTimerForm(ev) {
    ev.preventDefault();
    const relayEl = $('timerRelay'), indexEl = $('timerIndex');
    if (!relayEl || !indexEl) { showToast('Không tìm thấy ô nhập lịch. Hãy tải lại trang.', 'err'); return; }
    const payload = {
        msg_id: makeMsgId('UI_TIMER'),
        gateway_id: getGatewayId(),
        control_id: getGatewayId(),
        relay: intVal(relayEl.value, 1),
        index: intVal(indexEl.value, 1),
        enable: intVal($('timerEnable')?.value, 1),
        on: $('timerOn')?.value || '00:00',
        off: $('timerOff')?.value || '00:00',
        ack_req: true,
    };
    luuTimerDaCai(payload);
    luuCauHinhTuLocal();
    markConfigPending(payload.msg_id, { type: 'timer', relay: payload.relay, index: payload.index });
    setSaveStatus('timer', `Đang gửi lịch K${payload.relay} - lịch ${payload.index} xuống tủ...`, 'pending');
    send('timer_request', payload);
    showToast(`Đã gửi lịch K${payload.relay}, lịch ${payload.index}.`, 'ok');
}

function handleSensorRuleForm(ev) {
    ev.preventDefault();
    const relayEl = $('sensorRelay');
    if (!relayEl) { showToast('Không tìm thấy ô nhập điều kiện. Hãy tải lại trang.', 'err'); return; }
    const logic = $('sensorLogic')?.value || 'ABOVE';
    const payload = {
        msg_id: makeMsgId('UI_SENSOR_RULE'), gateway_id: getGatewayId(), control_id: getGatewayId(), relay: intVal(relayEl.value, 1), enable: intVal($('sensorEnable')?.value, 1), id_device: $('sensorDeviceSelect')?.value || trangThai.camBienDangChon, field: $('sensorField')?.value || 'temperature', logic, ack_req: true,
    };
    if (!payload.id_device) { showToast('Chưa chọn thiết bị cảm biến.', 'warn'); return; }
    if (logic === 'ABOVE') {
        payload.onAbove = floatVal($('sensorOnValue')?.value, 0);
        payload.offBelow = floatVal($('sensorOffValue')?.value, payload.onAbove - 5);
    } else {
        payload.onBelow = floatVal($('sensorOnValue')?.value, 0);
        payload.offAbove = floatVal($('sensorOffValue')?.value, payload.onBelow + 5);
    }
    luuSensorRuleDaCai({ ...payload, onValue: payload.onAbove ?? payload.onBelow, offValue: payload.offBelow ?? payload.offAbove });
    luuCauHinhTuLocal();
    markConfigPending(payload.msg_id, { type: 'sensor', relay: payload.relay });
    setSaveStatus('sensor', `Đang gửi điều kiện K${payload.relay} xuống tủ...`, 'pending');
    send('sensor_rule_request', payload);
    showToast('Đã gửi điều kiện tự động. Đang chờ tủ xác nhận.', 'ok');
    setTimeout(requestStatus, 1200);
    setTimeout(requestStatus, 2500);
}

function requestStatus() {
    send('status_request', { gateway_id: getGatewayId(), control_id: getGatewayId() });
}

function requestFullState() {
    send('full_state_request', { gateway_id: getGatewayId(), control_id: getGatewayId() });
}

function sendFan() {
    send('fan_request', { msg_id: makeMsgId('UI_FAN'), gateway_id: getGatewayId(), control_id: getGatewayId(), fanMode: intVal($('fanModeInput').value, 1), ack_req: true });
}

function sendFanThreshold() {
    send('fan_threshold_request', { msg_id: makeMsgId('UI_FAN_TH'), gateway_id: getGatewayId(), control_id: getGatewayId(), fanOnTemp: floatVal($('fanOnTempInput').value, 35), fanOffTemp: floatVal($('fanOffTempInput').value, 30), ack_req: true });
}

function handleRawCmd() {
    try {
        const obj = JSON.parse($('rawCmdInput').value);
        obj.gateway_id = obj.gateway_id || getGatewayId();
        obj.control_id = obj.control_id || getGatewayId();
        obj.msg_id = obj.msg_id || makeMsgId('UI_RAW');
        send('stm32_cmd_request', obj);
    } catch (err) {
        showToast('JSON không hợp lệ: ' + err.message, 'err');
    }
}

function handleHistoryForm(ev) {
    ev.preventDefault();
    send('history_request', { username: trangThai.tenDangNhap, device_id: $('historyDeviceSelect').value || trangThai.camBienDangChon, type: $('historyType').value, filter: $('historyFilter').value });
}

function todayLocalDate() {
    const d = new Date(); const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

function initUsageDates() {
    const today = todayLocalDate();
    if ($('usageDate') && !$('usageDate').value) $('usageDate').value = today;
    if ($('usageMonth') && !$('usageMonth').value) $('usageMonth').value = today.slice(0, 7);
    if ($('usageYear') && !$('usageYear').value) $('usageYear').value = today.slice(0, 4);
    updateUsageDateInputs();
}

function updateUsageDateInputs() {
    const period = $('usagePeriod')?.value || 'day';
    $('usageDateWrap')?.classList.toggle('hidden', period !== 'day');
    $('usageMonthWrap')?.classList.toggle('hidden', period !== 'month');
    $('usageYearWrap')?.classList.toggle('hidden', period !== 'year');
}

function handleUsageForm(ev) {
    ev.preventDefault();
    const period = $('usagePeriod').value;
    let date = $('usageDate').value || todayLocalDate();
    if (period === 'month') date = $('usageMonth').value || todayLocalDate().slice(0, 7);
    if (period === 'year') date = $('usageYear').value || todayLocalDate().slice(0, 4);
    send('usage_request', { username: trangThai.tenDangNhap, gateway_id: getGatewayId(), control_id: getGatewayId(), device_id: $('usageDeviceSelect').value || trangThai.camBienDangChon, metric: $('usageMetric').value, period, date, sample_sec: intVal($('usageSampleSec').value, 60), max_gap_sec: intVal($('usageMaxGapSec').value, 900) });
}

function handleIncoming(msg) {
    if (!msg || typeof msg !== 'object') return;
    ghiLog(`Node-RED → UI: ${msg.topic || '(không topic)'}`, msg.payload);
    const topic = msg.topic;
    const p = normalizePayload(msg.payload);

    switch (topic) {
        case 'auth_response': handleAuthResponse(p); break;
        case 'room_list_response':
        case 'area_tree_response':
            napDanhSachKhuVucTu(p); break;
        case 'room_devices_list_response':
        case 'sensor_devices_list_response':
            napDanhSachCamBien(p); break;
        case 'area_create_response':
        case 'control_unit_create_response':
        case 'room_create_response':
        case 'sensor_device_add_response':
        case 'room_device_add_response':
            showToast(p?.message || 'Đã cập nhật dữ liệu.', p?.status === 'error' ? 'err' : 'ok'); requestDanhSachKhuVucTu(); break;
        case 'room_delete_response':
            showToast(p?.message || 'Đã xóa.', p?.status === 'error' ? 'err' : 'ok'); requestDanhSachKhuVucTu(); break;
        case 'sensor_data':
        case 'nong_trai/sensors':
        case 'gateway_sensor': updateSensor(p || {}); break;
        case 'history_response': trangThai.lichSu = Array.isArray(p) ? p : []; renderHistory(); break;
        case 'usage_response': trangThai.thongKe = p || null; renderUsage(); break;
        case 'gateway_status': updateGatewayStatus(p || {}); break;
        case 'gateway_ack':
        case 'stm32_ack': handleAck(p || {}); break;
        case 'stm32_status':
        case 'config_state':
        case 'config_sync_response':
        case 'full_state':
        case 'relay_state': updateStmState(topic, p || {}); break;
        case 'fan_state': updateFanState(p || {}); break;
        case 'stm32_boot': showToast('Tủ điều khiển vừa khởi động lại.', 'warn'); break;
        case 'gateway_device_request': showToast('Tủ điều khiển vừa hỏi danh sách cảm biến.', 'warn'); break;
        case 'sync_request_from_stm32': showToast('Tủ đang kiểm tra lại cài đặt đã lưu.', 'warn'); break;
        default: break;
    }
}

function updateSensor(p) {
    const base = (p && typeof p.data === 'object' && p.data !== null) ? { ...p.data, ...p } : (p || {});
    const src = normalizePayload(base.payload || base);
    const did = String(src.device_id || src.id_device || src.idDevice || src.id || '').trim();
    if (did) trangThai.duLieuTheoCamBien[did] = src;

    const dsTrongTu = camBienTheoTu();
    if (!trangThai.camBienDangChon) {
        const first = dsTrongTu[0] || trangThai.danhSachCamBien[0] || (did ? { id: did } : null);
        if (first?.id) trangThai.camBienDangChon = first.id;
    }

    const belongsToCurrentCabinet = !trangThai.tuDangChon || camBienThuocTuDangChon(did) || !dsTrongTu.length;
    const isSelectedSensor = did && String(did) === String(trangThai.camBienDangChon);

    if (belongsToCurrentCabinet && isSelectedSensor) {
        trangThai.duLieuMoiNhat = src;
        hienThiDuLieuCamBien(src);
    } else if (!layDuLieuCamBienDangChon()) {
        hienThiDuLieuCamBien(null);
    }

    renderDeviceSelects();
    updateSelectedDisplay();
    renderThietBiHeThong();
}

function updateGatewayStatus(p) {
    trangThai.trangThaiTuMoiNhat = p || {};
    setBadge(String(p.state || '').toUpperCase() === 'ONLINE');
    renderThietBiHeThong();
}

function handleAck(p) {
    const ok = String(p.status || '').toUpperCase() === 'OK' || String(p.status || '').toUpperCase() === 'SUCCESS';
    const timeout = String(p.status || '').toUpperCase() === 'TIMEOUT';
    const ackFor = String(p.ack_for || '');
    if (ok && trangThai.cheDoChoAck[ackFor]) { setModeUi(p.mode || trangThai.cheDoChoAck[ackFor]); delete trangThai.cheDoChoAck[ackFor]; }
    if (/^UI_TIMER_/.test(ackFor) || /^UI_SENSOR_RULE_/.test(ackFor)) {
        clearConfigPending(ackFor, ok, p.message || p.error_code || '');
        if (ok) { setTimeout(requestConfigSync, 400); setTimeout(requestStatus, 800); }
    }

    const relayMatch = ackFor.match(/^UI_RELAY_(\d+)_/);
    if (relayMatch) {
        const relay = Number(relayMatch[1]);
        const pending = trangThai.relayDangCho[relay];
        if (ok && pending) {
            trangThai.relay[relay - 1] = pending.state ? 1 : 0;
            trangThai.relayChiTiet[relay - 1] = { ...(trangThai.relayChiTiet[relay - 1] || {}), source: 'MANUAL' };
        } else if (pending) {
            trangThai.relay[relay - 1] = pending.oldState ? 1 : 0;
        }
        delete trangThai.relayDangCho[relay];
        updateRelayUi();
    }

    showToast(`${p.status || 'ACK'}: ${p.message || p.ack_for || ''}`, ok ? 'ok' : timeout ? 'warn' : 'err');
}

function updateStmState(topic, p) {
    capNhatCauHinhDaLuu(p);
    const mode = p.mode || p.workMode || p.currentMode || p.data?.mode;
    if (mode) setModeUi(mode);
    if (p.cfgVersion !== undefined || p.cfg_version !== undefined) {
        trangThai.phienBanCauHinh = p.cfgVersion ?? p.cfg_version;
        if ($('cfgVersionState')) $('cfgVersionState').textContent = trangThai.phienBanCauHinh;
    }
    const cabinetTemp = p.cabinetTemp ?? p.cabinet_temp ?? p.tempCabinet ?? p.lm35;
    if (cabinetTemp !== undefined && $('cabinetTempState')) $('cabinetTempState').textContent = `${num(cabinetTemp)} °C`;
    if (p.emergency !== undefined && $('emergencyState')) $('emergencyState').textContent = truthy(p.emergency) ? 'ĐANG KÍCH HOẠT' : 'OK';
    parseRelayState(p);
    updateFanState(p);
}

function parseRelayState(p) {
    const applyRelayArray = arr => arr.forEach((v, idx) => {
        if (idx >= 10) return;
        const state = parseRelayValue(v);
        trangThai.relay[idx] = state;
        if (!state) trangThai.relayChiTiet[idx] = { ...(trangThai.relayChiTiet[idx] || {}), state: 0, source: '' };
    });
    if (Array.isArray(p.relays)) applyRelayArray(p.relays);
    if (Array.isArray(p.relayState)) applyRelayArray(p.relayState);
    if (Array.isArray(p.relay_state)) applyRelayArray(p.relay_state);

    const detailArray = p.relay_details || p.relay_meta || p.relayDetails || p.relay_status;
    if (Array.isArray(detailArray)) {
        detailArray.forEach(item => {
            const idx = intVal(item.relay, 0) - 1;
            if (idx >= 0 && idx < 10) {
                trangThai.relayChiTiet[idx] = { ...(trangThai.relayChiTiet[idx] || {}), ...item };
                if (item.state !== undefined) trangThai.relay[idx] = parseRelayValue(item.state);
            }
        });
    }

    const activeArray = p.active_relays || p.activeRelays || p.running_relays;
    if (Array.isArray(activeArray)) {
        activeArray.forEach(item => {
            const idx = intVal(item.relay, 0) - 1;
            if (idx >= 0 && idx < 10) {
                trangThai.relay[idx] = 1;
                trangThai.relayChiTiet[idx] = { ...(trangThai.relayChiTiet[idx] || {}), ...item, state: 1 };
            }
        });
    }

    if (p.relay !== undefined && p.state !== undefined) {
        const idx = intVal(p.relay, 1) - 1;
        if (idx >= 0 && idx < 10) {
            trangThai.relay[idx] = parseRelayValue(p.state);
            trangThai.relayChiTiet[idx] = { ...(trangThai.relayChiTiet[idx] || {}), ...p };
            const pending = trangThai.relayDangCho[idx + 1];
            if (pending && pending.state === trangThai.relay[idx]) delete trangThai.relayDangCho[idx + 1];
        }
    }
    if (p.data && p.data.relay !== undefined && p.data.state !== undefined) {
        const idx = intVal(p.data.relay, 1) - 1;
        if (idx >= 0 && idx < 10) {
            trangThai.relay[idx] = parseRelayValue(p.data.state);
            trangThai.relayChiTiet[idx] = { ...(trangThai.relayChiTiet[idx] || {}), ...p.data };
            const pending = trangThai.relayDangCho[idx + 1];
            if (pending && pending.state === trangThai.relay[idx]) delete trangThai.relayDangCho[idx + 1];
        }
    }
    updateRelayUi();
}

function parseRelayValue(v) {
    if (typeof v === 'object' && v !== null) return parseRelayValue(v.state ?? v.value ?? v.on);
    return truthy(v) ? 1 : 0;
}

function truthy(v) {
    return v === true || v === 1 || v === '1' || String(v).toUpperCase() === 'ON' || String(v).toUpperCase() === 'TRUE' || String(v).toUpperCase() === 'ACTIVE';
}

function updateFanState(p) {
    if (!p || typeof p !== 'object') return;
    const fanMode = p.fanMode ?? p.fan_mode ?? p.fanModeRuntime ?? p.data?.fanMode;
    const fan = p.fan ?? p.fanRunning ?? p.fan_on ?? p.state;
    const fanText = fanMode !== undefined ? ['Tắt', 'Tự động', 'Bật liên tục'][Number(fanMode)] || String(fanMode) : fan !== undefined ? (truthy(fan) ? 'Bật' : 'Tắt') : null;
    if (fanText && $('fanState')) $('fanState').textContent = fanText;
    const cab = p.cabinetTemp ?? p.cabinet_temp;
    if (cab !== undefined && $('cabinetTempState')) $('cabinetTempState').textContent = `${num(cab)} °C`;
    if (p.fanOnTemp !== undefined && $('fanOnTempInput')) $('fanOnTempInput').value = p.fanOnTemp;
    if (p.fanOffTemp !== undefined && $('fanOffTempInput')) $('fanOffTempInput').value = p.fanOffTemp;
    if (fanMode !== undefined && $('fanModeInput')) $('fanModeInput').value = String(fanMode);
    capNhatCauHinhDaLuu({ fanMode, fanOnTemp: p.fanOnTemp, fanOffTemp: p.fanOffTemp });
}


function metricUnit(metric) {
    return { energy_kwh: 'kWh', temperature: '°C', humidity: '%', co2: 'ppm', light: 'lux', voltage: 'V', current: 'A', frequency: 'Hz', power: 'W' }[metric] || '';
}

function renderUsage() {
    const data = trangThai.thongKe || {};
    const rows = Array.isArray(data.rows) ? data.rows : [];
    const summary = data.summary || {};
    const metric = data.metric || $('usageMetric')?.value || 'energy_kwh';
    const unit = data.unit || summary.unit || metricUnit(metric);
    const isEnergy = metric === 'energy_kwh';
    if ($('usageTotal')) $('usageTotal').textContent = isEnergy ? `${num(summary.total_kwh ?? summary.total ?? 0, 3)} ${unit}` : '--';
    if ($('usageAvg')) $('usageAvg').textContent = summary.avg !== undefined ? `${num(summary.avg, isEnergy ? 3 : 2)} ${unit}` : '--';
    if ($('usageMin')) $('usageMin').textContent = summary.min !== undefined ? `${num(summary.min, isEnergy ? 3 : 2)} ${unit}` : '--';
    if ($('usageMax')) $('usageMax').textContent = summary.max !== undefined ? `${num(summary.max, isEnergy ? 3 : 2)} ${unit}` : '--';
    if ($('usageCount')) $('usageCount').textContent = summary.count ?? rows.reduce((s, r) => s + (Number(r.count) || 0), 0) ?? '--';
    drawSimpleChart('usageCanvas', rows, unit);
    const box = $('usageTable');
    if (!box) return;
    if (!rows.length) { box.innerHTML = '<p class="muted">Không có dữ liệu trong khoảng đã chọn.</p>'; return; }
    box.innerHTML = `<table><thead><tr><th>Mốc thời gian</th><th>Giá trị</th><th>Min</th><th>Max</th><th>Số mẫu</th></tr></thead><tbody>${rows.map(r => `<tr><td>${escapeHtml(r.time_label ?? '')}</td><td>${escapeHtml(num(r.value, isEnergy ? 4 : 2))} ${escapeHtml(unit)}</td><td>${r.min !== undefined ? escapeHtml(num(r.min, 2)) : '--'}</td><td>${r.max !== undefined ? escapeHtml(num(r.max, 2)) : '--'}</td><td>${escapeHtml(r.count ?? '')}</td></tr>`).join('')}</tbody></table>`;
}

function renderHistory() {
    const data = trangThai.lichSu || [];
    drawSimpleChart('historyCanvas', data, '');
    const box = $('historyTable');
    if (!box) return;
    if (!data.length) { box.innerHTML = '<p class="muted">Không có dữ liệu.</p>'; return; }
    box.innerHTML = `<table><thead><tr><th>Thời gian</th><th>Giá trị</th></tr></thead><tbody>${data.map(r => `<tr><td>${escapeHtml(r.time_label ?? r.time ?? '')}</td><td>${escapeHtml(num(r.value, 2))}</td></tr>`).join('')}</tbody></table>`;
}

function drawSimpleChart(canvasId, data, unit = '') {
    const canvas = $(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const ratio = window.devicePixelRatio || 1;
    const width = canvas.clientWidth || 700;
    const height = 260;
    canvas.width = width * ratio; canvas.height = height * ratio;
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#ffffff'; ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = '#dbe9e4'; ctx.lineWidth = 1;
    for (let i = 0; i < 5; i++) { const y = 24 + i * ((height - 54) / 4); ctx.beginPath(); ctx.moveTo(44, y); ctx.lineTo(width - 16, y); ctx.stroke(); }
    if (!Array.isArray(data) || data.length === 0) { ctx.fillStyle = '#687972'; ctx.font = '14px system-ui'; ctx.fillText('Không có dữ liệu', 24, 40); return; }
    const values = data.map(r => Number(r.value)).filter(Number.isFinite);
    if (!values.length) return;
    let min = Math.min(...values), max = Math.max(...values);
    if (min === max) { min -= 1; max += 1; }
    const left = 44, right = width - 16, top = 20, bottom = height - 34;
    const xFor = i => left + (i / Math.max(1, data.length - 1)) * (right - left);
    const yFor = v => bottom - ((v - min) / (max - min)) * (bottom - top);
    ctx.strokeStyle = '#0f766e'; ctx.lineWidth = 3; ctx.beginPath();
    data.forEach((r, i) => { const v = Number(r.value); if (!Number.isFinite(v)) return; const x = xFor(i), y = yFor(v); if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y); });
    ctx.stroke();
    ctx.fillStyle = '#0f766e'; data.forEach((r, i) => { const v = Number(r.value); if (!Number.isFinite(v)) return; ctx.beginPath(); ctx.arc(xFor(i), yFor(v), 4, 0, Math.PI * 2); ctx.fill(); });
    ctx.fillStyle = '#687972'; ctx.font = '12px system-ui';
    ctx.fillText(`${num(max, 2)} ${unit}`, 6, top + 5); ctx.fillText(`${num(min, 2)} ${unit}`, 6, bottom + 4);
    ctx.fillText(String(data[0].time_label || ''), left, height - 10); ctx.textAlign = 'right'; ctx.fillText(String(data[data.length - 1].time_label || ''), right, height - 10); ctx.textAlign = 'left';
}

function changePage(id) {
    $$('.page').forEach(p => p.classList.toggle('active', p.id === id));
    $$('.nav-btn').forEach(b => b.classList.toggle('active', b.dataset.page === id));
    const [title, sub] = thongTinTrang[id] || thongTinTrang.pageOverview;
    if ($('pageTitle')) $('pageTitle').textContent = title;
    if ($('pageSubtitle')) $('pageSubtitle').textContent = sub;
    if (id === 'pageDev') updateDevView();
}

function updateDevView() {
    const unlocked = !!trangThai.daMoKyThuat;
    $('devLoginPanel')?.classList.toggle('hidden', unlocked);
    $('devToolsContent')?.classList.toggle('hidden', !unlocked);
}

function handleDevLogin(ev) {
    ev.preventDefault();
    const u = $('devUsername')?.value?.trim() || '';
    const p = $('devPassword')?.value || '';
    if (u === 'dev_fuvitech' && p === 'fuvitech2026') {
        trangThai.daMoKyThuat = true;
        sessionStorage.setItem('nongtrai_dev_unlocked', '1');
        if ($('devLoginStatus')) $('devLoginStatus').textContent = 'Đã mở nhật ký kỹ thuật.';
        showToast('Đã đăng nhập kỹ thuật.', 'ok');
        updateDevView();
    } else {
        if ($('devLoginStatus')) $('devLoginStatus').textContent = 'Sai tài khoản hoặc mật khẩu kỹ thuật.';
        showToast('Sai tài khoản hoặc mật khẩu kỹ thuật.', 'err');
    }
}

function attachEvents() {
    $('loginTab')?.addEventListener('click', () => setAuthMode('login'));
    $('registerTab')?.addEventListener('click', () => setAuthMode('register'));
    $('authForm')?.addEventListener('submit', handleAuthSubmit);
    $('logoutBtn')?.addEventListener('click', logout);
    $('refreshRoomsBtn')?.addEventListener('click', requestDanhSachKhuVucTu);
    $('getStatusBtn')?.addEventListener('click', requestStatus);
    $('getFullStateTopBtn')?.addEventListener('click', requestFullState);
    $('syncConfigBtn')?.addEventListener('click', requestConfigSync);
    $('reloadConfigFromTimerBtn')?.addEventListener('click', requestConfigSync);
    $('reloadConfigFromSensorBtn')?.addEventListener('click', requestConfigSync);
    $('deviceListCmdBtn')?.addEventListener('click', () => send('device_list_cmd_request', { gateway_id: getGatewayId(), control_id: getGatewayId() }));
    $('areaForm')?.addEventListener('submit', handleAreaForm);
    $('controlUnitForm')?.addEventListener('submit', handleControlUnitForm);
    $('sensorDeviceForm')?.addEventListener('submit', handleSensorDeviceForm);
    $('timerForm')?.addEventListener('submit', handleTimerForm);
    $('sensorRuleForm')?.addEventListener('submit', handleSensorRuleForm);
    $('timerRelay')?.addEventListener('change', () => apDungCauHinhLichLenForm(true));
    $('timerIndex')?.addEventListener('change', () => apDungCauHinhLichLenForm(true));
    $('sensorRelay')?.addEventListener('change', () => apDungCauHinhCamBienLenForm(true));
    $('sendFanBtn')?.addEventListener('click', sendFan);
    $('sendFanThresholdBtn')?.addEventListener('click', sendFanThreshold);
    $('sendRawCmdBtn')?.addEventListener('click', handleRawCmd);
    $('historyForm')?.addEventListener('submit', handleHistoryForm);
    $('usageForm')?.addEventListener('submit', handleUsageForm);
    $('usagePeriod')?.addEventListener('change', updateUsageDateInputs);
    $('devLoginForm')?.addEventListener('submit', handleDevLogin);
    $('overviewAreaSelect')?.addEventListener('change', ev => chonKhuVuc(ev.target.value));
    $('controlAreaSelectMain')?.addEventListener('change', ev => chonKhuVuc(ev.target.value));
    $('overviewControlSelect')?.addEventListener('change', ev => chonTu(ev.target.value));
    $('overviewSensorSelect')?.addEventListener('change', ev => chonCamBien(ev.target.value));
    $('controlUnitSelectMain')?.addEventListener('change', ev => chonTu(ev.target.value));
    $('sensorAreaSelect')?.addEventListener('change', ev => { chonKhuVuc(ev.target.value); renderAreaControlSelects(); });
    $('sensorControlSelect')?.addEventListener('change', ev => chonTu(ev.target.value));
    $('clearLogsBtn')?.addEventListener('click', () => { trangThai.logHeThong = []; if ($('logBox')) $('logBox').textContent = ''; });
    $('allRelayOffBtn')?.addEventListener('click', () => { for (let i = 1; i <= 10; i++) sendRelay(i, 0); });
    $$('.nav-btn').forEach(btn => btn.addEventListener('click', () => changePage(btn.dataset.page)));
    $$('.mode-btn').forEach(btn => btn.addEventListener('click', () => sendMode(btn.dataset.mode)));
    $$('.quick-actions [data-quick]').forEach(btn => btn.addEventListener('click', () => send(btn.dataset.quick, { gateway_id: getGatewayId(), control_id: getGatewayId() })));
    $('sensorDeviceSelect')?.addEventListener('change', ev => chonCamBien(ev.target.value));
    $('historyDeviceSelect')?.addEventListener('change', ev => chonCamBien(ev.target.value));
    $('usageDeviceSelect')?.addEventListener('change', ev => chonCamBien(ev.target.value));
    $('timerSavedList')?.addEventListener('click', ev => { const btn = ev.target.closest('[data-load-timer]'); if (!btn) return; const [relay, index] = String(btn.dataset.loadTimer || '').split(':'); loadTimerToForm(relay, index); });
    $('sensorRuleSavedList')?.addEventListener('click', ev => { const btn = ev.target.closest('[data-load-sensor-rule]'); if (!btn) return; loadSensorRuleToForm(btn.dataset.loadSensorRule); });
    window.addEventListener('resize', () => { drawSimpleChart('historyCanvas', trangThai.lichSu); if (trangThai.thongKe) drawSimpleChart('usageCanvas', trangThai.thongKe.rows || [], trangThai.thongKe.unit || ''); });
}

function startUibuilder() {
    if (!window.uibuilder) {
        $('uibuilderStatus')?.classList.remove('hidden');
        setBadge(false);
        ghiLog('Không tìm thấy uibuilder client. Kiểm tra đường dẫn script ../uibuilder/uibuilder.iife.min.js');
        return;
    }
    try {
        if (typeof uibuilder.start === 'function') uibuilder.start();
        trangThai.uibReady = true;
        $('uibuilderStatus')?.classList.add('hidden');
        setBadge(true);
    } catch (err) { ghiLog('Lỗi start uibuilder', err.message || err); }
    if (typeof uibuilder.onChange === 'function') {
        uibuilder.onChange('msg', handleIncoming);
        uibuilder.onChange('ioConnected', val => setBadge(!!val));
    } else if (typeof uibuilder.on === 'function') {
        uibuilder.on('msg', handleIncoming);
        uibuilder.on('io connected', () => setBadge(true));
        uibuilder.on('io disconnected', () => setBadge(false));
    }
}

function init() {
    attachEvents();
    setAuthMode('login');
    buildRelayControls();
    buildRelayMiniGrid();
    napCauHinhTuLocal();
    renderTatCaDanhSach();
    apDungCauHinhLenForm(false);
    renderCauHinhDaLuu();
    initUsageDates();
    startUibuilder();
    updateDevView();
    if (trangThai.tenDangNhap) showApp();
    else showAuth();
    setInterval(kiemTraLenhRelayQuaHan, 1000);
    setInterval(kiemTraLenhCaiDatQuaHan, 1000);
}

document.addEventListener('DOMContentLoaded', init);
