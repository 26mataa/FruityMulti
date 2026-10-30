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
import base64
import hashlib
import json
import os
import re
import socket
import sys
import threading
import time
import zlib
from collections import Counter

# --- Mises a jour -----------------------------------------------------------
# Le depot public qui heberge les versions. L'agent y lit la derniere release
# et telecharge le plugin + lui-meme. A incrementer a chaque publication.
DEPOT = "26mataa/FruityMulti"
AGENT_VERSION = 47
ADRESSE_RELEASES = f"https://github.com/{DEPOT}/releases/latest"

PLUGIN_HOST = "127.0.0.1"
PLUGIN_PORT = 47831
POLL = 0.005

# Echange du projet : l'invite charge le .flp de l'hote a la connexion.
MORCEAU_PROJET = 128 * 1024        # deux morceaux tiennent dans une trame
TAILLE_PROJET_MAX = 64 * 1024 * 1024
TAILLE_PROJET_MIN = 4096           # en dessous : pas un vrai projet (voir plus bas)
DELAI_SAUVEGARDE = 60.0            # gros projet : FL peut mettre du temps
DELAI_CHOIX = 120.0                # l'invite a deux minutes pour repondre
DELAI_COUPURE = 15.0               # apres LOADPROJECT, le plugin tombe vite
DELAI_RETOUR_PLUGIN = 300.0        # Zenology et consorts : des dizaines de s
DELAI_STABILITE = 60.0
DELAI_VEILLE = 5.0                 # le plugin coupe un client muet a 20 s
DELAI_ECHO_TRANSPORT = 1.0
# Bouton de tempo tourne a la souris : la derniere valeur suffit, le relais n'a
# qu'une file et les notes attendent derriere.
INTERVALLE_TEMPO = 0.25
INTERVALLE_POTARD = 0.1               # volume, pan, stereo d'une tranche
TOUCHENT_NOTES = ("add", "del", "pattern", "reset", "delchannels", "racklist",
                  "canal_debut", "canal_morceau")
# Un canal part en entier : son .fst (reglages du plugin) et son sample.
TAILLE_CANAL_MAX = 8 * 1024 * 1024
TAILLE_SAMPLE_MAX = 4 * 1024 * 1024   # le relais passe 3 Mo en 47 s, les notes attendent
EVT_SAMPLE = 0xC4
EVT_TYPE_CANAL = 0x15
EVT_NOM_PLUGIN = 0xC9
EVT_NOUVEAU_PLUGIN = 0xD4
EVT_ETAT_PLUGIN = 0xD5
PLUGIN_ENVELOPPE = "Fruity Wrapper"   # VST et VST3 : le vrai nom est dans l'etat
VARIABLE_USINE = "%FLStudioFactoryData%"
VARIABLE_UTILISATEUR = "%FLStudioUserData%"
VARIABLE_PARTAGE = "%ILSharedData%"      # Documents\Image-Line : kits de l'utilisateur
# Depuis le plugin 47-d, DELCHANNEL ouvre son etape d'historique comme FL : un
# Ctrl+Z rend le canal et ses notes au lieu de planter (POUR-CLAUDE, 13.15).
SUPPRESSION_RECUE = True
SAUVEGARDES_GARDEES = 10
# Boucle lente du mixeur : tout ce que STATUS ne donne pas a chaque cycle.
INTERVALLE_MIXEUR = 0.5
INTERVALLE_ROUTAGE = 2.0
PARAMS_PAR_EFFET = 64
TRANCHES_RAPIDES = 16                 # les suivantes ne passent que par MIXFULL
DRAPEAUX_TRANCHE = ("polarite", "inversion", "slots")
DECALAGES_EQ = ("1FD0", "1FD1", "1FD2", "1FD8", "1FD9", "1FDA", "1FE0", "1FE1", "1FE2")
TOLERANCE_REC = 1 << 17               # FL quantifie : en dessous, ce n'est pas un geste


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


def _racines_image_line():
    """Documents n'est pas toujours dans le profil (OneDrive) : FL note le vrai
    dossier dans le registre, et Windows connait le sien."""
    racines = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Image-Line\Shared\Paths") as cle:
            racines.append(winreg.QueryValueEx(cle, "Shared data")[0])
    except (ImportError, OSError):
        pass
    try:
        import ctypes
        tampon = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, tampon) == 0:
            racines.append(os.path.join(tampon.value, "Image-Line"))
    except (AttributeError, OSError):
        pass
    racines.append(os.path.expanduser("~\\Documents\\Image-Line"))
    return racines


def _dossiers_fl():
    bases = []
    for racine in _racines_image_line():
        for version in ("FL Studio", "FL Studio 21", "FL Studio 20"):
            base = os.path.join(racine, version, "Presets", "Plugin database")
            connus = [os.path.normcase(b) for b in bases]
            if os.path.isdir(base) and os.path.normcase(base) not in connus:
                bases.append(base)
    return bases


def _sous_arbres(base, arbre):
    """FL range ses plugins dans <arbre>, ceux que l'utilisateur a installes
    dans Installed\\<arbre>."""
    return [os.path.join(base, arbre), os.path.join(base, "Installed", arbre)]


def scan_plugins():
    """Renvoie {"fl": [...], "vst3": [...], "vst2": [...]}."""
    fl, vst3, vst2 = set(), set(), set()

    for base in _dossiers_fl():
        for generateurs in _sous_arbres(base, "Generators"):
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


def sous_dossier_local(nom):
    dossier = os.path.join(dossier_local(), nom)
    try:
        os.makedirs(dossier, exist_ok=True)
    except OSError:
        pass
    return dossier


def garder_recentes(dossier, nombre):
    """Ne garde que les `nombre` fichiers les plus recents du dossier."""
    try:
        fichiers = [os.path.join(dossier, nom) for nom in os.listdir(dossier)]
        fichiers.sort(key=os.path.getmtime, reverse=True)
        for chemin in fichiers[nombre:]:
            os.remove(chemin)
    except OSError:
        pass


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
def nom_generique(nom):
    """Un canal cree par NEWCHANNEL n'a pas de nom ; ceux de FL, "Sampler"."""
    cle = (nom or "").strip().lower()
    return not cle or cle.startswith("sampler")


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
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.sock.settimeout(5.0)
        self.buf = b""
        self.lock = threading.Lock()
        self.veille = None

    def veiller(self):
        """Le plugin coupe un client muet au bout de 20 s, et personne ne lui
        parle tant que l'invite n'est pas la."""
        if self.veille is not None:
            return
        arret = threading.Event()

        def boucle():
            while not arret.wait(DELAI_VEILLE):
                try:
                    self.ping()
                except (OSError, IndexError):
                    # une reponse en retard decalerait toutes les suivantes
                    try:
                        self.reconnecter()
                    except OSError:
                        pass

        fil = threading.Thread(target=boucle, daemon=True)
        self.veille = (arret, fil)
        fil.start()

    def fin_veille(self):
        if self.veille is None:
            return
        arret, fil = self.veille
        self.veille = None
        arret.set()
        fil.join()

    def reconnecter(self):
        """Connexion neuve sur le meme objet : plugin recharge, ou reponse en
        retard qui decalerait toutes les suivantes."""
        with self.lock:
            self.close()
            self.buf = b""
            self.sock = socket.create_connection((self.host, self.port),
                                                 timeout=5.0)
            self.sock.settimeout(5.0)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def attendre_fermeture(self, delai):
        """Vrai quand le plugin coupe la connexion. Sans RIEN envoyer : sa
        minuterie est deja arretee, une commande ne recevrait jamais de reponse
        et son thread serveur resterait bloque dessus."""
        limite = time.monotonic() + delai
        with self.lock:
            self.sock.settimeout(0.2)
            try:
                while time.monotonic() < limite:
                    try:
                        if not self.sock.recv(4096):
                            return True
                    except TimeoutError:
                        pass
            except OSError:
                return True
            finally:
                self.sock.settimeout(5.0)
        return False

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("plugin ferme")
            self.buf += chunk
        line, _, rest = self.buf.partition(b"\n")
        self.buf = rest
        return line.decode("utf-8", "replace").strip()

    def cmd(self, text, delai=None):
        with self.lock:
            if delai is not None:
                self.sock.settimeout(delai)
            try:
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
            finally:
                if delai is not None:
                    self.sock.settimeout(5.0)

    # -- raccourcis --------------------------------------------------------
    def ping(self):
        return self.cmd("PING")[0] == "OK"

    def sauver_projet(self, chemin):
        """Ecrit le projet ouvert dans `chemin`. None si c'est fait, sinon la
        raison.

        Le "OK" du plugin ne suffit pas : de mauvais arguments ont longtemps
        produit un squelette de 1122 octets, en-tete valide, que FL refuse de
        recharger. Le projet de l'invite ne repose que sur ce fichier."""
        reponse = self.cmd(f"SAVEPROJECT {chemin}", delai=DELAI_SAUVEGARDE)[0]
        if not reponse.startswith("SAVEPROJECT OK"):
            return reponse
        try:
            with open(chemin, "rb") as fichier:
                entete = fichier.read(4)
            taille = os.path.getsize(chemin)
        except OSError as exc:
            return f"fichier illisible ({exc})"
        if entete != b"FLhd":
            return "ce n'est pas un projet FL"
        if taille < TAILLE_PROJET_MIN:
            return f"fichier incomplet ({taille} octets)"
        return None

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
                "mute": int(mmute),
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
                "clic": len(parts) > 26 and parts[26] == "1",
                "mode": int(parts[27]) if len(parts) > 27 else -1,
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
        except IndexError:
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
        # format : index:tranche:nom, separes par une espace. Le nom peut
        # contenir des espaces : on coupe sur les entetes, pas sur les blancs
        # (sinon "FL Keys" devenait "FL", et "Kick 808" et "Kick 909" le
        # meme canal).
        _, _, suite = line.partition(" ")
        _, _, suite = suite.partition(" ")
        entetes = list(re.finditer(r"(?:^| )(\d+):(-?\d+):", suite))
        out = []
        for k, entete in enumerate(entetes):
            fin = entetes[k + 1].start() if k + 1 < len(entetes) else len(suite)
            out.append((int(entete.group(1)), int(entete.group(2)),
                        suite[entete.end():fin]))
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

    def new_channel(self):
        """Cree un canal vide (Sampler). Sert a garder les positions alignees."""
        return self.cmd("NEWCHANNEL")[0]

    def load_plugin(self, chemin):
        return self.cmd(f"LOADPLUGIN {chemin}")[0]

    def save_channel(self, index, chemin):
        return self.cmd(f"SAVECHANNEL {index} {chemin}")[0]

    def load_channel(self, index, chemin, nom=""):
        return self.cmd(f"LOADCHANNEL {index} {chemin}|{nom}")[0]

    def replace_channel(self, index, chemin, nom=""):
        return self.cmd(f"REPLACECHANNEL {index} {chemin}|{nom}")[0]

    def delete_channel(self, index):
        return self.cmd(f"DELCHANNEL {index}")[0]

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
            for racine in _sous_arbres(base, arbre):
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

    def mix_full(self):
        """Tout le mixeur : {tranche: fiche}, ou None."""
        try:
            line = self.cmd("MIXFULL")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("MIXFULL "):
            return None
        tranches = {}
        try:
            self.nb_tranches = int(line.split()[1])
            for bloc in line.split(" ", 2)[2].split(";"):
                tete, *slots = bloc.split("/")
                track, valeurs = tete.split(":")
                v = [int(x) for x in valeurs.split(",")]
                tranches[int(track)] = {
                    "vol": v[0] / 1073741824.0,
                    "pan": (v[1] / 1073741824.0) * 2.0 - 1.0,
                    "stereo": (v[2] / 1073741824.0) * 2.0 - 1.0,
                    "mute": v[3],
                    "drapeaux": dict(zip(DRAPEAUX_TRANCHE, v[5:8])),
                    "eq": dict(zip(DECALAGES_EQ, v[8:17])),
                    "slots": {int(a): (int(b), int(c)) for a, b, c in
                              (x.split(":") for x in slots)},
                }
        except (ValueError, IndexError):
            return None
        return tranches

    def routes(self):
        """Envois actifs : {(source, cible): niveau}, ou None."""
        try:
            line = self.cmd("ROUTES")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("ROUTES "):
            return None
        envois = {}
        try:
            for mot in line.split()[2:]:
                source, reste = mot.split(">")
                cible, niveau = reste.split(":")
                envois[(int(source), int(cible))] = int(niveau)
        except ValueError:
            return None
        return envois

    def track_names(self):
        """{tranche: (couleur, nom)}, ou None."""
        try:
            line = self.cmd("TRACKNAMES")[0]
        except (OSError, TimeoutError, IndexError):
            return None
        if not line.startswith("TRACKNAMES "):
            return None
        noms = {}
        try:
            for bloc in line.split(" ", 2)[2].split("|"):
                track, couleur, nom = bloc.split(":", 2)
                noms[int(track)] = (int(couleur), nom)
        except (ValueError, IndexError):
            return None
        return noms

    def set_mix_count(self, compte):
        return self.cmd(f"MIXCOUNT {compte}")[0]

    def rec_set(self, track, slot, decalage, brut):
        return self.cmd(f"RECSET {track} {slot} {decalage} {brut}")[0]

    def track_flag(self, track, nom, valeur):
        return self.cmd(f"TRACKFLAG {track} {nom} {1 if valeur else 0}")[0]

    def route_set(self, source, cible, actif, niveau):
        return self.cmd(f"ROUTESET {source} {cible} {actif} {niveau}")[0]

    def set_track_name(self, track, nom):
        return self.cmd(f"TRACKNAME {track} {nom}")[0]

    def set_track_color(self, track, couleur):
        return self.cmd(f"TRACKCOLOR {track} {couleur}")[0]

    def fx_save(self, track, slot, chemin):
        return self.cmd(f"FXSAVE {track} {slot} {chemin}")[0]

    def fx_load(self, track, slot, chemin=""):
        """Pose un .fst dans le slot, ou le vide sans chemin."""
        return self.cmd(f"FXLOAD {track} {slot} {chemin}")[0]

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
def canaux_retires(avant, apres):
    """Positions de `avant` disparues de `apres`, ou None si le rack a change
    autrement qu'en perdant des canaux (ajout, deplacement, renommage)."""
    retires = []
    suivant = 0
    for position, canal in enumerate(avant):
        if suivant < len(apres) and apres[suivant] == canal:
            suivant += 1
        else:
            retires.append(position)
    return retires if suivant == len(apres) else None


def decaler_notes(notes, retires):
    """Notes par canal, renumerotees comme FL apres la suppression de `retires`."""
    sortie = {}
    for canal, liste in notes.items():
        if canal not in retires:
            sortie[canal - sum(1 for r in retires if r < canal)] = liste
    return sortie


def inserer_notes(notes, ajoutes):
    """L'inverse : des canaux sont revenus aux positions `ajoutes` (Ctrl+Z)."""
    sortie = {}
    for canal, liste in notes.items():
        for position in sorted(ajoutes):
            if canal >= position:
                canal += 1
        sortie[canal] = liste
    return sortie


def canaux_absents(voulus, locaux):
    """Canaux de `voulus` sans homonyme dans `locaux`, dans l'ordre du rack."""
    noms = Counter((n or "").strip().lower() for _i, _t, n in locaux)
    absents = []
    for canal in voulus:
        cle = (canal[2] or "").strip().lower()
        if noms[cle] > 0:
            noms[cle] -= 1
        else:
            absents.append(canal)
    return absents


def evenements_fl(brut):
    """Evenements d'un .fst ou .flp : (identifiant, debut, valeur, fin)."""
    if brut[:4] != b"FLhd":
        raise ValueError("pas un fichier FL")
    pos = 8 + int.from_bytes(brut[4:8], "little")
    if brut[pos:pos + 4] != b"FLdt":
        raise ValueError("pas de bloc FLdt")
    pos += 8
    while pos < len(brut):
        debut = pos
        ev = classe = brut[pos]
        pos += 1
        if ev == 0xAC:
            # Identifiant sur 32 bits : la classe est dans les 4 bits hauts.
            ev = int.from_bytes(brut[pos:pos + 4], "little")
            classe = (ev >> 24) & 0xF0
            pos += 4
        if classe >= 0xC0:
            taille = decalage = 0
            while True:
                octet = brut[pos]
                pos += 1
                taille |= (octet & 0x7F) << decalage
                decalage += 7
                if not octet & 0x80:
                    break
        else:
            taille = 1 if classe < 0x40 else 2 if classe < 0x80 else 4
        yield ev, debut, pos, pos + taille
        pos += taille


def chemin_sample(brut):
    """Chemin du sample d'un canal, tel que FL l'a ecrit dans le .fst."""
    for ev, _debut, valeur, fin in evenements_fl(brut):
        if ev == EVT_SAMPLE:
            return brut[valeur:fin].decode("utf-16-le", "replace").rstrip("\0") or None
    return None


def modifier_evenements(brut, neufs):
    """Le meme .fst avec d'autres valeurs : {identifiant: octets, None = retire}."""
    bloc = 8 + int.from_bytes(brut[4:8], "little")
    corps = bytearray()
    for ev, debut, valeur, fin in evenements_fl(brut):
        if ev not in neufs:
            corps += brut[debut:fin]
        elif neufs[ev] is None:
            continue
        elif ev < 0xC0:
            corps += brut[debut:valeur] + neufs[ev]
        else:
            corps.append(ev)
            reste = len(neufs[ev])
            while True:
                octet = reste & 0x7F
                reste >>= 7
                corps.append(octet | (0x80 if reste else 0))
                if not reste:
                    break
            corps += neufs[ev]
    return brut[:bloc + 4] + len(corps).to_bytes(4, "little") + bytes(corps)


def remplacer_sample(brut, chemin):
    """Le meme .fst, avec le sample a un autre endroit."""
    return modifier_evenements(brut, {EVT_SAMPLE: (chemin + "\0").encode("utf-16-le")})


def plugin_requis(brut):
    """(nom, chemin) du plugin d'un canal, None pour un canal sans plugin.

    Un VST passe par l'enveloppe de FL : son etat est une suite de blocs
    (identifiant 32 bits, taille 64 bits), 54 = nom, 55 = chemin.
    """
    nom = etat = None
    for ev, _debut, valeur, fin in evenements_fl(brut):
        if ev == EVT_NOM_PLUGIN:
            nom = brut[valeur:fin].decode("utf-16-le", "replace").rstrip("\0")
        elif ev == EVT_ETAT_PLUGIN:
            etat = brut[valeur:fin]
    if not nom:
        return None
    if nom != PLUGIN_ENVELOPPE or etat is None:
        return nom, ""
    chemin = ""
    pos = 4
    while pos + 12 <= len(etat):
        bloc = int.from_bytes(etat[pos:pos + 4], "little")
        taille = int.from_bytes(etat[pos + 4:pos + 12], "little")
        contenu = etat[pos + 12:pos + 12 + taille]
        if bloc == 54:
            nom = contenu.decode("utf-8", "replace")
        elif bloc == 55:
            chemin = contenu.decode("utf-8", "replace")
        pos += 12 + taille
    return nom, chemin


def sans_plugin(brut):
    """Le meme canal sans son plugin : FL affiche sinon une boite bloquante
    (plugin introuvable) tant que personne ne clique dessus."""
    neufs = {EVT_NOM_PLUGIN: b"\0\0", EVT_ETAT_PLUGIN: None, EVT_TYPE_CANAL: b"\0"}
    for ev, _debut, valeur, fin in evenements_fl(brut):
        if ev == EVT_NOUVEAU_PLUGIN:
            neufs[ev] = brut[valeur:valeur + 8] + b"\xff\xff\xff\xff" + brut[valeur + 12:fin]
            break
    return modifier_evenements(brut, neufs)


def fichier_sample(chemin):
    """Fichier a joindre pour ce sample. Rien pour ceux livres avec FL : l'autre
    les a au meme endroit logique."""
    if not chemin or chemin.startswith(VARIABLE_USINE):
        return None
    for variable, sous_dossier in ((VARIABLE_UTILISATEUR, "FL Studio"), (VARIABLE_PARTAGE, "")):
        if chemin.startswith(variable):
            reste = chemin[len(variable):].lstrip("\\/")
            for racine in _racines_image_line():
                complet = os.path.join(racine, sous_dossier, reste)
                if os.path.isfile(complet):
                    return complet
            return None
    return chemin if os.path.isfile(chemin) else None


def diff_additions(old, new):
    """Notes presentes dans `new` mais pas dans `old`."""
    co, cn = Counter(old), Counter(new)
    return list((cn - co).elements())


def diff_removals(old, new):
    """Notes presentes dans `old` mais plus dans `new`."""
    co, cn = Counter(old), Counter(new)
    return list((co - cn).elements())


class Sync:
    def __init__(self, plugin, link, is_host, pair=None, annonces=(), code="",
                 accepter_projet=False):
        self.plugin = plugin
        self.link = link
        self.is_host = is_host
        self.annonces = list(annonces)   # STATE a redire a un plugin recharge
        self.code = code
        self.accepter_projet = accepter_projet
        self.poste_pair = (pair or {}).get("poste", "?")
        self.incoming = []
        self.incoming_lock = threading.Lock()
        self.sent_initial = False
        self.label = "host" if is_host else "guest"
        self.sent_plugins = False
        self._presets = None
        self.my_plugins = None
        self.peer_plugins = None
        # None = pas encore teste, False = plugin sans POLL
        self.supports_poll = None
        # Echange du projet. Hote : a_offrir, offert, envoye. Invite : attente,
        # choix, reception, chargement. None = pas d'echange en cours.
        self.projet_file = []
        self.projet_attendu = None
        self.projet_morceaux = {}
        self.projet_pourcent = 0
        self.projet_limite = 0.0
        self.projet_lecture = 0.0
        self.projet_etat = None
        if pair and pair.get("projet"):
            self.projet_etat = "a_offrir" if is_host else "attente"
        self.lire_projet()

    def lire_projet(self):
        """Tout ce que l'agent sait du projet ouvert. Rappele apres un
        chargement : plus rien de ce qu'on savait n'est vrai."""
        self.snapshot = self.plugin.state()
        self.tempo = self.plugin.tempo()
        self.playing = self.plugin.playing()
        self.pattern = self.plugin.pattern()
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
        self.rack_hote = []
        self.signature_rack = None   # (index, nom) au dernier passage
        self.canal_recu = None
        self.averti_noms = set()
        self.position = None
        self.channels_state = None
        self.peer_channels = None
        self.channels_warned = None
        self.last_channels = 0.0
        self.channel_mix_state = None
        self.last_channel_mix = 0.0
        # effets du mixeur : etat connu et cadence (une requete par seconde)
        self.role_sent = False
        self.tempo_limite = 0.0
        self.tempo_prochain = 0.0
        self.potard_prochain = {}
        self.pattern_limite = 0.0
        self.transport_limite = 0.0
        self.mode = None             # 0 = PAT, 1 = SONG
        self.mode_limite = 0.0
        self.clic = False
        self.notes_retenues = []
        self.fx = None
        self.fx_attendus = {}
        self.fx_recu = None
        self.mix_cache = None
        self.mix_tard = None
        self.compte_tranches = None
        self.envois = None
        self.noms_tranches = None
        self.last_mixeur = 0.0
        self.last_routage = 0.0
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

    def publier_rack(self, channels=None):
        """Rack complet de l'hote, d'un bloc et dans l'ordre : c'est ce qui
        permet a l'invite de s'aligner et de faire correspondre les canaux."""
        if channels is None:
            channels = self.plugin.channels()
        if channels is None:
            return
        self.signature_rack = [(i, n) for i, _t, n in channels]
        self.publish_global({"type": "channelcount", "n": len(channels)})
        if channels:
            self.publish_global({"type": "racklist",
                                 "channels": [[int(i), int(t), name or ""]
                                              for i, t, name in channels]})

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

    def publier_reglages(self):
        """Tempo, motif, transport et position : ce qui ne se rejoue pas."""
        tempo = self.plugin.tempo()
        if tempo:
            self.publish_global({"type": "tempo", "bpm": tempo})
        pattern = self.plugin.pattern()
        if pattern is not None:
            self.publish_global({"type": "pattern", "n": pattern})
        playing = self.plugin.playing()
        if playing is not None:
            self.publish_global({"type": "transport", "playing": playing})
        # sans la position, l'invite demarre au debut alors que l'hote est
        # ailleurs
        status = self.plugin.status()
        if status:
            self.publish_global({"type": "songpos", "pos": status["position"]})
            if status.get("mode", -1) >= 0:
                self.publish_global({"type": "songmode", "mode": status["mode"]})
        return tempo

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

        # Le rack part avant les notes : l'invite en a besoin pour savoir sur
        # quel canal les ecrire.
        self.publier_rack()

        total = 0
        for channel, notes in self.snapshot.items():
            if notes:
                self.publish({"type": "add", "ch": channel, "notes": notes})
                total += len(notes)

        tempo = self.publier_reglages()

        channels = self.plugin.channels()
        if channels:
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
            kind = str(message.get("type", ""))
            if kind.startswith("projet_"):
                if kind == "projet_debut":
                    # Ce qui precede est dans le fichier. Ce qui suit attend
                    # ici et sera rejoue une fois le projet charge.
                    self.incoming.clear()
                self.projet_file.append(message)
            else:
                self.incoming.append(message)

    def drain(self):
        with self.incoming_lock:
            pending, self.incoming = self.incoming, []
        return pending

    def apply_incoming(self, seulement=None):
        applied = 0
        messages = self.drain()
        if seulement is None:
            messages = self.notes_retenues + messages
            self.notes_retenues = []
            if self.clic:
                # L'utilisateur tient peut-etre une note : la reecrire sous sa
                # souris annulerait son geste. Ca attend le relachement.
                self.notes_retenues = [m for m in messages
                                       if m.get("type") in TOUCHENT_NOTES]
                messages = [m for m in messages
                            if m.get("type") not in TOUCHENT_NOTES]
        touche_notes = any(m.get("type") in TOUCHENT_NOTES for m in messages)
        if touche_notes and seulement is None:
            # Ce qui n'est pas encore parti doit partir avant : la relecture
            # de la fin l'effacerait sinon, sans que le pair le recoive jamais.
            self.envoyer_notes(self.plugin.state())

        for message in messages:
            kind = message.get("type")
            if seulement is not None and kind not in seulement:
                continue
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
                    # vient du pair : detect_local ne doit pas le lui renvoyer
                    self.tempo = float(bpm)
                    self.tempo_limite = time.monotonic() + DELAI_ECHO_TRANSPORT

            elif kind == "transport":
                want = bool(message.get("playing"))
                reply = self.plugin.cmd_force("PLAY" if want else "STOP")[0]
                if reply.startswith("ERR"):
                    print(f"[sync] transport refuse : {reply}")
                else:
                    print(f"[sync] transport applique : {'lecture' if want else 'arret'}")
                # ce changement vient du pair : detect_local ne doit pas le lui
                # renvoyer, ni la position que l'arret vient de remettre au debut
                self.playing = want
                self.transport_limite = time.monotonic() + DELAI_ECHO_TRANSPORT

            elif kind == "songmode":
                mode = int(message.get("mode", 0))
                reply = self.plugin.cmd_force(f"SONGMODE {mode}")[0]
                if reply.startswith("ERR"):
                    print(f"[sync] mode refuse : {reply}")
                else:
                    print(f"[sync] mode applique : {'SONG' if mode else 'PAT'}")
                # FL l'applique un peu apres : le STATUS suivant dit encore l'ancien
                self.mode = mode
                self.mode_limite = time.monotonic() + DELAI_ECHO_TRANSPORT

            elif kind == "songpos":
                position = int(message.get("pos", 0))
                if position >= 0:
                    self.plugin.cmd_force(f"SONGPOS {position}")
                    self.position = position

            elif kind == "channelcount":
                self.peer_channels = int(message.get("n", -1))
                print(f"[sync] l'autre a {self.peer_channels} canal(aux)")

            elif kind == "channelmix":
                index = self.index_local(int(message.get("index", -1)))
                if index is not None and index >= 0:
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

            elif kind == "racklist" and not self.is_host:
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
                manque = len(wanted) - len(local)
                # Pas forcement les derniers : Ctrl+Z ramene un canal au milieu.
                a_creer = canaux_absents(wanted, local)[-manque:] if manque > 0 else []
                for index, track, name in a_creer:
                    chemin = self.plugin.find_preset(name) if name else None
                    if chemin:
                        print(f"[rack] creation de '{name}' "
                              f"(position {index})")
                        self.plugin.load_plugin(chemin)
                        crees += 1
                    else:
                        # Sans l'instrument, on cree quand meme un canal
                        # vide : il recevra les notes de celui de l'hote.
                        try:
                            self.plugin.new_channel()
                            crees += 1
                            print(f"[rack] '{name}' absent de ce poste — "
                                  f"canal vide cree pour garder la position")
                        except (OSError, TimeoutError, IndexError):
                            print(f"[rack] '{name}' absent et canal vide "
                                  f"impossible : les positions suivantes "
                                  f"seront decalees")
                fresh = self.plugin.channels() or []
                self.construire_carte(wanted, fresh)
                tranches = {i: t for i, t, _ in fresh}
                for index, track, _name in wanted:
                    cible = self.index_local(index)
                    if cible in tranches and tranches[cible] != track:
                        self.plugin.set_channel_track(cible, track)
                        tranches[cible] = track
                self.channels_state = tranches
                self.signature_rack = [(i, n) for i, _t, n in fresh]
                if crees:
                    print(f"[rack] {crees} canal(aux) cree(s) — "
                          f"verifie l'ordre a l'ecran")
                applied += 1

            elif kind == "canal_debut":
                self.canal_recu = dict(message, recus=[])

            elif kind == "fx_debut":
                self.fx_recu = dict(message, recus=[])

            elif kind == "fx_morceau" and self.fx_recu:
                self.fx_recu["recus"].append(base64.b64decode(message.get("d", "")))
                if len(self.fx_recu["recus"]) >= int(self.fx_recu.get("morceaux", 0)):
                    recu, self.fx_recu = self.fx_recu, None
                    self.recevoir_fx(recu)
                    applied += 1

            elif kind == "mixcount":
                compte = int(message.get("n", 0))
                if 2 <= compte <= 126:
                    self.plugin.set_mix_count(compte)
                    self.compte_tranches = compte
                    self.oublier_listes_tranches()
                    applied += 1

            elif kind == "mixflag":
                t, nom = int(message.get("track", -1)), str(message.get("nom", ""))
                if 0 <= t <= 125 and nom in DRAPEAUX_TRANCHE:
                    valeur = int(message.get("v", 0))
                    self.plugin.track_flag(t, nom, valeur)
                    if self.mix_cache is not None:
                        self.mix_cache[("drapeau", t, nom)] = valeur
                    applied += 1

            elif kind == "mixrec":
                t, s = int(message.get("track", -1)), int(message.get("slot", -1))
                decalage = str(message.get("off", ""))
                if 0 <= t <= 125 and 0 <= s <= 9 and re.fullmatch("[0-9A-F]{4}", decalage):
                    valeur = int(message.get("v", 0))
                    # MIXFULL lit le bouton du slot en 0/1, FL l'ecrit sur 2^30
                    brut = (1 << 30 if valeur else 0) if decalage == "1F00" else valeur
                    self.plugin.rec_set(t, s, decalage, brut)
                    if self.mix_cache is not None:
                        self.mix_cache[("rec", t, s, decalage)] = valeur
                    applied += 1

            elif kind == "route":
                source, cible = int(message.get("source", -1)), int(message.get("cible", -1))
                if 0 <= source <= 125 and 0 <= cible <= 125:
                    niveau = int(message.get("niveau", -1))
                    self.plugin.route_set(source, cible, 1 if niveau >= 0 else 0, niveau)
                    if self.envois is not None:
                        if niveau >= 0:
                            self.envois[(source, cible)] = niveau
                        else:
                            self.envois.pop((source, cible), None)
                    applied += 1

            elif kind == "tranche_nom":
                t = int(message.get("track", -1))
                if 0 <= t <= 125:
                    couleur, nom = int(message.get("couleur", 0)), str(message.get("nom", ""))
                    if "\n" not in nom:
                        self.plugin.set_track_name(t, nom)
                        self.plugin.set_track_color(t, couleur)
                        if self.noms_tranches is not None:
                            self.noms_tranches[t] = (couleur, nom)
                    applied += 1

            elif kind == "canal_morceau" and self.canal_recu:
                self.canal_recu["recus"].append(base64.b64decode(message.get("d", "")))
                if len(self.canal_recu["recus"]) >= int(self.canal_recu.get("morceaux", 0)):
                    recu, self.canal_recu = self.canal_recu, None
                    self.recevoir_canal(recu)
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

            elif kind == "delchannels":
                applied += self.retirer_canaux(message.get("indices", []))

            elif kind == "fx":
                t = int(message.get("track", -1))
                s = int(message.get("slot", -1))
                nom = str(message.get("name", "")).strip()
                if t < 0 or s < 0:
                    pass
                elif not nom:
                    print(f"[fx] l'autre a retire l'effet de la tranche {t} slot {s}")
                    self.plugin.fx_load(t, s)
                    if self.fx is not None:
                        self.fx.pop((t, s), None)
                    self.oublier_slot(t, s)
                    applied += 1
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
                index = self.index_local(int(message.get("index", -1)))
                track = int(message.get("track", -1))
                if index is not None and index >= 0 and 0 <= track <= 125:
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
                    self.pattern = int(number)
                    self.pattern_limite = time.monotonic() + DELAI_ECHO_TRANSPORT
                    # compte comme un changement : la relecture de la fin evite
                    # de prendre les notes de l'autre motif pour des retraits
                    applied += 1

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
            if touche_notes:
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
        self.clic = bool(st and st.get("clic"))

        # Tempo et motif sont a tous, comme le transport : le dernier qui parle
        # gagne.
        current_tempo = st["tempo"] if st else None
        if current_tempo is not None:
            if self.tempo is None:
                self.tempo = current_tempo
            elif (abs(current_tempo - self.tempo) > 0.01
                  and time.monotonic() >= max(self.tempo_limite, self.tempo_prochain)):
                self.tempo_prochain = time.monotonic() + INTERVALLE_TEMPO
                self.publish_global({"type": "tempo", "bpm": current_tempo})
                print(f"[sync] tempo envoye : {current_tempo}")
                changed += 1
                self.tempo = current_tempo

        current_playing = st["playing"] if st else None
        if current_playing is not None:
            if self.playing is None:
                self.playing = current_playing
            elif (current_playing != self.playing
                  and time.monotonic() >= self.transport_limite):
                self.publish_global({"type": "transport", "playing": current_playing})
                print(f"[sync] transport envoye : {'lecture' if current_playing else 'arret'}")
                changed += 1
                self.playing = current_playing

        current_mode = st.get("mode", -1) if st else -1
        if current_mode >= 0:
            if self.mode is None:
                self.mode = current_mode
            elif current_mode != self.mode and time.monotonic() >= self.mode_limite:
                self.publish_global({"type": "songmode", "mode": current_mode})
                print(f"[sync] mode envoye : {'SONG' if current_mode else 'PAT'}")
                changed += 1
                self.mode = current_mode

        current_pattern = st["pattern"] if st else None
        if current_pattern is not None:
            if self.pattern is None:
                self.pattern = current_pattern
            elif (current_pattern != self.pattern
                  and time.monotonic() >= self.pattern_limite):
                self.publish_global({"type": "pattern", "n": current_pattern})
                print(f"[sync] pattern envoye : {current_pattern}")
                changed += 1
                self.pattern = current_pattern
                # Autre motif, autres notes : ce n'est pas une modification. Sans
                # ca, ses notes partiraient chez le pair avant qu'il ait suivi.
                self.snapshot = current

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

        # channel rack : assignation canal -> tranche (cadence lente). Des notes
        # qui changent peuvent venir d'un canal supprime : les index ont glisse,
        # et il faut le savoir AVANT de comparer les notes.
        if (now - self.last_channels >= 1.0
                or (not self.clic and current != self.snapshot)):
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
                    revenus = self.canaux_revenus(current_channels)
                    if revenus:
                        current = self.plugin.state()
                    for index, track, name in current_channels:
                        if (index not in revenus if revenus
                                else index in self.channels_state):
                            continue
                        print(f"[sync] canal ajoute : '{name}' (index {index})")
                        if self.publier_canal(index, name, revenu=index in revenus):
                            continue
                        if not name or name.strip().lower().startswith("sampler"):
                            continue
                        self.publish_global({"type": "addchannel",
                                             "name": name.strip(),
                                             "index": index})

                    # Meme place, autre nom : FL renomme le canal quand on y pose
                    # un sample ou qu'on y change de plugin.
                    if (self.signature_rack is not None
                            and len(self.signature_rack) == len(current_channels)):
                        for (_i, ancien), (index, _t, name) in zip(self.signature_rack,
                                                                   current_channels):
                            if ancien != name:
                                print(f"[sync] canal {index} : '{ancien}' -> '{name}'")
                                self.publier_canal(index, name, remplace=True)

                    if self.publier_retraits(current_channels):
                        current = self.plugin.state()

                    # Rack change : les index ont pu glisser, comparer les
                    # tranches index par index inventerait des changements.
                    if not self.suivre_rack(current_channels):
                        for index, track, name in current_channels:
                            old_track = self.channels_state.get(index)
                            cible = self.index_hote(index)
                            if (old_track is not None and old_track != track
                                    and cible is not None):
                                self.publish_global({"type": "channeltrack",
                                                     "index": cible, "track": track})
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
                            cible = self.index_hote(index)
                            if cible is None:
                                continue
                            self.publish_global({"type": "channelmix",
                                                 "index": cible,
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
                        self.oublier_slot(t, s)
                        if self.publier_fx(t, s, nom):
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
                        self.oublier_slot(t, s)
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

        if now - self.last_mixeur >= INTERVALLE_MIXEUR:
            self.last_mixeur = now
            self.suivre_mixeur()
        if now - self.last_routage >= INTERVALLE_ROUTAGE:
            self.last_routage = now
            self.suivre_routage()

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
                    vals = self.plugin.params(t, s, PARAMS_PAR_EFFET)
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
                    and abs(position - self.position) > 8
                    and time.monotonic() >= self.transport_limite):
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
            tranches = dict(st["mix"])
            tranches.update(self.mix_tard or {})
            self.mix_tard = None
            for track, snapshot in tranches.items():
                state = self.mix.setdefault(track, {})
                changes = {}
                # Pas d'ecart note pendant l'attente : la derniere valeur part au
                # tour suivant.
                if time.monotonic() >= self.potard_prochain.get(track, 0.0):
                    for key, tolerance in (("vol", 0.004), ("pan", 0.01),
                                           ("stereo", 0.01)):
                        value = snapshot[key]
                        old = state.get(key)
                        if old is None or abs(value - old) > tolerance:
                            state[key] = value
                            changes[key] = value
                    if changes:
                        self.potard_prochain[track] = time.monotonic() + INTERVALLE_POTARD
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

        # Note tenue : on n'envoie qu'au relachement, le geste en une seule fois.
        if not self.clic:
            changed += self.envoyer_notes(current)

        if changed:
            print(f"[sync] {changed} changement(s) local(aux) envoye(s)")
        return changed

    def envoyer_notes(self, current):
        """Envoie au pair l'ecart entre `current` et ce qu'il connait deja."""
        changed = 0
        for channel in set(list(current.keys()) + list(self.snapshot.keys())):
            # Un canal qui n'existe pas chez l'hote ne doit pas partir :
            # le plugin de l'autre poste le refuserait (et FL plantait avant
            # ce garde-fou).
            cible = self.index_hote(channel)
            if cible is None or (not self.is_host
                                 and self.nb_canaux_hote is not None
                                 and cible >= self.nb_canaux_hote):
                if not self.averti_canaux:
                    self.averti_canaux = True
                    print(f"[rack] ton canal {channel + 1} n'existe pas chez "
                          f"l'hote : ses notes ne seront pas envoyees")
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
            removed = diff_removals(old, new)
            if removed:
                self.publish({"type": "del", "ch": cible, "notes": removed})
                changed += len(removed)

            added = diff_additions(old, new)
            if added:
                self.publish({"type": "add", "ch": cible, "notes": added})
                changed += len(added)

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
        if not self.carte_local_vers_hote:
            return index_local     # pas encore de table : on garde l'index
        return self.carte_local_vers_hote.get(index_local)

    def publier_retraits(self, channels):
        """L'utilisateur a supprime des canaux : le pair les perd aussi.

        Vrai s'il y en a. FL a deja renumerote ses notes : la reference suit,
        sinon le glissement partirait au pair comme des notes deplacees.
        """
        if self.signature_rack is None:
            return False
        avant = [(n, self.channels_state.get(i)) for i, n in self.signature_rack]
        apres = [(n, t) for _i, t, n in channels]
        retires = canaux_retires(avant, apres)
        if not retires:
            return False
        cibles = []
        for position in retires:
            cible = self.index_hote(position)
            if cible is not None:
                cibles.append(cible)
            print(f"[rack] canal supprime : '{avant[position][0]}'"
                  + ("" if cible is not None else " (absent chez l'hote)"))
        if cibles:
            self.publish_global({"type": "delchannels", "indices": cibles})
        self.snapshot = decaler_notes(self.snapshot, retires)
        self.channel_mix_state = None
        return True

    def oublier_slot(self, track, slot):
        """L'effet du slot a change : ses reglages connus ne valent plus rien."""
        self.fx_params.pop((track, slot), None)
        if self.mix_cache is not None:
            for decalage in ("1F00", "1F01"):
                self.mix_cache.pop(("rec", track, slot, decalage), None)

    def oublier_listes_tranches(self):
        """Les colonnes ont change : routage et noms repartent d'une nouvelle
        reference, sinon les tranches apparues passeraient pour des modifications."""
        self.envois = None
        self.noms_tranches = None

    def suivre_mixeur(self):
        """Compare tout le mixeur au dernier etat connu et envoie les ecarts."""
        tranches = self.plugin.mix_full()
        if not isinstance(tranches, dict):
            return
        premiere = self.mix_cache is None
        if premiere:
            self.mix_cache = {}
        # Le + du mixeur : avant les valeurs, l'autre doit avoir les colonnes.
        compte = getattr(self.plugin, "nb_tranches", None)
        if isinstance(compte, int):
            if not premiere and compte != self.compte_tranches:
                self.publish_global({"type": "mixcount", "n": compte})
                print(f"[sync] mixeur : {compte - 2} tranches, envoye a l'autre")
                self.oublier_listes_tranches()
            self.compte_tranches = compte
        vus = {}
        for t, fiche in tranches.items():
            for nom, valeur in fiche["drapeaux"].items():
                vus[("drapeau", t, nom)] = valeur
            for decalage, valeur in fiche["eq"].items():
                vus[("rec", t, 0, decalage)] = valeur
            for s, (mute, niveau) in fiche["slots"].items():
                vus[("rec", t, s, "1F00")] = mute
                vus[("rec", t, s, "1F01")] = niveau
        for cle, valeur in vus.items():
            connu = self.mix_cache.get(cle)
            self.mix_cache[cle] = valeur
            if premiere or connu is None:
                continue
            if cle[0] == "drapeau":
                if valeur != connu:
                    self.publish({"type": "mixflag", "track": cle[1], "nom": cle[2], "v": valeur})
            elif abs(valeur - connu) > (TOLERANCE_REC if cle[3] != "1F00" else 0):
                self.publish({"type": "mixrec", "track": cle[1], "slot": cle[2],
                              "off": cle[3], "v": valeur})
        # Les tranches suivantes rejoignent le volume, le pan et le mute de STATUS.
        tard = {t: f for t, f in tranches.items() if t > TRANCHES_RAPIDES}
        # Une tranche qu'on decouvre (projet deja transmis, ou colonne ajoutee)
        # sert de reference : l'autre a la meme.
        for t, fiche in tard.items():
            if t not in self.mix:
                self.mix[t] = {cle: fiche[cle] for cle in ("vol", "pan", "stereo", "mute")}
        if not premiere:
            self.mix_tard = tard

    def suivre_routage(self):
        """Envois entre tranches, noms et couleurs : ca bouge peu."""
        envois = self.plugin.routes()
        if isinstance(envois, dict):
            if self.envois is not None:
                for cle in set(envois) | set(self.envois):
                    niveau = envois.get(cle, -1)
                    if self.envois.get(cle, -1) != niveau:
                        self.publish({"type": "route", "source": cle[0], "cible": cle[1],
                                      "niveau": niveau})
            self.envois = envois
        noms = self.plugin.track_names()
        if isinstance(noms, dict):
            if self.noms_tranches is not None:
                for t, (couleur, nom) in noms.items():
                    if self.noms_tranches.get(t) != (couleur, nom):
                        self.publish({"type": "tranche_nom", "track": t, "couleur": couleur,
                                      "nom": nom})
            self.noms_tranches = noms

    def publier_fx(self, track, slot, nom):
        """Envoie l'effet avec ses reglages. Faux si ca n'a pas pu partir."""
        chemin = os.path.join(sous_dossier_local("canaux"), f"effet-{os.getpid()}.fst")
        brut = None
        try:
            if self.plugin.fx_save(track, slot, chemin).startswith("FXSAVE OK"):
                with open(chemin, "rb") as fichier:
                    brut = fichier.read()
        except (OSError, TimeoutError, IndexError, AttributeError) as exc:
            print(f"[fx] sauvegarde impossible : {exc}")
        try:
            os.remove(chemin)
        except OSError:
            pass
        if not brut or len(brut) > TAILLE_CANAL_MAX:
            return False
        paquet = zlib.compress(brut, 6)
        nombre = max(1, (len(paquet) + MORCEAU_PROJET - 1) // MORCEAU_PROJET)
        self.publish_global({"type": "fx_debut", "track": track, "slot": slot, "nom": nom,
                             "sha256": hashlib.sha256(brut).hexdigest(), "morceaux": nombre})
        for i in range(nombre):
            morceau = paquet[i * MORCEAU_PROJET:(i + 1) * MORCEAU_PROJET]
            self.publish_global({"type": "fx_morceau", "i": i,
                                 "d": base64.b64encode(morceau).decode("ascii")})
        print(f"[fx] '{nom}' envoye avec ses reglages : {len(brut)} octets")
        return True

    def recevoir_fx(self, recu):
        """Le pair a pose un effet : on en fait une copie dans le meme slot."""
        t, s, nom = int(recu.get("track", -1)), int(recu.get("slot", -1)), str(recu.get("nom", ""))
        if not (0 <= t <= 125 and 0 <= s <= 9):
            return
        chemin = os.path.join(sous_dossier_local("canaux"), f"effet-recu-{os.getpid()}.fst")
        try:
            brut = zlib.decompress(b"".join(recu["recus"]))
            if hashlib.sha256(brut).hexdigest() != recu.get("sha256"):
                print(f"[fx] '{nom}' recu abime : ignore")
                return
            requis = plugin_requis(brut)
            if requis and not (self.plugin_present(*requis)
                                or self.plugin.find_effect(requis[0])):
                print(f"[fx] effet '{requis[0]}' absent de ce poste : tranche {t} slot {s} vide")
                return
            with open(chemin, "wb") as fichier:
                fichier.write(brut)
            reponse = self.plugin.fx_load(t, s, chemin)
        except (OSError, ValueError, IndexError, zlib.error) as exc:
            print(f"[fx] '{nom}' illisible : {exc}")
            return
        finally:
            try:
                os.remove(chemin)
            except OSError:
                pass
        if not reponse.startswith("FXLOAD OK"):
            print(f"[fx] '{nom}' refuse : {reponse}")
            return
        print(f"[fx] tranche {t} slot {s} : '{nom}' pose")
        self.oublier_slot(t, s)
        if self.fx is not None:
            self.fx[(t, s)] = reponse[len("FXLOAD OK "):]

    def publier_canal(self, index, nom, remplace=False, revenu=False):
        """Envoie le canal entier, sample compris : le pair en a une copie
        exacte. Faux si ca n'a pas pu partir."""
        chemin = os.path.join(sous_dossier_local("canaux"), f"envoi-{os.getpid()}.fst")
        brut = None
        try:
            reponse = self.plugin.save_channel(index, chemin)
            if reponse.startswith("SAVECHANNEL OK"):
                with open(chemin, "rb") as fichier:
                    brut = fichier.read()
            else:
                print(f"[canal] sauvegarde refusee : {reponse}")
        except (OSError, TimeoutError, IndexError) as exc:
            print(f"[canal] sauvegarde impossible : {exc}")
        try:
            os.remove(chemin)
        except OSError:
            pass
        if brut is None:
            return False
        if len(brut) > TAILLE_CANAL_MAX:
            print(f"[canal] '{nom}' trop lourd ({len(brut)} octets) : pas envoye")
            return False

        son = b""
        original = None
        try:
            original = chemin_sample(brut)
            source = fichier_sample(original)
            if source is not None:
                if os.path.getsize(source) <= TAILLE_SAMPLE_MAX:
                    with open(source, "rb") as fichier:
                        son = fichier.read()
                else:
                    print(f"[canal] sample trop lourd, l'autre ne l'aura pas : {source}")
            elif original and not original.startswith(VARIABLE_USINE):
                print(f"[canal] sample introuvable, l'autre ne l'aura pas : {original}")
        except (ValueError, IndexError, OSError) as exc:
            print(f"[canal] sample illisible : {exc}")

        contenu = brut + son
        paquet = zlib.compress(contenu, 1 if son else 6)
        nombre = max(1, (len(paquet) + MORCEAU_PROJET - 1) // MORCEAU_PROJET)
        cible = self.index_hote(index)
        self.publish_global({"type": "canal_debut",
                             "index": index if cible is None else cible,
                             "nom": nom or "", "taille": len(brut),
                             "chemin": original or "", "remplace": remplace,
                             "revenu": revenu,
                             "sha256": hashlib.sha256(contenu).hexdigest(),
                             "morceaux": nombre})
        for i in range(nombre):
            morceau = paquet[i * MORCEAU_PROJET:(i + 1) * MORCEAU_PROJET]
            self.publish_global({"type": "canal_morceau", "i": i,
                                 "d": base64.b64encode(morceau).decode("ascii")})
        print(f"[canal] '{nom}' envoye : {len(brut)} octets"
              + (f" + sample de {len(son)} octets" if son else ""))
        return True

    def plugin_present(self, nom, chemin):
        """Faux si FL ne trouvera pas ce plugin. Sans dossier de plugins lisible,
        on ne peut rien dire pour un plugin de FL : on le laisse tenter."""
        if chemin and os.path.exists(chemin):
            return True
        if self.plugin.find_preset(nom):
            return True
        # "Serum 2.fst" pour un plugin qui se nomme "Serum2"
        compact = nom.replace(" ", "").lower()
        table = self.plugin.preset_paths()
        if any(cle.replace(" ", "") == compact for cle in table):
            return True
        return not chemin and not table

    def poser_sample(self, brut, son, recu):
        """Range le sample recu et fait pointer le .fst dessus. Celui qu'on a
        deja au meme endroit reste ou il est."""
        original = str(recu.get("chemin", ""))
        local = fichier_sample(original)
        if local is not None and os.path.getsize(local) == len(son):
            return brut
        nom = re.sub(r"[^\w .()-]", "_", original.replace("/", "\\").rsplit("\\", 1)[-1])
        cible = os.path.join(sous_dossier_local("samples"),
                             f"{str(recu.get('sha256', ''))[:8]}-{nom}")
        if not os.path.isfile(cible):
            with open(cible, "wb") as fichier:
                fichier.write(son)
        return remplacer_sample(brut, cible)

    def recevoir_canal(self, recu):
        """Le pair a envoye un canal entier : on en fait une copie ici."""
        nom = str(recu.get("nom", ""))
        if recu.get("revenu") and any(n == nom for _i, _t, n in self.plugin.channels() or []):
            # Ctrl+Z chez l'autre : tant que la suppression n'est pas suivie,
            # le canal n'a jamais quitte ce poste.
            return
        try:
            contenu = zlib.decompress(b"".join(recu["recus"]))
            if hashlib.sha256(contenu).hexdigest() != recu.get("sha256"):
                print(f"[canal] '{nom}' recu abime : ignore")
                return
            taille = int(recu.get("taille", 0))
            brut, son = contenu[:taille], contenu[taille:]
            if son:
                brut = self.poser_sample(brut, son, recu)
            sample = chemin_sample(brut)
            if (sample and not sample.startswith(VARIABLE_USINE)
                    and fichier_sample(sample) is None):
                print(f"[canal] sample absent de ce poste : {sample}")
                brut = modifier_evenements(brut, {EVT_SAMPLE: None})
            requis = plugin_requis(brut)
            if requis and not self.plugin_present(*requis):
                print(f"[canal] plugin '{requis[0]}' absent de ce poste : canal vide a la place")
                brut = sans_plugin(brut)
            chemin = os.path.join(sous_dossier_local("canaux"), f"recu-{os.getpid()}.fst")
            with open(chemin, "wb") as fichier:
                fichier.write(brut)
        except (OSError, ValueError, IndexError, zlib.error) as exc:
            print(f"[canal] '{nom}' illisible : {exc}")
            return

        index = int(recu.get("index", 0))
        try:
            if recu.get("remplace"):
                cible = index if self.is_host else self.index_local(index)
                if cible is None:
                    print(f"[canal] '{nom}' n'a pas de correspondant ici")
                    return
                reponse = self.plugin.replace_channel(cible, chemin, nom)
            else:
                local = self.plugin.channels() or []
                reponse = self.plugin.load_channel(min(index, len(local)), chemin, nom)
        except (OSError, TimeoutError, IndexError) as exc:
            reponse = str(exc)
        finally:
            try:
                os.remove(chemin)
            except OSError:
                pass
        if " OK " not in reponse:
            print(f"[canal] '{nom}' refuse : {reponse}")
            return
        print(f"[canal] '{nom}' recu")

        fresh = self.plugin.channels()
        if fresh is None:
            return
        self.channels_state = {i: t for i, t, _ in fresh}
        self.signature_rack = [(i, n) for i, _t, n in fresh]
        self.channel_mix_state = None
        if self.is_host:
            self.publier_rack(fresh)
        elif self.rack_hote:
            self.construire_carte(self.rack_hote, fresh)

    def canaux_revenus(self, channels):
        """Positions des canaux reapparus au milieu du rack (Ctrl+Z dans FL).

        Les suivants ont glisse : comme pour une suppression, la reference suit,
        sinon le glissement partirait au pair comme des notes deplacees. Un ajout
        en fin de rack ne deplace rien et ne passe pas ici.
        """
        if self.signature_rack is None:
            return []
        avant = [(n, self.channels_state.get(i)) for i, n in self.signature_rack]
        apres = [(n, t) for _i, t, n in channels]
        revenus = canaux_retires(apres, avant) or []
        if not revenus or revenus[0] >= len(avant):
            return []
        self.snapshot = inserer_notes(self.snapshot, revenus)
        self.channel_mix_state = None
        return revenus

    def retirer_canaux(self, indices):
        """Le pair a supprime ces canaux (numerotation de l'hote)."""
        if not SUPPRESSION_RECUE:
            for index in indices:
                nom = self.nom_hote(int(index)) or f"numero {int(index) + 1}"
                print(f"[rack] l'autre a supprime le canal '{nom}' : "
                      f"supprime-le a la main si tu veux le suivre")
            return 0
        if not self.is_host and not self.carte_hote_vers_local:
            # sans table, l'index pourrait viser un autre instrument
            print("[rack] suppression ignoree : racks sans nom commun")
            return 0
        locaux = {self.index_local(int(i)) for i in indices} - {None}
        retires = 0
        # du dernier au premier : les suivants ne glissent pas en route
        for index in sorted(locaux, reverse=True):
            reponse = self.plugin.delete_channel(index)
            if reponse.startswith("DELCHANNEL OK"):
                retires += 1
                print(f"[rack] canal {index} supprime comme chez l'autre")
            else:
                print(f"[rack] suppression refusee : {reponse}")
        fresh = self.plugin.channels()
        if fresh is not None:
            self.channels_state = {i: t for i, t, _ in fresh}
            self.signature_rack = [(i, n) for i, _t, n in fresh]
            if self.is_host:
                self.publier_rack(fresh)
        self.channel_mix_state = None
        return retires

    def suivre_rack(self, channels):
        """Le rack a change en pleine session : on refait la correspondance.

        L'hote renvoie son rack ; l'invite recalcule sa table. Les index
        n'ont de sens que tant que la table est a jour. Vrai si le rack a
        change.
        """
        signature = [(i, n) for i, _t, n in channels]
        if signature == self.signature_rack:
            return False
        premiere = self.signature_rack is None
        self.signature_rack = signature
        if premiere:
            return False
        if self.is_host:
            print("[rack] le rack a change : envoi a l'invite")
            self.publier_rack(channels)
        elif self.rack_hote:
            self.construire_carte(self.rack_hote, channels)
        return True

    def construire_carte(self, wanted, local=None):
        """Table de correspondance par nom entre le rack de l'hote et le mien.

        Deux canaux peuvent porter le meme nom : le 2e "Sampler" de l'hote
        va sur mon 2e "Sampler", dans l'ordre du rack. Ce qui reste est
        rapproche avec meme_nom, car FL renomme parfois les canaux.
        """
        if self.is_host:
            return
        if local is None:
            local = self.plugin.channels() or []
        par_nom = {}
        for i, _t, n in local:
            cle = (n or "").strip().lower()
            if cle:
                par_nom.setdefault(cle, []).append(i)

        carte = {}
        restants = []
        for index, _t, name in wanted:
            cle = (name or "").strip().lower()
            if par_nom.get(cle):
                carte[index] = par_nom[cle].pop(0)
            else:
                restants.append((index, name))

        noms = {i: n for i, _t, n in local}
        libres = [(i, noms[i]) for indices in par_nom.values() for i in indices]
        libres += [(i, n) for i, _t, n in local if not (n or "").strip()]
        libres.sort()
        for index, name in restants:
            for libre in libres:
                if meme_nom(libre[1], name):
                    carte[index] = libre[0]
                    libres.remove(libre)
                    break

        # Un "Sampler" tient la place d'un canal que l'autre poste n'a pas pu
        # creer. Il recoit les notes : mieux vaut les voir que les perdre.
        for index, name in restants:
            if index in carte:
                continue
            for libre in libres:
                if nom_generique(name) or nom_generique(libre[1]):
                    carte[index] = libre[0]
                    libres.remove(libre)
                    break

        changee = carte != self.carte_hote_vers_local
        self.carte_hote_vers_local = carte
        self.carte_local_vers_hote = {l: h for h, l in carte.items()}
        self.averti_canaux = False
        self.averti_noms = set()
        if not changee:
            return
        if carte:
            print(f"[rack] correspondance par nom : "
                  f"{len(carte)} canal(aux) reconnu(s)")
        else:
            print("[rack] aucun nom commun : les notes suivront les index")

    def proteger(self, action, message):
        """Une erreur de traitement ne coupe pas la session, un plugin perdu
        si : il faut qu'il remonte jusqu'a run(), sinon on tourne dans le vide."""
        try:
            action()
        except TimeoutError:
            # FL est occupe. Sa reponse arrivera en retard et decalerait toutes
            # les suivantes : on repart sur une connexion neuve.
            print("[sync] FL ne repond pas a temps : reconnexion au plugin")
            self.reprendre_plugin()
        except (ConnectionError, OSError):
            raise
        except Exception as exc:
            print(f"{message} : {exc}")

    def reprendre_plugin(self):
        self.plugin.reconnecter()
        self.annoncer()

    def annoncer(self):
        """La fenetre oublie la session des que l'agent se deconnecte."""
        self.role_sent = False
        for texte in self.annonces:
            gui_etat(self.plugin, texte)

    def statut_session(self):
        if self.annonces:
            gui_etat(self.plugin, self.annonces[-1])

    # -- Echange du projet ---------------------------------------------------
    # L'hote fait foi : a la connexion, l'invite charge son .flp, et FL fait
    # tout le travail (motifs, rack, instruments, mixage, playlist). Refuser
    # revient a quitter la session : la synchro effacerait ses motifs.
    #
    #   hote                                invite
    #   projet_offre            ->
    #                                       question dans la fenetre
    #                                       sauvegarde de son projet
    #                           <-          projet_demande
    #   SAVEPROJECT
    #   projet_debut, morceaux  ->          ecrit, verifie, LOADPROJECT
    #   (synchro normale)       ->          garde tout ca de cote
    #                                       le plugin revient
    #                           <-          projet_pret
    #   reglages                ->          rejoue ce qu'il a garde
    #
    # projet_refuse : l'hote n'a pas pu produire le fichier, l'invite quitte la
    # session.

    def projet_gele(self):
        return not self.is_host and self.projet_etat is not None

    def avancer_projet(self):
        with self.incoming_lock:
            recus, self.projet_file = self.projet_file, []
        if self.is_host:
            self.projet_cote_hote(recus)
        else:
            self.projet_cote_invite(recus)

    def projet_cote_hote(self, recus):
        if self.projet_etat == "a_offrir":
            # l'inventaire d'abord : l'invite voit ce qui lui manque avant de
            # repondre
            self.send_plugins()
            self.publish_global({"type": "projet_offre",
                                 "poste": socket.gethostname().upper()})
            self.projet_etat = "offert"
            gui_etat(self.plugin, f"En attente de la réponse de {self.poste_pair}…")

        for message in recus:
            kind = message.get("type")
            if kind == "projet_demande" and self.projet_etat == "offert":
                self.envoyer_projet()
            elif kind == "projet_pret":
                print(f"[projet] {self.poste_pair} a charge le projet")
                self.projet_etat = None
                self.sent_initial = True
                self.publier_rack()
                self.publier_reglages()
                self.statut_session()

    def envoyer_projet(self):
        gui_etat(self.plugin, f"Envoi du projet à {self.poste_pair}…")
        chemin = os.path.join(sous_dossier_local("projets"),
                              f"envoi-{os.getpid()}.flp")
        brut = None
        try:
            raison = self.plugin.sauver_projet(chemin)
        except TimeoutError:
            raison = "FL n'a pas repondu"
            self.reprendre_plugin()
        if raison is None:
            try:
                with open(chemin, "rb") as fichier:
                    brut = fichier.read()
            except OSError as exc:
                raison = str(exc)
        try:
            os.remove(chemin)
        except OSError:
            pass
        if raison is None and len(brut) > TAILLE_PROJET_MAX:
            raison = f"projet trop lourd ({len(brut)} octets)"
        if raison is not None:
            self.refuser_projet(raison)
            return

        paquet = zlib.compress(brut, 6)
        nombre = max(1, (len(paquet) + MORCEAU_PROJET - 1) // MORCEAU_PROJET)
        print(f"[projet] envoi : {len(brut)} octets, {len(paquet)} compresses, "
              f"{nombre} morceau(x)")
        self.publish_global({"type": "projet_debut", "taille": len(brut),
                             "sha256": hashlib.sha256(brut).hexdigest(),
                             "morceaux": nombre})
        for i in range(nombre):
            morceau = paquet[i * MORCEAU_PROJET:(i + 1) * MORCEAU_PROJET]
            self.publish_global({"type": "projet_morceau", "i": i,
                                 "d": base64.b64encode(morceau).decode("ascii")})
        self.projet_etat = "envoye"

    def refuser_projet(self, raison):
        """L'invite s'en va : son depart rend la main a l'hote, qui attend le
        suivant."""
        print(f"[projet] pas d'envoi : {raison}")
        self.projet_etat = "refuse"
        self.publish_global({"type": "projet_refuse", "raison": raison})
        self.statut_session()

    def projet_cote_invite(self, recus):
        for message in recus:
            kind = message.get("type")
            if kind == "projet_offre":
                self.poste_pair = message.get("poste", self.poste_pair)
                if self.accepter_projet:
                    self.accepter()
                else:
                    self.projet_etat = "choix"
                    self.projet_limite = time.monotonic() + DELAI_CHOIX
                    gui_etat(self.plugin, f"DEMANDE {self.poste_pair}")
            elif kind == "projet_refuse":
                # son projet a ete sauvegarde, mais il n'a pas encore ete touche
                self.quitter_session(f"L'hôte n'a pas pu envoyer son projet : "
                                     f"{message.get('raison')}")
            elif kind == "projet_debut":
                self.projet_attendu = message
                self.projet_morceaux = {}
                self.projet_pourcent = 0
            elif kind == "projet_morceau" and self.projet_attendu:
                self.projet_morceaux[int(message.get("i", -1))] = message.get("d", "")
                nombre = int(self.projet_attendu.get("morceaux", 0))
                pourcent = 100 * len(self.projet_morceaux) // max(1, nombre)
                if pourcent >= self.projet_pourcent + 10:
                    self.projet_pourcent = pourcent
                    gui_etat(self.plugin, f"Réception du projet : {pourcent} %")
                if len(self.projet_morceaux) >= nombre:
                    self.charger_projet_hote()
                    return

        if self.projet_etat == "choix":
            self.lire_choix()

    def lire_choix(self):
        maintenant = time.monotonic()
        if maintenant - self.projet_lecture < 0.2:
            return
        self.projet_lecture = maintenant

        reponse = self.plugin.cmd("CHOIX")[0]
        if not reponse.startswith("CHOIX "):
            self.quitter_session("Plugin trop ancien : mets FruityMulti à jour")
        choix = reponse[6:].strip()
        if choix == "oui":
            self.accepter()
        elif choix == "non":
            self.quitter_session("Session quittée : projet de l'hôte refusé")
        elif maintenant > self.projet_limite:
            gui_etat(self.plugin, "DEMANDE -")
            self.quitter_session("Session quittée : pas de réponse")

    def accepter(self):
        """La sauvegarde passe avant tout le reste : a partir d'ici, le projet
        de l'invite va etre remplace."""
        gui_etat(self.plugin, "Sauvegarde de ton projet…")
        dossier = sous_dossier_local("sauvegardes")
        chemin = os.path.join(dossier, time.strftime("avant-session-%Y%m%d-%H%M%S.flp"))
        try:
            raison = self.plugin.sauver_projet(chemin)
        except TimeoutError:
            raison = "FL n'a pas repondu"
        if raison is not None:
            self.quitter_session(f"Sauvegarde de ton projet impossible : {raison}")
        garder_recentes(dossier, SAUVEGARDES_GARDEES)
        print(f"[projet] ton projet est sauvegarde : {chemin}")

        self.projet_etat = "reception"
        gui_etat(self.plugin, f"Réception du projet de {self.poste_pair}…")
        self.publish_global({"type": "projet_demande"})

    def lire_fichier_recu(self):
        """Le .flp recu, ou None s'il n'est pas intact."""
        attendu = self.projet_attendu
        try:
            paquet = b"".join(base64.b64decode(self.projet_morceaux[i])
                              for i in range(int(attendu["morceaux"])))
            brut = zlib.decompress(paquet)
        except (KeyError, ValueError, zlib.error) as exc:
            print(f"[projet] fichier recu illisible : {exc}")
            return None
        if hashlib.sha256(brut).hexdigest() != attendu.get("sha256"):
            print("[projet] empreinte fausse : fichier abime en route")
            return None
        # Un fichier refuse par FL laisserait le plugin arrete pour de bon :
        # LOADPROJECT coupe tous ses threads avant de lancer le chargement.
        if brut[:4] != b"FLhd":
            print("[projet] ce n'est pas un projet FL")
            return None
        return brut

    def charger_projet_hote(self):
        brut = self.lire_fichier_recu()
        self.projet_morceaux = {}
        if brut is None:
            self.quitter_session("Projet de l'hôte reçu abîmé")

        nom = f"session-{self.code or 'directe'}.flp"
        chemin = os.path.join(sous_dossier_local("projets"), nom)
        try:
            with open(chemin, "wb") as fichier:
                fichier.write(brut)
        except OSError as exc:
            self.quitter_session(f"Écriture du projet impossible : {exc}")

        print(f"[projet] chargement de {chemin}")
        gui_etat(self.plugin, "Chargement du projet…")
        reponse = self.plugin.cmd(f"LOADPROJECT {chemin}")[0]
        if not reponse.startswith("LOADPROJECT planifie"):
            self.quitter_session(f"Chargement refusé : {reponse}")

        self.projet_etat = "chargement"
        if not self.plugin.attendre_fermeture(DELAI_COUPURE):
            self.quitter_session("FL n'a pas lancé le chargement")
        self.attendre_retour_plugin()
        self.verifier_chargement()
        self.attendre_stabilite()

        with self.incoming_lock:
            self.incoming = [m for m in self.incoming if m.get("type") != "mouse"]
        self.lire_projet()
        self.annoncer()
        self.projet_etat = None
        self.sent_initial = True
        self.publish_global({"type": "projet_pret"})
        print(f"[projet] projet de {self.poste_pair} charge")

    def verifier_chargement(self):
        """Le plugin revient aussi quand FL a refuse le fichier : son retour ne
        prouve rien. -1 : le chargeur n'a pas encore rendu la main. Un plugin
        sans LOADSTATUS ne peut pas le dire."""
        limite = time.monotonic() + DELAI_RETOUR_PLUGIN
        while True:
            reponse = self.plugin.cmd("LOADSTATUS")[0]
            code = reponse[11:].strip() if reponse.startswith("LOADSTATUS ") else "0"
            if code != "-1" or time.monotonic() > limite:
                break
            time.sleep(0.5)
        if code != "0":
            self.quitter_session(f"FL a refusé le projet (code {code})")

    def attendre_retour_plugin(self):
        """Le Bridge est dans le projet de l'hote, donc il revient tout seul.
        S'il ne revient pas, on retombe sur le cas du plugin perdu."""
        limite = time.monotonic() + DELAI_RETOUR_PLUGIN
        while time.monotonic() < limite:
            time.sleep(1.0)
            try:
                self.plugin.reconnecter()
                if self.plugin.ping():
                    return
            except OSError:
                pass
        raise ConnectionError("le plugin n'est pas revenu apres le chargement")

    def attendre_stabilite(self):
        """FL peut repondre alors qu'il n'a pas fini : une lecture a moitie
        faite partirait a l'hote comme des modifications de l'invite."""
        limite = time.monotonic() + DELAI_STABILITE
        precedente = None
        while time.monotonic() < limite:
            vue = (self.plugin.channels(), self.plugin.state())
            if vue == precedente:
                return
            precedente = vue
            time.sleep(1.0)

    def quitter_session(self, raison):
        """Sortie de l'agent : la fenetre garde la raison affichee."""
        print(f"[projet] {raison}")
        self._liberer_autorite()
        gui_etat(self.plugin, f"ERREUR {raison}")
        raise SystemExit(1)

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

                if self.projet_gele():
                    # On ne touche a rien avant de savoir si le projet sera
                    # remplace ; le comparatif des plugins aide a decider.
                    if self.projet_etat in ("attente", "choix"):
                        self.proteger(lambda: self.apply_incoming(("plugins",)),
                                      "[sync] message ignore")
                else:
                    # Un message malforme ne doit jamais couper la session : on
                    # le signale et on continue. Sans cela, la moindre erreur
                    # dans un gestionnaire tuait la connexion et provoquait une
                    # boucle de reconnexions.
                    self.proteger(self.apply_incoming, "[sync] message ignore")
                    self.proteger(self.detect_local,
                                  "[sync] analyse locale impossible")

                if self.projet_etat is not None:
                    self.avancer_projet()
                if not self.sent_initial and self.projet_etat is None:
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
RELAIS = os.environ.get("FM_RELAIS", "wss://fruitymulti-relais.fruitymulti-relais.workers.dev")
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
           "role": "hote" if est_hote else "invite", "projet": 1,
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
        # sans Accept, GitHub peut servir une redirection perimee
        # (l'ancienne version, longtemps apres la publication)
        headers={"User-Agent": "FruityMulti", "Accept": "*/*"})
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
    # le journal de l'interface reste vide. Et un nom de sample en caracteres
    # exotiques ne doit jamais faire tomber un print.
    try:
        sys.stdout.reconfigure(line_buffering=True, errors="replace")
        sys.stderr.reconfigure(line_buffering=True, errors="replace")
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
    parser.add_argument("--accepter-projet", action="store_true",
                        help="invite sans fenetre : charger le projet de l'hote "
                             "sans poser la question")
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
    plugin.veiller()

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
        plugin.veiller()
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
        role_affiche = "hôte" if est_hote else "invité"
        annonces = ["ROLE " + ("hote" if est_hote else "invite"),
                    "ADRESSE " + (adresse if est_hote
                                  else (args.rejoindre or args.peer).strip()),
                    f"PAIR {poste}", f"Connecté à {poste} ({role_affiche})"]
        plugin.fin_veille()
        link = None
        try:
            # Sync d'abord : un message recu avant qu'elle existe serait perdu,
            # et c'est le premier qui compte (l'offre du projet).
            sync = Sync(plugin, None, est_hote, pair=pair, annonces=annonces,
                        code=code, accepter_projet=args.accepter_projet)
            link = Link(transport, sync.on_message)
            sync.link = link
            sync.run()
        except KeyboardInterrupt:
            raise
        except (PluginLost, OSError) as exc:
            # FL a ete ferme, ou l'effet a ete retire : on attend son retour
            # au lieu de demander a l'utilisateur de relancer l'agent.
            print(f"[sync] FL a disparu ({exc})")
            print("[sync] on attend le retour de l'effet FruityMulti Bridge...")
            plugin.close()
            if link is not None:
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

        if link is not None:
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
