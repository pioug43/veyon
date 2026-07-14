/*
 * ExamModeFeaturePlugin.cpp - implementation of ExamModeFeaturePlugin class
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

#include <QFile>
#include <QProcess>
#include <QTimer>

#include "ExamModeFeaturePlugin.h"
#include "FeatureMessage.h"
#include "VeyonCore.h"
#include "VeyonServerInterface.h"

// intervalle de rappel de l'application des restrictions (fin des processus interdits)
static constexpr int EnforceIntervalMs = 4000;


ExamModeFeaturePlugin::ExamModeFeaturePlugin( QObject* parent ) :
	QObject( parent ),
	m_examModeFeature( QStringLiteral( "ExamMode" ),
					   Feature::Flag::Mode | Feature::Flag::AllComponents,
					   Feature::Uid( "c1d2e3f4-0a1b-4c2d-8e3f-4a5b6c7d8e90" ),
					   Feature::Uid(),
					   tr( "Start exam mode" ), tr( "Stop exam mode" ),
					   tr( "Enforce an exam profile: blocked applications are terminated "
						   "and blocked websites are made unreachable while active." ) ),
	m_features( { m_examModeFeature } )
{
}



ExamModeFeaturePlugin::~ExamModeFeaturePlugin()
{
	// filet de sécurité : ne jamais laisser le fichier hosts modifié derrière soi
	if( m_hostsModified )
	{
		revertHostsBlocking();
	}
}



bool ExamModeFeaturePlugin::controlFeature( Feature::Uid featureUid, Operation operation,
										   const QVariantMap& arguments,
										   const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( hasFeature( featureUid ) == false )
	{
		return false;
	}

	if( operation == Operation::Start )
	{
		const auto apps = arguments.value( argToString( Argument::BlockedApps ) ).toStringList();
		const auto sites = arguments.value( argToString( Argument::Sites ) ).toStringList();
		const auto sitesMode = arguments.value( argToString( Argument::SitesMode ),
												QStringLiteral("block") ).toString();

		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StartExam }
							.addArgument( Argument::BlockedApps, apps )
							.addArgument( Argument::Sites, sites )
							.addArgument( Argument::SitesMode, sitesMode ),
							computerControlInterfaces );
		return true;
	}

	if( operation == Operation::Stop )
	{
		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StopExam }, computerControlInterfaces );
		return true;
	}

	return false;
}



bool ExamModeFeaturePlugin::handleFeatureMessage( VeyonServerInterface& server,
												  const MessageContext& messageContext,
												  const FeatureMessage& message )
{
	Q_UNUSED(server)
	Q_UNUSED(messageContext)

	if( message.featureUid() != m_examModeFeature.uid() )
	{
		return false;
	}

	// l'application des restrictions (processus, hosts) n'a de sens que dans le
	// service (composant SYSTEM/root) — pas dans le master.
	if( VeyonCore::component() != VeyonCore::Component::Service )
	{
		return true;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartExam:
		startEnforcement( message.argument( Argument::BlockedApps ).toStringList(),
						  message.argument( Argument::Sites ).toStringList(),
						  message.argument( Argument::SitesMode ).toString() );
		return true;

	case FeatureCommand::StopExam:
		stopEnforcement();
		return true;

	default:
		break;
	}

	return false;
}



bool ExamModeFeaturePlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	Q_UNUSED(worker)
	Q_UNUSED(message)

	// pas d'action côté worker : le verrouillage éventuel réutilise ScreenLock.
	return false;
}



void ExamModeFeaturePlugin::startEnforcement( const QStringList& blockedApps, const QStringList& sites,
											  const QString& sitesMode )
{
	m_blockedApps = blockedApps;
	m_sites = sites;
	m_sitesMode = sitesMode.isEmpty() ? QStringLiteral("block") : sitesMode;
	m_active = true;

	vInfo() << "ExamMode: enforcement started -" << m_blockedApps.size() << "app(s),"
			<< m_sites.size() << "site(s), mode" << m_sitesMode;

	applyHostsBlocking( m_sites );

	if( m_timer == nullptr )
	{
		m_timer = new QTimer( this );
		connect( m_timer, &QTimer::timeout, this, &ExamModeFeaturePlugin::enforceTick );
	}
	m_timer->start( EnforceIntervalMs );
	enforceTick();		// première passe immédiate
}



void ExamModeFeaturePlugin::stopEnforcement()
{
	m_active = false;
	if( m_timer )
	{
		m_timer->stop();
	}
	revertHostsBlocking();
	vInfo() << "ExamMode: enforcement stopped";
}



void ExamModeFeaturePlugin::enforceTick()
{
	if( m_active == false )
	{
		return;
	}
	for( const auto& app : m_blockedApps )
	{
		killApplication( app );
	}
}



void ExamModeFeaturePlugin::killApplication( const QString& executable ) const
{
	const auto name = executable.trimmed();
	if( name.isEmpty() )
	{
		return;
	}

#if defined(Q_OS_WIN)
	// /F force, /IM par nom d'image ; on garantit le suffixe .exe
	auto image = name;
	if( image.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) == false )
	{
		image += QStringLiteral(".exe");
	}
	QProcess::startDetached( QStringLiteral("taskkill"), { QStringLiteral("/F"), QStringLiteral("/IM"), image } );
#else
	// -f : correspond sur la ligne de commande complète (Linux VDI)
	QProcess::startDetached( QStringLiteral("pkill"), { QStringLiteral("-f"), name } );
#endif
}



QString ExamModeFeaturePlugin::hostsFilePath()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/Windows/System32/drivers/etc/hosts");
#else
	return QStringLiteral("/etc/hosts");
#endif
}



void ExamModeFeaturePlugin::applyHostsBlocking( const QStringList& sites )
{
	// Le fichier hosts ne permet qu'une liste NOIRE (rediriger un domaine vers
	// 127.0.0.1). Le mode "allow" (liste blanche) nécessiterait un proxy/pare-feu :
	// non couvert ici, on avertit et on n'altère pas le système.
	if( m_sitesMode != QStringLiteral("block") )
	{
		vWarning() << "ExamMode: sites_mode" << m_sitesMode
				   << "non supporté via hosts (liste blanche) — sites non filtrés";
		return;
	}
	if( sites.isEmpty() )
	{
		return;
	}

	revertHostsBlocking();		// repart d'un fichier propre

	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadWrite | QIODevice::Text ) == false )
	{
		vWarning() << "ExamMode: impossible d'ouvrir le fichier hosts" << hostsFilePath();
		return;
	}

	QByteArray content = file.readAll();
	if( content.isEmpty() == false && content.endsWith( '\n' ) == false )
	{
		content.append( '\n' );
	}

	content.append( QByteArrayLiteral("\n") ).append( HostsMarkerBegin ).append( '\n' );
	for( const auto& site : sites )
	{
		const auto host = site.trimmed();
		if( host.isEmpty() )
		{
			continue;
		}
		const auto line = QStringLiteral("127.0.0.1\t%1\n::1\t%1\n").arg( host );
		content.append( line.toUtf8() );
	}
	content.append( HostsMarkerEnd ).append( '\n' );

	file.resize( 0 );
	file.write( content );
	file.close();
	m_hostsModified = true;
}



void ExamModeFeaturePlugin::revertHostsBlocking()
{
	if( m_hostsModified == false )
	{
		return;
	}

	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadWrite | QIODevice::Text ) == false )
	{
		return;
	}

	const auto lines = QString::fromUtf8( file.readAll() ).split( QLatin1Char('\n') );
	QString rebuilt;
	bool inSection = false;
	for( const auto& line : lines )
	{
		if( line.startsWith( QLatin1String(HostsMarkerBegin) ) )
		{
			inSection = true;
			continue;
		}
		if( line.startsWith( QLatin1String(HostsMarkerEnd) ) )
		{
			inSection = false;
			continue;
		}
		if( inSection == false )
		{
			rebuilt += line + QLatin1Char('\n');
		}
	}
	// retire les sauts de ligne surnuméraires laissés en fin de fichier
	while( rebuilt.endsWith( QLatin1String("\n\n") ) )
	{
		rebuilt.chop( 1 );
	}

	file.resize( 0 );
	file.write( rebuilt.toUtf8() );
	file.close();
	m_hostsModified = false;
}
