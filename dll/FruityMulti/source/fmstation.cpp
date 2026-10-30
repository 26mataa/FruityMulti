// FruityMulti - l'interface « station pirate ».
//
// Direction artistique : une radio pirate imprimee. Papier, encre, bleu
// signal ; tout penche, le titre deborde, le menu descend en escalier.
// Heberger = prendre l'antenne, le code = la frequence, l'invite = l'auditeur.
//
// Technique : Dear ImGui ne sert ici que de moteur de dessin (aucun widget).
// Chaque element est dessine droit, puis ses sommets sont transformes
// (rotation, inclinaison) : c'est ce qui permet de tout pencher sans perdre
// la nettete du texte.

#include "fmstation.h"

#include "imgui.h"

#define NOMINMAX   // std::min et std::max, pas les macros de windows.h
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fmstation {
namespace {

// --------------------------------------------------------------------------
// Couleurs
// --------------------------------------------------------------------------
const ImU32 PAPIER  = IM_COL32 (0xEC, 0xE7, 0xDA, 255);
const ImU32 ENCRE   = IM_COL32 (0x12, 0x12, 0x12, 255);
const ImU32 BLEU    = IM_COL32 (0x23, 0x40, 0xFF, 255);
const ImU32 ORANGE  = IM_COL32 (0xFF, 0x5A, 0x1F, 255);
const ImU32 BLEU_CL = IM_COL32 (0x8F, 0xA0, 0xFF, 255);

ImU32 alpha (ImU32 c, float a)
{
	const int base = (int)((c >> IM_COL32_A_SHIFT) & 0xFF);
	const int na = (int)(base * std::clamp (a, 0.0f, 1.0f) + 0.5f);
	return (c & ~IM_COL32_A_MASK) | ((ImU32)na << IM_COL32_A_SHIFT);
}

const char* const ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

bool dansAlphabet (char c)
{
	return c != 0 && strchr (ALPHABET, c) != nullptr;
}

// --------------------------------------------------------------------------
// Transformations affines appliquees aux sommets
// --------------------------------------------------------------------------
struct Mat
{
	// x' = a x + c y + tx ; y' = b x + d y + ty
	float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

	ImVec2 operator() (ImVec2 p) const
	{
		return ImVec2 (a * p.x + c * p.y + tx, b * p.x + d * p.y + ty);
	}
};

// m apres n
Mat operator* (const Mat& m, const Mat& n)
{
	Mat r;
	r.a = m.a * n.a + m.c * n.b;
	r.b = m.b * n.a + m.d * n.b;
	r.c = m.a * n.c + m.c * n.d;
	r.d = m.b * n.c + m.d * n.d;
	r.tx = m.a * n.tx + m.c * n.ty + m.tx;
	r.ty = m.b * n.tx + m.d * n.ty + m.ty;
	return r;
}

Mat inverse (const Mat& m)
{
	const float det = m.a * m.d - m.b * m.c;
	Mat r;
	if (std::fabs (det) < 1e-8f)
		return r;
	r.a = m.d / det;
	r.b = -m.b / det;
	r.c = -m.c / det;
	r.d = m.a / det;
	r.tx = -(r.a * m.tx + r.c * m.ty);
	r.ty = -(r.b * m.tx + r.d * m.ty);
	return r;
}

Mat translation (float x, float y)
{
	Mat r;
	r.tx = x;
	r.ty = y;
	return r;
}

Mat rotation (float degres, ImVec2 pivot)
{
	const float a = degres * 3.14159265f / 180.0f;
	Mat r;
	r.a = std::cos (a);
	r.b = std::sin (a);
	r.c = -std::sin (a);
	r.d = std::cos (a);
	return translation (pivot.x, pivot.y) * r * translation (-pivot.x, -pivot.y);
}

// inclinaison horizontale, comme skewX() en CSS
Mat inclinaison (float degres, ImVec2 pivot)
{
	Mat r;
	r.c = std::tan (degres * 3.14159265f / 180.0f);
	return translation (pivot.x, pivot.y) * r * translation (-pivot.x, -pivot.y);
}

Mat echelleAutour (float s, ImVec2 pivot)
{
	Mat r;
	r.a = s;
	r.d = s;
	return translation (pivot.x, pivot.y) * r * translation (-pivot.x, -pivot.y);
}

void transformer (ImDrawList* dl, int debut, const Mat& m)
{
	ImDrawVert* v = dl->VtxBuffer.Data;
	for (int i = debut; i < dl->VtxBuffer.Size; ++i)
		v[i].pos = m (v[i].pos);
}

// --------------------------------------------------------------------------
// Animation
// --------------------------------------------------------------------------

// Ressort amorti : c'est lui qui donne le rebond (le depassement puis le
// retour) qui fait « vivant ».
struct Ressort
{
	float x = 0, v = 0;

	void vers (float cible, float dt, float raideur = 420.0f, float amorti = 22.0f)
	{
		// sous-pas : reste stable meme si une image prend du retard
		const int pas = 4;
		const float h = dt / pas;
		for (int i = 0; i < pas; ++i)
		{
			const float acc = raideur * (cible - x) - amorti * v;
			v += acc * h;
			x += v * h;
		}
	}
	void poser (float val)
	{
		x = val;
		v = 0;
	}
};

float lisser (float k)
{
	k = std::clamp (k, 0.0f, 1.0f);
	return k * k * (3.0f - 2.0f * k);
}

float sortieRapide (float k)   // ease-out cubique
{
	k = std::clamp (k, 0.0f, 1.0f);
	const float u = 1.0f - k;
	return 1.0f - u * u * u;
}

// depassement puis retour, de 0 a 1
float rebond (float k)
{
	k = std::clamp (k, 0.0f, 1.0f);
	const float c1 = 2.2f, c3 = c1 + 1.0f;
	const float u = k - 1.0f;
	return 1.0f + c3 * u * u * u + c1 * u * u;
}

// --------------------------------------------------------------------------
// Texte UTF-8
// --------------------------------------------------------------------------

// Majuscules, accents compris (latin-1 et œ)
std::string majuscules (const std::string& s)
{
	std::string r = s;
	for (size_t i = 0; i < r.size (); ++i)
	{
		unsigned char c = (unsigned char)r[i];
		if (c >= 'a' && c <= 'z')
			r[i] = (char)(c - 32);
		else if (c == 0xC3 && i + 1 < r.size ())
		{
			unsigned char n = (unsigned char)r[i + 1];
			if (n >= 0xA0 && n <= 0xBE && n != 0xB7)
				r[i + 1] = (char)(n - 0x20);
			++i;
		}
		else if (c == 0xC5 && i + 1 < r.size () && (unsigned char)r[i + 1] == 0x93)
		{
			r[i + 1] = (char)0x92;
			++i;
		}
	}
	return r;
}

int nbCaracteres (const std::string& s)
{
	int n = 0;
	for (unsigned char c : s)
		if ((c & 0xC0) != 0x80)
			++n;
	return n;
}

std::string debutDe (const std::string& s, int n)
{
	if (n <= 0)
		return std::string ();
	int vus = 0;
	for (size_t i = 0; i < s.size (); ++i)
	{
		if (((unsigned char)s[i] & 0xC0) != 0x80)
		{
			if (vus == n)
				return s.substr (0, i);
			++vus;
		}
	}
	return s;
}

ImVec2 mesure (ImFont* f, float taille, const std::string& s, float largeurLigne = 0.0f)
{
	return f->CalcTextSizeA (taille, FLT_MAX, largeurLigne, s.c_str ());
}

// Coupe avec « … » pour tenir dans `largeur`
std::string tronquer (ImFont* f, float taille, const std::string& s, float largeur)
{
	if (mesure (f, taille, s).x <= largeur)
		return s;
	const int n = nbCaracteres (s);
	for (int k = n - 1; k > 0; --k)
	{
		std::string essai = debutDe (s, k) + "\xE2\x80\xA6";
		if (mesure (f, taille, essai).x <= largeur)
			return essai;
	}
	return std::string ();
}

// --------------------------------------------------------------------------
// Les entrees du menu
// --------------------------------------------------------------------------
enum class Entree { Heberger, Rejoindre, Compat, Journal, Maj, CopierCode, Couper };

struct Ligne
{
	Entree id;
	const char* libelle;
};

std::vector<Ligne> lignesDuMenu (bool enOndes, int role)
{
	if (!enOndes)
		return { { Entree::Heberger, "HÉBERGER" },
		         { Entree::Rejoindre, "REJOINDRE" },
		         { Entree::Compat, "COMPATIBILITÉ" },
		         { Entree::Journal, "JOURNAL" },
		         { Entree::Maj, "MISE À JOUR" } };
	return { { Entree::CopierCode, "COPIER LE CODE" },
	         { Entree::Compat, "COMPATIBILITÉ" },
	         { Entree::Journal, "JOURNAL" },
	         { Entree::Couper, role == 1 ? "COUPER L'ANTENNE" : "QUITTER LA SESSION" } };
}

std::string numero (int v)
{
	char b[16];
	snprintf (b, sizeof (b), "N°%03d", v);
	return b;
}

// le code seul, sans l'adresse directe
std::string codeSeul (const std::string& adresse)
{
	const size_t barre = adresse.rfind ('/');
	return barre == std::string::npos ? adresse : adresse.substr (barre + 1);
}

std::string adresseDirecte (const std::string& adresse)
{
	const size_t barre = adresse.rfind ('/');
	return barre == std::string::npos ? std::string () : adresse.substr (0, barre);
}

} // namespace

// ==========================================================================
struct Station::Impl
{
	// --- cadre de l'image en cours ------------------------------------------
	ImDrawList* dl = nullptr;
	Polices f;
	float S = 1.0f;
	float t = 0.0f;
	float dt = 0.0f;

	ImVec2 V (float x, float y) const { return ImVec2 (x * S, y * S); }
	float F (float taille) const { return taille * S; }
	int marque () const { return dl->VtxBuffer.Size; }

	void texte (ImFont* police, float taille, float x, float y, ImU32 c,
	            const std::string& s, float largeurLigne = 0.0f)
	{
		dl->AddText (police, F (taille), V (x, y), c, s.c_str (), nullptr,
		             largeurLigne > 0 ? F (largeurLigne) : 0.0f);
	}
	float largeur (ImFont* police, float taille, const std::string& s) const
	{
		return mesure (police, F (taille), s).x / S;
	}
	void carre (float x, float y, float cote, ImU32 c, bool plein)
	{
		if (plein)
			dl->AddRectFilled (V (x, y), V (x + cote, y + cote), c);
		else
			dl->AddRect (V (x, y), V (x + cote, y + cote), c, 0.0f, 0, F (1.5f));
	}

	// --- entrees -------------------------------------------------------------
	ImVec2 souris = ImVec2 (-1, -1);
	ImVec2 sourisPrec = ImVec2 (-1, -1);
	bool clicAttendu = false;
	ImVec2 clic = ImVec2 (-1, -1);   // ou l'appui a eu lieu
	int survol = -1;

	// --- menu ------------------------------------------------------------------
	bool enOndes = false;          // menu affiche (change au milieu du volet)
	int roleAffiche = 0;
	int sel = 0;
	int contenu = 0;               // ligne dont l'etiquette montre le contenu
	Ressort barre[8], decalage[8], ecart[8], grossi[8];
	float tFrappe = -10.0f;        // derniere validation (tampon)
	int ligneFrappee = -1;

	// --- etiquette -------------------------------------------------------------
	float tChoix = -10.0f;
	Ressort etiqX, etiqY, etiqRot;
	float tErreur = -10.0f;

	// --- grand numero ----------------------------------------------------------
	Ressort numRot, numY;

	// --- titre -------------------------------------------------------------------
	std::string titre;
	float tTitre = 0.0f;

	// --- secousse, volet, oscilloscope ------------------------------------------
	float tSecousse = -10.0f, dureeSecousse = 0.16f;
	float tVolet = -10.0f;
	bool voletVers = false;
	float amplitude = 0.1f, impulsion = 0.0f;

	// --- code et saisie -------------------------------------------------------
	std::string codeAffiche;
	float tVolets = -10.0f;        // volets du code qui tournent
	float tCopie = -10.0f;
	std::string saisie;            // code tape par l'invite
	std::string saisieDirecte;     // ip:port colle depuis une adresse directe
	float tFrappeCase[6] = { -10, -10, -10, -10, -10, -10 };

	// --- etat precedent, pour reperer les changements ------------------------
	std::string pairPrec;
	Maj majPrec = Maj::Repos;

	// --- message bref, action ------------------------------------------------
	std::string message;
	float tMessage = -10.0f;
	bool actionPrete = false;
	Action action = Action::Heberger;
	std::string argument;

	std::vector<Ligne> lignes;
	Etat e;
	bool premiere = true;

	// ------------------------------------------------------------------------
	void demander (Action a, const std::string& arg = std::string ())
	{
		action = a;
		argument = arg;
		actionPrete = true;
	}

	void secouer (float duree)
	{
		tSecousse = t;
		dureeSecousse = duree;
	}

	void tamponner ()
	{
		tFrappe = t;
		ligneFrappee = sel;
		secouer (0.18f);
		impulsion = 1.0f;
	}

	void choisir (int i)
	{
		if (lignes.empty ())
			return;
		i = (i % (int)lignes.size () + (int)lignes.size ()) % (int)lignes.size ();
		if (i == sel)
			return;
		sel = i;
		tChoix = t;
	}

	Entree courante () const
	{
		return lignes.empty () ? Entree::Heberger : lignes[std::min (sel, (int)lignes.size () - 1)].id;
	}

	void valider ()
	{
		switch (courante ())
		{
		case Entree::Heberger:
			tamponner ();
			demander (Action::Heberger);
			break;
		case Entree::Rejoindre:
			tamponner ();
			if (saisie.size () != 6)
			{
				tErreur = t;
				break;
			}
			demander (Action::Rejoindre,
			          saisieDirecte.empty () ? saisie : saisieDirecte + "/" + saisie);
			break;
		case Entree::Maj:
			if (e.maj == Maj::Verif || e.maj == Maj::Telechargement)
				break;
			tamponner ();
			if (e.maj == Maj::Dispo)
				demander (Action::InstallerMaj);
			else if (e.maj != Maj::Redemarrer)
				demander (Action::VerifierMaj);
			break;
		case Entree::CopierCode:
			tamponner ();
			tCopie = t;
			demander (Action::Copier);
			break;
		case Entree::Couper:
			tamponner ();
			demander (Action::Couper);
			break;
		default:
			tamponner ();
			break;
		}
	}

	void saisir (const std::string& brut)
	{
		std::string code = brut;
		saisieDirecte.clear ();
		const size_t barre = brut.rfind ('/');
		if (barre != std::string::npos && brut.find (':') != std::string::npos)
		{
			// adresse directe collee : ip:port/CODE
			for (char c : brut.substr (0, barre))
				if (isalnum ((unsigned char)c) || c == '.' || c == ':' || c == '-')
					saisieDirecte += c;
			code = brut.substr (barre + 1);
		}
		std::string propre;
		for (char c : majuscules (code))
			if (dansAlphabet (c) && propre.size () < 6)
				propre += c;
		for (size_t i = 0; i < propre.size (); ++i)
			if (i >= saisie.size () || saisie[i] != propre[i])
				tFrappeCase[i] = t + 0.03f * i;
		saisie = propre;
		tErreur = -10.0f;
	}

	// ========================================================================
	// Dessin
	// ========================================================================
	std::string titreVoulu () const
	{
		if (!enOndes)
			return e.maj == Maj::Telechargement ? "MISE À JOUR" : "HORS ANTENNE";
		if (e.role == 1)
			return e.code.empty () ? "RÉGLAGE…" : "EN ONDES";
		return e.pair.empty () ? "RÉCEPTION…" : "EN ONDES";
	}

	bool alAntenne () const
	{
		return enOndes && (e.role == 1 ? !e.code.empty () : !e.pair.empty ());
	}

	std::string bandeau () const
	{
		const std::string code = codeSeul (e.code);
		if (!enOndes)
		{
			if (e.maj == Maj::Dispo)
				return "MISE À JOUR DISPONIBLE — " + numero (e.majVersion) + " — ENTRÉE POUR INSTALLER —";
			if (e.maj == Maj::Telechargement)
				return "INSTALLATION DE LA " + numero (e.majVersion) + " — FL VA REDÉMARRER —";
			return "HORS ANTENNE — AUCUNE SESSION — FRUITYMULTI " + numero (e.version) + " —";
		}
		const std::string pair = majuscules (e.pair);
		if (e.role == 1)
		{
			if (code.empty ())
				return "RÉGLAGE DE LA STATION —";
			if (pair.empty ())
				return "EN ONDES — FRÉQUENCE " + code + " — EN ATTENTE D'UN AUDITEUR —";
			return "EN ONDES — " + pair + " À L'ÉCOUTE — SYNCHRO OK —";
		}
		if (pair.empty ())
			return "RÉCEPTION — RECHERCHE DE LA FRÉQUENCE " + code + " —";
		return "EN ONDES — CALÉ SUR " + pair + " — SYNCHRO OK —";
	}

	std::string ligneEtat () const
	{
		if (t - tMessage < 2.2f)
			return majuscules (message);
		const std::string s = e.statut;
		if (s.empty () || s == "Aucune session")
			return "STATION LOCALE · " + majuscules (e.poste);
		return majuscules (s);
	}

	// --- bande verticale et nom ------------------------------------------------
	void dessinerBande ()
	{
		int m = marque ();
		dl->AddRectFilled (V (-30, -40), V (66, 460), ENCRE);
		transformer (dl, m, rotation (7, V (18, 210)));

		const float tb = 54;
		const float w1 = largeur (f.titre, tb, "FRUITY");
		const float w2 = largeur (f.titre, tb, "MULTI");
		const float cx = 30, cy = 205;
		m = marque ();
		texte (f.titre, tb, cx - (w1 + w2) / 2, cy - tb * 0.5f, PAPIER, "FRUITY");
		texte (f.titre, tb, cx - (w1 + w2) / 2 + w1, cy - tb * 0.5f, BLEU_CL, "MULTI");
		transformer (dl, m, rotation (-83, V (cx, cy)));

		m = marque ();
		const std::string v = numero (e.version);
		texte (f.monoGras, 11, 12, 14, PAPIER, v);
		if (e.maj == Maj::Dispo || e.maj == Maj::Telechargement)
		{
			const float x = 12 + largeur (f.monoGras, 11, v) + 5;
			dl->AddTriangleFilled (V (x, 25), V (x + 8, 25), V (x + 4, 17), BLEU_CL);
		}
		transformer (dl, m, rotation (7, V (12, 14)));
	}

	// --- grand numero fantome -------------------------------------------------
	void dessinerNumero ()
	{
		numRot.vers (9.0f + sel * 3.0f, dt, 300, 20);
		numY.vers (sel * -6.0f, dt, 300, 20);

		char n[8];
		snprintf (n, sizeof (n), "0%d", sel + 1);
		const float taille = 300;
		const float w = largeur (f.titre, taille, n);
		const float x = 670 - w, y = 60 + numY.x;

		// la pulsation du tampon le fait respirer un instant
		const float k = t - tFrappe < 0.3f ? (1.0f - (t - tFrappe) / 0.3f) : 0.0f;
		const int m = marque ();
		texte (f.titre, taille, x, y, alpha (ENCRE, 0.07f + 0.05f * k), n);
		const ImVec2 centre = V (x + w / 2, y + taille / 2);
		transformer (dl, m, rotation (numRot.x, centre) * echelleAutour (1.0f + 0.06f * k, centre));
	}

	// --- oscilloscope -----------------------------------------------------------
	void dessinerOscillo ()
	{
		const bool on = enOndes;
		amplitude += ((on ? 1.0f : 0.1f) - amplitude) * std::min (1.0f, dt * 3.0f);
		impulsion *= std::pow (0.92f, dt * 60.0f);

		auto ligne = [&] (ImU32 c, float phase, float a) {
			ImVec2 pts[200];
			int n = 0;
			for (float x = 0; x <= 760 && n < 200; x += 4)
			{
				const float enveloppe = std::sin (x / 760.0f * 3.14159265f);
				float y = 40 + enveloppe * (a * amplitude * (std::sin (x * 0.04f + t * 3 + phase) * 16
				                                              + std::sin (x * 0.1f - t * 5 + phase) * 5)
				                            + impulsion * 20 * std::sin (x * 0.28f - t * 20)
				                                  * std::exp (-std::fabs (x - 380) / 150));
				if (!on)
					y += std::sin (x * 0.9f + t * 30) * 0.7f;
				pts[n++] = V (-40 + x, 296 + y);
			}
			dl->AddPolyline (pts, n, c, F (3.0f), 0);
		};

		const int m = marque ();
		ligne (BLEU, 0.0f, 1.0f);
		if (on && !e.pair.empty ())
			ligne (ORANGE, 0.35f, 0.92f);
		transformer (dl, m, rotation (-6, V (340, 336)));
	}

	// --- titre ------------------------------------------------------------------
	void dessinerTitre ()
	{
		const std::string voulu = titreVoulu ();
		if (voulu != titre)
		{
			titre = voulu;
			tTitre = t;
		}
		const int n1 = (int)((t - tTitre) / 0.03f) + 1;
		const int n2 = (int)((t - tTitre - 0.04f) / 0.03f) + 1;

		float taille = 82;
		const float w = largeur (f.titre, taille, titre);
		if (w > 470)
			taille *= 470 / w;

		const int m = marque ();
		texte (f.titre, taille, 82, 48, alpha (BLEU, 0.7f), debutDe (titre, n2));
		texte (f.titre, taille, 78, 44, ENCRE, debutDe (titre, n1));
		transformer (dl, m, rotation (-5, V (78, 44)));

		if (alAntenne () && std::fmod (t, 1.0f) < 0.5f)
		{
			const int m2 = marque ();
			const float wt = largeur (f.monoGras, 11, "ON AIR");
			dl->AddRectFilled (V (520, 16), V (520 + wt + 30, 36), BLEU);
			dl->AddCircleFilled (V (532, 26), F (3.5f), PAPIER);
			texte (f.monoGras, 11, 542, 19, PAPIER, "ON AIR");
			transformer (dl, m2, rotation (10, V (520, 16)));
		}

		const int m3 = marque ();
		texte (f.monoGras, 11, 84, 130, alpha (ENCRE, 0.72f),
		       tronquer (f.monoGras, F (11), ligneEtat (), F (280)));
		transformer (dl, m3, rotation (-5, V (84, 130)));
	}

	// --- menu ---------------------------------------------------------------------
	Mat matriceMenu () const { return rotation (-5, V (74, 156)); }

	int ligneSous (ImVec2 p) const
	{
		if (p.x < 0)
			return -1;
		const ImVec2 local = inverse (matriceMenu ()) (p);
		const float x = local.x / S, y = local.y / S;
		for (int i = 0; i < (int)lignes.size (); ++i)
		{
			const float x0 = 74 + i * 14.0f;
			const float y0 = 156 + i * 35.0f;
			if (x >= x0 && x <= x0 + 290 && y >= y0 && y < y0 + 35)
				return i;
		}
		return -1;
	}

	void dessinerMenu ()
	{
		const int m = marque ();
		for (int i = 0; i < (int)lignes.size (); ++i)
		{
			const bool choisie = i == sel;
			barre[i].vers (choisie ? 1.0f : 0.0f, dt, 520, 34);
			decalage[i].vers (i * 14.0f + (choisie ? 16.0f : 0.0f), dt);
			ecart[i].vers (choisie ? 10.0f : 0.0f, dt);
			grossi[i].vers (choisie ? 1.1f : 1.0f, dt);

			const float x = 74 + decalage[i].x;
			const float y = 156 + i * 35.0f;
			const bool frappee = i == ligneFrappee && t - tFrappe < 0.17f;

			const float k = std::clamp (barre[i].x, 0.0f, 1.08f);
			if (k > 0.01f)
			{
				const int mb = marque ();
				dl->AddRectFilled (V (x, y + 3), V (x + 250 * k, y + 32), frappee ? BLEU : ENCRE);
				transformer (dl, mb, inclinaison (-18, V (x, y + 17.5f)));
			}

			char n[8];
			snprintf (n, sizeof (n), "0%d", i + 1);
			const float xn = x + 14;
			texte (f.mono, 11, xn, y + 11, choisie ? PAPIER : ENCRE, n);

			const float tl = 28;
			const float xl = xn + largeur (f.mono, 11, n) + 12 + ecart[i].x;
			const float yl = y + 17.5f - tl * 0.5f;
			const int ml = marque ();
			if (frappee)
				texte (f.titre, tl, xl + 4, yl + 3, ORANGE, lignes[i].libelle);
			texte (f.titre, tl, xl, yl, choisie ? PAPIER : alpha (ENCRE, 0.55f), lignes[i].libelle);
			transformer (dl, ml, echelleAutour (grossi[i].x, V (xl, y + 17.5f)));

			if (lignes[i].id == Entree::Maj && (e.maj == Maj::Dispo || e.maj == Maj::Telechargement))
			{
				const float xb = xl + largeur (f.titre, tl, lignes[i].libelle) * grossi[i].x + 10;
				const std::string v = numero (e.majVersion);
				const float wb = largeur (f.mono, 11, v) + 10;
				const int mb = marque ();
				dl->AddRectFilled (V (xb, y + 9), V (xb + wb, y + 26), BLEU);
				texte (f.mono, 11, xb + 5, y + 11, PAPIER, v);
				transformer (dl, mb, rotation (6, V (xb, y + 9)));
			}
		}
		transformer (dl, m, matriceMenu ());
	}

	// --- etiquette ------------------------------------------------------------------
	// Elle s'efface, change de contenu puis revient : c'est ce qui montre
	// clairement que la ligne choisie a change.
	void dessinerEtiquette ()
	{
		const float depuis = t - tChoix;
		float opacite = 1.0f;
		if (depuis < 0.11f)
			opacite = 1.0f - depuis / 0.11f;
		else
		{
			contenu = sel;
			opacite = std::min (1.0f, (depuis - 0.11f) / 0.12f);
		}

		const bool sortie = depuis < 0.11f;
		const float pente = (contenu % 2) ? -2.0f : 4.0f;
		etiqX.vers (sortie ? 16.0f : 0.0f, dt, 360, 20);
		etiqY.vers (sortie ? 6.0f : 0.0f, dt, 360, 20);
		etiqRot.vers (sortie ? pente + 1.0f : pente, dt, 300, 16);

		float tremble = 0.0f;
		const float de = t - tErreur;
		if (de < 0.3f)
			tremble = std::sin (de * 60.0f) * 9.0f * (1.0f - de / 0.3f);

		const float ox = 376 + etiqX.x + tremble, oy = 134 + etiqY.x;
		const int m = marque ();

		const ImVec2 forme[4] = { V (ox, oy), V (ox + 244, oy + 6.9f),
		                          V (ox + 236.7f, oy + 172), V (ox + 7.3f, oy + 163.4f) };
		dl->AddConvexPolyFilled (forme, 4, alpha (ENCRE, opacite));

		const int mr = marque ();
		dl->AddRectFilled (V (ox + 76, oy - 10), V (ox + 162, oy + 10), alpha (BLEU, 0.7f * opacite));
		transformer (dl, mr, rotation (-8, V (ox + 119, oy)));

		contenuEtiquette (ox + 20, oy + 20, opacite);
		transformer (dl, m, rotation (etiqRot.x, V (ox + 122, oy + 86)));
	}

	void entete (float x, float y, float a, int index, const char* s)
	{
		char b[96];
		snprintf (b, sizeof (b), "0%d / %s", index + 1, s);
		texte (f.monoGras, 11, x, y, alpha (BLEU_CL, a), b);
	}

	float paragraphe (float x, float y, float a, const std::string& s)
	{
		texte (f.texte, 14, x, y, alpha (PAPIER, a), s, 204);
		return mesure (f.texte, F (14), s, F (204)).y / S;
	}

	void consigne (float x, float y, float a, const std::string& s, ImU32 c = ORANGE)
	{
		texte (f.monoGras, 11, x, y, alpha (c, a), tronquer (f.monoGras, F (11), s, F (204)));
	}

	void contenuEtiquette (float x, float y, float a)
	{
		if (contenu >= (int)lignes.size ())
			contenu = 0;
		const Entree id = lignes.empty () ? Entree::Heberger : lignes[contenu].id;
		const int i = contenu;

		switch (id)
		{
		case Entree::Heberger:
		{
			entete (x, y, a, i, "HÉBERGER");
			const float h = paragraphe (x, y + 18, a,
			    "Prends l'antenne. Tu reçois une fréquence de 6 signes à donner à ton invité.");
			consigne (x, y + 18 + h + 12, a, "ENTRÉE → PRENDRE L'ANTENNE");
			break;
		}
		case Entree::Rejoindre:
			contenuRejoindre (x, y, a, i);
			break;
		case Entree::Compat:
			contenuCompat (x, y, a, i);
			break;
		case Entree::Journal:
		{
			entete (x, y, a, i, "JOURNAL");
			if (e.journal.empty ())
				texte (f.mono, 11, x, y + 20, alpha (PAPIER, 0.6f * a), "RIEN POUR L'INSTANT");
			const size_t debut = e.journal.size () > 6 ? e.journal.size () - 6 : 0;
			for (size_t k = debut; k < e.journal.size (); ++k)
			{
				const std::string l = tronquer (f.mono, F (11), majuscules (e.journal[k]), F (204));
				const bool recente = k + 1 == e.journal.size ();
				texte (f.mono, 11, x, y + 20 + (k - debut) * 19.0f,
				       alpha (recente ? BLEU_CL : PAPIER, a), l);
			}
			break;
		}
		case Entree::Maj:
			contenuMaj (x, y, a, i);
			break;
		case Entree::CopierCode:
			contenuCode (x, y, a, i);
			break;
		case Entree::Couper:
		{
			entete (x, y, a, i, e.role == 1 ? "COUPER L'ANTENNE" : "QUITTER");
			const float h = paragraphe (x, y + 18, a, e.role == 1
			    ? "Ferme la station. Ton invité est déconnecté, chacun garde son projet."
			    : "Tu quittes la session. Ton projet reste tel quel.");
			consigne (x, y + 18 + h + 12, a, "ENTRÉE → CONFIRMER");
			break;
		}
		}
	}

	void contenuRejoindre (float x, float y, float a, int i)
	{
		entete (x, y, a, i, "FRÉQUENCE DE L'HÔTE");
		const bool erreur = t - tErreur < 1.6f;
		for (int k = 0; k < 6; ++k)
		{
			const float sx = x + k * 31.0f, sy = y + 26;
			const float base = (k % 2) ? 3.0f : -4.0f;
			const int m = marque ();
			dl->AddRectFilled (V (sx, sy + 37), V (sx + 27, sy + 40),
			                   alpha (erreur ? ORANGE : PAPIER, a));
			if (k < (int)saisie.size ())
			{
				const std::string c (1, saisie[k]);
				const float p = sortieRapide ((t - tFrappeCase[k]) / 0.28f);
				const float tc = 32;
				const float wc = largeur (f.titre, tc, c);
				const ImU32 couleur = p < 1.0f ? BLEU_CL : PAPIER;
				const int mc = marque ();
				texte (f.titre, tc, sx + 13.5f - wc / 2, sy + 4, alpha (couleur, a), c);
				const ImVec2 centre = V (sx + 13.5f, sy + 20);
				transformer (dl, mc, translation (0, F (-12 * (1 - p)))
				                         * rotation (-12 * (1 - p), centre)
				                         * echelleAutour (1.0f + 0.4f * (1 - p), centre));
			}
			else if (k == (int)saisie.size () && courante () == Entree::Rejoindre
			         && std::fmod (t, 1.0f) < 0.55f)
			{
				dl->AddRectFilled (V (sx + 6, sy + 28), V (sx + 21, sy + 32), alpha (BLEU_CL, a));
			}
			transformer (dl, m, rotation (base, V (sx + 13.5f, sy + 20)));
		}

		const float yc = y + 80;
		if (erreur)
			consigne (x, yc, a, "6 SIGNES ATTENDUS");
		else if (saisie.size () == 6)
			consigne (x, yc, a, "ENTRÉE → SE CALER");
		else
			consigne (x, yc, a, "TAPE LE CODE → ENTRÉE", PAPIER);

		if (!saisieDirecte.empty ())
			texte (f.mono, 11, x, yc + 18, alpha (BLEU_CL, a),
			       tronquer (f.mono, F (11), "DIRECT · " + saisieDirecte, F (204)));
		else
			texte (f.mono, 11, x, yc + 18, alpha (PAPIER, 0.5f * a), "CTRL+V POUR COLLER");
	}

	void contenuCompat (float x, float y, float a, int i)
	{
		entete (x, y, a, i, "TOI · L'AUTRE");
		if (e.compat.empty ())
		{
			paragraphe (x, y + 18, a,
			    "Connecte-toi à quelqu'un : vos plugins seront comparés ici.");
			return;
		}

		// ce qui manque d'abord : c'est ce qui casse une session
		std::vector<std::pair<char, std::string>> rangs = e.compat;
		auto poids = [] (char c) { return c == '-' ? 0 : (c == '+' ? 1 : 2); };
		std::stable_sort (rangs.begin (), rangs.end (),
		                  [&] (const auto& p, const auto& q) { return poids (p.first) < poids (q.first); });

		// 5 rangs tiennent dans l'etiquette ; au-dela, 4 et une ligne de reste
		const int visibles = (int)rangs.size () <= 5 ? (int)rangs.size () : 4;
		for (int r = 0; r < visibles; ++r)
		{
			const float ry = y + 18 + r * 24.0f;
			const float rx = x + r * 3.0f;
			const char sorte = rangs[r].first;
			const ImU32 c = sorte == '=' ? PAPIER : ORANGE;
			texte (f.titre, 19, rx, ry, alpha (PAPIER, a),
			       tronquer (f.titre, F (19), majuscules (rangs[r].second), F (150)));
			carre (x + 178, ry + 5, 9, alpha (c, a), sorte != '-');
			carre (x + 192, ry + 5, 9, alpha (c, a), sorte != '+');
			dl->AddRectFilled (V (rx, ry + 21), V (x + 204, ry + 22), alpha (PAPIER, 0.2f * a));
		}
		if ((int)rangs.size () > visibles)
		{
			const int reste = (int)rangs.size () - visibles;
			int manquants = 0;
			for (int r = visibles; r < (int)rangs.size (); ++r)
				manquants += rangs[r].first != '=';
			char b[64];
			if (manquants > 0)
				snprintf (b, sizeof (b), "+%d AUTRE%s · %d PAS DES DEUX CÔTÉS", reste,
				          reste > 1 ? "S" : "", manquants);
			else
				snprintf (b, sizeof (b), "+%d AUTRE%s EN COMMUN", reste, reste > 1 ? "S" : "");
			texte (f.mono, 11, x + visibles * 3.0f, y + 22 + visibles * 24.0f,
			       alpha (PAPIER, 0.6f * a), tronquer (f.mono, F (11), b, F (204)));
		}
	}

	void contenuMaj (float x, float y, float a, int i)
	{
		entete (x, y, a, i, "MISE À JOUR · GITHUB");
		switch (e.maj)
		{
		case Maj::Repos:
			texte (f.titre, 56, x, y + 18, alpha (PAPIER, a), numero (e.version));
			texte (f.mono, 11, x + largeur (f.titre, 56, numero (e.version)) + 10, y + 52,
			       alpha (PAPIER, a), "INSTALLÉE");
			consigne (x, y + 86, a, "ENTRÉE → VÉRIFIER");
			break;
		case Maj::Verif:
		{
			const int points = 1 + (int)(t * 4) % 3;
			texte (f.titre, 56, x, y + 18, alpha (PAPIER, 0.4f * a), "N°" + std::string (points, '.'));
			consigne (x, y + 86, a, "INTERROGATION DE GITHUB…", PAPIER);
			break;
		}
		case Maj::AJour:
			texte (f.titre, 56, x, y + 18, alpha (PAPIER, a), "À JOUR");
			consigne (x, y + 72, a, numero (e.version) + " EST LA DERNIÈRE", PAPIER);
			consigne (x, y + 90, a, "ENTRÉE → REVÉRIFIER");
			break;
		case Maj::Dispo:
		{
			const std::string vieux = numero (e.version);
			texte (f.titre, 22, x, y + 40, alpha (PAPIER, 0.5f * a), vieux);
			const float wv = largeur (f.titre, 22, vieux);
			dl->AddRectFilled (V (x, y + 50), V (x + wv, y + 52), alpha (PAPIER, 0.5f * a));
			const int m = marque ();
			texte (f.titre, 52, x + wv + 10, y + 16, alpha (BLEU_CL, a), numero (e.majVersion));
			transformer (dl, m, rotation (-5, V (x + wv + 10, y + 42)));
			texte (f.mono, 11, x, y + 74, alpha (PAPIER, a), "FL SE FERME PUIS REDÉMARRE.");
			texte (f.mono, 11, x, y + 89, alpha (PAPIER, a), "ENREGISTRE TON PROJET.");
			consigne (x, y + 108, a, "ENTRÉE → INSTALLER");
			break;
		}
		case Maj::Telechargement:
		{
			texte (f.titre, 30, x, y + 22, alpha (PAPIER, a), "TÉLÉCHARGEMENT");
			// barre indeterminee : un paquet de segments qui court
			const int tete = (int)(t * 14) % 26;
			for (int s = 0; s < 18; ++s)
			{
				const bool plein = s <= tete && s > tete - 7;
				const int m = marque ();
				dl->AddRectFilled (V (x + s * 11.0f, y + 64), V (x + s * 11.0f + 8, y + 80),
				                   alpha (PAPIER, (plein ? 1.0f : 0.2f) * a));
				transformer (dl, m, inclinaison (-18, V (x + s * 11.0f, y + 72)));
			}
			consigne (x, y + 92, a, "FL VA SE FERMER PUIS REDÉMARRER", PAPIER);
			break;
		}
		case Maj::Erreur:
			texte (f.titre, 44, x, y + 22, alpha (ORANGE, a), "HORS LIGNE");
			consigne (x, y + 72, a, "GITHUB NE RÉPOND PAS", PAPIER);
			consigne (x, y + 90, a, "ENTRÉE → RÉESSAYER");
			break;
		case Maj::Redemarrer:
			texte (f.titre, 40, x, y + 22, alpha (BLEU_CL, a), "REDÉMARRE FL");
			paragraphe (x, y + 64, a, "La nouvelle version est installée : ferme puis rouvre FL Studio.");
			break;
		}
	}

	void contenuCode (float x, float y, float a, int i)
	{
		entete (x, y, a, i, e.role == 1 ? "TA FRÉQUENCE" : "FRÉQUENCE");
		const std::string code = codeSeul (e.code);
		if (code != codeAffiche)
		{
			codeAffiche = code;
			tVolets = t;
		}

		if (code.empty ())
		{
			const int points = 1 + (int)(t * 4) % 3;
			texte (f.titre, 36, x, y + 22, alpha (PAPIER, 0.5f * a), "RÉGLAGE" + std::string (points, '.'));
		}
		for (int k = 0; k < (int)code.size () && k < 6; ++k)
		{
			const float fx = x + k * 32.0f, fy = y + 18 + ((k % 2) ? 3.0f : 0.0f);
			const float pose = tVolets + 0.3f + k * 0.12f;
			const float flash = t - tCopie - k * 0.04f;
			const ImU32 fond = flash >= 0 && flash < 0.26f ? BLEU_CL : PAPIER;
			const int m = marque ();
			dl->AddRectFilled (V (fx, fy), V (fx + 29, fy + 46), alpha (fond, a));

			std::string c;
			float chute = 1.0f;
			if (t < pose)
			{
				// le volet tourne : un signe au hasard, change toutes les 55 ms
				const unsigned graine = (unsigned)(t / 0.055f) * 2654435761u + k * 97u;
				c = std::string (1, ALPHABET[(graine >> 7) % 32]);
			}
			else
			{
				c = std::string (1, code[k]);
				chute = rebond ((t - pose) / 0.26f);
			}
			const float tc = 36;
			const float wc = largeur (f.titre, tc, c);
			const int mc = marque ();
			texte (f.titre, tc, fx + 14.5f - wc / 2, fy + 6, alpha (ENCRE, a), c);
			transformer (dl, mc, translation (0, F (-30 * (1 - chute))));
			dl->AddRectFilled (V (fx, fy + 22.5f), V (fx + 29, fy + 23.5f), alpha (ENCRE, 0.25f * a));
			transformer (dl, m, rotation ((k % 2) ? 2.0f : -3.0f, V (fx + 14.5f, fy + 23)));
		}

		float ly = y + 80;
		const std::string direct = adresseDirecte (e.code);
		if (!direct.empty ())
		{
			texte (f.mono, 11, x, y + 70, alpha (BLEU_CL, a),
			       tronquer (f.mono, F (11), "DIRECT · " + direct, F (204)));
			ly = y + 88;
		}

		const std::string moi = majuscules (e.poste);
		const std::string lui = majuscules (e.pair);
		auto auditeur = [&] (float yy, bool plein, ImU32 c, const std::string& s, bool curseur) {
			carre (x, yy + 3, 8, alpha (c, a), plein);
			const std::string l = tronquer (f.mono, F (11), s, F (190));
			texte (f.mono, 11, x + 14, yy, alpha (c, a), l);
			if (curseur && std::fmod (t, 1.0f) < 0.5f)
				texte (f.mono, 11, x + 14 + largeur (f.mono, 11, l), yy, alpha (c, a), "_");
		};
		if (e.role == 1)
		{
			auditeur (ly, true, PAPIER, moi + " · HÔTE", false);
			if (lui.empty ())
				auditeur (ly + 18, false, PAPIER, "EN ATTENTE D'UN AUDITEUR", true);
			else
				auditeur (ly + 18, true, ORANGE, lui + " · À L'ÉCOUTE", false);
		}
		else
		{
			auditeur (ly, true, PAPIER, (lui.empty () ? std::string ("RECHERCHE") : lui) + " · HÔTE",
			          lui.empty ());
			auditeur (ly + 18, true, ORANGE, "TOI · INVITÉ", false);
		}
	}

	// --- bandeau defilant ------------------------------------------------------
	void dessinerBandeau ()
	{
		const int m = marque ();
		dl->AddRectFilled (V (-40, 360), V (720, 386), ENCRE);
		const std::string s = bandeau () + "   ";
		const float w = largeur (f.monoGras, 11, s);
		if (w > 1)
		{
			float x = -40 - std::fmod (t * 40.0f, w);
			for (int k = 0; k < 40 && x < 720; ++k, x += w)
				texte (f.monoGras, 11, x, 366, PAPIER, s);
		}
		transformer (dl, m, rotation (-3, V (340, 373)));
	}

	// --- volet bleu entre les deux etats ----------------------------------------
	// --- question de l'agent : par-dessus tout, tant qu'on n'a pas repondu ------
	Mat matriceDemande () const { return rotation (-2, V (340, 200)); }

	// 0 = charger, 1 = quitter, -1 = rien
	int boutonSous (ImVec2 p) const
	{
		if (p.x < 0)
			return -1;
		const ImVec2 local = inverse (matriceDemande ()) (p);
		const float x = local.x / S, y = local.y / S;
		if (y < 250 || y >= 284)
			return -1;
		if (x >= 138 && x < 300)
			return 0;
		return (x >= 312 && x < 530) ? 1 : -1;
	}

	void dessinerDemande ()
	{
		dl->AddRectFilled (V (-40, -40), V (720, 460), alpha (ENCRE, 0.62f));

		const int m = marque ();
		dl->AddRectFilled (V (124, 100), V (556, 304), ENCRE);
		dl->AddRectFilled (V (116, 92), V (548, 296), PAPIER);
		dl->AddRect (V (116, 92), V (548, 296), ENCRE, 0.0f, 0, F (2.0f));

		texte (f.monoGras, 11, 138, 110, BLEU, "EN ONDES / PROJET DE L'HÔTE");
		texte (f.titre, 34, 138, 128, ENCRE, "CHARGER SON PROJET ?");
		texte (f.monoGras, 12, 138, 170, ORANGE,
		       tronquer (f.monoGras, F (12), majuscules (e.demande), F (390)));
		texte (f.texte, 14, 138, 192, ENCRE,
		       "Ton projet actuel est sauvegardé, puis remplacé par le sien. "
		       "Sans ça, tu quittes la session.", 392);

		const bool surOui = survol == 0, surNon = survol == 1;
		dl->AddRectFilled (V (138, 250), V (300, 284), surOui ? ENCRE : BLEU);
		texte (f.titre, 20, 219 - largeur (f.titre, 20, "CHARGER") / 2, 256, PAPIER, "CHARGER");
		if (surNon)
			dl->AddRectFilled (V (312, 250), V (530, 284), ENCRE);
		dl->AddRect (V (312, 250), V (530, 284), ENCRE, 0.0f, 0, F (2.0f));
		texte (f.titre, 20, 421 - largeur (f.titre, 20, "QUITTER LA SESSION") / 2, 256,
		       surNon ? PAPIER : ENCRE, "QUITTER LA SESSION");

		transformer (dl, m, matriceDemande ());
	}

	void dessinerVolet ()
	{
		const float p = t - tVolet;
		if (p < 0 || p >= 0.48f)
			return;
		float x0 = -200, x1 = 900;
		if (p < 0.22f)
			x1 = -200 + 1100 * sortieRapide (p / 0.22f);
		else if (p >= 0.26f)
			x0 = 900 - 1100 * (1.0f - sortieRapide ((p - 0.26f) / 0.22f));
		const int m = marque ();
		dl->AddRectFilled (V (x0, -100), V (x1, 500), BLEU);
		transformer (dl, m, rotation (-6, V (350, 200)));
	}

	Mat secousse () const
	{
		const float q = (t - tSecousse) / dureeSecousse;
		if (q < 0 || q >= 1)
			return Mat ();
		// les images-cles de la maquette : 25 %, 50 %, 75 %
		const float cles[5][3] = { { 0, 0, 0 }, { 4, -3, 0.4f }, { -4, 2, -0.4f }, { 2, 1, 0 }, { 0, 0, 0 } };
		const float pos = q * 4.0f;
		const int k = std::min ((int)pos, 3);
		const float u = pos - k;
		float v[3];
		for (int j = 0; j < 3; ++j)
			v[j] = cles[k][j] + (cles[k + 1][j] - cles[k][j]) * u;
		return translation (F (v[0]), F (v[1])) * rotation (v[2], V (320, 200));
	}
};

// ==========================================================================
Station::Station () : d (new Impl ()) {}
Station::~Station () { delete d; }

void Station::image (ImDrawList* dl, const Polices& p, float echelle, float dt, const Etat& e)
{
	Impl& s = *d;
	s.dl = dl;
	s.f = p;
	s.S = echelle;
	s.dt = std::clamp (dt, 0.0f, 1.0f / 20.0f);
	s.t += s.dt;
	s.e = e;

	// --- passage hors antenne <-> en ondes : par le volet --------------------
	// (a l'ouverture de la fenetre, on se pose directement dans le bon etat)
	const bool voulu = e.role != 0;
	if (s.premiere)
	{
		s.premiere = false;
		s.enOndes = voulu;
	}
	if (voulu != s.enOndes && s.t - s.tVolet >= 0.48f)
	{
		s.tVolet = s.t;
		s.voletVers = voulu;
	}
	if (s.t - s.tVolet >= 0.24f && s.enOndes != s.voletVers && s.t - s.tVolet < 0.48f)
	{
		s.enOndes = s.voletVers;
		s.sel = 0;
		s.contenu = 0;
		s.tChoix = s.t - 1.0f;
		if (!s.enOndes)
		{
			s.codeAffiche.clear ();
		}
	}
	s.roleAffiche = e.role;
	s.lignes = lignesDuMenu (s.enOndes, e.role);
	if (s.sel >= (int)s.lignes.size ())
		s.sel = 0;

	// --- l'auditeur arrive : tout le monde le sent -----------------------------
	if (s.pairPrec.empty () && !e.pair.empty ())
	{
		s.impulsion = 1.5f;
		s.secouer (0.22f);
	}
	s.pairPrec = e.pair;
	if (s.majPrec != e.maj && (e.maj == Maj::Dispo || e.maj == Maj::Telechargement))
		s.impulsion = 1.2f;
	s.majPrec = e.maj;

	// --- souris ---------------------------------------------------------------------
	const bool question = !e.demande.empty ();
	s.survol = question ? s.boutonSous (s.souris) : s.ligneSous (s.souris);
	const bool bouge = s.souris.x != s.sourisPrec.x || s.souris.y != s.sourisPrec.y;
	if (!question && bouge && s.survol >= 0)
		s.choisir (s.survol);
	s.sourisPrec = s.souris;
	if (question)
	{
		// Le menu dessous ne repond plus. Pas d'Entree pour CHARGER : l'invite
		// vient de valider son code, un appui de trop remplacerait son projet.
		if (s.clicAttendu)
		{
			s.clicAttendu = false;
			const int b = s.boutonSous (s.clic);
			if (b == 0)
				s.demander (Action::ProjetOui);
			else if (b == 1)
				s.demander (Action::ProjetNon);
		}
	}
	else if (s.clicAttendu)
	{
		// la ligne sous l'APPUI : un « souris sortie » arrive parfois entre
		// l'appui et l'image, et le clic se perdait
		s.clicAttendu = false;
		const int ligne = s.ligneSous (s.clic);
		if (ligne >= 0)
		{
			s.choisir (ligne);
			s.valider ();
		}
	}

	// --- dessin, du fond vers l'avant ----------------------------------------------
	const ImVec2 taille = s.V ((float)kLargeur, (float)kHauteur);
	dl->AddRectFilled (ImVec2 (0, 0), taille, PAPIER);
	const int apresFond = s.marque ();

	// ImGui jette les lettres hors de la zone de decoupe AVANT nos rotations :
	// « FRUITY », qui deborde a gauche avant d'etre redresse, disparaissait.
	// La vraie decoupe (la fenetre) se fait de toute facon au rendu.
	dl->PushClipRect (ImVec2 (-4096, -4096), ImVec2 (8192, 8192), false);

	s.dessinerNumero ();
	s.dessinerOscillo ();
	s.dessinerBande ();
	s.dessinerTitre ();
	s.dessinerMenu ();
	s.dessinerEtiquette ();
	s.dessinerBandeau ();
	s.dessinerVolet ();
	if (question)
		s.dessinerDemande ();

	transformer (dl, apresFond, s.secousse ());   // le fond ne bouge pas
	dl->PopClipRect ();
}

void Station::souris (float x, float y) { d->souris = ImVec2 (x, y); }

void Station::bouton (bool appuye)
{
	if (appuye)
	{
		d->clic = d->souris;
		d->clicAttendu = true;
	}
}

bool Station::touche (int vk, bool ctrl)
{
	Impl& s = *d;
	if (!s.e.demande.empty ())
	{
		if (vk == VK_ESCAPE)
			s.demander (Action::ProjetNon);
		return true;
	}
	switch (vk)
	{
	case VK_UP:
		s.choisir (s.sel - 1);
		return true;
	case VK_DOWN:
		s.choisir (s.sel + 1);
		return true;
	case VK_RETURN:
		s.valider ();
		return true;
	case VK_BACK:
		if (s.courante () == Entree::Rejoindre && !s.saisie.empty ())
		{
			s.saisie.pop_back ();
			if (s.saisie.empty ())
				s.saisieDirecte.clear ();
			s.tErreur = -10.0f;
			return true;
		}
		return false;
	case VK_ESCAPE:
		if (s.courante () == Entree::Rejoindre && !s.saisie.empty ())
		{
			s.saisie.clear ();
			s.saisieDirecte.clear ();
			return true;
		}
		return false;
	default:
		break;
	}
	(void)ctrl;
	return false;
}

bool Station::caractere (unsigned c)
{
	Impl& s = *d;
	if (!s.e.demande.empty ())
		return true;
	if (c < 32 || c > 126)
		return false;
	const char ch = (char)toupper ((int)c);

	if (s.courante () == Entree::Rejoindre)
	{
		if (dansAlphabet (ch) && s.saisie.size () < 6)
		{
			s.tFrappeCase[s.saisie.size ()] = s.t;
			s.saisie += ch;
			s.tErreur = -10.0f;
			return true;
		}
		return dansAlphabet (ch);   // un 7e signe : on l'absorbe sans rien faire
	}

	// hors saisie, les chiffres choisissent une ligne (comme un menu de jeu)
	if (ch >= '1' && ch <= '9' && ch - '1' < (int)s.lignes.size ())
	{
		s.choisir (ch - '1');
		return true;
	}
	return false;
}

void Station::coller (const std::string& texte)
{
	Impl& s = *d;
	if (s.enOndes || !s.e.demande.empty ())
		return;
	// coller un code doit marcher depuis n'importe quelle ligne
	for (int i = 0; i < (int)s.lignes.size (); ++i)
		if (s.lignes[i].id == Entree::Rejoindre)
			s.choisir (i);
	s.saisir (texte);
}

bool Station::survolCliquable () const { return d->survol >= 0; }

bool Station::prendreAction (Action& action, std::string& argument)
{
	if (!d->actionPrete)
		return false;
	d->actionPrete = false;
	action = d->action;
	argument = d->argument;
	return true;
}

void Station::signaler (const std::string& message)
{
	d->message = message;
	d->tMessage = d->t;
}

} // namespace fmstation
