// FruityMulti - la fenetre du plugin.
//
// Une fenetre Win32 enfant de FL, rendue en DirectX 11 par Dear ImGui. Le
// dessin lui-meme est dans fmstation.cpp ; ici : l'etat partage avec l'agent,
// le lancement de l'agent, la mise a jour, et la plomberie de la vue VST3.
//
// Tout tourne sur le thread de l'interface de FL (minuterie de 16 ms) : rien
// ici ne doit bloquer, sinon FL entier se fige.

#include "fmgui.h"
#include "fmstation.h"

// winsock2 doit venir avant windows.h
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <d3d11.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "polices.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

using namespace Steinberg;

// --------------------------------------------------------------------------
// Etat partage (ecrit par le fil reseau du plugin, lu par la fenetre)
// --------------------------------------------------------------------------
namespace {

std::mutex g_mtx;
std::string g_statut;
std::string g_adresse;
std::string g_pair;
std::string g_erreur;   // pourquoi l'agent s'est arrete, s'il l'a dit
int  g_role = 0;
bool g_agent = false;

// l'agent de session lance depuis la fenetre (un seul par FL)
HANDLE g_agentProc = nullptr;
bool   g_arretVoulu = false;

// mise a jour
std::atomic<int> g_majEtat ((int)fmstation::Maj::Repos);
std::atomic<int> g_majVersion (0);
std::atomic<bool> g_checkEnCours (false);

// --------------------------------------------------------------------------
// Petits utilitaires
// --------------------------------------------------------------------------
std::string dossierLocal ()
{
	char base[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA ("LOCALAPPDATA", base, MAX_PATH) == 0)
		return std::string ();
	return std::string (base) + "\\FruityMulti";
}

std::string cheminAgent ()
{
	const std::string local = dossierLocal ();
	if (!local.empty ())
	{
		const std::string p = local + "\\fm_agent.exe";
		if (GetFileAttributesA (p.c_str ()) != INVALID_FILE_ATTRIBUTES)
			return p;
	}

	// repli : a cote de la DLL du plugin
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

HINSTANCE moduleDuPlugin ()
{
	HMODULE soi = nullptr;
	GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    (LPCWSTR)&moduleDuPlugin, &soi);
	return (HINSTANCE)soi;
}

bool creerProcessus (const std::string& arguments, HANDLE* proc)
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
	if (!CreateProcessA (nullptr, buf.data (), nullptr, nullptr, FALSE,
	                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;
	CloseHandle (pi.hThread);
	if (proc != nullptr)
		*proc = pi.hProcess;
	else
		CloseHandle (pi.hProcess);
	return true;
}

// Arrete tous les agents de session. Un agent orphelin (FL ferme ou plante
// pendant une session) gardait le verrou : le suivant sortait aussitot et la
// fenetre affichait une session qui n'existait pas.
void tuerAgents ()
{
	if (g_agentProc != nullptr)
	{
		g_arretVoulu = true;
		TerminateProcess (g_agentProc, 0);
		WaitForSingleObject (g_agentProc, 1000);
		CloseHandle (g_agentProc);
		g_agentProc = nullptr;
	}

	HANDLE instantane = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
	if (instantane == INVALID_HANDLE_VALUE)
		return;
	PROCESSENTRY32W entree;
	entree.dwSize = sizeof (entree);
	if (Process32FirstW (instantane, &entree))
	{
		do
		{
			if (_wcsicmp (entree.szExeFile, L"fm_agent.exe") == 0)
			{
				HANDLE p = OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
				                        entree.th32ProcessID);
				if (p != nullptr)
				{
					TerminateProcess (p, 0);
					WaitForSingleObject (p, 1000);
					CloseHandle (p);
				}
			}
		} while (Process32NextW (instantane, &entree));
	}
	CloseHandle (instantane);
}

bool lancerSession (const std::string& arguments)
{
	tuerAgents ();
	char parent[32];
	snprintf (parent, sizeof (parent), " --parent %lu", GetCurrentProcessId ());
	g_arretVoulu = false;
	{
		std::lock_guard<std::mutex> g (g_mtx);
		g_erreur.clear ();
	}
	return creerProcessus (arguments + parent, &g_agentProc);
}

// L'agent s'est arrete sans qu'on le demande : la fenetre doit le dire au
// lieu d'afficher une session fantome.
void surveillerAgent ()
{
	if (g_agentProc == nullptr)
		return;
	if (WaitForSingleObject (g_agentProc, 0) != WAIT_OBJECT_0)
		return;
	CloseHandle (g_agentProc);
	g_agentProc = nullptr;
	if (!g_arretVoulu)
	{
		std::lock_guard<std::mutex> g (g_mtx);
		g_role = 0;
		g_pair.clear ();
		g_statut = g_erreur.empty () ? "L'agent s'est arrêté — voir le journal" : g_erreur;
	}
}

// Lance un programme et recupere sa sortie (utilise pour --check). La sortie
// est lue au fil de l'eau : attendre la fin d'abord bloquait l'enfant des que
// le tuyau etait plein.
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

	const DWORD debut = GetTickCount ();
	for (;;)
	{
		DWORD dispo = 0;
		if (PeekNamedPipe (lecture, nullptr, 0, nullptr, &dispo, nullptr) && dispo > 0)
		{
			char morceau[512];
			DWORD lus = 0;
			if (ReadFile (lecture, morceau, sizeof (morceau), &lus, nullptr) && lus > 0)
				sortie.append (morceau, lus);
			continue;
		}
		if (WaitForSingleObject (pi.hProcess, 50) == WAIT_OBJECT_0)
		{
			char morceau[512];
			DWORD lus = 0;
			while (PeekNamedPipe (lecture, nullptr, 0, nullptr, &dispo, nullptr) && dispo > 0
			       && ReadFile (lecture, morceau, sizeof (morceau), &lus, nullptr) && lus > 0)
				sortie.append (morceau, lus);
			break;
		}
		if (GetTickCount () - debut > attenteMs)
		{
			TerminateProcess (pi.hProcess, 1);
			break;
		}
	}

	CloseHandle (lecture);
	CloseHandle (pi.hThread);
	CloseHandle (pi.hProcess);
	return sortie;
}

// Vrai si le fichier du plugin sur le disque a ete remplace APRES le
// demarrage de FL : la version chargee en memoire est donc perimee.
bool pluginObsolete ()
{
	wchar_t chemin[MAX_PATH] = { 0 };
	if (GetModuleFileNameW (moduleDuPlugin (), chemin, MAX_PATH) == 0)
		return false;

	WIN32_FILE_ATTRIBUTE_DATA info;
	if (!GetFileAttributesExW (chemin, GetFileExInfoStandard, &info))
		return false;

	FILETIME creation, sortie, noyau, utilisateur;
	if (!GetProcessTimes (GetCurrentProcess (), &creation, &sortie, &noyau, &utilisateur))
		return false;

	ULARGE_INTEGER fichier, processus;
	fichier.LowPart = info.ftLastWriteTime.dwLowDateTime;
	fichier.HighPart = info.ftLastWriteTime.dwHighDateTime;
	processus.LowPart = creation.dwLowDateTime;
	processus.HighPart = creation.dwHighDateTime;
	return fichier.QuadPart > processus.QuadPart;
}

std::wstring enLarge (const std::string& s)
{
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, nullptr, 0);
	std::wstring w (n > 0 ? n : 1, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, &w[0], n);
	return w;
}

std::string enUtf8 (const wchar_t* w)
{
	const int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s (n > 0 ? n : 1, '\0');
	WideCharToMultiByte (CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	if (!s.empty () && s.back () == '\0')
		s.pop_back ();
	return s;
}

void copierTexte (HWND proprietaire, const std::string& texte)
{
	if (texte.empty () || !OpenClipboard (proprietaire))
		return;
	EmptyClipboard ();
	const std::wstring w = enLarge (texte);
	HGLOBAL mem = GlobalAlloc (GMEM_MOVEABLE, w.size () * sizeof (wchar_t));
	if (mem != nullptr)
	{
		void* p = GlobalLock (mem);
		if (p != nullptr)
		{
			memcpy (p, w.c_str (), w.size () * sizeof (wchar_t));
			GlobalUnlock (mem);
			if (SetClipboardData (CF_UNICODETEXT, mem) == nullptr)
				GlobalFree (mem);
		}
	}
	CloseClipboard ();
}

std::string lirePressePapiers (HWND proprietaire)
{
	std::string r;
	if (!OpenClipboard (proprietaire))
		return r;
	HANDLE h = GetClipboardData (CF_UNICODETEXT);
	if (h != nullptr)
	{
		const wchar_t* w = (const wchar_t*)GlobalLock (h);
		if (w != nullptr)
		{
			r = enUtf8 (w);
			GlobalUnlock (h);
		}
	}
	CloseClipboard ();
	return r.size () > 200 ? r.substr (0, 200) : r;
}

std::string nomDuPoste ()
{
	char nom[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
	DWORD n = sizeof (nom);
	GetComputerNameA (nom, &n);
	return nom;
}

// Fin d'un fichier texte (le journal de l'agent peut peser 400 Ko).
std::vector<std::string> dernieresLignes (const std::string& chemin, size_t combien)
{
	std::vector<std::string> lignes;
	HANDLE f = CreateFileA (chemin.c_str (), GENERIC_READ,
	                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                        nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return lignes;
	LARGE_INTEGER taille;
	GetFileSizeEx (f, &taille);
	const LONGLONG lire = taille.QuadPart < 4096 ? taille.QuadPart : 4096;
	LARGE_INTEGER pos;
	pos.QuadPart = taille.QuadPart - lire;
	SetFilePointerEx (f, pos, nullptr, FILE_BEGIN);
	std::string buf ((size_t)lire, '\0');
	DWORD lus = 0;
	if (lire > 0)
		ReadFile (f, &buf[0], (DWORD)lire, &lus, nullptr);
	CloseHandle (f);
	buf.resize (lus);

	size_t debut = 0;
	while (debut < buf.size ())
	{
		size_t fin = buf.find ('\n', debut);
		if (fin == std::string::npos)
			fin = buf.size ();
		std::string l = buf.substr (debut, fin - debut);
		while (!l.empty () && (l.back () == '\r' || l.back () == ' '))
			l.pop_back ();
		// les cadres de « ==== » de l'agent n'apportent rien ici
		size_t g = 0;
		while (g < l.size () && (l[g] == ' ' || l[g] == '='))
			++g;
		// une erreur repetee en boucle ne doit pas remplir tout l'ecran
		if (g < l.size () && (lignes.empty () || lignes.back () != l.substr (g)))
			lignes.push_back (l.substr (g));
		debut = fin + 1;
	}
	if (lire == 4096 && !lignes.empty ())
		lignes.erase (lignes.begin ());   // premiere ligne sans doute coupee
	if (lignes.size () > combien)
		lignes.erase (lignes.begin (), lignes.end () - combien);
	return lignes;
}

// compat.txt, ecrit par l'agent : « = nom », « + nom », « - nom »
std::vector<std::pair<char, std::string>> lireCompat (const std::string& chemin)
{
	std::vector<std::pair<char, std::string>> r;
	FILE* f = fopen (chemin.c_str (), "rb");
	if (f == nullptr)
		return r;
	char ligne[512];
	while (fgets (ligne, sizeof (ligne), f) != nullptr)
	{
		std::string l (ligne);
		while (!l.empty () && (l.back () == '\n' || l.back () == '\r'))
			l.pop_back ();
		if (l.size () > 2 && (l[0] == '=' || l[0] == '+' || l[0] == '-') && l[1] == ' ')
			r.emplace_back (l[0], l.substr (2));
	}
	fclose (f);
	return r;
}

// --------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------
void actionHeberger ()
{
	{
		std::lock_guard<std::mutex> g (g_mtx);
		g_adresse.clear ();
		g_pair.clear ();
	}
	if (lancerSession ("--heberger"))
	{
		fmgui::setRole (1);
		fmgui::setStatus ("Ouverture de la station…");
	}
	else
	{
		fmgui::setStatus ("fm_agent.exe introuvable : réinstalle FruityMulti");
	}
}

void actionRejoindre (const std::string& brut)
{
	// Un code (K7P2QX) ou, en direct, ip:port/CODE. Rien d'autre ne passe :
	// le texte finit sur la ligne de commande de l'agent.
	std::string adresse;
	for (char c : brut)
		if (isalnum ((unsigned char)c) || c == '.' || c == ':' || c == '/' || c == '-')
			adresse += c;
	if (adresse.empty ())
		return;

	{
		std::lock_guard<std::mutex> g (g_mtx);
		g_pair.clear ();
	}
	if (lancerSession ("--rejoindre " + adresse))
	{
		fmgui::setRole (2);
		fmgui::setAddress (adresse.c_str ());
		fmgui::setStatus ("Recherche de la fréquence…");
	}
	else
	{
		fmgui::setStatus ("fm_agent.exe introuvable : réinstalle FruityMulti");
	}
}

void actionCouper ()
{
	tuerAgents ();
	fmgui::agentParti ();
}

void actionVerifierMaj ()
{
	// Le fichier a deja ete remplace (installation faite pendant que FL
	// tournait) : inutile de telecharger, il faut redemarrer.
	if (pluginObsolete ())
	{
		g_majEtat = (int)fmstation::Maj::Redemarrer;
		return;
	}
	if (g_checkEnCours.exchange (true))
		return;
	g_majEtat = (int)fmstation::Maj::Verif;

	// JAMAIS d'attente sur le thread de l'interface : Windows declare FL
	// fige au bout de 5 s. On interroge GitHub depuis un thread.
	std::thread ([] {
		const DWORD debut = GetTickCount ();
		const std::string sortie = lancerEtLire ("--check", 20000);
		int distante = 0;
		const size_t pos = sortie.find ("MAJ ");
		if (pos != std::string::npos)
			distante = atoi (sortie.c_str () + pos + 4);
		// une reponse instantanee passerait inapercue : on laisse le temps
		// de voir que GitHub a ete interroge
		const DWORD duree = GetTickCount () - debut;
		if (duree < 700)
			Sleep (700 - duree);
		g_majVersion = distante;
		if (distante <= 0)
			g_majEtat = (int)fmstation::Maj::Erreur;
		else if (distante <= fmgui::kVersionPlugin)
			g_majEtat = (int)fmstation::Maj::AJour;
		else
			g_majEtat = (int)fmstation::Maj::Dispo;
		g_checkEnCours = false;
	}).detach ();
}

void actionInstallerMaj ()
{
	// L'agent telecharge la nouvelle version puis lance l'installateur, qui
	// ferme FL, copie le plugin et l'agent, et redemarre FL. Ce processus-la
	// n'est pas l'agent de session : il ne doit pas etre surveille ni tue.
	if (creerProcessus ("--maj --installer", nullptr))
		g_majEtat = (int)fmstation::Maj::Telechargement;
	else
		g_majEtat = (int)fmstation::Maj::Erreur;
}

// ==========================================================================
// La vue
// ==========================================================================
const wchar_t kClasse[] = L"FruityMultiStation";
int g_vues = 0;   // classe de fenetre enregistree tant qu'il reste une vue

LRESULT CALLBACK procVue (HWND h, UINT msg, WPARAM w, LPARAM l);

class FmView : public IPlugView, public IPlugViewContentScaleSupport
{
public:
	// getSize est appele avant attached : on part de l'echelle du systeme
	FmView () : refCount (1)
	{
		const UINT dpi = GetDpiForSystem ();
		if (dpi >= 96)
			echelle = dpi / 96.0f;
	}

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
		if (FUnknownPrivate::iidEqual (iid, IPlugViewContentScaleSupport::iid))
		{
			*obj = static_cast<IPlugViewContentScaleSupport*> (this);
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
		if (parent == nullptr || type == nullptr || strcmp (type, kPlatformTypeHWND) != 0)
			return kResultFalse;
		if (hwnd != nullptr)
			return kResultFalse;

		// Pas d'echelle donnee par FL : on prend celle de l'ecran. L'hote a
		// deja dimensionne son cadre avec getSize : si l'ecran du parent n'a
		// pas l'echelle prevue, il faut le lui redemander, sinon le bas de
		// la fenetre est coupe.
		bool redemander = false;
		if (!echelleImposee)
		{
			const UINT dpi = GetDpiForWindow ((HWND)parent);
			if (dpi >= 96 && std::fabs (dpi / 96.0f - echelle) > 0.01f)
			{
				echelle = dpi / 96.0f;
				redemander = true;
			}
		}

		HINSTANCE inst = moduleDuPlugin ();
		if (g_vues++ == 0)
		{
			// Classe enregistree avec la DLL (et non l'exe de FL) et retiree
			// a la derniere vue : sinon, apres un dechargement du plugin, la
			// classe pointait vers du code disparu.
			WNDCLASSEXW wc;
			memset (&wc, 0, sizeof (wc));
			wc.cbSize = sizeof (wc);
			wc.style = CS_DBLCLKS;
			wc.lpfnWndProc = procVue;
			wc.hInstance = inst;
			wc.hCursor = LoadCursorW (nullptr, (LPCWSTR)IDC_ARROW);
			wc.lpszClassName = kClasse;
			RegisterClassExW (&wc);
		}

		hwnd = CreateWindowExW (0, kClasse, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		                        0, 0, largeurPx (), hauteurPx (), (HWND)parent, nullptr,
		                        inst, this);
		if (hwnd == nullptr)
		{
			if (--g_vues == 0)
				UnregisterClassW (kClasse, inst);
			return kResultFalse;
		}
		poste = nomDuPoste ();
		initialiserRendu ();
		SetTimer (hwnd, 1, 16, nullptr);
		dernierTick = GetTickCount64 ();
		if (redemander && frame != nullptr)
		{
			ViewRect r (0, 0, largeurPx (), hauteurPx ());
			frame->resizeView (this, &r);
		}
		return kResultTrue;
	}

	tresult PLUGIN_API removed () SMTG_OVERRIDE
	{
		if (hwnd != nullptr)
		{
			KillTimer (hwnd, 1);
			libererRendu ();
			DestroyWindow (hwnd);
			hwnd = nullptr;
			if (--g_vues == 0)
				UnregisterClassW (kClasse, moduleDuPlugin ());
		}
		return kResultTrue;
	}

	tresult PLUGIN_API onWheel (float) SMTG_OVERRIDE { return kResultFalse; }

	// Certains hotes envoient le clavier par ici plutot que par la fenetre.
	tresult PLUGIN_API onKeyDown (char16 key, int16 keyCode, int16 modifiers) SMTG_OVERRIDE
	{
		int vk = 0;
		switch (keyCode)
		{
		case KEY_UP: vk = VK_UP; break;
		case KEY_DOWN: vk = VK_DOWN; break;
		case KEY_RETURN:
		case KEY_ENTER: vk = VK_RETURN; break;
		case KEY_BACK: vk = VK_BACK; break;
		case KEY_ESCAPE: vk = VK_ESCAPE; break;
		default: break;
		}
		bool servi = false;
		const bool ctrl = (modifiers & kControlKey) != 0;
		if (ctrl && (key == 'v' || key == 'V'))
		{
			station.coller (lirePressePapiers (hwnd));
			servi = true;
		}
		else if (vk != 0)
			servi = station.touche (vk, ctrl);
		else if (key != 0)
			servi = station.caractere ((unsigned)key);
		if (servi)
			dedoublon = GetTickCount64 ();
		return servi ? kResultTrue : kResultFalse;
	}

	tresult PLUGIN_API onKeyUp (char16, int16, int16) SMTG_OVERRIDE { return kResultFalse; }

	tresult PLUGIN_API getSize (ViewRect* taille) SMTG_OVERRIDE
	{
		if (taille == nullptr)
			return kInvalidArgument;
		taille->left = 0;
		taille->top = 0;
		taille->right = largeurPx ();
		taille->bottom = hauteurPx ();
		return kResultTrue;
	}

	tresult PLUGIN_API onSize (ViewRect*) SMTG_OVERRIDE { return kResultTrue; }
	tresult PLUGIN_API onFocus (TBool) SMTG_OVERRIDE { return kResultTrue; }

	tresult PLUGIN_API setFrame (IPlugFrame* f) SMTG_OVERRIDE
	{
		frame = f;
		return kResultTrue;
	}

	tresult PLUGIN_API canResize () SMTG_OVERRIDE { return kResultFalse; }

	tresult PLUGIN_API checkSizeConstraint (ViewRect* rect) SMTG_OVERRIDE
	{
		if (rect == nullptr)
			return kInvalidArgument;
		rect->right = rect->left + largeurPx ();
		rect->bottom = rect->top + hauteurPx ();
		return kResultTrue;
	}

	// --- IPlugViewContentScaleSupport -----------------------------------
	tresult PLUGIN_API setContentScaleFactor (ScaleFactor facteur) SMTG_OVERRIDE
	{
		if (facteur < 0.5f || facteur > 4.0f)
			return kResultFalse;
		echelleImposee = true;
		if (std::fabs (facteur - echelle) < 0.01f)
			return kResultTrue;
		echelle = facteur;
		if (hwnd != nullptr)
		{
			SetWindowPos (hwnd, nullptr, 0, 0, largeurPx (), hauteurPx (),
			              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			redimensionnerRendu ();
			if (frame != nullptr)
			{
				ViewRect r (0, 0, largeurPx (), hauteurPx ());
				frame->resizeView (this, &r);
			}
		}
		return kResultTrue;
	}

	// ----------------------------------------------------------------------
	LRESULT message (UINT msg, WPARAM w, LPARAM l);

private:
	~FmView () = default;

	int largeurPx () const { return (int)(fmstation::kLargeur * echelle + 0.5f); }
	int hauteurPx () const { return (int)(fmstation::kHauteur * echelle + 0.5f); }

	bool initialiserRendu ();
	void libererRendu ();
	void redimensionnerRendu ();
	void creerCible ();
	void image ();
	void executer (fmstation::Action a, const std::string& arg);

	std::atomic<uint32> refCount;
	HWND hwnd = nullptr;
	IPlugFrame* frame = nullptr;
	float echelle = 1.0f;
	bool echelleImposee = false;
	ULONGLONG dernierTick = 0;
	ULONGLONG dedoublon = 0;     // touche deja traitee par onKeyDown
	bool suiviSouris = false;
	std::string poste;

	// rendu
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* contexte = nullptr;
	IDXGISwapChain* chaine = nullptr;
	ID3D11RenderTargetView* cible = nullptr;
	ImGuiContext* imgui = nullptr;
	fmstation::Polices polices;
	fmstation::Station station;

	// fichiers relus de temps en temps
	ULONGLONG dernierJournal = 0;
	std::vector<std::string> journal;
	std::vector<std::pair<char, std::string>> compat;
};

bool FmView::initialiserRendu ()
{
	DXGI_SWAP_CHAIN_DESC sd;
	memset (&sd, 0, sizeof (sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = largeurPx ();
	sd.BufferDesc.Height = hauteurPx ();
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL niveaux[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL obtenu;
	HRESULT hr = D3D11CreateDeviceAndSwapChain (nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
	                                            niveaux, 2, D3D11_SDK_VERSION, &sd, &chaine,
	                                            &device, &obtenu, &contexte);
	if (FAILED (hr))   // pas de carte compatible : rendu logiciel
		hr = D3D11CreateDeviceAndSwapChain (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		                                    niveaux, 2, D3D11_SDK_VERSION, &sd, &chaine,
		                                    &device, &obtenu, &contexte);
	if (FAILED (hr))
		return false;
	creerCible ();

	IMGUI_CHECKVERSION ();
	ImGuiContext* avant = ImGui::GetCurrentContext ();
	imgui = ImGui::CreateContext ();
	ImGui::SetCurrentContext (imgui);
	ImGuiIO& io = ImGui::GetIO ();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;   // les polices sont dans la DLL
	polices.titre = io.Fonts->AddFontFromMemoryTTF ((void*)archivo_cond_black,
	                                                sizeof (archivo_cond_black), 28.0f, &cfg);
	polices.texte = io.Fonts->AddFontFromMemoryTTF ((void*)archivo_semibold,
	                                                sizeof (archivo_semibold), 14.0f, &cfg);
	polices.mono = io.Fonts->AddFontFromMemoryTTF ((void*)plex_mono_medium,
	                                               sizeof (plex_mono_medium), 11.0f, &cfg);
	polices.monoGras = io.Fonts->AddFontFromMemoryTTF ((void*)plex_mono_bold,
	                                                   sizeof (plex_mono_bold), 11.0f, &cfg);
	ImGui_ImplDX11_Init (device, contexte);
	ImGui::SetCurrentContext (avant != nullptr ? avant : imgui);
	return true;
}

void FmView::creerCible ()
{
	ID3D11Texture2D* tampon = nullptr;
	if (SUCCEEDED (chaine->GetBuffer (0, IID_PPV_ARGS (&tampon))) && tampon != nullptr)
	{
		device->CreateRenderTargetView (tampon, nullptr, &cible);
		tampon->Release ();
	}
}

void FmView::redimensionnerRendu ()
{
	if (chaine == nullptr)
		return;
	if (cible != nullptr)
	{
		cible->Release ();
		cible = nullptr;
	}
	chaine->ResizeBuffers (0, largeurPx (), hauteurPx (), DXGI_FORMAT_UNKNOWN, 0);
	creerCible ();
}

void FmView::libererRendu ()
{
	if (imgui != nullptr)
	{
		ImGui::SetCurrentContext (imgui);
		ImGui_ImplDX11_Shutdown ();
		ImGui::DestroyContext (imgui);
		imgui = nullptr;
	}
	if (cible != nullptr) { cible->Release (); cible = nullptr; }
	if (chaine != nullptr) { chaine->Release (); chaine = nullptr; }
	if (contexte != nullptr) { contexte->Release (); contexte = nullptr; }
	if (device != nullptr) { device->Release (); device = nullptr; }
}

void FmView::executer (fmstation::Action a, const std::string& arg)
{
	switch (a)
	{
	case fmstation::Action::Heberger: actionHeberger (); break;
	case fmstation::Action::Rejoindre: actionRejoindre (arg); break;
	case fmstation::Action::Couper: actionCouper (); break;
	case fmstation::Action::VerifierMaj: actionVerifierMaj (); break;
	case fmstation::Action::InstallerMaj: actionInstallerMaj (); break;
	case fmstation::Action::Copier:
	{
		std::string adresse;
		{
			std::lock_guard<std::mutex> g (g_mtx);
			adresse = g_adresse;
		}
		if (adresse.empty ())
			station.signaler ("Pas encore de fréquence");
		else
		{
			copierTexte (hwnd, adresse);
			station.signaler ("Fréquence copiée → presse-papiers");
		}
		break;
	}
	}
}

void FmView::image ()
{
	surveillerAgent ();
	if (imgui == nullptr || cible == nullptr)
		return;

	const ULONGLONG maintenant = GetTickCount64 ();
	const float dt = (float)(maintenant - dernierTick) / 1000.0f;
	dernierTick = maintenant;

	// journal et comparaison : relus deux fois par seconde, pas plus
	if (maintenant - dernierJournal > 500)
	{
		dernierJournal = maintenant;
		const std::string dossier = dossierLocal ();
		journal = dernieresLignes (dossier + "\\journal.txt", 6);
		compat = lireCompat (dossier + "\\compat.txt");
	}

	fmstation::Etat e;
	{
		std::lock_guard<std::mutex> g (g_mtx);
		e.role = g_role;
		e.code = g_adresse;
		e.statut = g_statut;
		e.pair = g_pair;
	}
	e.poste = poste;
	e.version = fmgui::kVersionPlugin;
	e.maj = (fmstation::Maj)g_majEtat.load ();
	e.majVersion = g_majVersion.load ();
	e.journal = journal;
	e.compat = compat;

	ImGuiContext* avant = ImGui::GetCurrentContext ();
	ImGui::SetCurrentContext (imgui);
	ImGuiIO& io = ImGui::GetIO ();
	io.DisplaySize = ImVec2 ((float)largeurPx (), (float)hauteurPx ());
	io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

	ImGui_ImplDX11_NewFrame ();
	ImGui::NewFrame ();
	station.image (ImGui::GetBackgroundDrawList (), polices, echelle, io.DeltaTime, e);
	ImGui::Render ();

	const float fond[4] = { 0xEC / 255.0f, 0xE7 / 255.0f, 0xDA / 255.0f, 1.0f };
	contexte->OMSetRenderTargets (1, &cible, nullptr);
	contexte->ClearRenderTargetView (cible, fond);
	ImGui_ImplDX11_RenderDrawData (ImGui::GetDrawData ());
	const HRESULT hr = chaine->Present (0, 0);
	ImGui::SetCurrentContext (avant != nullptr ? avant : imgui);

	// carte graphique perdue (pilote mis a jour, veille) : on recree tout
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
	{
		libererRendu ();
		initialiserRendu ();
	}

	fmstation::Action a;
	std::string arg;
	while (station.prendreAction (a, arg))
		executer (a, arg);
}

LRESULT FmView::message (UINT msg, WPARAM w, LPARAM l)
{
	switch (msg)
	{
	case WM_TIMER:
		image ();
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		BeginPaint (hwnd, &ps);
		EndPaint (hwnd, &ps);
		image ();
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_MOUSEMOVE:
		if (!suiviSouris)
		{
			TRACKMOUSEEVENT tme;
			memset (&tme, 0, sizeof (tme));
			tme.cbSize = sizeof (tme);
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = hwnd;
			TrackMouseEvent (&tme);
			suiviSouris = true;
		}
		station.souris ((float)GET_X_LPARAM (l), (float)GET_Y_LPARAM (l));
		return 0;

	case WM_MOUSELEAVE:
		suiviSouris = false;
		station.souris (-1.0f, -1.0f);
		return 0;

	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		SetFocus (hwnd);   // sans le focus, le clavier va a FL
		station.souris ((float)GET_X_LPARAM (l), (float)GET_Y_LPARAM (l));
		station.bouton (true);
		return 0;

	case WM_LBUTTONUP:
		station.bouton (false);
		return 0;

	case WM_SETCURSOR:
		if (LOWORD (l) == HTCLIENT)
		{
			SetCursor (LoadCursorW (nullptr, station.survolCliquable () ? (LPCWSTR)IDC_HAND
			                                                            : (LPCWSTR)IDC_ARROW));
			return TRUE;
		}
		break;

	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

	case WM_KEYDOWN:
	{
		if (GetTickCount64 () - dedoublon < 60)
			return 0;
		const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
		if (ctrl && w == 'V')
		{
			station.coller (lirePressePapiers (hwnd));
			return 0;
		}
		if (station.touche ((int)w, ctrl))
			return 0;
		break;
	}

	case WM_CHAR:
		if (GetTickCount64 () - dedoublon < 60)
			return 0;
		if (station.caractere ((unsigned)w))
			return 0;
		break;

	case WM_DESTROY:
		KillTimer (hwnd, 1);
		return 0;
	}
	return DefWindowProcW (hwnd, msg, w, l);
}

LRESULT CALLBACK procVue (HWND h, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_NCCREATE)
	{
		const CREATESTRUCTW* cs = (const CREATESTRUCTW*)l;
		SetWindowLongPtrW (h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
	}
	FmView* vue = (FmView*)GetWindowLongPtrW (h, GWLP_USERDATA);
	if (vue == nullptr || msg == WM_NCCREATE)
		return DefWindowProcW (h, msg, w, l);
	if (msg == WM_NCDESTROY)
	{
		SetWindowLongPtrW (h, GWLP_USERDATA, 0);
		return DefWindowProcW (h, msg, w, l);
	}
	return vue->message (msg, w, l);
}

} // namespace

// --------------------------------------------------------------------------
namespace fmgui {

void setRole (int role)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_role = role;
	if (role == 0)
		g_pair.clear ();
}

void setStatus (const char* texte)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_statut = (texte != nullptr) ? texte : "";
	g_agent = true;
}

void setAddress (const char* adresse)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_adresse = (adresse != nullptr) ? adresse : "";
}

void setPeer (const char* nom)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_pair = (nom != nullptr && strcmp (nom, "-") != 0) ? nom : "";
}

void setErreur (const char* texte)
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_erreur = (texte != nullptr) ? texte : "";
	g_statut = g_erreur;
}

void soumettreEtat (const char* texte)
{
	setStatus (texte);
}

void agentParti ()
{
	std::lock_guard<std::mutex> g (g_mtx);
	g_role = 0;
	g_pair.clear ();
	g_adresse.clear ();
	g_statut = g_erreur;   // vide, sauf si l'agent a dit pourquoi il part
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

IPlugView* fmCreateView ()
{
	return new FmView ();
}
