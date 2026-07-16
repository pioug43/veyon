/*
 * ExamModeWindowsNative.h - shell-free Windows enforcement primitives
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#pragma once

#include <QJsonObject>
#include <QStringList>

namespace ExamModeWindowsNative
{

QJsonObject readRegistryValue( const QString& key, const QString& name );
bool setRegistryValue( const QString& key, const QString& name, const QString& type, const QString& data );
bool restoreRegistryValue( const QString& key, const QString& name, const QJsonObject& value );
bool deleteRegistryValue( const QString& key, const QString& name );

struct EnforcementResult
{
	QStringList detected;
	QStringList terminated;
	QStringList failed;
};

EnforcementResult terminateProcesses( const QStringList& imageNames );
bool restrictFileToAdministratorsAndSystem( const QString& path );

// État plein écran du client VDI (Omnissa/VMware Horizon) sur ce poste.
enum class VdiClientState
{
	Unknown,		// non concluant : aucune fenêtre client visible (client absent,
					// minimisé/fermé, ou exécution hors session interactive p.ex. SYSTEM)
	Fullscreen,		// une fenêtre du client couvre tout l'écran (barre des tâches masquée)
	NotFullscreen,	// le client est visible mais en fenêtré/maximisé (hôte accessible)
};

// Détermine si le client VDI Omnissa Horizon occupe tout un écran. Enumère les
// fenêtres visibles, identifie le processus client (vmware-view / omnissa /
// horizon+client) et compare la fenêtre au rectangle du moniteur. Unknown quand
// rien n'est trouvé : à traiter comme « impossible de vérifier », PAS comme une
// violation (évite les faux positifs quand l'appel ne voit pas le bureau user).
VdiClientState vdiClientFullscreenState();

}
