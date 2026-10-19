# Steam Link Native Passthrough installieren

[English](INSTALL.md)

Die **experimentelle gekoppelte Einrichtung für PC und Frame** benötigt beide
Komponenten. Eine reine Frame-Installation wird nicht mehr angeboten.
Unterstützt werden **Windows + NVIDIA + HEVC** und auf der Frame
**SteamVR 2.18.2, Build 1791331621** mit passenden Laufzeitdateien.
Die [Kompatibilitätsseite](COMPATIBILITY.md) nennt die Grenzen.

## Direkt auf der Frame vorbereiten

1. Aktiviere Entwicklermodus und SSH in den Frame-Einstellungen und setze das
   Benutzerpasswort.
2. Öffne im Desktop-Modus ein Terminal. SSH dauerhaft aktivieren:

   ```bash
   sudo systemctl enable --now sshd.service
   ```

3. Prüfe den Zustand, auch nach einem Neustart:

   ```bash
   systemctl is-enabled sshd.service
   systemctl is-active sshd.service
   ```

   Erwartet werden `enabled` und `active`. Lass den Entwicklermodus eingeschaltet.
4. Lass die Frame wach und im selben Netzwerk wie den PC; beende den VR-Stream.
5. Öffne **auf deinem Windows-Gaming-PC** PowerShell und starte den Assistenten:

   ```powershell
   irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
   ```

   Er installiert beide Komponenten und bringt Python sowie die benötigte
   Oberfläche selbst mit. Folge den Verbindungs- und Bestätigungsdialogen.

## Vom PC über SSH

Derselbe Assistent sucht die Frame und verbindet sich per SSH. Nur falls die
Suche fehlschlägt, musst du ihre aktuelle WLAN-IP eingeben. Das Passwort wird
innerhalb der Oberfläche abgefragt und nur für diese Einrichtungssitzung gehalten.

Ist SSH noch nicht aktiv, nutze die Desktop-Schritte oben. Ein manueller
Verbindungstest lautet `ssh steamos@<frame-ip>`. Die aktuelle Adresse steht in den
WLAN-Einstellungen der Frame; sie kann sich ändern. Weitere Hinweise bietet
Valves [Entwickleranleitung](https://partner.steamgames.com/doc/steamhardware/steamframe/setup).

Nach der Prüfung bietet der Assistent Installieren, Aktualisieren oder Reparieren
an. Lass die Frame während Neustart und Bildprüfung eingeschaltet. Beim Ablegen
der Dateien bleibt ihre Oberfläche zunächst aktiv; anschließend prüft der
Assistent den erforderlichen Neustart. SteamVR auf dem PC bleibt geschlossen.
Starte den Stream danach selbst wie gewohnt.

Unter Linux sind Prüfung und unterstützte Wartung der Frame möglich:

```bash
curl -fsSL https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.sh | bash
```

Ein nativer Linux-PC-Sender wird noch nicht unterstützt. Proton ersetzt den
Linux-SteamVR-Treiberhost nicht durch einen Windows-Treiberhost.

## Spielen und aktualisieren

Starte Steam Link und ein [unterstütztes Spiel](../README.de.md#unterstützte-spiele).
Aktiviere dort natives Steam-Link-Passthrough. Fehlt eine passende Komponente,
bleibt das normale Bild erhalten. Dieselbe Option schaltet es wieder aus; einen
automatischen Ablauf-Timer gibt es nicht.

Starte für Update, Reparatur, Deinstallation oder Rücknahme denselben Assistenten
erneut. Er prüft zuerst den vorhandenen Zustand und erhält unbekannte oder
veränderte Dateien. Ein SteamVR-/Steam-Link-Update kann die genaue
Kompatibilitätsprüfung scheitern lassen; natives Passthrough bleibt dann aus,
bis ein kompatibles Patch-Update verfügbar ist.

[Deinstallation, Rücknahme, Logs und Wiederherstellung](PC-FRAME-SETUP.md#update-uninstall-and-undo)
sind im Detail beschrieben. Strg+C bricht ohne automatische Rücknahme ab; bewahre
die angezeigten Log- und Wiederherstellungspfade auf. Eine Rücknahme ist erst
fertig, wenn der vorherige Zustand geprüft und die Frame-Oberfläche wieder aktiv ist.

Alte reine Frame-Releases sind [historische Archive](history/ATLAS-RECEIVER.md).
Ihre Installations- oder Aktivierungsbefehle gelten nicht für den aktuellen Stand.
