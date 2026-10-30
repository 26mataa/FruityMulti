"""FruityMulti - agent de synchronisation.

Relie le plugin local (127.0.0.1:47831) a un pair sur le reseau.
Le plugin expose FL ; cet agent ne fait que comparer et diffuser.

Utilisation
-----------
Hote   : python agent_sync.py --heberger          (affiche un code, ex. K7P2QX)
Invite : python agent_sync.py --rejoindre K7P2QX

Sans relais (reseau local, ou port ouvert sur la box) :
Hote   : python agent_sync.py --listen 5000       (affiche ip:port/CODE)
Invite : python agent_sync.py --peer ip:5000/CODE

Le plugin ne gere qu'un client a la fois : on garde donc UNE seule
connexion ouverte et on serialise les commandes.
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
from collections import Counter

# --- Mises a jour -----------------------------------------------------------
# Le depot public qui heberge les versions. L'agent y lit la derniere release
# et telecharge le plugin + lui-meme. A incrementer a chaque publication.
DEPOT = "26mataa/FruityMulti"
AGENT_VERSION = 29
ADRESSE_RELEASES = f"https://github.com/{DEPOT}/releases/latest"

PLUGIN_HOST = "127.0.0.1"
PLUGIN_PORT = 47831
POLL = 0.005


# --------------------------------------------------------------------------
# Inventaire des plugins du poste.
#
# Volontairement autonome : l'agent est distribue en un seul executables, donc
# il ne peut pas importer le module de re/tools. Aucune installation, on ne
# fait que lire des noms de fichiers.

DOSSIERS_VST3 = [r"C:\Program Files\Common Files\VST3",
                 r"C:\Program Files (x86)\Common Files\VST3"]
DOSSIERS_VST2 = [r"C:\Program Files\VstPlugins",
                 r"C:\Program Files\Steinberg\VSTPlugins",
                 r"C:\Program Files\Common Files\VST2",
                 r"C:\VST", r"C:\VSTPlugins"]


def _dossiers_fl():
    doc = os.path.expanduser("~\\Documents")
    bases = [os.path.join(doc, "Image-Line", "FL Studio", "Presets",
                          "Plugin database"),
             os.path.join(doc, "Image-Line", "FL Studio 21", "Presets",
                          "Plugin database"),
             os.path.join(doc, "Image-Line", "FL Studio 20", "Presets",
                          "Plugin database")]
    return [b for b in bases if os.path.isdir(b)]


def scan_plugins():
    """Renvoie {"fl": [...], "vst3": [...], "vst2": [...]}."""
    fl, vst3, vst2 = set(), set(), set()

    for base in _dossiers_fl():
        generateurs = os.path.join(base, "Generators")
        if not os.path.isdir(generateurs):
            continue
        for _racine, _dossiers, fichiers in os.walk(generateurs):
            for fichier in fichiers:
                if fichier.lower().endswith(".fst"):
                    fl.add(os.path.splitext(fichier)[0])

    for dossier in DOSSIERS_VST3:
        if not os.path.isdir(dossier):
            continue
        try:
            entrees = os.listdir(dossier)
        except OSError:
            continue
        for entree in entrees:
            chemin = os.path.join(dossier, entree)
            if entree.lower().endswith(".vst3"):
                vst3.add(os.path.splitext(entree)[0])
            elif os.path.isdir(chemin):
                try:
                    for sous in os.listdir(chemin):
                        if sous.lower().endswith(".vst3"):
                            vst3.add(os.path.splitext(sous)[0])
                except OSError:
                    continue

    for dossier in DOSSIERS_VST2:
        if not os.path.isdir(dossier):
            continue
        for racine, _dossiers, fichiers in os.walk(dossier):
            for fichier in fichiers:
                if fichier.lower().endswith(".dll"):
                    vst2.add(os.path.splitext(fichier)[0])
            if racine.count(os.sep) - dossier.count(os.sep) >= 2:
                _dossiers[:] = []

    return {"fl": sorted(fl, key=str.lower),
            "vst3": sorted(vst3, key=str.lower),
            "vst2": sorted(vst2, key=str.lower)}


def dossier_local():
    """%LOCALAPPDATA%\\FruityMulti : journal, inventaires, comparaison.

    Jamais le dossier courant : lance par FL, c'est Program Files.
    """
    dossier = os.path.join(os.environ.get("LOCALAPPDATA", "."), "FruityMulti")
    try:
        os.makedirs(dossier, exist_ok=True)
    except OSError:
        pass
    return dossier


def ecrire_compat(commun, ici_seul, autre_seul):
    """compat.txt, lu par la fenetre : "= nom" partout, "+ nom" ici
    seulement, "- nom" chez l'autre seulement. Vide = pas de session."""
    chemin = os.path.join(dossier_local(), "compat.txt")
    try:
        with open(chemin + ".tmp", "w", encoding="utf-8") as fichier:
            for prefixe, noms in (("-", autre_seul), ("+", ici_seul), ("=", commun)):
                for nom in noms:
                    fichier.write(f"{prefixe} {nom}\n")
        os.replace(chemin + ".tmp", chemin)
    except OSError:
        pass


def comparer_plugins(ici, autre):
    """(en commun, seulement ici, seulement chez lui), tous prefixes."""
    commun, ici_seul, autre_seul = [], [], []
    for cle in ("fl", "vst3", "vst2"):
        miens = {n.lower(): n for n in (ici or {}).get(cle, [])}
        siens = {n.lower(): n for n in (autre or {}).get(cle, [])}
        for minuscule in sorted(set(miens) | set(siens)):
            if minuscule in miens and minuscule in siens:
                commun.append(miens[minuscule])
            elif minuscule in miens:
                ici_seul.append(miens[minuscule])
            else:
                autre_seul.append(siens[minuscule])
    return commun, ici_seul, autre_seul


def copier_presse_papier(texte):
    """Met un texte dans le presse-papiers Windows.

    Sert a donner au pair le chemin exact d'un effet a glisser dans FL : il
    n'a plus qu'a coller dans le navigateur. Renvoie True si ca a marche.
    """
    if not texte:
        return False
    try:
        if os.name != "nt":
            return False
        import subprocess
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "$t = $args[0]; Set-Clipboard -Value $t", texte],
                       capture_output=True, timeout=10, check=False)
        return True
    except Exception:
        return False


# --------------------------------------------------------------------------
def meme_nom(a, b):
    """Deux noms de canal designent-ils le meme instrument ?

    FL renomme parfois : le preset "Twin 3.fst" donne le canal "Twin".
    On compare donc les mots, pas la chaine exacte.
    """
    x = (a or "").strip().lower()
    y = (b or "").strip().lower()
    if not x or not y:
        return False
    if x == y or x.startswith(y) or y.startswith(x):
        return True
    premier = x.split()[0]
    return len(premier) >= 3 and premier == y.split()[0]


# --------------------------------------------------------------------------
class PluginLost(Exception):
    """Le plugin a disparu : FL ferme, ou l'effet a ete retire."""


# --------------------------------------------------------------------------
class Plugin:
    """Client du plugin, avec une seule connexion reutilisee."""

    def __init__(self, host=PLUGIN_HOST, port=PLUGIN_PORT):
        self._presets = None
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.sock.settimeout(5.0)
        self.buf = b""
        self.lock = threading.Lock()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("plugin ferme")
            self.buf += chunk
        line, _, rest = self.buf.partition(b"\n")
        self.buf = rest
        return line.decode("utf-8", "replace").strip()

    def cmd(self, text):
        with self.lock:
            # UTF-8 : un simple tiret cadratin suffisait a faire planter
            # l'agent, qui parlait ASCII.
            self.sock.sendall((text + "\n").encode("utf-8"))
            first = self._readline()
            lines = [first]
            # Reponses multilignes : NOTES <motif> <n> ... END <n>
            if text.strip() == "POLL":
                while True:
                    nxt = self._readline()
                    lines.append(nxt)
                    if nxt.startswith("END"):
                        break
            elif first.startswith("LIST") or first.startswith("NOTES "):
                while True:
                    nxt = self._readline()
                    lines.append(nxt)
                    if nxt.startswith("END"):
                        break
            return lines

    # -- raccourcis --------------------------------------------------------
    def ping(self):
        return self.cmd("PING")[0] == "OK"

    def state(self):
        """Notes du motif courant, groupees par canal.

        Depuis le 28/10 20h15 on sait que FL stocke UNE liste de notes par
        motif, et que chaque note porte son canal (+0x06). La cle du
        dictionnaire est donc le canal, pas un index de liste.

        Retourne {canal: [(temps, duree, hauteur, velocite), ...]}
        """
        state = {}
        tokens = []
        for line in self.cmd("NOTES"):
            if line.startswith("NOTES "):
                tokens = []
                continue
            if line.startswith("END"):
                break
            tokens.extend(line.split())
        for tok in tokens:
            parts = tok.split(":")
            if len(parts) < 5:
                continue
            t, l, p, v, ch = (int(x) for x in parts[:5])
            state.setdefault(ch, []).append((t, l, p, v))
        for notes in state.values():
            notes.sort()
        return state

    def add(self, channel, note):
        t, l, p, v = note
        return self.cmd(f"ADD {channel} {t} {l} {p} {v}")[0]

    def add_many(self, channel, notes):
        """Ajoute un lot de notes en UNE requete (donc un seul rafraichissement).

        Note par note, chaque envoi declenchait un saut de pattern : coller ou
        annuler cinquante notes prenait plusieurs secondes.
        """
        if not notes:
            return None
        tokens = " ".join(f"{t}:{l}:{p}:{v}" for t, l, p, v in notes)
        return self.cmd(f"ADDX {channel} {len(notes)} {tokens}")[0]

    def del_many(self, channel, notes):
        """Supprime un lot de notes (temps:hauteur) en UNE requete."""
        if not notes:
            return None
        tokens = " ".join(f"{n[0]}:{n[2]}" for n in notes)
        return self.cmd(f"DELX {channel} {len(notes)} {tokens}")[0]

    def refresh(self):
        return self.cmd("REFRESH")[0]

    def tempo(self):
        """Tempo courant en BPM (float), ou None."""
        try:
            line = self.cmd("TEMPO")[0]
            if not line.startswith("TEMPO "):
                return None
            return int(line.split()[1]) / 1000.0
        except (OSError, ValueError, IndexError):
            return None

    def set_tempo(self, bpm):
        return self.cmd(f"TEMPO {bpm:.3f}")[0]

    def playing(self):
        try:
            line = self.cmd("ISPLAYING")[0]
            if line.startswith("PLAYING "):
                return line.split()[1] == "1"
        except (OSError, IndexError):
            pass
        return None

    def transport(self, playing):
        return self.cmd("PLAY" if playing else "STOP")[0]

    def pattern(self):
        try:
            line = self.cmd("PATTERN")[0]
            if line.startswith("PATTERN "):
                return int(line.split()[1])
        except (OSError, ValueError, IndexError):
            pass
        return None

    def set_pattern(self, number):
        return self.cmd(f"PATTERN {int(number)}")[0]

    def mouse(self):
        """Position du curseur dans FL, ou None s'il est dehors."""
        try:
            line = self.cmd("MOUSE")[0]
            if line.startswith("MOUSE "):
                _, x, y = line.split()
                return int(x), int(y)
        except (OSError, ValueError, IndexError):
            pass
        return None

    def cursor(self, x, y, label=""):
        if x < 0 or y < 0:
            return self.cmd("CURSOR-HIDE")[0]
        if label:
            return self.cmd(f"CURSOR {x} {y} {label}")[0]
        return self.cmd(f"CURSOR {x} {y}")[0]

    def cursor_f(self, nx, ny, label=""):
        """Curseur en coordonnees proportionnelles (0..1000).

        Les pixels dependent de la resolution et du zoom : deux FL differents
        affichaient le curseur au mauvais endroit.
        """
        if nx < 0 or ny < 0:
            return self.cmd("CURSOR-HIDE")[0]
        if label:
            return self.cmd(f"CURSORF {nx} {ny} {label}")[0]
        return self.cmd(f"CURSORF {nx} {ny}")[0]

    def track_volume(self, index):
        """Volume d'une piste du mixeur, de 0 a 1."""
        try:
            line = self.cmd(f"TRACKVOL {index}")[0]
            if line.startswith("TRACKVOL "):
                return float(line.split()[2])
        except (OSError, ValueError, IndexError):
            pass
        return None

    def set_track_volume(self, index, value):
        return self.cmd(f"TRACKVOL {index} {value:.6f}")[0]

    # --- panoramique, mute, separation stereo -------------------------------
    def _mix_read(self, command, index):
        """Lit une valeur du mixeur : renvoie le 3e champ de la reponse."""
        try:
            line = self.cmd(f"{command} {index}")[0]
            if line.startswith(command + " "):
                return line.split()[2]
        except (OSError, ValueError, IndexError):
            pass
        return None

    def track_pan(self, index):
        value = self._mix_read("TRACKPAN", index)
        return None if value is None else float(value)

    def set_track_pan(self, index, value):
        return self.cmd(f"TRACKPAN {index} {value:.6f}")[0]

    def track_stereo(self, index):
        value = self._mix_read("TRACKSTEREO", index)
        return None if value is None else float(value)

    def set_track_stereo(self, index, value):
        return self.cmd(f"TRACKSTEREO {index} {value:.6f}")[0]

    def track_mute(self, index):
        value = self._mix_read("TRACKMUTE", index)
        return None if value is None else int(value)

    def set_track_mute(self, index, value):
        return self.cmd(f"TRACKMUTE {index} {1 if value else 0}")[0]

    def mix_state(self, index):
        """Volume, pan, stereo et mute d'une piste en UNE requete.

        Quatre requetes separees saturaient le plugin : il traite tout sur le
        thread principal de FL, donc chaque aller-retour coute un cycle.
        """
        try:
            line = self.cmd(f"MIX {index}")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("MIX "):
            return None
        parts = line.split()
        if len(parts) < 6:
            return None
        try:
            return {
                "vol": int(parts[2]) / 1073741824.0,
                "pan": (int(parts[3]) / 1073741824.0) * 2.0 - 1.0,
                "stereo": (int(parts[4]) / 1073741824.0) * 2.0 - 1.0,
                "mute": int(parts[5]),
            }
        except (ValueError, IndexError):
            return None

    def _parse_status(self, line):
        """Decode la ligne STATUS (une seule ligne, tout l'etat)."""
        if not line.startswith("STATUS "):
            return None
        parts = line.split()
        if len(parts) < 26:
            return None
        try:
            mix = {}
            # parts[4] = master (piste 0), puis les pistes 1 a 8
            mvol, mpan, msep, mmute = parts[4].split(":")
            mix[0] = {
                "vol": int(mvol) / 1073741824.0,
                "pan": (int(mpan) / 1073741824.0) * 2.0 - 1.0,
                "stereo": (int(msep) / 1073741824.0) * 2.0 - 1.0,
                "mute": -1,
            }
            for index, token in enumerate(parts[5:5 + 16], start=1):
                vol, pan, sep, mute = token.split(":")
                mix[index] = {
                    "vol": int(vol) / 1073741824.0,
                    "pan": (int(pan) / 1073741824.0) * 2.0 - 1.0,
                    "stereo": (int(sep) / 1073741824.0) * 2.0 - 1.0,
                    "mute": int(mute),
                }
            mx, my, mw, mh = (int(parts[21]), int(parts[22]),
                              int(parts[23]), int(parts[24]))
            position = int(parts[25]) if len(parts) > 25 else 0
            return {
                "tempo": int(parts[1]) / 1000.0,
                "playing": parts[2] == "1",
                "pattern": int(parts[3]),
                "mix": mix,
                "mouse": (mx, my, mw, mh),
                "position": position,
            }
        except (ValueError, IndexError):
            return None

    def status(self):
        """Tout l'etat en UNE requete : tempo, lecture, pattern, mixage."""
        try:
            line = self.cmd("STATUS")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        return self._parse_status(line)

    def poll(self):
        """Etat ET notes du motif en UNE requete (plugin v14).

        Retourne (etat, notes) ou (None, None) si le plugin ne sait pas faire :
        l'appelant se rabat alors sur status() + state().
        """
        try:
            lines = self.cmd("POLL")
        except (OSError, TimeoutError, IndexError):
            return None, None
        if not lines or not lines[0].startswith("STATUS "):
            return None, None

        etat = self._parse_status(lines[0])
        if etat is None:
            return None, None

        notes = {}
        if len(lines) > 1 and lines[1].startswith("NOTES "):
            for tok in lines[1].split()[3:]:
                bits = tok.split(":")
                if len(bits) < 5:
                    continue
                try:
                    t, l, p, v, ch = (int(x) for x in bits[:5])
                except ValueError:
                    continue
                notes.setdefault(ch, []).append((t, l, p, v))
        for liste in notes.values():
            liste.sort()
        return etat, notes

    # --- channel rack -------------------------------------------------------
    def channels(self):
        """Liste des canaux : [(index, tranche de mixage), ...]."""
        try:
            line = self.cmd("CHANNELS")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("CHANNELS "):
            return None
        parts = line.split()
        out = []
        for token in parts[2:]:
            # format : index:tranche:nom  (le nom peut contenir des ':')
            bits = token.split(":", 2)
            if len(bits) < 2:
                continue
            try:
                index = int(bits[0])
                track = int(bits[1])
            except ValueError:
                continue
            name = bits[2] if len(bits) > 2 else ""
            out.append((index, track, name))
        return out

    def channel_mix(self):
        """Volume et panoramique de chaque canal : [(index, vol, pan)]."""
        try:
            line = self.cmd("CHANNELMIX")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("CHANNELMIX "):
            return None
        out = []
        for token in line.split()[2:]:
            bits = token.split(":")
            if len(bits) < 3:
                continue
            try:
                mute = int(bits[3]) if len(bits) > 3 else 1
                out.append((int(bits[0]), float(bits[1]), float(bits[2]), mute))
            except ValueError:
                continue
        return out

    def set_channel_volume(self, index, value):
        return self.cmd(f"CHANNELVOL {index} {value:.6f}")[0]

    def set_channel_mute(self, index, mute):
        return self.cmd(f"CHANNELMUTE {index} {1 if mute else 0}")[0]

    def set_channel_pan(self, index, value):
        return self.cmd(f"CHANNELPAN {index} {value:.6f}")[0]

    def current_channel(self):
        """Canal sur lequel le piano roll ecrit."""
        try:
            line = self.cmd("CURCHANNEL")[0]
            if line.startswith("CURCHANNEL "):
                return int(line.split()[1])
        except (OSError, TimeoutError, IndexError, ValueError):
            pass
        return None

    def select_channel(self, index):
        try:
            return self.cmd(f"SELECTCHANNEL {index}")[0]
        except (OSError, TimeoutError, IndexError):
            return None

    def preset_paths(self):
        """Table nom -> chemin absolu des presets .fst de FL."""
        if self._presets is not None:
            return self._presets
        table = {}
        for base in _dossiers_fl():
            for arbre in ("Generators", "Installed"):
                racine = os.path.join(base, arbre)
                if not os.path.isdir(racine):
                    continue
                for dossier, _sous, fichiers in os.walk(racine):
                    for fichier in fichiers:
                        if fichier.lower().endswith(".fst"):
                            nom = os.path.splitext(fichier)[0].lower()
                            table.setdefault(nom, os.path.join(dossier, fichier))
        self._presets = table
        return table

    def new_channel(self):
        """Cree un canal vide (Sampler). Sert a garder les positions alignees."""
        return self.cmd("NEWCHANNEL")[0]

    def load_plugin(self, chemin):
        return self.cmd(f"LOADPLUGIN {chemin}")[0]

    def effects(self):
        """Effets du mixeur : {(tranche, slot): nom}.

        Une seule requete (FXALL) : 17 tranches x 10 slots lus en memoire.
        """
        try:
            line = self.cmd("FXALL")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("FXALL "):
            return None
        out = {}
        for token in line.split("|"):
            token = token.strip()
            if not token or ":" not in token:
                continue
            place, nom = token.split(":", 1)
            if "." not in place:
                continue
            t, s = place.split(".", 1)
            try:
                out[(int(t), int(s))] = nom.strip()
            except ValueError:
                continue
        return out

    def effect_detail(self, track, slot):
        """(nom, dll, preset) d'un slot, ou None s'il est vide."""
        try:
            line = self.cmd(f"FX {track} {slot}")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("FX "):
            return None
        parts = [p.strip() for p in line.split("|")]
        nom = parts[0].split(" ", 3)[3] if len(parts[0].split(" ", 3)) > 3 else ""
        dll = parts[1] if len(parts) > 1 else ""
        preset = parts[2] if len(parts) > 2 else ""
        return (nom, dll, preset)

    def _table_presets(self, arbre):
        """Index nom -> chemin pour un arbre du dossier Plugin database."""
        if self._presets is None:
            self._presets = {}
        if arbre in self._presets:
            return self._presets[arbre]
        table = {}
        for base in _dossiers_fl():
            racine = os.path.join(base, arbre)
            if not os.path.isdir(racine):
                continue
            for dossier, _sous, fichiers in os.walk(racine):
                for fichier in fichiers:
                    if fichier.lower().endswith(".fst"):
                        nom = os.path.splitext(fichier)[0].lower()
                        table.setdefault(nom, os.path.join(dossier, fichier))
        self._presets[arbre] = table
        return table

    def preset_paths(self):
        """Table nom -> chemin des presets de generateurs (canaux)."""
        return self._table_presets("Generators")

    def effect_paths(self):
        """Table nom -> chemin des presets d'effets (mixeur)."""
        return self._table_presets("Effects")

    @staticmethod
    def _cherche(table, nom):
        """Trouve le preset dont le nom correspond, meme approximativement."""
        cle = (nom or "").strip().lower()
        if not cle:
            return None
        if cle in table:
            return table[cle]

        candidats = [k for k in table
                     if k.startswith(cle + " ") or cle.startswith(k + " ")]
        if not candidats:
            # Dernier recours, seulement pour un nom d'un seul mot : sinon
            # "Fruity Delay 3" tombait sur un generateur "Fruity ..." au hasard.
            mots = cle.split()
            if len(mots) == 1 and len(mots[0]) >= 3:
                candidats = [k for k in table if k.split()[0] == mots[0]]
        if not candidats:
            return None
        candidats.sort(key=len)
        return table[candidats[0]]

    def find_preset(self, nom):
        """Chemin du .fst d'un generateur.

        FL ne garde pas toujours le nom du fichier : "Serum 2.fst" donne le canal
        "Serum 2", mais "Twin 3.fst" donne le canal "Twin". Une recherche exacte
        echouait donc sur la moitie des plugins.
        """
        return self._cherche(self.preset_paths(), nom)

    def find_effect(self, nom):
        """Chemin du .fst d'un effet du mixeur."""
        return self._cherche(self.effect_paths(), nom)

    def set_channel_track(self, index, track):
        return self.cmd(f"CHANNELTRACK {index} {track}")[0]

    # --- playlist -----------------------------------------------------------
    def clips(self):
        """Liste des clips : [(position, longueur, motif), ...]."""
        try:
            line = self.cmd("CLIPLIST")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("CLIPLIST "):
            return None
        parts = line.split()
        out = []
        for token in parts[2:]:
            bits = token.split(":")
            if len(bits) != 4:
                continue
            try:
                out.append((int(bits[0]), int(bits[1]),
                            int(bits[2]), int(bits[3])))
            except ValueError:
                continue
        return out

    def add_clip(self, position, length, motif, track=1):
        return self.cmd(f"CLIPADD {position} {length} {motif} {track}")[0]

    def del_clip(self, position, track=1, pat=1):
        return self.cmd(f"CLIPDEL {position} {track} {pat}")[0]

    def set_clip_len(self, position, length, track=1, pat=1):
        return self.cmd(f"CLIPSETLEN {position} {length} {track} {pat}")[0]

    # --- parametres de plugin -----------------------------------------------
    def params(self, track, slot, count=8):
        """Valeurs brutes des parametres d'un emplacement, ou None."""
        try:
            line = self.cmd(f"PARAMS {track} {slot} {count}")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("PARAMS "):
            return None
        parts = line.split()
        try:
            n = int(parts[3])
            return [int(x) for x in parts[4:4 + n]]
        except (ValueError, IndexError):
            return None

    def set_param(self, track, slot, index, value):
        return self.cmd(f"PARAM {track} {slot} {index} {value:.6f}")[0]

    def cmd_force(self, texte):
        """Ordre de l'hote : s'execute meme chez l'invite.

        Le plugin refuse les commandes qui touchent le projet quand on est
        invite ; ce prefixe dit 'c'est l'hote qui parle.'
        """
        return self.cmd(f"FORCE {texte}")
    def set_param_raw(self, track, slot, index, raw):
        """Ecrit un parametre en valeur brute, sans conversion d'echelle."""
        return self.cmd(f"PARAMRAW {track} {slot} {index} {int(raw)}")[0]


# --------------------------------------------------------------------------
def diff_additions(old, new):
    """Notes presentes dans `new` mais pas dans `old`."""
    co, cn = Counter(old), Counter(new)
    return list((cn - co).elements())


def diff_removals(old, new):
    """Notes presentes dans `old` mais plus dans `new`."""
    co, cn = Counter(old), Counter(new)
    return list((co - cn).elements())


class Sync:
    def __init__(self, plugin, link, is_host):
        self.plugin = plugin
        self.link = link
        self.is_host = is_host
        self.snapshot = plugin.state()
        self.tempo = plugin.tempo()
        self.playing = plugin.playing()
        self.pattern = plugin.pattern()
        self.incoming = []
        self.incoming_lock = threading.Lock()
        self.sent_initial = False
        self.label = "host" if is_host else "guest"
        self.mix = {}
        self.last_mix = 0.0
        self.clips = None            # None = pas encore lu
        self.clips_ready = False
        self.params = {}             # (piste, slot) -> [valeurs brutes]
        self.last_params = 0.0
        self.last_clips = 0.0
        self.last_mouse = None
        self.clip_lengths = None
        self.clips_key = None
        self.last_clipsync = 0.0
        self.nb_canaux_hote = None   # taille du rack de l hote
        self.averti_canaux = False
        # correspondance par nom entre les deux racks
        self.carte_hote_vers_local = {}
        self.carte_local_vers_hote = {}
        self.averti_noms = set()
        self.position = None
        self.channels_state = None
        self.peer_channels = None
        self.channels_warned = None
        self.last_channels = 0.0
        self.channel_mix_state = None
        self.sent_plugins = False
        self._presets = None
        self.my_plugins = None
        self.peer_plugins = None
        self.last_channel_mix = 0.0
        # effets du mixeur : etat connu et cadence (une requete par seconde)
        self.role_sent = False
        # None = pas encore teste, False = plugin sans POLL
        self.supports_poll = None
        # valeur de reference venue de l'hote (l'invite s'y ramene)
        self.host_tempo = None
        self.host_playing = None
        self.host_pattern = None
        self.fx = None
        self.fx_attendus = {}
        self.fx_params = {}
        self.last_fx = 0.0
        self.last_fxparams = 0.0
        # Le curseur a besoin d'une cadence bien plus rapide que les notes.
        # le curseur passe par STATUS : plus de thread ni de requete dediee

    def _mouse_loop(self):
        last = None
        while True:
            try:
                pos = self.plugin.mouse()
            except (ConnectionError, OSError):
                # le plugin a disparu : on s'arrete, l'agent principal gerera
                return
            except Exception as exc:
                # toute autre erreur ne doit pas tuer le thread en silence
                print(f"[curseur] lecture impossible : {exc}")
                return
            if pos != last:
                last = pos
                if pos is None:
                    self.publish({"type": "mouse", "x": -1, "y": -1, "label": self.label})
                else:
                    self.publish({"type": "mouse", "x": pos[0], "y": pos[1],
                                  "label": self.label})
            time.sleep(0.04)

    def publish(self, message):
        try:
            self.link.send(message)
        except OSError as exc:
            print(f"[reseau] envoi impossible : {exc}")

    def publish_global(self, message):
        """Tempo, transport, pattern : bidirectionnels.

        Les deux participants peuvent lancer la lecture ou regler le tempo.
        En cas de conflit (les deux agissent en meme temps), la derniere
        action recue gagne. L'hote n'est plus en lecture seule : c'etait
        trop restrictif a l'usage.
        """
        self.publish(message)

    def send_plugins(self):
        """Envoie l'inventaire des plugins, une seule fois par session."""
        if self.sent_plugins:
            return
        self.sent_plugins = True
        try:
            inventaire = scan_plugins()
        except OSError as exc:
            print(f"[plugins] analyse impossible : {exc}")
            return
        inventaire["poste"] = socket.gethostname().upper()
        self.my_plugins = inventaire
        total = sum(len(inventaire[k]) for k in ("fl", "vst3", "vst2"))
        print(f"[plugins] {total} plugin(s) sur ce poste")
        self.publish_global({"type": "plugins", "inventaire": inventaire})

    def push_all(self):
        """Envoi de l'etat complet (a la connexion). L'HOTE fait foi.

        L'invite ne pousse rien : il efface ses patterns puis reprend tout ce
        que l'hote lui envoie (notes, tempo, pattern, transport, mixage).
        """
        if not self.is_host:
            print("[sync] invite : on reprend l'etat de l'hote")
            return

        # l'inventaire part en premier : c'est ce qui permet de savoir ce que
        # les deux postes ont en commun avant de parler de plugins.
        self.send_plugins ()

        # l'invite doit repartir de zero, sinon ses notes s'ajoutent aux notres
        self.publish_global({"type": "reset"})

        total = 0
        for channel, notes in self.snapshot.items():
            if notes:
                self.publish({"type": "add", "ch": channel, "notes": notes})
                total += len(notes)

        tempo = self.plugin.tempo()
        if tempo:
            self.publish_global({"type": "tempo", "bpm": tempo})
        pattern = self.plugin.pattern()
        if pattern is not None:
            self.publish_global({"type": "pattern", "n": pattern})
        playing = self.plugin.playing()
        if playing is not None:
            self.publish_global({"type": "transport", "playing": playing})

        channels = self.plugin.channels()
        if channels is not None:
            self.publish_global({"type": "channelcount", "n": len(channels)})
        if channels:
            # Le rack part d'un bloc, dans l'ordre : c'est ce qui permet a
            # l'invite de le reconstruire a l'identique (meme ordre, donc memes
            # index) au lieu de recevoir des canaux un par un.
            self.publish_global({"type": "racklist",
                                 "channels": [[int(i), int(t), name or ""]
                                              for i, t, name in channels]})
            for index, track, name in channels:
                self.publish_global({"type": "channeltrack", "index": index,
                                     "track": track})
                print(f"[sync] canal {index} : '{name}' sur tranche {track}")

        # la playlist fait partie de l'etat initial : sans elle, l'invite
        # arrive sur un arrangement vide alors que l'hote en a un.
        clips = self.plugin.clips()
        if clips:
            self.publish_global({"type": "clipsync",
                                 "clips": [[int(p), int(l), int(m), int(t)]
                                           for p, l, m, t in clips]})

        niveaux = self.plugin.channel_mix()
        if niveaux:
            for index, volume, pan, mute in niveaux:
                self.publish_global({"type": "channelmix", "index": index,
                                     "vol": round(volume, 5),
                                     "pan": round(pan, 5), "mute": mute})

        # la position de lecture fait partie de l'etat initial : sans elle,
        # l'invite demarre au debut alors que l'hote est ailleurs.
        status = self.plugin.status()
        if status:
            self.publish_global({"type": "songpos", "pos": status["position"]})

        # tranche 0 = master : oubliee tant que la boucle commencait a 1.
        for track in range(0, 17):
            state = self.plugin.mix_state(track)
            if state is None:
                continue
            payload = {"type": "mix", "track": track}
            payload.update(state)
            self.publish(payload)

        print(f"[sync] etat initial envoye ({total} notes, tempo {tempo})")

    def on_message(self, message):
        with self.incoming_lock:
            self.incoming.append(message)

    def drain(self):
        with self.incoming_lock:
            pending, self.incoming = self.incoming, []
        return pending

    def apply_incoming(self):
        applied = 0
        for message in self.drain():
            kind = message.get("type")
            # "ch" = canal du piano roll. Les anciens messages portaient "list".
            index = message.get("ch", message.get("list", 0))

            # Les reglages globaux sont bidirectionnels : on ne filtre plus.

            if kind == "reset":
                # l'hote vient de se connecter : on efface nos patterns pour
                # repartir exactement de son projet.
                cleared = 0
                for channel in range(16):
                    try:
                        self.plugin.cmd(f"CLEAR {channel}")
                        cleared += 1
                    except (OSError, TimeoutError, IndexError):
                        break
                self.snapshot = {}
                try:
                    self.plugin.cmd("CLIPCLEAR")
                except (OSError, TimeoutError, IndexError):
                    pass
                self.clips = None
                self.clip_lengths = None
                self.clips_key = None
                print(f"[sync] projet de l'hote repris : {cleared} canal(aux) efface(s)")

            elif kind == "add":
                notes = [tuple(n) for n in message.get("notes", [])]
                cible = self.index_local(int(index))
                if notes and cible is None:
                    nom = self.nom_hote(int(index))
                    if nom not in self.averti_noms:
                        self.averti_noms.add(nom)
                        print(f"[rack] canal '{nom or index}' absent ici : "
                              f"ses notes ne sont pas ecrites")
                elif notes:
                    self.plugin.add_many(int(cible), notes)
                    applied += len(notes)

            elif kind == "del":
                notes = [tuple(n) for n in message.get("notes", [])]
                cible = self.index_local(int(index))
                if notes and cible is not None:
                    self.plugin.del_many(int(cible), notes)
                    applied += len(notes)

            elif kind == "tempo":
                bpm = message.get("bpm")
                if bpm:
                    reply = self.plugin.cmd_force(f"TEMPO {float(bpm):.3f}")[0]
                    if reply.startswith("ERR"):
                        print(f"[sync] TEMPO REFUSE par le plugin : {reply}")
                        print("[sync] -> le plugin de ce cote est trop ancien")
                        print("       (il faut FruityMultiBridge.vst3 de 503 Ko)")
                    else:
                        print(f"[sync] tempo applique : {bpm}  [{reply}]")
                    self.tempo = self.plugin.tempo()
                    try:
                        self.host_tempo = float(bpm)
                    except (TypeError, ValueError):
                        pass

            elif kind == "transport":
                want = bool(message.get("playing"))
                reply = self.plugin.cmd_force("PLAY" if want else "STOP")[0]
                if reply.startswith("ERR"):
                    print(f"[sync] transport refuse : {reply}")
                else:
                    print(f"[sync] transport applique : {'lecture' if want else 'arret'}")
                self.playing = self.plugin.playing()
                self.host_playing = want

            elif kind == "songpos":
                position = int(message.get("pos", 0))
                if position >= 0:
                    self.plugin.cmd_force(f"SONGPOS {position}")
                    self.position = position

            elif kind == "channelcount":
                self.peer_channels = int(message.get("n", -1))
                print(f"[sync] l'autre a {self.peer_channels} canal(aux)")

            elif kind == "channelmix":
                index = int(message.get("index", -1))
                if index >= 0:
                    if "vol" in message:
                        self.plugin.set_channel_volume(index,
                                                       float(message["vol"]))
                    if "pan" in message:
                        self.plugin.set_channel_pan(index,
                                                    float(message["pan"]))
                    if "mute" in message:
                        self.plugin.set_channel_mute(index,
                                                     int(message["mute"]))
                    if self.channel_mix_state is not None:
                        avant = self.channel_mix_state.get(index, (0.0, 0.0, 1))
                        self.channel_mix_state[index] = (
                            float(message.get("vol", avant[0])),
                            float(message.get("pan", avant[1])),
                            int(message.get("mute", avant[2])))
                    applied += 1

            elif kind == "plugins":
                autre = message.get("inventaire") or {}
                self.peer_plugins = autre
                poste = (autre.get("poste") or "pair").replace("/", "-")
                chemin_inventaire = os.path.join(dossier_local(),
                                                 f"inventaire_{poste}.json")
                try:
                    with open(chemin_inventaire, "w",
                              encoding="utf-8") as fichier:
                        json.dump(autre, fichier, ensure_ascii=False, indent=2)
                except OSError:
                    pass

                ici = self.my_plugins or scan_plugins()
                commun, ici_seul, autre_seul = comparer_plugins(ici, autre)
                ecrire_compat(commun, ici_seul, autre_seul)
                print()
                print(f"[plugins] {autre.get('poste', 'pair')} : "
                      f"{len(commun)} en commun, "
                      f"{len(ici_seul)} seulement ici, "
                      f"{len(autre_seul)} seulement chez lui")
                if autre_seul:
                    apercu = ", ".join(autre_seul[:8])
                    print(f"[plugins] presents chez lui et pas ici : {apercu}"
                          + (" ..." if len(autre_seul) > 8 else ""))
                print(f"[plugins] fichier ecrit : {chemin_inventaire}")
                print()
                self.send_plugins ()
                applied += 1

            elif kind == "racklist":
                # Rack complet de l'hote, dans l'ordre. On s'aligne pour que les
                # index veuillent dire la meme chose des deux cotes.
                wanted = [(int(i), int(t), str(n))
                          for i, t, n in message.get("channels", [])]
                self.nb_canaux_hote = len(wanted)
                self.rack_hote = wanted
                local = self.plugin.channels() or []
                print(f"[rack] l'hote a {len(wanted)} canal(aux), "
                      f"j'en ai {len(local)}")
                crees = 0
                for index, track, name in wanted:
                    if index >= len(local) + crees:
                        chemin = self.plugin.find_preset(name) if name else None
                        if chemin:
                            print(f"[rack] creation de '{name}' "
                                  f"(position {index})")
                            self.plugin.load_plugin(chemin)
                            crees += 1
                        else:
                            # Sans l'instrument, on cree quand meme un canal
                            # vide : sinon les suivants remontent d'une
                            # position et tout le rack est decale.
                            try:
                                self.plugin.new_channel()
                                crees += 1
                                print(f"[rack] '{name}' absent de ce poste — "
                                      f"canal vide cree pour garder la position")
                            except (OSError, TimeoutError, IndexError):
                                print(f"[rack] '{name}' absent et canal vide "
                                      f"impossible : les positions suivantes "
                                      f"seront decalees")
                        continue
                    if index < len(local):
                        lindex, ltrack, lname = local[index]
                        if name and not meme_nom(lname, name):
                            print(f"[rack] ATTENTION : position {index} = "
                                  f"'{lname}' chez moi, '{name}' chez l'hote")
                        if ltrack != track:
                            self.plugin.set_channel_track(lindex, track)
                fresh = self.plugin.channels()
                if fresh is not None:
                    self.channels_state = {i: t for i, t, _ in fresh}
                self.construire_carte(wanted)
                if crees:
                    print(f"[rack] {crees} canal(aux) cree(s) — "
                          f"verifie l'ordre a l'ecran")
                applied += 1

            elif kind == "addchannel":
                nom = str(message.get("name", "")).strip()
                if nom:
                    chemin = self.plugin.find_preset(nom)
                    if chemin:
                        print(f"[plugins] creation de '{nom}' depuis "
                              f"{os.path.basename(chemin)}")
                        self.plugin.load_plugin(chemin)
                        # on relit l'etat pour ne pas renvoyer ce qu'on vient
                        # de creer
                        fresh = self.plugin.channels()
                        if fresh is not None:
                            self.channels_state = {i: t for i, t, _ in fresh}
                            voulu = message.get("index")
                            obtenu = fresh[-1][0] if fresh else None
                            if (voulu is not None and obtenu is not None
                                    and int(voulu) != obtenu):
                                print(f"[plugins] ATTENTION : cree en position "
                                      f"{obtenu} alors que l'hote l'a en "
                                      f"position {voulu} — les reglages de ce "
                                      f"canal peuvent viser le mauvais.")
                    else:
                        print(f"[plugins] '{nom}' introuvable sur ce poste")
                    applied += 1

            elif kind == "fx":
                t = int(message.get("track", -1))
                s = int(message.get("slot", -1))
                nom = str(message.get("name", "")).strip()
                if t < 0 or s < 0:
                    pass
                elif not nom:
                    print(f"[fx] l'hote a retire l'effet de la tranche {t} "
                          f"slot {s}")
                else:
                    preset = str(message.get("preset", ""))
                    detail = self.plugin.effect_detail(t, s)
                    deja = bool(detail and detail[0])
                    if deja:
                        print(f"[fx] tranche {t} slot {s} : l'hote a "
                              f"'{nom}', moi '{detail[0]}' — a verifier")
                    else:
                        print(f"[fx] l'hote a mis '{nom}' sur la tranche "
                              f"{t} slot {s}")
                        # FL n'expose pas (encore) de fonction d'insertion
                        # d'effet : on guide, et on met le chemin dans le
                        # presse-papiers pour qu'il n'ait qu'a coller.
                        cible = preset or self.plugin.find_effect(nom) or ""
                        if cible:
                            if copier_presse_papier(cible):
                                print("[fx] chemin copie dans le presse-papiers")
                            print(f"[fx] a poser a la main : glisse "
                                  f"'{os.path.basename(cible)}' (ou colle le "
                                  f"chemin dans le navigateur de FL) sur la "
                                  f"tranche {t}, slot {s}")
                            self.fx_attendus[(t, s)] = nom
                        else:
                            print(f"[fx] '{nom}' introuvable sur ce poste")
                    applied += 1

            elif kind == "fxparam":
                t = int(message.get("track", -1))
                s = int(message.get("slot", -1))
                i = int(message.get("i", -1))
                v = int(message.get("v", 0))
                if 0 <= t <= 125 and 0 <= s <= 9 and i >= 0:
                    self.plugin.set_param_raw(t, s, i, v)
                    # on met a jour notre reference : sinon on renverrait au
                    # pair ce qu'il vient de nous envoyer
                    cache = self.fx_params.setdefault((t, s), [])
                    while len(cache) <= i:
                        cache.append(0)
                    cache[i] = v
                    applied += 1

            elif kind == "channeltrack":
                index = int(message.get("index", -1))
                track = int(message.get("track", -1))
                if index >= 0 and 0 <= track <= 125:
                    self.plugin.set_channel_track(index, track)
                    if self.channels_state is not None:
                        self.channels_state[index] = track
                    applied += 1

            elif kind == "pattern":
                number = message.get("n")
                if number:
                    reply = self.plugin.cmd_force(f"PATTERN {number}")[0]
                    if reply.startswith("ERR"):
                        print(f"[sync] pattern refuse : {reply}")
                    else:
                        print(f"[sync] pattern applique : {number}")
                    self.pattern = self.plugin.pattern()
                    self.host_pattern = int(number)

            elif kind == "mouse":
                # coordonnees proportionnelles (0..1000) : independantes de la
                # resolution et du zoom de chacun.
                self.plugin.cursor_f(int(message.get("nx", -1)),
                                     int(message.get("ny", -1)),
                                     str(message.get("label", "")))

            elif kind == "mix":
                track = int(message.get("track", 0))
                if not (0 <= track <= 125):
                    continue
                state = self.mix.setdefault(track, {})
                if "vol" in message:
                    vol = float(message["vol"])
                    self.plugin.set_track_volume(track, vol)
                    state["vol"] = vol
                if "pan" in message:
                    pan = float(message["pan"])
                    self.plugin.set_track_pan(track, pan)
                    state["pan"] = pan
                if "stereo" in message:
                    sep = float(message["stereo"])
                    self.plugin.set_track_stereo(track, sep)
                    state["stereo"] = sep
                if "mute" in message:
                    mute = int(message["mute"])
                    self.plugin.set_track_mute(track, mute)
                    state["mute"] = mute

            elif kind == "param":
                track = int(message.get("track", 1))
                slot = int(message.get("slot", 0))
                index = int(message.get("i", 0))
                raw = int(message.get("v", 0))
                if 1 <= track <= 125 and 0 <= slot <= 9 and 0 <= index <= 127:
                    self.plugin.set_param(track, slot, index,
                                          raw / 1073741824.0)

            elif kind == "clipsync":
                # La liste recue est complete et fait foi. On rapproche au lieu
                # de tout reconstruire : un clip deja present n'est pas touche,
                # donc plus de clignotement inutile et bien moins de commandes.
                # Un clip en trop chez moi est supprime : aucun orphelin.
                items = []
                for item in message.get("clips", [])[:300]:
                    if len(item) < 4:
                        continue
                    try:
                        entry = (int(item[0]), int(item[1]),
                                 int(item[2]), int(item[3]))
                    except (TypeError, ValueError):
                        continue
                    if (entry[0] >= 0 and 1 <= entry[1] <= 100000
                            and 1 <= entry[2] <= 255 and 1 <= entry[3] <= 200):
                        items.append(entry)

                wanted = {(p, m, t): l for p, l, m, t in items}
                mine = {}
                for p, l, m, t in (self.plugin.clips() or []):
                    mine[(p, m, t)] = l

                added = 0
                resized = 0
                removed = 0

                for (position, motif, track), length in wanted.items():
                    current = mine.get((position, motif, track))
                    if current is None:
                        self.plugin.add_clip(position, length, motif, track)
                        added += 1
                    elif current != length:
                        self.plugin.set_clip_len(position, length, track, motif)
                        resized += 1

                for (position, motif, track) in mine:
                    if (position, motif, track) not in wanted:
                        self.plugin.del_clip(position, track, motif)
                        removed += 1

                # FL remet la longueur du motif quand on insere un clip. On
                # relit donc ce qui existe vraiment et on corrige les ecarts :
                # sans cette verification, un clip recu restait a une mesure.
                for _ in range(3):
                    reel = {(pos, motif, track): longueur
                            for pos, longueur, motif, track
                            in (self.plugin.clips() or [])}
                    corriges = 0
                    for (position, motif, track), longueur in wanted.items():
                        if reel.get((position, motif, track)) != longueur:
                            self.plugin.set_clip_len(position, longueur,
                                                     track, motif)
                            corriges += 1
                    if not corriges:
                        break
                    resized += corriges

                # on adopte la liste recue : pas de renvoi en echo
                self.clips_key = tuple(sorted(items))
                applied += 1 + added + resized + removed
                if added or resized or removed:
                    print(f"[sync] playlist : {added} ajout(s), "
                          f"{resized} redimensionnement(s), "
                          f"{removed} suppression(s)")

            elif kind == "clip":
                position = int(message.get("pos", 0))
                length = int(message.get("len", 384))
                motif = int(message.get("pat", 1))
                track = int(message.get("track", 1))
                if (position >= 0 and 1 <= length <= 100000
                        and 1 <= motif <= 255 and 1 <= track <= 200):
                    self.plugin.add_clip(position, length, motif, track)
                    applied += 1

            elif kind == "clipdel":
                position = int(message.get("pos", 0))
                if position >= 0:
                    self.plugin.del_clip(position,
                                         int(message.get("track", 1)),
                                         int(message.get("pat", 1)))
                    applied += 1

            elif kind == "cliplen":
                position = int(message.get("pos", 0))
                length = int(message.get("len", 384))
                if position >= 0 and 1 <= length <= 100000:
                    self.plugin.set_clip_len(position, length,
                                             int(message.get("track", 1)),
                                             int(message.get("pat", 1)))
                    applied += 1

        if applied:
            self.plugin.refresh()
            # On relit l'etat pour ne pas renvoyer au pair ce qu'il vient d'envoyer.
            self.snapshot = self.plugin.state()

            # Meme chose pour les clips : sans cette relecture, le clip recu
            # etait vu comme "nouveau chez moi" au cycle suivant et renvoye a
            # l'expediteur, qui le renvoyait... ping-pong permanent.
            clips = self.plugin.clips()
            if clips is not None:
                self.clips = Counter((pos, pat, track)
                                     for pos, length, pat, track in clips)
                self.clip_lengths = {(pos, pat, track): length
                                     for pos, length, pat, track in clips}

            print(f"[sync] {applied} changement(s) recu(s)")
        return applied

    def detect_local(self):
        st = None
        current = None

        # Une seule requete pour l'etat ET les notes quand le plugin sait le
        # faire (POLL, v14) : le cycle passe de ~31 ms a ~15 ms.
        if self.supports_poll is not False:
            resultat = self.plugin.poll()
            if (isinstance(resultat, tuple) and len(resultat) == 2
                    and isinstance(resultat[0], dict)
                    and isinstance(resultat[1], dict)):
                st, current = resultat
                self.supports_poll = True
            else:
                if self.supports_poll is None:
                    print("[sync] plugin sans POLL : deux requetes par cycle")
                self.supports_poll = False

        if current is None:
            current = self.plugin.state()
        changed = 0

        if st is None:
            st = self.plugin.status()

        # --- Autorite : sur ces trois reglages, l'HOTE FAIT FOI ------------
        # L'invite ne les envoie jamais. S'il les change chez lui, on le
        # ramene a la valeur de l'hote : c'est visible, et ca evite qu'une
        # session parte chacun de son cote.
        current_tempo = st["tempo"] if st else None
        if current_tempo is not None:
            if self.is_host:
                if self.tempo is not None and abs(current_tempo - self.tempo) > 0.001:
                    self.publish_global({"type": "tempo", "bpm": current_tempo})
                    print(f"[sync] tempo envoye : {current_tempo}")
                    changed += 1
            elif self.host_tempo is not None and abs(current_tempo - self.host_tempo) > 0.01:
                self.plugin.cmd_force(f"TEMPO {self.host_tempo:.3f}")
                print(f"[autorite] tempo ramene a {self.host_tempo} (celui de l'hote)")
            self.tempo = current_tempo

        current_playing = st["playing"] if st else None
        if current_playing is not None:
            if self.is_host:
                if self.playing is not None and current_playing != self.playing:
                    self.publish_global({"type": "transport", "playing": current_playing})
                    print(f"[sync] transport envoye : {'lecture' if current_playing else 'arret'}")
                    changed += 1
            elif (self.host_playing is not None
                  and current_playing != self.host_playing):
                self.plugin.cmd_force("PLAY" if self.host_playing else "STOP")
                print(f"[autorite] transport ramene a "
                      f"{'lecture' if self.host_playing else 'arret'}")
            self.playing = current_playing

        current_pattern = st["pattern"] if st else None
        if current_pattern is not None:
            if self.is_host:
                if self.pattern is not None and current_pattern != self.pattern:
                    self.publish_global({"type": "pattern", "n": current_pattern})
                    print(f"[sync] pattern envoye : {current_pattern}")
                    changed += 1
            elif (self.host_pattern is not None
                  and current_pattern != self.host_pattern):
                self.plugin.cmd_force(f"PATTERN {self.host_pattern}")
                print(f"[autorite] motif ramene a {self.host_pattern}")
            self.pattern = current_pattern

        # horodatage commun a tous les blocs qui suivent
        now = time.time()

        # playlist : on envoie la LISTE COMPLETE, pas des differences.
        # Le suivi position par position laissait des copies orphelines des
        # qu'une suppression ne tombait pas juste (glissement rapide) : c'est
        # l'escalier de clips qu'on voyait dans la playlist.
        if now - self.last_clips >= 0.25:
            self.last_clips = now
            current_clips = self.plugin.clips()
            if current_clips is not None:
                snapshot = [(int(p), int(l), int(m), int(t))
                            for p, l, m, t in current_clips]
                key = tuple(sorted(snapshot))

                if self.clips_key is None:
                    # premiere lecture : on memorise sans rien envoyer
                    self.clips_key = key
                    self.clips = Counter((p, m, t) for p, l, m, t in snapshot)
                    self.clip_lengths = {(p, m, t): l for p, l, m, t in snapshot}
                elif key != self.clips_key:
                    # On ne met a jour l'etat de reference QUE si l'envoi a
                    # vraiment eu lieu. Sinon un changement bloque par la
                    # limitation etait perdu pour de bon.
                    if now - self.last_clipsync >= 0.25:
                        self.last_clipsync = now
                        self.publish_global({"type": "clipsync",
                                             "clips": snapshot})
                        print(f"[sync] playlist envoyee "
                              f"({len(snapshot)} clip(s))")
                        self.clips_key = key
                        self.clips = Counter((p, m, t) for p, l, m, t in snapshot)
                        self.clip_lengths = {(p, m, t): l
                                             for p, l, m, t in snapshot}

        # channel rack : assignation canal -> tranche (cadence lente)
        if now - self.last_channels >= 1.0:
            self.last_channels = now
            current_channels = self.plugin.channels()
            if current_channels is not None:
                # Les deux cotes doivent avoir le meme nombre de canaux, sinon
                # les assignations visent des canaux qui n'existent pas.
                if (self.peer_channels is not None
                        and len(current_channels) != self.peer_channels):
                    if self.channels_warned != self.peer_channels:
                        self.channels_warned = self.peer_channels
                        print(f"[sync] ATTENTION : {len(current_channels)} canal(aux) "
                              f"chez moi contre {self.peer_channels} chez l'autre. "
                              f"Chargez le meme projet des deux cotes.")
                if self.channels_state is None:
                    self.channels_state = {i: t for i, t, _ in current_channels}
                else:
                    # Un canal qui n'existait pas et qui porte un nom : c'est un
                    # plugin que l'utilisateur vient d'ajouter. On l'annonce
                    # pour que l'autre cote le cree avec le meme preset.
                    for index, track, name in current_channels:
                        if index in self.channels_state:
                            continue
                        if not name or name.strip().lower().startswith("sampler"):
                            continue
                        self.publish_global({"type": "addchannel",
                                             "name": name.strip(),
                                             "index": index})
                        print(f"[sync] canal ajoute : '{name}' (index {index})")

                    for index, track, name in current_channels:
                        old_track = self.channels_state.get(index)
                        if old_track is not None and old_track != track:
                            self.publish_global({"type": "channeltrack",
                                                 "index": index, "track": track})
                            print(f"[sync] canal {index} ('{name}') -> tranche {track}")
                    self.channels_state = {i: t for i, t, _ in current_channels}

        # niveaux des canaux du channel rack (volume et panoramique)
        if now - self.last_channel_mix >= 0.3:
            self.last_channel_mix = now
            niveaux = self.plugin.channel_mix()
            if isinstance (niveaux, list):
                if self.channel_mix_state is None:
                    self.channel_mix_state = {i: (v, p, m) for i, v, p, m in niveaux}
                else:
                    for index, volume, pan, mute in niveaux:
                        old = self.channel_mix_state.get(index)
                        if old is None:
                            continue
                        if (abs(old[0] - volume) > 0.004
                                or abs(old[1] - pan) > 0.004
                                or old[2] != mute):
                            self.publish_global({"type": "channelmix",
                                                 "index": index,
                                                 "vol": round(volume, 5),
                                                 "pan": round(pan, 5),
                                                 "mute": mute})
                    self.channel_mix_state = {i: (v, p, m)
                                              for i, v, p, m in niveaux}

        # effets du mixeur : un seul FXALL par seconde (17 tranches d'un coup)
        if now - self.last_fx >= 1.0:
            self.last_fx = now
            courants = self.plugin.effects()
            if isinstance (courants, dict):
                if self.fx is None:
                    self.fx = dict(courants)
                else:
                    for (t, s), nom in courants.items():
                        if self.fx.get((t, s)) == nom:
                            continue
                        detail = self.plugin.effect_detail(t, s)
                        preset = detail[2] if detail else ""
                        self.publish_global({"type": "fx", "track": t,
                                             "slot": s, "name": nom,
                                             "preset": preset})
                        print(f"[fx] tranche {t} slot {s} : '{nom}'"
                              + (f"  [{os.path.basename(preset)}]"
                                 if preset else ""))
                    for (t, s) in list(self.fx.keys()):
                        if (t, s) in courants:
                            continue
                        self.publish_global({"type": "fx", "track": t,
                                             "slot": s, "name": "",
                                             "preset": ""})
                        print(f"[fx] tranche {t} slot {s} : retire")
                    self.fx = dict(courants)

                    # confirmation de ce qu'on avait demande a l'utilisateur
                    for (t, s), nom in list(self.fx_attendus.items()):
                        pose = courants.get((t, s))
                        if not pose:
                            continue
                        self.fx_attendus.pop((t, s), None)
                        if meme_nom(pose, nom):
                            print(f"[fx] tranche {t} slot {s} : '{pose}' en "
                                  f"place — c'est bon")
                        else:
                            print(f"[fx] tranche {t} slot {s} : '{pose}' pose "
                                  f"a la place de '{nom}'")

        # Reglages des effets : uniquement pour un effet PRESENT DES DEUX COTES
        # et du meme nom. On compare des valeurs BRUTES (PARAMS / PARAMRAW),
        # donc aucune echelle a supposer.
        if now - self.last_fxparams >= 1.0:
            self.last_fxparams = now
            if self.fx:
                for (t, s) in sorted(self.fx):
                    detail = self.plugin.effect_detail(t, s)
                    if not detail or not detail[0]:
                        continue
                    if not meme_nom(detail[0], self.fx[(t, s)]):
                        continue
                    vals = self.plugin.params(t, s, 16)
                    if not isinstance(vals, list) or not vals:
                        continue
                    cle = (t, s)
                    connu = self.fx_params.get(cle)
                    self.fx_params[cle] = list(vals)
                    if connu is None:
                        continue
                    changes = {}
                    for i, v in enumerate(vals):
                        if v == -2147483648:        # parametre inexistant
                            continue
                        old = connu[i] if i < len(connu) else None
                        # FL quantifie les valeurs (ecart mesure : ~90 000
                        # sur les 2^30). En dessous de 2^17 on ne voit rien ;
                        # au-dessus, c'est un vrai mouvement de potard.
                        if old is None or abs(v - old) > (1 << 17):
                            changes[i] = v
                    for i, v in changes.items():
                        self.publish({"type": "fxparam", "track": t,
                                      "slot": s, "i": i, "v": v})
                    if changes:
                        print(f"[fx] tranche {t} slot {s} : "
                              f"{len(changes)} reglage(s) envoye(s)")

        # parametres de plugin : DESACTIVES. Sans verification que les deux
        # cotes ont le meme plugin dans le meme emplacement, on ecrasait les
        # reglages du pair avec des valeurs qui ne le concernaient pas.
        if False and now - self.last_params >= 1.0:
            self.last_params = now
            param_changed = 0
            for track in range(1, 9):
                for slot in (0, 1):
                    values = self.plugin.params(track, slot, 8)
                    if values is None:
                        continue
                    key = (track, slot)
                    known = self.params.get(key)
                    if known is None:
                        self.params[key] = values
                        continue
                    for index, raw in enumerate(values):
                        if index < len(known) and raw != known[index]:
                            self.publish({"type": "param", "track": track,
                                          "slot": slot, "i": index, "v": raw})
                            param_changed += 1
                    self.params[key] = values
            if param_changed:
                print(f"[sync] {param_changed} parametre(s) envoye(s)")

        # position de lecture : diffusee seulement a l'arret. Pendant la
        # lecture elle avance toute seule, la synchroniser ferait des combats.
        if st and not st["playing"]:
            position = st["position"]
            if (self.position is not None
                    and abs(position - self.position) > 8):
                self.publish_global({"type": "songpos", "pos": position})
                print(f"[sync] position envoyee : {position}")
            self.position = position
        elif st:
            self.position = st["position"]

        # curseur : deja present dans STATUS, on le diffuse s'il a bouge
        if st:
            mx, my, mw, mh = st["mouse"]
            if mw > 0 and mh > 0 and mx >= 0 and my >= 0:
                nx = int(mx * 1000 / mw)
                ny = int(my * 1000 / mh)
            else:
                nx, ny = -1, -1
            if (nx, ny) != self.last_mouse:
                self.last_mouse = (nx, ny)
                self.publish({"type": "mouse", "nx": nx, "ny": ny,
                              "label": self.label})

        # mixeur : deja present dans STATUS, donc plus aucune requete dediee
        if st:
            sent = 0
            for track, snapshot in st["mix"].items():
                state = self.mix.setdefault(track, {})
                changes = {}
                for key, tolerance in (("vol", 0.004), ("pan", 0.01),
                                       ("stereo", 0.01)):
                    value = snapshot[key]
                    old = state.get(key)
                    if old is None or abs(value - old) > tolerance:
                        state[key] = value
                        changes[key] = value
                mute = snapshot["mute"]
                if mute >= 0 and state.get("mute") != mute:
                    state["mute"] = mute
                    changes["mute"] = mute

                if changes:
                    payload = {"type": "mix", "track": track}
                    payload.update(changes)
                    self.publish(payload)
                    sent += 1

            if sent:
                print(f"[sync] {sent} piste(s) de mixage envoyee(s)")

        for channel in set(list(current.keys()) + list(self.snapshot.keys())):
            # Un canal qui n'existe pas chez l'hote ne doit pas partir :
            # le plugin de l'autre poste le refuserait (et FL plantait avant
            # ce garde-fou).
            if (not self.is_host and self.nb_canaux_hote is not None
                    and channel >= self.nb_canaux_hote):
                if not self.averti_canaux:
                    self.averti_canaux = True
                    print(f"[rack] ton canal {channel + 1} n'existe pas chez "
                          f"l'hote ({self.nb_canaux_hote} canal(aux)) : ses "
                          f"notes ne seront pas envoyees")
                continue

            old = self.snapshot.get(channel, [])
            new = current.get(channel, [])

            # Les SUPPRESSIONS partent AVANT les ajouts.
            # Changer une velocite, c'est une note retiree et une autre ajoutee
            # sur le meme (temps, hauteur) : si l'ajout passait en premier, la
            # suppression qui suit pouvait tomber sur la nouvelle note.
            # Le canal est dans la note elle-meme : plus besoin de selectionner
            # le piano roll avant d'ecrire (c'etait la cause du "tout sur le
            # premier canal").
            cible = self.index_hote(channel)
            removed = diff_removals(old, new)
            if removed:
                self.publish({"type": "del", "ch": cible, "notes": removed})
                changed += len(removed)

            added = diff_additions(old, new)
            if added:
                self.publish({"type": "add", "ch": cible, "notes": added})
                changed += len(added)

        if changed:
            print(f"[sync] {changed} changement(s) local(aux) envoye(s)")

        self.snapshot = current
        return changed

    def _liberer_autorite(self):
        """La session est finie : on rend a FL ses menus."""
        try:
            self.plugin.cmd("ROLE aucun")
            print("[autorite] actions de l'hote rendues")
        except (OSError, TimeoutError, IndexError):
            pass

    def nom_hote(self, index_hote):
        """Nom du canal de l'hote, pour les messages."""
        for index, _t, name in getattr(self, "rack_hote", []) or []:
            if index == index_hote:
                return name
        return None

    def index_local(self, index_hote):
        """Mon canal qui correspond a ce canal de l'hote, ou None."""
        if self.is_host:
            return index_hote
        if not self.carte_hote_vers_local:
            return index_hote      # pas encore de table : on garde l'index
        return self.carte_hote_vers_local.get(index_hote)

    def index_hote(self, index_local):
        """Le canal de l'hote qui correspond a mon canal."""
        if self.is_host:
            return index_local
        return self.carte_local_vers_hote.get(index_local, index_local)

    def construire_carte(self, wanted):
        """Table de correspondance par nom entre le rack de l'hote et le mien."""
        if self.is_host:
            return
        local = self.plugin.channels() or []
        par_nom = {}
        for i, _t, n in local:
            cle = (n or "").strip().lower()
            if cle and cle not in par_nom:
                par_nom[cle] = i

        self.carte_hote_vers_local = {}
        self.carte_local_vers_hote = {}
        for index, _t, name in wanted:
            cle = (name or "").strip().lower()
            if not cle or cle not in par_nom:
                continue
            self.carte_hote_vers_local[index] = par_nom[cle]
            self.carte_local_vers_hote[par_nom[cle]] = index

        if self.carte_hote_vers_local:
            print(f"[rack] correspondance par nom : "
                  f"{len(self.carte_hote_vers_local)} canal(aux) reconnu(s)")
        else:
            print("[rack] aucun nom commun : les notes suivront les index")

    def run(self):
        print("[sync] pret")
        try:
            while True:
                # Le pair est parti : on rend la main pour pouvoir en accepter
                # un autre, au lieu de tourner dans le vide indefiniment.
                if not getattr(self.link, "alive", True):
                    print("[sync] pair deconnecte")
                    self._liberer_autorite()
                    return False

                # Autorite : le plugin doit savoir s'il est hote ou invite.
                # Chez l'invite, il grise alors les actions du projet (export,
                # sauvegarde, ouverture, scan de plugins).
                if not self.role_sent:
                    try:
                        reponse = self.plugin.cmd(
                            "ROLE " + ("hote" if self.is_host else "invite"))[0]
                        print(f"[autorite] {reponse}")
                    except (OSError, TimeoutError, IndexError) as exc:
                        print(f"[autorite] role non transmis : {exc}")
                    self.role_sent = True

                # Un message malforme ne doit jamais couper la session : on le
                # signale et on continue. Sans cela, la moindre erreur dans un
                # gestionnaire tuait la connexion et provoquait une boucle de
                # reconnexions.
                try:
                    self.apply_incoming()
                except Exception as exc:
                    print(f"[sync] message ignore : {exc}")

                try:
                    self.detect_local()
                except Exception as exc:
                    print(f"[sync] analyse locale impossible : {exc}")
                if not self.sent_initial:
                    self.push_all()
                    self.sent_initial = True
                time.sleep(POLL)
        except (ConnectionError, OSError) as exc:
            print(f"[sync] connexion au plugin perdue : {exc}")
            raise PluginLost(str(exc))


# --------------------------------------------------------------------------
# Reseau entre les deux agents.
#
# Deux chemins, le meme protocole :
#   - le RELAIS (par defaut) : chacun se connecte en WebSocket a un salon
#     designe par un code de 6 caracteres. Aucun port a ouvrir, aucune IP
#     echangee, ca passe partout (4G, CGNAT, reseau d'ecole).
#   - le DIRECT (repli, ou reseau local) : TCP sur le port 5000, UPnP sur la
#     box. L'adresse a donner est alors "ip:port/CODE".
#
# Dans les deux cas, chaque trame est une liste JSON de messages : l'emetteur
# regroupe ce qui part dans la meme fenetre de 40 ms. Le relais gratuit compte
# les trames, pas les messages.

PROTO = 1
RELAIS = os.environ.get("FM_RELAIS", "wss://fruitymulti-relais.26mataa.workers.dev")
PORT_DIRECT = 5000
ALPHABET_CODE = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # ni I, ni O, ni 0, ni 1


class Refus(Exception):
    """La connexion a abouti mais l'autre cote (ou le relais) n'en veut pas."""


def nouveau_code():
    import secrets
    return "".join(secrets.choice(ALPHABET_CODE) for _ in range(6))


def est_un_code(texte):
    return (len(texte) == 6
            and all(c in ALPHABET_CODE for c in texte.upper()))


class TransportTcp:
    """Une ligne de texte par trame, sur une socket TCP."""

    def __init__(self, sock):
        # Socket bloquante : le delai de lecture passe par select(). Un
        # settimeout() changerait aussi le delai de l'envoi en cours dans
        # l'autre thread.
        sock.settimeout(None)
        self.sock = sock
        self.buf = b""
        self.lock = threading.Lock()

    def envoyer(self, texte):
        with self.lock:
            self.sock.sendall((texte + "\n").encode("utf-8"))

    def recevoir(self, delai=None):
        import select
        while b"\n" not in self.buf:
            if delai is not None:
                lisible, _, _ = select.select([self.sock], [], [], delai)
                if not lisible:
                    raise TimeoutError("rien recu")
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("pair deconnecte")
            self.buf += chunk
        ligne, _, self.buf = self.buf.partition(b"\n")
        return ligne.decode("utf-8")

    def fermer(self):
        try:
            self.sock.close()
        except OSError:
            pass


class TransportRelais:
    """Une trame WebSocket par trame, via le relais."""

    def __init__(self, ws):
        self.ws = ws

    def envoyer(self, texte):
        from websockets.exceptions import ConnectionClosed
        try:
            self.ws.send(texte)
        except ConnectionClosed as exc:
            raise ConnectionError(f"relais ferme ({exc})")

    def recevoir(self, delai=None):
        from websockets.exceptions import ConnectionClosed
        try:
            message = self.ws.recv(timeout=delai)
        except ConnectionClosed as exc:
            raise ConnectionError(f"relais ferme ({exc})")
        return message if isinstance(message, str) else message.decode("utf-8")

    def fermer(self):
        try:
            self.ws.close()
        except Exception:
            pass


def ouvrir_relais(code, role):
    from websockets.sync.client import connect
    url = f"{RELAIS}/salon/{code}?role={role}&proto={PROTO}"
    ws = connect(url, open_timeout=10, max_size=4 * 1024 * 1024,
                 ping_interval=20, ping_timeout=20)
    return TransportRelais(ws)


def _messages(texte):
    """Une trame -> liste de messages (une trame peut aussi etre un objet seul)."""
    donnees = json.loads(texte)
    if isinstance(donnees, list):
        return [m for m in donnees if isinstance(m, dict)]
    return [donnees] if isinstance(donnees, dict) else []


def attendre_pair_relais(transport):
    """Attend que le relais annonce l'autre. Leve Refus si le salon est refuse."""
    while True:
        for m in _messages(transport.recevoir(None)):
            evenement = m.get("_relais")
            if evenement == "pair_arrive":
                return
            if evenement == "refuse":
                raise Refus(m.get("raison", "refuse par le relais"))


def poignee_de_main(transport, est_hote, code, delai=10.0):
    """Chacun se presente. Retourne la carte du pair, ou leve Refus.

    L'invite parle le premier ; l'hote verifie le code et le protocole avant de
    repondre. Un agent d'avant le protocole 1 ne se presente pas : il tombe
    sur le delai, avec un message qui dit de mettre a jour.
    """
    moi = {"type": "hello", "proto": PROTO, "version": AGENT_VERSION,
           "role": "hote" if est_hote else "invite",
           "poste": socket.gethostname().upper()}
    if not est_hote:
        moi["code"] = code
        transport.envoyer(json.dumps([moi]))

    lui = None
    limite = time.monotonic() + delai
    while lui is None:
        reste = limite - time.monotonic()
        if reste <= 0:
            raise Refus("le pair ne s'est pas presente : version trop ancienne ?")
        try:
            texte = transport.recevoir(reste)
        except TimeoutError:
            continue
        for m in _messages(texte):
            if m.get("_relais") == "pair_parti":
                raise ConnectionError("pair parti pendant la presentation")
            if m.get("type") == "refus":
                raise Refus(m.get("raison", "refuse par l'hote"))
            if m.get("type") == "hello":
                lui = m
                break

    raison = None
    if lui.get("proto") != PROTO:
        raison = (f"protocole {lui.get('proto')} chez l'autre, {PROTO} ici : "
                  f"mettez FruityMulti a jour tous les deux")
    elif lui.get("role") == moi["role"]:
        raison = f"deux {moi['role']}s dans la meme session"
    elif est_hote and str(lui.get("code", "")).upper() != code:
        raison = "code de session faux"

    if raison is not None:
        if est_hote:
            try:
                transport.envoyer(json.dumps([{"type": "refus", "raison": raison}]))
            except OSError:
                pass
        raise Refus(raison)

    if est_hote:
        transport.envoyer(json.dumps([moi]))
    if lui.get("version") != AGENT_VERSION:
        print(f"[reseau] ATTENTION : version {lui.get('version')} chez "
              f"l'autre, {AGENT_VERSION} ici")
    return lui


class Link:
    """Liaison avec le pair : messages groupes par trame, battement de coeur.

    Sans nouvelles du pair pendant SILENCE_MAX, la liaison est declaree morte :
    une coupure Wi-Fi ne ferme pas toujours la connexion proprement, et la
    session restait figee indefiniment.
    """

    LOT = 0.04
    BATTEMENT = 5.0
    SILENCE_MAX = 20.0
    TRAME_MAX = 512 * 1024

    def __init__(self, transport, on_message):
        self.transport = transport
        self.on_message = on_message
        self.alive = True
        self.file = []
        self.cond = threading.Condition()
        self.dernier_recu = time.monotonic()
        threading.Thread(target=self._lecteur, daemon=True).start()
        threading.Thread(target=self._emetteur, daemon=True).start()

    def send(self, message):
        if not self.alive:
            return
        with self.cond:
            self.file.append(message)
            self.cond.notify()

    def _mort(self, raison):
        if self.alive:
            self.alive = False
            print(f"[reseau] {raison}")
        with self.cond:
            self.cond.notify()
        self.transport.fermer()

    def _trames(self, lot):
        """Decoupe un lot en trames de taille raisonnable (le relais coupe a 1 Mo)."""
        trame, taille = [], 0
        for message in lot:
            texte = json.dumps(message)
            if trame and taille + len(texte) > self.TRAME_MAX:
                yield "[" + ",".join(trame) + "]"
                trame, taille = [], 0
            trame.append(texte)
            taille += len(texte) + 1
        if trame:
            yield "[" + ",".join(trame) + "]"

    def _emetteur(self):
        dernier_envoi = 0.0
        while self.alive:
            with self.cond:
                if not self.file:
                    self.cond.wait(timeout=self.BATTEMENT)
            # laisse les messages d'une meme rafale rejoindre la trame
            attente = self.LOT - (time.monotonic() - dernier_envoi)
            if attente > 0:
                time.sleep(attente)
            with self.cond:
                lot, self.file = self.file, []
            if not self.alive:
                return
            if not lot:
                if time.monotonic() - dernier_envoi < self.BATTEMENT:
                    continue
                lot = [{"type": "_battement"}]
            try:
                for trame in self._trames(lot):
                    self.transport.envoyer(trame)
            except Exception as exc:
                self._mort(f"pair deconnecte ({exc})")
                return
            dernier_envoi = time.monotonic()

    def _lecteur(self):
        while self.alive:
            try:
                texte = self.transport.recevoir(1.0)
            except TimeoutError:
                if time.monotonic() - self.dernier_recu > self.SILENCE_MAX:
                    self._mort(f"plus de nouvelles du pair depuis {self.SILENCE_MAX:.0f} s")
                    return
                continue
            except Exception as exc:
                self._mort(f"pair deconnecte ({exc})")
                return
            self.dernier_recu = time.monotonic()
            try:
                messages = _messages(texte)
            except ValueError as exc:
                print(f"[reseau] message invalide : {exc}")
                continue
            for message in messages:
                if message.get("_relais") == "pair_parti":
                    self._mort("pair deconnecte")
                    return
                if "_relais" in message or message.get("type") in ("_battement", "hello"):
                    continue
                # Le thread de lecture demarre avant que Sync existe : un
                # message peut arriver dans cet intervalle. On l'ignore au lieu
                # de tuer le thread (l'hote plantait a la connexion du pair).
                handler = self.on_message
                if handler is not None:
                    try:
                        handler(message)
                    except Exception as exc:
                        print(f"[reseau] traitement impossible : {exc}")


# --------------------------------------------------------------------------
UPNP_EXTERNAL = None   # port externe retenu (peut differer du port interne)


def try_upnp(port):
    """Ouvre le port sur la box. Retourne (ip_publique, port_externe) ou None.

    Le port interne reste `port` ; seul le port EXTERNE peut changer. C'est ce
    qui permet de contourner une regle fantome : certaines box refusent
    d'ouvrir un port deja mappe par un autre appareil, et refusent aussi de
    supprimer cette regle (NoSuchEntryInArray) puisqu'elle ne leur appartient
    pas. On demande alors un autre port externe.
    """
    try:
        import miniupnpc
    except ImportError:
        print("[upnp] module absent (pip install miniupnpc)")
        return None

    try:
        u = miniupnpc.UPnP()
        u.discoverdelay = 400
        if u.discover() == 0:
            print("[upnp] aucun routeur compatible trouve")
            return None
        u.selectigd()

        public = u.externalipaddress()
        print(f"[upnp] IP locale {u.lanaddr}   IP publique {public}")

        # 1. le port demande, apres une tentative de nettoyage de la vieille regle
        candidates = [port]
        try:
            u.deleteportmapping(port, "TCP")
            print(f"[upnp] ancienne regle sur {port} supprimee")
        except Exception:
            pass

        # 2. autres ports externes si celui-la reste bloque
        candidates += [50000, 50100, 50200, 51000, 52000, 53000]

        for external in candidates:
            try:
                if u.addportmapping(external, "TCP", u.lanaddr, port,
                                    "FruityMulti", ""):
                    if external == port:
                        print(f"[upnp] port {port} ouvert automatiquement")
                    else:
                        print(f"[upnp] le port {port} etait bloque : "
                              f"la box ecoute sur {external} a la place")
                    global UPNP_EXTERNAL
                    UPNP_EXTERNAL = external
                    return public, external
            except Exception:
                continue

        print("[upnp] aucun port n'a pu etre ouvert automatiquement.")
        print(f"       ouvre le port {port}/TCP vers {u.lanaddr} sur ta box,")
        print("       dans la section NAT / redirection de ports.")
        return public, port
    except Exception as exc:
        print(f"[upnp] echec general : {exc}")
        return None


def close_upnp(port):
    global UPNP_EXTERNAL
    ports = [port]
    if UPNP_EXTERNAL and UPNP_EXTERNAL != port:
        ports.append(UPNP_EXTERNAL)
    try:
        import miniupnpc
        u = miniupnpc.UPnP()
        u.discoverdelay = 300
        if u.discover() == 0:
            return
        u.selectigd()
        for p in ports:
            try:
                u.deleteportmapping(p, "TCP")
                print(f"[upnp] port {p} referme")
            except Exception:
                pass
    except Exception:
        pass



# --------------------------------------------------------------------------
# Mise a jour par GitHub Releases
#
# Un plugin VST3 ne peut pas remplacer son propre fichier pendant que FL
# tourne : Windows verrouille le fichier charge. La mise a jour se fait donc en
# deux temps :
#   1. l'agent telecharge dans %LOCALAPPDATA%\FruityMulti\maj ;
#   2. il ecrit un script qui copie les fichiers FL FERME, au prochain demarrage.
# --------------------------------------------------------------------------
def numero_de_version(texte):
    """Extrait le numero de tete : "VERSION 16-sans-surcouche" -> 16."""
    try:
        return int(texte.split()[1].split("-")[0])
    except (IndexError, ValueError):
        return 0


def _api(chemin):
    import json
    import urllib.request
    requete = urllib.request.Request(
        f"https://api.github.com/{chemin}",
        headers={"User-Agent": "FruityMulti"})
    with urllib.request.urlopen(requete, timeout=20) as reponse:
        return json.loads(reponse.read().decode("utf-8"))


def numero_release_latest():
    """Numero de la derniere release, sans passer par l'API GitHub.

    L'API plafonne a 60 appels par heure pour une adresse non authentifiee :
    en usage normal la verification finissait par echouer et la fenetre
    annoncait 'pas de reseau'. On lit la redirection de releases/latest, qui
    n'est pas limitee.
    """
    import urllib.error
    import urllib.request

    class SansRedirection(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    ouvreur = urllib.request.build_opener(SansRedirection)
    requete = urllib.request.Request(
        f"https://github.com/{DEPOT}/releases/latest",
        headers={"User-Agent": "FruityMulti"})
    adresse = ""
    try:
        with ouvreur.open(requete, timeout=20) as reponse:
            adresse = reponse.headers.get("Location", "") or ""
    except urllib.error.HTTPError as exc:
        if exc.headers is not None:
            adresse = exc.headers.get("Location", "") or ""
    if not adresse:
        raise RuntimeError("reponse inattendue de GitHub")

    morceau = adresse.rstrip("/").rsplit("/", 1)[-1]
    chiffres = "".join(c for c in morceau if c.isdigit())
    if not chiffres:
        raise RuntimeError(f"tag illisible : {morceau}")
    return int(chiffres)


def derniere_version():
    """(numero, {nom du fichier: adresse}) de la derniere release."""
    infos = _api(f"repos/{DEPOT}/releases/latest")
    # le tag peut etre "v16", "16", "release-16" : on ne garde que les chiffres
    brut = str(infos.get("tag_name", "0")).lstrip("vV")
    numero = int("".join(c for c in brut if c.isdigit()) or 0)
    fichiers = {a.get("name", ""): a.get("browser_download_url", "")
                for a in infos.get("assets", [])}
    return numero, fichiers


SCRIPT_MAJ = r"""@echo off
chcp 65001 >nul
echo ==========================================
echo   FruityMulti - application de la mise a jour
echo ==========================================
echo.

tasklist /FI "IMAGENAME eq FL64.exe" 2>nul | find /I "FL64.exe" >nul
if not errorlevel 1 (
  echo   FL Studio est encore ouvert : ferme-le, puis relance ce script.
  echo.
  pause
  exit /b 1
)

rem elevation si necessaire (Program Files demande les droits)
net session >nul 2>&1
if errorlevel 1 (
  echo Demande des droits d'administrateur...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set "DEST_VST3=%ProgramFiles%\Common Files\VST3"
copy /Y "%~dp0FruityMultiBridge.vst3" "%DEST_VST3%\FruityMultiBridge.vst3" >nul
if errorlevel 1 (echo ECHEC du plugin) else (echo OK : plugin)

set "DEST_AGENT=%LOCALAPPDATA%\FruityMulti"
if not exist "%DEST_AGENT%" mkdir "%DEST_AGENT%"
copy /Y "%~dp0fm_agent.exe" "%DEST_AGENT%\fm_agent.exe" >nul
if errorlevel 1 (echo ECHEC de l'agent) else (echo OK : agent)

echo.
echo Termine. Relance FL Studio et remets l'effet FruityMulti Bridge.
pause
"""


def lancer_setup (dossier):
    """Lance FruityMultiSetup.exe --apply : il ferme FL, installe, relance FL."""
    import subprocess

    for nom in ("FruityMultiSetup.exe",):
        chemin = os.path.join (dossier, nom)
        if os.path.exists (chemin):
            break
    else:
        # repli : installateur installe avec l'agent
        chemin = os.path.join (os.environ.get ("LOCALAPPDATA", "."),
                               "FruityMulti", "FruityMultiSetup.exe")
        if not os.path.exists (chemin):
            print ("[maj] installateur introuvable : reinstalle une fois a la main")
            return False

    print ("[maj] installation : FL va se fermer puis redemarrer")
    try:
        subprocess.Popen ([chemin, "--apply"], close_fds=True)
        return True
    except Exception as exc:
        print (f"[maj] lancement impossible : {exc}")
        return False


def mettre_a_jour(silencieux=False, lancer_installateur=False):
    """Verifie et telecharge la derniere version. 0 = a jour ou pret."""
    import urllib.request

    try:
        numero, fichiers = derniere_version()
    except Exception as exc:
        print(f"[maj] verification impossible : {exc}")
        return 2

    print(f"[maj] derniere version : {numero}   la mienne : {AGENT_VERSION}")
    if numero <= AGENT_VERSION:
        print("[maj] deja a jour")
        return 0

    dossier = os.path.join(os.environ.get("LOCALAPPDATA", "."), "FruityMulti", "maj")
    os.makedirs(dossier, exist_ok=True)

    manquants = []
    for nom in ("FruityMultiBridge.vst3", "fm_agent.exe"):
        adresse = fichiers.get(nom)
        if not adresse:
            manquants.append(nom)
            continue
        print(f"[maj] telechargement de {nom}...")
        urllib.request.urlretrieve(adresse, os.path.join(dossier, nom))

    chemin_script = os.path.join(dossier, "appliquer-maj.cmd")
    with open(chemin_script, "w", encoding="utf-8") as f:
        f.write(SCRIPT_MAJ)

    print(f"[maj] version {numero} telechargee dans :")
    print(f"      {dossier}")
    if manquants:
        print(f"[maj] ATTENTION : absent de la release : {', '.join(manquants)}")
    if lancer_installateur:
        lancer_setup (dossier)
    else:
        print("[maj] pour l'installer : relance FruityMultiSetup.exe")
    return 1


def verifier_versions(version_plugin):
    """Le plugin et l'agent doivent etre de la meme version."""
    numero = numero_de_version(version_plugin)
    if numero == AGENT_VERSION:
        return True
    print(f"[maj] VERSIONS DESALIGNEES : plugin {numero}, agent {AGENT_VERSION}")
    print(f"      les deux postes doivent avoir la meme version.")
    print(f"      mets a jour : fm_agent.exe --maj   ({ADRESSE_RELEASES})")
    return False


def gui_etat(plugin, texte):
    """Annonce a la fenetre du plugin ce qui se passe (ligne d'etat).

    C'est de l'affichage : une erreur ici ne doit jamais interrompre la
    session.
    """
    try:
        plugin.cmd(f"STATE {texte}")
    except Exception:
        pass


class _Journal:
    """Double la sortie de l'agent dans un fichier.

    Lance par la fenetre du plugin, l'agent n'a pas de console : sans ce
    fichier, il ne reste rien a relire apres une session.
    """

    def __init__(self, flux, chemin):
        self.flux = flux
        self.fichier = None
        try:
            self.fichier = open(chemin, "a", encoding="utf-8", errors="replace")
            self.fichier.write("\n" + "=" * 60 + "\n")
            self.fichier.write(time.strftime("%d/%m/%Y %H:%M:%S") + "  demarrage\n")
            self.fichier.flush()
        except OSError:
            self.fichier = None

    def write(self, texte):
        if self.flux is not None:
            try:
                self.flux.write(texte)
            except Exception:
                pass
        if self.fichier is not None:
            try:
                self.fichier.write(texte)
                self.fichier.flush()
            except Exception:
                pass

    def flush(self):
        if self.flux is not None:
            try:
                self.flux.flush()
            except Exception:
                pass
        if self.fichier is not None:
            try:
                self.fichier.flush()
            except Exception:
                pass


def verrou_unique():
    """Un seul agent a la fois.

    Plusieurs agents qui se disputent le plugin le bloquaient : chacun gardait
    une connexion ouverte, et le serveur ne servait plus personne.
    """
    try:
        import ctypes
        from ctypes import wintypes

        noyau = ctypes.WinDLL("kernel32", use_last_error=True)
        noyau.CreateMutexW.restype = wintypes.HANDLE
        noyau.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL,
                                       wintypes.LPCWSTR]
        poignee = noyau.CreateMutexW(None, False, "Local\\FruityMultiAgent")
        if poignee:
            # 183 = ERROR_ALREADY_EXISTS : un agent tourne deja
            if ctypes.get_last_error() == 183:
                return None
            return poignee
    except Exception:
        pass
    return True   # pas de verification possible : on laisse passer


def ouvrir_journal():
    """Tout ce que l'agent affiche part aussi dans journal.txt."""
    dossier = dossier_local()
    if not os.path.isdir(dossier):
        return
    chemin = os.path.join(dossier, "journal.txt")
    try:
        # on garde les 400 derniers milliers de caracteres
        if os.path.exists(chemin) and os.path.getsize(chemin) > 400_000:
            with open(chemin, "r", encoding="utf-8", errors="replace") as f:
                contenu = f.read()[-200_000:]
            with open(chemin, "w", encoding="utf-8", errors="replace") as f:
                f.write(contenu)
    except OSError:
        pass
    sys.stdout = _Journal(sys.stdout, chemin)
    sys.stderr = sys.stdout


def surveiller_parent(pid):
    """Quitte l'agent quand FL se ferme.

    Sans ca, apres un plantage de FL, l'agent orphelin gardait le verrou :
    la session suivante sortait aussitot ("un agent tourne deja").
    """
    if sys.platform != "win32":
        return
    import ctypes
    noyau = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    poignee = noyau.OpenProcess(SYNCHRONIZE, False, pid)
    if not poignee:
        print(f"[sync] FL (pid {pid}) deja ferme : arret")
        os._exit(0)

    def attendre():
        noyau.WaitForSingleObject(poignee, 0xFFFFFFFF)
        print("[sync] FL s'est ferme : arret de l'agent")
        ecrire_compat([], [], [])
        os._exit(0)

    threading.Thread(target=attendre, daemon=True).start()


def main():
    # Sortie ligne par ligne : sans ca, lorsque l'agent est lance par la GUI
    # (stdout redirige vers un pipe), Python bufferise par blocs de 8 Ko et
    # le journal de l'interface reste vide.
    try:
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)
    except Exception:
        pass

    ouvrir_journal()

    parser = argparse.ArgumentParser(description="FruityMulti - synchronisation")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--heberger", action="store_true",
                       help="ouvrir une session par le relais (hote)")
    group.add_argument("--rejoindre", metavar="CODE",
                       help="rejoindre une session : CODE, ou ip:port/CODE en direct")
    group.add_argument("--listen", type=int, metavar="PORT",
                       help="ouvrir une session en direct, sans relais (hote)")
    group.add_argument("--peer", metavar="HOTE:PORT/CODE",
                       help="rejoindre une session directe (invite)")
    parser.add_argument("--plugin", default=f"{PLUGIN_HOST}:{PLUGIN_PORT}",
                        help="adresse du plugin local")
    parser.add_argument("--once", action="store_true",
                        help="ne pas se reconnecter apres une deconnexion")
    parser.add_argument("--maj", action="store_true",
                        help="telecharger la derniere version publiee")
    parser.add_argument("--check", action="store_true",
                        help="imprimer la derniere version publiee (MAJ n)")
    parser.add_argument("--installer", action="store_true",
                        help="apres telechargement, lance l'installateur")
    parser.add_argument("--parent", type=int, metavar="PID",
                        help="s'arreter quand ce processus (FL) se ferme")
    args = parser.parse_args()
    if args.parent:
        surveiller_parent(args.parent)

    if args.maj:
        return mettre_a_jour(lancer_installateur=args.installer)

    if args.check:
        try:
            numero = numero_release_latest()
            print(f"MAJ {numero}")
        except Exception:
            print("MAJ 0")
        return 0
    if not (args.heberger or args.rejoindre or args.listen or args.peer):
        parser.error("il faut --heberger, --rejoindre, --listen, --peer, --maj ou --check")

    # un seul agent de SESSION a la fois (les --check et --maj sont libres)
    verrou = verrou_unique()
    if verrou is None:
        print("[sync] un agent tourne deja : rien a faire")
        return 0

    host, _, port = args.plugin.partition(":")
    try:
        plugin = Plugin(host, int(port))
    except OSError as exc:
        print(f"Plugin injoignable sur {args.plugin} : {exc}")
        print("FL est-il lance, avec l'effet FruityMulti Bridge ?")
        return 1

    # Le plugin n'accepte qu'un seul client : si un autre agent tourne deja,
    # on reessaie au lieu de planter.
    last_error = None
    for attempt in range(6):
        try:
            reponse = plugin.cmd("PING")[0]
            if reponse == "OK":
                last_error = None
                break
            if "version de FL" in reponse:
                # FL mis a jour depuis la compilation du plugin : inutile
                # d'insister, et surtout pas "un autre agent l'utilise"
                print("[sync] le plugin ne reconnait pas cette version de FL")
                print("[sync] il faut la version de FL prevue par FruityMulti")
                gui_etat(plugin, "ERREUR Version de FL non reconnue : mets FruityMulti à jour")
                return 1
            last_error = "reponse inattendue"
        except (OSError, TimeoutError) as exc:
            last_error = exc
        print(f"[sync] plugin occupe ou lent ({last_error}) — nouvel essai "
              f"{attempt + 1}/6")
        time.sleep(1.0)

    if last_error is not None:
        print("Le plugin ne repond pas : un autre agent l'utilise peut-etre deja.")
        return 1

    print(f"[sync] plugin connecte ({args.plugin})")
    ecrire_compat([], [], [])   # rien de la session precedente

    try:
        version = plugin.cmd("VERSION")[0]
    except (OSError, IndexError):
        version = "inconnue"
    print(f"[sync] version du plugin : {version}")
    verifier_versions(version)
    est_hote = bool(args.heberger or args.listen)
    gui_etat(plugin, "ROLE " + ("hote" if est_hote else "invite"))

    # --- ou se retrouver ---------------------------------------------------
    serveur = None        # socket d'ecoute en direct (hote)
    port_upnp = None
    pair_direct = None    # (ip, port) a joindre en direct (invite)
    transport = None
    if est_hote:
        code = nouveau_code()
        par_relais = not args.listen
        if par_relais:
            try:
                transport = ouvrir_relais(code, "hote")
                adresse = code
            except Exception as exc:
                print(f"[reseau] relais injoignable ({exc}) : connexion directe")
                par_relais = False
        if not par_relais:
            port_ecoute = args.listen or PORT_DIRECT
            publique = try_upnp(port_ecoute)
            if publique:
                ip_hote, port_hote = publique
                port_upnp = port_ecoute
            else:
                # pas de box compatible : on donne l'adresse du reseau local
                try:
                    ip_hote = socket.gethostbyname(socket.gethostname())
                except OSError:
                    ip_hote = "127.0.0.1"
                port_hote = port_ecoute
            serveur = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            serveur.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            serveur.bind(("0.0.0.0", port_ecoute))
            serveur.listen(1)
            adresse = f"{ip_hote}:{port_hote}/{code}"

        print("")
        print("=" * 52)
        print("  donnez ce code a votre invite :")
        print(f"        {adresse}")
        print("=" * 52)
        print("")
        gui_etat(plugin, f"ADRESSE {adresse}")
        gui_etat(plugin, f"Code {adresse} : en attente de l'invité")
    else:
        cible = (args.rejoindre or args.peer).strip()
        if est_un_code(cible):
            par_relais = True
            code = cible.upper()
        else:
            par_relais = False
            cible, _, code = cible.partition("/")
            code = code.strip().upper()
            ip, _, port_pair = cible.partition(":")
            pair_direct = (ip, int(port_pair or PORT_DIRECT))
        gui_etat(plugin, f"Connexion à la session {code or cible}…")

    def fermer_tout():
        plugin.close()
        if serveur is not None:
            serveur.close()
        if port_upnp is not None:
            close_upnp(port_upnp)

    while True:
        # --- etablissement de la connexion ---------------------------------
        try:
            if est_hote and par_relais:
                if transport is None:
                    transport = ouvrir_relais(code, "hote")
                attendre_pair_relais(transport)
            elif est_hote:
                sock, addr = serveur.accept()
                print(f"[sync] connexion entrante : {addr[0]}:{addr[1]}")
                transport = TransportTcp(sock)
            elif par_relais:
                transport = ouvrir_relais(code, "invite")
                attendre_pair_relais(transport)
            else:
                sock = socket.create_connection(pair_direct, timeout=10.0)
                transport = TransportTcp(sock)
            pair = poignee_de_main(transport, est_hote, code)
        except KeyboardInterrupt:
            raise
        except Refus as exc:
            print(f"[reseau] refuse : {exc}")
            gui_etat(plugin, f"Refusé : {exc}")
            if transport is not None:
                transport.fermer()
                transport = None
            if not est_hote:
                time.sleep(3.0)
            continue
        except Exception as exc:
            if transport is not None:
                transport.fermer()
                transport = None
            raison = getattr(exc, "winerror", None)
            print(f"[reseau] connexion impossible ({exc})")
            if pair_direct is not None:
                if raison == 10061:
                    print("[sync]   -> personne n'ecoute a cette adresse.")
                    print("[sync]      verifie le PORT : la box peut avoir ouvert")
                    print("[sync]      un autre que celui affiche chez l'hote.")
                elif raison == 10060:
                    print("[sync]   -> pas de reponse : le port est filtre")
                    print("[sync]      (pare-feu ou box). Demande a l'hote de")
                    print("[sync]      verifier que l'agent est bien lance chez lui.")
                elif raison == 11001:
                    print("[sync]   -> adresse introuvable : erreur de frappe ?")
            print("[sync] nouvelle tentative dans 3 s (Ctrl+C pour arreter)")
            time.sleep(3.0)
            continue

        poste = pair.get("poste", "?")
        print(f"[sync] session ouverte avec {poste} (version {pair.get('version')})")
        gui_etat(plugin, f"PAIR {poste}")
        gui_etat(plugin, f"Connecté à {poste} ({'hôte' if est_hote else 'invité'})")

        # --- session -------------------------------------------------------
        sync = None
        link = Link(transport,
                    lambda m: sync.on_message(m) if sync is not None else None)
        sync = Sync(plugin, link, est_hote)

        try:
            sync.run()
        except KeyboardInterrupt:
            raise
        except PluginLost as exc:
            # FL a ete ferme, ou l'effet a ete retire : on attend son retour
            # au lieu de demander a l'utilisateur de relancer l'agent.
            print(f"[sync] FL a disparu ({exc})")
            print("[sync] on attend le retour de l'effet FruityMulti Bridge...")
            link.alive = False
            transport.fermer()
            transport = None
            ecrire_compat([], [], [])
            while True:
                time.sleep(2.0)
                try:
                    plugin = Plugin(host, int(port))
                except OSError:
                    continue
                try:
                    if plugin.ping():
                        print("[sync] plugin retrouve")
                        break
                except (OSError, TimeoutError):
                    pass
                plugin.close()
            continue
        except SystemExit:
            fermer_tout()
            return 1
        except Exception as exc:
            print(f"[sync] session interrompue : {exc}")

        link.alive = False
        transport.fermer()
        transport = None
        ecrire_compat([], [], [])
        gui_etat(plugin, "PAIR -")

        if args.once:
            break
        print("[sync] on attend un nouveau pair")
        gui_etat(plugin, "Pair parti : en attente" if est_hote
                 else "Session perdue : reconnexion…")
        if not est_hote:
            time.sleep(2.0)

    fermer_tout()
    return 0


if __name__ == "__main__":
    sys.exit(main())
