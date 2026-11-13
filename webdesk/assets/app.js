(function () {
  'use strict';

/**
 * LING OS Web 桌面（生产前端，无构建）
 *
 * 结构：
 *   1. 基础工具   图标 / 转义 / DOM 选择器 / 时间格式化 / toast / 确认条
 *   2. 网络层     api() 统一 fetch：8s 超时、401 回登录、错误不抛给调用方
 *   3. 状态与启动  S 全局状态、health 探测、err-noserver、登录/初始化
 *   4. 布局       竖屏（手机）与横屏（电脑）自动切换，body[data-layout]
 *   5. 七个视图   家 / 设备 / 文件 / 安全 / AI / 系统 / 设置
 *   6. 事件绑定   一个 document 级委托 + 少量表单提交
 *
 * 约束：
 *   - 没有后端就显示 err-noserver，绝不使用模拟数据。
 *   - 命令服务（18080）未启动时如实报错，不伪造状态。
 *   - 摄像头预览默认关，没画面就不占位。
 *   - 危险操作（开锁、切等级、删用户/设备、恢复备份）一律二次确认。
 */


  /* ============ 基础工具 ============ */
  var P = {
    home: '<path d="M3.5 10.5 12 3.5l8.5 7"/><path d="M5.5 9.5V20h13V9.5"/><path d="M10 20v-5h4v5"/>',
    grid: '<rect x="3.5" y="3.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.5"/><rect x="3.5" y="13.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.5"/>',
    chip: '<rect x="6.5" y="6.5" width="11" height="11" rx="2"/><path d="M9.5 3v3.5M14.5 3v3.5M9.5 17.5V21M14.5 17.5V21M3 9.5h3.5M3 14.5h3.5M17.5 9.5H21M17.5 14.5H21"/>',
    chat: '<path d="M20.5 11.6a8.2 8.2 0 0 1-8.2 8.2H7l-3.5 2.2 1.2-4.6A8.2 8.2 0 1 1 20.5 11.6z"/>',
    shield: '<path d="M12 3 4.5 6v5.6c0 4.3 3.2 7.5 7.5 9.4 4.3-1.9 7.5-5.1 7.5-9.4V6L12 3z"/><path d="m8.8 12.2 2.2 2.2 4.2-4.4"/>',
    folder: '<path d="M3.5 6.5h6l2 2h9v11h-17z"/>',
    settings: '<circle cx="12" cy="12" r="2.6"/><path d="M12 3.5v2.2M12 18.3v2.2M3.5 12h2.2M18.3 12h2.2M6 6l1.6 1.6M16.4 16.4 18 18M18 6l-1.6 1.6M7.6 16.4 6 18"/>',
    lock: '<rect x="5" y="10" width="14" height="10.5" rx="2.5"/><path d="M8 10V7.2a4 4 0 0 1 8 0V10"/><circle cx="12" cy="15.2" r="1.3"/>',
    lamp: '<path d="M9 19h6M10 21.5h4"/><path d="M8.2 15.5a6 6 0 1 1 7.6 0c-1 1-1 2-1 4h-5.6c0-2 0-3-1-4z"/>',
    sensor: '<circle cx="12" cy="12" r="1.8"/><path d="M7.4 7.4a6.5 6.5 0 0 0 0 9.2M16.6 7.4a6.5 6.5 0 0 1 0 9.2M4.6 4.6a10.5 10.5 0 0 0 0 14.8M19.4 4.6a10.5 10.5 0 0 1 0 14.8"/>',
    camera: '<path d="M3.5 7.5h4l2-2h5l2 2h4v13h-17z"/><circle cx="12" cy="13.5" r="3.2"/>',
    switch: '<rect x="3.5" y="7" width="17" height="10" rx="5"/><circle cx="16.5" cy="12" r="2.4"/>',
    moon: '<path d="M20 15.4A8.6 8.6 0 0 1 8.6 4 8.6 8.6 0 1 0 20 15.4z"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2.2M12 19.8V22M2 12h2.2M19.8 12H22M5 5l1.6 1.6M17.4 17.4 19 19M19 5l-1.6 1.6M6.6 17.4 5 19"/>',
    refresh: '<path d="M20 12a8 8 0 1 1-2.4-5.6"/><path d="M20 4v4.5h-4.5"/>',
    check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
    alert: '<path d="M12 4 2.8 20h18.4L12 4z"/><path d="M12 10v4.2M12 17.2h.01"/>',
    trash: '<path d="M4 7h16M9 7V4.5h6V7M6.5 7l1 13h9l1-13"/>',
    download: '<path d="M12 4v12"/><path d="m7 11 5 5 5-5"/><path d="M4.5 20h15"/>',
    upload: '<path d="M12 20V8"/><path d="m7 13 5-5 5 5"/><path d="M4.5 4h15"/>',
    file: '<path d="M6 3.5h8l4 4v13H6z"/><path d="M14 3.5v4h4"/>',
    back: '<path d="m14 5-7 7 7 7"/>',
    plus: '<path d="M12 5v14M5 12h14"/>',
    person: '<circle cx="12" cy="8" r="3.6"/><path d="M4.5 20.5a7.5 7.5 0 0 1 15 0"/>',
    send: '<path d="M4 12 20.5 4l-7.4 16.5-2.2-7.1L4 12z"/>',
    dots: '<circle cx="5" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="19" cy="12" r="1.6"/>'
  };
  var FILLED = { dots: true };

  /** 统一线性图标：全站只此一套，避免图标风格混杂。 */
  function icon(name, size) {
    var s = size || 18;
    var body = P[name] || P.grid;
    if (FILLED[name]) return '<svg width="' + s + '" height="' + s + '" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">' + body + '</svg>';
    return '<svg width="' + s + '" height="' + s + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + body + '</svg>';
  }

  /** HTML 转义：所有来自后端的字符串在拼进 innerHTML 前都要过这里。 */
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  /** 单个 / 多个元素查询的短名字，减少重复样板。 */
  function $(sel, root) { return (root || document).querySelector(sel); }
  function $all(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }
  /** 把 index.html 里 data-icon 的占位 span 填成 SVG（启动时和局部重建后调用）。 */
  function paintIcons(root) {
    $all('[data-icon]', root || document).forEach(function (el) {
      el.innerHTML = icon(el.getAttribute('data-icon'), Number(el.getAttribute('data-icon-size')) || 20);
    });
  }

  var KIND = { auth: '登录', device: '设备', user: '用户', security: '安全', settings: '设置', fs: '文件', acl: '权限' };
  function kindName(k) { return KIND[k] || k || '事件'; }

  function fmtTime(iso) {
    if (!iso) return '—';
    var d = new Date(iso);
    if (isNaN(d.getTime())) return String(iso);
    return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
  }
  function fmtSize(n) {
    if (n == null) return '';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1024 / 1024).toFixed(1) + ' MB';
  }

  var toastTimer = null;
  /** 轻提示：2.4s 自动消失，同一时间只留一条。 */
  function toast(msg) {
    var n = $('#toast');
    if (!n) return;
    n.textContent = msg;
    n.classList.add('is-show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { n.classList.remove('is-show'); }, 2400);
  }

  /* 全局确认条 */
  var confirmCb = null;
  /** 底部确认条：危险动作统一入口，焦点默认落在「算了」上。 */
  function confirmBox(opts) {
    var box = $('#confirm');
    if (!box) return;
    box.querySelector('[data-confirm-title]').textContent = opts.title || '确认一下';
    box.querySelector('[data-confirm-body]').textContent = opts.body || '';
    box.querySelector('[data-confirm-ok]').textContent = opts.okText || '确定';
    confirmCb = opts.onOk || null;
    box.classList.add('is-open');
    var cancel = box.querySelector('[data-confirm-cancel]');
    if (cancel) setTimeout(function () { cancel.focus(); }, 40);
  }
  /** 关闭确认条并归还焦点，避免键盘用户丢失位置。 */
  function closeConfirm() {
    var box = $('#confirm');
    if (box) box.classList.remove('is-open');
    confirmCb = null;
  }

  /* ============ 网络 ============ */
  var apiBase = localStorage.getItem('lo-api') || '';

  /**
   * 统一请求封装。
   * - 8 秒超时（命令代理单独放宽），避免后台挂起时按钮一直转。
   * - 任意业务接口 401 时清会话并回登录页；登录/初始化接口的 401 交给表单自己显示。
   * - 永远 resolve 成 {ok,status,data}，调用方只关心结果，不写 try/catch。
   */
  function api(path, opts) {
    opts = opts || {};
    var init = {
      method: opts.method || 'GET',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' }
    };
    if (opts.headers) {
      Object.keys(opts.headers).forEach(function (k) { init.headers[k] = opts.headers[k]; });
    }
    if (opts.body !== undefined) init.body = JSON.stringify(opts.body);
    var ctrl = null;
    try { ctrl = new AbortController(); init.signal = ctrl.signal; } catch (e) {}
    var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, opts.timeout || 8000);
    return fetch(apiBase + path, init).then(function (res) {
      return res.text().then(function (text) {
        var data = null;
        try { data = text ? JSON.parse(text) : null; } catch (e) { data = { raw: text }; }
        return { ok: res.ok, status: res.status, data: data };
      });
    }).catch(function (err) {
      return { ok: false, status: 0, data: null, network: true, timeout: err && err.name === 'AbortError', error: err };
    }).then(function (res) {
      clearTimeout(timer);
      var loginCall = path.indexOf('/api/login') === 0 || path.indexOf('/api/setup') === 0 || path.indexOf('/api/me') === 0;
      if (res.status === 401 && !loginCall) {
        S.me = null;
        toast('会话过期，请重新登录');
        if (S.health) showAuth(false);
        else $('#auth').hidden = false;
      }
      return res;
    });
  }

  /* ============ 应用状态 ============ */
  var S = {
    health: null,
    me: null,
    devices: [],
    adapters: [],
    events: [],
    security: null,
    settings: {},
    view: 'home',
    loading: false,
    lastError: '',
    discovered: [],
    acl: null,
    selectedDev: null
  };

  /* 主题 */
  /** 主题只改根节点 data-theme + theme-color，令牌在 app.css 里成对定义。 */
  function applyTheme(t) {
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem('lo-theme', t); } catch (e) {}
    var meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = t === 'night' ? '#15180f' : '#f1ecdf';
    var btn = $('#themeBtn');
    if (btn) btn.innerHTML = icon(t === 'night' ? 'sun' : 'moon', 17);
  }
  function toggleTheme() {
    applyTheme(document.documentElement.dataset.theme === 'night' ? 'day' : 'night');
  }

  /* 摄像头预览：默认关闭，只有设置里打开才渲染，且必须真的有在线摄像头 */
  /** 摄像头预览开关存在浏览器本地：默认关，避免首页留一块没画面的空白。 */
  function previewOn() { return localStorage.getItem('lo-preview') === 'on'; }
  /** root key 只存本机浏览器，用于删除设备 / 写 ACL 这类高风险操作。 */
  function rootKey() { return localStorage.getItem('lo-rootkey') || ''; }
  /** 带 root key 的请求头；没有就返回空对象，让服务端决定拒绝。 */
  function rootHeaders() {
    var k = rootKey();
    return k ? { 'X-Lingos-Root-Key': k } : {};
  }

  /* ============ 启动流程 ============ */
  /** 启动/错误屏。code 直接显示，方便对日志：err-noserver 表示后台不可达。 */
  function showBoot(code, title, text, meta) {
    $('#boot').hidden = false;
    $('#bootCode').textContent = code;
    $('#bootTitle').textContent = title;
    $('#bootText').textContent = text || '';
    $('#bootMeta').textContent = meta || '';
    $('#auth').hidden = true;
    $('#app').hidden = true;
  }
  function hideBoot() { $('#boot').hidden = true; }

  /**
   * 启动顺序：先探 /api/health。
   * 失败 → err-noserver（不渲染任何业务界面）；成功 → 看 /api/me 决定登录还是进桌面。
   */
  function boot() {
    showBoot('正在连接 LING OS', '等后台回话', '正在检查同一地址下的 Web 服务。', '');
    api('/api/health').then(function (r) {
      if (r.network || !r.ok || !r.data || !r.data.ok) {
        var detail = r.network
          ? '没有收到响应。检查 lingos-web 是否在运行、地址是否正确。'
          : '后台返回 HTTP ' + r.status + '。检查 /api/health 是否可用。';
        showBoot('err-noserver', '连不上 LING OS 后台',
          '这个页面必须接后台才能用。没有后台时不显示任何模拟数据。',
          '地址：' + (apiBase || location.origin) + '\n' + detail);
        return;
      }
      S.health = r.data;
      hideBoot();
      api('/api/me').then(function (me) {
        if (me.ok && me.data) { S.me = me.data; enterApp(); }
        else showAuth(r.data.setup === true);
      });
    });
  }

  /** 登录 / 首次初始化。setup=true 时文案和要求不同，但走同一个表单。 */
  function showAuth(setup) {
    $('#app').hidden = true;
    $('#boot').hidden = true;
    var auth = $('#auth');
    auth.hidden = false;
    $('#authTitle').textContent = setup ? '第一次用，先建管理员' : '登录';
    $('#authHint').textContent = setup
      ? '这台机器还没有账号。创建管理员后，数据只留在本机。'
      : '这个家里的 Web 服务在本机，数据不出门。';
    $('#authSubmit').textContent = setup ? '创建并进入' : '进入';
    $('#authSub').textContent = (S.health && S.health.version ? 'LING OS ' + S.health.version : '本机安全入口') + (setup ? ' · 初始化' : '');
    $('#pairField').hidden = !!(S.health && S.health.lan === false);
    $('#authError').textContent = '';
    auth.dataset.setup = setup ? '1' : '0';
    setTimeout(function () { $('#authUser').focus(); }, 40);
  }

  /** 登录或初始化：按状态码给不同话术；初始化成功会拿到一次性 root key。 */
  function submitAuth(e) {
    e.preventDefault();
    var setup = $('#auth').dataset.setup === '1';
    var body = {
      username: $('#authUser').value.trim(),
      password: $('#authPass').value
    };
    var pair = $('#authPair').value.trim();
    if (pair) body.pair = pair;
    $('#authSubmit').disabled = true;
    api(setup ? '/api/setup' : '/api/login', { method: 'POST', body: body }).then(function (r) {
      $('#authSubmit').disabled = false;
      if (r.ok && r.data && r.data.ok !== false) {
        S.me = { username: r.data.username || body.username, role: r.data.role || 'admin' };
        if (setup && r.data.root_key) {
          try { localStorage.setItem('lo-rootkey', r.data.root_key); } catch (e) {}
          $('#authError').textContent = '';
          $('#authHint').textContent = '管理员建好了。root key 已存在这台浏览器（高风险操作用），只显示这一次：' + r.data.root_key;
        }
        enterApp();
        toast(setup ? '管理员已创建' : '已登录');
        return;
      }
      var msg = (r.data && (r.data.error || r.data.code)) || ('HTTP ' + r.status);
      if (r.status === 403) msg = '需要配对码：局域网首次访问要看主人屏幕上的 6 位数字。';
      if (r.status === 401) msg = '用户名或密码不对。';
      if (r.status === 409) msg = '已经初始化过了，直接登录。';
      if (r.status === 400) msg = '用户名不能空，密码至少 8 位。';
      if (r.network) {
        showBoot(r.timeout ? 'err-noserver' : 'err-noserver', '连不上 LING OS 后台',
          r.timeout ? '登录请求超时。检查后台是否在跑。' : '登录请求没有到达后台。',
          '地址：' + (apiBase || location.origin));
        return;
      }
      $('#authError').textContent = msg;
    });
  }

  /** 进入主界面：装配导航、按当前 hash 路由、拉数据。 */
  function enterApp() {
    $('#auth').hidden = true;
    $('#app').hidden = false;
    $('#tabbar').hidden = false;
    detectLayout();
    renderNav();
    applyTheme(document.documentElement.dataset.theme);
    route(location.hash || '#/home');
    loadAll();
  }

  /* ============ 布局检测：手机竖屏 / 电脑横屏 ============ */
  /** 竖屏 = 手机（底栏 + 单列），横屏 = 电脑（侧栏 + 多栏）。以高矮判断，不认 UA。 */
  function detectLayout() {
    var portrait = window.innerHeight > window.innerWidth;
    document.body.dataset.layout = portrait ? 'portrait' : 'landscape';
  }

  /* ============ 导航 ============ */
  var NAV = [
    ['home', '家', 'home'],
    ['devices', '设备', 'chip'],
    ['files', '文件', 'folder'],
    ['security', '安全', 'shield'],
    ['ai', 'AI', 'chat'],
    ['system', '系统', 'grid'],
    ['settings', '设置', 'settings']
  ];

  /** 侧栏与底栏共用同一份 NAV，竖屏显示底栏、横屏显示侧栏。 */
  function renderNav() {
    var rail = $('#rail');
    if (rail) {
      rail.innerHTML = '<div class="logo" aria-label="LING OS">L</div><nav class="rail-nav" aria-label="主导航">' +
        NAV.map(function (n) {
          return '<button class="rail-btn' + (S.view === n[0] ? ' is-active' : '') + '" data-nav="' + n[0] + '" title="' + n[1] + '" aria-label="' + n[1] + '"' + (S.view === n[0] ? ' aria-current="page"' : '') + '>' + icon(n[2], 21) + '</button>';
        }).join('') + '</nav>';
    }
    var tab = $('#tabbar');
    if (tab) {
      tab.innerHTML = NAV.map(function (n) {
        return '<button data-nav="' + n[0] + '" class="' + (S.view === n[0] ? 'is-on' : '') + '">' + icon(n[2], 20) + '<span>' + n[1] + '</span></button>';
      }).join('');
    }
  }

  var TITLES = { home: '家宅防线', devices: '设备', files: '文件', security: '安全', ai: '管家', system: '系统', settings: '设置' };

  /** hash 路由：#/home #/devices #/files #/security #/ai #/system #/settings。 */
  function route(hash) {
    var m = /^#\/([a-z]+)/.exec(hash || '');
    var v = m && TITLES[m[1]] ? m[1] : 'home';
    S.view = v;
    $all('.view').forEach(function (el) { el.hidden = el.id !== 'view-' + v; });
    var where = $('#barWhere');
    if (where) where.textContent = TITLES[v];
    renderNav();
    if (v === 'home') renderHome();
    else if (v === 'devices') renderDevices();
    else if (v === 'files') { renderFiles(); if (!fsLoaded) { fsLoaded = true; loadFs('/'); } }
    else if (v === 'security') renderSecurity();
    else if (v === 'ai') renderAi();
    else if (v === 'system') renderSystem();
    else if (v === 'settings') renderSettings();
  }

  /* ============ 数据加载 ============ */
  /** 一次性拉设备 / 事件 / 安全，三路并行；单路失败不影响其它视图。 */
  function loadAll() {
    S.loading = true;
    Promise.all([
      api('/api/devices'),
      api('/api/events'),
      api('/api/security')
    ]).then(function (rs) {
      var dev = rs[0], ev = rs[1], sec = rs[2];
      if (dev.ok && dev.data) { S.devices = dev.data.devices || []; S.adapters = dev.data.adapters || []; }
      if (ev.ok && ev.data) S.events = ev.data.events || [];
      if (sec.ok && sec.data) S.security = sec.data;
      S.loading = false;
      route(location.hash || '#/home');
    });
  }

  function refresh() {
    loadAll();
    toast('已刷新');
  }

  /* ============ 设备模型 ============ */
  /** 设备类型 → 图标名。 */
  function devIcon(type) {
    return { light: 'lamp', lock: 'lock', sensor: 'sensor', camera: 'camera', switch: 'switch' }[type] || 'chip';
  }
  /** 设备状态文案：按类型读 state 里的不同字段（light.on / lock.locked / sensor.* / camera.stream）。 */
  function devStateText(d) {
    if (!d.online) return '离线';
    var st = d.state || {};
    if (d.type === 'light') return st.on ? '开着' + (st.brightness != null ? ' · ' + st.brightness + '%' : '') : '关着';
    if (d.type === 'lock') return st.locked ? '已锁' : '已开 · 未上锁';
    if (d.type === 'sensor') {
      if (st.smoke === true) return '有烟';
      if (st.motion === true) return '有动静';
      if (st.contact === false || st.open === true) return '开着';
      if (st.temperature != null) return st.temperature + '°' + (st.humidity != null ? ' · ' + st.humidity + '%' : '');
      if (st.humidity != null) return '湿度 ' + st.humidity + '%';
      return '正常';
    }
    if (d.type === 'camera') return st.stream ? '有画面' : '未接画面';
    if (d.type === 'switch') return st.on ? '开着' : '关着';
    return '在线';
  }
  /** 状态色：离线灰、门开着/有烟橙，其余正常绿。 */
  function devBadge(d) {
    if (!d.online) return 'state--off';
    if (d.type === 'lock' && d.state && d.state.locked === false) return 'state--warn';
    if (d.type === 'sensor' && d.state && (d.state.smoke === true)) return 'state--warn';
    return '';
  }

  /**
   * 发设备命令。成功才更新本地 state 并重渲染；失败保留原状态 + 顶部错误条，
   * 避免界面显示一个后端根本没接受的状态。
   */
  function sendCmd(d, cmd, args) {
    return api('/api/devices/' + encodeURIComponent(d.id) + '/command', {
      method: 'POST', body: { cmd: cmd, args: args || {} }
    }).then(function (r) {
      if (r.ok && r.data && r.data.ok) {
        var st = (r.data.device && r.data.device.state) || d.state || {};
        d.state = st;
        S.lastError = '';
        toast(d.name + ' · ' + cmd);
        route(location.hash || '#/home');
        return true;
      }
      var msg = (r.data && (r.data.error || r.data.detail)) || ('HTTP ' + r.status);
      S.lastError = d.name + ' · ' + cmd + ' 没成功：' + msg;
      toast(S.lastError);
      if (S.view === 'devices') renderDevices();
      return false;
    });
  }

  /* ============ 首页 ============ */
  /** 首页标题由真实状态生成：门没锁 > 设备离线 > 一切正常。 */
  function homeHeadline() {
    if (!S.devices.length) return { h: '后台还没有设备', p: '去「设备」页添加一个，或等局域网发现。' };
    var offline = S.devices.filter(function (d) { return !d.online; });
    var unlocked = S.devices.filter(function (d) { return d.type === 'lock' && d.state && d.state.locked === false; });
    if (unlocked.length) return { h: '有一扇门没锁', p: unlocked[0].name + ' 现在是开着的，先去设备页处理。' };
    if (offline.length) return { h: offline.length + ' 台设备离线', p: offline.map(function (d) { return d.name; }).join('、') + ' 没有回话。' };
    return { h: '一切正常', p: '设备都在线，门锁着。' };
  }

  /** 首页：设备聚合、最近事件、防线、后台信息；摄像头预览仅在开关打开且有 stream 时渲染。 */
  function renderHome() {
    var el = $('#view-home');
    var online = S.devices.filter(function (d) { return d.online; }).length;
    var offline = S.devices.length - online;
    var cam = S.devices.filter(function (d) { return d.type === 'camera'; })[0];
    var showCam = previewOn() && cam && cam.online && cam.state && cam.state.stream;
    var head = homeHeadline();
    var events = S.events.slice(0, 8);

    el.innerHTML =
      '<div class="view-head">' +
        '<div><h1>' + esc(head.h) + '</h1><p>' + esc(head.p) + ' · ' + S.devices.length + ' 台设备 · ' + online + ' 台在线</p></div>' +
        '<div class="view-actions"><button class="btn btn--primary" data-act="refresh">' + icon('refresh', 16) + '刷新</button></div>' +
      '</div>' +
      '<div class="home-grid">' +
        '<div class="home-main">' +
          '<div class="summary">' +
            '<div class="tile"><div class="tile-ic">' + icon('chip', 21) + '</div><div class="tile-body"><div class="big-num">' + online + '<small>/' + S.devices.length + '</small></div><div class="tile-label">设备在线</div></div><div class="tile-side">' + (offline ? offline + ' 台离线' : '全在线') + '</div></div>' +
            '<div class="tile"><div class="tile-ic">' + icon('lock', 21) + '</div><div class="tile-body"><div class="big-num">' + (S.devices.filter(function (d) { return d.type === 'lock' && d.state && d.state.locked; }).length) + '<small>把</small></div><div class="tile-label">门锁已锁</div></div><div class="tile-side">' + (S.devices.filter(function (d) { return d.type === 'lock' && d.state && d.state.locked === false; }).length ? '有门开着' : '都锁着') + '</div></div>' +
            '<div class="tile"><div class="tile-ic">' + icon('shield', 21) + '</div><div class="tile-body"><div class="big-num" style="font-size:22px">' + esc((S.security && S.security.level) || '—') + '</div><div class="tile-label">安全等级</div></div><div class="tile-side">' + esc((S.security && S.security.name) || '未设置') + '</div></div>' +
          '</div>' +
          '<section class="panel"><div class="panel-head"><h2>最近发生</h2><span class="more">' + (S.events.length ? S.events.length + ' 条' : '实时') + '</span></div>' +
            (events.length
              ? '<ul class="timeline">' + events.map(function (e) {
                  return '<li class="tl-item"><time>' + fmtTime(e.ts) + '</time><i class="tl-dot' + (e.kind === 'security' ? ' tl-dot--warn' : '') + '"></i><div class="tl-body"><b>' + esc(kindName(e.kind)) + '</b><small>' + esc(e.detail) + '</small></div></li>';
                }).join('') + '</ul>'
              : '<div class="tl-empty">后台还没有事件。做点什么就会记在这里。</div>') +
          '</section>' +
        '</div>' +
        '<div class="home-side">' +
          (showCam ? '<div class="cam-preview"><div class="frame"><img src="' + esc(cam.state.stream) + '" alt="' + esc(cam.name) + '"></div><div class="cap"><span>' + esc(cam.name) + '</span><span>实时画面</span></div></div>' : '') +
          '<div class="panel"><div class="panel-head"><h2>防线</h2><span class="more">' + (S.security ? esc(S.security.platform) : '') + '</span></div>' +
            '<ul class="mini">' + (S.devices.length ? S.devices.slice(0, 6).map(function (d) {
              return '<li><i class="' + (d.online ? '' : 'warn') + '"></i><div><b>' + esc(d.name) + '</b><small>' + esc(devStateText(d)) + '</small></div></li>';
            }).join('') : '<li><i class="warn"></i><div><b>还没有设备</b><small>去设备页添加</small></div></li>') + '</ul></div>' +
          '<div class="panel"><div class="panel-head"><h2>后台</h2><span class="more">' + esc((S.health && S.health.version) || '') + '</span></div>' +
            '<div class="kv-row"><span>地址</span><b>' + esc(apiBase || location.origin) + '</b></div>' +
            '<div class="kv-row"><span>账号</span><b>' + esc(S.me ? S.me.username + ' · ' + S.me.role : '—') + '</b></div>' +
            '<div class="kv-row"><span>局域网</span><b>' + (S.health && S.health.lan ? '已开' : '仅本机') + '</b></div></div>' +
        '</div>' +
      '</div>';
  }

  /* ============ 设备 ============ */
  var devFilter = 'all';

  /** 场景会动到几台设备，先算给用户看。 */
  function sceneSummary(name) {
    var lights = S.devices.filter(function (d) { return d.type === 'light' && d.online; });
    var locks = S.devices.filter(function (d) { return d.type === 'lock' && d.online; });
    if (name === 'night') return '关 ' + lights.length + ' 盏灯，锁 ' + locks.length + ' 把门';
    if (name === 'away') return '关灯，上锁，门窗都盯着';
    if (name === 'home') return '开锁，把灯打开';
    return '';
  }

  /** 串行执行场景命令，全部结束再拉一次数据；失败计数如实汇总。 */
  function runScene(name) {
    var lights = S.devices.filter(function (d) { return d.type === 'light' && d.online; });
    var locks = S.devices.filter(function (d) { return d.type === 'lock' && d.online; });
    var steps = [];
    if (name === 'night' || name === 'away') {
      lights.forEach(function (d) { steps.push([d, 'off']); });
      locks.forEach(function (d) { steps.push([d, 'lock']); });
    } else if (name === 'home') {
      locks.forEach(function (d) { steps.push([d, 'unlock']); });
      lights.forEach(function (d) { steps.push([d, 'on']); });
    }
    if (!steps.length) { toast('没有能动的设备'); return; }
    var ok = 0, bad = 0;
    var chain = Promise.resolve();
    steps.forEach(function (st) {
      chain = chain.then(function () {
        return api('/api/devices/' + encodeURIComponent(st[0].id) + '/command', { method: 'POST', body: { cmd: st[1], args: {} } })
          .then(function (r) {
            if (r.ok && r.data && r.data.ok) { ok++; if (r.data.device && r.data.device.state) st[0].state = r.data.device.state; }
            else bad++;
          });
      });
    });
    chain.then(function () {
      loadAll();
      toast(name === 'home' ? '回家：' + ok + ' 个成功' + (bad ? '，' + bad + ' 个失败' : '') : '已执行：' + ok + ' 个成功' + (bad ? '，' + bad + ' 个失败' : ''));
    });
  }

  /** 设备页：场景快捷、选中详情、类型筛选、命令、发现结果、适配器状态。 */
  function renderDevices() {
    var el = $('#view-devices');
    var types = [['all', '全部'], ['light', '灯'], ['lock', '门锁'], ['sensor', '传感器'], ['camera', '摄像头'], ['switch', '开关']];
    var list = S.devices.filter(function (d) { return devFilter === 'all' || d.type === devFilter; });
    var sel = S.selectedDev ? S.devices.filter(function (d) { return d.id === S.selectedDev; })[0] : null;
    el.innerHTML =
      '<div class="view-head"><div><h1>设备</h1><p>后台 <b>' + esc((S.health && S.health.version) || '') + '</b> 记住的每一台。离线也留着，方便排查。</p></div>' +
        '<div class="view-actions"><button class="btn" data-act="discover">' + icon('refresh', 16) + '发现</button>' +
        '<button class="btn btn--primary" data-act="add-device">' + icon('plus', 16) + '加一台</button></div></div>' +
      (S.lastError ? '<div class="errorbar" role="alert"><span>' + esc(S.lastError) + '</span><button class="btn btn--sm" data-act="clear-dev-error">知道了</button></div>' : '') +
      '<div class="dev-toolbar">' + types.map(function (t) {
        return '<button class="filter' + (devFilter === t[0] ? ' is-on' : '') + '" data-devfilter="' + t[0] + '">' + t[1] + '</button>';
      }).join('') + '</div>' +
      '<div class="cards" style="grid-template-columns:repeat(3,minmax(0,1fr))">' +
        ['night', 'away', 'home'].map(function (n) {
          var label = n === 'night' ? '夜间' : n === 'away' ? '离家' : '回家';
          return '<button class="panel-block" style="text-align:left;cursor:pointer" data-scene-run="' + n + '"><h2 style="font-size:14px">' + label + '</h2><p>' + sceneSummary(n) + '</p></button>';
        }).join('') + '</div>' +
      (sel ? '<section class="panel"><div class="panel-head"><h2>' + esc(sel.name) + '</h2><span class="more">' + esc(sel.id) + '</span></div>' +
        '<div class="kv-row"><span>类型 / 协议</span><b>' + esc(sel.type) + ' · ' + esc(sel.protocol) + '</b></div>' +
        '<div class="kv-row"><span>在线</span><b>' + (sel.online ? '是' : '否') + '</b></div>' +
        '<div class="kv-row"><span>状态</span><b>' + esc(devStateText(sel)) + '</b></div>' +
        '<pre class="cmd-out" style="margin-top:10px">' + esc(JSON.stringify(sel.state || {}, null, 2)) + '</pre>' +
        '<div style="display:flex;gap:8px;margin-top:10px;flex-wrap:wrap"><button class="btn btn--sm" data-act="dev-close">收起</button>' +
        '<button class="btn btn--sm" data-del-dev="' + esc(sel.id) + '">删除这台</button></div></section>' : '') +
      (list.length ? '<div class="dev-grid">' + list.map(function (d) {
        var st = d.state || {};
        var actions = '';
        if (!d.online) actions = '<span class="dev-state">等它上线</span>';
        else if (d.type === 'light') {
          actions = '<div class="seg"><button class="' + (st.on ? 'is-on' : '') + '" data-cmd="on" data-dev="' + esc(d.id) + '">开</button><button class="' + (!st.on ? 'is-on' : '') + '" data-cmd="off" data-dev="' + esc(d.id) + '">关</button></div>';
        } else if (d.type === 'lock') {
          actions = '<button class="btn btn--sm" data-cmd="' + (st.locked ? 'unlock' : 'lock') + '" data-dev="' + esc(d.id) + '">' + (st.locked ? '开锁' : '上锁') + '</button>';
        } else if (d.type === 'switch') {
          actions = '<div class="seg"><button class="' + (st.on ? 'is-on' : '') + '" data-cmd="on" data-dev="' + esc(d.id) + '">开</button><button class="' + (!st.on ? 'is-on' : '') + '" data-cmd="off" data-dev="' + esc(d.id) + '">关</button></div>';
        } else {
          actions = '<span class="dev-state">只读</span>';
        }
        var bright = (d.type === 'light' && d.online && st.on)
          ? '<div class="row" style="border:0;padding:6px 0 0"><input class="slider" type="range" min="0" max="100" value="' + (st.brightness != null ? st.brightness : 80) + '" data-bright="' + esc(d.id) + '" aria-label="亮度"><span class="dev-state" data-brightval="' + esc(d.id) + '">' + (st.brightness != null ? st.brightness : 80) + '%</span></div>'
          : '';
        return '<article class="dev-card' + (d.online ? '' : ' is-off') + (sel && sel.id === d.id ? ' is-selected' : '') + '" data-dev="' + esc(d.id) + '" data-pick="' + esc(d.id) + '">' +
          '<div class="dev-head"><div class="dev-ico">' + icon(devIcon(d.type), 20) + '</div>' +
          '<span class="dev-state ' + devBadge(d) + '"><i style="width:7px;height:7px;border-radius:50%;background:currentColor;display:inline-block"></i>' + esc(devStateText(d)) + '</span></div>' +
          '<div><h3 class="dev-title">' + esc(d.name) + '</h3><div class="dev-sub">' + esc(d.type) + ' · ' + esc(d.protocol) + '</div></div>' +
          bright +
          '<div class="dev-foot"><span class="dev-sub">' + esc(d.id) + '</span><div class="dev-actions">' + actions + '</div></div>' +
        '</article>';
      }).join('') + '</div>'
        : '<div class="panel"><div class="tl-empty">这个筛选下没有设备。' + (devFilter === 'all' ? '先加一台虚拟设备试命令。' : '换个类型看看。') + '</div></div>') +
      (S.discovered.length ? '<section class="panel"><div class="panel-head"><h2>发现到</h2><span class="more">' + S.discovered.length + ' 个</span></div>' +
        S.discovered.map(function (x, i) {
          return '<div class="adapter"><b>' + esc(x.name || x.id || '未命名') + '</b><span class="badge">' + esc(x.protocol || '') + '</span>' +
            '<span class="muted">' + esc(x.type || '') + '</span>' +
            '<button class="btn btn--sm" style="margin-left:auto" data-addfound="' + i + '">按开关加入</button></div>';
        }).join('') + '</section>' : '') +
      '<section class="panel"><div class="panel-head"><h2>适配器</h2><span class="more">' + S.adapters.length + ' 个</span></div>' +
        (S.adapters.length ? S.adapters.map(function (a) {
          return '<div class="adapter"><span class="badge ' + (a.available ? 'ok' : 'no') + '">' + (a.available ? '可用' : '未配') + '</span><b>' + esc(a.protocol) + '</b><span class="muted">' + esc(a.detail || '') + '</span></div>';
        }).join('') : '<div class="tl-empty">后台没有返回适配器。</div>') + '</section>';
  }

  /* ============ 文件 ============ */
  var fsPath = '/';
  var fsEntries = [];
  var fsPreview = null;
  var fsLoaded = false;
  var fsSort = 'name';

  /** 读目录：空目录渲染空态，不塞假文件。 */
  function loadFs(path) {
    fsPath = path || '/';
    return api('/api/fs/list?path=' + encodeURIComponent(fsPath)).then(function (r) {
      if (r.ok && r.data) { fsEntries = r.data.entries || []; renderFiles(); }
      else toast('列目录失败：' + ((r.data && r.data.error) || ('HTTP ' + r.status)));
    });
  }

  /** 文件页：面包屑、排序、行操作（查看 / 删除）、预览、写文件表单。 */
  function renderFiles() {
    var el = $('#view-files');
    var crumbs = fsPath.split('/').filter(Boolean);
    var trail = '<button class="cmd" data-fs-enter="/">/</button>';
    var acc = '';
    crumbs.forEach(function (c) {
      acc += '/' + c;
      trail += '<span class="muted">›</span><button class="cmd" data-fs-enter="' + esc(acc) + '">' + esc(c) + '</button>';
    });
    var entries = fsEntries.slice();
    if (fsSort === 'name') entries.sort(function (a, b) { return String(a.name || '').localeCompare(String(b.name || ''), 'zh'); });
    else if (fsSort === 'time') entries.sort(function (a, b) { return String(b.mtime || '').localeCompare(String(a.mtime || '')); });
    el.innerHTML =
      '<div class="view-head"><div><h1>文件</h1><p>后台的文件目录，读写都受 ACL 管。删目录不会删，只删文件。</p></div>' +
        '<div class="view-actions"><button class="btn" data-act="fs-up">' + icon('back', 16) + '上一级</button>' +
        '<button class="btn" data-act="fs-new">' + icon('plus', 16) + '新建</button>' +
        '<button class="btn" data-act="fs-upload">' + icon('upload', 16) + '上传</button>' +
        '<button class="btn btn--primary" data-act="fs-refresh">' + icon('refresh', 16) + '刷新</button></div></div>' +
      '<div class="fs-bar"><div class="fs-path" style="display:flex;gap:4px;align-items:center;padding:4px 6px">' + trail + '</div>' +
        '<button class="btn btn--sm" data-act="fs-sort">' + (fsSort === 'name' ? '按时间' : '按名称') + '</button>' +
        '<input id="fsFile" type="file" hidden></div>' +
      '<div class="fs-list">' +
        (entries.length ? entries.map(function (e) {
          var name = e.name || '?';
          var rel = e.rel || name;
          var full = (fsPath === '/' ? '/' : fsPath.replace(/\/$/, '') + '/') + rel;
          var isDir = e.dir === true || e.type === 'dir' || e.is_dir === true;
          return '<div class="fs-row"><span class="ico">' + icon(isDir ? 'folder' : 'file', 16) + '</span>' +
            '<div style="min-width:0;flex:1"><b>' + esc(name) + '</b><small>' + (isDir ? '目录' : fmtSize(e.size)) + ' · ' + fmtTime(e.mtime) + '</small></div>' +
            '<div class="act">' + (isDir
              ? '<button class="btn btn--sm" data-fs-enter="' + esc(full) + '">打开</button>'
              : '<button class="btn btn--sm" data-fs-view="' + esc(full) + '">查看</button>' +
                '<button class="btn btn--sm" data-fs-del="' + esc(full) + '">删除</button>') + '</div></div>';
        }).join('') : '<div class="fs-row"><div class="tl-empty">这个目录是空的。用「新建」写第一个文件。</div></div>') +
      '</div>' +
      (fsPreview ? '<section class="panel"><div class="panel-head"><h2>' + esc(fsPreview.path) + '</h2><span class="more">' + esc(String(fsPreview.content || '').length) + ' 字</span></div>' +
        '<pre style="margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.6 var(--mono);color:var(--ink);max-height:420px;overflow:auto">' + esc((fsPreview.content || '').slice(0, 20000)) + '</pre></section>' : '') +
      '<section class="panel" id="fsEditor" hidden><div class="panel-head"><h2>写文件</h2><button class="btn btn--sm" data-act="fs-cancel">收起</button></div>' +
        '<label class="field"><span>路径</span><input id="fsWritePath" placeholder="/files/note.md"></label>' +
        '<label class="field"><span>内容（最大 2MB）</span><input id="fsWriteContent" placeholder="写点什么"></label>' +
        '<button class="btn btn--primary" data-act="fs-save">保存</button></section>';
  }

  /* ============ 安全 ============ */
  var users = [];

  /** 安全页：等级、机器信息、ACL、系统安全命令快捷、用户、审计。 */
  function renderSecurity() {
    var el = $('#view-security');
    var sec = S.security || {};
    var levels = sec.levels || [];
    if (S.acl === null) loadAcl();
    el.innerHTML =
      '<div class="view-head"><div><h1>安全</h1><p>等级、审计、用户、ACL 都在后台改，前端只发请求，失败就说失败。</p></div>' +
        '<div class="view-actions"><button class="btn" data-act="sec-refresh">' + icon('refresh', 16) + '刷新</button>' +
        '<a class="btn" href="' + esc(apiBase) + '/api/audit/export">' + icon('download', 16) + '导出审计</a></div></div>' +
      '<div class="cards">' +
        '<section class="panel-block"><h2>隐私等级</h2><p>当前 <b>' + esc(sec.level || '—') + '</b> · ' + esc(sec.name || '') + '</p><div class="level-list">' +
          (levels.length ? levels.map(function (l) {
            return '<button class="level' + (l.id === sec.level ? ' is-on' : '') + '" data-level="' + esc(l.id) + '"><span><b>' + esc(l.id) + ' · ' + esc(l.name) + '</b><small>' + esc(l.description) + '</small></span></button>';
          }).join('') : '<div class="tl-empty">后台没有返回等级。</div>') +
        '</div></section>' +
        '<section class="panel-block"><h2>这台机器</h2><div class="kv-row"><span>平台</span><b>' + esc(sec.platform || '—') + '</b></div>' +
          '<div class="kv-row"><span>版本</span><b>' + esc((S.health && S.health.version) || '—') + '</b></div>' +
          '<div class="kv-row"><span>仅本机</span><b>' + (S.health && S.health.lan ? '否（局域网开）' : '是') + '</b></div>' +
          '<div class="kv-row"><span>账号</span><b>' + esc(S.me ? S.me.username + ' · ' + S.me.role : '—') + '</b></div></section>' +
      '</div>' +
      '<section class="panel"><div class="panel-head"><h2>目录访问控制（ACL）</h2><span class="more">' + (S.acl ? S.acl.length + ' 条' : '读取中') + '</span></div>' +
        '<p class="sys-note">写 ACL 要 root key（设置里填），普通用户按这里放行。</p>' +
        ((S.acl && S.acl.length) ? S.acl.map(function (a) {
          return '<div class="adapter"><b>' + esc(a.subject) + '</b><span class="badge">' + esc(a.perm) + '</span><code class="muted">' + esc(a.path) + '</code>' +
            '<button class="btn btn--sm" style="margin-left:auto" data-acl-del="' + esc(a.subject) + '|' + esc(a.path) + '">删除</button></div>';
        }).join('') : '<div class="tl-empty">还没有 ACL 条目。admin 默认全权；普通用户需要放行。</div>') +
        '<div style="display:flex;gap:8px;margin-top:12px;flex-wrap:wrap">' +
          '<input id="aclSubject" placeholder="用户 / 角色" style="flex:1;min-width:110px;height:38px;border:1px solid var(--line-2);border-radius:8px;padding:0 10px;background:var(--paper)">' +
          '<input id="aclPath" placeholder="/files" style="flex:1;min-width:110px;height:38px;border:1px solid var(--line-2);border-radius:8px;padding:0 10px;background:var(--paper)">' +
          '<select id="aclPerm" style="height:38px;border:1px solid var(--line-2);border-radius:8px;padding:0 8px;background:var(--paper);color:var(--ink)"><option>read</option><option>write</option><option>rw</option></select>' +
          '<button class="btn" data-act="add-acl">加一条</button></div></section>' +
      '<section class="panel"><div class="panel-head"><h2>系统安全命令</h2><span class="more">/api/cmd</span></div>' +
        '<p class="sys-note">权限清单、行为、输入保护、规则都走系统命令；命令服务没起会明确报错。</p>' +
        '<div class="cmds" style="display:flex;gap:6px;flex-wrap:wrap;margin-top:10px">' +
        ['security.perm_list', 'security.behavior', 'security.input', 'security.privilege_status', 'rules.list', 'rules.toggle'].map(function (c) {
          return '<button class="cmd" data-sys="' + c + '">' + c + '</button>';
        }).join('') + '</div></section>' +
      '<section class="panel"><div class="panel-head"><h2>用户</h2><span class="more">' + users.length + ' 个</span></div>' +
        (users.length ? users.map(function (u) {
          return '<div class="adapter"><b>' + esc(u.username) + '</b><span class="badge">' + esc(u.role) + '</span><span class="muted">' + esc(u.created) + '</span>' +
            (u.username === (S.me && S.me.username) ? '' : '<button class="btn btn--sm" style="margin-left:auto" data-deluser="' + esc(u.id) + '">删除</button>') + '</div>';
        }).join('') : '<div class="tl-empty">只读（需要管理员权限，或还没有用户数据）。</div>') +
        '<div style="display:flex;gap:8px;margin-top:12px;flex-wrap:wrap"><input id="newUser" placeholder="用户名" style="flex:1;min-width:120px;height:38px;border:1px solid var(--line-2);border-radius:8px;padding:0 10px;background:var(--paper)">' +
        '<input id="newPass" type="password" placeholder="密码 ≥8 位" style="flex:1;min-width:140px;height:38px;border:1px solid var(--line-2);border-radius:8px;padding:0 10px;background:var(--paper)">' +
        '<button class="btn" data-act="add-user">加用户</button></div></section>' +
      '<section class="panel"><div class="panel-head"><h2>审计</h2><span class="more">' + (sec.audit ? sec.audit.length : 0) + ' 条</span></div>' +
        (sec.audit && sec.audit.length ? '<ul class="timeline">' + sec.audit.slice(0, 20).map(function (e) {
          return '<li class="tl-item"><time>' + fmtTime(e.ts) + '</time><i class="tl-dot"></i><div class="tl-body"><b>' + esc(kindName(e.kind)) + '</b><small>' + esc(e.detail) + '</small></div></li>';
        }).join('') + '</ul>' : '<div class="tl-empty">还没有审计记录。</div>') + '</section>';
  }

  /** ACL 列表读不到就显示空态，不缓存上一次的假数据。 */
  function loadAcl() {
    api('/api/acl').then(function (r) {
      S.acl = (r.ok && r.data && r.data.acl) ? r.data.acl : [];
      if (S.view === 'security') renderSecurity();
    });
  }

  /* ============ AI ============ */
  var aiMsgs = [];
  try { aiMsgs = JSON.parse(localStorage.getItem('lo-ai') || '[]') || []; } catch (e) { aiMsgs = []; }
  /** 对话只存本机 localStorage（最多 40 条），不上传。 */
  function saveAi() {
    try { localStorage.setItem('lo-ai', JSON.stringify(aiMsgs.slice(-40))); } catch (e) {}
  }

  /** AI 页：一个线程 + 输入框，走 /api/chat；后端没接会如实显示错误。 */
  function renderAi() {
    var el = $('#view-ai');
    el.innerHTML =
      '<div class="view-head"><div><h1>管家</h1><p>走后台的 /api/chat，本机模型；没接上会直接说「暂未连接」。</p></div>' +
        '<div class="view-actions"><button class="btn" data-act="ai-clear">' + icon('trash', 16) + '清空</button></div></div>' +
      '<div class="ai-wrap"><div class="ai-thread" id="aiThread" role="log" aria-live="polite" aria-label="和管家的对话"></div>' +
      '<form class="ai-form" id="aiForm"><textarea id="aiInput" rows="1" placeholder="问点什么，比如：家里现在怎么样" aria-label="对管家说点什么"></textarea>' +
      '<button class="send btn btn--primary" type="submit" id="aiSend">' + icon('send', 14) + '发送</button></form></div>';
    renderAiThread();
  }

  function renderAiThread() {
    var t = $('#aiThread');
    if (!t) return;
    if (!aiMsgs.length) {
      t.innerHTML = '<div class="tl-empty">还没有对话。发送第一条就会走 /api/chat。</div>';
      return;
    }
    t.innerHTML = '<div class="ai-list">' + aiMsgs.map(function (m) {
      return '<div class="msg ' + (m.who === 'user' ? 'user' : '') + '">' +
        (m.who === 'user' ? '<div class="msg-body"><div class="msg-who"><b>先生</b><span>' + esc(m.time) + '</span></div><div class="msg-text"><p>' + esc(m.text) + '</p></div></div><div class="msg-av">先</div>'
          : '<div class="msg-av">L</div><div class="msg-body"><div class="msg-who"><b>管家</b><span>' + esc(m.time) + '</span></div><div class="msg-text"><p>' + esc(m.text) + '</p></div></div>') + '</div>';
    }).join('') + '</div>';
    t.scrollTop = t.scrollHeight;
  }

  /** 发送：先落用户消息，再把后端回复追加；回复原样显示，不包装成成功。 */
  function aiSend(text) {
    var now = new Date();
    var time = ('0' + now.getHours()).slice(-2) + ':' + ('0' + now.getMinutes()).slice(-2);
    aiMsgs.push({ who: 'user', text: text, time: time });
    saveAi();
    renderAiThread();
    api('/api/chat', { method: 'POST', body: { text: text } }).then(function (r) {
      var reply = (r.data && r.data.reply) || ('后台没有回话（HTTP ' + r.status + '）');
      aiMsgs.push({ who: 'bot', text: reply, time: time });
      saveAi();
      renderAiThread();
    });
  }

  /* ============ 系统（/api/cmd） ============ */
  var CMDS = [
    ['核心', ['core.status', 'core.health', 'core.log_level', 'core.registry_list', 'core.dashboard', 'system.status', 'system.health', 'system.security', 'system.defense', 'system.privilege']],
    ['更新与修复', ['system.update', 'update.install', 'update.rollback', 'system.rollback', 'snapshot.list', 'snapshot.create', 'repair.run', 'health.trend', 'health.watchdog']],
    ['通讯与日志', ['comm.port_list', 'comm.port_set', 'comm.server_mode', 'log.level', 'core.logdump']],
    ['数据与记忆', ['data.memory_list', 'data.memory_search', 'data.memory_write', 'data.model_list', 'data.session_list', 'memory.list', 'memory.write']],
    ['安全', ['security.status', 'security.audit', 'security.perm_list', 'security.behavior', 'security.input', 'security.privilege_status', 'rules.list', 'rules.toggle']],
    ['家居与预警', ['home.status', 'home.states', 'alert.status', 'alert.history', 'alert.config']],
    ['AI 与技能', ['ai.status', 'ai.config', 'ai.provider_list', 'ai.nook_status', 'skill.list', 'skill.search', 'skill.browse', 'plugin.list', 'plugin.load', 'plugin.unload']],
    ['配置与设备', ['config.get', 'config.list', 'config.set', 'config.advanced', 'config.reload', 'voice.query', 'voice.usage', 'ui.desktop']]
  ];
  var sysOut = '';
  var cmdProbe = null;

  /** 探测系统命令服务（18080）是否可达，结果缓存到 cmdProbe，避免反复重试。 */
  function probeCmd() {
    var el = document.getElementById('cmdState');
    if (!el) return;
    if (cmdProbe !== null) {
      el.textContent = cmdProbe === 'on' ? '在线' : cmdProbe === 'off' ? '未连（18080 未启动）' : '异常';
      return;
    }
    el.textContent = '检查中…';
    api('/api/cmd', { method: 'POST', body: { cmd: 'system.status', args: {} }, timeout: 5000 }).then(function (r) {
      var el2 = document.getElementById('cmdState');
      if (r.status === 502) cmdProbe = 'off';
      else if (r.ok) cmdProbe = 'on';
      else cmdProbe = 'err';
      if (el2) el2.textContent = cmdProbe === 'on' ? '在线' : cmdProbe === 'off' ? '未连（18080 未启动）' : ('异常 HTTP ' + r.status);
    });
  }

  /** 会改系统状态的命令，走二次确认。 */
  function sysDanger(cmd) {
    return ['system.update', 'update.install', 'update.rollback', 'system.rollback', 'snapshot.create', 'repair.run', 'config.set', 'plugin.load', 'plugin.unload', 'security.audit'].indexOf(cmd) >= 0;
  }

  /** 执行一条系统命令：按钮 loading、12s 超时、结果 JSON 原样展示。 */
  function sysCall(cmd, btn) {
    if (btn) { btn.disabled = true; btn.dataset.old = btn.textContent; btn.textContent = '执行中…'; }
    return api('/api/cmd', { method: 'POST', body: { cmd: cmd, args: {} }, timeout: 12000 }).then(function (r) {
      if (btn) { btn.disabled = false; btn.textContent = btn.dataset.old || cmd; }
      if (r.status === 502) {
        sysOut = '系统命令服务未启动。\n\n/api/cmd 会转发到 127.0.0.1:18080，这个后端不属于 Web 进程。\n启动 lingos 核心（或 lingos.sh start）后再试。\n\n原始返回：' + JSON.stringify(r.data || {});
      } else if (r.timeout) {
        sysOut = '系统命令服务没回话（12 秒超时）。\n检查 127.0.0.1:18080 是否在跑。';
      } else if (!r.ok) {
        sysOut = '执行失败：HTTP ' + r.status + '\n' + JSON.stringify(r.data, null, 2);
      } else {
        sysOut = JSON.stringify(r.data, null, 2);
      }
      renderSystem();
      toast(cmd + (r.ok ? ' 完成' : ' 失败'));
      probeCmd();
    });
  }

  /** 系统页：86 条命令按组铺开，先给常用入口，再给完整清单。 */
  function renderSystem() {
    var el = $('#view-system');
    el.innerHTML =
      '<div class="view-head"><div><h1>系统</h1><p>86 条统一命令的图形入口，全部走 <code>/api/cmd</code>。命令服务没起时会明确报错，不会伪造状态。</p></div>' +
        '<div class="view-actions"><button class="btn" data-sys="system.status">' + icon('refresh', 16) + '系统状态</button>' +
        '<button class="btn btn--primary" data-sys="core.health">' + icon('check', 16) + '健康检查</button></div></div>' +
      '<div class="cards"><section class="panel-block"><h2>常用</h2><p>先跑这两个看这台机器醒没醒。</p>' +
        '<div class="cmds" style="display:flex;gap:6px;flex-wrap:wrap;margin-top:10px">' +
        ['core.status', 'system.status', 'health.trend', 'snapshot.list', 'repair.run'].map(function (c) {
          return '<button class="cmd' + (sysDanger(c) ? ' danger' : '') + '" data-sys="' + c + '">' + c + '</button>';
        }).join('') + '</div></section>' +
        '<section class="panel-block"><h2>这台机器</h2><div class="kv-row"><span>Web</span><b>' + esc((S.health && S.health.version) || '—') + '</b></div>' +
          '<div class="kv-row"><span>平台</span><b>' + esc((S.security && S.security.platform) || '—') + '</b></div>' +
          '<div class="kv-row"><span>命令服务</span><b id="cmdState">未检查</b></div></div>' +
      '</div>' +
      '<div class="cmd-groups">' + CMDS.map(function (g) {
        return '<section class="cmd-group"><h3>' + g[0] + '</h3><div class="cmds">' + g[1].map(function (c) {
          return '<button class="cmd' + (sysDanger(c) ? ' danger' : '') + '" data-sys="' + c + '">' + c + '</button>';
        }).join('') + '</div></section>';
      }).join('') + '</div>' +
      '<section class="panel"><div class="panel-head"><h2>输出</h2><span class="more">JSON</span></div>' +
        '<pre class="cmd-out" id="sysOut">' + esc(sysOut || '点上面任意一条命令，结果会显示在这里。') + '</pre></section>';
    probeCmd();
  }

  /* ============ 设置 ============ */
  var settingsLoaded = false;

  /** 设置页：后台地址、外观、摄像头预览、外接设备、备份、App Token、root key、退出。 */
  function renderSettings() {
    var el = $('#view-settings');
    var s = S.settings || {};
    el.innerHTML =
      '<div class="view-head"><div><h1>设置</h1><p>后台地址、外接设备、备份和这个前端的扩展项。主界面只留每天要用的。</p></div>' +
        '<div class="view-actions"><button class="btn" data-act="settings-reload">' + icon('refresh', 16) + '重读</button></div></div>' +
      '<div class="cards">' +
        '<section class="panel-block"><h2>后台</h2><p>默认同源。换地址后整个前端都走这个后台。</p>' +
          '<label class="field" style="margin-top:12px"><span>地址（留空 = 同源）</span><input id="setApi" value="' + esc(apiBase) + '" placeholder="http://127.0.0.1:8080"></label>' +
          '<button class="btn btn--primary" data-act="save-api">保存并重连</button>' +
          '<div class="kv-row" style="margin-top:12px"><span>当前</span><b>' + esc(apiBase || location.origin) + '</b></div>' +
          '<div class="kv-row"><span>健康</span><b>' + (S.health ? 'ok · ' + esc(S.health.version) : '—') + '</b></div>' +
          '<div class="kv-row"><span>账号</span><b>' + esc(S.me ? S.me.username + ' · ' + S.me.role : '—') + '</b></div>' +
        '</section>' +
        '<section class="panel-block"><h2>外观</h2><div class="row"><label>深浅色<button class="btn btn--sm" data-act="theme">' + (document.documentElement.dataset.theme === 'night' ? '切到日间' : '切到夜间') + '</button></label></div>' +
          '<div class="row"><span>摄像头预览</span><button class="btn btn--sm" data-act="toggle-preview">' + (previewOn() ? '已开' : '默认关') + '</button></div>' +
          '<p style="margin-top:8px">摄像头预览默认关。没接画面时首页不会留一块空白。</p></section>' +
        '<section class="panel-block"><h2>外接设备</h2><p>填了才会连；没填的适配器显示「未配」。</p>' +
          '<label class="field" style="margin-top:12px"><span>Home Assistant 地址</span><input id="setHaUrl" value="' + esc(s.ha_url || '') + '" placeholder="http://homeassistant.local:8123"></label>' +
          '<label class="field"><span>HA Token</span><input id="setHaToken" type="password" value="' + esc(s.ha_token || '') + '"></label>' +
          '<label class="field"><span>MQTT Host</span><input id="setMqttHost" value="' + esc(s.mqtt_host || '') + '" placeholder="192.168.1.10"></label>' +
          '<label class="field"><span>MQTT Port</span><input id="setMqttPort" value="' + esc(s.mqtt_port || '1883') + '"></label>' +
          '<label class="field"><span>MQTT 用户</span><input id="setMqttUser" value="' + esc(s.mqtt_user || '') + '"></label>' +
          '<label class="field"><span>MQTT 密码</span><input id="setMqttPass" type="password" value="' + esc(s.mqtt_pass || '') + '"></label>' +
          '<button class="btn btn--primary" data-act="save-conn">保存连接</button></section>' +
        '<section class="panel-block"><h2>备份</h2><p>备份包含数据库、文件和隐私策略。</p>' +
          '<div class="row"><span>导出</span><a class="btn btn--sm" href="' + esc(apiBase) + '/api/backup">' + icon('download', 14) + '下载备份</a></div>' +
          '<div class="row"><span>恢复</span><button class="btn btn--sm" data-act="restore">选择备份文件</button></div>' +
          '<input id="restoreFile" type="file" hidden accept=".gz,.tgz,.tar">' +
        '</section>' +
        '<section class="panel-block"><h2>适配器</h2>' +
          (S.adapters.length ? S.adapters.map(function (a) {
            return '<div class="adapter"><span class="badge ' + (a.available ? 'ok' : 'no') + '">' + (a.available ? '可用' : '未配') + '</span><b>' + esc(a.protocol) + '</b><span class="muted">' + esc(a.detail || '') + '</span></div>';
          }).join('') : '<div class="tl-empty">后台没有返回适配器。</div>') + '</section>' +
        '<section class="panel-block"><h2>App Token</h2><p>App 用 Bearer 走同一套接口，只显示一次。</p>' +
          '<div class="row" style="margin-top:10px"><span>生成</span><button class="btn btn--sm" data-act="new-token">生成 Token</button></div>' +
          '<pre class="cmd-out" id="tokenOut" style="margin-top:10px"></pre></section>' +
        '<section class="panel-block"><h2>安全操作</h2><p>ACL 改目录权限要 root key，它只在创建管理员那次显示。</p>' +
          '<label class="field" style="margin-top:12px"><span>root key（只存在这台浏览器）</span><input id="setRootKey" type="password" value="' + esc(localStorage.getItem('lo-rootkey') || '') + '"></label>' +
          '<button class="btn" data-act="save-rootkey">记住 root key</button>' +
          '<div class="row" style="margin-top:12px"><span>退出登录</span><button class="btn btn--sm" data-act="logout">退出</button></div>' +
        '</section>' +
      '</div>';
    if (!settingsLoaded) { settingsLoaded = true; loadSettings(); }
  }

  /** 设置与用户列表只有 admin 能读；403 时保持空状态由页面提示。 */
  function loadSettings() {
    api('/api/settings').then(function (r) {
      if (r.ok && r.data && r.data.settings) {
        S.settings = r.data.settings;
        if (S.view === 'settings') renderSettings();
      }
    });
    api('/api/users').then(function (r) {
      if (r.ok && r.data && r.data.users) { users = r.data.users; if (S.view === 'security') renderSecurity(); }
    });
  }

  /* ============ 事件绑定 ============ */
  /* 统一事件委托：data-nav 路由 → 各页动作 → 设备命令 → 危险动作确认。
     顺序即优先级，新增动作时挂 data-act / data-sys / data-cmd 即可。 */
  document.addEventListener('click', function (e) {
    var nav = e.target.closest('[data-nav]');
    if (nav) {
      location.hash = '#/' + nav.getAttribute('data-nav');
      return;
    }
    var f = e.target.closest('[data-devfilter]');
    if (f) { devFilter = f.getAttribute('data-devfilter'); renderDevices(); return; }

    var cmd = e.target.closest('[data-cmd]');
    if (cmd) {
      var id = cmd.getAttribute('data-dev');
      var d = S.devices.filter(function (x) { return x.id === id; })[0];
      if (!d) return;
      var name = cmd.getAttribute('data-cmd');
      if (name === 'unlock') {
        confirmBox({ title: '现在开锁？', body: d.name + ' 会打开，门口如果有人会记事件。', okText: '开锁', onOk: function () { sendCmd(d, 'unlock'); } });
      } else sendCmd(d, name);
      return;
    }

    var level = e.target.closest('[data-level]');
    if (level) {
      var lv = level.getAttribute('data-level');
      confirmBox({ title: '切到 ' + lv + '？', body: '等级会写进本机安全策略，影响摄像头、麦克风和外联。', okText: '切换', onOk: function () {
        api('/api/security/level', { method: 'POST', body: { level: lv } }).then(function (r) {
          if (r.ok) { toast('已切到 ' + lv); loadAll(); } else toast('没切成功：' + ((r.data && r.data.error) || r.status));
        });
      } });
      return;
    }

    var enter = e.target.closest('[data-fs-enter]');
    if (enter) { loadFs(enter.getAttribute('data-fs-enter')); return; }
    var view = e.target.closest('[data-fs-view]');
    if (view) {
      var p = view.getAttribute('data-fs-view');
      api('/api/fs/read?path=' + encodeURIComponent(p)).then(function (r) {
        if (r.ok && r.data) { fsPreview = { path: r.data.path, content: r.data.content }; renderFiles(); }
        else toast('读不了：' + ((r.data && r.data.error) || r.status));
      });
      return;
    }
    var del = e.target.closest('[data-fs-del]');
    if (del) {
      var dp = del.getAttribute('data-fs-del');
      confirmBox({ title: '删掉这个文件？', body: dp + ' 会从本机删除，删了就没了。', okText: '删除', onOk: function () {
        api('/api/fs?path=' + encodeURIComponent(dp), { method: 'DELETE' }).then(function (r) {
          if (r.ok) { toast('已删除'); loadFs(fsPath); } else toast('没删成：' + ((r.data && r.data.error) || r.status));
        });
      } });
      return;
    }
    var ad = e.target.closest('[data-acl-del]');
    if (ad) {
      var parts = ad.getAttribute('data-acl-del').split('|');
      confirmBox({ title: '删掉这条 ACL？', body: parts[0] + ' 对 ' + parts[1] + ' 的权限会被移除。', okText: '删除', onOk: function () {
        api('/api/acl?subject=' + encodeURIComponent(parts[0]) + '&path=' + encodeURIComponent(parts[1]),
          { method: 'DELETE', headers: rootHeaders() }).then(function (r) {
          if (r.ok) { toast('已删除'); S.acl = null; loadAcl(); }
          else toast('没删成：' + ((r.data && (r.data.error || r.data.code)) || r.status));
        });
      } });
      return;
    }

    var af = e.target.closest('[data-addfound]');
    if (af) {
      var item = S.discovered[parseInt(af.getAttribute('data-addfound'), 10)];
      if (!item) return;
      api('/api/devices', { method: 'POST', body: { name: item.name || item.id || '发现设备', type: 'switch' } }).then(function (r) {
        if (r.ok) { toast('已按开关加入：' + (item.name || item.id)); S.discovered = []; refresh(); }
        else toast('没加成：' + ((r.data && (r.data.error || r.data.detail)) || r.status));
      });
      return;
    }

    var du = e.target.closest('[data-deluser]');
    if (du) {
      var uid = du.getAttribute('data-deluser');
      confirmBox({ title: '删除这个用户？', body: '他的会话会一起失效。', okText: '删除', onOk: function () {
        api('/api/users/' + uid, { method: 'DELETE' }).then(function (r) {
          if (r.ok) { toast('已删除'); loadSettings(); renderSecurity(); } else toast('没删成：' + ((r.data && r.data.error) || r.status));
        });
      } });
      return;
    }

    var pick = e.target.closest('[data-pick]');
    if (pick && !e.target.closest('[data-cmd]') && !e.target.closest('input')) {
      S.selectedDev = pick.getAttribute('data-pick');
      renderDevices();
      return;
    }
    var scene = e.target.closest('[data-scene-run]');
    if (scene) {
      var sn = scene.getAttribute('data-scene-run');
      if (sn === 'home') {
        confirmBox({ title: '回家：开锁？', body: '会把门锁打开，并打开灯。有摄像头的话会记事件。', okText: '回家', onOk: function () { runScene('home'); } });
      } else runScene(sn);
      return;
    }
    var dv = e.target.closest('[data-del-dev]');
    if (dv) {
      var did = dv.getAttribute('data-del-dev');
      if (!rootKey()) { toast('先到设置里填 root key'); return; }
      confirmBox({ title: '删除这台设备？', body: did + ' 会从后台删掉，相关事件保留。', okText: '删除', onOk: function () {
        api('/api/devices/' + encodeURIComponent(did), { method: 'DELETE', headers: rootHeaders() }).then(function (r) {
          if (r.ok) { toast('已删除'); S.selectedDev = null; loadAll(); }
          else toast('没删成：' + ((r.data && (r.data.error || r.data.code)) || r.status));
        });
      } });
      return;
    }

    var sys = e.target.closest('[data-sys]');
    if (sys) {
      var c = sys.getAttribute('data-sys');
      if (sysDanger(c)) {
        confirmBox({ title: '执行 ' + c + '？', body: '这条会改系统状态，确认后再跑。', okText: '执行', onOk: function () { sysCall(c, sys); } });
      } else sysCall(c, sys);
      return;
    }

    var b = e.target.closest('[data-act]');
    if (!b) return;
    var act = b.getAttribute('data-act');
    if (act === 'refresh' || act === 'sec-refresh') refresh();
    else if (act === 'discover') {
      api('/api/devices/discover', { method: 'POST', body: {} }).then(function (r) {
        if (r.ok && r.data && r.data.devices) {
          S.discovered = r.data.devices;
          renderDevices();
          toast('发现 ' + S.discovered.length + ' 个，下面是结果');
        } else toast('发现失败：' + ((r.data && (r.data.error || r.data.detail)) || r.status));
      });
    } else if (act === 'add-acl') {
      var sub = ($('#aclSubject') || {}).value || '';
      var pth = ($('#aclPath') || {}).value || '/';
      var prm = ($('#aclPerm') || {}).value || 'read';
      if (!sub.trim()) { toast('先写用户或角色'); return; }
      if (!rootKey()) { toast('先到设置里填 root key'); return; }
      api('/api/acl', { method: 'POST', body: { subject: sub.trim(), path: pth.trim() || '/', perm: prm }, headers: rootHeaders() }).then(function (r) {
        if (r.ok) { toast('ACL 已加'); S.acl = null; loadAcl(); }
        else toast('没加成：' + ((r.data && (r.data.error || r.data.code)) || r.status));
      });
    } else if (act === 'dev-close') { S.selectedDev = null; renderDevices(); }
    else if (act === 'clear-dev-error') {
      S.lastError = ''; renderDevices();
    } else if (act === 'add-device') {
      confirmBox({ title: '加一台虚拟设备', body: '先加一台灯，用来验证后端命令通路。', okText: '加灯', onOk: function () {
        api('/api/devices', { method: 'POST', body: { name: '新灯', type: 'light' } }).then(function (r) {
          if (r.ok) { toast('已加一台灯'); refresh(); } else toast('没加成：' + ((r.data && r.data.error) || r.status));
        });
      } });
    } else if (act === 'fs-refresh') loadFs(fsPath);
    else if (act === 'fs-up') loadFs(fsPath.replace(/\/+$/, '').split('/').slice(0, -1).join('/') || '/');
    else if (act === 'fs-new') { var ed = $('#fsEditor'); if (ed) { ed.hidden = false; var wp = $('#fsWritePath'); if (wp) wp.focus(); } }
    else if (act === 'fs-cancel') { var ed2 = $('#fsEditor'); if (ed2) ed2.hidden = true; }
    else if (act === 'fs-save') {
      var wp = $('#fsWritePath'), wc = $('#fsWriteContent');
      if (!wp || !wc || !wp.value.trim()) { toast('先写路径'); return; }
      api('/api/fs/write', { method: 'POST', body: { path: wp.value.trim(), content: wc.value } }).then(function (r) {
        if (r.ok) { toast('已写入'); loadFs(fsPath); } else toast('没写成：' + ((r.data && r.data.error) || r.status));
      });
    } else if (act === 'fs-upload') {
      var inp = $('#fsFile'); if (inp) inp.click();
    } else if (act === 'add-user') {
      var nu = $('#newUser'), np = $('#newPass');
      if (!nu || !np || !nu.value.trim() || np.value.length < 8) { toast('用户名要有，密码至少 8 位'); return; }
      api('/api/users', { method: 'POST', body: { username: nu.value.trim(), password: np.value, role: 'user' } }).then(function (r) {
        if (r.ok) { toast('已加用户'); nu.value = ''; np.value = ''; loadSettings(); renderSecurity(); }
        else toast('没加成：' + ((r.data && r.data.error) || r.status));
      });
    } else if (act === 'ai-clear') { aiMsgs = []; saveAi(); renderAiThread(); }
    else if (act === 'settings-reload') { settingsLoaded = false; renderSettings(); }
    else if (act === 'save-api') {
      var v = ($('#setApi') && $('#setApi').value.trim()) || '';
      localStorage.setItem('lo-api', v);
      location.reload();
    } else if (act === 'save-conn') {
      var body = {
        ha_url: ($('#setHaUrl') || {}).value || '',
        ha_token: ($('#setHaToken') || {}).value || '',
        mqtt_host: ($('#setMqttHost') || {}).value || '',
        mqtt_port: ($('#setMqttPort') || {}).value || '',
        mqtt_user: ($('#setMqttUser') || {}).value || '',
        mqtt_pass: ($('#setMqttPass') || {}).value || ''
      };
      api('/api/settings', { method: 'POST', body: body }).then(function (r) {
        toast(r.ok ? '连接已保存' : '没保存：' + ((r.data && r.data.error) || r.status));
      });
    } else if (act === 'save-rootkey') {
      localStorage.setItem('lo-rootkey', ($('#setRootKey') || {}).value || '');
      toast('root key 已记在这台浏览器');
    } else if (act === 'new-token') {
      api('/api/token', { method: 'POST', body: { name: 'web' } }).then(function (r) {
        var out = $('#tokenOut');
        if (out) out.textContent = r.ok ? String((r.data && r.data.token) || '') : ('没生成：' + ((r.data && r.data.error) || r.status));
        toast(r.ok ? 'Token 已生成，只显示这一次' : '生成失败');
      });
    } else if (act === 'toggle-preview') {
      localStorage.setItem('lo-preview', previewOn() ? 'off' : 'on');
      renderSettings();
      toast(previewOn() ? '摄像头预览已开' : '摄像头预览已关');
    } else if (act === 'theme') { toggleTheme(); renderSettings(); }
    else if (act === 'restore') { var rf = $('#restoreFile'); if (rf) rf.click(); }
    else if (act === 'logout') {
      api('/api/logout', { method: 'POST', body: {} }).then(function () {
        S.me = null; location.hash = ''; location.reload();
      });
    }
  });

  document.addEventListener('input', function (e) {
    var br = e.target.closest('[data-bright]');
    if (br) {
      var label = document.querySelector('[data-brightval="' + br.getAttribute('data-bright') + '"]');
      if (label) label.textContent = br.value + '%';
    }
  });
  document.addEventListener('change', function (e) {
    var br = e.target.closest('[data-bright]');
    if (br) {
      var d = S.devices.filter(function (x) { return x.id === br.getAttribute('data-bright'); })[0];
      if (d) sendCmd(d, 'brightness', { value: parseInt(br.value, 10) });
      return;
    }
    if (e.target.id === 'fsFile' && e.target.files && e.target.files[0]) {
      var f = e.target.files[0];
      var reader = new FileReader();
      reader.onload = function () {
        api('/api/fs/write', { method: 'POST', body: { path: fsPath.replace(/\/$/, '') + '/' + f.name, content: String(reader.result || '') } }).then(function (r) {
          if (r.ok) { toast('已上传'); loadFs(fsPath); } else toast('没上传成功：' + ((r.data && r.data.error) || r.status));
        });
      };
      reader.readAsText(f);
      return;
    }
    if (e.target.id === 'restoreFile' && e.target.files && e.target.files[0]) {
      var rf = e.target.files[0];
      confirmBox({ title: '用这个备份恢复？', body: '会覆盖当前数据库、文件和隐私策略。', okText: '恢复', onOk: function () {
        fetch(apiBase + '/api/restore', { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/gzip' }, body: rf })
          .then(function (r) { toast(r.ok ? '恢复完成，重新加载' : '恢复失败（HTTP ' + r.status + '）'); if (r.ok) setTimeout(function () { location.reload(); }, 800); })
          .catch(function () { toast('恢复请求没发出去'); });
      } });
    }
  });

  var authForm = $('#authForm');
  if (authForm) authForm.addEventListener('submit', submitAuth);
  var authPass = $('#authPass');
  if (authPass) authPass.addEventListener('input', function () {
    var err = $('#authError');
    if (!err) return;
    var v = authPass.value || '';
    if (v && v.length < 8) err.textContent = '密码还差 ' + (8 - v.length) + ' 位（至少 8 位）';
    else if (err.textContent.indexOf('密码还差') === 0) err.textContent = '';
  });

  document.addEventListener('submit', function (e) {
    if (!e.target || e.target.id !== 'aiForm') return;
    e.preventDefault();
    var input = $('#aiInput');
    var text = (input && input.value || '').trim();
    if (!text) return;
    input.value = '';
    aiSend(text);
  });

  document.addEventListener('click', function (e) {
    if (e.target.closest('[data-confirm-cancel]')) { closeConfirm(); }
    if (e.target.closest('[data-confirm-ok]')) {
      var cb = confirmCb; closeConfirm();
      if (cb) cb();
    }
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closeConfirm();
  });

  var retryBtn = $('#bootRetry');
  if (retryBtn) retryBtn.addEventListener('click', function () { location.reload(); });
  var bootSet = $('#bootSettings');
  if (bootSet) bootSet.addEventListener('click', function () {
    var v = window.prompt('后台地址（留空 = 同源）', apiBase || '');
    if (v !== null) { localStorage.setItem('lo-api', v.trim()); location.reload(); }
  });
  var refreshBtn = $('#refreshBtn');
  if (refreshBtn) refreshBtn.addEventListener('click', refresh);
  var themeBtn = $('#themeBtn');
  if (themeBtn) themeBtn.addEventListener('click', toggleTheme);
  var logoutBtn = $('#logoutBtn');
  if (logoutBtn) logoutBtn.addEventListener('click', function () {
    confirmBox({ title: '退出登录？', body: '下次要重新输密码。', okText: '退出', onOk: function () {
      api('/api/logout', { method: 'POST', body: {} }).then(function () { location.reload(); });
    } });
  });

  window.addEventListener('hashchange', function () { route(location.hash); });
  window.addEventListener('resize', function () {
    var before = document.body.dataset.layout;
    detectLayout();
    if (before !== document.body.dataset.layout) renderNav();
  });

  /* 演示时钟：接入后台时显示真实时间 */
  function tickClock() {
    var d = new Date();
    var el = $('#clock');
    if (el) el.textContent = ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
  }
  tickClock();
  setInterval(tickClock, 20000);

  /* 启动 */
  applyTheme(document.documentElement.dataset.theme || 'day');
  detectLayout();
  paintIcons();
  boot();
})();
