// FruityMulti - installateur / metteur a jour.
//
// Un seul fichier a donner aux gens : FruityMultiSetup.exe.
//   double-clic                  -> installe la version CONTENUE dans le fichier
//   FruityMultiSetup.exe --apply -> installe ce qui a ete telecharge (mise a jour)
//
// Le programme embarque le plugin et l'agent en ressources : la premiere
// installation fonctionne donc sans reseau. Ensuite, la fenetre du plugin
// telecharge la nouvelle version et rappelle ce meme programme avec --apply.
//
// Il demande l'elevation lui-meme, propose de fermer FL Studio (le plugin est
// verrouille tant que FL tourne), installe, puis relance FL.

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const wchar_t* kTitre = L"FruityMulti";
const int IDR_VST3 = 1001;
const int IDR_AGENT = 1002;

std::mutex g_mtx;
std::wstring g_statut = L"Ready.";
std::atomic<bool> g_travailFini (false);
std::atomic<bool> g_echec (false);
bool g_applySeulement = false;
std::wstring g_profil;

HWND g_fenetre = nullptr;
HWND g_bouton = nullptr;

// --------------------------------------------------------------------------
std::wstring dossierLocal ()
{
	if (!g_profil.empty ())
		return g_profil + L"\\FruityMulti";

	wchar_t base[MAX_PATH] = { 0 };
	GetEnvironmentVariableW (L"LOCALAPPDATA", base, MAX_PATH);
	return std::wstring (base) + L"\\FruityMulti";
}

// FL Studio scanne plusieurs dossiers VST3, et sur ce poste il charge celui
// de "Program Files (x86)". On ecrit donc dans tous ceux qui existent.
std::vector<std::wstring> dossiersVst3 ()
{
	std::vector<std::wstring> liste;
	const wchar_t* variables[] = { L"CommonProgramFiles", L"CommonProgramFiles(x86)" };
	for (const wchar_t* variable : variables)
	{
		wchar_t base[MAX_PATH] = { 0 };
		if (GetEnvironmentVariableW (variable, base, MAX_PATH) == 0 || base[0] == 0)
			continue;
		std::wstring d = std::wstring (base) + L"\\VST3";
		bool deja = false;
		for (const std::wstring& e : liste)
			if (_wcsicmp (e.c_str (), d.c_str ()) == 0)
				deja = true;
		if (!deja)
			liste.push_back (d);
	}

	// copie par-utilisateur : seulement si elle existe deja, car FL la scanne
	wchar_t local[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableW (L"LOCALAPPDATA", local, MAX_PATH) > 0)
	{
		std::wstring d = std::wstring (local) + L"\\Programs\\Common\\VST3";
		if (GetFileAttributesW (d.c_str ()) != INVALID_FILE_ATTRIBUTES)
			liste.push_back (d);
	}
	return liste;
}

void statut (const std::wstring& texte)
{
	{
		std::lock_guard<std::mutex> g (g_mtx);
		g_statut = texte;
	}
	if (g_fenetre != nullptr)
		PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
}

bool estAdmin ()
{
	HANDLE jeton = nullptr;
	if (!OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &jeton))
		return false;
	TOKEN_ELEVATION elevation;
	DWORD taille = sizeof (elevation);
	const bool admin = GetTokenInformation (jeton, TokenElevation, &elevation,
	                                        sizeof (elevation), &taille)
	                   && elevation.TokenIsElevated != 0;
	CloseHandle (jeton);
	return admin;
}

bool relancerEnAdmin ()
{
	wchar_t chemin[MAX_PATH] = { 0 };
	GetModuleFileNameW (nullptr, chemin, MAX_PATH);

	std::wstring arguments = L"--eleve";
	if (g_applySeulement)
		arguments += L" --apply";
	if (!g_profil.empty ())
		arguments += L" --profil \"" + g_profil + L"\"";

	SHELLEXECUTEINFOW info;
	memset (&info, 0, sizeof (info));
	info.cbSize = sizeof (info);
	info.lpVerb = L"runas";
	info.lpFile = chemin;
	info.lpParameters = arguments.c_str ();
	info.nShow = SW_SHOWNORMAL;
	return ShellExecuteExW (&info) != FALSE;
}

// --- FL Studio -------------------------------------------------------------
HWND fenetreFL ()
{
	return FindWindowW (L"TFruityLoopsMainForm", nullptr);
}

std::wstring cheminFL ()
{
	HWND h = fenetreFL ();
	if (h == nullptr)
		return std::wstring ();

	DWORD pid = 0;
	GetWindowThreadProcessId (h, &pid);
	HANDLE p = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (p == nullptr)
		return std::wstring ();

	wchar_t chemin[MAX_PATH] = { 0 };
	DWORD taille = MAX_PATH;
	std::wstring resultat;
	if (QueryFullProcessImageNameW (p, 0, chemin, &taille))
		resultat = chemin;

	CloseHandle (p);
	return resultat;
}

bool fermerFL (DWORD attenteMs)
{
	HWND h = fenetreFL ();
	if (h == nullptr)
		return true;

	PostMessageW (h, WM_CLOSE, 0, 0);

	const DWORD debut = GetTickCount ();
	while (GetTickCount () - debut < attenteMs)
	{
		if (fenetreFL () == nullptr)
			return true;
		Sleep (300);
	}
	return fenetreFL () == nullptr;
}

// --- ressources embarquees -------------------------------------------------
bool extraire (int identifiant, const std::wstring& destination)
{
	HRSRC ressource = FindResourceW (nullptr, MAKEINTRESOURCEW (identifiant), (LPCWSTR)RT_RCDATA);
	if (ressource == nullptr)
		return false;

	HGLOBAL donnees = LoadResource (nullptr, ressource);
	if (donnees == nullptr)
		return false;

	const void* octets = LockResource (donnees);
	const DWORD taille = SizeofResource (nullptr, ressource);
	if (octets == nullptr || taille == 0)
		return false;

	HANDLE fichier = CreateFileW (destination.c_str (), GENERIC_WRITE, 0, nullptr,
	                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (fichier == INVALID_HANDLE_VALUE)
		return false;

	DWORD ecrits = 0;
	const bool ok = WriteFile (fichier, octets, taille, &ecrits, nullptr) != 0
	                && ecrits == taille;
	CloseHandle (fichier);
	return ok;
}

// Un agent deja lance verrouille fm_agent.exe : on l'arrete avant de copier.
// C'est le notre : rien a perdre, la fenetre le relancera.
void arreterAgent ()
{
	HANDLE instantane = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
	if (instantane == INVALID_HANDLE_VALUE)
		return;

	PROCESSENTRY32W entree;
	entree.dwSize = sizeof (entree);
	if (Process32FirstW (instantane, &entree))
	{
		do
		{
			if (_wcsicmp (entree.szExeFile, L"fm_agent.exe") != 0)
				continue;
			HANDLE p = OpenProcess (PROCESS_TERMINATE, FALSE, entree.th32ProcessID);
			if (p != nullptr)
			{
				TerminateProcess (p, 0);
				CloseHandle (p);
			}
		} while (Process32NextW (instantane, &entree));
	}
	CloseHandle (instantane);
	Sleep (400);
}

bool copier (const std::wstring& source, const std::wstring& destination)
{
	const size_t barre = destination.find_last_of (L'\\');
	if (barre != std::wstring::npos)
		CreateDirectoryW (destination.substr (0, barre).c_str (), nullptr);

	// FL peut garder le fichier verrouille un instant apres sa fermeture
	for (int essai = 0; essai < 12; essai++)
	{
		if (CopyFileW (source.c_str (), destination.c_str (), FALSE) != 0)
			return true;
		Sleep (700);
	}
	return false;
}

// --------------------------------------------------------------------------
void travail ()
{
	const std::wstring local = dossierLocal ();
	const std::wstring maj = local + L"\\maj";
	CreateDirectoryW (local.c_str (), nullptr);
	CreateDirectoryW (maj.c_str (), nullptr);

	const std::wstring vst3Source = maj + L"\\FruityMultiBridge.vst3";
	const std::wstring agentSource = maj + L"\\fm_agent.exe";

	if (!g_applySeulement)
	{
		statut (L"Preparing files…");
		const bool a = extraire (IDR_VST3, vst3Source);
		const bool b = extraire (IDR_AGENT, agentSource);
		if (!a || !b)
		{
			statut (L"This installer is incomplete: download it again.");
			g_echec = true;
			g_travailFini = true;
			PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
			return;
		}
	}

	if (GetFileAttributesW (vst3Source.c_str ()) == INVALID_FILE_ATTRIBUTES
	    || GetFileAttributesW (agentSource.c_str ()) == INVALID_FILE_ATTRIBUTES)
	{
		statut (L"Nothing to install.");
		g_echec = true;
		g_travailFini = true;
		PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
		return;
	}

	// FL ouvert : on propose de le fermer, puis on le redemarre
	bool relancer = false;
	std::wstring cheminDeFL;

	if (fenetreFL () != nullptr)
	{
		statut (L"FL Studio is open: waiting for you…");
		const int choix = MessageBoxW (
		    g_fenetre,
		    L"FL Studio must be closed to replace the plugin.\n\n"
		    L"Save your project, then click OK: FL closes, the install "
		    L"finishes, and FL restarts by itself.\n\n"
		    L"Cancel: close FL yourself and run this installer again.",
		    kTitre, MB_OKCANCEL | MB_ICONINFORMATION);

		if (choix != IDOK)
		{
			statut (L"Install cancelled. Run this installer again once FL "
			        L"is closed.");
			g_travailFini = true;
			PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
			return;
		}

		cheminDeFL = cheminFL ();
		statut (L"Closing FL Studio…");
		if (!fermerFL (40000))
		{
			statut (L"FL Studio did not close: close it, then run "
			        L"this installer again.");
			g_echec = true;
			g_travailFini = true;
			PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
			return;
		}
		relancer = !cheminDeFL.empty ();
		Sleep (900);
	}

	statut (L"Installing the plugin…");
	std::wstring emplacements;
	int copies = 0;
	for (const std::wstring& dossier : dossiersVst3 ())
	{
		const std::wstring cible = dossier + L"\\FruityMultiBridge.vst3";
		if (copier (vst3Source, cible))
		{
			emplacements += L"\n  " + cible;
			copies++;
		}
	}
	if (copies == 0)
	{
		statut (L"Could not write to the VST3 folders.\n"
		        L"Run the installer as administrator.");
		g_echec = true;
		g_travailFini = true;
		PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
		return;
	}

	statut (L"Installing the agent…");
	arreterAgent ();
	const std::wstring agentCible = local + L"\\fm_agent.exe";
	if (!copier (agentSource, agentCible))
	{
		wchar_t pourquoi[600];
		swprintf_s (pourquoi,
		            L"Could not install the agent to:\n\n  %s\n\n"
		            L"Windows error: %lu\n\n"
		            L"Check that fm_agent.exe is not running, allow the file "
		            L"in your antivirus, then try again.",
		            agentCible.c_str (), GetLastError ());
		statut (pourquoi);
		g_echec = true;
		g_travailFini = true;
		PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
		return;
	}

	// l'installateur se copie lui-meme : il servira aux mises a jour
	wchar_t soi[MAX_PATH] = { 0 };
	GetModuleFileNameW (nullptr, soi, MAX_PATH);
	copier (soi, local + L"\\FruityMultiSetup.exe");

	if (relancer)
	{
		statut (L"Restarting FL Studio…");
		SHELLEXECUTEINFOW info;
		memset (&info, 0, sizeof (info));
		info.cbSize = sizeof (info);
		info.lpFile = cheminDeFL.c_str ();
		info.nShow = SW_SHOWNORMAL;
		ShellExecuteExW (&info);
		Sleep (1200);
		statut (L"Installed:" + emplacements + L"\n  " + agentCible +
		        L"\n\nFL Studio is restarting with the new version.");
	}
	else
	{
		statut (L"Installed:" + emplacements + L"\n  " + agentCible +
		        L"\n\nRestart FL Studio, then add the FruityMulti Bridge "
		        L"effect to a mixer track.");
	}

	g_travailFini = true;
	PostMessageW (g_fenetre, WM_APP + 1, 0, 0);
}

// --------------------------------------------------------------------------
void dessiner (HDC dc, int largeur, int hauteur)
{
	HBRUSH fond = CreateSolidBrush (RGB (0x17, 0x18, 0x1C));
	RECT tout = { 0, 0, largeur, hauteur };
	FillRect (dc, &tout, fond);
	DeleteObject (fond);

	HFONT titre = CreateFontW (-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
	                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                           DEFAULT_PITCH, L"Segoe UI");
	HFONT texte = CreateFontW (-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
	                           DEFAULT_PITCH, L"Segoe UI");

	SetBkMode (dc, TRANSPARENT);
	SetTextColor (dc, RGB (0xE9, 0xEA, 0xEE));
	SelectObject (dc, titre);
	RECT r = { 22, 16, largeur - 22, 50 };
	DrawTextW (dc, L"FruityMulti", -1, &r, DT_LEFT | DT_SINGLELINE);

	SetTextColor (dc, RGB (0x8A, 0x8D, 0x97));
	SelectObject (dc, texte);
	r = { 22, 52, largeur - 22, 72 };
	DrawTextW (dc, L"Install into FL Studio", -1, &r, DT_LEFT | DT_SINGLELINE);

	std::wstring message;
	{
		std::lock_guard<std::mutex> g (g_mtx);
		message = g_statut;
	}
	SetTextColor (dc, RGB (0xE9, 0xEA, 0xEE));
	r = { 22, 90, largeur - 22, hauteur - 20 };
	DrawTextW (dc, message.c_str (), -1, &r, DT_LEFT | DT_WORDBREAK);

	DeleteObject (titre);
	DeleteObject (texte);
}

LRESULT CALLBACK proc (HWND h, UINT msg, WPARAM w, LPARAM l)
{
	switch (msg)
	{
	case WM_APP + 1:
		InvalidateRect (h, nullptr, FALSE);
		if (g_travailFini)
		{
			SetWindowTextW (g_bouton, g_echec ? L"Close" : L"Finish");
			EnableWindow (g_bouton, TRUE);
		}
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint (h, &ps);
		if (dc != nullptr)
		{
			RECT rc;
			GetClientRect (h, &rc);
			HDC mem = CreateCompatibleDC (dc);
			HBITMAP bmp = CreateCompatibleBitmap (dc, rc.right, rc.bottom);
			HGDIOBJ ancien = SelectObject (mem, bmp);
			dessiner (mem, rc.right, rc.bottom);
			BitBlt (dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
			SelectObject (mem, ancien);
			DeleteObject (bmp);
			DeleteDC (mem);
		}
		EndPaint (h, &ps);
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_COMMAND:
		if ((HWND)l == g_bouton)
		{
			if (g_travailFini)
				DestroyWindow (h);
			else
				EnableWindow (g_bouton, FALSE);
			return 0;
		}
		break;

	case WM_DESTROY:
		PostQuitMessage (0);
		return 0;
	}
	return DefWindowProcW (h, msg, w, l);
}

} // namespace

int WINAPI wWinMain (HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	wchar_t profil[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableW (L"LOCALAPPDATA", profil, MAX_PATH) > 0)
		g_profil = profil;

	for (int i = 1; i < __argc; i++)
	{
		if (wcscmp (__wargv[i], L"--apply") == 0)
			g_applySeulement = true;
		if (wcscmp (__wargv[i], L"--profil") == 0 && i + 1 < __argc)
			g_profil = __wargv[i + 1];
	}

	bool dejaEleve = false;
	for (int i = 1; i < __argc; i++)
		if (wcscmp (__wargv[i], L"--eleve") == 0)
			dejaEleve = true;

	if (!estAdmin () && !dejaEleve)
	{
		if (relancerEnAdmin ())
			return 0;
		MessageBoxW (nullptr,
		             L"The installer needs administrator rights.\n"
		             L"Accept the Windows prompt, then try again.",
		             kTitre, MB_ICONWARNING);
		return 1;
	}

	WNDCLASSEXW wc;
	memset (&wc, 0, sizeof (wc));
	wc.cbSize = sizeof (wc);
	wc.lpfnWndProc = proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW (nullptr, (LPCWSTR)IDC_ARROW);
	wc.lpszClassName = L"FruityMultiSetup";
	RegisterClassExW (&wc);

	const int largeur = 470;
	const int hauteur = 260;

	g_fenetre = CreateWindowExW (0, L"FruityMultiSetup",
	                             L"FruityMulti Setup",
	                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
	                             CW_USEDEFAULT, CW_USEDEFAULT, largeur, hauteur,
	                             nullptr, nullptr, instance, nullptr);
	if (g_fenetre == nullptr)
		return 1;

	g_bouton = CreateWindowExW (0, L"BUTTON", L"Installing…",
	                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
	                            largeur - 200, hauteur - 62, 178, 34,
	                            g_fenetre, (HMENU)1, instance, nullptr);
	EnableWindow (g_bouton, FALSE);

	ShowWindow (g_fenetre, SW_SHOWNORMAL);
	UpdateWindow (g_fenetre);

	std::thread (travail).detach ();

	MSG msg;
	while (GetMessageW (&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}
	return 0;
}
