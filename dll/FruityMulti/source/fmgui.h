// FruityMulti - fenetre du plugin (Héberger / Rejoindre / Mise à jour)
//
// Le VST3 demande sa vue par createView() ; cette vue est une fenêtre Win32
// dessinée par nos soins, comme n'importe quel plugin (Serum, FabFilter...).
#pragma once

#include "pluginterfaces/base/funknown.h"

namespace Steinberg {
class IPlugView;
}

// Crée la vue. A appeler depuis BridgeProcessor::createView.
Steinberg::IPlugView* fmCreateView ();

// Etat partage entre l'agent (qui parle au plugin) et la fenêtre.
namespace fmgui {

// role : 0 = aucune session, 1 = hote, 2 = invite
void setRole (int role);
void setStatus (const char* texte);
void setAddress (const char* adresse);
void soumettreEtat (const char* texte);

int  role ();
bool agentLance ();

// Version du plugin, telle qu'annoncee par la commande VERSION.
constexpr int kVersionPlugin = 27;

} // namespace fmgui
