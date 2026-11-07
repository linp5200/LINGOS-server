'use strict';
const CACHE = 'lingos-webdesk-0.13.1';
const SHELL = ['/', '/index.html', '/files.html', '/debug.html', '/assets/style.css', '/assets/app.js', '/manifest.json', '/assets/icon.svg'];
self.addEventListener('install', function(e){
  e.waitUntil(caches.open(CACHE).then(function(c){ return c.addAll(SHELL).catch(function(){}); }).then(function(){ return self.skipWaiting(); }));
});
self.addEventListener('activate', function(e){
  e.waitUntil(caches.keys().then(function(keys){
    return Promise.all(keys.filter(function(k){ return k !== CACHE; }).map(function(k){ return caches.delete(k); }));
  }).then(function(){ return self.clients.claim(); }));
});
self.addEventListener('fetch', function(e){
  var u = new URL(e.request.url);
  if(e.request.method !== 'GET' || u.pathname.indexOf('/api/') === 0) return;
  if(e.request.mode === 'navigate'){
    e.respondWith(fetch(e.request).then(function(resp){
      var copy = resp.clone();
      caches.open(CACHE).then(function(c){ c.put('/index.html', copy).catch(function(){}); });
      return resp;
    }).catch(function(){ return caches.match('/index.html'); }));
    return;
  }
  e.respondWith(caches.match(e.request).then(function(r){ return r || fetch(e.request).then(function(resp){
    var copy = resp.clone();
    caches.open(CACHE).then(function(c){ c.put(e.request, copy).catch(function(){}); });
    return resp;
  }).catch(function(){ return caches.match(e.request); }); }));
});
