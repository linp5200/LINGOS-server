/* LINGOS 主题切换（A/B/C）——无依赖，离线可用
 * 用法：index.html </body> 前引入；自动在 .topbar 注入切换器。
 */
(function () {
  var KEY = 'lingos_theme';
  var VALID = ['A', 'B', 'C'];
  function current() {
    var t = 'A';
    try { t = localStorage.getItem(KEY) || 'A'; } catch (e) {}
    return VALID.indexOf(t) >= 0 ? t : 'A';
  }
  function apply(t) {
    if (VALID.indexOf(t) < 0) t = 'A';
    document.documentElement.setAttribute('data-theme', t);
    try { localStorage.setItem(KEY, t); } catch (e) {}
    var btns = document.querySelectorAll('.lingos-theme-switch button');
    for (var i = 0; i < btns.length; i++) {
      btns[i].className = btns[i].getAttribute('data-theme') === t ? 'on' : '';
    }
  }
  function mount() {
    var top = document.querySelector('.topbar');
    if (!top || document.querySelector('.lingos-theme-switch')) return;
    var box = document.createElement('div');
    box.className = 'lingos-theme-switch';
    box.title = '界面风格：A 静默深色 / B 暖白家居 / C 诺玛 HUD';
    var names = { A: 'A 深色', B: 'B 暖白', C: 'C 诺玛' };
    VALID.forEach(function (t) {
      var b = document.createElement('button');
      b.textContent = names[t];
      b.setAttribute('data-theme', t);
      b.onclick = function () { apply(t); };
      box.appendChild(b);
    });
    top.appendChild(box);
    apply(current());
  }
  window.setLingosTheme = apply;
  window.getLingosTheme = current;
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', mount);
  } else {
    mount();
  }
})();
