// FruityMulti - l'interface « station pirate ».
//
// Tout le dessin et toute l'animation de la fenetre. La vue (fmgui.cpp) lui
// donne l'etat de la session a chaque image, lui passe la souris et le
// clavier, et execute les actions qu'elle demande.
//
// Les coordonnees de conception sont celles d'une fenetre de 640 x 400 ; le
// dessin se fait directement en pixels (x echelle) pour garder un texte net.
#pragma once

#include <string>
#include <utility>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace fmstation {

constexpr int kLargeur = 640;
constexpr int kHauteur = 400;

enum class Maj { Repos, Verif, AJour, Dispo, Telechargement, Erreur, Redemarrer };

struct Etat
{
	int role = 0;              // 0 aucune session, 1 hote, 2 invite
	std::string code;          // K7P2QX, ou ip:port/K7P2QX en direct
	std::string statut;        // derniere phrase de l'agent
	std::string pair;          // poste en face, vide = personne
	std::string demande;       // poste qui propose son projet, vide = rien
	std::string poste;         // ce PC
	int version = 0;
	Maj maj = Maj::Repos;
	int majVersion = 0;
	int sync = 0;              // 1 lecture partagee, 2 motif partage
	// Tenus par la vue, valables pendant l'image : ne pas les recopier 60 fois
	// par seconde.
	const std::vector<std::string>* journal = nullptr;                  // dernieres lignes
	const std::vector<std::pair<char, std::string>>* compat = nullptr;  // '=' les deux, '+' ici, '-' chez l'autre
};

struct Polices
{
	ImFont* titre = nullptr;     // Archivo etroit, tres gras
	ImFont* texte = nullptr;     // Archivo demi-gras
	ImFont* mono = nullptr;      // IBM Plex Mono
	ImFont* monoGras = nullptr;
};

enum class Action { Heberger, Rejoindre, Copier, Couper, VerifierMaj, InstallerMaj,
                    ProjetOui, ProjetNon, Sync, CopierJournal };

class Station
{
public:
	Station ();
	~Station ();
	Station (const Station&) = delete;
	Station& operator= (const Station&) = delete;

	// Une image. `echelle` = pixels par unite de conception.
	void image (ImDrawList* dl, const Polices& p, float echelle, float dt, const Etat& e);

	// Entrees, en pixels de la fenetre. x < 0 : la souris est sortie.
	void souris (float x, float y);
	void bouton (bool appuye);
	void molette (float crans);        // > 0 : vers le haut
	bool touche (int vk, bool ctrl);   // vrai si la touche a servi
	bool caractere (unsigned c);
	void coller (const std::string& texte);
	bool survolCliquable () const;

	// Action demandee depuis la derniere image (une a la fois).
	bool prendreAction (Action& action, std::string& argument);

	// Message bref sur la ligne d'etat (« FRÉQUENCE COPIÉE »…).
	void signaler (const std::string& message);

private:
	struct Impl;
	Impl* d;
};

} // namespace fmstation
