// FruityMulti - fenetre du plugin
//
// Le VST3 demande sa vue par createView() ; cette vue est une fenetre Win32
// rendue en DirectX 11 avec Dear ImGui (voir fmstation.cpp pour le dessin).
#pragma once

#include "pluginterfaces/base/funknown.h"

#include <string>
#include <utility>
#include <vector>

namespace Steinberg {
class IPlugView;
}

// Cree la vue. A appeler depuis BridgeProcessor::createView.
Steinberg::IPlugView* fmCreateView ();

// Etat partage entre l'agent (qui parle au plugin) et la fenetre.
namespace fmgui {

// role : 0 = aucune session, 1 = hote, 2 = invite
void setRole (int role);
void setStatus (const char* texte);
void setAddress (const char* adresse);
void setPeer (const char* nom);       // "" = personne en face
// Derniere phrase de l'agent avant de s'arreter : elle reste affichee.
void setErreur (const char* texte);
void soumettreEtat (const char* texte);

int  role ();
bool agentLance ();

// L'agent s'est deconnecte du plugin : plus de session.
void agentParti ();

// Version du plugin, telle qu'annoncee par la commande VERSION.
constexpr int kVersionPlugin = 30;

} // namespace fmgui
