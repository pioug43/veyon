/*
 * ExamModeVdiClient.h - cross-platform VDI client (Omnissa/VMware Horizon) checks
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This file is part of Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#pragma once

#include <QString>

// Contrôles du client VDI Omnissa Horizon (ex-VMware Horizon).
// En invité VDI, le client tourne sur le POSTE PHYSIQUE, hors de la VM — on
// compare donc les écrans PHYSIQUES du poste, rapportés par le client via
// l'agent Horizon (ViewClient_Displays.Topology), à ce que la session distante
// affiche RÉELLEMENT ; couverture partielle = hôte accessible = NotFullscreen.
//   Windows : registre agent (SessionData + Blast\Telemetry, repli
//             EnumDisplayMonitors), lisible par le service SYSTEM.
//   Linux   : environnement de session ViewClient_* (/proc/<pid>/environ) +
//             XRandR (repli : étendue de l'écran X).
// Sur poste PHYSIQUE Linux (client local), détection fenêtre X11/EWMH + forçage
// plein écran par requête EWMH (voir la note dans le .cpp).
namespace ExamModeVdiClient
{

enum class State
{
	Unknown,		// non concluant : topologie du client VDI illisible (poste
					// physique/console, hors session Horizon, ou données agent
					// absentes). À NE PAS traiter comme une violation (évite les
					// faux positifs).
	Fullscreen,		// la session distante couvre tout le matériel physique du poste
					// (tous les écrans) → l'hôte physique est masqué
	NotFullscreen,	// la session distante ne couvre qu'une partie des écrans du poste
					// (client fenêtré, ou plein écran sur un seul écran) → hôte accessible
};

// Le client VDI occupe-t-il tout un écran ?
State fullscreenState();

// Tente (best-effort) de forcer le client en plein écran. Windows : impossible
// depuis l'invité (le client est sur le poste physique) → simple constat de l'état.
// Linux : requête EWMH _NET_WM_STATE_FULLSCREEN au WM (si Veyon tourne sur le poste
// du client). Renvoie true si l'état plein écran est constaté après l'opération.
bool forceFullscreen();

// Affiche un message NON bloquant dans la session graphique de l'étudiant, avec
// auto-fermeture après timeoutSeconds. Windows : WTSSendMessage (fonctionne même
// depuis SYSTEM). Linux : notify-send (best-effort ; nécessite le bus de session
// utilisateur). No-op si l'affichage n'est pas atteignable.
void showSessionMessage( const QString& title, const QString& text, int timeoutSeconds );

}
