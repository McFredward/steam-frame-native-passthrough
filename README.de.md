# Steam Link Native Passthrough

<p align="center">
  <img src="docs/img/steam-link-icon.png" width="56" height="56" alt="Steam Link">
  &nbsp;&nbsp;
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/img/steam-frame-logo-dark.svg">
    <img src="docs/img/steam-frame-logo.svg" width="240" alt="Steam Frame">
  </picture>
</p>

<p align="center">
  <a href="README.md"><img src="docs/img/flag-en.png" width="24" alt="English">&nbsp;English</a>
  &nbsp;&nbsp;|&nbsp;&nbsp;
  <img src="docs/img/flag-de.png" width="24" alt="Deutsch">&nbsp;<b>Deutsch</b>
</p>

> [!IMPORTANT]
> **Unterstütztes SteamVR auf der Frame: 2.18.2 · Build 1791331621**

**Für Steam Frame.** Spiele unterstützte PC-VR-Spiele über Steam Link in deinem echten Zimmer.

## Natives Passthrough — keine Farbersetzung

Das Spiel überträgt separat, welche Bildbereiche durchsichtig sind; alle
Spielfarben bleiben erhalten. Die Frame verbindet das mit ihrem eigenen
Live-Kamerabild, auch bei teilweiser Transparenz; das Kamerabild muss nicht zum PC.

**[Stabilen Frame-Empfänger installieren](docs/INSTALL.md)** ·
**[Gekoppelte PC- und Frame-Vorschau testen](docs/PC-FRAME-SETUP.md#start-the-assistant)** ·
[Kompatibilität](docs/COMPATIBILITY.md)

Die weiterführenden Anleitungen sind auf Englisch.

## Wähle deine Einrichtung

| Variante | Was dich erwartet | Installation |
|---|---|---|
| **Stabil: nur Frame** | Natives Passthrough ist im Headset bestätigt. Die Transparenz teilt sich Platz im Videobild und kann die Schärfe verringern. | [Desktop-Modus](docs/INSTALL.md#directly-on-the-frame) oder [SSH](docs/INSTALL.md#from-a-pc-over-ssh) |
| **Vorschau: PC + Frame** | Windows + NVIDIA + HEVC. Soll die normale RGB-Auflösung erhalten; **noch nicht im gemeinsamen Headset-Test bestätigt**. | [Einrichtungsassistent mit einem Befehl](docs/PC-FRAME-SETUP.md#start-the-assistant) |

Die Vorschau wird separat gewählt. Sie ist noch keine bestätigte Lösung für
Bildschärfe oder Steam-Menüs. Siehe [Grenzen und Updates](docs/COMPATIBILITY.md).

## Unterstützte Spiele

| Spiel | Benötigter Mod | Mindestversion |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** (noch nicht veröffentlicht) |

Andere Spiele benötigen eine eigene Integration. Dieser Patch allein ergänzt
kein Mixed Reality in einem unveränderten Spiel. Vorschau-Tests benötigen einen
passenden Vorschau-Build der Spielintegration.

## Loslegen

1. Wähle oben eine Installationsvariante. Lass die Frame eingeschaltet und beende deinen VR-Stream.
2. Starte das Spiel wie gewohnt und aktiviere dessen natives Steam-Link-Passthrough.
3. Schalte die Option aus, wenn du das normale Bild zurückhaben möchtest.

Die Installation bleibt zwischen Sitzungen erhalten. Für **Aktualisieren**,
**Deinstallieren** oder **Rücknahme** nutze den
[gekoppelten Assistenten](docs/PC-FRAME-SETUP.md#update-uninstall-and-undo), oder die
[stabile Frame-Anleitung](docs/INSTALL.md#update), wenn du nur diesen Empfänger installiert hast.

## Für Entwickler

[Eigenes Spiel integrieren](docs/DEVELOPING.md) ·
[Laufzeit-Kompatibilität und Grenzen](docs/COMPATIBILITY.md)

## Credits

Basiert auf [Jake Luries steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Die Historie und [MIT-Lizenz](LICENSE) bleiben erhalten; die native Transparenz
entwickeln McFredward und Mitwirkende in diesem Projekt weiter.

Inoffizielles Projekt ohne Verbindung zu Valve.
