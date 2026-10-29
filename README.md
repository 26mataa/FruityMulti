# FruityMulti

Produire à deux dans FL Studio, chacun sur son PC, sur le **même projet** —
sans partage d'écran.

L'un clique **HÉBERGER** dans la fenêtre du plugin, l'autre clique
**REJOINDRE** et saisit l'adresse. À partir de là : notes de chaque canal,
playlist, tempo, position de lecture, mixeur et réglages d'effets sont
synchronisés.

Aucun son n'est transmis : chacun entend son propre FL. Il faut donc la même
version de FL Studio et les mêmes instruments des deux côtés.

## Installation

1. Fermer FL Studio.
2. Télécharger **FruityMultiSetup.exe** dans [Releases](../../releases) et le
   lancer. Windows demande l'autorisation : c'est l'installation du plugin.
3. L'installateur télécharge la dernière version et met tout en place.
4. Lancer FL Studio et ajouter l'effet **FruityMulti Bridge** sur une piste du
   mixeur.
5. Ouvrir la fenêtre du plugin (le bouton d'édition de l'effet).

## La fenêtre

| Élément | Rôle |
|---|---|
| **HÉBERGER** | ouvre la session, affiche l'adresse à donner à l'autre |
| **REJOINDRE** | se connecte à l'adresse saisie |
| **Copier** | met l'adresse dans le presse-papiers |
| point d'état | orange = en attente, vert = connecté |
| **Vérifier les mises à jour** | interroge GitHub ; si une version plus récente existe, le bouton devient **Installer la version N** et lance l'installateur, qui télécharge et installe (il demande de fermer FL) |

## Installateur et mises à jour

Le dépôt sert deux choses : le code source (ici) et les **binaires**, publiés en
releases. Les fichiers y portent toujours les mêmes noms, ce qui permet à
l'installateur de viser l'adresse `releases/latest/download/<fichier>`.

`FruityMultiSetup.exe` (`setup/setup.cpp`) fait tout :
téléchargement, demande d'élévation, attente de la fermeture de FL, copie du
plugin dans `%CommonProgramFiles%\VST3` et de l'agent dans
`%LOCALAPPDATA%\FruityMulti`. Il se recopie lui-même pour servir aux
prochaines mises à jour.

Un plugin VST3 ne peut pas remplacer son propre fichier pendant que FL tourne :
c'est pour cela que l'installateur attend que FL soit fermé.

## Le modèle d'autorité

- **L'hôte fait foi** sur le projet : tempo, motif courant, transport.
- **Les deux composent** : les notes de chacun arrivent sur le bon canal.
- **Les deux mixent** : volumes, pans, mutes, master.
- **Les effets** : l'invité voit ceux de l'hôte, reçoit le chemin exact à poser,
  et les réglages se synchronisent quand le même effet est présent des deux
  côtés.

## Technique

| Élément | Où |
|---|---|
| Plugin VST3 (C++), l'effet de mixeur et sa fenêtre | `dll/FruityMulti/` |
| Agent Python, la synchronisation | `net/agent_sync.py` |

Le plugin lit et écrit directement dans le moteur de FL (`FLEngine_x64.dll`) et
expose une petite API texte sur `127.0.0.1:47831`. L'agent s'y connecte, parle
au pair par le réseau, et applique les changements.

### Compiler le plugin

Le SDK VST3 n'est pas versionné ici (169 Mo). Le placer dans `dll/vst3sdk`
avant de compiler :

```powershell
git clone --depth 1 https://github.com/steinbergmedia/vst3sdk dll\vst3sdk
cmake -S dll\FruityMulti -B dll\build-fm -G "Visual Studio 17 2022" -A x64
cmake --build dll\build-fm --config Release --target FruityMultiBridge
```

Résultat :
`dll\build-fm\VST3\Release\FruityMultiBridge.vst3\Contents\x86_64-win\FruityMultiBridge.vst3`

### Construire l'agent

```powershell
pip install pyinstaller
python -m PyInstaller --onefile --name fm_agent --distpath net\dist --workpath net\build --specpath net net\agent_sync.py
```

## Règles apprises à la dure

- **FL doit être fermé** pendant l'installation du plugin.
- Les adresses Ghidra sont en base `0x400000` : `RVA = adresse - 0x400000`.
- Les textes de FL sont en **UTF-16**, pas en ASCII.
- Le moteur de FL ne se touche que **depuis le thread de l'interface**.
