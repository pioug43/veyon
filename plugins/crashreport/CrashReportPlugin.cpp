/*
 * CrashReportPlugin.cpp - implementation of CrashReportPlugin class
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

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "CrashReportPlugin.h"
#include "CrashHandler.h"
#include "Filesystem.h"
#include "VeyonConfiguration.h"


CrashReportPlugin::CrashReportPlugin( QObject* parent ) :
	QObject( parent )
{
	if( VeyonCore::config().crashReportingEnabled() == false )
	{
		return;
	}

	const auto spoolDir = VeyonCore::filesystem().expandPath( VeyonCore::config().crashReportDirectory() );
	if( VeyonCore::filesystem().ensurePathExists( spoolDir ) == false )
	{
		vWarning() << "CrashReport: unable to create spool directory" << spoolDir << "- crash reporting disabled";
		return;
	}

	// Purge des anciens rapports au démarrage (jamais dans le gestionnaire de
	// crash, qui doit rester minimal).
	pruneOldReports( spoolDir, MaxRetainedReports );

	const auto logDir = VeyonCore::filesystem().expandPath( VeyonCore::config().logFileDirectory() );

	CrashHandler::install( spoolDir, componentName(), logDir );

	vDebug() << "CrashReport: crash capture installed for" << componentName() << "-> spool" << spoolDir;
}



QString CrashReportPlugin::componentName()
{
	switch( VeyonCore::component() )
	{
	case VeyonCore::Component::Service: return QStringLiteral("Service");
	case VeyonCore::Component::Server: return QStringLiteral("Server");
	case VeyonCore::Component::Worker: return QStringLiteral("Worker");
	case VeyonCore::Component::Master: return QStringLiteral("Master");
	case VeyonCore::Component::CLI: return QStringLiteral("CLI");
	case VeyonCore::Component::Configurator: return QStringLiteral("Configurator");
	}

	return QStringLiteral("Unknown");
}



void CrashReportPlugin::pruneOldReports( const QString& spoolDir, int keep )
{
	// Tous les fichiers d'un même rapport partagent la base « crash-* » ; on
	// trie par date (plus récents d'abord) et on supprime au-delà de `keep`
	// rapports .json, en emportant les fichiers frères (.dmp/.trace).
	QDir dir( spoolDir );
	const auto reports = dir.entryInfoList( { QStringLiteral("crash-*.json") }, QDir::Files, QDir::Time );

	for( int i = keep; i < reports.size(); ++i )
	{
		const auto base = reports.at( i ).completeBaseName();
		const auto siblings = dir.entryInfoList( { base + QStringLiteral(".*") }, QDir::Files );
		for( const auto& sibling : siblings )
		{
			QFile::remove( sibling.absoluteFilePath() );
		}
	}
}
