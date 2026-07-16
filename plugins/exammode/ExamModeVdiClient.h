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

// Contrôles du client VDI Omnissa Horizon (ex-VMware Horizon) sur le poste
// physique. Windows : Win32 (user32/wtsapi32). Linux : X11/EWMH + notify-send.
// Le processus client est identifié par son exécutable (vmware-view / omnissa /
// horizon+client), robuste au renommage VMware→Omnissa et aux versions.
namespace ExamModeVdiClient
{

enum class State
{
	Unknown,		// non concluant : aucune fenêtre client visible (client absent,
					// minimisé/fermé, ou pas d'accès à la session graphique — p.ex.
					// exécution en SYSTEM/root sans DISPLAY). À NE PAS traiter comme
					// une violation (évite les faux positifs).
	Fullscreen,		// une fenêtre du client couvre tout l'écran (hôte masqué)
	NotFullscreen,	// le client est visible mais fenêtré/maximisé (hôte accessible)
};

// Le client VDI occupe-t-il tout un écran ?
State fullscreenState();

// Tente (best-effort) de forcer le client en plein écran. Windows : étend la
// fenêtre pour couvrir le moniteur. Linux : requête EWMH _NET_WM_STATE_FULLSCREEN
// au gestionnaire de fenêtres. Renvoie true si l'état plein écran est constaté
// après l'opération.
bool forceFullscreen();

// Affiche un message NON bloquant dans la session graphique de l'étudiant, avec
// auto-fermeture après timeoutSeconds. Windows : WTSSendMessage (fonctionne même
// depuis SYSTEM). Linux : notify-send (best-effort ; nécessite le bus de session
// utilisateur). No-op si l'affichage n'est pas atteignable.
void showSessionMessage( const QString& title, const QString& text, int timeoutSeconds );

}
