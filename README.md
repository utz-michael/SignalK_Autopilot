# SignalK_Autopilot

Lass uns von vorne beginnen mit dem Autopiloten für mein Boot. Hier sind Stichpunkte.
1. Autopilot soll über vorhandenes Signal K gesteuert werden
2. Der Autopilot selbst läuft auf einem ESP32-Dev-Board mit WiFi und Schrittmotor-Pololu-Treiber für Nema17-Schrittmotor.
3. Der Schrittmotor treibt über ein Reibrad das Lenkrad am Außendurchmesser an.
4. Signal k hat GPS, Kompass und Ruderstellung.
5. GPS ist ein Garmin 24xd mit NMEA‑2000-Schnittstelle.
6. Kompass ist der Maretron SSC 3000 mit NMEA 2000‑Schnittstelle. Stellt MAG-Heading, Nick, Roll und ROT zur Verfügung.
7. Ruder-Ausschlag ist von −40° bis +40° 
8. Das Boot ist ein Motorboot als Vollgleiter mit Z‑Antrieb. 
9. Der Autopilot soll nur in Verdrängerfahrt bis max. 15 km/h genutzt werden. Dies bedeutet aufgrund der Information von Punkt 8, dass das Boot nicht kursstabil ist und stark schlingert.
10. Konfiguration über Webseite lokal auf dem ESP.

Zu “Pilot” v0.1.0 habe ich keine Dokumentation gefunden, ich kann also nur nach der Beschreibung urteilen. Als “Bedienkopf für jeden Signal-K-v2-Autopilot” ist es eine reine UI und ersetzt höchstens die PyPilot New-UI. Die Brücke zwischen Signal K und deinem ESP spart es nicht. Auch “Pilot” schickt seine Befehle an die Autopilot-API, und diese Anfragen werden vom Signal-K-Server geprüft und dann an ein Provider-Plugin weitergereicht, das sie am Autopilotgerät ausführt. Ohne Provider-Plugin kommt kein Befehl bei deinem ESP an. 
uniparthenope

Einfachster Weg: ein eigenes, minimales Provider-Plugin

Dann brauchst du kein pypilot-Gerüst, weder Socket.IO noch pypilot-Nachbau, und die UI ist beliebig austauschbar (New-UI, Pilot, KIP ...):

UI → Autopilot-API v2 → eigenes Provider-Plugin (kleines JS) → ESP

Das Plugin hat nur diese Aufgaben:

Es registriert den ESP als Autopilotgerät und implementiert die geforderten API-Methoden (Engage, Disengage, Modus, Zielkurs, Zielkurs verstellen). Ein Provider muss alle Methoden der Schnittstelle umsetzen, den Engaged-Zustand pflegen und den Zustand auf offline setzen, wenn das Gerät nicht erreichbar ist. 
uniparthenope
Es gibt die Befehle an den ESP weiter.
Es meldet den ESP-Status über autopilotUpdate zurück. Damit werden die Status-Deltas steering.autopilot.* für V1 und V2 automatisch an die Clients verteilt. 
uniparthenope

Das sind grob 100-150 Zeilen JavaScript, deutlich weniger Aufwand als die Shim. Auf dem ESP muss dann nur ein einfaches JSON-Protokoll laufen (Modus, Zielkurs, Status), kein Socket.IO.

Ob “Pilot” v0.1.0 alle API-Funktionen sauber nutzt, kann ich nicht beurteilen. Bei einer so frühen Version würde ich die Bedienung einfach ausprobieren, nachdem das Plugin läuft.
Das Plugin ist klein. Ich lege es als installierbares Signal-K-Plugin an. Die Methoden folgen dem Beispiel aus der Signal-K-Doku.

Das Plugin ist fertig (index.js und package.json). Die Syntax ist geprüft, getestet habe ich es nicht.

Installation auf dem Raspberry
mkdir -p ~/.signalk/node_modules/signalk-esp-autopilot-provider
# index.js und package.json dorthin kopieren
cd ~/.signalk/node_modules/signalk-esp-autopilot-provider && npm install
# Signal K neu starten, Plugin im Admin-UI aktivieren, ESP-URL eintragen
Was das Plugin tut
Es registriert das Gerät esp-pilot an der Autopilot-API v2, mit den Zuständen standby und auto und dem Modus compass.
Es verbindet sich als WebSocket-Client mit dem ESP und versucht alle 3 s einen Neuaufbau. Bricht die Verbindung ab, setzt es den Zustand auf off-line. Befehle bei getrennter Verbindung schlagen mit einem Fehler fehl, statt still verloren zu gehen.
Tack, Gybe und Dodge werfen wie in der Doku vorgesehen einen Fehler. Wind- und GPS-Modus gibt es bewusst nicht.
WebSocket-Protokoll (JSON, Winkel in Grad)
Richtung	Nachricht
Plugin → ESP	{"cmd":"engage"}, {"cmd":"disengage"}, {"cmd":"mode","value":"compass"}, {"cmd":"target","value":123.0}, {"cmd":"adjust","value":-10}
ESP → Plugin	{"state":"auto","mode":"compass","target":123.0,"heading":121.5,"rudder":-4.2} bei Änderung und ca. 2 Hz

Auf dem ESP läuft dazu ein WebSocket-Server auf Port 81, per AsyncWebSocket oder WebSocketsServer. Die Verbindung zu Signal K für Kurs, ROT und Ruderwinkel ist ein zweiter, eigener Client.

Zu prüfen
Die Methoden-Signaturen habe ich aus dem Beispiel der Signal-K-Doku übernommen. Welche Felder autopilotUpdate genau akzeptiert, sollte der erste Test mit deiner Server-Version zeigen. Schau dafür ins Plugin-Log.


Ob “Pilot” v0.1.0 bzw. die New-UI mit den Zustandsnamen standby/auto sauber umgehen, sehen wir erst beim Ausprobieren. Zur Not passen wir die Namen an.

#define STEP_PIN    2
#define DIR_PIN     1
#define ENABLE_PIN  3

Als Nächstes kann ich die ESP32-Firmware schreiben: WebSocket-Server, Signal-K-Client, Kaskadenregler, Schrittmotor, Failsafes und Konfig-Webseite. Wenn du willst, fange ich mit dem Grundgerüst an. Dafür bräuchte ich nur die Pins für STEP, DIR und ENABLE des Pololu-Treibers.
