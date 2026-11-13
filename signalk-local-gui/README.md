# Signal K Lokale GUI – eigenständige Webapp

Separat installierbare Variante nach dem Proxy-Prinzip von `signalk-victron-gui`:
https://github.com/utz-michael/SignalK_Victron_GUI

Ziel: `http://192.168.192.66/` (konfigurierbar). Keine Änderung an `signalk-victron-gui`.

## Installation

Auf dem Signal-K-Rechner den Ordner `signalk-local-gui` ablegen, dann:

```sh
cd ~/.signalk
npm install /pfad/zum/signalk-local-gui
sudo systemctl restart signalk
```

Unter **Server → Plugin Config → Lokale GUI (Proxy)** aktivieren / Zieladresse konfigurieren / speichern.
Unter **Webapps** heißt die App **Lokale GUI 192.168.192.66**.
Direktaufruf: `http://SIGNALK-IP:3000/signalk-local-gui/`
Im App Dock ggf. **Discover Installed Webapps** ausführen.

## Wichtige Grenzen

- Das Zielgerät muss vom Signal-K-Rechner aus via HTTP erreichbar sein.
- Der Reverse-Proxy arbeitet nur mit HTTP, **nicht mit WebSockets**. Absolute URLs, JavaScript-fetch-Aufrufe und spezielle Authentifizierung können Anpassungen erfordern.
- Falls das Plugin nicht aktiv ist, benutzt die Webapp die direkte Adresse. In diesem Fallback sind Doppelklicks im App Dock wegen Cross-Origin nicht unbedingt erkennbar.
- Nur im vertrauenswürdigen LAN einsetzen. Der Proxy entfernt einige Frame-Sicherheitsheader; nicht ins Internet freigeben.
- Die konkrete Webseite auf 192.168.192.66 konnte hier nicht live getestet werden.
