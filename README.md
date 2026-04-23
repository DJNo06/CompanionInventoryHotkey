# Companion Inventory Hotkey

Plugin F4SE pour Fallout 4 permettant d'ouvrir rapidement l'inventaire du compagnon actif, ainsi que d'acceder a certaines fonctions de l'Atelier de Sanctuary depuis n'importe ou.

Version actuelle : `2.0.0`

## Fonctionnalites

- Touche configurable via MCM pour ouvrir l'inventaire du compagnon actif
- Compatibilite avec le systeme vanilla `Followers`
- Support des compagnons classiques et de Canigou
- Conversion de l'Argent d'avant-guerre en Capsules
- Depot et retrait rapides des Capsules via l'Atelier de Sanctuary
- Ouverture directe du menu d'echange de l'Atelier de Sanctuary depuis n'importe quelle cell
- Stabilite amelioree avec un polling moins agressif

## Raccourcis

- `F6` par defaut : ouvre l'inventaire du compagnon actif
- `$` : convertit l'Argent d'avant-guerre en Capsules puis depose les Capsules dans l'Atelier de Sanctuary
- `Ctrl + $` : recupere les Capsules depuis l'Atelier de Sanctuary
- `Maj + $` : ouvre le menu d'echange de l'Atelier de Sanctuary

## Important

- Les raccourcis utilisant `$` sont prevus pour les claviers `AZERTY`
- Les actions Atelier/Capsules ciblent uniquement l'Atelier de Sanctuary (`000250FE`)
- La touche configurable via MCM concerne uniquement l'inventaire du compagnon

## Prerequis

- Fallout 4 `1.11.191`
- F4SE `0.7.7`
- Mod Configuration Menu (`MCM`) optionnel mais recommande

## Installation

1. Installez le contenu du dossier `Data` dans votre installation Fallout 4, ou via votre gestionnaire de mods
2. Lancez le jeu via `F4SE`
3. Si vous utilisez `MCM`, reglez la touche du compagnon dans le menu du mod

## Releases

- DLL et package FR disponibles dans les [GitHub Releases](../../releases)

## Build

### Prerequis developpement

- [XMake](https://xmake.io)
- Visual Studio / MSVC avec support `C++23`
- Sous-modules git initialises

### Recuperation

```bat
git clone --recurse-submodules https://github.com/DJNo06/CompanionInventoryHotkey.git
cd CompanionInventoryHotkey
```

### Compilation

```bat
xmake f -m releasedbg -y
xmake build -y CompanionInventoryHotkey
```

Le binaire est genere dans `build/windows/x64/releasedbg/`.

Dans cette configuration du repo, la build `releasedbg` copie aussi automatiquement la DLL dans :

```text
C:\Program Files (x86)\Steam\steamapps\common\Fallout 4\Data\F4SE\Plugins\
```

## Credits

- Equipe F4SE
- Developpeurs CommonLibF4
- Equipe MCM

Base initiale du projet issue du template CommonLibF4.
