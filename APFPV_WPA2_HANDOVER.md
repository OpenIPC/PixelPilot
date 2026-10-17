# Übergabe: WPA2-PSK für den APFPV-Modus

Einstieg für eine neue Claude-Sitzung. Antworten auf **Deutsch**. Der Nutzer baut die App selbst
in Android Studio. **Keine eigenen Build- oder Testschleifen.** Er schickt Compiler-Fehler und Logs.
Ziel ist, sparsam mit Token umzugehen: nur die Stellen lesen, die man ändert, nicht ganze Dateien.

## Stand

- Repo `C:\Users\david\Documents\Claude\PixelPilot_apk`, Branch `ap-mode`, **nichts committet**.
  Vor Etappe 1 den Nutzer fragen, ob er erst committen will.
- **APFPV-Modus:** Der USB-Adapter (devourer, Userspace-Treiber, RTL8812EU) ist als Station mit dem
  **offenen** AP der OpenIPC-apfpv-Kamera verbunden. Die Kamera hat 192.168.0.1, das Handy die feste
  IP 192.168.0.10. Video kommt per RTP/UDP auf Port 5600.
- Läuft auf dem Gerät:
  - Verbinden ohne Scan
  - CSA folgen (inkl. Kanalbreite 40/80)
  - Rejoin nach Deauth oder TSF-Sprung direkt zur bekannten BSSID
  - Dropouts beenden die Assoziation nicht
- Kernklasse ist `app/wfbngrtl8812/src/main/cpp/StaLink.{h,cpp}`. Sie enthält den
  Zustandsautomaten Searching → Authenticating → Associating → Connected, ARP/ICMP/TCP-Client und den
  Worker-Thread. `send_packet` muss außerhalb des RX-Threads laufen, deshalb gibt es eine TX-Queue.
- JNI liegt in `WfbngLink.cpp` (`nativeSetApConfig`, …), Java in `WfbNgLink.java` und
  `VideoActivity.java` (`showApSettingsDialog`, Prefs `ap_*`).

## Fakten aus der Prüfung

- **devourer hat keine Verschlüsselung für eine Station.** Der Sicherheits-CAM des Chips wird beim
  Init geleert, und für Jaguar3 fehlen die TX/RX-Deskriptorfelder für Verschlüsselung. Geschützte
  Frames kommen roh an (RCR accept-all), selbst gebaute verschlüsselte Frames lassen sich per
  `send_packet` senden, HW-ACK funktioniert.
- **Vorlage auf AP-Seite:** `devourer/tests/ap_wpa2.cpp` (4-Way-Authenticator, Software-CCMP, nutzt
  OpenSSL) und `devourer/docs/ap-mode.md`.
  - Der CCMP-AAD maskiert in FC die Subtype-, Retry-, PwrMgmt- und MoreData-Bits und setzt Protected.
  - Er maskiert die Sequenznummer und lässt die Fragmentnummer stehen.
  - QoS-AAD enthält nur die TID.
  - Nonce = `prio(TID) | A2 | PN(6, big-endian)`.
  - CCMP-Header = PN0 PN1 0 (0x20|keyid<<6) PN2..PN5.
- **OpenSSL gibt es im NDK nicht.** Das vorhandene libsodium hat weder AES-CCM noch SHA1. Den
  BSD-lizenzierten hostap-Code übernehmen statt ihn neu zu schreiben:
  `devourer/reference/8821cu/core/crypto/` (aes-internal*, aes-ccm, aes-unwrap, sha1*). Vorher die
  Lizenzköpfe prüfen.
- **Kamera:** Die Stock-Firmware `/usr/bin/adapter` erzeugt `wpa=2`, `rsn_pairwise=CCMP`,
  `wpa_passphrase=$pass`, `auth_algs=1` und **kein** `ieee80211w`. Das ist reines WPA2-PSK/CCMP ohne
  PMF. hostapd kann zwar auch SAE.
  - Das Skript des Nutzers `C:\Users\david\Documents\Claude\OpenIPC\apfpv-open-static-cam.sh` schaltet
    WPA2 ab (wpa=0, MAC-ACL).
  - Für den Test braucht es eine Variante, die `wpa=2` behält und nur `disassoc_low_ack=0` setzt.
- **hostapd-Zeitlimits für EAPOL:** erstes Timeout 100 ms, dann je 1 s, 4 Versuche. Danach kommt ein
  Deauth mit Grund 15. Den GTK erneuert hostapd standardmäßig alle 86400 s.

## Entscheidungen (mit dem Nutzer abgestimmt)

- Nur **WPA2-PSK + CCMP**, `ieee80211w` 0 oder 1. Offene Netze bleiben unterstützt.
- Kein WPA3/SAE, keine PMF-Pflicht, kein TKIP. Erkennen und eine klare Statusmeldung anzeigen.
- Die PMK (PBKDF2-SHA1, 4096 Runden, SSID als Salt) **vorab in Java** mit
  `SecretKeyFactory("PBKDF2WithHmacSHA1")` beim Speichern berechnen und als 32 Byte an native
  übergeben. Neu berechnen, wenn sich SSID oder Passwort ändern, auch beim SSID-Ändern im
  `CameraControlDialog`.

## Etappe 1: Kern (nativ)

1. Krypto-Dateien von hostap übernehmen (AES-128-Block, CCM bzw. CCMP, AES-Key-Unwrap, HMAC-SHA1
   und PRF-384/512) und in `app/wfbngrtl8812/src/main/cpp/CMakeLists.txt` eintragen. Eine dünne
   Wrapper-Header `WpaCrypto.h` schreiben.
2. `StaConfig` um `std::array<uint8_t,32> pmk` und `bool use_wpa` erweitern.
3. **Netzwahl und Assoziation:**
   - Im Beacon oder der Probe-Response das RSN-IE (48) merken.
   - Wenn `use_wpa` gilt, ein RSN-IE in den Assoc-Request schreiben: Version 1, Gruppenchiffre aus
     dem AP-IE, paarweise CCMP, AKM PSK, Caps 0.
   - Dasselbe IE byte-genau in Nachricht 2 verwenden.
   - Ein Privacy-Bit, das nicht zur Konfiguration passt, als Hinweis in der Statuszeile melden.
4. **Neuer Zustand `Handshaking`** nach erfolgreicher Assoc-Response. EAPOL kommt mit LLC/SNAP und
   Ethertype 0x888E (in `handleMsdu` abzweigen).
   - **Nachricht 1:** ANonce lesen, Replay-Counter merken, SNonce zufällig erzeugen,
     PTK = PRF-384(PMK, "Pairwise key expansion", min/max(AA,SPA) + min/max(ANonce,SNonce)),
     KCK/KEK/TK ableiten, dann Nachricht 2 mit MIC (HMAC-SHA1 über KCK, 16 Byte) und RSN-IE senden.
   - **Nachricht 3:**
     - Prüfen: MIC, Replay-Counter größer als bisher, ANonce gleich.
     - RSN-IE gegen das Beacon prüfen, bei Abweichung abbrechen.
     - Key-Data mit KEK entpacken (unwrap), GTK-KDE (00-0F-AC:1) parsen: Key-ID plus GTK.
     - RSC als Start-Replay-Zähler für die Gruppe übernehmen.
   - **Nachricht 4** unverschlüsselt senden, **danach** PTK und GTK installieren und Connected setzen.
   - EAPOL-Frames gehen nie über den CCMP-Pfad, solange kein PTK installiert ist.
   - **Group-Key-Handshake** (Rekey): Group-Nachricht 1, verschlüsselt, Key-Data entpacken, GTK
     ersetzen und Group-Nachricht 2 senden.
5. **RX** in `handleData`, wo heute steht „Protected: only open networks are supported“:
   - Duplikate über Sequenznummer und Retry vor dem Entschlüsseln verwerfen.
   - Key-ID aus dem CCMP-Header lesen: Unicast → PTK, Multicast → GTK.
   - CCM entschlüsseln und den 8-Byte-MIC prüfen. Bei falschem MIC verwerfen und zählen.
   - **Replay-Prüfung pro TID (16 Zähler) erst nach erfolgreichem MIC.**
   - Dann den bisherigen Pfad (LLC/A-MSDU) auf dem Klartext fortsetzen.
6. **TX** in `sendData` (ARP, ICMP, TCP): mit installiertem PTK das Protected-Bit setzen,
   CCMP-Header plus Chiffretext plus MIC anhängen. Der 48-Bit-PN wird unter dem Mutex streng
   hochgezählt. Null-Keepalive-Frames bleiben unverschlüsselt.
7. **`reconnect()` und alle Wege zurück zu Searching:** PTK, GTK, PN und Replay-Zähler löschen,
   **bevor** `beginAuth()` aufgerufen wird.
8. **Falsches Passwort erkennen:** Kommt Nachricht 1 mehrmals ohne Nachricht 3 oder folgt ein Deauth
   mit Grund 15 oder 2, Status „APFPV: 4-way handshake failed – wrong password?“ anzeigen und den
   Rejoin bremsen (Backoff 2 s).
9. JNI `nativeSetApConfig` um `byte[] pmk` erweitern (null bedeutet offen).
   `statusText()`/`printableAscii` bleiben, wie sie sind.

Danach testet der Nutzer: Kamera mit WPA2, App mit Passwort, dann Logcat-Tag `pixelpilot-sta`.

## Etappe 2: App und Doku (erst nach erfolgreichem Test)

- In `showApSettingsDialog` ein Passwortfeld ergänzen (leer bedeutet offen), Pref `ap_password`.
- PMK-Berechnung in einem Hintergrund-Thread laufen lassen, Ergebnis in `applyLinkConfig()`
  übergeben.
- Hinweis anzeigen, wenn die Kamera WPA3 oder PMF-Pflicht meldet.
- README-Abschnitt „Link modes: WFB-NG and APFPV“ aktualisieren. „Limitations: open networks only“
  ändern zu „open or WPA2-PSK (CCMP), no WPA3/SAE, no required PMF“.
- Optional eine Kamera-Skriptvariante mit WPA2 an.

## Was für das Recovering wichtig ist

- Ein Dropout ohne Deauth bleibt unverändert: Die Schlüssel bleiben gültig, Video läuft sofort weiter.
- Ein Rejoin kostet zusätzlich den 4-Way-Handshake, etwa 10–50 ms. Deshalb muss die PMK vorab
  berechnet sein, im Rejoin-Pfad darf kein PBKDF2 laufen.
- Deauth und Disassoc bleiben ungeschützt (kein PMF). Die bisherige Erkennung „Kamera kennt uns
  nicht mehr“ funktioniert weiter.
- Mit einem Schlüssel darf nie ein PN doppelt vorkommen. Software-Wiederholungen bekommen einen
  neuen PN.
