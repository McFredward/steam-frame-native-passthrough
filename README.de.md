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
> **Aktuell nur für <img src="docs/img/windows.svg" width="18" height="18" alt=""> Windows-PCs mit <img src="docs/img/nvidia.svg" width="20" height="20" alt=""> NVIDIA-GPU.**
>
> **Getestet mit Steam Frame SteamVR 2.18.2 · Build 1791331621**
>
> Ungetestete SteamVR-Versionen erfordern eine zusätzliche Bestätigung. Passthrough bleibt inaktiv, wenn die benötigten nativen Komponenten nicht verifiziert werden können.

Spiele unterstützte PC-VR-Spiele über Steam Link in deinem echten Zimmer.

## Native Transparenz, kein Farbersatz

Das Spiel sendet Transparenz getrennt von seinen Farben; keine Farbe wird aus
dem Bild entfernt. Die Frame verbindet das Spiel mit ihrem eigenen Kamerabild,
auch bei Teiltransparenz; das Kamerabild bleibt auf der Brille.

## Unterstützte Spiele

| Spiel | Benötigte Mod | Mindestversion |
|---|---|---|
| Gloomhaven Digital | [GloomhavenVR](https://github.com/McFredward/GloomhavenVR) | **1.2.0** |

Andere Spiele benötigen eine Integration; der Patch allein reicht nicht aus.

## Installation

**SSH einmalig auf der Steam Frame aktivieren:**

1. Öffne **Steam-Einstellungen → System → Entwicklermodus aktivieren**.
2. Öffne **Entwickler → Benutzerpasswort festlegen** und wähle ein Passwort.
3. Öffne im Desktop-Modus ein Terminal und aktiviere SSH dauerhaft:

   ```bash
   sudo systemctl enable --now sshd.service
   ```

[Valves Geräteanleitung](https://partner.steamgames.com/doc/steamhardware/steamframe/setup) zeigt die Einstellungen.
Lass die Frame eingeschaltet und im selben Netzwerk wie den PC; beende den VR-Stream.

**Auf deinem Windows-Gaming-PC ausführen — PowerShell:**

```powershell
irm https://raw.githubusercontent.com/McFredward/steam-link-native-passthrough/main/get.ps1 | iex
```

Der Assistent findet die Frame, prüft beide Geräte und führt durch die Installation.
Er lädt seine Werkzeuge selbst herunter. Gib auf Nachfrage das Frame-Passwort ein
und lass sie bis zum Abschluss eingeschaltet.

Starte deinen Steam-Link-Stream und aktiviere natives Steam-Link-Passthrough im Spiel.
Dieselbe Zeile bietet später **Update, Deinstallation oder Rücknahme** an.

## Für Entwickler

[Spiel integrieren](docs/DEVELOPING.md) · [Kompatibilität](docs/COMPATIBILITY.md) ·
[Referenz für Einrichtung und Wiederherstellung](docs/PC-FRAME-SETUP.md)

## Credits

Auf Grundlage von [Jake Luries steam-frame-chroma-key](https://github.com/spyjacob/steam-frame-chroma-key).
Die Vorgeschichte und [MIT-Lizenz](LICENSE) bleiben erhalten; native Transparenz
wird hier von McFredward und Mitwirkenden entwickelt.

Inoffizielles Projekt, nicht mit Valve verbunden.
