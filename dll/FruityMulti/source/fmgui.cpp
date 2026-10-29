// FruityMulti - la fenêtre du plugin.
//
// Trois choses, et rien de plus : héberger une session, en rejoindre une, et
// vérifier les mises à jour. Le reste (l'état, l'adresse, le journal) tient sur
// une seule page.
//
// Tout est dessiné à la main en GDI : pas de dépendance, pas de toolkit, et ça
// ressemble à un vrai plugin.

#include "fmgui.h"

// winsock2 doit venir avant windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <atomic>
#include <thread>
#include <string>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

using namespace Steinberg;

// --------------------------------------------------------------------------
// Etat partagé (écrit par le fil réseau du plugin, lu par la fenêtre)
// --------------------------------------------------------------------------
namespace {

std::mutex g_mtx;
std::string g_statut = "Aucune session";
std::string g_adresse;
std::string g_maj = "";
int  g_role = 0;
bool g_agent = false;

HWND g_panel = nullptr;      // notre fenêtre
HANDLE g_agentProc = nullptr; // agent lance depuis la fenetre
DWORD  g_agentPid = 0;
HWND g_edit = nullptr;       // champ adresse
bool g_hover[5] = { false, false, false, false, false };
int  g_majDispo = 0;   // numero de la version proposee, 0 = aucune
std::atomic<bool> g_checkEnCours (false);
std::atomic<int>  g_checkResultat (-1);   // -1 = pas encore repondu

const char* kClasse = "FruityMultiPanel";

// Couleurs (thème sombre, accent orange FL)
const COLORREF C_FOND      = RGB (0x17, 0x18, 0x1C);
const COLORREF C_CARTE     = RGB (0x20, 0x21, 0x27);
const COLORREF C_BORD      = RGB (0x2E, 0x30, 0x38);
const COLORREF C_TEXTE     = RGB (0xE9, 0xEA, 0xEE);
const COLORREF C_DIM       = RGB (0x8A, 0x8D, 0x97);
const COLORREF C_ACCENT    = RGB (0xFF, 0x7A, 0x18);
const COLORREF C_ACCENT2   = RGB (0xD9, 0x62, 0x0E);
const COLORREF C_VERT      = RGB (0x3D, 0xDC, 0x84);
const COLORREF C_ROUGE     = RGB (0xE5, 0x5B, 0x5B);

const int L = 22;            // marge
const int W = 520;
const int H = 352;

// Zones cliquables
RECT g_btnHeberger, g_btnRejoindre, g_btnCopier, g_btnMaj, g_btnQuitter;

// --------------------------------------------------------------------------
// Petits utilitaires
// --------------------------------------------------------------------------
std::string cheminAgent ()
{
	char base[MAX_PATH] = { 0 };
	const DWORD n = GetEnvironmentVariableA ("LOCALAPPDATA", base, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
	{
		std::string p (base);
		p += "\\FruityMulti\\fm_agent.exe";
		if (GetFileAttributesA (p.c_str ()) != INVALID_FILE_ATTRIBUTES)
			return p;
	}

	// repli : à côté de la DLL du plugin
	HMODULE self = nullptr;
	GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    (LPCSTR)&cheminAgent, &self);
	char dll[MAX_PATH] = { 0 };
	GetModuleFileNameA (self, dll, MAX_PATH);
	std::string p (dll);
	const size_t fin = p.find_last_of ("\\/");
	if (fin != std::string::npos)
		p = p.substr (0, fin + 1) + "fm_agent.exe";
	if (GetFileAttributesA (p.c_str ()) != INVALID_FILE_ATTRIBUTES)
		return p;
	return std::string ();
}

// Lance l'agent. `console` = fenêtre noire visible (pour voir les messages).
bool lancerAgent (const std::string& arguments, bool console)
{
	const std::string exe = cheminAgent ();
	if (exe.empty ())
		return false;

	std::string ligne = "\"" + exe + "\" " + arguments;
	std::vector<char> buf (ligne.begin (), ligne.end ());
	buf.push_back (0);

	STARTUPINFOA si;
	memset (&si, 0, sizeof (si));
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi;
	memset (&pi, 0, sizeof (pi));

	const DWORD drapeaux = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
	if (!CreateProcessA (nullptr, buf.data (), nullptr, nullptr, FALSE,
	                     drapeaux, nullptr, nullptr, &si, &pi))
		return false;

	CloseHandle (pi.hThread);
	if (g_agentProc != nullptr)
		CloseHandle (g_agentProc);
	g_agentProc = pi.hProcess;
	g_agentPid = pi.dwProcessId;
	return true;
}

// Lance un programme et récupère sa sortie (utilisé pour --check).
std::string lancerEtLire (const std::string& arguments, DWORD attenteMs)
{
	std::string sortie;
	const std::string exe = cheminAgent ();
	if (exe.empty ())
		return sortie;

	SECURITY_ATTRIBUTES sa;
	memset (&sa, 0, sizeof (sa));
	sa.nLength = sizeof (sa);
	sa.bInheritHandle = TRUE;

	HANDLE lecture = nullptr, ecriture = nullptr;
	if (!CreatePipe (&lecture, &ecriture, &sa, 0))
		return sortie;
	SetHandleInformation (lecture, HANDLE_FLAG_INHERIT, 0);

	std::string ligne = "\"" + exe + "\" " + arguments;
	std::vector<char> buf (ligne.begin (), ligne.end ());
	buf.push_back (0);

	STARTUPINFOA si;
	memset (&si, 0, sizeof (si));
	si.cb = sizeof (si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = ecriture;
	si.hStdError = ecriture;
	si.hStdInput = nullptr;

	PROCESS_INFORMATION pi;
	memset (&pi, 0, sizeof (pi));

	if (!CreateProcessA (nullptr, buf.data (), nullptr, nullptr, TRUE,
	                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
	{
		CloseHandle (lecture);
		CloseHandle (ecriture);
		return sortie;
	}

	CloseHandle (ecriture);
	WaitForSingleObject (pi.hProcess, attenteMs);

	char morceau[512];
	DWORD lus = 0;
	while (ReadFile (lecture, morceau, sizeof (morceau) - 1, &lus, nullptr) && lus > 0)
	{
		morceau[lus] = 0;
		sortie += morceau;
	}

	CloseHandle (lecture);
	CloseHandle (pi.hThread);
	CloseHandle (pi.hProcess);
	return sortie;
}

// Adresse IPv4 locale (première non-loopback).
std::string adresseLocale ()
{
	// On demande a Windows quelle adresse sortirait vers internet : c'est la
	// seule qui soit joignable par l'autre poste. gethostname() renvoyait une
	// adresse d'une carte virtuelle au lieu de la vraie carte reseau.
	SOCKET s = socket (AF_INET, SOCK_DGRAM, 0);
	if (s == INVALID_SOCKET)
		return "127.0.0.1";

	sockaddr_in cible;
	memset (&cible, 0, sizeof (cible));
	cible.sin_family = AF_INET;
	cible.sin_port = htons (53);
	cible.sin_addr.s_addr = inet_addr ("8.8.8.8");

	std::string resultat = "127.0.0.1";
	if (connect (s, (sockaddr*)&cible, sizeof (cible)) == 0)
	{
		sockaddr_in local;
		int taille = sizeof (local);
		if (getsockname (s, (sockaddr*)&local, &taille) == 0)
		{
			const unsigned char* o = (const unsigned char*)&local.sin_addr;
			char texte[64];
			snprintf (texte, sizeof (texte), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
			if (o[0] != 127)
				resultat = texte;
		}
	}
	closesocket (s);
	return resultat;
}

void copier (const std::string& texte)
{
	if (texte.empty ())
		return;
	if (!OpenClipboard (g_panel))
		return;
	EmptyClipboard ();
	const size_t n = texte.size () + 1;
	HGLOBAL mem = GlobalAlloc (GMEM_MOVEABLE, n);
	if (mem != nullptr)
	{
		void* p = GlobalLock (mem);
		if (p != nullptr)
		{
			memcpy (p, texte.c_str (), n);
			GlobalUnlock (mem);
			SetClipboardData (CF_TEXT, mem);
		}
	}
	CloseClipboard ();
}

// Peinture : coins arrondis + dégradé discret
void carte (HDC dc, RECT r, COLORREF fond, COLORREF bord)
{
	HBRUSH b = CreateSolidBrush (fond);
	HPEN p = CreatePen (PS_SOLID, 1, bord);
	HGDIOBJ ob = SelectObject (dc, b);
	HGDIOBJ op = SelectObject (dc, p);
	RoundRect (dc, r.left, r.top, r.right, r.bottom, 10, 10);
	SelectObject (dc, ob);
	SelectObject (dc, op);
	DeleteObject (b);
	DeleteObject (p);
}

void texteCentre (HDC dc, const wchar_t* s, RECT r, COLORREF c, HFONT f)
{
	HGDIOBJ of = SelectObject (dc, f);
	SetBkMode (dc, TRANSPARENT);
	SetTextColor (dc, c);
	DrawTextW (dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject (dc, of);
}

void texteGauche (HDC dc, const char* s, RECT r, COLORREF c, HFONT f, UINT format = 0)
{
	HGDIOBJ of = SelectObject (dc, f);
	SetBkMode (dc, TRANSPARENT);
	SetTextColor (dc, c);
	int largeur = MultiByteToWideChar (CP_UTF8, 0, s, -1, nullptr, 0);
	std::wstring w (largeur > 0 ? largeur : 1, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s, -1, &w[0], largeur);
	DrawTextW (dc, w.c_str (), -1, &r, format | DT_SINGLELINE | DT_END_ELLIPSIS);
	SelectObject (dc, of);
}

std::string texteDeLEdit ()
{
	if (g_edit == nullptr)
		return std::string ();
	char buf[128] = { 0 };
	GetWindowTextA (g_edit, buf, sizeof (buf) - 1);
	return std::string (buf);
}

void poserEdit (const std::string& s)
{
	if (g_edit != nullptr)
		SetWindowTextA (g_edit, s.c_str ());
}

// --------------------------------------------------------------------------
// Actions des boutons
// --------------------------------------------------------------------------
void actionHeberger ()
{
	// On n'invente aucune adresse : seule celle que l'agent obtient (IP
	// publique + port reellement ouvert sur la box) est juste.
	poserEdit ("");
	if (lancerAgent ("--listen 5000", false))
	{
		fmgui::setRole (1);
		fmgui::setStatus ("ouverture de la session\u2026 l'adresse arrive");
	}
	else
	{
		fmgui::setStatus ("fm_agent.exe introuvable : reinstalle le plugin");
	}
	InvalidateRect (g_panel, nullptr, FALSE);
}

void actionRejoindre ()
{
	std::string adresse = texteDeLEdit ();
	if (adresse.find (':') == std::string::npos)
		adresse += ":5000";

	if (lancerAgent ("--peer " + adresse, false))
	{
		fmgui::setRole (2);
		fmgui::setAddress (adresse.c_str ());
		fmgui::setStatus (("Connexion à " + adresse + "…").c_str ());
	}
	else
	{
		fmgui::setStatus ("fm_agent.exe introuvable : reinstalle le plugin");
	}
	InvalidateRect (g_panel, nullptr, FALSE);
}

void actionQuitter ()
{
	if (g_agentProc != nullptr)
	{
		TerminateProcess (g_agentProc, 0);
		CloseHandle (g_agentProc);
		g_agentProc = nullptr;
		g_agentPid = 0;
	}
	fmgui::setRole (0);
	fmgui::setStatus ("Aucune session");
	InvalidateRect (g_panel, nullptr, FALSE);
}

// Vrai si le fichier du plugin sur le disque a ete remplace APRES le
// demarrage de FL : la version chargee en memoire est donc perimee.
bool pluginObsolete ()
{
	wchar_t chemin[MAX_PATH] = { 0 };
	HMODULE soi = nullptr;
	if (!GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                         | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         (LPCWSTR)&pluginObsolete, &soi))
		return false;
	if (GetModuleFileNameW (soi, chemin, MAX_PATH) == 0)
		return false;

	WIN32_FILE_ATTRIBUTE_DATA info;
	if (!GetFileAttributesExW (chemin, GetFileExInfoStandard, &info))
		return false;

	FILETIME creation, sortie, noyau, utilisateur;
	if (!GetProcessTimes (GetCurrentProcess (), &creation, &sortie,
	                      &noyau, &utilisateur))
		return false;

	ULARGE_INTEGER fichier, processus;
	fichier.LowPart = info.ftLastWriteTime.dwLowDateTime;
	fichier.HighPart = info.ftLastWriteTime.dwHighDateTime;
	processus.LowPart = creation.dwLowDateTime;
	processus.HighPart = creation.dwHighDateTime;
	return fichier.QuadPart > processus.QuadPart;
}

std::string dossierLocal ()
{
	char base[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA ("LOCALAPPDATA", base, MAX_PATH) == 0)
		return std::string ();
	return std::string (base) + "\\FruityMulti";
}

void actionInstaller ()
{
	// L'agent telecharge la nouvelle version puis lance l'installateur, qui
	// ferme FL, copie le plugin et l'agent au bon endroit, et redemarre FL.
	if (!lancerAgent ("--maj --installer", false))
	{
		fmgui::soumettreEtat ("fm_agent.exe introuvable : reinstalle le plugin");
	}
	else
	{
		const std::string setup = dossierLocal () + "\\FruityMultiSetup.exe";
		if (GetFileAttributesA (setup.c_str ()) == INVALID_FILE_ATTRIBUTES)
			fmgui::soumettreEtat ("installateur absent : reinstalle une fois a la main");
		else
			fmgui::soumettreEtat ("mise a jour en cours : FL va redemarrer tout seul");
	}
	InvalidateRect (g_panel, nullptr, FALSE);
}

void actionCopier ()
{
	const std::string a = texteDeLEdit ();
	copier (a.empty () ? g_adresse : a);
	fmgui::soumettreEtat ("adresse copiée");
	InvalidateRect (g_panel, nullptr, FALSE);
}

// Reponse de la verification, arrivee par WM_APP + 2 (thread de travail).
void afficherResultatMaj ()
{
	const int distante = g_checkResultat.exchange (-1);
	g_checkEnCours = false;
	if (distante < 0)
	{
		InvalidateRect (g_panel, nullptr, FALSE);
		return;
	}

	char message[128];
	if (distante == 0)
	{
		g_majDispo = 0;
		snprintf (message, sizeof (message),
		          "vérification impossible (pas de réseau ?)");
	}
	else if (distante <= fmgui::kVersionPlugin)
	{
		g_majDispo = 0;
		snprintf (message, sizeof (message),
		          "à jour (version %d)", fmgui::kVersionPlugin);
	}
	else
	{
		g_majDispo = distante;
		snprintf (message, sizeof (message),
		          "version %d disponible : clique sur Installer", distante);
	}
	fmgui::soumettreEtat (message);
	InvalidateRect (g_panel, nullptr, FALSE);
}

void actionMaj ()
{
	// Le fichier a deja ete remplace (installation ou mise a jour faite
	// pendant que FL tournait) : inutile de telecharger, il faut redemarrer.
	if (pluginObsolete ())
	{
		g_majDispo = 0;
		fmgui::soumettreEtat ("mise à jour déjà installée : redémarre FL Studio");
		InvalidateRect (g_panel, nullptr, FALSE);
		return;
	}

	if (g_checkEnCours.exchange (true))
		return;   // deja en cours

	fmgui::soumettreEtat ("vérification en cours…");
	InvalidateRect (g_panel, nullptr, FALSE);

	// JAMAIS d'attente sur le thread de l'interface : Windows declare FL
	// fige au bout de 5 s. On interroge GitHub depuis un thread.
	std::thread ([] {
		const std::string sortie = lancerEtLire ("--check", 20000);
		int distante = 0;
		const size_t pos = sortie.find ("MAJ ");
		if (pos != std::string::npos)
			distante = atoi (sortie.c_str () + pos + 4);
		g_checkResultat = distante;
		if (g_panel != nullptr)
			PostMessageW (g_panel, WM_APP + 2, 0, 0);
	}).detach ();
}

// --------------------------------------------------------------------------
// Fenêtre
// --------------------------------------------------------------------------
void dessiner (HDC dc, int largeur, int hauteur)
{
	// fond
	HBRUSH fond = CreateSolidBrush (C_FOND);
	RECT tout = { 0, 0, largeur, hauteur };
	FillRect (dc, &tout, fond);
	DeleteObject (fond);

	HFONT titre   = CreateFontW (-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
	                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                             DEFAULT_PITCH, L"Segoe UI");
	HFONT sous    = CreateFontW (-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                             DEFAULT_PITCH, L"Segoe UI");
	HFONT bouton  = CreateFontW (-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
	                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                             DEFAULT_PITCH, L"Segoe UI");
	HFONT petit   = CreateFontW (-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                             DEFAULT_PITCH, L"Segoe UI");

	RECT r = { L, 16, largeur - L, 52 };
	texteGauche (dc, "FruityMulti", r, C_TEXTE, titre);

	RECT rv = { largeur - 150, 22, largeur - L, 46 };
	char version[64];
	snprintf (version, sizeof (version), "version %d", fmgui::kVersionPlugin);
	texteGauche (dc, version, rv, C_DIM, petit, DT_RIGHT);

	r = { L, 50, largeur - L, 72 };
	texteGauche (dc, "Produire à deux dans FL Studio, chacun sur son PC", r, C_DIM, sous);

	// deux gros boutons
	const int bw = (largeur - 3 * L) / 2;
	g_btnHeberger = { L, 88, L + bw, 148 };
	g_btnRejoindre = { L * 2 + bw, 88, largeur - L, 148 };

	carte (dc, g_btnHeberger, g_hover[0] ? C_ACCENT2 : C_ACCENT, C_ACCENT);
	texteCentre (dc, L"HÉBERGER", g_btnHeberger, RGB (20, 12, 6), bouton);

	carte (dc, g_btnRejoindre, g_hover[1] ? RGB (0x2A, 0x2C, 0x34) : C_CARTE, C_BORD);
	texteCentre (dc, L"REJOINDRE", g_btnRejoindre, C_TEXTE, bouton);

	// adresse
	r = { L, 164, largeur - L, 184 };
	texteGauche (dc, "Adresse de la session", r, C_DIM, petit);

	// le champ lui-même est un contrôle enfant (voir WM_CREATE)
	g_btnCopier = { largeur - L - 88, 190, largeur - L, 222 };
	carte (dc, g_btnCopier, g_hover[2] ? RGB (0x2A, 0x2C, 0x34) : C_CARTE, C_BORD);
	texteCentre (dc, L"Copier", g_btnCopier, C_TEXTE, petit);

	// état
	COLORREF couleur = C_DIM;
	if (g_role == 1) couleur = C_ACCENT;
	if (g_role == 2) couleur = C_VERT;

	HBRUSH pastille = CreateSolidBrush (couleur);
	HGDIOBJ ob = SelectObject (dc, pastille);
	HGDIOBJ op = SelectObject (dc, GetStockObject (NULL_PEN));
	Ellipse (dc, L, 246, L + 10, 256);
	SelectObject (dc, ob);
	SelectObject (dc, op);
	DeleteObject (pastille);

	RECT re = { L + 18, 240, largeur - L - 92, 262 };
	texteGauche (dc, g_statut.c_str (), re, C_TEXTE, sous);

	// bouton Quitter, seulement quand une session tourne
	g_btnQuitter = { largeur - L - 84, 236, largeur - L, 264 };
	if (g_role != 0)
	{
		carte (dc, g_btnQuitter,
		       g_hover[4] ? RGB (0x3A, 0x2A, 0x2A) : C_CARTE, C_BORD);
		texteCentre (dc, L"Quitter", g_btnQuitter, C_ROUGE, petit);
	}

	// pied de page
	g_btnMaj = { largeur - L - 190, 292, largeur - L, 324 };
	carte (dc, g_btnMaj, g_hover[3] ? RGB (0x2A, 0x2C, 0x34) : C_CARTE, C_BORD);
	if (g_majDispo > 0)
	{
		wchar_t libelle[64];
		swprintf_s (libelle, L"Installer la version %d", g_majDispo);
		carte (dc, g_btnMaj, g_hover[3] ? C_ACCENT2 : C_ACCENT, C_ACCENT);
		texteCentre (dc, libelle, g_btnMaj, RGB (20, 12, 6), petit);
	}
	else
	{
		texteCentre (dc, L"Vérifier les mises à jour", g_btnMaj, C_TEXTE, petit);
	}

	RECT rm = { L, 296, largeur - L - 200, 322 };
	texteGauche (dc, g_maj.c_str (), rm, C_DIM, petit);

	DeleteObject (titre);
	DeleteObject (sous);
	DeleteObject (bouton);
	DeleteObject (petit);
}

LRESULT CALLBACK panelProc (HWND h, UINT msg, WPARAM w, LPARAM l)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		HFONT police = CreateFontA (-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
		                            DEFAULT_PITCH, "Segoe UI");
		g_edit = CreateWindowExA (0, "EDIT", "",
		                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		                          L, 190, W - 2 * L - 100, 32,
		                          h, nullptr, nullptr, nullptr);
		SendMessageA (g_edit, WM_SETFONT, (WPARAM)police, TRUE);

		// rien de pre-rempli : une adresse inventee est pire que pas d'adresse
		SetWindowTextA (g_edit, "");

		SetTimer (h, 1, 400, nullptr);
		return 0;
	}
	case WM_APP + 2:
		afficherResultatMaj ();
		return 0;

	case WM_TIMER:
	{
		// L'agent connait la vraie adresse (UPnP) : on suit ce qu'il annonce.
		static std::string derniere;
		std::string voulue;
		{
			std::lock_guard<std::mutex> g (g_mtx);
			voulue = g_adresse;
		}
		if (!voulue.empty () && voulue != derniere)
		{
			derniere = voulue;
			if (g_edit != nullptr && GetFocus () != g_edit)
				SetWindowTextA (g_edit, voulue.c_str ());
		}
		InvalidateRect (h, nullptr, FALSE);
		return 0;
	}

	case WM_CTLCOLOREDIT:
	{
		HDC dc = (HDC)w;
		SetBkColor (dc, RGB (0x24, 0x26, 0x2D));
		SetTextColor (dc, C_TEXTE);
		static HBRUSH pinceau = CreateSolidBrush (RGB (0x24, 0x26, 0x2D));
		return (LRESULT)pinceau;
	}

	case WM_MOUSEMOVE:
	{
		POINT p = { GET_X_LPARAM (l), GET_Y_LPARAM (l) };
		g_hover[0] = PtInRect (&g_btnHeberger, p) != 0;
		g_hover[1] = PtInRect (&g_btnRejoindre, p) != 0;
		g_hover[2] = PtInRect (&g_btnCopier, p) != 0;
		g_hover[3] = PtInRect (&g_btnMaj, p) != 0;
		g_hover[4] = g_role != 0 && PtInRect (&g_btnQuitter, p) != 0;
		InvalidateRect (h, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR:
	{
		POINT p;
		GetCursorPos (&p);
		ScreenToClient (h, &p);
		if (PtInRect (&g_btnHeberger, p) || PtInRect (&g_btnRejoindre, p)
		    || PtInRect (&g_btnCopier, p) || PtInRect (&g_btnMaj, p)
		    || (g_role != 0 && PtInRect (&g_btnQuitter, p)))
		{
			SetCursor (LoadCursorW (nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}

	case WM_LBUTTONDOWN:
	{
		POINT p = { GET_X_LPARAM (l), GET_Y_LPARAM (l) };
		if (PtInRect (&g_btnHeberger, p))
			actionHeberger ();
		else if (PtInRect (&g_btnRejoindre, p))
			actionRejoindre ();
		else if (PtInRect (&g_btnCopier, p))
			actionCopier ();
		else if (PtInRect (&g_btnMaj, p))
		{
			if (g_majDispo > 0)
				actionInstaller ();
			else
				actionMaj ();
		}
		else if (g_role != 0 && PtInRect (&g_btnQuitter, p))
			actionQuitter ();
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;   // tout est redessiné dans WM_PAINT

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint (h, &ps);
		if (dc != nullptr)
		{
			// double tampon : évite le clignotement
			HDC mem = CreateCompatibleDC (dc);
			HBITMAP bmp = CreateCompatibleBitmap (dc, W, H);
			HGDIOBJ ancien = SelectObject (mem, bmp);
			dessiner (mem, W, H);
			BitBlt (dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
			SelectObject (mem, ancien);
			DeleteObject (bmp);
			DeleteDC (mem);
		}
		EndPaint (h, &ps);
		return 0;
	}

	case WM_DESTROY:
		KillTimer (h, 1);
		g_panel = nullptr;
		g_edit = nullptr;
		return 0;
	}
	return DefWindowProcA (h, msg, w, l);
}

void enregistrerClasse ()
{
	static bool fait = false;
	if (fait)
		return;
	WNDCLASSEXA wc;
	memset (&wc, 0, sizeof (wc));
	wc.cbSize = sizeof (wc);
	wc.lpfnWndProc = panelProc;
	wc.hInstance = GetModuleHandleA (nullptr);
	wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
	wc.lpszClassName = kClasse;
	wc.hbrBackground = nullptr;
	RegisterClassExA (&wc);
	fait = true;
}

} // namespace

// --------------------------------------------------------------------------
// Etat partagé
// --------------------------------------------------------------------------
namespace fmgui {

void setRole (int role)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_role = role;
}

void setStatus (const char* texte)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_statut = (texte != nullptr && *texte != 0) ? texte : "Aucune session";
	g_agent = true;
}

void setAddress (const char* adresse)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_adresse = (adresse != nullptr) ? adresse : "";
}

void soumettreEtat (const char* texte)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_maj = (texte != nullptr) ? texte : "";
}

int role ()
{
	std::lock_guard<std::mutex> g (g_mtx);
	return g_role;
}

bool agentLance ()
{
	std::lock_guard<std::mutex> g (g_mtx);
	return g_agent;
}

} // namespace fmgui

// --------------------------------------------------------------------------
// La vue VST3
// --------------------------------------------------------------------------
namespace {

class FmView : public IPlugView
{
public:
	FmView () : refCount (1) {}

	// --- FUnknown ---------------------------------------------------------
	tresult PLUGIN_API queryInterface (const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (obj == nullptr)
			return kInvalidArgument;
		if (FUnknownPrivate::iidEqual (iid, FUnknown::iid)
		    || FUnknownPrivate::iidEqual (iid, IPlugView::iid))
		{
			*obj = static_cast<IPlugView*> (this);
			addRef ();
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef () SMTG_OVERRIDE { return ++refCount; }

	uint32 PLUGIN_API release () SMTG_OVERRIDE
	{
		const uint32 restant = --refCount;
		if (restant == 0)
			delete this;
		return restant;
	}

	// --- IPlugView --------------------------------------------------------
	tresult PLUGIN_API isPlatformTypeSupported (FIDString type) SMTG_OVERRIDE
	{
		if (type == nullptr)
			return kResultFalse;
		return strcmp (type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
	}

	tresult PLUGIN_API attached (void* parent, FIDString type) SMTG_OVERRIDE
	{
		if (parent == nullptr || type == nullptr)
			return kResultFalse;
		if (strcmp (type, kPlatformTypeHWND) != 0)
			return kResultFalse;
		if (g_panel != nullptr)
			return kResultFalse;

		enregistrerClasse ();
		g_panel = CreateWindowExA (0, kClasse, "",
		                           WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		                           0, 0, W, H,
		                           (HWND)parent, nullptr,
		                           GetModuleHandleA (nullptr), nullptr);
		return g_panel != nullptr ? kResultTrue : kResultFalse;
	}

	tresult PLUGIN_API removed () SMTG_OVERRIDE
	{
		if (g_panel != nullptr)
		{
			DestroyWindow (g_panel);
			g_panel = nullptr;
		}
		return kResultTrue;
	}

	tresult PLUGIN_API onWheel (float) SMTG_OVERRIDE { return kResultFalse; }

	tresult PLUGIN_API onKeyDown (char16, int16, int16) SMTG_OVERRIDE
	{
		return kResultFalse;
	}

	tresult PLUGIN_API onKeyUp (char16, int16, int16) SMTG_OVERRIDE
	{
		return kResultFalse;
	}

	tresult PLUGIN_API getSize (ViewRect* taille) SMTG_OVERRIDE
	{
		if (taille == nullptr)
			return kInvalidArgument;
		taille->left = 0;
		taille->top = 0;
		taille->right = W;
		taille->bottom = H;
		return kResultTrue;
	}

	tresult PLUGIN_API onSize (ViewRect*) SMTG_OVERRIDE { return kResultTrue; }

	tresult PLUGIN_API onFocus (TBool) SMTG_OVERRIDE { return kResultTrue; }

	tresult PLUGIN_API setFrame (IPlugFrame*) SMTG_OVERRIDE { return kResultTrue; }

	tresult PLUGIN_API canResize () SMTG_OVERRIDE { return kResultFalse; }

	tresult PLUGIN_API checkSizeConstraint (ViewRect* rect) SMTG_OVERRIDE
	{
		if (rect == nullptr)
			return kInvalidArgument;
		rect->right = rect->left + W;
		rect->bottom = rect->top + H;
		return kResultTrue;
	}

private:
	~FmView () = default;
	std::atomic<uint32> refCount;
};

} // namespace

IPlugView* fmCreateView ()
{
	return new FmView ();
}
