'use strict';
// Eigenständige Signal-K-Webapp nach dem Proxy-Prinzip des signalk-victron-gui.
// Upstream wird NICHT im Internet veröffentlicht; nur in einem vertrauenswürdigen LAN verwenden.
const http = require('http');
module.exports = function (app) {
  const plugin = {
    id: 'signalk-local-gui',
    name: 'Lokale GUI (Proxy)',
    description: 'Lokale Webseite über Signal K unter demselben Origin wie das App Dock anzeigen'
  };
  const DEFAULTS = { host: '192.168.192.66', port: 80, basePath: '/' };
  let cfg = { ...DEFAULTS };
  const prefix = '/plugins/' + plugin.id;
  plugin.schema = {
    type: 'object',
    properties: {
      host: { type: 'string', title: 'Ziel-Host / IP', default: DEFAULTS.host },
      port: { type: 'integer', title: 'HTTP-Port des Zielgeräts', minimum: 1, maximum: 65535, default: DEFAULTS.port },
      basePath: { type: 'string', title: 'Startpfad der Webseite', default: DEFAULTS.basePath }
    }
  };
  const base = () => '/' + String(cfg.basePath || '/').replace(/^\/+|\/+$/g, '');
  const pathFor = (rest) => (base() === '/' ? '' : base().replace(/\/$/, '')) + rest;
  plugin.start = function (options) {
    cfg = { ...DEFAULTS, ...options };
    if (app.setPluginStatus) app.setPluginStatus('HTTP-Proxy auf http://' + cfg.host + ':' + cfg.port + base());
  };
  plugin.stop = function () {};

  function proxy(req, res) {
    const path = req.originalUrl.split('?')[0];
    if (path === prefix + '/site') return res.redirect(302, prefix + '/site/' + req.originalUrl.substring(path.length));
    const upstreamPath = pathFor(req.url || '/');
    const headers = { ...req.headers, host: cfg.host + ':' + cfg.port };
    for (const key of ['cookie', 'authorization', 'referer', 'origin']) delete headers[key];
    // Hat ein Body-Parser des Signal-K-Servers den Request-Body schon gelesen,
    // ist der Stream leer. Dann den geparsten Body wieder serialisieren und senden.
    let bodyBuf = null;
    if (req.method !== 'GET' && req.method !== 'HEAD' && req.readableEnded) {
      const ct = String(req.headers['content-type'] || '');
      const b = req.body;
      if (Buffer.isBuffer(b)) bodyBuf = b;
      else if (typeof b === 'string') bodyBuf = Buffer.from(b);
      else if (b && /json/i.test(ct)) bodyBuf = Buffer.from(JSON.stringify(b));
      else if (b && /urlencoded/i.test(ct)) bodyBuf = Buffer.from(new URLSearchParams(b).toString());
      else bodyBuf = Buffer.alloc(0);
      headers['content-length'] = String(bodyBuf.length);
      delete headers['transfer-encoding'];
    }
    // Unkomprimierte Antworten erlauben HTML- und CSS-Umschreibung.
    headers['accept-encoding'] = 'identity';
    const up = http.request({ host: cfg.host, port: cfg.port, path: upstreamPath, method: req.method, headers, timeout: 15000 }, (ur) => {
      const h = { ...ur.headers };
      delete h['x-frame-options'];
      delete h['content-security-policy'];
      delete h['content-security-policy-report-only'];
      const origin = 'http://' + cfg.host + (Number(cfg.port) === 80 ? '' : ':' + cfg.port);
      const upstreamBase = base();
      const proxied = prefix + '/site';
      const rewrite = str => str
        .split(origin + '/').join(proxied + '/')
        .split('"' + upstreamBase).join('"' + proxied + (upstreamBase === '/' ? '/' : '/'))
        .split("'" + upstreamBase).join("'" + proxied + '/');
      if (h.location) {
        if (h.location.startsWith(origin + '/')) h.location = proxied + h.location.slice(origin.length);
        else if (h.location.startsWith('/')) h.location = proxied + h.location;
      }
      const contentType = h['content-type'] || '';
      if (ur.statusCode === 200 && /text\/html/i.test(contentType)) {
        const chunks = [];
        let length = 0;
        ur.on('data', c => {
          length += c.length;
          if (length > 5 * 1024 * 1024) { up.destroy(new Error('HTML zu groß')); return; }
          chunks.push(c);
        });
        ur.on('end', () => {
          let body = Buffer.concat(chunks).toString('utf8');
          body = rewrite(body);
          // Häufige absolute URLs (/css, /js, /api) auf den Proxy umbiegen.
          body = body.replace(/(\b(?:src|href|action)\s*=\s*["'])\/(?!\/)/gi, '$1' + proxied + '/');
          delete h['content-length']; delete h['content-encoding'];
          res.writeHead(200, h); res.end(body);
        });
      } else {
        res.writeHead(ur.statusCode || 502, h);
        ur.pipe(res);
      }
    });
    up.on('timeout', () => up.destroy(new Error('Zeitüberschreitung')));
    up.on('error', err => {
      if (!res.headersSent) res.status(502).type('text/plain').send('Lokale GUI nicht erreichbar (' + cfg.host + ':' + cfg.port + '): ' + err.message);
      else res.end();
    });
    if (bodyBuf) up.end(bodyBuf); else req.pipe(up);
  }
  plugin.registerWithRouter = function (router) {
    router.get('/info', (req, res) => res.json({ url: prefix + '/site/', host: cfg.host, port: cfg.port, basePath: cfg.basePath }));
    router.use('/site', proxy);
  };
  return plugin;
};
