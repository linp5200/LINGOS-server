'use strict';
(function(){
  var S = { health: null, me: null, setup: false, devices: [], security: null, filePath: '/', currentFile: '', apps: [], activeApp: '', minimized: false, events: [], deviceFilter: 'all', batch: false, sel: {} };
  function el(id){ return document.getElementById(id); }
  function esc(s){ return String(s == null ? '' : s).replace(/[&<>"']/g, function(c){ return ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c]; }); }
  function api(path, opts){
    opts = opts || {};
    opts.headers = Object.assign({'Content-Type':'application/json'}, opts.headers || {});
    return fetch(path, opts).then(function(r){
      return r.json().catch(function(){ return {}; }).then(function(j){
        if(!r.ok){ var e = new Error(j.error || ('HTTP ' + r.status)); e.status = r.status; throw e; }
        return j;
      });
    });
  }
  function toast(msg){
    var t = el('toast'); t.textContent = msg; t.classList.remove('hidden');
    clearTimeout(t._t); t._t = setTimeout(function(){ t.classList.add('hidden'); }, 2600);
  }
  function theme(){
    var v = localStorage.getItem('lingos-theme') || 'dark';
    document.documentElement.setAttribute('data-theme', v);
    var m = document.querySelector('meta[name=theme-color]'); if(m) m.setAttribute('content', v === 'dark' ? '#0f1115' : '#f4f6f9');
  }
  function toggleTheme(){
    var v = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    localStorage.setItem('lingos-theme', v); theme();
  }
  function fs(){
    var d = document.documentElement;
    if(!document.fullscreenElement){ (d.requestFullscreen || d.webkitRequestFullscreen || function(){}).call(d); }
    else { (document.exitFullscreen || document.webkitExitFullscreen || function(){}).call(document); }
  }
  function clock(){
    var d = new Date();
    var t = ('0'+d.getHours()).slice(-2) + ':' + ('0'+d.getMinutes()).slice(-2);
    if(el('clock')) el('clock').textContent = t;
    if(el('trayClock')) el('trayClock').textContent = t;
  }
  function showLogin(setup){
    S.setup = !!setup;
    el('login').classList.remove('hidden'); el('desktop').classList.add('hidden');
    el('loginTitle').textContent = setup ? '首次设置' : '登录';
    el('loginHint').textContent = setup ? '创建管理员账户；本机可直接设置，局域网需配对码。' : '本地安全入口。';
    el('loginBtn').textContent = setup ? '创建并进入' : '进入桌面';
    el('pairWrap').classList.toggle('hidden', !(S.health && S.health.lan));
  }
  function showDesktop(){
    el('login').classList.add('hidden'); el('desktop').classList.remove('hidden');
    if(S.me){ el('helloName').textContent = S.me.username; el('avatarBtn').textContent = (S.me.username || 'A').slice(0,1).toUpperCase(); el('accountInfo').textContent = S.me.username + ' · ' + S.me.role; }
    loadHome(); loadSecurity(); loadNotifs();
  }
  function doLogin(){
    var body = { username: el('username').value.trim() || 'admin', password: el('password').value, pair: el('pair').value.trim() };
    var path = S.setup ? '/api/setup' : '/api/login';
    el('loginError').textContent = '';
    api(path, { method:'POST', body: JSON.stringify(body) }).then(function(res){
      if(res && res.root_key){
        sessionStorage.setItem('lingos-root-key', res.root_key);
        alert('root key（只显示一次，请妥善保存）：\n\n' + res.root_key);
      }
      return api('/api/me');
    }).then(function(me){
      S.me = me; showDesktop(); toast(S.setup ? '管理员已创建' : '欢迎回来');
    }).catch(function(e){ el('loginError').textContent = e.message || '登录失败'; });
  }
  function showLoading(id){
    var n = el(id); if(n && !n.innerHTML) n.innerHTML = '<div class="empty">加载中…</div>';
  }
  function showErr(id, err, retry){
    var n = el(id); if(!n) return;
    n.innerHTML = '<div class="load-error">无法加载：' + esc(err && err.message ? err.message : '未知错误') +
      '<button class="link" data-retry="' + esc(retry) + '">重试</button></div>';
  }
  function loadNotifs(){
    showLoading('notifList');
    api('/api/events').then(function(r){
      S.events = r.events || [];
      var b = el('notifBadge');
      if(b){ b.textContent = String(S.events.length > 99 ? 99 : S.events.length); b.classList.toggle('hidden', S.events.length === 0); }
      var list = el('notifList');
      if(list){
        list.innerHTML = S.events.slice(0,30).map(function(e){
          return '<div class="list-row"><div class="grow"><strong>' + esc(e.kind) + '</strong><small>' + esc(e.ts) + ' · ' + esc(e.detail) + '</small></div></div>';
        }).join('') || '<div class="empty">暂无通知</div>';
      }
    }).catch(function(e){ showErr('notifList', e, 'notifs'); });
  }
  function loadHome(){
    showLoading('homeEvents'); showLoading('homeDevices'); showLoading('deviceGrid');
    api('/api/devices').then(function(r){
      S.devices = r.devices || []; S.adapters = r.adapters || [];
      renderHomeDevices(); renderDevices(); renderAdapterChips();
    }).catch(function(e){ showErr('deviceGrid', e, 'home'); showErr('homeDevices', e, 'home'); });
    api('/api/events').then(function(r){
      var rows = (r.events || []).slice(0,6).map(function(e){ return '<div class="list-row"><span class="dot"></span><div class="grow"><strong>' + esc(e.kind) + '</strong><small>' + esc(e.ts) + ' · ' + esc(e.detail) + '</small></div></div>'; }).join('');
      if(el('homeEvents')) el('homeEvents').innerHTML = rows || '<div class="empty">暂无事件</div>';
    }).catch(function(e){ showErr('homeEvents', e, 'home'); });
    api('/api/security').then(function(sec){ S.security = sec; renderSecurity(sec); }).catch(function(e){
      if(el('statSecurity')) el('statSecurity').textContent = '—';
      if(el('statSecurityName')) el('statSecurityName').textContent = '读取失败';
      if(el('chipSecurity')) el('chipSecurity').textContent = '安全 未知';
      showErr('levelBtns', e, 'home');
    });
    if(S.health){
      el('statPlatform').textContent = (S.health.platform || '--').replace('linux','Termux/Linux');
      el('statUptime').textContent = (S.health.version || '0.13.1');
      el('chipLan').textContent = S.health.lan ? '局域网' : '本地';
    }
  }
  function renderHomeDevices(){
    var rows = S.devices.slice(0,5).map(function(d){
      return '<div class="list-row"><span class="dot ' + (d.online?'':'off') + '"></span><div class="grow"><strong>' + esc(d.name) + '</strong><small>' + esc(d.id) + ' · ' + esc(d.type) + '</small></div></div>';
    }).join('');
    if(el('homeDevices')) el('homeDevices').innerHTML = rows || '<div class="muted">暂无设备</div>';
    if(el('statDevices')) el('statDevices').textContent = String(S.devices.filter(function(d){return d.online;}).length);
  }
  function renderDeviceFilters(){
    var h = el('deviceFilters'); if(!h) return;
    var types = ['all','light','lock','sensor','switch','camera'];
    var labels = {all:'全部',light:'灯',lock:'锁',sensor:'传感器',switch:'开关',camera:'摄像头'};
    h.innerHTML = types.map(function(t){
      var n = t === 'all' ? S.devices.length : S.devices.filter(function(d){return d.type === t;}).length;
      return '<button type="button" class="chip filter-chip ' + (S.deviceFilter === t ? 'active' : '') + '" data-filter="' + t + '" aria-pressed="' + (S.deviceFilter === t) + '">' + labels[t] + ' ' + n + '</button>';
    }).join('');
  }
  function renderDevices(focusSel){
    var g = el('deviceGrid'); if(!g) return;
    renderDeviceFilters();
    var list = S.devices.filter(function(d){ return S.deviceFilter === 'all' || d.type === S.deviceFilter; });
    g.innerHTML = list.map(function(d){
      var sel = S.batch ? '<input type="checkbox" class="batch-sel" data-sel="' + esc(d.id) + '" aria-label="选择 ' + esc(d.name) + '"' + (S.sel[d.id] ? ' checked' : '') + '>' : '';
      var actions = '';
      if(d.type === 'light'){
        actions = '<button class="btn" data-dev="' + esc(d.id) + '" data-cmd="toggle">开/关</button>';
        if(typeof d.state.brightness === 'number') actions += '<input class="brightness" type="range" min="0" max="100" step="10" value="' + Number(d.state.brightness) + '" data-dev="' + esc(d.id) + '" aria-label="亮度 ' + esc(d.name) + '">';
      }
      else if(d.type === 'lock') actions = '<button class="btn" data-dev="' + esc(d.id) + '" data-cmd="lock">上锁</button><button class="btn" data-dev="' + esc(d.id) + '" data-cmd="unlock">解锁</button>';
      else actions = '<button class="btn" data-dev="' + esc(d.id) + '" data-cmd="refresh">刷新</button>';
      var del = d.protocol === 'virtual' ? '<button class="btn danger" data-del-dev="' + esc(d.id) + '">删除</button>' : '';
      return '<article class="card device-card' + (S.sel[d.id] ? ' selected' : '') + '">' + sel + '<div class="dev-head"><div><div class="type">' + esc(String(d.type).toUpperCase()) + ' / ' + esc(d.protocol) + '</div><h2>' + esc(d.name) + '</h2><small class="muted">' + esc(d.id) + '</small></div><span class="dot ' + (d.online?'':'off') + '"></span></div><pre class="muted device-state">' + esc(JSON.stringify(d.state, null, 2)) + '</pre><div class="device-actions">' + actions + del + '</div></article>';
    }).join('') || '<div class="empty">没有符合条件的设备</div>';
    renderBatch();
    if(focusSel) setTimeout(function(){ var n=document.querySelector(focusSel); if(n) n.focus(); }, 0);
  }
  function renderBatch(){
    var bar = el('batchBar'); if(!bar) return;
    bar.classList.toggle('hidden', !S.batch);
    var n = Object.keys(S.sel).length;
    if(el('batchCount')) el('batchCount').textContent = '已选 ' + n + ' 台';
    if(el('batchBtn')){ el('batchBtn').classList.toggle('active', S.batch); el('batchBtn').setAttribute('aria-pressed', String(S.batch)); }
  }
  function devCmd(id, cmd, args){
    api('/api/devices/' + encodeURIComponent(id) + '/command', { method:'POST', body: JSON.stringify({ cmd: cmd, args: args || {} }) })
      .then(function(r){ toast('已发送：' + cmd); loadHome(); }).catch(function(e){ toast(e.message); });
  }
  function ripple(btn, ev){
    if(!btn || !ev || !btn.getBoundingClientRect) return;
    var r = btn.getBoundingClientRect();
    var s = document.createElement('span');
    var size = Math.max(r.width, r.height) * 1.6;
    s.className = 'ripple';
    s.style.width = s.style.height = size + 'px';
    s.style.left = (ev.clientX - r.left - size / 2) + 'px';
    s.style.top = (ev.clientY - r.top - size / 2) + 'px';
    btn.appendChild(s);
    setTimeout(function(){ if(s.parentNode) s.parentNode.removeChild(s); }, 280);
  }
  function showApp(id, minimize){
    var views = document.querySelectorAll('.view');
    var v = el('view-' + id);
    var names = {home:'概览',devices:'设备',files:'文件',security:'安全',ai:'AI 助手',settings:'设置'};
    var ta = el('taskApps');
    if(S.activeApp === id && v && v.classList.contains('active')){
      if(minimize){
        v.classList.remove('active'); S.minimized = true;
        ta.innerHTML = '<button class="task-app" data-task="' + esc(id) + '">' + esc(names[id] || id) + ' · 已最小化</button>';
        el('startMenu').classList.add('hidden');
        return;
      }
      var heading = v.querySelector('h1');
      if(heading){ heading.tabIndex = -1; heading.focus(); }
      el('startMenu').classList.add('hidden');
      return;
    }
    for(var i=0;i<views.length;i++) views[i].classList.remove('active');
    if(v) v.classList.add('active');
    S.activeApp = id; S.minimized = false;
    document.querySelectorAll('.dock-app').forEach(function(b){ b.classList.toggle('active', b.getAttribute('data-app') === id); });
    document.querySelectorAll('.tab-app').forEach(function(b){
      var on = b.getAttribute('data-app') === id;
      b.classList.toggle('active', on);
      if(on) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current');
    });
    ta.innerHTML = '<button class="task-app active" data-task="' + esc(id) + '">' + esc(names[id] || id) + '</button>';
    if(id === 'files') loadFiles(S.filePath);
    if(id === 'security') loadSecurity();
    if(id === 'settings') loadSettings();
    if(id === 'home') loadHome();
    el('startMenu').classList.add('hidden');
  }
  function parentPath(p){
    var parts = String(p || '/').split('/').filter(Boolean);
    parts.pop();
    return parts.length ? '/' + parts.join('/') : '/';
  }
  function renderCrumb(path){
    var node = el('fileCrumb'); if(!node) return;
    var parts = String(path || '/').split('/').filter(Boolean);
    var html = '<button data-crumb="/" title="根目录">/</button>';
    var acc = '';
    parts.forEach(function(p){
      acc += '/' + p;
      html += '<span class="sep">/</span><button data-crumb="' + esc(acc) + '" title="' + esc(acc) + '">' + esc(p) + '</button>';
    });
    node.innerHTML = html;
    var up = el('fileUp');
    if(up) up.disabled = path === '/';
  }
  function loadFiles(path, focusSel){
    S.filePath = path || '/';
    renderCrumb(S.filePath);
    el('fileList').innerHTML = '<div class="empty">加载中…</div>';
    api('/api/fs/list?path=' + encodeURIComponent(S.filePath)).then(function(r){
      var rows = (r.entries || []).map(function(e){
        return '<button type="button" class="file-row" data-rel="' + esc(e.rel) + '" data-dir="' + (e.dir?1:0) + '"><span>' + (e.dir?'▸':'·') + '</span><span class="name">' + esc(e.name) + '</span><span class="size">' + (e.dir?'':esc(String(e.size))) + '</span></button>';
      }).join('');
      el('fileList').innerHTML = rows || '<div class="empty">空目录</div>';
      if(focusSel) setTimeout(function(){ var n=document.querySelector(focusSel); if(n) n.focus(); }, 0);
    }).catch(function(e){ showErr('fileList', e, 'files'); });
  }
  function openFile(rel, dir){
    if(dir){ loadFiles('/' + rel, '[data-crumb="/' + rel.replace(/"/g, '') + '"]'); return; }
    api('/api/fs/read?path=' + encodeURIComponent('/' + rel)).then(function(r){
      S.currentFile = '/' + rel; el('fileTitle').textContent = S.currentFile; el('fileEditor').value = r.content || '';
    }).catch(function(e){ toast(e.message); });
  }
  function saveFile(){
    var rel = S.currentFile;
    var name = el('fileNewName').value.trim();
    if(name){ rel = (S.filePath.replace(/\/$/,'') + '/' + name).replace(/\/+/g,'/'); }
    if(!rel){ toast('先选择文件或输入文件名'); return; }
    api('/api/fs/write', { method:'POST', body: JSON.stringify({ path: rel, content: el('fileEditor').value }) })
      .then(function(){ toast('已保存 ' + rel); S.currentFile = rel; loadFiles(S.filePath); })
      .catch(function(e){ toast(e.message); });
  }
  function renderSecurity(sec){
    S.security = sec;
    if(el('statSecurity')) el('statSecurity').textContent = sec.level;
    if(el('statSecurityName')) el('statSecurityName').textContent = sec.name;
    if(el('chipSecurity')) el('chipSecurity').textContent = '安全 ' + sec.level;
    if(el('secDesc')) el('secDesc').textContent = sec.name + '：' + sec.description;
    if(el('levelBtns')) el('levelBtns').innerHTML = (sec.levels || []).map(function(l){
      return '<button class="btn ' + (l.id === sec.level ? 'active' : '') + '" data-level="' + esc(l.id) + '">' + esc(l.id) + ' ' + esc(l.name) + '</button>';
    }).join('');
    var rows = (sec.audit || []).map(function(e){ return '<div class="list-row"><div class="grow"><strong>' + esc(e.kind) + '</strong><small>' + esc(e.ts) + ' · ' + esc(e.detail) + '</small></div></div>'; }).join('');
    if(el('auditList')) el('auditList').innerHTML = rows || '<div class="empty">暂无审计</div>';
    if(el('platformCaps')) el('platformCaps').innerHTML = '<div class="list-row"><div class="grow"><strong>平台</strong><small>' + esc(sec.platform) + '</small></div></div><div class="list-row"><div class="grow"><strong>内核级防火墙</strong><small>Termux 不可用；完整平台启用</small></div></div>';
    if(S.focusLevel){ var b=document.querySelector('[data-level="' + String(S.focusLevel).replace(/"/g,'') + '"]'); if(b) b.focus(); S.focusLevel=null; }
  }
  function loadSecurity(){
    showLoading('levelBtns'); showLoading('auditList');
    api('/api/security').then(renderSecurity).catch(function(e){ showErr('levelBtns', e, 'security'); showErr('auditList', e, 'security'); });
  }
  function setLevel(level){
    S.focusLevel = level;
    api('/api/security/level', { method:'POST', body: JSON.stringify({ level: level }) }).then(function(){ toast('安全等级：' + level); loadSecurity(); }).catch(function(e){ S.focusLevel = null; toast(e.message); });
  }
  function sendChat(){
    var t = el('chatText').value.trim(); if(!t) return;
    var log = el('chatLog');
    log.innerHTML += '<div class="msg me">' + esc(t) + '</div>';
    el('chatText').value = '';
    log.scrollTop = log.scrollHeight;
    api('/api/chat', { method:'POST', body: JSON.stringify({ text: t }) }).then(function(r){
      log.innerHTML += '<div class="msg">' + esc(r.reply || '') + '</div>';
      log.scrollTop = log.scrollHeight;
    }).catch(function(e){
      log.innerHTML += '<div class="msg sys">' + esc(e.message) + '</div>';
    });
  }
  function rootKey(){
    var k = sessionStorage.getItem('lingos-root-key') || '';
    if(!k){ k = prompt('输入 root key（高风险操作需要）'); if(k) sessionStorage.setItem('lingos-root-key', k); }
    return k || '';
  }
  function rootHeaders(){
    var k = rootKey();
    return k ? {'X-Lingos-Root-Key': k} : {};
  }
  function withRoot(opts){
    opts = opts || {};
    opts.headers = Object.assign({}, opts.headers || {}, rootHeaders());
    return opts;
  }
  function renderAdapterChips(){
    var h = el('adapterChips'); if(!h) return;
    h.innerHTML = (S.adapters || []).map(function(a){
      return '<span class="chip">' + esc(a.protocol) + ' · ' + (a.available ? '可用' : '未配置') + '</span>';
    }).join('');
  }
  function loadSettings(){ loadAcl(); loadUsers(); loadAdapters(); loadAdapterSettings(); }
  function loadAcl(){
    api('/api/acl').then(function(r){
      var rows = (r.acl || []).map(function(a){
        return '<div class="list-row"><div class="grow"><strong>' + esc(a.subject) + ' → ' + esc(a.path) + '</strong><small>' + esc(a.perm) + '</small></div><button class="btn danger" data-del-acl data-subject="' + esc(a.subject) + '" data-path="' + esc(a.path) + '">删除</button></div>';
      }).join('');
      if(el('aclList')) el('aclList').innerHTML = rows || '<div class="muted">暂无 ACL（admin 全权）</div>';
    }).catch(function(e){ showErr('aclList', e, 'settings'); });
  }
  function addAcl(){
    var subject = prompt('用户或角色（例如 user / bob / admin）'); if(!subject) return;
    var path = prompt('目录路径（例如 / 或 /docs）', '/'); if(path == null) return;
    var perm = prompt('权限：read / write / rw', 'read'); if(!perm) return;
    api('/api/acl', withRoot({ method:'POST', body: JSON.stringify({subject:subject, path:path, perm:perm}) }))
      .then(function(){ toast('ACL 已添加'); loadAcl(); }).catch(function(e){ toast(e.message); });
  }
  function loadUsers(){
    api('/api/users').then(function(r){
      var rows = (r.users || []).map(function(u){
        return '<div class="list-row"><div class="grow"><strong>' + esc(u.username) + '</strong><small>' + esc(u.role) + ' · ' + esc(u.created) + '</small></div>' + (u.role === 'admin' ? '' : '<button class="btn danger" data-del-user="' + esc(String(u.id)) + '">删除</button>') + '</div>';
      }).join('');
      if(el('userList')) el('userList').innerHTML = rows || '<div class="muted">无</div>';
    }).catch(function(e){ showErr('userList', e, 'settings'); });
  }
  function addUser(){
    var username = prompt('新用户名'); if(!username) return;
    var password = prompt('密码（至少 8 位）'); if(!password) return;
    api('/api/users', { method:'POST', body: JSON.stringify({username:username, password:password, role:'user'}) })
      .then(function(){ toast('用户已创建'); loadUsers(); }).catch(function(e){ toast(e.message); });
  }
  function loadAdapters(){
    api('/api/adapters').then(function(r){ S.adapters = r.adapters || []; renderAdapterChips(); }).catch(function(e){ showErr('adapterChips', e, 'settings'); });
  }
  function loadAdapterSettings(){
    api('/api/settings').then(function(r){
      S.settings = r.settings || {};
      var rows = ['ha_url','ha_token','mqtt_host','mqtt_port','miio_devices'].map(function(k){
        var v = S.settings[k] || '';
        if(k === 'ha_token' && v) v = '已配置';
        return '<div class="list-row"><div class="grow"><strong>' + esc(k) + '</strong><small>' + esc(v || '未配置') + '</small></div></div>';
      }).join('');
      if(el('adapterSettings')) el('adapterSettings').innerHTML = rows;
    }).catch(function(e){ showErr('adapterSettings', e, 'settings'); });
  }
  function cfgHA(){
    var url = prompt('Home Assistant URL（如 http://192.168.1.10:8123）', (S.settings && S.settings.ha_url) || ''); if(url == null) return;
    var token = prompt('Long-Lived Token（留空保持）'); if(token == null) return;
    var body = { ha_url: url };
    if(token) body.ha_token = token;
    api('/api/settings', { method:'POST', body: JSON.stringify(body) }).then(function(){ toast('HA 已保存'); loadAdapterSettings(); }).catch(function(e){ toast(e.message); });
  }
  function cfgMQTT(){
    var host = prompt('MQTT host（如 192.168.1.10）', (S.settings && S.settings.mqtt_host) || ''); if(host == null) return;
    var port = prompt('MQTT 端口', (S.settings && S.settings.mqtt_port) || '1883'); if(port == null) return;
    api('/api/settings', { method:'POST', body: JSON.stringify({mqtt_host:host, mqtt_port:port}) }).then(function(){ toast('MQTT 已保存'); loadAdapterSettings(); }).catch(function(e){ toast(e.message); });
  }
  function cfgMiio(){
    var raw = prompt('米家设备 JSON 数组（[{"ip":"...","did":"...","token":"...","name":"..."}]）', (S.settings && S.settings.miio_devices) || '[]'); if(raw == null) return;
    api('/api/settings', { method:'POST', body: JSON.stringify({miio_devices:raw}) }).then(function(){ toast('米家配置已保存（实验）'); loadAdapterSettings(); }).catch(function(e){ toast(e.message); });
  }
  function boot(){
    theme(); clock(); setInterval(clock, 15000);
    api('/api/health').then(function(h){
      S.health = h;
      if(h.setup){ showLogin(true); return; }
      api('/api/me').then(function(me){ S.me = me; showDesktop(); }).catch(function(){ showLogin(false); });
    }).catch(function(){ el('loginError').textContent = '无法连接 lingos-web'; showLogin(false); });
    el('loginBtn').addEventListener('click', doLogin);
    el('password').addEventListener('keydown', function(e){ if(e.key === 'Enter') doLogin(); });
    el('pair').addEventListener('keydown', function(e){ if(e.key === 'Enter') doLogin(); });
    el('notifBtn').addEventListener('click', function(){ el('notifPanel').classList.toggle('hidden'); loadNotifs(); });
    el('notifClose').addEventListener('click', function(){ el('notifPanel').classList.add('hidden'); });
    el('themeBtn').addEventListener('click', toggleTheme);
    el('themeBtn2').addEventListener('click', toggleTheme);
    el('fullBtn').addEventListener('click', fs);
    el('fullBtn2').addEventListener('click', fs);
    el('startBtn').addEventListener('click', function(){ el('startMenu').classList.toggle('hidden'); });
    el('taskStart').addEventListener('click', function(){ el('startMenu').classList.toggle('hidden'); });
    el('logoutBtn').addEventListener('click', function(){ api('/api/logout', {method:'POST', body:'{}'}).finally(function(){ location.reload(); }); });
    el('startLogout').addEventListener('click', function(){ api('/api/logout', {method:'POST', body:'{}'}).finally(function(){ location.reload(); }); });
    el('avatarBtn').addEventListener('click', function(){ showApp('settings'); });
    el('refreshDevices').addEventListener('click', loadHome);
    el('addDeviceBtn').addEventListener('click', function(){
      var name = prompt('设备名称', '新设备'); if(!name) return;
      var dtype = prompt('类型：light / lock / sensor / switch', 'light'); if(!dtype) return;
      api('/api/devices', { method:'POST', body: JSON.stringify({name:name, type:dtype}) })
        .then(function(){ toast('设备已添加'); loadHome(); }).catch(function(e){ toast(e.message); });
    });
    el('discoverBtn').addEventListener('click', function(){
      api('/api/devices/discover', { method:'POST', body: JSON.stringify({protocol:''}) })
        .then(function(r){ toast('发现 ' + (r.count || 0) + ' 个（需适配器已配置）'); }).catch(function(e){ toast(e.message); });
    });
    el('batchBtn').addEventListener('click', function(){ S.batch = !S.batch; S.sel = {}; renderDevices(); setTimeout(function(){ el('batchBtn').focus(); }, 0); });
    el('batchCancel').addEventListener('click', function(){ S.batch = false; S.sel = {}; renderDevices(); setTimeout(function(){ el('batchBtn').focus(); }, 0); });
    el('addAclBtn').addEventListener('click', addAcl);
    el('addUserBtn').addEventListener('click', addUser);
    el('haCfgBtn').addEventListener('click', cfgHA);
    el('mqttCfgBtn').addEventListener('click', cfgMQTT);
    el('miioCfgBtn').addEventListener('click', cfgMiio);
    el('openFilesBtn').addEventListener('click', function(){ window.open('/files.html', '_blank'); });
    el('rootKeyBtn').addEventListener('click', function(){
      var pw = prompt('当前管理员密码（用于轮换 root key）'); if(!pw) return;
      api('/api/rootkey', { method:'POST', body: JSON.stringify({password:pw}) }).then(function(r){
        sessionStorage.setItem('lingos-root-key', r.root_key);
        alert('新 root key（只显示一次）：\n\n' + r.root_key);
      }).catch(function(e){ toast(e.message); });
    });
    el('appTokenBtn').addEventListener('click', function(){
      var name = prompt('Token 名称（例如 App/平板）', 'app'); if(name == null) return;
      api('/api/token', { method:'POST', body: JSON.stringify({name:name}) }).then(function(r){
        prompt('API Token（只显示一次，复制到 App）', r.token);
      }).catch(function(e){ toast(e.message); });
    });
    el('exportAuditBtn').addEventListener('click', function(){ window.location.href = '/api/audit/export'; });
    el('backupBtn').addEventListener('click', function(){ window.location.href = '/api/backup'; });
    el('restoreBtn').addEventListener('click', function(){ el('restoreFile').click(); });
    el('restoreFile').addEventListener('change', function(){
      var f = this.files && this.files[0]; if(!f) return;
      var k = rootKey(); if(!k) return;
      if(!confirm('恢复将覆盖当前数据库、文件和策略，确定继续？')) return;
      var input = this, reader = new FileReader();
      reader.onload = function(){
        fetch('/api/restore', { method:'POST', headers:{'Content-Type':'application/gzip','X-Lingos-Root-Key':k}, body:reader.result })
          .then(function(r){ return r.json().catch(function(){ return {}; }).then(function(j){ if(!r.ok) throw new Error(j.error || ('HTTP '+r.status)); return j; }); })
          .then(function(){ toast('恢复完成，正在刷新'); setTimeout(function(){ location.reload(); }, 600); })
          .catch(function(e){ toast('恢复失败：' + e.message); });
      };
      reader.onerror = function(){ toast('读取备份失败'); };
      reader.readAsArrayBuffer(f);
      input.value = '';
    });
    el('refreshFiles').addEventListener('click', function(){ loadFiles(S.filePath); });
    el('refreshSecurity').addEventListener('click', loadSecurity);
    el('saveFile').addEventListener('click', saveFile);
    el('chatSend').addEventListener('click', sendChat);
    el('chatText').addEventListener('keydown', function(e){ if(e.key === 'Enter' && !e.shiftKey){ e.preventDefault(); sendChat(); } });
    el('clearChat').addEventListener('click', function(){ el('chatLog').innerHTML = ''; });
    el('openAI').addEventListener('click', function(){ showApp('ai'); });
    el('openDevices').addEventListener('click', function(){ showApp('devices'); });
    document.addEventListener('click', function(e){
      var app = e.target.closest('[data-app]'); if(app){ showApp(app.getAttribute('data-app')); return; }
      var bc = e.target.closest('[data-batch-cmd]'); if(bc){
        var cmd = bc.getAttribute('data-batch-cmd');
        var ids = Object.keys(S.sel).filter(function(id){
          var d = S.devices.filter(function(x){return x.id === id;})[0];
          return d && (d.type === 'light' || d.type === 'switch') && d.online;
        });
        if(!ids.length){ toast('没有可批量控制的在线灯/开关'); return; }
        ids.forEach(function(id){ devCmd(id, cmd); });
        S.batch = false; S.sel = {}; renderDevices();
        return;
      }
      var fl = e.target.closest('[data-filter]'); if(fl){ var fv=fl.getAttribute('data-filter'); S.deviceFilter = fv; renderDevices('[data-filter="' + String(fv).replace(/"/g,'') + '"]'); return; }
      var dc = e.target.closest('[data-disclosure]'); if(dc){
        var dp = el(dc.getAttribute('aria-controls'));
        if(dp){ var open = dc.getAttribute('aria-expanded') === 'true'; dc.setAttribute('aria-expanded', String(!open)); dp.hidden = open; }
        return;
      }
      var dev = e.target.closest('[data-dev]'); if(dev && !dev.classList.contains('brightness')){ devCmd(dev.getAttribute('data-dev'), dev.getAttribute('data-cmd')); return; }
      var lv = e.target.closest('[data-level]'); if(lv){ ripple(lv, e); setLevel(lv.getAttribute('data-level')); return; }
      var fi = e.target.closest('.file-row'); if(fi){ openFile(fi.getAttribute('data-rel'), fi.getAttribute('data-dir') === '1'); return; }
      var tk = e.target.closest('[data-task]'); if(tk){ showApp(tk.getAttribute('data-task'), true); return; }
      var cr = e.target.closest('[data-crumb]'); if(cr){ var cp=String(cr.getAttribute('data-crumb')); loadFiles(cp, '[data-crumb="' + cp.replace(/"/g,'') + '"]'); return; }
      var rt = e.target.closest('[data-retry]'); if(rt){
        var k = rt.getAttribute('data-retry');
        if(k === 'home' || k === 'devices') loadHome();
        else if(k === 'files') loadFiles(S.filePath);
        else if(k === 'security') loadSecurity();
        else if(k === 'notifs') loadNotifs();
        else if(k === 'settings') loadSettings();
        return;
      }
      var da = e.target.closest('[data-del-acl]'); if(da){ if(confirm('删除该 ACL？')){ api('/api/acl?subject=' + encodeURIComponent(da.getAttribute('data-subject')) + '&path=' + encodeURIComponent(da.getAttribute('data-path')), withRoot({ method:'DELETE', body:'{}' })).then(loadAcl).catch(function(err){ toast(err.message); }); } return; }
      var du = e.target.closest('[data-del-user]'); if(du){ if(confirm('删除该用户？')){ api('/api/users/' + encodeURIComponent(du.getAttribute('data-del-user')), withRoot({ method:'DELETE', body:'{}' })).then(loadUsers).catch(function(err){ toast(err.message); }); } return; }
      var dd = e.target.closest('[data-del-dev]'); if(dd){ if(confirm('删除该虚拟设备？')){ api('/api/devices/' + encodeURIComponent(dd.getAttribute('data-del-dev')), withRoot({ method:'DELETE', body:'{}' })).then(loadHome).catch(function(err){ toast(err.message); }); } return; }
    });
    var startApps = ['home','devices','files','security','ai','settings'];
    var names = {home:'概览',devices:'设备',files:'文件',security:'安全',ai:'AI 助手',settings:'设置'};
    var icons = {home:'⌂',devices:'◈',files:'▤',security:'◇',ai:'✦',settings:'⚙'};
    el('startApps').innerHTML = startApps.map(function(id){ return '<button class="start-app" data-app="' + id + '"><div>' + icons[id] + '</div>' + names[id] + '</button>'; }).join('');
    el('fileUp').addEventListener('click', function(){ if(S.filePath !== '/'){ var pp=parentPath(S.filePath); loadFiles(pp, '[data-crumb="' + pp + '"]'); } });
    document.addEventListener('change', function(e){
      var br = e.target.closest('.brightness'); if(br){ devCmd(br.getAttribute('data-dev'), 'brightness', { value: Number(br.value) }); return; }
      var bs = e.target.closest('.batch-sel'); if(bs){
        var id = bs.getAttribute('data-sel');
        if(bs.checked) S.sel[id] = true; else delete S.sel[id];
        renderBatch();
        var card = bs.closest('.device-card'); if(card) card.classList.toggle('selected', !!S.sel[id]);
      }
    });
    if('serviceWorker' in navigator){ navigator.serviceWorker.register('/sw.js').catch(function(){}); }
    document.addEventListener('keydown', function(e){ if(e.key === '/' && document.activeElement.tagName !== 'INPUT' && document.activeElement.tagName !== 'TEXTAREA'){ e.preventDefault(); el('quickSearch').focus(); } });
    el('quickSearch').addEventListener('keydown', function(e){
      if(e.key === 'Enter'){ var q = e.target.value.trim(); if(q){ showApp('ai'); el('chatText').value = q; el('chatText').focus(); } }
    });
  }
  if(document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot); else boot();
})();
