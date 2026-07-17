/*
 * CrashReportPlugin.h - declaration of CrashReportPlugin class
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

#include "PluginInterface.h"

// Plugin qui installe une capture de crash à l'échelle du processus dès son
// chargement (donc dans TOUS les exécutables Veyon). Les rapports sont écrits
// dans un dossier spool local et collectés plus tard par le portail via la
// Web API (endpoint crashreport). Voir CrashHandler.
class CrashReportPlugin : public QObject, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.CrashReport")
	Q_INTERFACES(PluginInterface)
public:
	explicit CrashReportPlugin( QObject* parent = nullptr );
	~CrashReportPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("6a5f2b1e-1d4c-4c8e-9b7a-2f0e9c3d5a10") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("CrashReport");
	}

	QString description() const override
	{
		return tr( "Capture crashes and expose diagnostic reports for later analysis" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Pierrick Belledent");
	}

	QString copyright() const override
	{
		return QStringLiteral("Pierrick Belledent");
	}

private:
	static QString componentName();
	static void pruneOldReports( const QString& spoolDir, int keep );
	// Téléverse au démarrage les rapports en attente vers le portail (le Web API
	// du broker ne pouvant pas atteindre les VM). Hors gestionnaire de crash :
	// contexte normal, HTTP autorisé.
	static void uploadPendingReports( const QString& spoolDir, const QString& url, const QString& token );

	static constexpr int MaxRetainedReports = 25;

	// Budget disque total du spool (dumps compris) — le plus contraignant
	// de la limite en nombre et en octets s'applique.
	static constexpr qint64 MaxSpoolBytes = 512 * 1024 * 1024;

};
