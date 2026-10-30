// FruityMulti - plugin VST3 "cheval de Troie".
//
// Objectif de cette premiere version : prouver que notre code s'execute bien
// a l'interieur du processus de FL Studio, sans injecteur externe.
// Il ne traite aucun audio et n'affiche aucune interface.
//
// Preuve : ecriture d'un fichier de log avec l'adresse de base de FLEngine.

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
// inclus en dernier : son BEGIN_FACTORY_DEF doit ecraser celui de pluginfactory.h
#include "public.sdk/source/main/pluginfactory_constexpr.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "fmgui.h"
#include <shellapi.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace Steinberg;
using namespace Steinberg::Vst;

// UID du plugin (doit rester stable d'une version a l'autre) â€” scope global,
// car les macros de fabrique l'utilisent hors de tout namespace.
static DECLARE_UID (FruityMultiBridgeUID, 0x8F3A1C21, 0x4B7E4C11, 0xA9D23E5B, 0x7C6D8E9F);
static constexpr size_t numberOfClasses = 1;

namespace FruityMulti {

static std::string logPath ()
{
	char tempPath[MAX_PATH] = { 0 };
	DWORD n = GetEnvironmentVariableA ("TEMP", tempPath, MAX_PATH);
	if (n == 0)
		return std::string ();
	std::string p (tempPath);
	p += "\\FruityMulti_agent.log";
	return p;
}

static void logLine (const char* fmt, ...)
{
	std::string path = logPath ();
	if (path.empty ())
		return;

	FILE* f = nullptr;
	if (fopen_s (&f, path.c_str (), "a") != 0 || f == nullptr)
		return;

	SYSTEMTIME st;
	GetLocalTime (&st);
	fprintf (f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list ap;
	va_start (ap, fmt);
	vfprintf (f, fmt, ap);
	va_end (ap);

	fprintf (f, "\n");
	fclose (f);
}

//------------------------------------------------------------------------
// Recherche d'une signature d'octets dans l'image d'un module.
// Format : "57 56 53 48 ?? ?? 20" â€” "??" = joker.
// C'est ce qui remplace les adresses fixes : le code peut bouger, le motif reste.
static void* findSignature (HMODULE module, const char* sig)
{
	if (module == nullptr)
		return nullptr;

	const unsigned char* base = (const unsigned char*)module;

	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return nullptr;

	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return nullptr;

	const size_t imageSize = nt->OptionalHeader.SizeOfImage;

	std::vector<unsigned char> bytes;
	std::vector<bool> mask;

	auto hexVal = [] (char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		return -1;
	};

	for (const char* p = sig; *p != 0;)
	{
		if (*p == ' ')
		{
			++p;
			continue;
		}
		if (p[0] == '?' && p[1] == '?')
		{
			bytes.push_back (0);
			mask.push_back (false);
			p += 2;
		}
		else
		{
			int hi = hexVal (p[0]);
			int lo = hexVal (p[1]);
			if (hi < 0 || lo < 0)
				return nullptr;
			bytes.push_back ((unsigned char)((hi << 4) | lo));
			mask.push_back (true);
			p += 2;
		}
	}

	const size_t n = bytes.size ();
	if (n < 8 || n >= imageSize)
		return nullptr;

	for (size_t i = 0; i + n < imageSize; ++i)
	{
		size_t j = 0;
		for (; j < n; ++j)
		{
			if (mask[j] && base[i + j] != bytes[j])
				break;
		}
		if (j == n)
			return (void*)(base + i);
	}
	return nullptr;
}

// Verifie qu'une zone est lisible avant de la dereferencer : evite un plantage
// de FL si une entree de table est perimee.
static bool isReadable (const void* p, size_t n)
{
	if (p == nullptr)
		return false;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery (p, &mbi, sizeof (mbi)) == 0)
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;
	const unsigned char* start = (const unsigned char*)p;
	const unsigned char* regionEnd = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
	return (start + n) <= regionEnd;
}

// Appels proteges au moteur de FL.
//
// Une faute d'acces dans le moteur ne doit pas emporter le thread principal :
// quand c'est le cas, plus aucune commande ne rend de reponse et l'agent reste
// bloque sans le dire (constate le 30/10 sur les ecritures de proprietes).
static unsigned long guardedSet7 (void (*fn) (long long, int, int, int, int, int, int),
                                  long long handle, int valeur, int mode)
{
	__try
	{
		fn (handle, valeur, mode, 0, 0, 0, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode ();
	}
	return 0;
}

// Suppression d'un canal par la fonction de FL elle-meme (RVA 0xC04A60).
//
// Signature reelle : void FUN_01004a60 (unsigned index, char transaction,
//                                          unsigned long long drapeaux)
//
// 46-p : on s'aligne sur ce que fait FL. Le releve des 38 sites d'appel
// (re/ghidra/sites_del.md, re/tools/drapeaux_suppression.py) montre que FL
// appelle TOUJOURS (index, 0, <octet>) :
//   transaction = 0 partout ;
//   drapeaux = 0x00 (majorite), 0x01, 0x02 (famille d'interface 0xD1xxxx),
//              0x06 (Couper, FUN_0101eb30), 0x0E (rack, FUN_0101f730 / 0xC20E00).
// 0x02 = Â« pas de dialogue, aucune action annexe Â» (pas de remise a zero du
// nom / de la couleur / de l'icone, pas de menage de tranche de mixeur).
// L'ancien (index, 1, 2) n'existe nulle part chez FL.
//
// 47-d : avant de supprimer, on ouvre une etape d'historique comme FL
// (FUN_01019360, cree un TUndoEvent_RemovedChannels). Sans elle, le Ctrl+Z
// suivant annule l'ajout d'un canal disparu et FL plante (POUR-CLAUDE, 13.15).
static unsigned long guardedDeleteChannel (void (*fn) (unsigned, int, unsigned long long),
                                           void (*ouvrirEtape) (void*, void*, unsigned),
                                           void* historique, void* nom, unsigned index)
{
	__try
	{
		if (ouvrirEtape != nullptr)
			ouvrirEtape (historique, nom, index);
		fn (index, 0, 0x02);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode ();
	}
	return 0;
}

// Canal entier <-> fichier .fst, avec les fonctions du Ctrl+Z de FL.
//   ecriture  : FUN_00f54570 (chemin, 0x20, canal, 6), via FUN_010159e0 ;
//   insertion : FUN_01004230 (index, 1, 1) ;
//   lecture   : FUN_00f54760 (chemin, 0x20, canal, 0).
// Les deux premiers appels sont ceux de FUN_01020e00, l'annulation de Delete.
using ChannelFileFn = int (*) (void*, unsigned, void*, unsigned char);

static int guardedSaveChannel (ChannelFileFn ecrire, void* chemin, void* channel,
                               unsigned long* faute)
{
	__try
	{
		return ecrire (chemin, 0x20, channel, 0x06);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*faute = GetExceptionCode ();
	}
	return -1;
}

// Bascule PAT/SONG : FUN_00fdd300 (15, 1, drapeau, 15, 0), l'appel exact de
// transport.setLoopMode. FL l'applique juste apres, pas pendant l'appel.
static unsigned long guardedToggleLoopMode (unsigned char* base)
{
	__try
	{
		if (((char (*) ())(base + 0xDD1470)) () == 0)
			return 1;
		const unsigned char drapeau = ((unsigned char (*) ())(base + 0xBDD2B0)) ();
		((int (*) (int, int, unsigned char, int, int))(base + 0xBDD300)) (15, 1, drapeau, 15, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode ();
	}
	return 0;
}

static void* guardedLoadChannel (void* (*inserer) (int, int, int), ChannelFileFn lire,
                                 void* existant, int index, void* chemin, int* rc,
                                 unsigned long* faute)
{
	void* channel = existant;
	__try
	{
		if (channel == nullptr)
			channel = inserer (index, 1, 1);
		if (channel != nullptr)
			*rc = lire (chemin, 0x20, channel, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*faute = GetExceptionCode ();
	}
	return channel;
}

//------------------------------------------------------------------------
// Ecriture depuis le thread principal de FL.
// ---------------------------------------------------------------------

static DWORD g_flMainThreadId = 0;
static void* g_container = nullptr;
static bool  g_testDone = false;
static UINT_PTR g_timerId = 0;

static const char* kInsertSig =
	"57 56 53 48 83 EC 20 48 89 CB 48 89 D6 48 89 D9 48 8D 53 1C 44 8B 06 "
	"E8 ?? ?? ?? ?? 48 89 D9 8B 53 1C 41 B8 01 00 00 00 E8 ?? ?? ?? ??";

struct FindWindowCtx
{
	DWORD pid;
	HWND  best;
};

static BOOL CALLBACK enumWindowsProc (HWND hwnd, LPARAM lp)
{
	FindWindowCtx* ctx = (FindWindowCtx*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId (hwnd, &pid);
	if (pid != ctx->pid)
		return TRUE;

	char title[256] = { 0 };
	GetWindowTextA (hwnd, title, sizeof (title));
	if (strstr (title, "FL Studio") != nullptr)
	{
		ctx->best = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND findFlMainWindow ()
{
	FindWindowCtx ctx { GetCurrentProcessId (), nullptr };
	EnumWindows (enumWindowsProc, (LPARAM)&ctx);
	return ctx.best;
}

// --- Reveil immediat du thread principal ------------------------------------
// Le thread reseau ne touche jamais au modele : il depose la requete et previent
// le thread principal par un message. Sans cela il fallait attendre le tick de
// la minuterie (jusqu'a 15,6 ms) a chaque requete : c'est ce qui faisait 31 ms
// par cycle de synchronisation.
// DESACTIVE le 28/10 a 23h50 apres un crash de FL (defaut dans FLEngine).
// Cause probable : la fenetre etait creee dans initialize(), qui ne tourne pas
// forcement sur le thread principal de FL ; processPending() pouvait donc
// toucher au moteur hors du thread de l'interface.
// Pour le reactiver : creer la fenetre depuis fmTimerProc, qui lui tourne sur
// le thread principal (c'est prouve par tout le reste du plugin).
static const bool kUseWakeWindow = false;
static HWND g_wakeWnd = nullptr;

static void processPending ();          // definie plus bas

static LRESULT CALLBACK fmWakeProc (HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_APP + 1)
	{
		processPending ();
		return 0;
	}
	return DefWindowProcA (h, m, w, l);
}

static void createWakeWindow ()
{
	if (g_wakeWnd != nullptr)
		return;

	WNDCLASSA wc;
	memset (&wc, 0, sizeof (wc));
	wc.lpfnWndProc = fmWakeProc;
	wc.hInstance = GetModuleHandleA (nullptr);
	wc.lpszClassName = "FruityMultiWake";

	if (RegisterClassA (&wc) == 0 && GetLastError () != ERROR_CLASS_ALREADY_EXISTS)
	{
		logLine ("reveil           : RegisterClass echec");
		return;
	}

	g_wakeWnd = CreateWindowExA (0, "FruityMultiWake", "", 0, 0, 0, 0, 0,
	                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	logLine ("reveil           : %p", (void*)g_wakeWnd);
}

static void destroyWakeWindow ()
{
	if (g_wakeWnd != nullptr)
	{
		DestroyWindow (g_wakeWnd);
		g_wakeWnd = nullptr;
	}
}


// --- Autorite : l'hote fait foi ---------------------------------------------
// Role : 0 = pas de session, 1 = hote, 2 = invite.
//
// En session, l'invite ne decide pas du projet : le plugin refuse les commandes
// qui le touchent (TEMPO, PATTERN, PLAY/STOP, SONGPOS) et l'agent le ramene aux
// valeurs de l'hote. Les ordres venus de l'hote, eux, passent : l'agent les
// prefixe par FORCE.
//
// Note du 28/10 au soir : griser les menus de FL est IMPOSSIBLE par Win32 â€”
// FL n'a aucun menu Win32 (GetMenu renvoie 0, et aucun enfant n'en a). La
// fonction fmApplyRoleLock ci-dessous ne trouve donc rien a griser ; elle est
// conservee car elle ne coute rien et redeviendrait utile si FL changeait.
static volatile LONG g_role = 0;
// Un ordre venu de l'hote s'execute meme chez l'invite : l'agent le prefixe
// par FORCE. C'est la difference entre "l'invite change le projet" (refuse)
// et "l'invite applique ce que l'hote a decide" (autorise).
static volatile LONG g_forceRole = 0;

static bool roleBloque ()
{
	if (InterlockedCompareExchange (&g_forceRole, 0, 0) != 0)
		return false;
	return InterlockedCompareExchange (&g_role, 0, 0) == 2;
}

struct FmLockedMenu
{
	HMENU menu;
	UINT id;
};

static std::vector<FmLockedMenu> g_locked;
static DWORD g_lockScanAt = 0;
static DWORD g_lockApplyAt = 0;

// Un libelle de menu est-il reserve a l'hote ?
static bool fmHostOnly (const wchar_t* texte)
{
	if (texte == nullptr)
		return false;

	static const wchar_t* mots[] = {
		L"export", L"render", L"save", L"open", L"revert",
		L"new from template", L"close project", L"close",
		L"manage plugins", L"scan", L"plugin manager", L"project info"
	};

	std::wstring bas (texte);
	for (size_t i = 0; i < bas.size (); i++)
	{
		if (bas[i] >= L'A' && bas[i] <= L'Z')
			bas[i] = (wchar_t)(bas[i] - L'A' + L'a');
	}
	for (size_t m = 0; m < sizeof (mots) / sizeof (mots[0]); m++)
	{
		if (bas.find (mots[m]) != std::wstring::npos)
			return true;
	}
	return false;
}

static void fmScanMenu (HMENU menu, int profondeur)
{
	if (menu == nullptr || profondeur > 4)
		return;

	const int n = GetMenuItemCount (menu);
	for (int i = 0; i < n; i++)
	{
		wchar_t buf[192] = { 0 };
		const int len = GetMenuStringW (menu, i, buf, 191, MF_BYPOSITION);
		if (len > 0 && fmHostOnly (buf))
		{
			const UINT id = GetMenuItemID (menu, i);
			if (id != 0 && id != (UINT)-1)
			{
				FmLockedMenu item { menu, id };
				g_locked.push_back (item);
			}
		}
		HMENU sous = GetSubMenu (menu, i);
		if (sous != nullptr)
			fmScanMenu (sous, profondeur + 1);
	}
}

// Appelee par la minuterie : gratte les menus toutes les 5 s, applique l'etat
// toutes les 250 ms (FL reconstruit ses menus, donc il faut insister).
static void fmApplyRoleLock ()
{
	const DWORD maintenant = GetTickCount ();
	if (maintenant - g_lockApplyAt < 250)
		return;
	g_lockApplyAt = maintenant;

	const int role = (int)InterlockedCompareExchange (&g_role, 0, 0);

	if (g_locked.empty () || maintenant - g_lockScanAt > 5000)
	{
		g_locked.clear ();
		HWND fl = findFlMainWindow ();
		if (fl != nullptr)
			fmScanMenu (GetMenu (fl), 0);
		g_lockScanAt = maintenant;
	}

	if (g_locked.empty ())
		return;

	const UINT drapeau = (role == 2) ? MF_GRAYED : MF_ENABLED;
	for (size_t i = 0; i < g_locked.size (); i++)
		EnableMenuItem (g_locked[i].menu, g_locked[i].id, MF_BYCOMMAND | drapeau);
}


static void processPending ();              // definie plus bas
static void applyClipLength (unsigned, unsigned); // definie plus bas
static unsigned char* getClipContainer ();        // definie plus bas

// Correction de longueur differee : FL recalcule la longueur d'un clip qu'il
// vient d'inserer, donc on la reecrit quelques cycles APRES le rafraichissement.
static volatile LONG g_clipFixPending = 0;
static int g_clipFixWait = 0;
static unsigned g_clipFixPos = 0;
static unsigned g_clipFixLen = 0;
static void forceRefresh ();   // definie plus bas
static DWORD WINAPI serverThread (LPVOID);
static volatile LONG g_refreshPending = 0;
static volatile LONG g_disabled = 0;   // version de FL non reconnue
static volatile LONG g_serverWanted = 0;
static volatile LONG g_serverAlive = 0;
static volatile LONG g_serverStarting = 0;
static HANDLE g_serverThread = nullptr;

// Chargement de projet demande par LOADPROJECT.
//
// Il n'est PAS execute dans la commande : quand le projet est remplace, FL
// decharge notre plugin, donc revenir dans une pile de commande apres coup
// signifie revenir dans du code libere (plantage verifie le 30/10 : violation
// d'acces dans FruityMultiBridge.vst3, offset 0x18baa).
//
// La commande se contente donc de poser le chemin ; la minuterie du thread
// principal execute le chargement a la periode suivante, hors de toute requete.
static char g_loadPath[400] = { 0 };
static volatile LONG g_loadPending = 0;
static void doLoadProject ();

// Pose par terminate() quand l instance du plugin est detruite. Permet de savoir,
// au retour du chargeur, si on doit relancer nos threads ou si le projet a
// remplace l instance (auquel cas il ne faut plus rien toucher).
static volatile LONG g_instanceDead = 0;

// Dernier resultat du chargeur de projet : -2 jamais charge, -1 en cours,
// sinon le code rendu par FL (0 = charge, autre = echec).
//
// Cette valeur doit SURVIVRE au remplacement de l instance : un chargement
// reussi detruit le plugin, et c est la NOUVELLE instance qui doit lire le
// resultat. D ou une variable statique du module, et le module est epingle
// donc jamais demappe.
static volatile LONG g_loadStatus = -2;


// Relance le serveur s'il est tombe. FL peut appeler terminate() dans
// plusieurs situations sans nous rendre la main ; plutot que de deviner
// lesquelles, on surveille et on repare.
static void ensureServer ()
{
	if (InterlockedCompareExchange (&g_serverWanted, 1, 1) == 0)
		return;
	if (InterlockedCompareExchange (&g_serverAlive, 1, 1) != 0)
		return;
	if (InterlockedExchange (&g_serverStarting, 1) != 0)
		return;

	HANDLE t = CreateThread (nullptr, 0, serverThread, nullptr, 0, nullptr);
	if (t != nullptr)
		g_serverThread = t;   // conserve pour pouvoir attendre son arret

	InterlockedExchange (&g_serverStarting, 0);
}

static VOID CALLBACK fmTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
	// Chargement de projet demande : on l'execute ici, hors de toute commande.
	if (InterlockedExchange (&g_loadPending, 0) != 0)
	{
		doLoadProject ();
		return;   // apres un chargement, plus rien de sur a toucher
	}

	// S'execute sur le thread principal de FL : c'est ici, et seulement ici,
	// qu'on a le droit de toucher au modele.
	processPending ();

	// Un seul saut de pattern par periode, meme si plusieurs notes sont arrivees.
	if (InterlockedExchange (&g_refreshPending, 0) != 0)
		forceRefresh ();

	// correction de longueur differee, apres que FL a recalcule
	if (InterlockedCompareExchange (&g_clipFixPending, 0, 0) != 0)
	{
		if (g_clipFixWait > 0)
			--g_clipFixWait;
		else
		{
			applyClipLength (g_clipFixPos, g_clipFixLen);
			InterlockedExchange (&g_clipFixPending, 0);
		}
	}

	// surveille le serveur et le relance s'il est tombe
	ensureServer ();

	// grise (ou rend) les actions reservees a l'hote selon notre role
	fmApplyRoleLock ();
}

//------------------------------------------------------------------------
// Serveur TCP local.
//
// Le thread reseau ne touche JAMAIS au modele de FL : il depose une requete
// dans une file, et c'est la minuterie â€” qui s'execute sur le thread principal
// de FL â€” qui la traite. C'est la regle d'or : tout appel dans FL vient du
// thread principal.
// ---------------------------------------------------------------------

static HMODULE g_engine = nullptr;
static void*   g_insertFn = nullptr;
static SOCKET  g_srv = INVALID_SOCKET;
static volatile LONG g_stopServer = 0;

// Partagee entre le thread serveur et le thread principal : une requete
// expiree (TIMEOUT) peut encore etre executee plus tard sans ecrire dans une
// pile qui a servi a la requete suivante.
struct Request
{
	std::string cmd;
	std::string reply;
	std::atomic<bool> done { false };
};

static std::mutex g_mtx;
static std::vector<std::shared_ptr<Request>> g_pending;

static BOOL CALLBACK invalidateProc (HWND hwnd, LPARAM lp)
{
	DWORD pid = 0;
	GetWindowThreadProcessId (hwnd, &pid);
	if (pid != GetCurrentProcessId ())
		return TRUE;

	// Invalidation ASYNCHRONE uniquement.
	// RDW_UPDATENOW provoque un repaint synchrone : appele depuis la minuterie,
	// il fait rentrer FL dans sa propre boucle de peinture et le fait planter.
	InvalidateRect (hwnd, nullptr, FALSE);
	RedrawWindow (hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);

	int* count = (int*)lp;
	(*count)++;
	return TRUE;
}

// Rafraichissement differe.
//
// Le saut de pattern est le seul moyen connu de faire relire le modele aux vues
// de FL, mais il touche le moteur audio : repete a chaque note, il produit des
// micro-clics. On le regroupe donc : une demande pose un drapeau, et la minuterie
// (thread principal, toutes les 20 ms) n'execute qu'UN saut par periode.
// Le drapeau g_refreshPending est declare plus haut, avant la minuterie.

static void requestRefresh ()
{
	InterlockedExchange (&g_refreshPending, 1);
}

// Force les vues de FL a relire le pattern.
//
// FL ne repeint pas quand on modifie le modele en memoire : il ne le fait
// que sur un vrai changement de pattern. On reproduit donc ce que fait
// l'utilisateur a la main â€” passer a un autre pattern puis revenir â€” en
// deux appels immediats.
static void forceRefresh ()
{
	if (g_engine == nullptr)
		return;

	const int current = *(const int*)((const unsigned char*)g_engine + 0x11D51A0);
	if (current < 1 || current > 999)
		return;

	using PatternFn = void (*) (int);
	PatternFn setPattern = (PatternFn)((unsigned char*)g_engine + 0xED8B30);

	const int other = (current == 1) ? 2 : 1;
	setPattern (other);
	setPattern (current);
}

//------------------------------------------------------------------------
// Curseur distant.
//
// On ne dessine PAS dans les fenetres de FL : on cree une petite fenetre
// superposee, transparente (cle de couleur) et traversable par les clics.
// Elle suit la position envoyee par le pair.
// ---------------------------------------------------------------------

static HWND g_cursorWnd = nullptr;
static HWND g_flWindow = nullptr;
static volatile LONG g_remoteX = 0;
static volatile LONG g_remoteY = 0;
static char g_remoteLabel[64] = "peer";
static volatile LONG g_cursorThreadId = 0;
static HANDLE g_cursorThread = nullptr;

#define FM_KEY_COLOR RGB(255, 0, 255)

// Nombre de tranches de mixage suivies. FL en propose bien plus, mais chaque
// tranche coute deux nombres dans STATUS a chaque cycle.
#define FM_MIX_TRACKS 16

static LRESULT CALLBACK cursorWndProc (HWND h, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_CLOSE)
	{
		DestroyWindow (h);
		return 0;
	}
	if (msg == WM_DESTROY)
	{
		PostQuitMessage (0);
		return 0;
	}
	if (msg == WM_PAINT)
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint (h, &ps);

		RECT rc;
		GetClientRect (h, &rc);
		HBRUSH bg = CreateSolidBrush (FM_KEY_COLOR);
		FillRect (dc, &rc, bg);
		DeleteObject (bg);

		// petite fleche orange
		POINT pts[3] = { { 4, 4 }, { 4, 34 }, { 14, 24 } };
		HBRUSH fill = CreateSolidBrush (RGB (255, 90, 0));
		HPEN edge = CreatePen (PS_SOLID, 2, RGB (0, 0, 0));
		HGDIOBJ oldBrush = SelectObject (dc, fill);
		HGDIOBJ oldPen = SelectObject (dc, edge);
		Polygon (dc, pts, 3);
		SelectObject (dc, oldBrush);
		SelectObject (dc, oldPen);
		DeleteObject (fill);
		DeleteObject (edge);

		// L'etiquette du pair a ete retiree le 29/10 (demande) : on ne garde
		// que la fleche orange, qui suffit a montrer ou l'autre a son curseur.

		EndPaint (h, &ps);
		return 0;
	}
	return DefWindowProc (h, msg, w, l);
}

// Bouton flottant "FruityMulti" : retire le 29/10 (demande).
static const bool kShowToolbar = false;

static HWND g_barWnd = nullptr;
static volatile LONG g_barThreadId = 0;
static HANDLE g_barThread = nullptr;

static bool agentPath (char* sortie, size_t taille)
{
	const char* local = getenv ("LOCALAPPDATA");
	if (local != nullptr)
	{
		sprintf_s (sortie, taille, "%s\\FruityMulti\\fm_agent.exe", local);
		if (GetFileAttributesA (sortie) != INVALID_FILE_ATTRIBUTES)
			return true;
	}
	sortie[0] = 0;
	return false;
}

static std::string clipboardText ()
{
	std::string resultat;
	if (!OpenClipboard (g_barWnd))
		return resultat;
	HANDLE donnees = GetClipboardData (CF_TEXT);
	if (donnees != nullptr)
	{
		const char* texte = (const char*)GlobalLock (donnees);
		if (texte != nullptr)
		{
			resultat = texte;
			GlobalUnlock (donnees);
		}
	}
	CloseClipboard ();
	while (!resultat.empty () && (resultat.back () == '\n' || resultat.back () == '\r'))
		resultat.pop_back ();
	return resultat;
}

static void launchAgent (const char* arguments)
{
	char agent[MAX_PATH];
	if (!agentPath (agent, sizeof (agent)))
	{
		MessageBoxA (g_barWnd,
			"fm_agent.exe introuvable.\n\n"
			"Lance une fois 1-Installer-plugin.bat : il place l'agent au bon endroit.",
			"FruityMulti", MB_OK | MB_ICONWARNING);
		return;
	}

	SHELLEXECUTEINFOA info;
	memset (&info, 0, sizeof (info));
	info.cbSize = sizeof (info);
	info.lpVerb = "open";
	info.lpFile = agent;
	info.lpParameters = arguments;
	info.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExA (&info))
		MessageBoxA (g_barWnd, "Impossible de lancer l'agent.",
		             "FruityMulti", MB_OK | MB_ICONERROR);
}

static void barMenu ()
{
	POINT p;
	GetCursorPos (&p);

	HMENU menu = CreatePopupMenu ();
	AppendMenuA (menu, MF_STRING, 1, "Heberger la session");
	AppendMenuA (menu, MF_STRING, 2, "Rejoindre (adresse du presse-papier)");
	AppendMenuA (menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuA (menu, MF_STRING, 3, "A propos");

	const int choix = (int)TrackPopupMenu (menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
	                                       p.x, p.y, 0, g_barWnd, nullptr);
	DestroyMenu (menu);

	if (choix == 1)
		launchAgent ("--listen 5000");
	else if (choix == 2)
	{
		const std::string adresse = clipboardText ();
		if (adresse.empty ())
		{
			MessageBoxA (g_barWnd,
				"Copie d'abord l'adresse de l'hote dans le presse-papier\n"
				"(par exemple 203.0.113.10:50000, adresse d'exemple), puis reclique.",
				"FruityMulti", MB_OK | MB_ICONINFORMATION);
			return;
		}
		const std::string arguments = "--peer " + adresse;
		launchAgent (arguments.c_str ());
	}
	else if (choix == 3)
		MessageBoxA (g_barWnd,
			"FruityMulti\n\n"
			"Session FL Studio partagee entre deux postes.\n"
			"Heberger : ouvre la session et affiche l'adresse.\n"
			"Rejoindre : lit l'adresse dans le presse-papier.",
			"FruityMulti", MB_OK | MB_ICONINFORMATION);
}

static void positionBar ();   // definie plus bas

static LRESULT CALLBACK barWndProc (HWND h, UINT msg, WPARAM w, LPARAM l)
{
	switch (msg)
	{
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint (h, &ps);
		RECT rc;
		GetClientRect (h, &rc);

		HBRUSH fond = CreateSolidBrush (RGB (48, 48, 52));
		FillRect (dc, &rc, fond);
		DeleteObject (fond);

		SetBkMode (dc, TRANSPARENT);
		SetTextColor (dc, RGB (235, 235, 235));
		SelectObject (dc, (HFONT)GetStockObject (DEFAULT_GUI_FONT));
		DrawTextA (dc, "FruityMulti", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		EndPaint (h, &ps);
		return 0;
	}
	case WM_TIMER:
		positionBar ();
		return 0;
	case WM_LBUTTONUP:
		barMenu ();
		return 0;
	}
	return DefWindowProc (h, msg, w, l);
}

static void positionBar ()
{
	if (g_barWnd == nullptr || g_flWindow == nullptr)
		return;

	// Fenetre INDEPENDANTE (pas enfant) : coordonnees ecran, recalculees
	// periodiquement pour suivre FL. Cela evite le melange client/ecran qui
	// envoyait le bouton dans la barre des taches.
	RECT fl;
	if (!GetWindowRect (g_flWindow, &fl))
		return;
	if (fl.right - fl.left < 600)
		return;

	const int x = fl.right - 400;
	const int y = fl.top + 44;

	SetWindowPos (g_barWnd, HWND_TOPMOST, x, y, 96, 22,
	              SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static DWORD WINAPI barThread (LPVOID)
{
	InterlockedExchange (&g_barThreadId, (LONG)GetCurrentThreadId ());

	WNDCLASSA wc;
	memset (&wc, 0, sizeof (wc));
	wc.lpfnWndProc = barWndProc;
	wc.hInstance = GetModuleHandleA (nullptr);
	wc.lpszClassName = "FruityMultiBar";
	wc.hCursor = LoadCursor (nullptr, IDC_ARROW);
	if (RegisterClassA (&wc) == 0 && GetLastError () != ERROR_CLASS_ALREADY_EXISTS)
		return 0;

	g_barWnd = CreateWindowExA (
		WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
		"FruityMultiBar", "FruityMulti", WS_POPUP,
		0, 0, 96, 22, nullptr, nullptr, wc.hInstance, nullptr);

	if (g_barWnd == nullptr)
		return 0;

	positionBar ();
	SetTimer (g_barWnd, 1, 500, nullptr);   // suit les deplacements de FL

	MSG msg;
	while (GetMessageA (&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage (&msg);
		DispatchMessageA (&msg);
	}
	return 0;
}

static DWORD WINAPI cursorThread (LPVOID)
{
	InterlockedExchange (&g_cursorThreadId, (LONG)GetCurrentThreadId ());

	WNDCLASSA wc;
	memset (&wc, 0, sizeof (wc));
	wc.lpfnWndProc = cursorWndProc;
	wc.hInstance = GetModuleHandleA (nullptr);
	wc.lpszClassName = "FruityMultiCursor";
	wc.hCursor = nullptr;
	if (RegisterClassA (&wc) == 0 && GetLastError () != ERROR_CLASS_ALREADY_EXISTS)
		return 0;

	g_cursorWnd = CreateWindowExA (
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		"FruityMultiCursor", "", WS_POPUP,
		0, 0, 160, 64, nullptr, nullptr, wc.hInstance, nullptr);

	if (g_cursorWnd == nullptr)
		return 0;

	SetLayeredWindowAttributes (g_cursorWnd, FM_KEY_COLOR, 0, LWA_COLORKEY);
	ShowWindow (g_cursorWnd, SW_SHOWNOACTIVATE);

	MSG msg;
	while (GetMessageA (&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage (&msg);
		DispatchMessageA (&msg);
	}
	return 0;
}

// --- Notes -------------------------------------------------------------------
// Verifie le 28/10 a 20h15, en direct :
//   la table FLEngine+0x153FCE0 n'est pas un tableau de canaux mais de MOTIFS.
//   Un motif n (numero a partir de 1) occupe 0xC0 octets ; sa liste de notes
//   est a +0x28. Le motif courant se lit par le pointeur +0x11E4018.
//   Chaque note (24 octets) porte son CANAL en +0x06 : une seule liste contient
//   les notes de tous les canaux, et le piano roll filtre a l'affichage.
// C'est ce qui manquait : on ecrivait toujours canal 0.
static int currentPattern ()
{
	if (g_engine == nullptr)
		return 0;
	const int* slot = *(const int* const*)((const unsigned char*)g_engine + 0x11E4018);
	if (slot == nullptr || !isReadable (slot, sizeof (int)))
		return 0;
	return *slot;
}

static unsigned char* noteList (int pattern)
{
	if (g_engine == nullptr || pattern <= 0)
		return nullptr;
	unsigned char** slot = (unsigned char**)((unsigned char*)g_engine + 0x153FCE0
	                                          + (size_t)pattern * 0xC0 + 0x28);
	if (!isReadable (slot, 8))
		return nullptr;
	unsigned char* list = *slot;
	if (!isReadable (list, 0x20))
		return nullptr;
	if (*(const int*)(list + 0x10) != 24)
		return nullptr;
	return list;
}

// Nombre de canaux du channel rack, ou -1 si illisible.
// Sert a refuser une note dont le canal n'existe pas : ecrire un canal
// inexistant dans FL le fait planter.
static int channelCount ()
{
	if (g_engine == nullptr)
		return -1;

	const unsigned char* p1 =
		*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E0128);
	if (!isReadable (p1, 8))
		return -1;
	const unsigned char* container = *(const unsigned char* const*)p1;
	if (!isReadable (container, 0x18))
		return -1;

	const int count = *(const int*)(container + 0x10);
	if (count < 0 || count > 2000)
		return -1;
	return count;
}

static int noteChannelOf (const unsigned char* note)
{
	return (int)*(const unsigned short*)(note + 0x06);
}

static void fillNote (unsigned char* note, unsigned channel, unsigned t, unsigned l,
                      unsigned p, unsigned v)
{
	memset (note, 0, 24);
	note[0x05] = 0x40;
	*(unsigned short*)(note + 0x06) = (unsigned short)channel;
	*(unsigned*)(note + 0x00) = t;
	*(unsigned*)(note + 0x08) = l;
	*(unsigned*)(note + 0x0C) = p;
	note[0x10] = 0x78;
	note[0x12] = 0x40;
	note[0x14] = 0x40;
	note[0x15] = (unsigned char)(v > 127 ? 127 : v);
	note[0x16] = 0x80;
	note[0x17] = 0x80;
}

// --- Effets du mixeur --------------------------------------------------------
// Structure verifiee le 28/10 a 21h, en direct :
//   tableau des pistes = *(void**)(FLEngine + 0x11E06A0), pas de 0x1478
//   slot               = *(void**)(piste + 0x134C + slot*8)
//   slot + 0x58 : nom du plugin (UTF-16)   "Fruity Reeverb 2"
//   slot + 0x88 : DLL du plugin
//   slot + 0xE8 : preset .fst charge dans le slot
// Un slot vide a son +0x58 nul : c'est le test de presence.
static unsigned char* mixerTrack (int track)
{
	if (g_engine == nullptr || track < 0 || track > 125)
		return nullptr;
	void** table = (void**)((unsigned char*)g_engine + 0x11E06A0);
	if (!isReadable (table, 8))
		return nullptr;
	unsigned char* base = (unsigned char*)*table;
	if (base == nullptr)
		return nullptr;
	unsigned char* trackObj = base + (size_t)track * 0x1478;
	if (!isReadable (trackObj, 0x100))
		return nullptr;
	return trackObj;
}

static unsigned char* mixerSlot (int track, int slot)
{
	if (slot < 0 || slot > 63)
		return nullptr;
	unsigned char* trackObj = mixerTrack (track);
	if (trackObj == nullptr)
		return nullptr;
	unsigned char** slotPtr = (unsigned char**)(trackObj + 0x134C + (size_t)slot * 8);
	if (!isReadable (slotPtr, 8))
		return nullptr;
	unsigned char* s = *slotPtr;
	if (s == nullptr || !isReadable (s, 0x100))
		return nullptr;
	return s;
}

// Chaine UTF-16 pointee par l'objet a l'offset donne.
static std::string widePtr (const unsigned char* obj, size_t off, int maxChars)
{
	std::string out;
	if (obj == nullptr || !isReadable (obj + off, 8))
		return out;
	const unsigned short* text = *(const unsigned short* const*)(obj + off);
	if (text == nullptr || !isReadable (text, 2))
		return out;
	for (int i = 0; i < maxChars; i++)
	{
		if (!isReadable (text + i, 2))
			break;
		const unsigned short code = text[i];
		if (code == 0)
			break;
		// On ne touche pas au ':' : il fait partie des chemins ("C:\...").
		if (code == '|' || code == '\n' || code == '\r')
			out += ' ';
		else if (code < 0x80)
			out += (char)code;
		else
			out += '?';
	}
	return out;
}

static std::string slotName (const unsigned char* s)
{
	// +0x58 porte le nom affiche. Pour un VST, FL met "Fruity Wrapper" (le
	// conteneur) et le vrai nom est en +0x50 : on prefere celui-la.
	const std::string affiche = widePtr (s, 0x58, 96);
	const std::string interne = widePtr (s, 0x50, 96);
	std::string nom = (!interne.empty () && (affiche.empty () || affiche == "Fruity Wrapper"))
		? interne : affiche;
	if (nom.empty ())
		nom = interne;
	// Le nom sert de jeton dans FXALL, ou ':' separe la place du nom.
	for (size_t i = 0; i < nom.size (); i++)
		if (nom[i] == ':')
			nom[i] = ' ';
	return nom;
}

static std::string executeCommand (const std::string& line)
{
	// FORCE <commande> : execute sans tenir compte du role (ordre de l'hote).
	if (line.rfind ("FORCE ", 0) == 0)
	{
		InterlockedExchange (&g_forceRole, 1);
		std::string r = executeCommand (line.substr (6));
		InterlockedExchange (&g_forceRole, 0);
		return r;
	}

	// Plugin desactive (FL d'une autre version) : la fenetre doit pouvoir le
	// dire, donc ce qui ne parle qu'a la fenetre reste permis.
	if (InterlockedCompareExchange (&g_disabled, 0, 0) != 0
	    && line.rfind ("STATE", 0) != 0 && line != "VERSION" && line != "CHOIX")
		return "ERR version de FL non reconnue par le plugin";

	if (line == "PING")
		return "OK";

	// L'agent annonce son etat a la fenetre : "STATE en attenteâ€¦"
	if (line.rfind ("STATE", 0) == 0)
	{
		std::string quoi = line.size () > 6 ? line.substr (6) : std::string ();
		if (quoi.rfind ("ROLE ", 0) == 0)
		{
			const std::string r = quoi.substr (5);
			fmgui::setRole (r == "hote" ? 1 : (r == "invite" ? 2 : 0));
		}
		else if (quoi.rfind ("ADRESSE ", 0) == 0)
		{
			fmgui::setAddress (quoi.substr (8).c_str ());
		}
		else if (quoi.rfind ("PAIR ", 0) == 0)
		{
			fmgui::setPeer (quoi.substr (5).c_str ());
		}
		else if (quoi.rfind ("ERREUR ", 0) == 0)
		{
			fmgui::setErreur (quoi.substr (7).c_str ());
		}
		else if (quoi.rfind ("DEMANDE ", 0) == 0)
		{
			fmgui::setDemande (quoi.substr (8).c_str ());
		}
		else
		{
			fmgui::setStatus (quoi.c_str ());
		}
		return "STATE OK";
	}
	// Reponse a la question posee par STATE DEMANDE : oui, non, ou "-" tant que
	// l'utilisateur n'a pas choisi. Lire efface la reponse.
	if (line == "CHOIX")
		return "CHOIX " + fmgui::prendreChoix ();

	// Identifiant de build : permet de savoir quelle version tourne
	// chez chacun, sans ambiguite. Meme numero que AGENT_VERSION et
	// fmgui::kVersionPlugin.
	if (line == "VERSION")
		return "VERSION 47-d";

	// Role dans la session, annonce par l'agent :
	//   ROLE hote | ROLE invite | ROLE aucun     (sans argument : on repond)
	// En invite, les actions reservees a l'hote sont grisees dans les menus.
	if (line.rfind ("ROLE", 0) == 0)
	{
		std::istringstream ss (line.substr (4));
		std::string quoi;
		ss >> quoi;

		if (quoi == "hote" || quoi == "host")
			InterlockedExchange (&g_role, 1);
		else if (quoi == "invite" || quoi == "guest")
			InterlockedExchange (&g_role, 2);
		else if (quoi == "aucun" || quoi == "none")
			InterlockedExchange (&g_role, 0);

		const int role = (int)InterlockedCompareExchange (&g_role, 0, 0);
		const std::string nom = role == 2 ? "invite" : (role == 1 ? "hote" : "aucun");

		g_lockApplyAt = 0;      // application immediate, sans attendre la minuterie
		fmApplyRoleLock ();

		return "ROLE " + nom + " (" + std::to_string ((int)g_locked.size ())
		     + " action(s) reservee(s) a l'hote)";
	}

	if (line == "REFRESH")
	{
		int count = 0;
		EnumWindows (invalidateProc, (LPARAM)&count);
		return "REFRESHED " + std::to_string (count);
	}

	// Sonde memoire, pour le reverse : PEEK <rva_hex> [longueur]
	//
	// L'adresse est une RVA (base du module exclue) : elle ne change pas d'un
	// lancement a l'autre, contrairement a l'adresse absolue. Lecture seule, et
	// refusee si la zone n'est pas lisible : on ne touche jamais a la memoire.
	//
	// Reponse sur UNE ligne : "PEEK <adresse absolue> <longueur> <hexa>".
	if (line.rfind ("PEEK ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned long long rva = 0;
		int len = 64;
		if (sscanf_s (line.c_str () + 5, "%llx %d", &rva, &len) < 1)
			return "ERR syntaxe PEEK <rva_hex> [longueur]";
		if (len < 1 || len > 1024)
			return "ERR longueur 1..1024";

		const unsigned char* p = (const unsigned char*)g_engine + (size_t)rva;
		if (!isReadable (p, (size_t)len))
			return "ERR zone illisible";

		std::ostringstream out;
		out << "PEEK " << (const void*)p << " " << len << " ";
		char buf[4];
		for (int i = 0; i < len; ++i)
		{
			sprintf_s (buf, "%02x", p[i]);
			out << buf;
		}
		return out.str ();
	}

	// Suit un pointeur : PEEKPTR <rva_hex>
	if (line.rfind ("PEEKPTR ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned long long rva = 0;
		if (sscanf_s (line.c_str () + 8, "%llx", &rva) != 1)
			return "ERR syntaxe PEEKPTR <rva_hex>";

		const unsigned char* slot = (const unsigned char*)g_engine + (size_t)rva;
		if (!isReadable (slot, 8))
			return "ERR zone illisible";

		const unsigned char* cible = *(const unsigned char* const*)slot;
		std::ostringstream out;
		out << "PEEKPTR " << (const void*)cible;
		if (cible == nullptr || !isReadable (cible, 32))
			return out.str () + " (non lisible)";

		out << " ";
		char buf[4];
		for (int i = 0; i < 32; ++i)
		{
			sprintf_s (buf, "%02x", cible[i]);
			out << buf;
		}
		return out.str ();
	}

	// Sonde de propriete : essayer les deux fonctions d'ecriture de FL.
	//
	//   RAWSET      <mode_hex> <handle_hex> <brut>   -> FLEngine+0xC51D60 (7 args)
	//   RAWDISPATCH <mode_hex> <handle_hex> <brut>   -> FLEngine+0xC46A70 (3 args)
	//   RAWGET      <mode_hex> <handle_hex>          -> FLEngine+0xC51340 (lecture)
	//
	// Constat du 30/10 : 0xC51D60 ne rend jamais la main et n'applique rien ;
	// 0xC46A70 ecrit correctement. C'est ce qui a fait router toutes les
	// ecritures de proprietes par le repartiteur.
	if (line.rfind ("RAWSET ", 0) == 0 || line.rfind ("RAWDISPATCH ", 0) == 0
	    || line.rfind ("RAWGET ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const bool isSet = (line.rfind ("RAWSET ", 0) == 0);
		const bool isDispatch = (line.rfind ("RAWDISPATCH ", 0) == 0);
		const size_t saut = isSet ? 7 : (isDispatch ? 12 : 7);

		unsigned mode = 0;
		unsigned long long handle = 0;
		int brut = 0;
		const int lus = sscanf_s (line.c_str () + saut, "%x %llx %d",
		                          &mode, &handle, &brut);

		if (isSet || isDispatch)
		{
			if (lus < 3)
				return "ERR syntaxe RAWSET|RAWDISPATCH <mode_hex> <handle_hex> <brut>";

			const unsigned long long debut = GetTickCount64 ();
			unsigned long faute = 0;

			if (isSet)
			{
				using Set7Fn = void (*) (long long, int, int, int, int, int, int);
				Set7Fn fn = (Set7Fn)((unsigned char*)g_engine + 0xC51D60);
				faute = guardedSet7 (fn, (long long)handle, brut, (int)mode);
			}
			else
			{
				using DispatchFn = int (*) (int, int, int);
				DispatchFn fn = (DispatchFn)((unsigned char*)g_engine + 0xC46A70);
				fn ((int)handle, brut, (int)mode);
			}

			const unsigned long long duree = GetTickCount64 () - debut;
			std::ostringstream out;
			out << (isSet ? "RAWSET" : "RAWDISPATCH") << " mode=0x" << std::hex << mode
			    << std::dec << " handle=0x" << std::hex << handle << std::dec
			    << " brut=" << brut << " ms=" << duree;
			if (faute != 0)
				out << " FAUTE=0x" << std::hex << faute << std::dec;
			else
				out << " rendu";
			requestRefresh ();
			return out.str ();
		}

		if (lus < 2)
			return "ERR syntaxe RAWGET <mode_hex> <handle_hex>";

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int valeur = getter ((long long)handle, 0x7FFFFFFF, 1);

		std::ostringstream out;
		out << "RAWGET mode=0x" << std::hex << mode << std::dec
		    << " handle=0x" << std::hex << handle << std::dec
		    << " brut=" << valeur;
		return out.str ();
	}

	// Liste des motifs qui existent.
	//
	// Un motif existe si le pointeur de liste a +0x28 n'est pas nul. Les trous
	// sont normaux : verifie le 30/10, les motifs 1, 2, 3 et 5 existaient et le
	// 4 non (il a ete cree en selectionnant directement le motif 5). Il n'y a
	// donc pas de "nombre de motifs" a lire : c'est le nombre d'entrees non
	// nulles de la table.
	//
	// Chemin du projet courant.
	//
	//   PROJECTPATH  ->  PROJECTPATH <chemin absolu>   ou   PROJECTPATH -
	//
	// Adresse obtenue en decompilant le gestionnaire Â« Enregistrer Â»
	// (FUN_011d0f40, RVA 0xDD0F40) : il passe DAT_016bca50 comme chemin aux
	// trois branches d'enregistrement. C'est donc la variable du projet courant.
	if (line == "PROJECTPATH")
	{
		// DESACTIVEE le 30/10 : elle repondait Â« - Â» meme sur un projet
		// enregistre et nomme (verifie sur C:\Temp\Project_1\Project_1.flp,
		// 47130 o sur disque, globale 0x12BCA50 = 0). Cette globale n'est pas
		// le chemin du projet : c'est un parametre transitoire de la commande
		// d'enregistrement. On repond ERR pour que personne ne s'y fie.
		// Utiliser SAVEPROJECT, qui ecrit a un chemin choisi par l'appelant.
		return "ERR PROJECTPATH non fiable : utiliser SAVEPROJECT";

		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const void* const* slot =
			(const void* const*)((const unsigned char*)g_engine + 0x12BCA50);
		if (!isReadable (slot, 8))
			return "ERR chemin illisible";

		const wchar_t* p = (const wchar_t*)*slot;
		if (p == nullptr)
			return "PROJECTPATH -";

		std::wstring w;
		for (int k = 0; k < 400; ++k)
		{
			if (!isReadable (p + k, sizeof (wchar_t)))
				break;
			if (p[k] == 0)
				break;
			w.push_back (p[k]);
		}
		if (w.empty ())
			return "PROJECTPATH -";

		char buf[2048] = { 0 };
		const int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (),
		                                   buf, (int)sizeof (buf) - 1, nullptr, nullptr);
		return "PROJECTPATH " + std::string (buf, n > 0 ? (size_t)n : 0);
	}

	// Ecrit le projet courant dans un fichier donne.
	//
	//   SAVEPROJECT <chemin absolu>
	//
	// Recopie exactement ce que fait Â« Enregistrer Â», decompile dans
	// FUN_011d0f40 (RVA 0xDD0F40) :
	//
	//   FUN_00f54570(DAT_016bca50, 0x30, DAT_016bca60, DAT_011d10bc)  Enregistrer
	//   FUN_00f54570(DAT_016bca50, 0x40, DAT_016bca60, DAT_011d10bc)  Enregistrer sous
	//                  chemin       mode   projet        options
	//
	// On relit les deux globales a l'execution plutot que de les coder en dur :
	// elles appartiennent a FL, pas a nous. L'octet d'options vaut 0x00 : FL
	// ecrit Â« <chemin>~ Â» puis le renomme en Â« <chemin> Â». On n'y touche pas.
	//
	// Contrairement a LOADPROJECT, ceci ne remplace PAS le projet : le plugin
	// survit, donc la commande peut repondre.
	if (line.rfind ("SAVEPROJECT ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const std::string chemin = line.substr (12);
		if (chemin.size () < 5 || chemin.size () > 380)
			return "ERR syntaxe SAVEPROJECT <chemin absolu>";

		using ToDelphi = void* (*) (void*, const char*);
		ToDelphi toDelphi = (ToDelphi)((unsigned char*)g_engine + 0x1C6E0);

		// Le chemin est utilise TEL QUEL, en UTF-8 : FL le convertit lui-meme
		// (FLEngine + 0x1C6E0). Verifie le 30/10 : Â« C:\Temp\Ã©\x.flp Â» passe en
		// UTF-8 brut (rc=0, fichier cree) et echoue si on le convertit en ANSI.
		// Avec options 0, FL ecrit Â« <chemin>~ Â» puis le renomme en Â« <chemin> Â».
		void* chaine = nullptr;
		toDelphi (&chaine, chemin.c_str ());
		if (chaine == nullptr)
			return "ERR chaine chemin";

		// globales de FL, relues a chaque appel
		const unsigned long long projet =
			*(const unsigned long long*)((const unsigned char*)g_engine + 0x12BCA60);
		unsigned char options = 0;
		const unsigned char* optSlot =
			(const unsigned char*)g_engine + 0xDD10BC;
		if (isReadable (optSlot, 1))
			options = *optSlot;   // 0x00 chez FL : ajoute .flp, ecrit, puis renomme

		// Arguments REELS de FL, captures en interceptant cet ecrivain pendant un
		// Ctrl+S le 30/10 a 15h : mode = 0, projet = 0, options = 0x20.
		// Avec eux le fichier fait 47182 o, exactement la taille de celui que FL
		// ecrit lui-meme. Avec 0x40 / options 0 on n'obtenait qu'un squelette de
		// 1122 o, que FL refusait ensuite de recharger (rc=5).
		using SaveFn = int (*) (void*, int, unsigned long long, unsigned char);
		SaveFn save = (SaveFn)((unsigned char*)g_engine + 0xB54570);
		const int rc = save (chaine, 0, projet, 0x20);

		if (rc != 0)
			return "ERR SAVEPROJECT code " + std::to_string (rc);

		// Taille du fichier ecrit. En WIDE : avec une API ANSI, un chemin
		// accentue renvoyait 0 alors que le fichier existait (verifie le 30/10).
		WIN32_FILE_ATTRIBUTE_DATA infos;
		unsigned long long octets = 0;
		std::wstring large;
		{
			const int n = MultiByteToWideChar (CP_UTF8, 0, chemin.c_str (),
			                                   (int)chemin.size (), nullptr, 0);
			if (n > 0)
			{
				large.resize ((size_t)n);
				MultiByteToWideChar (CP_UTF8, 0, chemin.c_str (), (int)chemin.size (),
				                     &large[0], n);
			}
		}
		if (!large.empty ()
		    && GetFileAttributesExW (large.c_str (), GetFileExInfoStandard, &infos))
			octets = ((unsigned long long)infos.nFileSizeHigh << 32) | infos.nFileSizeLow;

		return "SAVEPROJECT OK " + std::to_string (octets);
	}

	// Sonde d'enregistrement : SAVEPROBESAVE <mode_hex> <options> <chemin>
	//
	// Sert a essayer les combinaisons sans recompiler. Le mode vient du
	// gestionnaire de FL (0x30 = Enregistrer, 0x40 = Enregistrer sous,
	// 0x20 = autre branche), les options de DAT_011d10bc (bit 0 : chemin tel
	// quel au lieu de Â« + .flp Â» puis renommage).
	if (line.rfind ("SAVEPROBESAVE ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned mode = 0;
		int options = -1;
		char chemin[400] = { 0 };
		if (sscanf_s (line.c_str () + 14, "%x %d %399[^\n]", &mode, &options, chemin) < 3)
			return "ERR syntaxe SAVEPROBESAVE <mode_hex> <options> <chemin>";

		using ToDelphi = void* (*) (void*, const char*);
		ToDelphi toDelphi = (ToDelphi)((unsigned char*)g_engine + 0x1C6E0);
		std::string base (chemin);
		if (options < 0)
		{
			const unsigned char* o = (const unsigned char*)g_engine + 0xDD10BC;
			options = isReadable (o, 1) ? (int)*o : 0;
		}

		void* chaine = nullptr;
		toDelphi (&chaine, base.c_str ());
		if (chaine == nullptr)
			return "ERR chaine chemin";

		const unsigned long long projet =
			*(const unsigned long long*)((const unsigned char*)g_engine + 0x12BCA60);

		using SaveFn = int (*) (void*, int, unsigned long long, unsigned char);
		SaveFn save = (SaveFn)((unsigned char*)g_engine + 0xB54570);
		const int rc = save (chaine, (int)mode, projet, (unsigned char)options);

		char buf[64];
		sprintf_s (buf, "SAVEPROBESAVE mode=0x%X options=%d rc=%d", mode, options, rc);
		return std::string (buf);
	}

	// Charge un projet .flp, exactement comme Â« Fichier > Ouvrir Â».
	//
	//   LOADPROJECT <chemin absolu>
	//
	// Sequence obtenue en decompilant FUN_00f54760 (RVA 0xB54760), confirmee par
	// la chaine Â« Loading FLP file: Â» qu'elle construit :
	//   flux  = FUN_00513540(classe de flux, 1, chemin, 0x20)
	//   projet= FUN_00f51fd0(flux, ...)
	//
	// Le chemin est converti en chaine Delphi par la meme fonction que pour les
	// presets de plugin (RVA 0x1C6E0).
	//
	// ATTENTION : le projet courant est REMPLACE, donc notre propre instance de
	// plugin disparait avec lui. La commande ne peut donc pas repondre : la
	// connexion tombe, c'est le signe que le chargement a demarre.
	if (line.rfind ("LOADPROJECT ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const std::string chemin = line.substr (12);
		if (chemin.size () < 5 || chemin.size () > 380)
			return "ERR syntaxe LOADPROJECT <chemin .flp absolu>";

		// On ne charge PAS ici : le projet remplace decharge ce plugin, donc
		// revenir dans cette fonction apres coup revient dans du code libere.
		// On pose le chemin, la minuterie fait le travail au tick suivant.
		strncpy_s (g_loadPath, chemin.c_str (), _TRUNCATE);
		InterlockedExchange (&g_loadStatus, -1);   // Â« en cours Â»
		InterlockedExchange (&g_loadPending, 1);
		return "LOADPROJECT planifie (" + std::to_string (chemin.size ()) + " o)";
	}

	// Etat du dernier chargement de projet.
	//
	//   LOADSTATUS  ->  LOADSTATUS <-2| -1|0|rc>
	//
	// -2 : aucun chargement demande depuis le demarrage de FL
	// -1 : en cours (ou bloque : si ca reste -1, le chargeur n a pas rendu)
	//  0 : charge
	//  >0 : echec, code rendu par FL (voir docs/POUR-CLAUDE.md)
	//
	// La valeur est statique et le DLL est epingle : elle survit au remplacement
	// de l instance, donc la nouvelle instance lit bien le rc du chargement qui
	// l a fait naitre.
	if (line == "LOADSTATUS")
	{
		const int etat = (int)InterlockedCompareExchange (&g_loadStatus, 0, 0);
		return "LOADSTATUS " + std::to_string (etat);
	}

	// Reponse : "PATTERNS <n>" puis " <motif>:<notes>:<capacite>" par motif.
	if (line == "PATTERNS")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int n = 0;
		std::ostringstream detail;
		for (int i = 1; i <= 999; ++i)
		{
			unsigned char** slot = (unsigned char**)((unsigned char*)g_engine
			                                         + 0x153FCE0 + (size_t)i * 0xC0 + 0x28);
			if (!isReadable (slot, 8))
				break;
			if (*slot == nullptr)
				continue;
			unsigned char* list = noteList (i);
			if (list == nullptr)
				continue;
			detail << " " << i << ":" << *(const int*)(list + 0x14)
			       << ":" << *(const int*)(list + 0x18);
			++n;
		}
		return "PATTERNS " + std::to_string (n) + detail.str ();
	}

	// Nom d'un motif.
	//
	// La fiche de motif (0xC0 octets) porte a +0x00 un pointeur vers une chaine
	// UTF-16 terminee par zero. Verifie le 30/10 : "TEST 1" pour le motif 1,
	// "batterie" pour le 2, chaine vide pour un motif jamais renomme.
	//
	// Reponse : "PATTERNNAME <motif> <nom>"
	if (line.rfind ("PATTERNNAME ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int pattern = 0;
		if (sscanf_s (line.c_str () + 12, "%d", &pattern) != 1
		    || pattern < 1 || pattern > 999)
			return "ERR syntaxe PATTERNNAME <motif>";

		const unsigned char* rec =
			(const unsigned char*)g_engine + 0x153FCE0 + (size_t)pattern * 0xC0;
		if (!isReadable (rec, 0xC0))
			return "ERR motif hors table";

		const wchar_t* nom = *(const wchar_t* const*)rec;
		if (nom == nullptr)
			return "PATTERNNAME " + std::to_string (pattern);

		// Lecture bornee : on ne fait jamais confiance a une chaine non relue.
		std::wstring w;
		for (int k = 0; k < 200; ++k)
		{
			if (!isReadable (nom + k, sizeof (wchar_t)))
				break;
			const wchar_t c = nom[k];
			if (c == 0)
				break;
			w.push_back (c);
		}

		char buf[1200] = { 0 };
		const int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), (int)w.size (),
		                                   buf, (int)sizeof (buf) - 1, nullptr, nullptr);
		std::string utf8 (buf, n > 0 ? (size_t)n : 0);
		return "PATTERNNAME " + std::to_string (pattern) + " " + utf8;
	}

	if (line == "NOTES" || line.rfind ("NOTES ", 0) == 0)
	{
		// Toutes les notes d'un motif, tous canaux confondus.
		// "NOTES" = motif courant ; "NOTES <n>" = motif n, meme s'il n'est pas
		// affiche. C'est ce qui permet de synchroniser TOUS les motifs de
		// l'hote, pas seulement celui qu'il regarde.
		// Reponse : "NOTES <motif> <nombre>" puis "t:l:p:v:canal" et "END <nombre>".
		const int pattern = (line == "NOTES")
			? currentPattern ()
			: (int)strtol (line.c_str () + 6, nullptr, 10);
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes pour le motif " + std::to_string (pattern);

		const int count = *(const int*)(list + 0x14);
		const unsigned char* data = *(const unsigned char* const*)(list + 0x08);
		if (count < 0 || count > 100000)
			return "ERR nombre de notes inattendu";
		if (count > 0 && !isReadable (data, (size_t)count * 24))
			return "ERR donnees illisibles";

		std::ostringstream out;
		out << "NOTES " << pattern << " " << count << "\n";
		for (int k = 0; k < count; ++k)
		{
			const unsigned char* n = data + (size_t)k * 24;
			out << *(const unsigned*)(n + 0x00)
			    << ":" << *(const unsigned*)(n + 0x08)
			    << ":" << *(const unsigned*)(n + 0x0C)
			    << ":" << (unsigned)*(const unsigned char*)(n + 0x15)
			    << ":" << noteChannelOf (n)
			    << (k + 1 < count ? " " : "");
		}
		out << "\nEND " << count;
		return out.str ();
	}

	if (line.rfind ("ADD ", 0) == 0)
	{
		if (g_engine == nullptr || g_insertFn == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (4));
		unsigned channel = 0, time = 0, len = 48, pitch = 60, vel = 100;
		if (!(ss >> channel >> time >> len >> pitch))
			return "ERR syntaxe ADD <canal> <temps> <duree> <hauteur> [vel]";
		if (!(ss >> vel))
			vel = 100;

		const int pattern = currentPattern ();
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes";

		unsigned char note[24];
		fillNote (note, channel, time, len, pitch, vel);

		int before = *(const int*)(list + 0x14);

		using InsertFn = int (*) (void*, void*);
		InsertFn fn = (InsertFn)g_insertFn;
		int idx = fn ((void*)list, note);

		list = noteList (pattern);
		int after = list ? *(const int*)(list + 0x14) : -1;

		std::ostringstream out;
		out << "ADDED " << channel << " " << idx << " " << before << " " << after;
		requestRefresh ();
		return out.str ();
	}

	if (line.rfind ("DEL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (4));
		unsigned channel = 0, at = 0, pitch = 0;
		if (!(ss >> channel >> at >> pitch))
			return "ERR syntaxe DEL <canal> <temps> <hauteur>";

		const int pattern = currentPattern ();
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes";

		const int count = *(int*)(list + 0x14);
		if (count <= 0)
			return "ERR liste vide";

		unsigned char* data = *(unsigned char**)(list + 0x08);
		if (!isReadable (data, (size_t)count * 24))
			return "ERR donnees illisibles";

		int found = -1;
		for (int k = 0; k < count; k++)
		{
			unsigned char* note = data + (size_t)k * 24;
			if (*(unsigned*)(note + 0x00) == at
			    && *(unsigned*)(note + 0x0C) == pitch
			    && (unsigned)noteChannelOf (note) == channel)
			{
				found = k;
				break;
			}
		}
		if (found < 0)
			return "ERR note introuvable";

		memmove (data + (size_t)found * 24,
		         data + (size_t)(found + 1) * 24,
		         (size_t)(count - found - 1) * 24);
		*(int*)(list + 0x14) = count - 1;

		std::ostringstream out;
		out << "DELETED " << channel << " " << found << " " << count << " " << (count - 1);
		requestRefresh ();
		return out.str ();
	}

	// Ajout groupe : ADDX <canal> <n> <t>:<l>:<p>:<v> ...
	// Un seul rafraichissement pour tout le lot : c'est ce qui rend les
	// gros colle/annulations supportables.
	if (line.rfind ("ADDX ", 0) == 0)
	{
		if (g_engine == nullptr || g_insertFn == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (5));
		unsigned channel = 0, count = 0;
		if (!(ss >> channel >> count))
			return "ERR syntaxe ADDX <canal> <nombre> <t:l:p:v>...";

		const int total = channelCount ();
		if (total >= 0 && (int)channel >= total)
		{
			std::ostringstream err;
			err << "ERR canal " << channel << " hors du rack (" << total
			    << " canal(aux) sur ce poste)";
			return err.str ();
		}

		const int pattern = currentPattern ();
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes";

		using InsertFn = int (*) (void*, void*);
		InsertFn fn = (InsertFn)g_insertFn;

		const int before = *(const int*)(list + 0x14);
		unsigned inserted = 0;

		std::string token;
		while (ss >> token && inserted < count)
		{
			unsigned t = 0, l = 48, p = 60, v = 100;
			if (sscanf_s (token.c_str (), "%u:%u:%u:%u", &t, &l, &p, &v) < 3)
				continue;

			unsigned char note[24];
			fillNote (note, channel, t, l, p, v);

			fn ((void*)list, note);
			inserted++;

			// la liste peut avoir ete reallouee par l'insertion
			list = noteList (pattern);
			if (list == nullptr)
				break;
		}

		const int after = list ? *(const int*)(list + 0x14) : -1;
		std::ostringstream out;
		out << "ADDEDX " << channel << " " << inserted << " " << before << " " << after;
		requestRefresh ();
		return out.str ();
	}

	// Suppression groupee : DELX <canal> <n> <t>:<p> ...
	// On reconstruit la liste en gardant les notes non visees : O(n) au lieu
	// de n deplacements memoire. Le canal fait partie de la cible.
	// Insertion dans un motif DONNE, sans changer le motif courant.
	//
	// ADDX ecrit toujours dans currentPattern(). Pour remplir tous les motifs de
	// l'hote chez l'invite il faut viser une liste precise : la fonction
	// d'insertion prend le pointeur de liste en premier argument, donc on peut
	// la nourrir directement avec celle du motif vise.
	//
	// Reponse : "ADDEDTO <motif> <canal> <inserees> <avant> <apres>"
	if (line.rfind ("ADDXTO ", 0) == 0)
	{
		if (g_engine == nullptr || g_insertFn == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (7));
		unsigned channel = 0, pattern = 0, count = 0;
		if (!(ss >> channel >> pattern >> count))
			return "ERR syntaxe ADDXTO <canal> <motif> <nombre> <t:l:p:v>...";

		const int total = channelCount ();
		if (total >= 0 && (int)channel >= total)
		{
			std::ostringstream err;
			err << "ERR canal " << channel << " hors du rack (" << total
			    << " canal(aux) sur ce poste)";
			return err.str ();
		}

		unsigned char* list = noteList ((int)pattern);
		if (list == nullptr)
			return "ERR motif " + std::to_string (pattern) + " absent";

		using InsertFn = int (*) (void*, void*);
		InsertFn fn = (InsertFn)g_insertFn;

		const int before = *(const int*)(list + 0x14);
		unsigned inserted = 0;

		std::string token;
		while (ss >> token && inserted < count)
		{
			unsigned t = 0, l = 48, p = 60, v = 100;
			if (sscanf_s (token.c_str (), "%u:%u:%u:%u", &t, &l, &p, &v) < 3)
				continue;

			unsigned char note[24];
			fillNote (note, channel, t, l, p, v);

			fn ((void*)list, note);
			inserted++;

			list = noteList ((int)pattern);
			if (list == nullptr)
				break;
		}

		const int after = list ? *(const int*)(list + 0x14) : -1;
		std::ostringstream out;
		out << "ADDEDTO " << pattern << " " << channel << " " << inserted
		    << " " << before << " " << after;
		requestRefresh ();
		return out.str ();
	}

	if (line.rfind ("DELX ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (5));
		unsigned channel = 0, count = 0;
		if (!(ss >> channel >> count))
			return "ERR syntaxe DELX <canal> <nombre> <t:p>...";

		const int pattern = currentPattern ();
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes";

		int total = *(int*)(list + 0x14);
		if (total <= 0)
			return "ERR liste vide";
		unsigned char* data = *(unsigned char**)(list + 0x08);
		if (!isReadable (data, (size_t)total * 24))
			return "ERR donnees illisibles";

		std::vector<std::pair<unsigned, unsigned>> targets;
		std::string token;
		while (ss >> token && targets.size () < count)
		{
			unsigned t = 0, p = 0;
			if (sscanf_s (token.c_str (), "%u:%u", &t, &p) == 2)
				targets.push_back (std::make_pair (t, p));
		}

		int kept = 0;
		for (int k = 0; k < total; k++)
		{
			unsigned char* note = data + (size_t)k * 24;
			const unsigned nt = *(unsigned*)(note + 0x00);
			const unsigned np = *(unsigned*)(note + 0x0C);
			const bool sameChannel = (unsigned)noteChannelOf (note) == channel;

			bool kill = false;
			for (size_t x = 0; x < targets.size (); x++)
			{
				if (sameChannel && targets[x].first == nt && targets[x].second == np)
				{
					kill = true;
					targets.erase (targets.begin () + x);
					break;
				}
			}
			if (kill)
				continue;

			if (kept != k)
				memcpy (data + (size_t)kept * 24, note, 24);
			kept++;
		}

		*(int*)(list + 0x14) = kept;
		std::ostringstream out;
		out << "DELETEDX " << channel << " " << (total - kept) << " " << total << " " << kept;
		requestRefresh ();
		return out.str ();
	}

	// Efface une liste de notes : CLEAR <index>
	// Longueur d'un clip existant : CLIPSETLEN <position> <longueur> [piste] [motif]
	if (line.rfind ("CLIPSETLEN ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (11));
		unsigned pos = 0, len = 0;
		if (!(ss >> pos >> len))
			return "ERR syntaxe CLIPSETLEN <position> <longueur> [piste] [motif]";
		int wantTrack = -1, wantPat = -1;
		ss >> wantTrack;
		ss >> wantPat;

		unsigned char* c = getClipContainer ();
		if (c == nullptr)
			return "ERR conteneur de clips introuvable";

		const int count = *(int*)(c + 0x14);
		unsigned char* data = *(unsigned char**)(c + 0x08);
		if (count <= 0 || !isReadable (data, (size_t)count * 0x58))
			return "ERR donnees illisibles";

		for (int k = 0; k < count; k++)
		{
			unsigned char* r = data + (size_t)k * 0x58;
			if (*(const unsigned*)r != pos)
				continue;
			if (wantTrack >= 1 && (244 - (int)r[0x0C]) != wantTrack)
				continue;
			if (wantPat >= 1 && (int)r[0x06] != wantPat)
				continue;
			*(unsigned*)(r + 0x08) = len;
			*(unsigned*)(r + 0x1C) = len;
			return "CLIPLEN " + std::to_string (pos) + " " + std::to_string (len);
		}
		return "ERR clip introuvable";
	}

	// Vide la playlist : CLIPCLEAR
	if (line == "CLIPCLEAR")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned char* c = getClipContainer ();
		if (c == nullptr)
			return "ERR conteneur de clips introuvable";

		const int count = *(int*)(c + 0x14);
		*(int*)(c + 0x14) = 0;
		requestRefresh ();

		std::ostringstream out;
		out << "CLIPCLEARED " << count;
		return out.str ();
	}

	// Efface les notes d'un canal : CLEAR <canal>
	if (line.rfind ("CLEAR ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (6));
		unsigned channel = 0;
		if (!(ss >> channel))
			return "ERR syntaxe CLEAR <canal>";

		const int pattern = currentPattern ();
		unsigned char* list = noteList (pattern);
		if (list == nullptr)
			return "ERR pas de liste de notes";

		int total = *(int*)(list + 0x14);
		if (total <= 0)
			return std::string ("CLEARED ") + std::to_string (channel) + " 0 0 0";

		unsigned char* data = *(unsigned char**)(list + 0x08);
		if (!isReadable (data, (size_t)total * 24))
			return "ERR donnees illisibles";

		int kept = 0;
		for (int k = 0; k < total; k++)
		{
			unsigned char* note = data + (size_t)k * 24;
			if ((unsigned)noteChannelOf (note) == channel)
				continue;
			if (kept != k)
				memcpy (data + (size_t)kept * 24, note, 24);
			kept++;
		}
		*(int*)(list + 0x14) = kept;

		std::ostringstream out;
		out << "CLEARED " << channel << " " << (total - kept) << " " << total << " " << kept;
		requestRefresh ();
		return out.str ();
	}

	// Memes operations, mais sur un motif DONNE (pas forcement le courant).
	//
	// CLEAR et DELX ne savent viser que currentPattern(). Pour aligner un
	// invite sur TOUS les motifs de l'hote, il faut pouvoir effacer et
	// supprimer dans une liste precise sans deplacer la vue de l'utilisateur.
	if (line.rfind ("CLEARTO ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (8));
		unsigned channel = 0, pattern = 0;
		if (!(ss >> channel >> pattern))
			return "ERR syntaxe CLEARTO <canal> <motif>";

		unsigned char* list = noteList ((int)pattern);
		if (list == nullptr)
			return "ERR motif " + std::to_string (pattern) + " absent";

		const int total = *(int*)(list + 0x14);
		if (total <= 0)
			return "CLEAREDTO " + std::to_string (pattern) + " "
			       + std::to_string (channel) + " 0 0 0";

		unsigned char* data = *(unsigned char**)(list + 0x08);
		if (!isReadable (data, (size_t)total * 24))
			return "ERR donnees illisibles";

		int kept = 0;
		for (int k = 0; k < total; k++)
		{
			unsigned char* note = data + (size_t)k * 24;
			if ((unsigned)noteChannelOf (note) == channel)
				continue;
			if (kept != k)
				memcpy (data + (size_t)kept * 24, note, 24);
			kept++;
		}
		*(int*)(list + 0x14) = kept;
		requestRefresh ();

		std::ostringstream out;
		out << "CLEAREDTO " << pattern << " " << channel << " " << (total - kept)
		    << " " << total << " " << kept;
		return out.str ();
	}

	if (line.rfind ("DELXTO ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (7));
		unsigned channel = 0, pattern = 0, count = 0;
		if (!(ss >> channel >> pattern >> count))
			return "ERR syntaxe DELXTO <canal> <motif> <nombre> <t:p>...";

		unsigned char* list = noteList ((int)pattern);
		if (list == nullptr)
			return "ERR motif " + std::to_string (pattern) + " absent";

		int total = *(int*)(list + 0x14);
		if (total <= 0)
			return "ERR liste vide";
		unsigned char* data = *(unsigned char**)(list + 0x08);
		if (!isReadable (data, (size_t)total * 24))
			return "ERR donnees illisibles";

		std::vector<std::pair<unsigned, unsigned>> targets;
		std::string token;
		while (ss >> token && targets.size () < count)
		{
			unsigned t = 0, p = 0;
			if (sscanf_s (token.c_str (), "%u:%u", &t, &p) == 2)
				targets.push_back (std::make_pair (t, p));
		}

		int kept = 0;
		for (int k = 0; k < total; k++)
		{
			unsigned char* note = data + (size_t)k * 24;
			const unsigned nt = *(unsigned*)(note + 0x00);
			const unsigned np = *(unsigned*)(note + 0x0C);
			const bool sameChannel = (unsigned)noteChannelOf (note) == channel;

			bool kill = false;
			for (size_t x = 0; x < targets.size (); x++)
			{
				if (sameChannel && targets[x].first == nt && targets[x].second == np)
				{
					kill = true;
					targets.erase (targets.begin () + x);
					break;
				}
			}
			if (kill)
				continue;

			if (kept != k)
				memcpy (data + (size_t)kept * 24, note, 24);
			kept++;
		}

		*(int*)(list + 0x14) = kept;
		requestRefresh ();

		std::ostringstream out;
		out << "DELETEDXTO " << pattern << " " << channel << " "
		    << (total - kept) << " " << total << " " << kept;
		return out.str ();
	}

	if (line == "TEMPO")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";
		const unsigned char* t = (const unsigned char*)g_engine + 0x11D8930;
		std::ostringstream out;
		out << "TEMPO " << *(const int*)(t + 0x00)
		    << " " << *(const double*)(t + 0x08)
		    << " " << *(const float*)(t + 0x10);
		return out.str ();
	}

	if (line.rfind ("TEMPO ", 0) == 0)
	{
		// Autorite : ces actions touchent le PROJET, qui appartient a l'hote.
		if (roleBloque ())
			return "ERR action reservee a l'hote";


		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		double bpm = atof (line.c_str () + 6);
		if (bpm < 20.0 || bpm > 999.0)
			return "ERR tempo hors plage (20..999)";

		unsigned char* t = (unsigned char*)g_engine + 0x11D8930;
		const int milli = (int)(bpm * 1000.0 + 0.5);

		// La structure +0x11D8930 n'est qu'un cache : l'ecrire ne change rien.
		// FL passe par un repertoire de commandes : id 0x40000005 = tempo.
		using DispatchFn = void (*) (int, int, int);
		DispatchFn dispatch = (DispatchFn)((unsigned char*)g_engine + 0xC46A70);
		dispatch (0x40000005, milli, 0x3DD);

		std::ostringstream out;
		out << "TEMPO-SET " << milli
		    << " (cache: " << *(const int*)(t + 0x00) << ")";
		return out.str ();
	}

	// Mode de lecture : 0 = PAT, 1 = SONG.
	//   SONGMODE          lit le mode
	//   SONGMODE <0|1>    bascule s'il le faut
	if (line == "SONGMODE" || line.rfind ("SONGMODE ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned char* base = (unsigned char*)g_engine;
		const int* mode = *(const int* const*)(base + 0x11E0EF0);
		if (!isReadable (mode, 4))
			return "ERR mode illisible";
		if (line == "SONGMODE")
			return "SONGMODE " + std::to_string (*mode);

		if (roleBloque ())
			return "ERR action reservee a l'hote";

		int voulu = -1;
		if (sscanf_s (line.c_str () + 9, "%d", &voulu) != 1 || voulu < 0 || voulu > 1)
			return "ERR syntaxe SONGMODE <0|1>";
		if (*mode != voulu)
		{
			const unsigned long faute = guardedToggleLoopMode (base);
			if (faute != 0)
				return "ERR SONGMODE faute " + std::to_string (faute);
		}
		return "SONGMODE OK " + std::to_string (voulu);
	}

	if (line == "PLAY" || line == "STOP")
	{
		// Autorite : ces actions touchent le PROJET, qui appartient a l'hote.
		if (roleBloque ())
			return "ERR action reservee a l'hote";


		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		// FUN_011CA520(int mode, byte flags) â€” vraie fonction de transport.
		// mode 1 = lecture, mode 0 = arret.
		// Ne PAS passer par FUN_00FDD300 (multiplexeur UI) : il prend un
		// pointeur et fait planter FL hors contexte (verifie le 28/10 a 01h).
		using TransportFn = int (*) (int, unsigned char);
		TransportFn transport = (TransportFn)((unsigned char*)g_engine + 0xDCA520);

		const bool play = (line == "PLAY");
		int rc = transport (play ? 1 : 0, 1);

		char buf[64];
		sprintf_s (buf, "%s rc=%d", play ? "PLAY" : "STOP", rc);
		return std::string (buf);
	}

	if (line == "ISPLAYING")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		// 0x11E09F8 contient un POINTEUR vers l'etat, pas l'etat lui-meme.
		// (Piege PTR_DAT de Ghidra : rencontrer trois fois ce soir.)
		const int* slot = *(const int* const*)((const unsigned char*)g_engine + 0x11E09F8);
		if (slot == nullptr || !isReadable (slot, sizeof (int)))
			return "ERR etat illisible";

		return std::string ("PLAYING ") + (*slot != 0 ? "1" : "0");
	}

	if (line == "PATTERN")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";
		// DAT_015d51a0 : numero du pattern courant (global direct, pas un pointeur)
		const int current = *(const int*)((const unsigned char*)g_engine + 0x11D51A0);
		return std::string ("PATTERN ") + std::to_string (current);
	}

	if (line.rfind ("PATTERN ", 0) == 0)
	{
		// Autorite : ces actions touchent le PROJET, qui appartient a l'hote.
		if (roleBloque ())
			return "ERR action reservee a l'hote";


		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int wanted = atoi (line.c_str () + 8);
		if (wanted < 1 || wanted > 999)
			return "ERR pattern hors plage (1..999)";

		// FUN_012D8B30(int) : c'est la fonction que FL appelle quand on
		// selectionne un pattern. Elle prend un simple entier.
		using PatternFn = void (*) (int);
		PatternFn setPattern = (PatternFn)((unsigned char*)g_engine + 0xED8B30);
		setPattern (wanted);

		const int now = *(const int*)((const unsigned char*)g_engine + 0x11D51A0);
		return std::string ("PATTERN-SET ") + std::to_string (wanted) + " (actuel: " +
		       std::to_string (now) + ")";
	}

	if (line == "MOUSE")
	{
		if (g_flWindow == nullptr)
			return "MOUSE -1 -1";

		POINT p;
		if (!GetCursorPos (&p))
			return "MOUSE -1 -1";

		POINT c = p;
		if (!ScreenToClient (g_flWindow, &c))
			return "MOUSE -1 -1";

		RECT rc;
		if (!GetClientRect (g_flWindow, &rc))
			return "MOUSE -1 -1";

		if (c.x < 0 || c.y < 0 || c.x >= rc.right || c.y >= rc.bottom)
			return "MOUSE -1 -1";

		return "MOUSE " + std::to_string (c.x) + " " + std::to_string (c.y);
	}

	if (line.rfind ("CURSORF ", 0) == 0)
	{
		std::istringstream ss (line.substr (8));
		int fx = 0, fy = 0;
		if (!(ss >> fx >> fy))
			return "ERR syntaxe CURSORF <x 0..1000> <y 0..1000> [etiquette]";

		std::string label;
		std::getline (ss, label);
		while (!label.empty () && (label.front () == ' ' || label.front () == '\t'))
			label.erase (label.begin ());
		if (label.size () > 48)
			label = label.substr (0, 48);

		if (g_cursorWnd == nullptr || g_flWindow == nullptr)
			return "ERR fenetre curseur indisponible";

		RECT rc;
		if (!GetClientRect (g_flWindow, &rc) || rc.right <= 0 || rc.bottom <= 0)
			return "ERR fenetre FL indisponible";

		const int x = (int)((long long)fx * rc.right / 1000);
		const int y = (int)((long long)fy * rc.bottom / 1000);

		if (!label.empty ())
			strncpy_s (g_remoteLabel, label.c_str (), _TRUNCATE);

		g_remoteX = x;
		g_remoteY = y;

		POINT p;
		p.x = x;
		p.y = y;
		ClientToScreen (g_flWindow, &p);
		SetWindowPos (g_cursorWnd, HWND_TOPMOST, p.x - 4, p.y - 4, 160, 64,
		              SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect (g_cursorWnd, nullptr, TRUE);

		return "CURSOR-OK " + std::to_string (x) + " " + std::to_string (y);
	}

	if (line.rfind ("CURSOR ", 0) == 0)
	{
		std::istringstream ss (line.substr (7));
		int x = 0, y = 0;
		if (!(ss >> x >> y))
			return "ERR syntaxe CURSOR <x> <y> [etiquette]";

		std::string label;
		std::getline (ss, label);
		while (!label.empty () && (label.front () == ' ' || label.front () == '\t'))
			label.erase (label.begin ());
		if (label.size () > 48)
			label = label.substr (0, 48);

		if (g_cursorWnd == nullptr || g_flWindow == nullptr)
			return "ERR fenetre curseur indisponible";

		if (!label.empty ())
			strncpy_s (g_remoteLabel, label.c_str (), _TRUNCATE);

		g_remoteX = x;
		g_remoteY = y;

		POINT p;
		p.x = x;
		p.y = y;
		ClientToScreen (g_flWindow, &p);

		SetWindowPos (g_cursorWnd, HWND_TOPMOST, p.x - 4, p.y - 4, 160, 64,
		              SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect (g_cursorWnd, nullptr, TRUE);

		return "CURSOR-OK " + std::to_string (x) + " " + std::to_string (y);
	}

	if (line == "CURSOR-HIDE")
	{
		if (g_cursorWnd != nullptr)
			ShowWindow (g_cursorWnd, SW_HIDE);
		return "CURSOR-HIDDEN";
	}

	// Volume d'une piste du mixeur.
	// FL stocke le volume en virgule fixe 0..2^30 (2^30 = 1.0).
	// Le handle est CALCULE, pas dereference : (piste*0x40 + slot)*0x10000 + 0x70001FC0
	if (line.rfind ("TRACKVOL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (9));
		int track = 0;
		if (!(ss >> track) || track < 0 || track > 125)
			return "ERR syntaxe TRACKVOL <piste 0..125> [valeur 0..1]";

		const long long handle = ((long long)track * 0x40) * 0x10000LL + 0x70001FC0LL;

		double wanted = -1.0;
		if (ss >> wanted)
		{
			if (wanted < 0.0) wanted = 0.0;
			if (wanted > 1.0) wanted = 1.0;

			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			const int raw = (int)(wanted * 1073741824.0 + 0.5);
			setter ((int)handle, raw, 0x3F5);
		}

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int now = getter (handle, 0x7FFFFFFF, 1);
		const double value = (double)now * (1.0 / 1073741824.0);

		char buf[128];
		sprintf_s (buf, "TRACKVOL %d %.6f (brut %d)", track, value, now);
		return std::string (buf);
	}

	// Panoramique d'une piste. Meme mecanique que le volume, mais le handle
	// est decale d'un octet (0x70001FC1) et la valeur va de -1 (gauche) a +1.
	if (line.rfind ("TRACKPAN ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (9));
		int track = 0;
		if (!(ss >> track) || track < 0 || track > 125)
			return "ERR syntaxe TRACKPAN <piste> [pan -1..1]";

		const long long handle = ((long long)track * 0x40) * 0x10000LL + 0x70001FC1LL;

		double wanted = -99.0;
		if (ss >> wanted)
		{
			if (wanted < -1.0) wanted = -1.0;
			if (wanted > 1.0) wanted = 1.0;

			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			const int raw = (int)(((wanted + 1.0) / 2.0) * 1073741824.0 + 0.5);
			setter ((int)handle, raw, 0x3F5);
		}

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int now = getter (handle, 0x7FFFFFFF, 1);
		const double value = ((double)now / 1073741824.0) * 2.0 - 1.0;

		char buf[128];
		sprintf_s (buf, "TRACKPAN %d %.4f (brut %d)", track, value, now);
		return std::string (buf);
	}

	// Mute d'une piste.
	// ATTENTION : FL attend l'INVERSE de l'intuitif â€” 0 = muet, 1 = actif.
	// L'etat se lit dans la structure de piste (pas de 0x1478), octet +0x18.
	if (line.rfind ("TRACKMUTE ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (10));
		int track = 0;
		if (!(ss >> track) || track < 0 || track > 125)
			return "ERR syntaxe TRACKMUTE <piste> [1=muet 0=actif]";

		int wanted = -1;
		if (ss >> wanted)
		{
			using MuteFn = void (*) (int, int);
			MuteFn mute = (MuteFn)((unsigned char*)g_engine + 0xE7B9B0);
			mute (track, wanted != 0 ? 0 : 1);   // sens inverse
		}

		int state = -1;
		const unsigned char* base =
			*(const unsigned char* const*)((const unsigned char*)g_engine + 0x11E06A0);
		if (isReadable (base, (size_t)track * 0x1478 + 0x20))
		{
			// L'octet +0x18 vaut 1 quand la piste est ACTIVE, donc muet = 0.
			const unsigned char active = *(const unsigned char*)(base + (size_t)track * 0x1478 + 0x18);
			state = active != 0 ? 0 : 1;
		}

		char buf[96];
		sprintf_s (buf, "TRACKMUTE %d %d", track, state);
		return std::string (buf);
	}

	// Separation stereo : handle +0x70001FC2, meme echelle que le pan (-1..+1).
	// -1 = mono, 0 = normal, +1 = stereo elargi.
	if (line.rfind ("TRACKSTEREO ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (12));
		int track = 0;
		if (!(ss >> track) || track < 0 || track > 125)
			return "ERR syntaxe TRACKSTEREO <piste> [-1..1]";

		const long long handle = ((long long)track * 0x40) * 0x10000LL + 0x70001FC2LL;

		double wanted = -99.0;
		if (ss >> wanted)
		{
			if (wanted < -1.0) wanted = -1.0;
			if (wanted > 1.0) wanted = 1.0;

			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			const int raw = (int)(((wanted + 1.0) / 2.0) * 1073741824.0 + 0.5);
			setter ((int)handle, raw, 0x3F5);
		}

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int now = getter (handle, 0x7FFFFFFF, 1);
		const double value = ((double)now / 1073741824.0) * 2.0 - 1.0;

		char buf[128];
		sprintf_s (buf, "TRACKSTEREO %d %.4f (brut %d)", track, value, now);
		return std::string (buf);
	}

	// Parametre d'un plugin dans une tranche du mixeur.
	// handle = (piste*0x40 + slot)*0x10000 + index_param + 0x70008000
	// identifiant 0x3FD, valeur normalisee 0..1 sur 2^30.
	if (line.rfind ("PARAM ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (6));
		int track = 0, slot = 0, index = 0;
		if (!(ss >> track >> slot >> index))
			return "ERR syntaxe PARAM <piste> <slot> <index> [0..1]";
		if (track < 0 || track > 125 || slot < 0 || slot > 9 || index < 0 || index > 0x7FFF)
			return "ERR plage invalide";

		const long long handle =
			((long long)track * 0x40 + slot) * 0x10000LL + (long long)index + 0x70008000LL;

		double wanted = -1.0;
		if (ss >> wanted)
		{
			if (wanted < 0.0) wanted = 0.0;
			if (wanted > 1.0) wanted = 1.0;

			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			const int raw = (int)(wanted * 1073741824.0 + 0.5);
			setter ((int)handle, raw, 0x3FD);
		}

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int now = getter (handle, 0x7FFFFFFF, 1);
		const double value = (double)now * (1.0 / 1073741824.0);

		char buf[128];
		sprintf_s (buf, "PARAM %d %d %d %.6f (brut %d)", track, slot, index, value, now);
		return std::string (buf);
	}

	// Ecriture d'un parametre en valeur BRUTE : PARAMRAW <piste> <slot> <index> <brut>
	// L'interet est de ne faire aucune conversion : on relit une valeur brute
	// avec PARAMS et on la reecrit telle quelle chez l'autre. Ca evite de
	// dependre d'une echelle (2^30) qu'on n'a pas encore verifiee sur un effet.
	if (line.rfind ("PARAMRAW ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (9));
		int track = 0, slot = 0, index = 0;
		long long raw = 0;
		if (!(ss >> track >> slot >> index >> raw))
			return "ERR syntaxe PARAMRAW <piste> <slot> <index> <brut>";
		if (track < 0 || track > 125 || slot < 0 || slot > 9 || index < 0 || index > 0x7FFF)
			return "ERR plage invalide";

		const long long handle =
			((long long)track * 0x40 + slot) * 0x10000LL + (long long)index + 0x70008000LL;

		using SetFn = int (*) (int, int, int);
		SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
		setter ((int)handle, (int)raw, 0x3FD);

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const int now = getter (handle, 0x7FFFFFFF, 1);

		char buf[96];
		sprintf_s (buf, "PARAMRAW %d %d %d %d %d", track, slot, index, (int)raw, now);
		return std::string (buf);
	}

	// Liste de TOUS les conteneurs : outil de diagnostic.
	if (line == "CLIPS")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const unsigned long long cls =
			(unsigned long long)((unsigned char*)g_engine + 0xC54168);
		unsigned char pattern[8];
		memcpy (pattern, &cls, 8);

		std::ostringstream out;
		int containers = 0;
		int totalClips = 0;

		MEMORY_BASIC_INFORMATION mbi;
		unsigned char* addr = nullptr;
		while (VirtualQuery (addr, &mbi, sizeof (mbi)) == (SIZE_T)sizeof (mbi))
		{
			unsigned char* base = (unsigned char*)mbi.BaseAddress;
			const size_t size = mbi.RegionSize;
			if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
			    size >= 0x20 && size <= 64u * 1024 * 1024)
			{
				for (size_t i = 0; i + 0x20 <= size; i += 8)
				{
					if (memcmp (base + i, pattern, 8) != 0)
						continue;
					const unsigned char* c = base + i;
					const int elemSize = *(const int*)(c + 0x10);
					const int count = *(const int*)(c + 0x14);
					const int capacity = *(const int*)(c + 0x18);
					const unsigned char* data = *(const unsigned char* const*)(c + 0x08);
					if (elemSize != 0x58 || count < 0 || count > 5000 ||
					    capacity < count || capacity > 200000)
						continue;
					if (count > 0 && !isReadable (data, (size_t)count * 0x58))
						continue;

					containers++;
					totalClips += count;
					out << "CONTAINER " << (void*)c << " " << count;
					for (int k = 0; k < count && k < 32; k++)
					{
						const unsigned char* r = data + (size_t)k * 0x58;
						out << " " << *(const unsigned*)(r + 0x00)
						    << ":" << *(const unsigned*)(r + 0x08)
						    << ":" << (unsigned)r[0x06];
					}
					out << "\n";
				}
			}
			unsigned char* next = base + size;
			if (next <= addr) break;
			addr = next;
		}
		out << "ENDCLIPS " << containers << " " << totalClips;
		return out.str ();
	}

	// Ajout d'un clip : on clone le dernier enregistrement et on l'insere.
	if (line.rfind ("CLIPADD ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (8));
		unsigned pos = 0, len = 384;
		if (!(ss >> pos))
			return "ERR syntaxe CLIPADD <position> <longueur> [motif]";
		ss >> len;
		int motif = -1;
		ss >> motif;
		int track = -1;
		ss >> track;

		unsigned char* best = getClipContainer ();
		if (best == nullptr)
			return "ERR conteneur de clips introuvable";
		if (g_insertFn == nullptr)
			return "ERR fonction d'insertion indisponible";

		const int count = *(int*)(best + 0x14);
		const int capacity = *(int*)(best + 0x18);
		unsigned char* data = *(unsigned char**)(best + 0x08);

		if (count > 0 && !isReadable (data, (size_t)count * 0x58))
			return "ERR donnees illisibles";
		if (count > 0 && data == nullptr)
			return "ERR donnees illisibles";

		unsigned char record[0x58];
		if (count > 0)
		{
			// le plus sur : recopier un clip existant (mise en page garantie)
			memcpy (record, data + (size_t)(count - 1) * 0x58, 0x58);
		}
		else
		{
			// playlist vide : on fabrique l'enregistrement d'apres le format
			// observe (88 octets, position, motif, couleur, gabarit de notes).
			memset (record, 0, sizeof (record));
			const unsigned pat = (motif >= 0 && motif <= 255) ? (unsigned)motif : 1u;
			*(unsigned*)(record + 0x04) = 0x50005000u | (pat << 16);
			*(unsigned*)(record + 0x0C) = 499u;
			*(unsigned*)(record + 0x10) = 0x00400078u;
			*(unsigned*)(record + 0x14) = 0x80806440u;
			*(unsigned*)(record + 0x18) = 0xFFFFFFFFu;
			*(unsigned*)(record + 0x3C) = 0xFFFFFFFFu;
			*(double*)(record + 0x30) = 1.0;
		}
		*(unsigned*)(record + 0x00) = pos;
		*(unsigned*)(record + 0x08) = len;
		*(unsigned*)(record + 0x1C) = len;
		*(unsigned*)(record + 0x20) = (unsigned)(count + 1);
		if (motif >= 0 && motif <= 255)
			record[0x06] = (unsigned char)motif;

		// La piste est encodee dans la couleur : piste = 244 - couleur.
		// Verifie en direct : changer cet octet deplace le clip de piste.
		if (track >= 1 && track <= 200)
		{
			const int color = 244 - track;
			if (color >= 0 && color <= 255)
				record[0x0C] = (unsigned char)color;
		}

		// Insertion par la fonction de FL : elle gere l'agrandissement du
		// tableau (capacite 0 sur une playlist vide) et le classement par
		// position. Notre memcpy manuel echouait des que la playlist etait vide.
		using InsertFn = int (*) (void*, void*);
		InsertFn fn = (InsertFn)g_insertFn;
		const int inserted = fn ((void*)best, record);

		(void)capacity;

		g_clipFixPos = pos;
		g_clipFixLen = len;
		g_clipFixWait = 3;
		InterlockedExchange (&g_clipFixPending, 1);

		requestRefresh ();

		std::ostringstream out;
		out << "CLIPADDED " << (void*)best << " " << pos << " " << len
		    << " (insere a " << inserted << ", " << count << " -> "
		    << *(int*)(best + 0x14) << ")";
		return out.str ();
	}

	// Ajout d'un clip dans un conteneur DESIGNNE (pour choisir la piste).
	if (line.rfind ("CLIPADDTO ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (10));
		std::string addrText;
		unsigned pos = 0, len = 384;
		if (!(ss >> addrText >> pos))
			return "ERR syntaxe CLIPADDTO <adresse> <position> <longueur>";
		ss >> len;

		unsigned long long addrValue = strtoull (addrText.c_str (), nullptr, 16);
		unsigned char* c = (unsigned char*)(uintptr_t)addrValue;
		if (!isReadable (c, 0x20))
			return "ERR adresse invalide";

		const int count = *(int*)(c + 0x14);
		const int capacity = *(int*)(c + 0x18);
		unsigned char* data = *(unsigned char**)(c + 0x08);

		if (count <= 0 || count >= capacity)
			return "ERR conteneur vide ou plein";
		if (!isReadable (data, (size_t)(count + 1) * 0x58))
			return "ERR donnees illisibles";

		unsigned char record[0x58];
		memcpy (record, data, 0x58);
		*(unsigned*)(record + 0x00) = pos;
		*(unsigned*)(record + 0x08) = len;
		*(unsigned*)(record + 0x1C) = len;
		*(unsigned*)(record + 0x20) = (unsigned)(count + 1);

		memcpy (data + (size_t)count * 0x58, record, 0x58);
		*(int*)(c + 0x14) = count + 1;

		requestRefresh ();

		char buf[128];
		sprintf_s (buf, "CLIPADDED-TO %s %u %u (%d -> %d)", addrText.c_str (), pos, len, count, count + 1);
		return std::string (buf);
	}

	// Liste compacte des clips du conteneur principal (celui qu'on modifie).
	if (line == "CLIPLIST")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

unsigned char* best = getClipContainer ();

		if (best == nullptr)
			return "ERR conteneur de clips introuvable";

		const int count = *(int*)(best + 0x14);
		const unsigned char* data = *(const unsigned char* const*)(best + 0x08);

		std::ostringstream out;
		out << "CLIPLIST " << count;
		for (int k = 0; k < count && k < 512; k++)
		{
			const unsigned char* r = data + (size_t)k * 0x58;
			out << " " << *(const unsigned*)(r + 0x00)
			    << ":" << *(const unsigned*)(r + 0x08)
			    << ":" << (unsigned)r[0x06]
			    << ":" << (244 - (int)r[0x0C]);
		}
		return out.str ();
	}

	// Suppression d'un clip par sa position.
	if (line.rfind ("CLIPDEL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (8));
		unsigned pos = 0;
		if (!(ss >> pos))
			return "ERR syntaxe CLIPDEL <position> [piste] [motif]";
		int wantTrack = -1, wantPat = -1;
		ss >> wantTrack;
		ss >> wantPat;

unsigned char* best = getClipContainer ();

		if (best == nullptr)
			return "ERR conteneur de clips introuvable";

		const int count = *(int*)(best + 0x14);
		unsigned char* data = *(unsigned char**)(best + 0x08);
		if (count <= 0)
			return "ERR aucun clip";

		int found = -1;
		for (int k = 0; k < count; k++)
		{
			const unsigned char* r = data + (size_t)k * 0x58;
			if (*(const unsigned*)r != pos)
				continue;
			// Sans la piste et le motif, deux clips a la meme position sont
			// indiscernables : on supprimait parfois le mauvais.
			if (wantTrack >= 1 && (244 - (int)r[0x0C]) != wantTrack)
				continue;
			if (wantPat >= 1 && (int)r[0x06] != wantPat)
				continue;
			found = k;
			break;
		}
		if (found < 0)
			return "ERR clip introuvable a cette position";

		// on decale la fin du tableau vers la gauche
		if (found < count - 1)
		{
			memmove (data + (size_t)found * 0x58,
			         data + (size_t)(found + 1) * 0x58,
			         (size_t)(count - found - 1) * 0x58);
		}
		*(int*)(best + 0x14) = count - 1;

		requestRefresh ();

		std::ostringstream out;
		out << "CLIPDELETED " << pos << " (" << count << " -> " << (count - 1) << ")";
		return out.str ();
	}

	// Tous les parametres d'un emplacement, en une seule reponse.
	if (line.rfind ("PARAMS ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (7));
		int track = 0, slot = 0, count = 16;
		if (!(ss >> track >> slot))
			return "ERR syntaxe PARAMS <piste> <slot> [nombre]";
		ss >> count;
		if (track < 0 || track > 125 || slot < 0 || slot > 9 || count < 1 || count > 128)
			return "ERR plage invalide";

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);

		const long long base =
			((long long)track * 0x40 + slot) * 0x10000LL + 0x70008000LL;

		int mode = 0;
		ss >> mode;

		std::ostringstream out;
		out << "PARAMS " << track << " " << slot << " " << count;
		for (int k = 0; k < count; k++)
		{
			int raw;
			if (mode == 1)      raw = getter (base + k, 0x3FD, 1);
			else if (mode == 2) raw = getter (base + k, 0x3FD, 0);
			else                raw = getter (base + k, 0x7FFFFFFF, 1);
			out << " " << raw;
		}
		return out.str ();
	}

	// Tous les effets du mixeur en UNE reponse :
	//   FXALL <n> <piste>.<slot>:<nom>|<piste>.<slot>:<nom>|...
	// Le nom peut contenir des espaces : les elements sont separes par '|'.
	if (line == "FXALL")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::string body;
		int n = 0;
		for (int t = 0; t <= 16; t++)
		{
			if (mixerTrack (t) == nullptr)
				continue;
			for (int s = 0; s < 10; s++)
			{
				unsigned char* sl = mixerSlot (t, s);
				if (sl == nullptr)
					continue;
				const std::string nom = slotName (sl);
				if (nom.empty ())
					continue;
				if (n > 0)
					body += "|";
				body += std::to_string (t) + "." + std::to_string (s) + ":" + nom;
				n++;
			}
		}
		std::ostringstream out;
		out << "FXALL " << n;
		if (n > 0)
			out << " " << body;
		return out.str ();
	}

	// Detail d'un slot : nom, DLL, preset .fst.
	//   FX <piste> <slot>  ->  "FX <piste> <slot> <nom> | <dll> | <preset>"
	if (line.rfind ("FX ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (3));
		int track = -1, slot = -1;
		if (!(ss >> track >> slot))
			return "ERR syntaxe FX <piste> <slot>";

		unsigned char* sl = mixerSlot (track, slot);
		if (sl == nullptr)
			return "ERR slot introuvable";

		const std::string nom = slotName (sl);
		if (nom.empty ())
			return "FXEMPTY " + std::to_string (track) + " " + std::to_string (slot);

		return "FX " + std::to_string (track) + " " + std::to_string (slot) + " " + nom
		     + " | " + widePtr (sl, 0x88, 260)
		     + " | " + widePtr (sl, 0xE8, 260);
	}

	// Tout le mixage d'une piste en UNE reponse : volume, pan, stereo, mute.
	if (line.rfind ("MIX ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (4));
		int track = 0;
		if (!(ss >> track) || track < 0 || track > 125)
			return "ERR syntaxe MIX <piste>";

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);

		const long long base = ((long long)track * 0x40) * 0x10000LL;
		const int vol = getter (base + 0x70001FC0LL, 0x7FFFFFFF, 1);
		const int pan = getter (base + 0x70001FC1LL, 0x7FFFFFFF, 1);
		const int sep = getter (base + 0x70001FC2LL, 0x7FFFFFFF, 1);

		int mute = -1;
		const unsigned char* tracks =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E06A0);
		if (isReadable (tracks, (size_t)track * 0x1478 + 0x20))
			mute = (*(const unsigned char*)(tracks + (size_t)track * 0x1478 + 0x18) != 0) ? 0 : 1;

		char buf[128];
		sprintf_s (buf, "MIX %d %d %d %d %d", track, vol, pan, sep, mute);
		return std::string (buf);
	}

	// TOUT l'etat en UNE reponse : tempo, lecture, pattern, et le mixage des
	// 8 premieres pistes. Chaque aller-retour coute ~30 ms (minuterie de FL),
	// donc une dizaine de requetes par cycle faisait 400 ms de latence.
	// Tout ce qui change vite, en UNE requete : etat + notes du motif courant.
	// Deux requetes coutaient deux fois le delai de la minuterie (~15 ms) ;
	// le cycle de synchronisation tombe donc de 31 ms a ~15 ms.
	if (line == "POLL")
	{
		const std::string etat = executeCommand ("STATUS");
		if (etat.rfind ("ERR", 0) == 0)
			return etat;

		const std::string notes = executeCommand ("NOTES");
		std::string entete = "NOTES 0 0";
		std::string jetons;
		if (notes.rfind ("NOTES ", 0) == 0)
		{
			const size_t fin = notes.find ('\n');
			entete = (fin == std::string::npos) ? notes : notes.substr (0, fin);
			if (fin != std::string::npos)
			{
				const size_t fin2 = notes.find ("\nEND");
				if (fin2 != std::string::npos && fin2 > fin)
					jetons = notes.substr (fin + 1, fin2 - fin - 1);
			}
		}

		std::ostringstream out;
		out << etat << "\n" << entete;
		if (!jetons.empty ())
			out << " " << jetons;
		out << "\nEND 1";
		return out.str ();
	}

	if (line == "STATUS")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const int tempoMilli = *(const int*)((unsigned char*)g_engine + 0x11D8930);
		const int playing = *(const int*)(*(const char* const*)((unsigned char*)g_engine + 0x11E09F8));
		const int pattern = *(const int*)((unsigned char*)g_engine + 0x11D51A0);

		using GetFn = int (*) (long long, int, int);
		GetFn getter = (GetFn)((unsigned char*)g_engine + 0xC51340);
		const unsigned char* tracks =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E06A0);

		std::ostringstream out;
		out << "STATUS " << tempoMilli << " " << (playing ? 1 : 0) << " " << pattern;

		// master (piste 0) : son mute est au meme endroit que celui des pistes.
		{
			const int mv = getter (0x70001FC0LL, 0x7FFFFFFF, 1);
			const int mp = getter (0x70001FC1LL, 0x7FFFFFFF, 1);
			const int ms = getter (0x70001FC2LL, 0x7FFFFFFF, 1);
			int mute = -1;
			if (isReadable (tracks, 0x20))
				mute = (*(const unsigned char*)(tracks + 0x18) != 0) ? 0 : 1;
			out << " " << mv << ":" << mp << ":" << ms << ":" << mute;
		}

		for (int track = 1; track <= FM_MIX_TRACKS; track++)
		{
			const long long base = ((long long)track * 0x40) * 0x10000LL;
			const int vol = getter (base + 0x70001FC0LL, 0x7FFFFFFF, 1);
			const int pan = getter (base + 0x70001FC1LL, 0x7FFFFFFF, 1);
			const int sep = getter (base + 0x70001FC2LL, 0x7FFFFFFF, 1);

			int mute = -1;
			if (isReadable (tracks, (size_t)track * 0x1478 + 0x20))
				mute = (*(const unsigned char*)(tracks + (size_t)track * 0x1478 + 0x18) != 0) ? 0 : 1;

			out << " " << vol << ":" << pan << ":" << sep << ":" << mute;
		}

		// Position de la souris et taille de la zone cliente : le destinataire
		// convertit en proportion, donc une resolution differente ne decale plus
		// le curseur. Inclus ici pour eviter 25 requetes par seconde.
		int mx = -1, my = -1, mw = 0, mh = 0;
		if (g_flWindow != nullptr)
		{
			RECT rc;
			if (GetClientRect (g_flWindow, &rc))
			{
				mw = rc.right;
				mh = rc.bottom;
				POINT p;
				if (GetCursorPos (&p))
				{
					POINT c = p;
					if (ScreenToClient (g_flWindow, &c) &&
					    c.x >= 0 && c.y >= 0 && c.x < rc.right && c.y < rc.bottom)
					{
						mx = c.x;
						my = c.y;
					}
				}
			}
		}
		out << " " << mx << " " << my << " " << mw << " " << mh;
		out << " " << (int)(*(const double*)((unsigned char*)g_engine + 0x156EC88));

		// Bouton de souris tenu : tant que l'utilisateur tient une note, l'agent
		// ne doit ni l'envoyer ni la reecrire. Le clic droit efface des notes.
		const bool tenu = (GetAsyncKeyState (VK_LBUTTON) & 0x8000) != 0
		               || (GetAsyncKeyState (VK_RBUTTON) & 0x8000) != 0;
		out << " " << (tenu ? 1 : 0);

		// Mode de lecture : 0 = PAT, 1 = SONG.
		const int* modeLecture = *(const int* const*)((unsigned char*)g_engine + 0x11E0EF0);
		out << " " << (isReadable (modeLecture, 4) ? *modeLecture : -1);
		return out.str ();
	}

	// Position de lecture : SONGPOS (lecture) ou SONGPOS <valeur> (ecriture)
	// Setter reel : FUN_011E7A10(valeur, drapeaux), appele par le binding
	// setSongPos via un callback differe.
	// La lecture courante est un double a FLEngine+0x156EC88.
	if (line == "SONGPOS")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const double current = *(const double*)((unsigned char*)g_engine + 0x156EC88);
		char buf[96];
		sprintf_s (buf, "SONGPOS %.3f", current);
		return std::string (buf);
	}

	if (line.rfind ("SONGPOS ", 0) == 0)
	{
		// Autorite : ces actions touchent le PROJET, qui appartient a l'hote.
		if (roleBloque ())
			return "ERR action reservee a l'hote";


		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (8));
		double value = 0.0;
		if (!(ss >> value) || value < 0.0)
			return "ERR syntaxe SONGPOS <position>";

		using SetPosFn = void (*) (double, unsigned char);
		SetPosFn setPos = (SetPosFn)((unsigned char*)g_engine + 0xDE7A10);
		setPos (value, 0);

		char buf[96];
		sprintf_s (buf, "SONGPOS %.3f", value);
		return std::string (buf);
	}

	// Nom d'un canal : pointeur UTF-16 a +0x50.
	// Verifie sur les canaux "Sampler" et "Sampler #2".
	auto channelName = [] (const unsigned char* channel) -> std::string
	{
		std::string out;
		if (!isReadable (channel + 0x50, 8))
			return out;

		const unsigned short* text =
			*(const unsigned short* const*)(channel + 0x50);
		if (text == nullptr || !isReadable (text, 2))
			return out;

		for (int i = 0; i < 128; i++)
		{
			if (!isReadable (text + i, 2))
				break;
			const unsigned short code = text[i];
			if (code == 0)
				break;
			if (code == '|' || code == '\n' || code == '\r')
				out += ' ';
			else if (code < 0x80)
				out += (char)code;
			else if (code < 0x800)
			{
				out += (char)(0xC0 | (code >> 6));
				out += (char)(0x80 | (code & 0x3F));
			}
			else
			{
				out += (char)(0xE0 | (code >> 12));
				out += (char)(0x80 | ((code >> 6) & 0x3F));
				out += (char)(0x80 | (code & 0x3F));
			}
		}
		return out;
	};

	// Liste des canaux du channel rack et de leur tranche de mixage.
	//   chemin : p1 = *(FLEngine+0x11E0128) ; conteneur = *p1
	//            tableau = *(conteneur+8) ; nombre = *(conteneur+0x10)
	//   canal  : +0x794 = index, +0x258 = tranche assignee (source de verite,
	//            verifie : ecrire dedans change bien le champ TRACK de FL)
	if (line == "CHANNELS")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const unsigned char* p1 =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E0128);
		if (!isReadable (p1, 8))
			return "ERR channel rack introuvable";
		const unsigned char* container = *(const unsigned char* const*)p1;
		if (!isReadable (container, 0x18))
			return "ERR conteneur canaux introuvable";

		const int count = *(const int*)(container + 0x10);
		const unsigned char* array = *(const unsigned char* const*)(container + 8);
		if (count < 0 || count > 2000 || (count > 0 && !isReadable (array, (size_t)count * 8)))
			return "ERR liste de canaux illisible";

		std::ostringstream out;
		out << "CHANNELS " << count;
		for (int i = 0; i < count; i++)
		{
			const unsigned char* ch = *(const unsigned char* const*)(array + (size_t)i * 8);
			if (ch == nullptr || !isReadable (ch, 0x800))
				continue;
			out << " " << *(const int*)(ch + 0x794)
			    << ":" << *(const int*)(ch + 0x258)
			    << ":" << channelName (ch);
		}
		return out.str ();
	}

	// Assignation d'un canal a une tranche : CHANNELTRACK <index> [tranche]
	if (line.rfind ("CHANNELTRACK ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (13));
		int wanted = 0;
		if (!(ss >> wanted))
			return "ERR syntaxe CHANNELTRACK <index> [tranche]";

		const unsigned char* p1 =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E0128);
		if (!isReadable (p1, 8))
			return "ERR channel rack introuvable";
		const unsigned char* container = *(const unsigned char* const*)p1;
		if (!isReadable (container, 0x18))
			return "ERR conteneur canaux introuvable";

		const int count = *(const int*)(container + 0x10);
		const unsigned char* array = *(const unsigned char* const*)(container + 8);
		if (count <= 0 || count > 2000 || !isReadable (array, (size_t)count * 8))
			return "ERR liste de canaux illisible";

		unsigned char* found = nullptr;
		for (int i = 0; i < count; i++)
		{
			unsigned char* ch = *(unsigned char* const*)(array + (size_t)i * 8);
			if (ch == nullptr || !isReadable (ch, 0x800))
				continue;
			if (*(int*)(ch + 0x794) == wanted)
			{
				found = ch;
				break;
			}
		}
		if (found == nullptr)
			return "ERR canal introuvable";

		int track = -1;
		if (ss >> track)
		{
			if (track < 0 || track > 125)
				return "ERR tranche hors plage";
			*(int*)(found + 0x258) = track;
			requestRefresh ();
		}
		else
		{
			track = *(int*)(found + 0x258);
		}

		std::ostringstream out;
		out << "CHANNELTRACK " << wanted << " " << track;
		return out.str ();
	}

	// Reglages d'un canal du channel rack.
	//   volume : identifiant index << 16      , brut 0..12800
	//   pan    : identifiant (index << 16) + 1, brut 0..12800, centre 6400
	// Lecture par le repartiteur FUN_01046A70(id, 0, 2) ; ecriture par
	// FUN_01051D60 comme les tranches du mixeur (propriete 0x3FD).
	if (line.rfind ("CHANNELVOL ", 0) == 0 || line.rfind ("CHANNELPAN ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const bool isPan = (line.rfind ("CHANNELPAN ", 0) == 0);
		std::istringstream ss (line.substr (isPan ? 11 : 11));
		int index = -1;
		if (!(ss >> index) || index < 0 || index > 255)
			return "ERR syntaxe CHANNELVOL|CHANNELPAN <index> [valeur]";

		const long long handle = ((long long)index << 16) + (isPan ? 1 : 0);

		double wanted = -999.0;
		if (ss >> wanted)
		{
			if (isPan)
			{
				if (wanted < -1.0) wanted = -1.0;
				if (wanted > 1.0) wanted = 1.0;
			}
			else
			{
				if (wanted < 0.0) wanted = 0.0;
				if (wanted > 1.0) wanted = 1.0;
			}

			// L'ECRITURE attend l'echelle 2^30 (propriete 0x3FD), alors que la
			// LECTURE renvoie 0..12800. Deux echelles differentes : ecrire
			// 7680 au lieu de 0.6*2^30 mettait le canal a zero, en silence.
			const double normalise = isPan ? (wanted + 1.0) * 0.5 : wanted;
			const int raw = (int)(normalise * 1073741824.0 + 0.5);

			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			setter ((int)handle, raw, 0x3FD);
			requestRefresh ();
		}

		using GetFn = int (*) (long long, int, int);
		GetFn read = (GetFn)((unsigned char*)g_engine + 0xC46A70);
		const int raw = read (handle, 0, 2);

		const double value = isPan ? ((double)raw / 6400.0 - 1.0)
		                           : ((double)raw / 12800.0);

		char buf[128];
		sprintf_s (buf, "%s %d %.6f (%d)", isPan ? "CHANNELPAN" : "CHANNELVOL",
		           index, value, raw);
		return std::string (buf);
	}

	// Volume et panoramique de TOUS les canaux, en une seule requete.
	// Format : CHANNELMIX <n> <index>:<volume>:<pan> ...
	if (line == "CHANNELMIX")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const unsigned char* p1 =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E0128);
		if (!isReadable (p1, 8))
			return "ERR channel rack introuvable";
		const unsigned char* container = *(const unsigned char* const*)p1;
		if (!isReadable (container, 0x18))
			return "ERR conteneur canaux introuvable";

		const int count = *(const int*)(container + 0x10);
		const unsigned char* array = *(const unsigned char* const*)(container + 8);
		if (count < 0 || count > 2000 || (count > 0 && !isReadable (array, (size_t)count * 8)))
			return "ERR liste de canaux illisible";

		using GetFn = int (*) (long long, int, int);
		GetFn read = (GetFn)((unsigned char*)g_engine + 0xC46A70);

		std::ostringstream out;
		out << "CHANNELMIX " << count;
		for (int i = 0; i < count; i++)
		{
			const unsigned char* ch = *(const unsigned char* const*)(array + (size_t)i * 8);
			if (ch == nullptr || !isReadable (ch, 0x800))
				continue;

			const int index = *(const int*)(ch + 0x794);
			const long long base = (long long)index << 16;
			const int rawVol = read (base, 0, 2);
			const int rawPan = read (base + 1, 0, 2);

			const int rawMute = read (base + 7, 0, 2);

			out << " " << index
			    << ":" << ((double)rawVol / 12800.0)
			    << ":" << ((double)rawPan / 6400.0 - 1.0)
			    << ":" << (rawMute ? 1 : 0);
		}
		return out.str ();
	}

	// Sourdine d'un canal : identifiant (index << 16) + 7.
	// Lecture : 1 = actif, 0 = muet. Ecriture : valeur brute passee telle
	// quelle au setter, le temps de determiner son echelle.
	if (line.rfind ("CHANNELMUTE ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		std::istringstream ss (line.substr (12));
		int index = -1;
		if (!(ss >> index) || index < 0 || index > 255)
			return "ERR syntaxe CHANNELMUTE <index> [brut]";

		const long long handle = ((long long)index << 16) + 7;

		int voulu = -1;
		if (ss >> voulu)
		{
			// L'ecriture attend l'echelle 2^30 : 0 = muet, 2^30 = actif.
			using SetFn = int (*) (int, int, int);
			SetFn setter = (SetFn)((unsigned char*)g_engine + 0xC46A70);
			setter ((int)handle, voulu ? 1073741824 : 0, 0x3FD);
			requestRefresh ();
		}

		using GetFn = int (*) (long long, int, int);
		GetFn read = (GetFn)((unsigned char*)g_engine + 0xC46A70);
		const int now = read (handle, 0, 2);

		char buf[96];
		sprintf_s (buf, "CHANNELMUTE %d %d (%d)", index, now ? 1 : 0, now);
		return std::string (buf);
	}

	// Cree un canal avec un VRAI plugin dedans, comme le fait le navigateur de FL.
	//
	// La sequence exacte vient de FUN_01098830 (le code du navigateur) :
	//   canal = FUN_011F53D0(*(void**)FLEngine+0x11E0FF8, 0)
	//   (*(vtable + 0x150))(canal, chemin_en_chaine_Delphi, 0, drapeaux, 0, 0)
	//
	// C'est la methode +0x150 qui instancie le plugin ; FUN_010010F0, appelee
	// seule, ne faisait que lire le nom du preset (verifie : le canal prenait le
	// bon nom mais restait un sampler vide).
	// Verifie le 28/10 a 20h45 : apres cet appel, l'interface de FabFilter One
	// s'ouvre vraiment, et le module du VST est charge dans FL.
	// Canal vide (Sampler), pour garder les positions du rack alignees quand
	// l'instrument de l'autre poste n'existe pas ici.
	// Supprime un canal du rack.
	//
	//   DELCHANNEL <index> [brut]
	//
	// 46-o : on ne refait plus le travail de FL a la main. On appelle sa propre
	// suppression de canal, FUN_01004a60 (RVA 0xC04A60), exactement comme son
	// interface le fait (voir FUN_01004a60 : elle construit un contexte par
	// FUN_010049f0 puis appelle FUN_01004820).
	//
	// Ce qu'elle fait, MESURE en direct le 30/10 sur FL 26.1.7 (v46-o) :
	//   - enregistre l'operation dans l'historique (Â« Delete channel <nom> Â») ;
	//   - detache puis LIBERE l'objet canal ;
	//   - retire l'entree de la liste maitresse ;
	//   - renumerote +0x794 des canaux restants (mesure : 1 -> 0) ;
	//   - RETIRE les notes du canal et RENUMEROTE celles des canaux suivants
	//     dans tous les motifs (mesure : 3 notes -> 2, canal 1 -> canal 0) ;
	//   - reconstruit le rack (FUN_01185ED0) et NOTE les vues (FUN_00fdf060).
	//     C'est cette notification-la qui remet a jour les objets d'affichage
	//     dont le +0x18 pointait le canal supprime : l'oubli qui faisait
	//     planter FL des le premier rafraichissement (0xED7D36, lecture a
	//     [RAX+0x38c] avec RAX nul apres le getter FUN_00fe8bf0).
	//
	// Donc : plus de notes a renumeroter nous-memes (ce serait un double
	// decalage), plus de liste a decaler, plus de canal a laisser fuir.
	//
	// Â« brut Â» garde l'ancien chemin (decalage + renumerotation a la main) :
	// il sert de temoin pour comparer, et de repli sans recompiler.
	if (line.rfind ("DELCHANNEL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int index = -1;
		if (sscanf_s (line.c_str () + 11, "%d", &index) != 1)
			return "ERR syntaxe DELCHANNEL <index>";

		const bool manuel = (line.find ("brut") != std::string::npos);

		// LISTE MAITRESSE (RVA 0x11E2218), PAS celle que lit CHANNELS (0x11E0128).
		// Verifie le 30/10 : decrementer la liste de 0x11E0128 ne sert a rien,
		// la reconstruction (0xD85ED0) la repeuple depuis la maitresse â€” le
		// canal supprime revenait aussitot.
		const unsigned char* pm =
			*(const unsigned char* const*)((unsigned char*)g_engine + 0x11E2218);
		if (!isReadable (pm, 8))
			return "ERR liste maitresse illisible";
		unsigned char* conteneur = *(unsigned char* const*)pm;
		if (!isReadable (conteneur, 0x18))
			return "ERR conteneur illisible";

		const int n = *(const int*)(conteneur + 0x10);
		if (index < 0 || index >= n)
			return "ERR index " + std::to_string (index) + " hors rack ("
			       + std::to_string (n) + " canal(aux))";

		void** tab = *(void***)(conteneur + 0x08);
		if (!isReadable (tab, (size_t)n * 8))
			return "ERR tableau illisible";

		// ---- chemin par defaut (46-o) : c'est FL qui supprime -------------
		if (!manuel)
		{
			// Comptage en LECTURE SEULE, uniquement pour rendre la meme
			// reponse qu'avant (l'agent lit ces trois nombres par position).
			int notesSupprimees = 0;
			int notesDecalees = 0;
			for (int pat = 1; pat <= 999; ++pat)
			{
				unsigned char* list = noteList (pat);
				if (list == nullptr)
					continue;
				const int total = *(const int*)(list + 0x14);
				if (total <= 0)
					continue;
				const unsigned char* data = *(const unsigned char**)(list + 0x08);
				if (!isReadable (data, (size_t)total * 24))
					continue;
				for (int k = 0; k < total; ++k)
				{
					const int ch = noteChannelOf (data + (size_t)k * 24);
					if (ch == index)
						notesSupprimees++;
					else if (ch > index)
						notesDecalees++;
				}
			}

			using DeleteFn = void (*) (unsigned, int, unsigned long long);
			DeleteFn supprimer = (DeleteFn)((unsigned char*)g_engine + 0xC04A60);

			// La liste des etapes (RVA 0x11E1C78) et le nom affiche dans
			// l'historique de FL. Â« sanshisto Â» garde l'ancien appel pour comparer.
			using OuvrirEtapeFn = void (*) (void*, void*, unsigned);
			OuvrirEtapeFn ouvrirEtape = nullptr;
			void* historique = nullptr;
			void* nomEtape = nullptr;
			if (line.find ("sanshisto") == std::string::npos)
			{
				void** global = *(void***)((unsigned char*)g_engine + 0x11E1C78);
				if (!isReadable (global, 8) || *global == nullptr)
					return "ERR historique illisible";
				historique = *global;
				using ToDelphi = void* (*) (void*, const char*);
				((ToDelphi)((unsigned char*)g_engine + 0x1C6E0)) (&nomEtape, "Delete channel");
				if (nomEtape == nullptr)
					return "ERR chaine nom";
				ouvrirEtape = (OuvrirEtapeFn)((unsigned char*)g_engine + 0xC19360);
			}

			const unsigned long faute = guardedDeleteChannel (
				supprimer, ouvrirEtape, historique, nomEtape, (unsigned)index);

			if (faute != 0)
			{
				std::ostringstream err;
				err << "ERR suppression refusee : FAUTE=0x"
				    << std::hex << faute << std::dec;
				return err.str ();
			}

			requestRefresh ();

			// FL garantit qu'il reste au moins un canal : s'il n'en restait
			// qu'un, la suppression en recree un. On relit donc le compte
			// reel au lieu d'annoncer n-1.
			const int reste = isReadable (conteneur, 0x18)
				? *(const int*)(conteneur + 0x10) : (n - 1);

			std::ostringstream out;
			out << "DELCHANNEL OK " << index << " (" << n << " -> " << reste
			    << ", " << notesSupprimees << " note(s) retiree(s), "
			    << notesDecalees << " decalee(s))";
			return out.str ();
		}

		// ---- chemin Â« brut Â» : l'ancien travail a la main -----------------
		// Garde comme temoin (il laisse les objets d'affichage perimes, donc
		// il plante au rafraichissement : c'est ce qu'on a corrige).
		for (int k = index; k < n - 1; ++k)
			tab[k] = tab[k + 1];
		tab[n - 1] = nullptr;
		*(int*)(conteneur + 0x10) = n - 1;

		// RENUMEROTATION. Chaque canal porte son propre numero a +0x794, et
		// c'est lui qui sert a fabriquer l'identifiant de mixeur (numero << 16).
		// Sans cette boucle, le canal restant garde son ancien numero : verifie
		// le 30/10, CHANNELS affichait encore Â« 2 Â» et CHANNELMIX rendait des
		// valeurs absurdes (pan -1.00016) parce que le handle visait le mauvais
		// emplacement.
		for (int k = index; k < n - 1; ++k)
		{
			unsigned char* ch = (unsigned char*)tab[k];
			if (ch == nullptr || !isReadable (ch + 0x794, 4))
				continue;
			*(int*)(ch + 0x794) = k;
		}

		// RENUMEROTATION DES NOTES, dans TOUS les motifs.
		//
		// Chaque note porte son canal a +0x06 (voir fillNote). Sans cette passe :
		//   - les notes des canaux suivants visent le mauvais instrument ;
		//   - celles du canal supprime restent orphelines sur un canal qui
		//     n'existe plus.
		int notesSupprimees = 0;
		int notesDecalees = 0;
		for (int pat = 1; pat <= 999; ++pat)
		{
			unsigned char* list = noteList (pat);
			if (list == nullptr)
				continue;
			const int total = *(const int*)(list + 0x14);
			if (total <= 0)
				continue;
			unsigned char* data = *(unsigned char**)(list + 0x08);
			if (!isReadable (data, (size_t)total * 24))
				continue;

			int kept = 0;
			for (int k = 0; k < total; ++k)
			{
				unsigned char* note = data + (size_t)k * 24;
				const unsigned ch = (unsigned)noteChannelOf (note);
				if ((int)ch == index)
				{
					notesSupprimees++;
					continue;
				}
				if ((int)ch > index)
				{
					*(unsigned short*)(note + 0x06) = (unsigned short)(ch - 1);
					notesDecalees++;
				}
				if (kept != k)
					memcpy (data + (size_t)kept * 24, note, 24);
				kept++;
			}
			*(int*)(list + 0x14) = kept;
		}

		// reconstruction des listes de FL
		using RebuildFn = void (*) ();
		RebuildFn rebuild = (RebuildFn)((unsigned char*)g_engine + 0xD85ED0);
		rebuild ();

		requestRefresh ();
		return "DELCHANNEL OK " + std::to_string (index) + " (" + std::to_string (n)
		       + " -> " + std::to_string (n - 1) + ", " + std::to_string (notesSupprimees)
		       + " note(s) retiree(s), " + std::to_string (notesDecalees)
		       + " decalee(s))";
	}

	// Copie un canal entier (sample, plugin, reglages) dans un fichier .fst.
	//   SAVECHANNEL <index> <chemin absolu>
	if (line.rfind ("SAVECHANNEL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int index = -1;
		int lu = 0;
		if (sscanf_s (line.c_str () + 12, "%d %n", &index, &lu) != 1 || lu == 0)
			return "ERR syntaxe SAVECHANNEL <index> <chemin>";
		const std::string chemin = line.substr (12 + (size_t)lu);
		if (chemin.size () < 8 || chemin.size () > 500)
			return "ERR syntaxe SAVECHANNEL <index> <chemin>";

		unsigned char* base = (unsigned char*)g_engine;

		const unsigned char* pm = *(const unsigned char* const*)(base + 0x11E2218);
		if (!isReadable (pm, 8))
			return "ERR liste maitresse illisible";
		unsigned char* conteneur = *(unsigned char* const*)pm;
		if (!isReadable (conteneur, 0x18))
			return "ERR conteneur illisible";

		const int n = *(const int*)(conteneur + 0x10);
		if (index < 0 || index >= n)
			return "ERR index " + std::to_string (index) + " hors rack ("
			       + std::to_string (n) + " canal(aux))";

		void** tab = *(void***)(conteneur + 0x08);
		if (!isReadable (tab, (size_t)n * 8) || !isReadable (tab[index], 0x800))
			return "ERR canal illisible";

		using ToDelphi = void* (*) (void*, const char*);
		void* nomFichier = nullptr;
		((ToDelphi)(base + 0x1C6E0)) (&nomFichier, chemin.c_str ());
		if (nomFichier == nullptr)
			return "ERR chaine chemin";

		unsigned long faute = 0;
		const int rc = guardedSaveChannel ((ChannelFileFn)(base + 0xB54570), nomFichier,
		                                   tab[index], &faute);
		if (faute != 0)
		{
			char code[16];
			sprintf_s (code, "0x%08lX", faute);
			return std::string ("ERR SAVECHANNEL faute ") + code;
		}
		if (rc != 0)
			return "ERR SAVECHANNEL code " + std::to_string (rc);
		return "SAVECHANNEL OK " + std::to_string (index);
	}

	// Recree un canal depuis un fichier .fst de SAVECHANNEL.
	//   LOADCHANNEL <index> <chemin absolu>[|<nom>]
	// Le canal est insere a <index> (ramene a la fin du rack s'il depasse).
	//   REPLACECHANNEL <index> <chemin absolu>[|<nom>]
	// Le meme contenu, dans le canal <index> : ses notes ne bougent pas.
	const bool remplaceCanal = line.rfind ("REPLACECHANNEL ", 0) == 0;
	if (remplaceCanal || line.rfind ("LOADCHANNEL ", 0) == 0)
	{
		const size_t debut = remplaceCanal ? 15 : 12;
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		int index = -1;
		int lu = 0;
		if (sscanf_s (line.c_str () + debut, "%d %n", &index, &lu) != 1 || lu == 0 || index < 0)
			return "ERR syntaxe LOADCHANNEL <index> <chemin>[|<nom>]";
		std::string chemin = line.substr (debut + (size_t)lu);
		std::string nom;
		const size_t barre = chemin.find ('|');
		if (barre != std::string::npos)
		{
			nom = chemin.substr (barre + 1);
			chemin.resize (barre);
		}
		if (chemin.size () < 8 || chemin.size () > 500)
			return "ERR syntaxe LOADCHANNEL <index> <chemin>[|<nom>]";

		unsigned char* base = (unsigned char*)g_engine;

		const unsigned char* pm = *(const unsigned char* const*)(base + 0x11E2218);
		if (!isReadable (pm, 8))
			return "ERR liste maitresse illisible";
		const unsigned char* conteneur = *(const unsigned char* const*)pm;
		if (!isReadable (conteneur, 0x18))
			return "ERR conteneur illisible";
		const int n = *(const int*)(conteneur + 0x10);
		void* existant = nullptr;
		if (remplaceCanal)
		{
			if (index >= n)
				return "ERR index " + std::to_string (index) + " hors rack ("
				       + std::to_string (n) + " canal(aux))";
			void** tab = *(void***)(conteneur + 0x08);
			if (!isReadable (tab, (size_t)n * 8) || !isReadable (tab[index], 0x800))
				return "ERR canal illisible";
			existant = tab[index];
		}
		else if (index > n)
			index = n;

		using ToDelphi = void* (*) (void*, const char*);
		ToDelphi toDelphi = (ToDelphi)(base + 0x1C6E0);
		void* nomFichier = nullptr;
		toDelphi (&nomFichier, chemin.c_str ());
		if (nomFichier == nullptr)
			return "ERR chaine chemin";

		using InsertFn = void* (*) (int, int, int);
		int rc = 0;
		unsigned long faute = 0;
		unsigned char* channel = (unsigned char*)guardedLoadChannel (
			(InsertFn)(base + 0xC04230), (ChannelFileFn)(base + 0xB54760), existant, index,
			nomFichier, &rc, &faute);
		if (faute != 0)
		{
			char code[16];
			sprintf_s (code, "0x%08lX", faute);
			return std::string ("ERR LOADCHANNEL faute ") + code;
		}
		if (channel == nullptr || !isReadable (channel, 0x800))
			return "ERR creation du canal";
		if (rc != 0)
			return "ERR LOADCHANNEL code " + std::to_string (rc);

		if (!nom.empty ())
		{
			void* nomCanal = nullptr;
			toDelphi (&nomCanal, nom.c_str ());
			void** vtable = *(void***)channel;
			if (nomCanal != nullptr && isReadable (vtable + 0x70 / 8, 8))
				((void (*) (void*, void*))vtable[0x70 / 8]) (channel, nomCanal);
		}

		// Fin de l'annulation de FL : FUN_00e39560 sur son objet global.
		void* const* vue = *(void* const* const*)(base + 0x11E35C8);
		if (isReadable (vue, 8) && *vue != nullptr)
			((void (*) (void*))(base + 0xA39560)) (*vue);

		requestRefresh ();
		return "LOADCHANNEL OK " + std::to_string (*(const int*)(channel + 0x794));
	}

	if (line == "NEWCHANNEL")
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned char* base = (unsigned char*)g_engine;
		void** selfSlot = (void**)(base + 0x11E0FF8);
		if (!isReadable (selfSlot, 8))
			return "ERR contexte de creation illisible";
		void* self = *selfSlot;

		using CreateFn = void* (*) (void*, unsigned char);
		CreateFn create = (CreateFn)(base + 0xDF53D0);
		void* channel = create (self, 0);
		if (channel == nullptr || !isReadable (channel, 8))
			return "ERR creation du canal";

		const int index = *(int*)((unsigned char*)channel + 0x794);
		requestRefresh ();
		return "NEWCHANNEL OK " + std::to_string (index);
	}

	if (line.rfind ("LOADPLUGIN ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		const std::string chemin = line.substr (11);
		if (chemin.size () < 8 || chemin.size () > 500)
			return "ERR syntaxe LOADPLUGIN <chemin .fst absolu>";

		unsigned char* base = (unsigned char*)g_engine;

		using ToDelphi = void* (*) (void*, const char*);
		ToDelphi toDelphi = (ToDelphi)(base + 0x1C6E0);

		void* preset = nullptr;
		toDelphi (&preset, chemin.c_str ());
		if (preset == nullptr)
			return "ERR chaine chemin";

		// Le navigateur passe un "self" non nul : le global +0x11E0FF8.
		void** selfSlot = (void**)(base + 0x11E0FF8);
		if (!isReadable (selfSlot, 8))
			return "ERR contexte de creation illisible";
		void* self = *selfSlot;

		using CreateFn = void* (*) (void*, unsigned char);
		CreateFn create = (CreateFn)(base + 0xDF53D0);
		void* channel = create (self, 0);
		if (channel == nullptr || !isReadable (channel, 8))
			return "ERR creation du canal";

		const int index = *(int*)((unsigned char*)channel + 0x794);

		void** vtable = *(void***)channel;
		if (vtable == nullptr || !isReadable (vtable + 0x150 / 8, 8))
			return "ERR table de methodes illisible";
		void* method = vtable[0x150 / 8];

		// Drapeaux : ceux que le navigateur construit (debordements compris).
		unsigned flags = *(const unsigned short*)(base + 0xC98B54);
		using OptFn = char (*) (int);
		OptFn option = (OptFn)(base + 0x4188F0);
		if (option (0x12) != 0)
			flags |= 0x40;

		using LoadMethod = void (*) (void*, void*, int, int, void*, void*);
		LoadMethod load = (LoadMethod)method;
		load (channel, preset, 0, (int)flags, nullptr, nullptr);

		requestRefresh ();
		return "LOADPLUGIN OK (canal " + std::to_string (index) + " " + std::to_string (flags) + ")";
	}

	// Canal en cours d'edition du piano roll (CURCHANNEL) et selection
	// (SELECTCHANNEL <index>). C'est ce canal qui determine OU les notes sont
	// ecrites : la liste de notes n'existe pas par canal dans le rack.
	if (line == "CURCHANNEL" || line.rfind ("SELECTCHANNEL ", 0) == 0)
	{
		if (g_engine == nullptr)
			return "ERR pas d'adresse";

		unsigned char* base = (unsigned char*)g_engine;

		if (line.rfind ("SELECTCHANNEL ", 0) == 0)
		{
			std::istringstream ss (line.substr (14));
			int index = -1;
			if (!(ss >> index) || index < 0 || index > 255)
				return "ERR syntaxe SELECTCHANNEL <index>";

			using SelFn = void (*) (int);
			SelFn select = (SelFn)(base + 0xDE84E0);
			select (index);
			requestRefresh ();
		}

		using GetSel = int (*) (int, int, int);
		GetSel getSel = (GetSel)(base + 0xDE82C0);
		const int now = getSel (0, 0, 0);

		char buf[96];
		sprintf_s (buf, "CURCHANNEL %d", now);
		return std::string (buf);
	}

	return "ERR commande inconnue";
}

// Appelee sur le thread principal de FL uniquement.
// Conteneur de clips avec CACHE : un balayage memoire complet coute des
// centaines de millisecondes et bloque le thread principal de FL. On garde donc
// l'adresse tant qu'elle reste valide, et on ne rebalaye qu'en cas d'echec.
static unsigned char* g_clipCache = nullptr;

static bool validClipContainer (unsigned char* c)
{
	if (c == nullptr || g_engine == nullptr)
		return false;
	if (!isReadable (c, 0x20))
		return false;
	const unsigned long long cls =
		(unsigned long long)((unsigned char*)g_engine + 0xC54168);
	if (*(const unsigned long long*)c != cls)
		return false;
	if (*(const int*)(c + 0x10) != 0x58)
		return false;
	const int cnt = *(const int*)(c + 0x14);
	const int cap = *(const int*)(c + 0x18);
	if (cnt < 0 || cnt > 5000 || cap < cnt || cap > 200000)
		return false;
	return true;
}

static unsigned char* getClipContainer ()
{
	if (validClipContainer (g_clipCache))
		return g_clipCache;

	g_clipCache = nullptr;

	const unsigned long long cls =
		(unsigned long long)((unsigned char*)g_engine + 0xC54168);
	unsigned char pattern[8];
	memcpy (pattern, &cls, 8);

	unsigned char* best = nullptr;
	int bestCount = -1;

	MEMORY_BASIC_INFORMATION mbi;
	unsigned char* addr = nullptr;
	while (VirtualQuery (addr, &mbi, sizeof (mbi)) == (SIZE_T)sizeof (mbi))
	{
		unsigned char* base = (unsigned char*)mbi.BaseAddress;
		const size_t size = mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
		    size >= 0x20 && size <= 64u * 1024 * 1024)
		{
			for (size_t i = 0; i + 0x20 <= size; i += 8)
			{
				if (memcmp (base + i, pattern, 8) != 0)
					continue;
				unsigned char* cand = base + i;
				const int es = *(const int*)(cand + 0x10);
				const int cnt = *(const int*)(cand + 0x14);
				const int cap = *(const int*)(cand + 0x18);
				const unsigned char* d = *(const unsigned char* const*)(cand + 0x08);
				if (es != 0x58 || cnt < 0 || cnt > 5000 || cap < cnt || cap > 200000)
					continue;
				if (cnt > 0 && !isReadable (d, (size_t)cnt * 0x58))
					continue;
				if (cnt > bestCount)
				{
					bestCount = cnt;
					best = cand;
				}
			}
		}
		unsigned char* next = base + size;
		if (next <= addr) break;
		addr = next;
	}

	if (best != nullptr)
		g_clipCache = best;
	return best;
}

// Reecrit la longueur d'un clip existant, identifie par sa position.
static void applyClipLength (unsigned pos, unsigned len)
{
	if (g_engine == nullptr)
		return;

	const unsigned long long cls =
		(unsigned long long)((unsigned char*)g_engine + 0xC54168);
	unsigned char pattern[8];
	memcpy (pattern, &cls, 8);

	MEMORY_BASIC_INFORMATION mbi;
	unsigned char* addr = nullptr;
	while (VirtualQuery (addr, &mbi, sizeof (mbi)) == (SIZE_T)sizeof (mbi))
	{
		unsigned char* base = (unsigned char*)mbi.BaseAddress;
		const size_t size = mbi.RegionSize;
		if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
		    size >= 0x20 && size <= 64u * 1024 * 1024)
		{
			for (size_t i = 0; i + 0x28 <= size; i += 8)
			{
				if (memcmp (base + i, pattern, 8) != 0)
					continue;
				unsigned char* cand = base + i;
				const int es = *(const int*)(cand + 0x10);
				const int cnt = *(const int*)(cand + 0x14);
				const int cap = *(const int*)(cand + 0x18);
				unsigned char* d = *(unsigned char**)(cand + 0x08);
				if (es != 0x58 || cnt <= 0 || cnt > 5000 || cap < cnt || cap > 200000)
					continue;
				if (!isReadable (d, (size_t)cnt * 0x58))
					continue;
				for (int k = 0; k < cnt; k++)
				{
					unsigned char* r = d + (size_t)k * 0x58;
					if (*(unsigned*)r == pos)
					{
						*(unsigned*)(r + 0x08) = len;
						*(unsigned*)(r + 0x1C) = len;
						return;
					}
				}
			}
		}
		unsigned char* next = base + size;
		if (next <= addr) break;
		addr = next;
	}
}

// Arrete tout ce que ce plugin fait tourner en dehors du thread de FL.
//
// Indispensable avant un chargement de projet : le projet remplace decharge
// notre DLL, et si un seul de nos threads vit encore a ce moment-la, son code
// disparait sous ses pieds. C'est le plantage Â« FruityMultiBridge.vst3_unloaded Â»
// observe trois fois le 30/10 (offsets 0x18baa, 0x1569b, 0x156f6).
// Rend false si un thread n'a pas pu etre arrete : dans ce cas il ne faut PAS
// appeler le chargeur. Sinon on lui passe un thread encore vivant qui attend une
// reponse du thread principal â€” celui-la meme qui va entrer dans le chargeur.
// C'est l'incident du 30/10 14h49 : Â« arret : serveur -> 258 Â» (WAIT_TIMEOUT)
// suivi d'un chargement qui n'a jamais rendu la main.
static bool arreterNosThreads ()
{
	bool calme = true;
	if (g_timerId != 0)
	{
		KillTimer (nullptr, g_timerId);
		destroyWakeWindow ();
		g_timerId = 0;
		logLine ("arret : minuterie coupee");
	}

	InterlockedExchange (&g_serverWanted, 0);
	InterlockedExchange (&g_stopServer, 1);

	if (g_srv != INVALID_SOCKET)
	{
		closesocket (g_srv);
		g_srv = INVALID_SOCKET;
	}
	if (g_cursorWnd != nullptr)
		PostMessageA (g_cursorWnd, WM_CLOSE, 0, 0);
	if (g_barThreadId != 0)
		PostThreadMessageA ((DWORD)g_barThreadId, WM_QUIT, 0, 0);
	if (g_cursorThreadId != 0)
		PostThreadMessageA ((DWORD)g_cursorThreadId, WM_QUIT, 0, 0);

	if (g_serverThread != nullptr)
	{
		const DWORD r = WaitForSingleObject (g_serverThread, 1500);
		logLine ("arret : serveur -> %lu", (unsigned long)r);
		if (r == WAIT_TIMEOUT)
			calme = false;
		CloseHandle (g_serverThread);
		g_serverThread = nullptr;
	}
	if (g_cursorThread != nullptr)
	{
		const DWORD r = WaitForSingleObject (g_cursorThread, 1500);
		logLine ("arret : curseur -> %lu", (unsigned long)r);
		if (r == WAIT_TIMEOUT)
			calme = false;
		CloseHandle (g_cursorThread);
		g_cursorThread = nullptr;
	}
	if (g_barThread != nullptr)
	{
		const DWORD r = WaitForSingleObject (g_barThread, 1500);
		logLine ("arret : barre -> %lu", (unsigned long)r);
		if (r == WAIT_TIMEOUT)
			calme = false;
		CloseHandle (g_barThread);
		g_barThread = nullptr;
	}
	logLine ("arret : threads arretes : %s", calme ? "oui" : "NON");
	return calme;
}

// Relance ce que arreterNosThreads() a coupe.
//
// Sert uniquement quand un chargement de projet a ECHOUE : l'instance existe
// encore, il faut lui rendre sa minuterie et son serveur. Quand le chargement
// a reussi, terminate() a detruit l'instance et on ne relance rien.
static void demarrerNosThreads ()
{
	if (kShowToolbar && g_barThread == nullptr)
	{
		HANDLE bar = CreateThread (nullptr, 0, barThread, nullptr, 0, nullptr);
		if (bar != nullptr)
			g_barThread = bar;
	}
	if (g_cursorThread == nullptr)
	{
		HANDLE cursor = CreateThread (nullptr, 0, cursorThread, nullptr, 0, nullptr);
		if (cursor != nullptr)
			g_cursorThread = cursor;
	}
	if (g_serverThread == nullptr)
	{
		HANDLE thread = CreateThread (nullptr, 0, serverThread, nullptr, 0, nullptr);
		if (thread != nullptr)
			g_serverThread = thread;
		else
			logLine ("relance          : CreateThread serveur echec");
	}
}

// Charge le projet pose par LOADPROJECT.
//
// Tourne sur le thread principal de FL, depuis la minuterie : aucune commande
// n'attend de reponse, donc le remplacement du projet peut decharger ce plugin
// sans que la pile d'appel revienne dans du code libere.
static void doLoadProject ()
{
	if (g_engine == nullptr)
		return;

	// EPINGLE le DLL dans le processus, avant tout le reste.
	//
	// Charger un projet detruit notre plugin : FL appelle terminate() PUIS
	// decharge le DLL, alors que notre pile d'appel y est encore (mesure du
	// 30/10 : Â« decharge Â» 12 ms apres l'appel au chargeur). Sans epinglage,
	// le retour de fonction atterrit dans du code demappe â€” c'est la faute
	// Â« FruityMultiBridge.vst3_unloaded Â» vue trois fois.
	//
	// GET_MODULE_HANDLE_EX_FLAG_PIN : le module reste charge jusqu'a la fin du
	// processus. Cout : quelques Mo qui ne sont pas rendus. Prix acceptable
	// pour que le chargement d'un projet ne tue pas FL.
	HMODULE soi = nullptr;
	GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                    | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                    (LPCSTR)(const void*)&doLoadProject, &soi);
	logLine ("chargement : DLL epingle -> %p", (void*)soi);

	// TOUT doit etre arrete avant : le chargement remplace le projet, donc FL
	// decharge ce DLL juste apres. Un seul thread survivant = faute dans du
	// code demappe.
	logLine ("chargement : arret des threads avant LOADPROJECT");
	if (!arreterNosThreads ())
	{
		// Un thread n'a pas voulu mourir (typiquement le serveur qui traite une
		// commande). Charger maintenant lui passerait un thread vivant qui attend
		// le thread principal, lequel entre dans le chargeur : blocage garanti.
		InterlockedExchange (&g_loadStatus, -3);
		logLine ("chargement : ANNULE (thread encore vivant) -> on relance");
		InterlockedExchange (&g_stopServer, 0);
		InterlockedExchange (&g_serverWanted, 1);
		g_timerId = SetTimer (nullptr, 0, 6, fmTimerProc);
		demarrerNosThreads ();
		return;
	}
	logLine ("chargement : appel du chargeur de FL");

	using ToDelphi = void* (*) (void*, const char*);
	ToDelphi toDelphi = (ToDelphi)((unsigned char*)g_engine + 0x1C6E0);
	void* chaine = nullptr;
	toDelphi (&chaine, g_loadPath);

	int rc = -1;
	if (chaine != nullptr)
	{
		using LoadFn = int (*) (void*, int, int, int);
		LoadFn load = (LoadFn)((unsigned char*)g_engine + 0xB54760);
		rc = load (chaine, 0, 0, 0);
	}

	// Le resultat est publie AVANT toute autre chose : c est ce que la nouvelle
	// instance (si le chargement a reussi) doit lire via LOADSTATUS.
	InterlockedExchange (&g_loadStatus, rc);

	// On arrive ici dans deux cas tres differents :
	//  - le chargement a echoue (fichier refuse, chemin invalide) : l'instance
	//    existe encore, il FAUT relancer minuterie et serveur, sinon le plugin
	//    reste muet pour de bon ;
	//  - le chargement a reussi : FL a appele terminate(), l'instance est
	//    detruite et on ne doit surtout plus rien relancer.
	//
	// Le drapeau g_instanceDead, pose par terminate(), tranche entre les deux.
	//
	// En pratique on se fie au CODE : rc=0 = projet charge, donc la nouvelle
	// instance a deja sa minuterie et son serveur (journal du 30/10 15h10 :
	// Â« decharge Â» puis Â« charge dans FL Â»). Le drapeau seul ne suffisait pas :
	// initialize() de la nouvelle instance le remet a 0, et on relancait des
	// threads sur l'ancienne instance morte.
	if (rc != 0 && InterlockedCompareExchange (&g_instanceDead, 0, 0) == 0)
	{
		logLine ("chargement : rc=%d -> relance des threads", rc);
		InterlockedExchange (&g_stopServer, 0);
		InterlockedExchange (&g_serverWanted, 1);
		g_timerId = SetTimer (nullptr, 0, 6, fmTimerProc);
		demarrerNosThreads ();
	}
	else
	{
		logLine ("chargement : rc=%d, instance detruite -> rien a relancer", rc);
	}
}

static void processPending ()
{
	std::vector<std::shared_ptr<Request>> work;
	{
		std::lock_guard<std::mutex> lock (g_mtx);
		work.swap (g_pending);
	}
	for (auto& r : work)
	{
		r->reply = executeCommand (r->cmd);
		r->done.store (true);
	}
}

static DWORD WINAPI serverThread (LPVOID)
{
	InterlockedExchange (&g_serverAlive, 1);

	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
	{
		logLine ("serveur          : WSAStartup echec");
		InterlockedExchange (&g_serverAlive, 0);
		return 0;
	}

	SOCKET srv = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv == INVALID_SOCKET)
	{
		logLine ("serveur          : socket echec");
		WSACleanup ();
		InterlockedExchange (&g_serverAlive, 0);
		return 0;
	}
	g_srv = srv;

	int reuse = 1;
	setsockopt (srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof (reuse));

	sockaddr_in addr;
	memset (&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons (47831);
	addr.sin_addr.s_addr = inet_addr ("127.0.0.1");

	if (bind (srv, (sockaddr*)&addr, sizeof (addr)) != 0)
	{
		logLine ("serveur          : bind echec sur 47831");
		closesocket (srv);
		WSACleanup ();
		g_srv = INVALID_SOCKET;
		InterlockedExchange (&g_serverAlive, 0);
		return 0;
	}

	listen (srv, SOMAXCONN);
	logLine ("serveur TCP      : 127.0.0.1:47831 pret");

	while (InterlockedCompareExchange (&g_stopServer, 0, 0) == 0)
	{
		// select avec delai : permet de verifier le drapeau d'arret au lieu
		// de rester bloque dans accept() pendant que FL decharge le DLL.
		fd_set rd;
		FD_ZERO (&rd);
		FD_SET (srv, &rd);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		if (select (0, &rd, nullptr, nullptr, &tv) <= 0)
			continue;

		SOCKET c = accept (srv, nullptr, nullptr);
		if (c == INVALID_SOCKET)
		{
			// Une erreur transitoire (client qui coupe net) ne doit pas tuer
			// le serveur : sinon plus personne ne peut se connecter jusqu'au
			// redemarrage de FL.
			if (InterlockedCompareExchange (&g_stopServer, 0, 0) != 0)
				break;
			logLine ("serveur TCP      : accept en echec, on continue");
			Sleep (100);
			continue;
		}

		// recv avec delai : meme raison, pour ne pas bloquer a l'arret
		DWORD timeoutMs = 200;
		setsockopt (c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof (timeoutMs));

		// Un client qui meurt sans fermer laisse un socket "established" que
		// recv voit expirer sans fin. Sans ce compteur, le serveur restait
		// bloque et refusait toute nouvelle connexion.
		DWORD dernierOctet = GetTickCount ();
		std::string recu;

		for (;;)
		{
			if (InterlockedCompareExchange (&g_stopServer, 0, 0) != 0)
				break;

			// Un recv ne rend pas une ligne : une longue commande (ADDX avec
			// des centaines de notes) arrive en plusieurs morceaux, et deux
			// courtes peuvent arriver ensemble. On decoupe sur les fins de ligne.
			char buf[4096];
			int n = recv (c, buf, sizeof (buf), 0);
			if (n == SOCKET_ERROR)
			{
				if (WSAGetLastError () == WSAETIMEDOUT)
				{
					if (GetTickCount () - dernierOctet > 20000)
					{
						logLine ("serveur TCP      : client muet, deconnexion");
						break;
					}
					continue;
				}
				break;
			}
			if (n <= 0)
				break;
			dernierOctet = GetTickCount ();
			recu.append (buf, n);
			if (recu.size () > 8 * 1024 * 1024)
			{
				logLine ("serveur TCP      : ligne demesuree, deconnexion");
				break;
			}

			bool coupe = false;
			size_t fin;
			while ((fin = recu.find ('\n')) != std::string::npos)
			{
				std::string line = recu.substr (0, fin);
				recu.erase (0, fin + 1);
				while (!line.empty () && (line.back () == '\r' || line.back () == '\n'))
					line.pop_back ();
				if (line.empty ())
					continue;

				auto r = std::make_shared<Request> ();
				r->cmd = line;
				{
					std::lock_guard<std::mutex> lock (g_mtx);
					g_pending.push_back (r);
				}

				// On previent le thread principal tout de suite au lieu d'attendre
				// le prochain tick de minuterie : c'est la moitie du gain de latence.
				if (g_wakeWnd != nullptr)
					PostMessageA (g_wakeWnd, WM_APP + 1, 0, 0);

				// Certaines commandes sont lentes : le mixeur redessine son interface,
				// et ADDCHANNEL scanne la base de plugins sur disque (jusqu'a 6 s).
				// On patiente jusqu'a 12 s, en revenant des que la reponse est prete.
				for (int i = 0; i < 10000 && !r->done.load (); ++i)
					Sleep (1);

				std::string out = r->done.load () ? r->reply : std::string ("TIMEOUT");
				out += "\n";
				// send peut n'envoyer qu'une partie (reponses INVENTORY de
				// plusieurs centaines de Ko)
				size_t envoye = 0;
				while (envoye < out.size ())
				{
					const int k = send (c, out.c_str () + envoye, (int)(out.size () - envoye), 0);
					if (k <= 0)
					{
						coupe = true;
						break;
					}
					envoye += (size_t)k;
				}
				if (coupe)
					break;
			}
			if (coupe)
				break;
		}
		closesocket (c);

		// l'agent est parti : la fenetre le dit
		fmgui::agentParti ();
	}

	closesocket (srv);
	WSACleanup ();
	g_srv = INVALID_SOCKET;
	InterlockedExchange (&g_serverAlive, 0);
	return 0;
}

//------------------------------------------------------------------------
class BridgeProcessor : public SingleComponentEffect
{
public:
	static FUnknown* createInstance (void*)
	{
		return static_cast<IAudioProcessor*> (new BridgeProcessor ());
	}

	tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE
	{
		tresult result = SingleComponentEffect::initialize (context);
		if (result != kResultOk)
			return result;

		// FL peut recharger l'effet SANS decharger le DLL : les variables
		// globales survivent. Sans cette remise a zero, l'ancien drapeau
		// d'arret tue le nouveau serveur des sa creation.
		InterlockedExchange (&g_stopServer, 0);
		InterlockedExchange (&g_instanceDead, 0);

		// Sans cela, Windows arrondit Sleep() et SetTimer() a 15,6 ms : chaque
		// commande attendait un tic complet. 1 ms rend les 6 ms effectifs.
		timeBeginPeriod (1);
		if (g_srv != INVALID_SOCKET)
		{
			closesocket (g_srv);
			g_srv = INVALID_SOCKET;
		}
		if (g_timerId != 0)
		{
			KillTimer (nullptr, g_timerId);
			destroyWakeWindow ();
			g_timerId = 0;
		}

		addAudioInput (USTRING ("Input"), SpeakerArr::kStereo);
		addAudioOutput (USTRING ("Output"), SpeakerArr::kStereo);

		HMODULE engine = GetModuleHandleA ("FLEngine_x64.dll");
		HMODULE python = GetModuleHandleA ("python312.dll");
		// Affecte tout de suite : les accesseurs de notes s'appuient dessus.
		g_engine = engine;

		logLine ("=== FruityMulti charge dans FL ===");
		logLine ("process pid      = %lu", (unsigned long)GetCurrentProcessId ());
		logLine ("FLEngine base    = %p", (void*)engine);
		logLine ("python312 base   = %p", (void*)python);

		// Resolution par signature : la fonction d'insertion de note.
		// 45 octets dont 8 jokers (operandes des deux appels relatifs).
		static const char* kInsertNoteSig =
			"57 56 53 48 83 EC 20 48 89 CB 48 89 D6 48 89 D9 48 8D 53 1C 44 8B 06 "
			"E8 ?? ?? ?? ?? 48 89 D9 8B 53 1C 41 B8 01 00 00 00 E8 ?? ?? ?? ??";

		void* insertNote = findSignature (engine, kInsertNoteSig);
		void* expected = engine ? (void*)((const char*)engine + 0xC60570) : nullptr;

		logLine ("insert note fn   = %p", insertNote);
		logLine ("attendu (RVA)    = %p", expected);
		const bool signatureOk = (insertNote && insertNote == expected);
		logLine ("signature        : %s", signatureOk ? "OK" : "ECHEC");

		if (!signatureOk)
		{
			logLine ("*** VERSION DE FL NON RECONNUE ***");
			logLine ("ce plugin a ete compile pour FL 26.1.7.5654");
			logLine ("les deux PCs doivent avoir la MEME version de FL");
			logLine ("le plugin se desactive pour ne pas lire au hasard");
			InterlockedExchange (&g_disabled, 1);
		}

		// Lecture de la liste de notes du motif courant.
		// Le 28/10 a 20h15 : la table FLEngine+0x153FCE0 est un tableau de
		// MOTIFS (0xC0 octets chacun, numero a partir de 1), la liste de notes
		// est a +0x28, et chaque note porte son CANAL en +0x06.
		// note (24 o) : +0x00 temps, +0x06 canal, +0x08 duree, +0x0C hauteur,
		//               +0x15 velocite
		{
			const int pattern = currentPattern ();
			const unsigned char* list = noteList (pattern);
			if (list != nullptr)
			{
				const int count = *(const int*)(list + 0x14);
				const unsigned char* data = *(const unsigned char* const*)(list + 0x08);
				logLine ("motif courant    : %d", pattern);
				logLine ("liste de notes   : %p   notes = %d", (void*)list, count);
				if (g_container == nullptr)
					g_container = (void*)list;

				if (count > 0 && isReadable (data, (size_t)count * 24))
				{
					for (int k = 0; k < count && k < 8; ++k)
					{
						const unsigned char* n = data + (size_t)k * 24;
						logLine ("    note[%d] temps=%u canal=%d duree=%u hauteur=%u vel=%u",
						         k, *(const unsigned*)(n + 0x00), noteChannelOf (n),
						         *(const unsigned*)(n + 0x08),
						         *(const unsigned*)(n + 0x0C),
						         (unsigned)*(const unsigned char*)(n + 0x15));
					}
				}
			}
			else
			{
				logLine ("motif courant    : %d (aucune liste)", pattern);
			}
		}

		// Preparation de l'ecriture : thread principal + minuterie.
		// Un timer cree depuis ce thread delivre ses tops sur ce meme thread,
		// a condition que celui-ci pompe les messages â€” c'est le cas du thread
		// principal de FL.
		HWND flWindow = findFlMainWindow ();
		g_flMainThreadId =
			flWindow ? GetWindowThreadProcessId (flWindow, nullptr) : GetCurrentThreadId ();

		logLine ("fenetre FL       = %p  thread principal = %lu",
		         (void*)flWindow, (unsigned long)g_flMainThreadId);
		logLine ("thread courant   = %lu", (unsigned long)GetCurrentThreadId ());
		logLine ("conteneur de test= %p", g_container);

		g_timerId = SetTimer (nullptr, 0, 6, fmTimerProc);
		logLine ("minuterie        = %llu", (unsigned long long)g_timerId);

		g_engine = engine;
		g_insertFn = insertNote;
		InterlockedExchange (&g_serverWanted, 1);
		g_flWindow = flWindow;

		// reveil immediat du thread principal par message
		if (kUseWakeWindow)
			createWakeWindow ();

		if (kShowToolbar)
		{
			HANDLE bar = CreateThread (nullptr, 0, barThread, nullptr, 0, nullptr);
			if (bar != nullptr)
				g_barThread = bar;
		}

		HANDLE cursor = CreateThread (nullptr, 0, cursorThread, nullptr, 0, nullptr);
		if (cursor != nullptr)
			g_cursorThread = cursor;   // conserve pour attendre son arret

		HANDLE thread = CreateThread (nullptr, 0, serverThread, nullptr, 0, nullptr);
		if (thread != nullptr)
			// On GARDE le handle : sans lui, terminate() ne peut pas attendre ce
			// thread, et FL decharge le DLL pendant qu'il tourne encore. C'est
			// exactement le plantage observe le 30/10 a 13h35 (faute dans
			// FruityMultiBridge.vst3_unloaded).
			g_serverThread = thread;
		else
			logLine ("serveur          : CreateThread echec");

		return kResultOk;
	}

	// Fenetre du plugin : c'est elle qui heberge / rejoint une session.
	IPlugView* PLUGIN_API createView (FIDString /*name*/) SMTG_OVERRIDE
	{
		return fmCreateView ();
	}
	tresult PLUGIN_API process (ProcessData& data) SMTG_OVERRIDE
	{
		// Effet transparent : on RECOPIE l'entree vers la sortie.
		// Ne rien faire coupe le son quand FL fournit des tampons separes.
		if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0)
			return kResultOk;

		const int32 samples = data.numSamples;
		const int32 outChannels = data.outputs[0].numChannels;
		const int32 inChannels = data.inputs[0].numChannels;

		for (int32 ch = 0; ch < outChannels; ++ch)
		{
			float* out = data.outputs[0].channelBuffers32[ch];
			if (out == nullptr)
				continue;

			float* in = (ch < inChannels) ? data.inputs[0].channelBuffers32[ch] : nullptr;
			if (in != nullptr)
			{
				if (in != out)
					memcpy (out, in, (size_t)samples * sizeof (float));
			}
			else
			{
				memset (out, 0, (size_t)samples * sizeof (float));
			}
		}

		return kResultOk;
	}

	// Nettoyage obligatoire : sans lui, FL decharge le DLL alors que notre
	// minuterie et notre thread serveur tournent encore -> plantage.
	tresult PLUGIN_API terminate () SMTG_OVERRIDE
	{
		InterlockedExchange (&g_instanceDead, 1);
		if (g_timerId != 0)
		{
			KillTimer (nullptr, g_timerId);
			destroyWakeWindow ();
			g_timerId = 0;
		}

		InterlockedExchange (&g_serverWanted, 0);
		InterlockedExchange (&g_stopServer, 1);

		// Le thread du curseur doit s'arreter AVANT le dechargement du DLL,
		// sinon son code disparait sous ses pieds -> plantage de FL.
		if (g_cursorWnd != nullptr)
			PostMessageA (g_cursorWnd, WM_CLOSE, 0, 0);
		if (g_barThreadId != 0)
		PostThreadMessageA ((DWORD)g_barThreadId, WM_QUIT, 0, 0);
	if (g_barThread != nullptr)
	{
		WaitForSingleObject (g_barThread, 2000);
		CloseHandle (g_barThread);
		g_barThread = nullptr;
	}	

	if (g_cursorThreadId != 0)
			PostThreadMessageA ((DWORD)g_cursorThreadId, WM_QUIT, 0, 0);

		// Meme regle que pour le serveur : attendre la fin effective du thread
		// du curseur avant que FL ne libere le DLL.
		if (g_cursorThread != nullptr)
		{
			WaitForSingleObject (g_cursorThread, 2000);
			CloseHandle (g_cursorThread);
			g_cursorThread = nullptr;
		}

		if (g_srv != INVALID_SOCKET)
		{
			closesocket (g_srv);
			g_srv = INVALID_SOCKET;
		}

		// Attendre que le thread serveur soit REELLEMENT termine. Sans cette
		// attente, FL decharge le DLL et le thread continue dans du code libere.
		if (g_serverThread != nullptr)
		{
			WaitForSingleObject (g_serverThread, 2000);
			CloseHandle (g_serverThread);
			g_serverThread = nullptr;
		}

		{
			std::lock_guard<std::mutex> lock (g_mtx);
			g_pending.clear ();
		}

		// Etat remis a zero pour la prochaine instance dans le meme processus.
		g_clipCache = nullptr;
		InterlockedExchange (&g_disabled, 0);

		timeEndPeriod (1);

		logLine ("=== FruityMulti decharge ===");
		return SingleComponentEffect::terminate ();
	}
};

} // namespace FruityMulti

BEGIN_FACTORY_DEF ("FruityMulti", "https://example.invalid", "mailto:none@example.invalid",
                   numberOfClasses)

DEF_CLASS (FruityMultiBridgeUID, Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
           "FruityMulti Bridge", Steinberg::Vst::kDistributable, Steinberg::Vst::PlugType::kFx,
           "1.0.0", kVstVersionString, FruityMulti::BridgeProcessor::createInstance, nullptr)

END_FACTORY
