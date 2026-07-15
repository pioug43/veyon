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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <limits>

#include "CryptoCore.h"
#include "ExamModeFeaturePlugin.h"
#include "ExamModeLinuxExecGuard.h"
#include "ExamModeProfile.h"
#include "ExamModeSession.h"
#include "ExamModeWindowsNative.h"
#include "FeatureMessage.h"
#include "Filesystem.h"
#include "VeyonCore.h"
#include "VeyonServerInterface.h"

// intervalle de rappel de l'application des restrictions (fin des processus
// interdits). Court pour réduire la fenêtre pendant laquelle une application
// interdite peut être lancée et utilisée entre deux passes.
static constexpr int EnforceIntervalMs = 1500;
// fail-safe : le portail ré-applique le profil chaque minute ; sans re-push
// pendant ce délai, on lève automatiquement toutes les restrictions.
static constexpr int DefaultLeaseSeconds = 5 * 60;
static constexpr int MinimumLeaseSeconds = 60;
static constexpr int MaximumLeaseSeconds = 60 * 60;

namespace
{

bool writeJsonFile( const QString& path, const QJsonObject& object )
{
	QDir().mkpath( QFileInfo( path ).absolutePath() );
	QSaveFile file( path );
	if( file.open( QIODevice::WriteOnly ) == false )
	{
		return false;
	}
#if !defined(Q_OS_WIN)
	if( file.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner ) == false )
	{
		file.cancelWriting();
		return false;
	}
#endif
	if( file.write( QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) < 0 )
	{
		file.cancelWriting();
		return false;
	}
	if( file.commit() == false )
	{
		return false;
	}
#if defined(Q_OS_WIN)
	return ExamModeWindowsNative::restrictFileToAdministratorsAndSystem( path );
#else
	return true;
#endif
}


QJsonObject readJsonFile( const QString& path )
{
	QFile file( path );
	if( file.open( QIODevice::ReadOnly ) == false )
	{
		return {};
	}
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson( file.readAll(), &error );
	return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

}


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
	// Filet de sécurité au démarrage du service : si un examen précédent s'est
	// terminé par un crash/redémarrage, le fichier hosts peut être resté modifié
	// (postes injoignables). On nettoie toute section résiduelle : un examen
	// encore actif sera ré-appliqué par le portail (re-push chaque minute).
	// Nettoyage de sûreté au démarrage des composants du POSTE (Service au boot,
	// Server à l'ouverture de session) — jamais sur le Master (poste enseignant).
	if( isEndpointComponent() )
	{
		loadReplayState();
		const auto recoverableState = readJsonFile( activeStateFile() );
		const auto recoverableUntil = recoverableState.value(
			QStringLiteral("expiresAtMs") ).toVariant().toLongLong();
		if( recoverableState.value( QStringLiteral("version") ).toInt() == 1 &&
			recoverableUntil > QDateTime::currentMSecsSinceEpoch() )
		{
			vInfo() << "ExamMode: état actif non expiré détecté; reprise transactionnelle programmée";
			QTimer::singleShot( 0, this, &ExamModeFeaturePlugin::recoverActiveState );
			return;
		}
		QFile::remove( activeStateFile() );
		if( removeHostsSection() )
		{
			flushDnsCache();
		}
		cleanupStaleLaunchPrevention();
#if defined(Q_OS_WIN)
		cleanupLegacyWindowsState();
		if( QFile::exists( siteFilterStateFile() ) )
		{
			vInfo() << "ExamMode: restauration d'un filtrage de sites résiduel";
			removeWindowsSiteFiltering();
		}
		// firewall egress résiduel (crash pendant un examen) : son marqueur est
		// indépendant du filtrage PAC (profil purement CIDR). Un examen encore actif
		// sera ré-appliqué par le portail au prochain re-push.
		else if( QFile::exists( windowsFirewallStateFile() ) )
		{
			vInfo() << "ExamMode: retrait d'un firewall egress Windows résiduel";
			removeWindowsFirewall();
		}
#endif
#if defined(Q_OS_LINUX)
		// firewall egress résiduel (crash pendant un examen) : on retire la table
		// et on désarme le dead-man. Un examen encore actif sera ré-appliqué par
		// le portail au prochain re-push.
		if( QFile::exists( firewallStateFile() ) )
		{
			vInfo() << "ExamMode: retrait d'un firewall egress résiduel";
			removeLinuxFirewall();
		}
#endif
	}
}



ExamModeFeaturePlugin::~ExamModeFeaturePlugin()
{
	if( isEndpointComponent() && m_active )
	{
		stopEnforcement();
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
		const auto leaseSeconds = arguments.value( argToString( Argument::LeaseSeconds ),
													DefaultLeaseSeconds ).toInt();
		const auto windowsApps = arguments.value( argToString( Argument::BlockedAppsWindows ) ).toStringList();
		const auto linuxApps = arguments.value( argToString( Argument::BlockedAppsLinux ) ).toStringList();
		const auto macosApps = arguments.value( argToString( Argument::BlockedAppsMacos ) ).toStringList();
		const auto processRules = arguments.value( argToString( Argument::ProcessRules ) ).toList();
		const auto urlRules = arguments.value( argToString( Argument::UrlRules ) ).toList();
		const auto urlDefaultAction = arguments.value( argToString( Argument::UrlDefaultAction ) ).toString();
		const auto profileId = arguments.value( argToString( Argument::ProfileId ), QStringLiteral("legacy") ).toString();
		const auto profileRevision = arguments.value( argToString( Argument::ProfileRevision ) ).toLongLong();
		const auto profileDigest = arguments.value( argToString( Argument::ProfileDigest ) ).toString();
		const auto strict = arguments.value( argToString( Argument::Strict ) ).toBool();
		const auto networkBackend = arguments.value( argToString( Argument::NetworkBackend ) ).toString();
		const auto allowedNetworks = arguments.value( argToString( Argument::AllowedNetworks ) ).toStringList();
		const auto dnsServers = arguments.value( argToString( Argument::DnsServers ) ).toStringList();
		const auto supervisionNetworks = arguments.value( argToString( Argument::SupervisionNetworks ) ).toStringList();
		const auto sessionId = arguments.value( argToString( Argument::SessionId ) ).toString();
		const auto sequence = arguments.value( argToString( Argument::Sequence ) ).toULongLong();
		const auto issuedAt = arguments.value( argToString( Argument::IssuedAt ) ).toLongLong();
		const auto expiresAt = arguments.value( argToString( Argument::ExpiresAt ) ).toLongLong();
		const auto highSecurity = arguments.value( argToString( Argument::HighSecurity ) ).toBool();
		const auto requiredCapabilities = arguments.value( argToString( Argument::RequiredCapabilities ) ).toStringList();
		const auto externalCapabilities = arguments.value( argToString( Argument::ExternalCapabilities ) ).toMap();
		const auto signingKeyId = arguments.value( argToString( Argument::SigningKeyId ) ).toString();
		const auto profileSignature = arguments.value( argToString( Argument::ProfileSignature ) );

		FeatureMessage outbound{ featureUid, FeatureCommand::StartExam };
		outbound.addArgument( Argument::BlockedApps, apps )
							.addArgument( Argument::Sites, sites )
							.addArgument( Argument::SitesMode, sitesMode )
							.addArgument( Argument::LeaseSeconds, leaseSeconds )
							.addArgument( Argument::BlockedAppsWindows, windowsApps )
							.addArgument( Argument::BlockedAppsLinux, linuxApps )
							.addArgument( Argument::BlockedAppsMacos, macosApps )
							.addArgument( Argument::ProcessRules, processRules )
							.addArgument( Argument::UrlRules, urlRules )
							.addArgument( Argument::UrlDefaultAction, urlDefaultAction )
							.addArgument( Argument::ProfileId, profileId )
							.addArgument( Argument::ProfileRevision, profileRevision )
							.addArgument( Argument::ProfileDigest, profileDigest )
							.addArgument( Argument::Strict, strict )
							.addArgument( Argument::NetworkBackend, networkBackend )
							.addArgument( Argument::AllowedNetworks, allowedNetworks )
							.addArgument( Argument::DnsServers, dnsServers )
							.addArgument( Argument::SupervisionNetworks, supervisionNetworks )
							.addArgument( Argument::SessionId, sessionId )
							.addArgument( Argument::Sequence, sequence )
							.addArgument( Argument::IssuedAt, issuedAt )
							.addArgument( Argument::ExpiresAt, expiresAt )
							.addArgument( Argument::HighSecurity, highSecurity )
							.addArgument( Argument::RequiredCapabilities, requiredCapabilities )
							.addArgument( Argument::ExternalCapabilities, externalCapabilities )
							.addArgument( Argument::SigningKeyId, signingKeyId )
							.addArgument( Argument::ProfileSignature, profileSignature );
		{
			QMutexLocker locker( &m_remoteStatusMutex );
			for( const auto& controlInterface : computerControlInterfaces )
			{
				const auto rawInterface = controlInterface.data();
				if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
				{
					m_trackedRemoteInterfaces.insert( rawInterface );
					connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
						QMutexLocker statusLocker( &m_remoteStatusMutex );
						m_remoteStatuses.remove( rawInterface );
						m_trackedRemoteInterfaces.remove( rawInterface );
					} );
				}
				m_remoteStatuses.insert( controlInterface.data(), QVariantMap{
					{ QStringLiteral("status"), QStringLiteral("PENDING") },
					{ QStringLiteral("sessionId"), sessionId },
					{ QStringLiteral("sequence"), sequence },
					{ QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch() },
				} );
			}
		}
		sendFeatureMessage( outbound, computerControlInterfaces );
		return true;
	}

	if( operation == Operation::Stop )
	{
		const auto now = QDateTime::currentMSecsSinceEpoch();
		const auto sessionId = arguments.value( argToString( Argument::SessionId ) ).toString();
		const auto sequence = arguments.value( argToString( Argument::Sequence ) ).toULongLong();
		FeatureMessage outbound{ featureUid, FeatureCommand::StopExam };
		outbound.addArgument( Argument::SessionId, sessionId )
			.addArgument( Argument::Sequence, sequence )
			.addArgument( Argument::IssuedAt, arguments.value( argToString( Argument::IssuedAt ), now ) )
			.addArgument( Argument::ExpiresAt, arguments.value( argToString( Argument::ExpiresAt ), now + 300000 ) )
			.addArgument( Argument::HighSecurity, arguments.value( argToString( Argument::HighSecurity ) ) )
			.addArgument( Argument::RequiredCapabilities, arguments.value( argToString( Argument::RequiredCapabilities ) ) )
			.addArgument( Argument::ExternalCapabilities, arguments.value( argToString( Argument::ExternalCapabilities ) ) )
			.addArgument( Argument::SigningKeyId, arguments.value( argToString( Argument::SigningKeyId ) ) )
			.addArgument( Argument::ProfileSignature, arguments.value( argToString( Argument::ProfileSignature ) ) );
		{
			QMutexLocker locker( &m_remoteStatusMutex );
			for( const auto& controlInterface : computerControlInterfaces )
			{
				const auto rawInterface = controlInterface.data();
				if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
				{
					m_trackedRemoteInterfaces.insert( rawInterface );
					connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
						QMutexLocker statusLocker( &m_remoteStatusMutex );
						m_remoteStatuses.remove( rawInterface );
						m_trackedRemoteInterfaces.remove( rawInterface );
					} );
				}
				m_remoteStatuses.insert( controlInterface.data(), QVariantMap{
					{ QStringLiteral("status"), QStringLiteral("PENDING") },
					{ QStringLiteral("sessionId"), sessionId },
					{ QStringLiteral("sequence"), sequence },
					{ QStringLiteral("timestamp"), now },
				} );
			}
		}
		sendFeatureMessage( outbound, computerControlInterfaces );
		return true;
	}

	return false;
}



bool ExamModeFeaturePlugin::handleFeatureMessage( VeyonServerInterface& server,
												  const MessageContext& messageContext,
												  const FeatureMessage& message )
{
	if( message.featureUid() != m_examModeFeature.uid() )
	{
		return false;
	}

	// handleFeatureMessage(server) s'exécute dans le composant Server du poste,
	// suffisamment privilégié pour agir sur le système (ScreenLock y désactive
	// déjà les périphériques d'entrée) : on y applique donc hosts/registre/kill.
	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartExam:
	{
		const auto envelope = envelopeFromMessage( message );
		m_statusSessionId = envelope.sessionId;
		m_statusSequence = envelope.sequence;
		auto applications = message.argument( Argument::BlockedApps ).toStringList();
		QString platform;
#if defined(Q_OS_WIN)
		platform = QStringLiteral("windows");
		applications.append( message.argument( Argument::BlockedAppsWindows ).toStringList() );
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
		platform = QStringLiteral("macos");
		applications.append( message.argument( Argument::BlockedAppsMacos ).toStringList() );
#else
		platform = QStringLiteral("linux");
		applications.append( message.argument( Argument::BlockedAppsLinux ).toStringList() );
#endif
		// Mode strict (opt-in) : ajoute shells/interpréteurs/outils système de la
		// plateforme au blocage (terminate + prevent-launch), défense en profondeur
		// contre l'exécution de code arbitraire et l'arrêt d'ExamMode.
		if( message.argument( Argument::Strict ).toBool() )
		{
			applications.append( ExamModeProfile::strictModeApplications( platform ) );
			vInfo() << "ExamMode: mode strict actif — interpréteurs/outils système bloqués";
		}
		QVariantList processRules;
		for( const auto& application : applications )
		{
			processRules.append( QVariantMap{
				{ QStringLiteral("active"), true }, { QStringLiteral("os"), QStringLiteral("all") },
				{ QStringLiteral("executable"), application }, { QStringLiteral("action"), QStringLiteral("block") },
				{ QStringLiteral("strongKill"), true },
			} );
		}
		processRules.append( message.argument( Argument::ProcessRules ).toList() );

		// Construire toute la politique candidate sans toucher à l'état actif. La
		// transaction ne publie ces valeurs qu'après validation de l'enveloppe.
		bool validBackend = false;
		auto networkBackend = ExamModeProfile::networkBackendFromString(
			message.argument( Argument::NetworkBackend ).toString(), &validBackend );
		if( validBackend == false )
		{
			vWarning() << "ExamMode: backend réseau invalide — profil refusé";
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_NETWORK_BACKEND"),
				QStringLiteral("Unknown network backend") );
			return server.sendFeatureMessageReply( messageContext, statusMessage() );
		}
		QStringList rejectedNetworks;
		ExamModeProfile::NetworkPolicy networkPolicy{
			networkBackend,
			ExamModeProfile::normalizeNetworks( message.argument( Argument::AllowedNetworks ).toStringList(), &rejectedNetworks ),
			ExamModeProfile::normalizeNetworks( message.argument( Argument::DnsServers ).toStringList(), &rejectedNetworks ),
			ExamModeProfile::normalizeNetworks( message.argument( Argument::SupervisionNetworks ).toStringList(), &rejectedNetworks ),
		};
		if( rejectedNetworks.isEmpty() == false )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_NETWORK"),
				QStringLiteral("One or more network entries are invalid") );
			return server.sendFeatureMessageReply( messageContext, statusMessage() );
		}
		QStringList rejectedProcesses;
		const auto processPolicy = ExamModeProfile::resolveProcessRules( processRules, platform, &rejectedProcesses );
		QStringList rejectedUrls;
		const auto urlRules = ExamModeProfile::normalizeUrlRules(
			message.argument( Argument::UrlRules ).toList(), &rejectedUrls );
		if( rejectedProcesses.isEmpty() == false )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_PROCESS_RULE"),
				QStringLiteral("One or more process rules are invalid") );
			return server.sendFeatureMessageReply( messageContext, statusMessage() );
		}
		if( rejectedUrls.isEmpty() == false )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_URL_RULE"),
				QStringLiteral("One or more URL rules are invalid") );
			return server.sendFeatureMessageReply( messageContext, statusMessage() );
		}
		// le message est traité même si le profil est refusé (validation) :
		// on retourne toujours true pour ne pas le laisser paraître non géré.
		if( startEnforcement( processPolicy, message.argument( Argument::Sites ).toStringList(), urlRules,
				message.argument( Argument::SitesMode ).toString(),
				message.argument( Argument::UrlDefaultAction ).toString(),
				message.hasArgument( Argument::LeaseSeconds ) ?
					message.argument( Argument::LeaseSeconds ).toInt() : DefaultLeaseSeconds,
				message.argument( Argument::ProfileId ).toString(),
				message.argument( Argument::ProfileRevision ).toLongLong(),
				message.argument( Argument::ProfileDigest ).toString(), networkPolicy, envelope ) == false )
		{
			vWarning() << "ExamMode: profil StartExam refusé — restrictions inchangées";
		}
		return server.sendFeatureMessageReply( messageContext, statusMessage() );
	}
	case FeatureCommand::StopExam:
		stopEnforcement( envelopeFromMessage( message ) );
		return server.sendFeatureMessageReply( messageContext, statusMessage() );

	case FeatureCommand::ExamStatus:
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



bool ExamModeFeaturePlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
												  const FeatureMessage& message )
{
	if( message.featureUid() != m_examModeFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::ExamStatus )
	{
		return false;
	}
	QVariantMap status{
		{ QStringLiteral("status"), message.argument( Argument::Status ) },
		{ QStringLiteral("errorCode"), message.argument( Argument::ErrorCode ) },
		{ QStringLiteral("errorMessage"), message.argument( Argument::ErrorMessage ) },
		{ QStringLiteral("effectiveDigest"), message.argument( Argument::EffectiveDigest ) },
		{ QStringLiteral("capabilities"), message.argument( Argument::Capabilities ) },
		{ QStringLiteral("backendResults"), message.argument( Argument::BackendResults ) },
		{ QStringLiteral("sessionId"), message.argument( Argument::SessionId ) },
		{ QStringLiteral("sequence"), message.argument( Argument::Sequence ) },
		{ QStringLiteral("timestamp"), message.argument( Argument::Timestamp ) },
	};
	QMutexLocker locker( &m_remoteStatusMutex );
	const auto rawInterface = computerControlInterface.data();
	if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
	{
		m_trackedRemoteInterfaces.insert( rawInterface );
		connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
			QMutexLocker statusLocker( &m_remoteStatusMutex );
			m_remoteStatuses.remove( rawInterface );
			m_trackedRemoteInterfaces.remove( rawInterface );
		} );
	}
	m_remoteStatuses.insert( computerControlInterface.data(), status );
	return true;
}



QVariantMap ExamModeFeaturePlugin::featureStatus( Feature::Uid featureUid,
												  ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_examModeFeature.uid() )
	{
		return {};
	}
	QMutexLocker locker( &m_remoteStatusMutex );
	return m_remoteStatuses.value( computerControlInterface.data(), QVariantMap{
		{ QStringLiteral("status"), QStringLiteral("UNKNOWN") },
	} );
}



void ExamModeFeaturePlugin::sendAsyncFeatureMessages( VeyonServerInterface& server,
												   const MessageContext& messageContext )
{
	if( isEndpointComponent() )
	{
		server.sendFeatureMessageReply( messageContext, statusMessage() );
	}
}



bool ExamModeFeaturePlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)
	return featureUid == m_examModeFeature.uid() && m_active;
}



ExamModeSession::Envelope ExamModeFeaturePlugin::envelopeFromMessage( const FeatureMessage& message ) const
{
	ExamModeSession::Envelope envelope;
	envelope.sessionId = message.argument( Argument::SessionId ).toString().trimmed();
	envelope.sequence = message.argument( Argument::Sequence ).toULongLong();
	envelope.issuedAtMs = message.argument( Argument::IssuedAt ).toLongLong();
	envelope.expiresAtMs = message.argument( Argument::ExpiresAt ).toLongLong();
	envelope.highSecurity = message.argument( Argument::HighSecurity ).toBool();
	envelope.signingKeyId = message.argument( Argument::SigningKeyId ).toString().trimmed();
	envelope.requiredCapabilities = message.argument( Argument::RequiredCapabilities ).toStringList();
	envelope.externalCapabilities = message.argument( Argument::ExternalCapabilities ).toMap();
	const auto encodedSignature = message.argument( Argument::ProfileSignature ).toByteArray().trimmed();
	envelope.signature = QByteArray::fromBase64( encodedSignature );
	return envelope;
}



bool ExamModeFeaturePlugin::verifyEnvelopeSignature( const ExamModeSession::Envelope& envelope,
													 const QString& operation, const QString& digest ) const
{
	if( envelope.signature.isEmpty() )
	{
		return envelope.highSecurity == false;
	}
	static const QRegularExpression KeyIdPattern( QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$") );
	if( KeyIdPattern.match( envelope.signingKeyId ).hasMatch() == false )
	{
		return false;
	}
	const auto keyPath = VeyonCore::filesystem().publicKeyPath( envelope.signingKeyId );
	CryptoCore::PublicKey publicKey( keyPath );
	if( publicKey.isNull() || publicKey.isPublic() == false || envelope.signature.size() > 8192 )
	{
		vWarning() << "ExamMode: impossible de charger la clé de signature" << keyPath;
		return false;
	}
	return publicKey.verifyMessage( ExamModeSession::canonicalPayload( envelope, operation, digest ),
		envelope.signature, CryptoCore::DefaultSignatureAlgorithm );
}



QVariantMap ExamModeFeaturePlugin::localCapabilities( const ExamModeProfile::NetworkPolicy& networkPolicy ) const
{
	QVariantMap capabilities{
		{ QStringLiteral("process.terminate"), QStringLiteral("SUPPORTED") },
		{ QStringLiteral("screenLock"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
		{ QStringLiteral("clipboard.block"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
		{ QStringLiteral("printing.block"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
		{ QStringLiteral("usb.block"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
		{ QStringLiteral("remoteDesktop.block"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
		{ QStringLiteral("applicationControl"), QStringLiteral("EXTERNAL_POLICY_REQUIRED") },
	};
#if defined(Q_OS_WIN)
	capabilities.insert( QStringLiteral("process.preventLaunch"), QStringLiteral("BASENAME_ONLY") );
	capabilities.insert( QStringLiteral("process.preventLaunch.basename"), QStringLiteral("SUPPORTED") );
	capabilities.insert( QStringLiteral("network.domainFilter"), QStringLiteral("BROWSER_POLICY") );
	capabilities.insert( QStringLiteral("network.firewall"), QStringLiteral("UNSUPPORTED_WFP_REQUIRED") );
#elif defined(Q_OS_LINUX)
	capabilities.insert( QStringLiteral("process.preventLaunch"), QStringLiteral("BASENAME_ONLY") );
	capabilities.insert( QStringLiteral("process.preventLaunch.basename"), QStringLiteral("SUPPORTED") );
	capabilities.insert( QStringLiteral("network.domainFilter"), QStringLiteral("HOSTS_BLOCKLIST_ONLY") );
	const bool firewallTools = QStandardPaths::findExecutable( QStringLiteral("nft") ).isEmpty() == false &&
		QStandardPaths::findExecutable( QStringLiteral("systemd-run") ).isEmpty() == false &&
		QStandardPaths::findExecutable( QStringLiteral("systemctl") ).isEmpty() == false;
	capabilities.insert( QStringLiteral("network.firewall"),
		firewallTools ? QStringLiteral("SUPPORTED") : QStringLiteral("UNSUPPORTED_DEPENDENCY") );
#else
	capabilities.insert( QStringLiteral("process.preventLaunch"), QStringLiteral("UNSUPPORTED") );
	capabilities.insert( QStringLiteral("network.domainFilter"), QStringLiteral("HOSTS_BLOCKLIST_ONLY") );
	capabilities.insert( QStringLiteral("network.firewall"), QStringLiteral("UNSUPPORTED") );
#endif
	capabilities.insert( QStringLiteral("network.requestedBackend"),
		networkPolicy.backend == ExamModeProfile::NetworkBackend::Firewall ?
			QStringLiteral("firewall") : QStringLiteral("hosts") );
	return capabilities;
}



bool ExamModeFeaturePlugin::requiredCapabilitiesAvailable( const ExamModeSession::Envelope& envelope,
														const QVariantMap& capabilities, QString* missing ) const
{
	auto effective = capabilities;
	for( auto it = envelope.externalCapabilities.constBegin(); it != envelope.externalCapabilities.constEnd(); ++it )
	{
		if( it.value().toString().compare( QStringLiteral("ACTIVE"), Qt::CaseInsensitive ) == 0 )
		{
			effective.insert( it.key(), QStringLiteral("EXTERNAL_ATTESTED") );
		}
	}
	for( const auto& required : envelope.requiredCapabilities )
	{
		const auto value = effective.value( required ).toString();
		if( value != QStringLiteral("SUPPORTED") && value != QStringLiteral("EXTERNAL_ATTESTED") &&
			value != QStringLiteral("ACTIVE") )
		{
			if( missing ) { *missing = required; }
			return false;
		}
	}
	return true;
}



void ExamModeFeaturePlugin::setStatus( const QString& status, const QString& errorCode,
									  const QString& errorMessage, const QVariantMap& backendResults )
{
	m_status = status;
	m_errorCode = errorCode;
	m_errorMessage = errorMessage;
	m_backendResults = backendResults;
	m_statusTimestampMs = QDateTime::currentMSecsSinceEpoch();
}



FeatureMessage ExamModeFeaturePlugin::statusMessage() const
{
	FeatureMessage message{ m_examModeFeature.uid(), FeatureCommand::ExamStatus };
	message.addArgument( Argument::Status, m_status )
		.addArgument( Argument::ErrorCode, m_errorCode )
		.addArgument( Argument::ErrorMessage, m_errorMessage )
		.addArgument( Argument::EffectiveDigest, m_profileDigest )
		.addArgument( Argument::Capabilities, m_capabilities )
		.addArgument( Argument::BackendResults, m_backendResults )
		.addArgument( Argument::SessionId, m_statusSessionId )
		.addArgument( Argument::Sequence, m_statusSequence )
		.addArgument( Argument::Timestamp, m_statusTimestampMs );
	return FeatureMessage{ message };
}



bool ExamModeFeaturePlugin::startEnforcement( const ExamModeProfile::ProcessPolicy& processPolicy,
											  const QStringList& sites, const QList<ExamModeProfile::UrlRule>& urlRules,
											  const QString& sitesMode, const QString& defaultUrlAction, int leaseSeconds,
											  const QString& profileId, qint64 profileRevision, const QString& expectedDigest,
											  const ExamModeProfile::NetworkPolicy& networkPolicy,
											  const ExamModeSession::Envelope& envelope )
{
	ExamModeProfile::ProcessPolicy normalizedProcessPolicy{
		ExamModeProfile::normalizeApplications( processPolicy.terminateApplications ),
		ExamModeProfile::normalizeApplications( processPolicy.preventLaunchApplications ),
	};
	QStringList rejectedSites;
	const auto normalizedSites = ExamModeProfile::normalizeDomains( sites, &rejectedSites );
	bool validMode = false;
	const auto normalizedSitesMode = ExamModeProfile::siteModeName(
		ExamModeProfile::siteModeFromString( sitesMode, &validMode ) );
	const auto normalizedLeaseSeconds = qBound( MinimumLeaseSeconds,
		leaseSeconds > 0 ? leaseSeconds : DefaultLeaseSeconds, MaximumLeaseSeconds );
	auto effectiveUrlRules = urlRules;
	const auto legacyUrlAction = normalizedSitesMode == QStringLiteral("allow") ?
		ExamModeProfile::RuleAction::Allow : ExamModeProfile::RuleAction::Block;
	for( const auto& site : normalizedSites )
	{
		effectiveUrlRules.append( { legacyUrlAction, site, false } );
	}
	auto effectiveDefaultAction = normalizedSitesMode == QStringLiteral("allow") ?
		ExamModeProfile::RuleAction::Block : ExamModeProfile::RuleAction::Allow;
	const auto defaultActionName = defaultUrlAction.trimmed().toLower();
	if( defaultActionName == QStringLiteral("allow") )
	{
		effectiveDefaultAction = ExamModeProfile::RuleAction::Allow;
	}
	else if( defaultActionName == QStringLiteral("block") )
	{
		effectiveDefaultAction = ExamModeProfile::RuleAction::Block;
	}
	const auto normalizedProfileId = profileId.trimmed().isEmpty() ? QStringLiteral("legacy") : profileId.trimmed();
	const auto digest = ExamModeProfile::profileDigest( normalizedProfileId, profileRevision,
		normalizedProcessPolicy, effectiveUrlRules, effectiveDefaultAction, normalizedLeaseSeconds,
		networkPolicy );
	m_statusSessionId = envelope.sessionId;
	m_statusSequence = envelope.sequence;
	m_capabilities = localCapabilities( networkPolicy );
	setStatus( QStringLiteral("PENDING") );
	if( validMode == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_SITE_MODE"),
			QStringLiteral("Unknown site filtering mode") );
		return false;
	}
	if( rejectedSites.isEmpty() == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_DOMAIN"),
			QStringLiteral("One or more domains are invalid") );
		return false;
	}
	if( normalizedProfileId.size() > 128 || normalizedProfileId.contains( QLatin1Char('\r') ) ||
		normalizedProfileId.contains( QLatin1Char('\n') ) || profileRevision < 0 )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_PROFILE_ID"),
			QStringLiteral("Invalid profile identity or revision") );
		return false;
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto envelopeValidation = ExamModeSession::validate( envelope, now, true );
	if( envelopeValidation.accepted == false )
	{
		setStatus( QStringLiteral("REJECTED"), envelopeValidation.code, envelopeValidation.message );
		return false;
	}
	const bool legacyEnvelope = envelopeValidation.code == QStringLiteral("LEGACY");
	if( legacyEnvelope == false )
	{
		if( m_active && m_sessionId.isEmpty() == false && envelope.sessionId != m_sessionId &&
			m_sessionExpiresAtMs > now )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("SESSION_CONFLICT"),
				QStringLiteral("Another non-expired exam session is active") );
			return false;
		}
		if( ExamModeSession::isNewer( envelope, m_sessionId, m_lastSequence ) == false )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("REPLAYED_SEQUENCE"),
				QStringLiteral("Sequence was already processed") );
			return false;
		}
	}
	if( verifyEnvelopeSignature( envelope, QStringLiteral("start"), digest ) == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_SIGNATURE"),
			QStringLiteral("Profile signature verification failed") );
		return false;
	}
	QString missingCapability;
	if( requiredCapabilitiesAvailable( envelope, m_capabilities, &missingCapability ) == false )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("MISSING_CAPABILITY"),
			QStringLiteral("Required capability is unavailable: %1").arg( missingCapability ) );
		return false;
	}
	if( networkPolicy.backend == ExamModeProfile::NetworkBackend::Firewall &&
		m_capabilities.value( QStringLiteral("network.firewall") ).toString() != QStringLiteral("SUPPORTED") )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("UNSUPPORTED_NETWORK_BACKEND"),
			QStringLiteral("Firewall enforcement is unavailable on this endpoint") );
		return false;
	}
	if( envelope.highSecurity && networkPolicy.backend != ExamModeProfile::NetworkBackend::Firewall &&
		( effectiveUrlRules.isEmpty() == false || effectiveDefaultAction == ExamModeProfile::RuleAction::Block ) )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("HIGH_ASSURANCE_NETWORK_REQUIRED"),
			QStringLiteral("High-security URL policies require the firewall backend") );
		return false;
	}
	if( envelope.highSecurity && normalizedProcessPolicy.preventLaunchApplications.isEmpty() == false &&
		envelope.externalCapabilities.value( QStringLiteral("applicationControl") ).toString().compare(
			QStringLiteral("ACTIVE"), Qt::CaseInsensitive ) != 0 )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("STRONG_APPLICATION_CONTROL_REQUIRED"),
			QStringLiteral("High-security process policies require broker-attested WDAC/AppLocker/IMA control") );
		return false;
	}
	if( normalizedProfileId == m_profileId && profileRevision < m_profileRevision )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("STALE_PROFILE_REVISION"),
			QStringLiteral("Profile revision is older than the applied revision") );
		return false;
	}
	const bool managedRevision = normalizedProfileId != QStringLiteral("legacy") ||
		profileRevision > 0 || expectedDigest.trimmed().isEmpty() == false;
	if( managedRevision && normalizedProfileId == m_profileId && profileRevision == m_profileRevision &&
		m_profileDigest.isEmpty() == false && digest != m_profileDigest )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("REVISION_COLLISION"),
			QStringLiteral("Profile content changed without a revision change") );
		return false;
	}
	if( expectedDigest.trimmed().isEmpty() == false &&
		digest.compare( expectedDigest.trimmed(), Qt::CaseInsensitive ) != 0 )
	{
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("DIGEST_MISMATCH"),
			QStringLiteral("Expected and effective profile digests differ") );
		return false;
	}

	struct Snapshot
	{
		bool active;
		QStringList blockedApps;
		QStringList launchApps;
		QStringList sites;
		QList<ExamModeProfile::UrlRule> urlRules;
		QString sitesMode;
		ExamModeProfile::RuleAction defaultAction;
		bool structuredRules;
		ExamModeProfile::NetworkPolicy network;
		QString profileId;
		qint64 revision;
		QString digest;
		int lease;
		QString hostsSignature;
		QString appsSignature;
		bool siteFilteringActive;
		QString sessionId;
		quint64 sequence;
		qint64 expiresAt;
		bool highSecurity;
	};
	const Snapshot previous{
		m_active, m_blockedApps, m_launchPreventedApps, m_sites, m_urlRules, m_sitesMode,
		m_defaultUrlAction, m_hasStructuredUrlRules,
		{ m_networkBackend, m_allowedNetworks, m_dnsServers, m_supervisionNetworks },
		m_profileId, m_profileRevision, m_profileDigest, m_leaseSeconds, m_hostsSignature,
		m_appsSignature, m_siteFilteringActive, m_sessionId, m_lastSequence,
		m_sessionExpiresAtMs, m_highSecurity,
	};

	m_blockedApps = normalizedProcessPolicy.terminateApplications;
	m_launchPreventedApps = normalizedProcessPolicy.preventLaunchApplications;
	m_sites = normalizedSites;
	m_urlRules = effectiveUrlRules;
	m_sitesMode = normalizedSitesMode;
	m_defaultUrlAction = effectiveDefaultAction;
	m_hasStructuredUrlRules = urlRules.isEmpty() == false;
	m_networkBackend = networkPolicy.backend;
	m_allowedNetworks = networkPolicy.allowedNetworks;
	m_dnsServers = networkPolicy.dnsServers;
	m_supervisionNetworks = networkPolicy.supervisionNetworks;
	m_leaseSeconds = normalizedLeaseSeconds;
	m_profileId = normalizedProfileId;
	m_profileRevision = profileRevision;
	m_profileDigest = digest;
	m_sessionId = legacyEnvelope ? m_sessionId : envelope.sessionId;
	m_lastSequence = legacyEnvelope ? m_lastSequence : envelope.sequence;
	m_sessionExpiresAtMs = legacyEnvelope ? now + normalizedLeaseSeconds * 1000LL : envelope.expiresAtMs;
	m_highSecurity = envelope.highSecurity;
	vInfo() << "ExamMode: profil" << m_profileId << "revision" << m_profileRevision
		<< "empreinte" << m_profileDigest.left( 12 );

	// Le portail ré-applique le profil chaque minute (couverture des postes
	// connectés après coup) : on ne réapplique le filtrage réseau QUE si la
	// politique réseau a changé (règles URL — qui incluent les sites hérités —,
	// action par défaut, mode), pour éviter réécriture hosts/flush DNS et
	// aller-retour registre inutiles quand seules les applis changent.
	QStringList signatureParts{ m_sitesMode,
		m_networkBackend == ExamModeProfile::NetworkBackend::Firewall ? QStringLiteral("firewall") : QStringLiteral("hosts"),
		m_defaultUrlAction == ExamModeProfile::RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") };
	for( const auto& rule : m_urlRules )
	{
		signatureParts.append( QStringLiteral("%1|%2|%3").arg(
			rule.action == ExamModeProfile::RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block"),
			rule.regularExpression ? QStringLiteral("regex") : QStringLiteral("glob"),
			rule.expression ) );
	}
	signatureParts.append( QStringLiteral("nets:") + m_allowedNetworks.join( QLatin1Char(',') ) );
	signatureParts.append( QStringLiteral("dns:") + m_dnsServers.join( QLatin1Char(',') ) );
	signatureParts.append( QStringLiteral("sup:") + m_supervisionNetworks.join( QLatin1Char(',') ) );
	const auto signature = signatureParts.join( QLatin1Char('\n') );
	bool networkTouched = false;
	bool launchTouched = false;
	const bool networkRequired = m_networkBackend == ExamModeProfile::NetworkBackend::Firewall ||
		m_urlRules.isEmpty() == false || m_defaultUrlAction == ExamModeProfile::RuleAction::Block;
	if( signature != previous.hostsSignature || previous.active == false )
	{
		networkTouched = true;
		vInfo() << "ExamMode: enforcement -" << m_blockedApps.size() << "app(s),"
				<< m_sites.size() << "site(s), mode" << m_sitesMode;
		m_siteFilteringActive = applySiteFiltering( m_sites, m_sitesMode );
		m_hostsSignature = m_siteFilteringActive ? signature : QString{};
		if( m_siteFilteringActive == false && networkRequired )
		{
			vWarning() << "ExamMode: filtrage réseau obligatoire non appliqué";
		}
	}

	// Empêche le lancement des logiciels interdits (IFEO Windows) si la liste change.
	const auto appsSig = m_blockedApps.join( QLatin1Char('\n') ) + QStringLiteral("\n--prevent--\n") +
		m_launchPreventedApps.join( QLatin1Char('\n') );
	bool launchApplied = true;
	if( appsSig != previous.appsSignature || previous.active == false )
	{
		launchTouched = true;
		launchApplied = applyLaunchPrevention( m_launchPreventedApps );
		if( launchApplied == false )
		{
			m_appsSignature.clear();
		}
		else
		{
			m_appsSignature = appsSig;
		}
	}

	bool deadManArmed = true;
	if( m_networkBackend == ExamModeProfile::NetworkBackend::Firewall && m_siteFilteringActive )
	{
#if defined(Q_OS_LINUX)
		deadManArmed = armFirewallDeadMan( m_leaseSeconds + 60 );
#endif
	}

	if( ( networkRequired && m_siteFilteringActive == false ) || launchApplied == false || deadManArmed == false )
	{
		const auto failureCode = deadManArmed == false ? QStringLiteral("DEADMAN_UNAVAILABLE") :
			launchApplied == false ? QStringLiteral("LAUNCH_PREVENTION_FAILED") :
			QStringLiteral("NETWORK_ENFORCEMENT_FAILED");
		if( launchTouched ) { removeLaunchPrevention(); }
		if( networkTouched ) { removeSiteFiltering(); }

		m_blockedApps = previous.blockedApps;
		m_launchPreventedApps = previous.launchApps;
		m_sites = previous.sites;
		m_urlRules = previous.urlRules;
		m_sitesMode = previous.sitesMode;
		m_defaultUrlAction = previous.defaultAction;
		m_hasStructuredUrlRules = previous.structuredRules;
		m_networkBackend = previous.network.backend;
		m_allowedNetworks = previous.network.allowedNetworks;
		m_dnsServers = previous.network.dnsServers;
		m_supervisionNetworks = previous.network.supervisionNetworks;
		m_profileId = previous.profileId;
		m_profileRevision = previous.revision;
		m_profileDigest = previous.digest;
		m_leaseSeconds = previous.lease;
		m_sessionId = previous.sessionId;
		m_lastSequence = previous.sequence;
		m_sessionExpiresAtMs = previous.expiresAt;
		m_highSecurity = previous.highSecurity;
		m_active = previous.active;
		m_hostsSignature = previous.hostsSignature;
		m_appsSignature = previous.appsSignature;
		m_siteFilteringActive = previous.siteFilteringActive;

		bool restored = true;
		if( previous.active && networkTouched )
		{
			m_siteFilteringActive = applySiteFiltering( m_sites, m_sitesMode );
			restored = m_siteFilteringActive || ( m_urlRules.isEmpty() &&
				m_defaultUrlAction == ExamModeProfile::RuleAction::Allow );
		}
		if( previous.active && launchTouched )
		{
			restored = applyLaunchPrevention( m_launchPreventedApps ) && restored;
		}
		if( restored == false )
		{
			m_active = false;
			setStatus( QStringLiteral("DEGRADED"), QStringLiteral("ROLLBACK_FAILED"),
				QStringLiteral("The candidate failed and the previous enforcement could not be fully restored") );
		}
		else
		{
			setStatus( QStringLiteral("REJECTED"), failureCode,
				QStringLiteral("A mandatory enforcement backend failed; the previous state was restored") );
		}
		return false;
	}

	m_active = true;

	if( m_timer == nullptr )
	{
		m_timer = new QTimer( this );
		connect( m_timer, &QTimer::timeout, this, &ExamModeFeaturePlugin::enforceTick );
	}
	if( m_timer->isActive() == false )
	{
		m_timer->start( EnforceIntervalMs );
	}

	// Fail-safe : (re)arme le watchdog. Sans nouveau startExam avant l'échéance
	// (portail arrêté, supervision coupée, poste hors-ligne au stop…), on lève tout.
	if( m_watchdog == nullptr )
	{
		m_watchdog = new QTimer( this );
		m_watchdog->setSingleShot( true );
		connect( m_watchdog, &QTimer::timeout, this, [this]() {
			vWarning() << "ExamMode: watchdog — plus de re-push du portail, levée automatique des restrictions";
			setStatus( QStringLiteral("EXPIRED"), QStringLiteral("LEASE_EXPIRED"),
				QStringLiteral("The exam lease was not renewed") );
			stopEnforcement();
		} );
	}
	m_watchdog->start( m_leaseSeconds * 1000 );

	if( m_driftTimer == nullptr )
	{
		m_driftTimer = new QTimer( this );
		connect( m_driftTimer, &QTimer::timeout, this, [this]() {
			if( m_active && verifyEnforcement() == false )
			{
				setStatus( QStringLiteral("DEGRADED"), QStringLiteral("ENFORCEMENT_DRIFT"),
					QStringLiteral("A mandatory backend drifted from the applied policy") );
			}
		} );
	}
	m_driftTimer->start( 10000 );

	enforceTick();		// passe immédiate sur les processus interdits
	QVariantMap results{
		{ QStringLiteral("network"), networkRequired ? QStringLiteral("APPLIED") : QStringLiteral("NOT_REQUESTED") },
		{ QStringLiteral("launchPrevention"), m_launchPreventedApps.isEmpty() ?
			QStringLiteral("NOT_REQUESTED") : QStringLiteral("APPLIED") },
		{ QStringLiteral("deadMan"), m_networkBackend == ExamModeProfile::NetworkBackend::Firewall ?
			QStringLiteral("ARMED") : QStringLiteral("NOT_REQUIRED") },
	};
	for( auto it = envelope.externalCapabilities.constBegin(); it != envelope.externalCapabilities.constEnd(); ++it )
	{
		m_capabilities.insert( it.key(), it.value() );
	}
	if( m_status != QStringLiteral("DEGRADED") )
	{
		setStatus( QStringLiteral("APPLIED"), {}, {}, results );
	}
	persistReplayState();
	persistActiveState();
	return true;
}



void ExamModeFeaturePlugin::stopEnforcement()
{
	m_active = false;
	m_hostsSignature.clear();
	m_appsSignature.clear();
	if( m_timer )
	{
		m_timer->stop();
	}
	if( m_watchdog )
	{
		m_watchdog->stop();
	}
	if( m_driftTimer )
	{
		m_driftTimer->stop();
	}
	removeLaunchPrevention();
	removeSiteFiltering();
	m_blockedApps.clear();
	m_launchPreventedApps.clear();
	m_sites.clear();
	m_urlRules.clear();
	m_hasStructuredUrlRules = false;
	m_defaultUrlAction = ExamModeProfile::RuleAction::Allow;
	m_networkBackend = ExamModeProfile::NetworkBackend::Hosts;
	m_allowedNetworks.clear();
	m_dnsServers.clear();
	m_supervisionNetworks.clear();
	m_siteFilteringActive = false;
	QFile::remove( activeStateFile() );
	vInfo() << "ExamMode: enforcement stopped";
}



bool ExamModeFeaturePlugin::stopEnforcement( const ExamModeSession::Envelope& envelope )
{
	m_statusSessionId = envelope.sessionId;
	m_statusSequence = envelope.sequence;
	const auto validation = ExamModeSession::validate( envelope, QDateTime::currentMSecsSinceEpoch(),
		m_highSecurity == false );
	if( validation.accepted == false )
	{
		setStatus( QStringLiteral("REJECTED"), validation.code, validation.message );
		return false;
	}
	const bool legacy = validation.code == QStringLiteral("LEGACY");
	if( legacy == false )
	{
		if( m_sessionId.isEmpty() || envelope.sessionId != m_sessionId )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("SESSION_MISMATCH"),
				QStringLiteral("Stop command does not target the active session") );
			return false;
		}
		if( envelope.sequence <= m_lastSequence )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("REPLAYED_SEQUENCE"),
				QStringLiteral("Sequence was already processed") );
			return false;
		}
		if( verifyEnvelopeSignature( envelope, QStringLiteral("stop"), m_profileDigest ) == false )
		{
			setStatus( QStringLiteral("REJECTED"), QStringLiteral("INVALID_SIGNATURE"),
				QStringLiteral("Stop signature verification failed") );
			return false;
		}
		m_lastSequence = envelope.sequence;
		m_sessionExpiresAtMs = envelope.expiresAtMs;
	}
	stopEnforcement();
	setStatus( QStringLiteral("STOPPED") );
	persistReplayState();
	return true;
}



bool ExamModeFeaturePlugin::verifyEnforcement()
{
#if defined(Q_OS_WIN)
	const auto registryStateHealthy = []( const QString& path ) {
		const auto entries = readJsonFile( path ).value( QStringLiteral("entries") ).toArray();
		if( entries.isEmpty() ) { return false; }
		for( const auto& value : entries )
		{
			const auto entry = value.toObject();
			const auto expected = entry.value( QStringLiteral("expected") ).toObject();
			const auto current = ExamModeWindowsNative::readRegistryValue(
				entry.value( QStringLiteral("key") ).toString(), entry.value( QStringLiteral("name") ).toString() );
			if( current.value( QStringLiteral("ok") ).toBool() == false ||
				current.value( QStringLiteral("exists") ).toBool() != expected.value( QStringLiteral("exists") ).toBool() ||
				current.value( QStringLiteral("type") ).toString().compare(
					expected.value( QStringLiteral("type") ).toString(), Qt::CaseInsensitive ) != 0 ||
				current.value( QStringLiteral("data") ).toString() != expected.value( QStringLiteral("data") ).toString() )
			{
				return false;
			}
		}
		return true;
	};
#endif

	bool networkHealthy = true;
	if( m_siteFilteringActive && ( m_networkBackend == ExamModeProfile::NetworkBackend::Firewall ||
		m_urlRules.isEmpty() == false || m_defaultUrlAction == ExamModeProfile::RuleAction::Block ) )
	{
#if defined(Q_OS_WIN)
		networkHealthy = registryStateHealthy( siteFilterStateFile() ) && QFile::exists( pacFilePath() );
#elif defined(Q_OS_LINUX)
		if( m_networkBackend == ExamModeProfile::NetworkBackend::Firewall )
		{
			QProcess nft;
			nft.start( QStringLiteral("nft"), { QStringLiteral("list"), QStringLiteral("table"),
				QStringLiteral("inet"), QStringLiteral("veyon_exammode") } );
			networkHealthy = nft.waitForStarted( 2000 ) && nft.waitForFinished( 3000 ) &&
				nft.exitStatus() == QProcess::NormalExit && nft.exitCode() == 0;
			QProcess timer;
			timer.start( QStringLiteral("systemctl"), { QStringLiteral("is-active"),
				QStringLiteral("veyon-exammode-failsafe.timer") } );
			networkHealthy = timer.waitForStarted( 2000 ) && timer.waitForFinished( 3000 ) &&
				timer.exitStatus() == QProcess::NormalExit && timer.exitCode() == 0 && networkHealthy;
		}
		else
#endif
		{
			QFile hosts( hostsFilePath() );
			networkHealthy = hosts.open( QIODevice::ReadOnly | QIODevice::Text ) &&
				hosts.readAll().contains( HostsMarkerBegin );
		}
	}

	bool launchHealthy = true;
#if defined(Q_OS_WIN)
	launchHealthy = m_launchPreventedApps.isEmpty() || registryStateHealthy( launchPreventionStateFile() );
#elif defined(Q_OS_LINUX)
	launchHealthy = m_launchPreventedApps.isEmpty() ||
		( m_execGuard && m_execGuard->isActive() && m_execGuard->isHealthy() && m_execGuard->refreshMounts() );
#endif
	if( networkHealthy && launchHealthy )
	{
		return true;
	}

	bool repaired = true;
	if( networkHealthy == false )
	{
#if defined(Q_OS_WIN)
		// Une dérive registre peut être une nouvelle GPO. Ne jamais l'écraser : la
		// restauration ownership-aware doit rester conservatrice.
		repaired = false;
#else
		m_hostsSignature.clear();
		m_siteFilteringActive = applySiteFiltering( m_sites, m_sitesMode );
		repaired = m_siteFilteringActive;
#endif
	}
	if( launchHealthy == false )
	{
#if defined(Q_OS_WIN)
		repaired = false;
#else
		m_appsSignature.clear();
		repaired = applyLaunchPrevention( m_launchPreventedApps ) && repaired;
#endif
	}
	if( repaired )
	{
		setStatus( QStringLiteral("APPLIED"), {}, {},
			{ { QStringLiteral("drift"), QStringLiteral("REPAIRED") } } );
	}
	return repaired;
}



QString ExamModeFeaturePlugin::replayStateFile()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/exammode-replay-state.json");
#else
	return QStringLiteral("/var/lib/veyon/exammode-replay-state.json");
#endif
}



void ExamModeFeaturePlugin::loadReplayState()
{
	const auto state = readJsonFile( replayStateFile() );
	const auto expiresAt = state.value( QStringLiteral("expiresAtMs") ).toVariant().toLongLong();
	if( state.value( QStringLiteral("version") ).toInt() != 1 ||
		expiresAt <= QDateTime::currentMSecsSinceEpoch() - ExamModeSession::MaximumClockSkewMs )
	{
		QFile::remove( replayStateFile() );
		return;
	}
	m_sessionId = state.value( QStringLiteral("sessionId") ).toString();
	m_lastSequence = state.value( QStringLiteral("lastSequence") ).toString().toULongLong();
	m_sessionExpiresAtMs = expiresAt;
	m_highSecurity = state.value( QStringLiteral("highSecurity") ).toBool();
}



void ExamModeFeaturePlugin::persistReplayState() const
{
	if( m_sessionId.isEmpty() )
	{
		return;
	}
	if( writeJsonFile( replayStateFile(), {
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("sessionId"), m_sessionId },
		{ QStringLiteral("lastSequence"), QString::number( m_lastSequence ) },
		{ QStringLiteral("expiresAtMs"), QString::number( m_sessionExpiresAtMs ) },
		{ QStringLiteral("highSecurity"), m_highSecurity },
	} ) == false )
	{
		vWarning() << "ExamMode: impossible de persister l'état anti-rejeu";
	}
}



QString ExamModeFeaturePlugin::activeStateFile()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/exammode-active-state.json");
#else
	return QStringLiteral("/var/lib/veyon/exammode-active-state.json");
#endif
}



void ExamModeFeaturePlugin::persistActiveState() const
{
	if( m_active == false )
	{
		return;
	}
	QJsonArray urlRules;
	const auto originalRuleCount = m_hasStructuredUrlRules ? qMax( 0, m_urlRules.size() - m_sites.size() ) : 0;
	for( int index = 0; index < originalRuleCount; ++index )
	{
		const auto& rule = m_urlRules.at( index );
		urlRules.append( QJsonObject{
			{ QStringLiteral("action"), rule.action == ExamModeProfile::RuleAction::Allow ?
				QStringLiteral("allow") : QStringLiteral("block") },
			{ QStringLiteral("expression"), rule.expression },
			{ QStringLiteral("regularExpression"), rule.regularExpression },
		} );
	}
	const QJsonObject state{
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("terminateApplications"), QJsonArray::fromStringList( m_blockedApps ) },
		{ QStringLiteral("preventLaunchApplications"), QJsonArray::fromStringList( m_launchPreventedApps ) },
		{ QStringLiteral("sites"), QJsonArray::fromStringList( m_sites ) },
		{ QStringLiteral("sitesMode"), m_sitesMode },
		{ QStringLiteral("urlRules"), urlRules },
		{ QStringLiteral("urlDefaultAction"), m_defaultUrlAction == ExamModeProfile::RuleAction::Allow ?
			QStringLiteral("allow") : QStringLiteral("block") },
		{ QStringLiteral("networkBackend"), m_networkBackend == ExamModeProfile::NetworkBackend::Firewall ?
			QStringLiteral("firewall") : QStringLiteral("hosts") },
		{ QStringLiteral("allowedNetworks"), QJsonArray::fromStringList( m_allowedNetworks ) },
		{ QStringLiteral("dnsServers"), QJsonArray::fromStringList( m_dnsServers ) },
		{ QStringLiteral("supervisionNetworks"), QJsonArray::fromStringList( m_supervisionNetworks ) },
		{ QStringLiteral("profileId"), m_profileId },
		{ QStringLiteral("profileRevision"), QString::number( m_profileRevision ) },
		{ QStringLiteral("profileDigest"), m_profileDigest },
		{ QStringLiteral("leaseSeconds"), m_leaseSeconds },
		{ QStringLiteral("sessionId"), m_sessionId },
		{ QStringLiteral("lastSequence"), QString::number( m_lastSequence ) },
		{ QStringLiteral("expiresAtMs"), QString::number( m_sessionExpiresAtMs ) },
		{ QStringLiteral("highSecurity"), m_highSecurity },
	};
	if( writeJsonFile( activeStateFile(), state ) == false )
	{
		vWarning() << "ExamMode: impossible de persister l'état d'enforcement actif";
	}
}



void ExamModeFeaturePlugin::recoverActiveState()
{
	const auto state = readJsonFile( activeStateFile() );
	const auto expiresAt = state.value( QStringLiteral("expiresAtMs") ).toString().toLongLong();
	if( state.value( QStringLiteral("version") ).toInt() != 1 ||
		expiresAt <= QDateTime::currentMSecsSinceEpoch() )
	{
		stopEnforcement();
		setStatus( QStringLiteral("EXPIRED"), QStringLiteral("RECOVERY_STATE_EXPIRED"),
			QStringLiteral("Persisted exam state is no longer valid") );
		return;
	}

	ExamModeProfile::ProcessPolicy processPolicy;
	for( const auto& value : state.value( QStringLiteral("terminateApplications") ).toArray() )
	{
		processPolicy.terminateApplications.append( value.toString() );
	}
	for( const auto& value : state.value( QStringLiteral("preventLaunchApplications") ).toArray() )
	{
		processPolicy.preventLaunchApplications.append( value.toString() );
	}
	QList<ExamModeProfile::UrlRule> urlRules;
	for( const auto& value : state.value( QStringLiteral("urlRules") ).toArray() )
	{
		const auto rule = value.toObject();
		urlRules.append( {
			rule.value( QStringLiteral("action") ).toString() == QStringLiteral("allow") ?
				ExamModeProfile::RuleAction::Allow : ExamModeProfile::RuleAction::Block,
			rule.value( QStringLiteral("expression") ).toString(),
			rule.value( QStringLiteral("regularExpression") ).toBool(),
		} );
	}
	auto stringList = [&state]( const QString& key ) {
		QStringList values;
		for( const auto& value : state.value( key ).toArray() ) { values.append( value.toString() ); }
		return values;
	};
	bool validBackend = false;
	ExamModeProfile::NetworkPolicy networkPolicy{
		ExamModeProfile::networkBackendFromString( state.value( QStringLiteral("networkBackend") ).toString(), &validBackend ),
		stringList( QStringLiteral("allowedNetworks") ), stringList( QStringLiteral("dnsServers") ),
		stringList( QStringLiteral("supervisionNetworks") ),
	};
	if( validBackend == false )
	{
		stopEnforcement();
		setStatus( QStringLiteral("REJECTED"), QStringLiteral("CORRUPT_RECOVERY_STATE"),
			QStringLiteral("Persisted network backend is invalid") );
		return;
	}

	m_sessionId = state.value( QStringLiteral("sessionId") ).toString();
	m_lastSequence = state.value( QStringLiteral("lastSequence") ).toString().toULongLong();
	m_sessionExpiresAtMs = expiresAt;
	const bool highSecurity = state.value( QStringLiteral("highSecurity") ).toBool();
	const bool recovered = startEnforcement( processPolicy, stringList( QStringLiteral("sites") ), urlRules,
		state.value( QStringLiteral("sitesMode") ).toString(),
		state.value( QStringLiteral("urlDefaultAction") ).toString(),
		state.value( QStringLiteral("leaseSeconds") ).toInt(),
		state.value( QStringLiteral("profileId") ).toString(),
		state.value( QStringLiteral("profileRevision") ).toString().toLongLong(),
		state.value( QStringLiteral("profileDigest") ).toString(), networkPolicy, {} );
	if( recovered == false )
	{
		stopEnforcement();
		QFile::remove( activeStateFile() );
		return;
	}
	m_highSecurity = highSecurity;
	m_sessionExpiresAtMs = expiresAt;
	if( m_watchdog )
	{
		const auto remaining = qMax<qint64>( 1, expiresAt - QDateTime::currentMSecsSinceEpoch() );
		m_watchdog->start( static_cast<int>( qMin<qint64>( remaining, std::numeric_limits<int>::max() ) ) );
	}
	setStatus( QStringLiteral("APPLIED"), {}, {},
		{ { QStringLiteral("recovery"), QStringLiteral("RESTORED") } } );
	persistReplayState();
	persistActiveState();
}



void ExamModeFeaturePlugin::enforceTick()
{
	if( m_active == false )
	{
		return;
	}
#if defined(Q_OS_WIN)
	QStringList images;
	for( const auto& app : m_blockedApps )
	{
		const auto image = windowsImageName( app );
		if( image.isEmpty() == false ) { images.append( image ); }
	}
	const auto result = ExamModeWindowsNative::terminateProcesses( images );
	if( result.detected.isEmpty() == false )
	{
		vWarning() << "ExamMode: processus interdits détectés:" << result.detected;
	}
	if( result.failed.isEmpty() == false )
	{
		vWarning() << "ExamMode: échec de terminaison native:" << result.failed;
		setStatus( QStringLiteral("DEGRADED"), QStringLiteral("PROCESS_TERMINATION_FAILED"),
			QStringLiteral("One or more blocked processes could not be terminated"),
			{ { QStringLiteral("failedProcesses"), result.failed } } );
	}
#else
	auditRunningBlockedProcesses();		// journalise les tentatives (processus interdit trouvé actif)
	for( const auto& app : m_blockedApps )
	{
		killApplication( app );
	}
#endif
}



/**
 * Détecte (une seule invocation par passe, non bloquante au-delà d'1 s) les
 * processus interdits actuellement en cours d'exécution et les journalise comme
 * tentatives de contournement. N'agit pas — le kill est fait séparément.
 */
void ExamModeFeaturePlugin::auditRunningBlockedProcesses() const
{
	if( m_blockedApps.isEmpty() )
	{
		return;
	}

	QProcess process;
#if defined(Q_OS_WIN)
	// La passe Windows combine audit et terminaison dans enforceTick() afin de ne
	// faire qu'un seul instantané Toolhelp par intervalle.
	Q_UNUSED(process)
#else
	QStringList patterns;
	for( const auto& app : m_blockedApps )
	{
		auto proc = app.section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
		if( proc.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) )
		{
			proc.chop( 4 );
		}
		if( proc.isEmpty() == false )
		{
			patterns.append( QRegularExpression::escape( proc ) );
		}
	}
	if( patterns.isEmpty() )
	{
		return;
	}
	// pgrep -x -l : correspondance exacte du nom (comm), affiche « pid nom ».
	process.start( QStringLiteral("pgrep"), { QStringLiteral("-x"), QStringLiteral("-l"), patterns.join( QLatin1Char('|') ) } );
	if( process.waitForStarted( 1000 ) == false || process.waitForFinished( 1000 ) == false )
	{
		return;
	}
	QSet<QString> found;
	const auto lines = QString::fromLocal8Bit( process.readAllStandardOutput() ).split( QLatin1Char('\n'), Qt::SkipEmptyParts );
	for( const auto& line : lines )
	{
		const auto name = line.section( QLatin1Char(' '), 1 ).trimmed();
		if( name.isEmpty() == false )
		{
			found.insert( name );
		}
	}
	if( found.isEmpty() == false )
	{
		vWarning() << "ExamMode: tentative de contournement — processus interdit(s) actif(s):" << QStringList( found.values() );
	}
#endif
}



void ExamModeFeaturePlugin::killApplication( const QString& executable ) const
{
	const auto name = executable.trimmed();
	if( name.isEmpty() )
	{
		return;
	}

#if defined(Q_OS_WIN)
	const auto image = windowsImageName( name );
	if( image.isEmpty() )
	{
		vWarning() << "ExamMode: nom d'application Windows invalide ignoré" << name;
		return;
	}
	ExamModeWindowsNative::terminateProcesses( { image } );
#else
	// Nom d'exécutable (on tolère un chemin ou un suffixe .exe hérité d'un profil
	// Windows) : on cible le NOM de processus exact (-x) pour ne pas tuer par
	// erreur un processus dont la ligne de commande contiendrait ce mot (-f).
	auto proc = name.section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
	if( proc.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) )
	{
		proc.chop( 4 );
	}
	if( proc.isEmpty() )
	{
		return;
	}
	QProcess::startDetached( QStringLiteral("pkill"), { QStringLiteral("-x"), QStringLiteral("--"), proc } );
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



/**
 * Applique le filtrage des sites. Windows : PAC (liste noire OU blanche) + DoH
 * désactivé, robuste au contournement. Linux : fichier hosts (liste noire).
 */
bool ExamModeFeaturePlugin::applySiteFiltering( const QStringList& sites, const QString& mode )
{
#if defined(Q_OS_WIN)
	return applyWindowsSiteFiltering( sites, mode );
#else
	// Backend firewall (Linux) : allow-list egress par CIDR, robuste au
	// contournement (IP directe, DoH, DNS alternatif, VPN). Ignore hosts/PAC.
	if( m_networkBackend == ExamModeProfile::NetworkBackend::Firewall )
	{
#if defined(Q_OS_LINUX)
		// on retire toute règle hosts résiduelle avant de basculer sur le firewall
		removeHostsSection();
		flushDnsCache();
		return applyLinuxFirewall();
#else
		vWarning() << "ExamMode: le backend firewall n'est disponible que sous Linux; profil réseau refusé";
		return false;
#endif
	}
	if( m_hasStructuredUrlRules )
	{
		vWarning() << "ExamMode: les regles URL ordonnees/regex necessitent le backend PAC Windows ou firewall; profil refuse";
		removeHostsSection();
		flushDnsCache();
		return false;
	}
	if( m_defaultUrlAction == ExamModeProfile::RuleAction::Block )
	{
		// « tout bloquer par défaut » = sémantique liste blanche : irréalisable via
		// hosts — on refuse explicitement plutôt que de laisser le poste ouvert.
		vWarning() << "ExamMode: l'action URL par défaut « block » nécessite le backend PAC Windows; "
					   "le profil réseau est refusé sur cette plateforme";
		removeHostsSection();
		flushDnsCache();
		return false;
	}
	if( mode == QStringLiteral("allow") )
	{
		vWarning() << "ExamMode: la liste blanche nécessite le backend PAC Windows; "
					   "le profil réseau est refusé sur cette plateforme";
		removeHostsSection();
		flushDnsCache();
		return false;
	}
	return applyHostsBlocking( sites );
#endif
}



void ExamModeFeaturePlugin::removeSiteFiltering()
{
#if defined(Q_OS_WIN)
	removeWindowsSiteFiltering();
#else
#if defined(Q_OS_LINUX)
	removeLinuxFirewall();		// idempotent : sans effet si le firewall n'est pas actif
#endif
	revertHostsBlocking();
#endif
}



bool ExamModeFeaturePlugin::applyHostsBlocking( const QStringList& sites )
{
	// Le fichier hosts ne permet qu'une liste NOIRE (rediriger un domaine vers
	// 127.0.0.1). Le mode "allow" (liste blanche) nécessiterait un proxy/pare-feu :
	// non couvert ici, on avertit et on retire toute règle existante.
	if( m_sitesMode != QStringLiteral("block") )
	{
		vWarning() << "ExamMode: sites_mode" << m_sitesMode
				   << "non supporté via hosts (liste blanche) — sites non filtrés";
		removeHostsSection();
		flushDnsCache();
		return false;
	}

	removeHostsSection();		// repart d'un fichier propre (idempotent)

	// Aucun site à bloquer : on s'est déjà assuré qu'il ne reste pas de section.
	if( sites.isEmpty() )
	{
		flushDnsCache();
		return true;
	}

	// Limite intrinsèque du fichier hosts : pas de jokers. On ne couvre que le
	// domaine et son sous-domaine « www » ; les autres sous-domaines (m., login.,
	// cdn.…) et l'accès par IP directe restent joignables. Pour une étanchéité
	// réelle sous Linux, utiliser networkBackend=firewall (allow-list egress).
	vWarning() << "ExamMode: backend hosts — seuls le domaine et www.<domaine> sont bloqués;"
			   << "les autres sous-domaines et l'accès par IP ne le sont pas. Préférer"
			   << "networkBackend=firewall pour une allow-list egress robuste.";

	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadWrite | QIODevice::Text ) == false )
	{
		vWarning() << "ExamMode: impossible d'ouvrir le fichier hosts" << hostsFilePath();
		return false;
	}

	QByteArray content = file.readAll();
	const auto permissions = file.permissions();
	if( content.isEmpty() == false && content.endsWith( '\n' ) == false )
	{
		content.append( '\n' );
	}

	content.append( QByteArrayLiteral("\n") ).append( HostsMarkerBegin ).append( '\n' );
	for( const auto& host : sites )
	{
		// Redirige le domaine ET son sous-domaine www vers l'adresse de rebouclage.
		const auto line = QStringLiteral("127.0.0.1\t%1\n127.0.0.1\twww.%1\n::1\t%1\n::1\twww.%1\n").arg( host );
		content.append( line.toUtf8() );
	}
	content.append( HostsMarkerEnd ).append( '\n' );

	file.close();

	QSaveFile output( hostsFilePath() );
	if( output.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		output.setPermissions( permissions ) == false || output.write( content ) != content.size() ||
		output.commit() == false )
	{
		vWarning() << "ExamMode: échec de l'écriture atomique de" << hostsFilePath();
		return false;
	}
	m_hostsModified = true;
	flushDnsCache();		// effet immédiat malgré les caches DNS/navigateur
	return true;
}



void ExamModeFeaturePlugin::revertHostsBlocking()
{
	if( removeHostsSection() )
	{
		flushDnsCache();
	}
	m_hostsModified = false;
}



/** Retire notre section délimitée du fichier hosts. Sûr même sans section présente. */
bool ExamModeFeaturePlugin::removeHostsSection()
{
	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadWrite | QIODevice::Text ) == false )
	{
		return false;
	}

	const auto raw = QString::fromUtf8( file.readAll() );
	const auto permissions = file.permissions();
	if( raw.contains( QLatin1String(HostsMarkerBegin) ) == false )
	{
		file.close();
		return false;		// rien à faire
	}

	const auto lines = raw.split( QLatin1Char('\n') );
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
	while( rebuilt.endsWith( QLatin1String("\n\n") ) )
	{
		rebuilt.chop( 1 );
	}

	file.close();
	QSaveFile output( hostsFilePath() );
	const auto content = rebuilt.toUtf8();
	if( output.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		output.setPermissions( permissions ) == false || output.write( content ) != content.size() ||
		output.commit() == false )
	{
		vWarning() << "ExamMode: impossible de nettoyer" << hostsFilePath();
		return false;
	}
	return true;
}



/** Purge le cache DNS pour que le blocage prenne effet sans attendre l'expiration. */
void ExamModeFeaturePlugin::flushDnsCache() const
{
#if defined(Q_OS_WIN)
	QProcess::startDetached( QStringLiteral("ipconfig"), { QStringLiteral("/flushdns") } );
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
	QProcess::startDetached( QStringLiteral("dscacheutil"), { QStringLiteral("-flushcache") } );
	QProcess::startDetached( QStringLiteral("killall"), { QStringLiteral("-HUP"), QStringLiteral("mDNSResponder") } );
#else
	// systemd-resolved (la plupart des VDI Linux récents) ; échec silencieux sinon.
	QProcess::startDetached( QStringLiteral("resolvectl"), { QStringLiteral("flush-caches") } );
#endif
}



bool ExamModeFeaturePlugin::isEndpointComponent()
{
	return VeyonCore::component() == VeyonCore::Component::Service ||
		   VeyonCore::component() == VeyonCore::Component::Server;
}


#if defined(Q_OS_LINUX)

QString ExamModeFeaturePlugin::firewallStateFile()
{
	return QStringLiteral("/var/lib/veyon/exammode-firewall.on");
}



QString ExamModeFeaturePlugin::firewallRulesetFile()
{
	return QStringLiteral("/var/lib/veyon/exammode-firewall.nft");
}



/**
 * Applique une allow-list egress via nftables (politique « drop »). Robuste aux
 * contournements navigateur (IP directe, DoH, résolveur alternatif, VPN vers un
 * endpoint arbitraire) puisque tout ce qui n'est pas explicitement autorisé est
 * jeté au niveau noyau. Nécessite les privilèges root (composant Service/Server
 * privilégié). Fail-closed : en cas d'échec d'application, aucune règle partielle
 * n'est laissée et false est retourné (le poste n'est PAS verrouillé, ce qui est
 * signalé bruyamment ; on préfère un poste ouvert et journalisé à un ruleset
 * incohérent). Un dead-man systemd retire les règles à l'échéance du bail même
 * si le processus Veyon meurt, pour ne jamais isoler durablement la VM.
 */
bool ExamModeFeaturePlugin::applyLinuxFirewall()
{
	const auto ruleset = ExamModeProfile::buildNftablesRuleset( m_allowedNetworks, m_dnsServers,
																m_supervisionNetworks );

	QDir().mkpath( QFileInfo( firewallRulesetFile() ).absolutePath() );
	QSaveFile file( firewallRulesetFile() );
	if( file.open( QIODevice::WriteOnly ) == false ||
		file.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner ) == false ||
		file.write( ruleset ) != ruleset.size() || file.commit() == false )
	{
		vWarning() << "ExamMode: impossible d'écrire le ruleset nftables" << firewallRulesetFile();
		return false;
	}

	QProcess nft;
	nft.start( QStringLiteral("nft"), { QStringLiteral("-f"), firewallRulesetFile() } );
	if( nft.waitForStarted( 3000 ) == false || nft.waitForFinished( 5000 ) == false ||
		nft.exitStatus() != QProcess::NormalExit || nft.exitCode() != 0 )
	{
		vWarning() << "ExamMode: échec de l'application nftables (poste NON verrouillé):"
				   << nft.readAllStandardError();
		removeLinuxFirewall();		// ne laisse pas de table partielle
		return false;
	}

	// marqueur pour le nettoyage au démarrage après crash
	QFile marker( firewallStateFile() );
	if( marker.open( QIODevice::WriteOnly | QIODevice::Text ) )
	{
		marker.write( "1" );
		marker.close();
	}

	// dead-man : retire la table à l'échéance du bail (+marge) même si Veyon meurt.
	if( armFirewallDeadMan( m_leaseSeconds + 60 ) == false )
	{
		vWarning() << "ExamMode: firewall retiré car son dead-man n'a pas pu être armé";
		removeLinuxFirewall();
		return false;
	}

	vInfo() << "ExamMode: firewall egress actif —" << m_allowedNetworks.size() << "réseau(x) autorisé(s),"
			<< m_dnsServers.size() << "résolveur(s) DNS";
	return true;
}



/** Retire la table nftables et désarme le dead-man. Idempotent. */
void ExamModeFeaturePlugin::removeLinuxFirewall()
{
	disarmFirewallDeadMan();

	// appel direct de nft (sans shell, pour ne pas dépendre d'un /bin/sh que le
	// mode strict pourrait bloquer). Le delete d'une table absente renvoie un
	// code non nul : on l'ignore volontairement (opération idempotente).
	QProcess nft;
	nft.start( QStringLiteral("nft"), { QStringLiteral("delete"), QStringLiteral("table"),
		QStringLiteral("inet"), QStringLiteral("veyon_exammode") } );
	nft.waitForStarted( 3000 );
	nft.waitForFinished( 5000 );

	QFile::remove( firewallStateFile() );
	QFile::remove( firewallRulesetFile() );
}



/**
 * Arme (ou ré-arme) une minuterie transitoire systemd à nom fixe qui supprimera
 * la table nftables dans « seconds » secondes. Chaque renouvellement du bail
 * remplace la minuterie précédente : tant que le portail ré-applique le profil,
 * la minuterie est repoussée ; s'il cesse (crash, arrêt, poste isolé), elle finit
 * par se déclencher et rétablit le réseau — garde-fou de dernier recours.
 */
bool ExamModeFeaturePlugin::armFirewallDeadMan( int seconds ) const
{
	disarmFirewallDeadMan();		// retire une minuterie précédente

	// systemd-run appelle nft directement (sans shell). nft delete d'une table
	// absente renvoie non nul mais le déclenchement est unique et sans effet.
	QProcess run;
	run.start( QStringLiteral("systemd-run"), {
		QStringLiteral("--unit=veyon-exammode-failsafe"),
		QStringLiteral("--on-active=%1s").arg( qMax( 60, seconds ) ),
		QStringLiteral("nft"), QStringLiteral("delete"), QStringLiteral("table"),
		QStringLiteral("inet"), QStringLiteral("veyon_exammode") } );
	if( run.waitForStarted( 3000 ) == false || run.waitForFinished( 5000 ) == false ||
		run.exitStatus() != QProcess::NormalExit || run.exitCode() != 0 )
	{
		vWarning() << "ExamMode: dead-man systemd non armé (systemd-run indisponible?):"
				   << run.readAllStandardError();
		return false;
	}
	return true;
}



void ExamModeFeaturePlugin::disarmFirewallDeadMan() const
{
	// appels directs systemctl (sans shell) ; codes de retour ignorés.
	QProcess stop;
	stop.start( QStringLiteral("systemctl"), { QStringLiteral("stop"),
		QStringLiteral("veyon-exammode-failsafe.timer") } );
	stop.waitForStarted( 3000 );
	stop.waitForFinished( 5000 );

	QProcess reset;
	reset.start( QStringLiteral("systemctl"), { QStringLiteral("reset-failed"),
		QStringLiteral("veyon-exammode-failsafe.timer") } );
	reset.waitForStarted( 3000 );
	reset.waitForFinished( 5000 );
}

#endif



/** Nom d'image Windows (basename + suffixe .exe) pour une clé IFEO. */
QString ExamModeFeaturePlugin::windowsImageName( const QString& executable )
{
	auto image = executable.trimmed().section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
	static const QRegularExpression InvalidImageCharacters( QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])") );
	if( image.isEmpty() || image == QStringLiteral(".") || image == QStringLiteral("..") ||
		InvalidImageCharacters.match( image ).hasMatch() )
	{
		return {};
	}
	if( image.isEmpty() == false && image.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) == false )
	{
		image += QStringLiteral(".exe");
	}
	return image;
}



/** Fichier d'état : liste des exécutables sous blocage de lancement (nettoyage anti-crash). */
QString ExamModeFeaturePlugin::launchPreventionStateFile()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/exammode-launch-state.json");
#else
	return QStringLiteral("/var/lib/veyon/exammode-launch-state.json");
#endif
}



/**
 * Empêche le LANCEMENT des exécutables interdits. Sous Windows via IFEO :
 * HKLM\...\Image File Execution Options\<exe> valeur Debugger = systray.exe
 * (no-op silencieux) → l'exe ne démarre plus. La liste appliquée est persistée
 * pour pouvoir la nettoyer même après un crash (cf. cleanupStaleLaunchPrevention).
 * Sous Linux : sans effet (le kill périodique fait le travail).
 */
bool ExamModeFeaturePlugin::applyLaunchPrevention( const QStringList& apps )
{
#if defined(Q_OS_WIN)
	removeLaunchPrevention();		// restaure d'abord l'état antérieur
	if( QFile::exists( launchPreventionStateFile() ) )
	{
		vWarning() << "ExamMode: restauration IFEO précédente incomplète; nouveau blocage refusé";
		return false;
	}
	if( apps.isEmpty() )
	{
		return true;
	}

	QJsonArray entries;
	for( const auto& app : apps )
	{
		const auto image = windowsImageName( app );
		if( image.isEmpty() )
		{
			continue;
		}
		const auto key = QStringLiteral(
			"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%1" ).arg( image );
		const auto previous = regRead( key, QStringLiteral("Debugger") );
		if( previous[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << "ExamMode: sauvegarde IFEO impossible; transaction refusée" << key;
			return false;
		}
		entries.append( QJsonObject{
			{ QStringLiteral("image"), image },
			{ QStringLiteral("key"), key },
			{ QStringLiteral("name"), QStringLiteral("Debugger") },
			{ QStringLiteral("previous"), previous },
			{ QStringLiteral("expected"), QJsonObject{
				{ QStringLiteral("exists"), true }, { QStringLiteral("type"), QStringLiteral("REG_SZ") },
				{ QStringLiteral("data"), QStringLiteral("C:\\Windows\\System32\\systray.exe") },
			} },
		} );
	}
	if( entries.isEmpty() )
	{
		return true;
	}

	if( writeJsonFile( launchPreventionStateFile(), QJsonObject{
		{ QStringLiteral("version"), 2 }, { QStringLiteral("entries"), entries } } ) == false )
	{
		vWarning() << "ExamMode: impossible de sauvegarder l'état IFEO; blocage annulé";
		return false;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		success = regSet( entry[QStringLiteral("key")].toString(), QStringLiteral("Debugger"),
			QStringLiteral("REG_SZ"), QStringLiteral("C:\\Windows\\System32\\systray.exe") ) && success;
	}
	if( success == false )
	{
		removeLaunchPrevention();
	}
	return success;
#elif defined(Q_OS_LINUX)
	// Prévention de lancement au niveau noyau via fanotify (FAN_OPEN_EXEC_PERM).
	// Le kill périodique (enforceTick) reste actif en complément et en repli si
	// fanotify est indisponible (privilèges/kernel). Une règle preventLaunch est
	// obligatoire : le kill périodique ne constitue pas un succès équivalent.
	if( apps.isEmpty() )
	{
		if( m_execGuard )
		{
			m_execGuard->stopGuarding();
		}
		return true;
	}
	if( m_execGuard == nullptr )
	{
		m_execGuard = new ExamModeLinuxExecGuard( this );
	}
	if( m_execGuard->isActive() )
	{
		m_execGuard->updateBlocked( apps );
		return true;
	}
	if( m_execGuard->startGuarding( apps ) == false )
	{
		vWarning() << "ExamMode: prévention de lancement noyau indisponible";
		return false;
	}
	return m_execGuard->isHealthy();
#else
	Q_UNUSED(apps)
	return apps.isEmpty();
#endif
}



/** Lève le blocage de lancement (retire les clés IFEO) et supprime le fichier d'état. */
void ExamModeFeaturePlugin::removeLaunchPrevention()
{
#if defined(Q_OS_WIN)
	if( QFile::exists( launchPreventionStateFile() ) == false )
	{
		return;
	}
	const auto state = readJsonFile( launchPreventionStateFile() );
	const auto entries = state[QStringLiteral("entries")].toArray();
	if( entries.isEmpty() )
	{
		vWarning() << "ExamMode: état IFEO illisible; conservation du fichier pour diagnostic";
		return;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		success = regRestore( entry[QStringLiteral("key")].toString(), QStringLiteral("Debugger"),
			entry[QStringLiteral("previous")].toObject(), entry[QStringLiteral("expected")].toObject() ) && success;
	}
	if( success )
	{
		QFile::remove( launchPreventionStateFile() );
	}
	else
	{
		vWarning() << "ExamMode: restauration IFEO incomplète; elle sera retentée au prochain démarrage";
	}
#elif defined(Q_OS_LINUX)
	if( m_execGuard )
	{
		m_execGuard->stopGuarding();
	}
#endif
}



/** Au démarrage du service : retire un blocage de lancement laissé par un crash. */
void ExamModeFeaturePlugin::cleanupStaleLaunchPrevention()
{
#if defined(Q_OS_WIN)
	if( QFile::exists( launchPreventionStateFile() ) )
	{
		vInfo() << "ExamMode: restauration d'un blocage de lancement résiduel";
		removeLaunchPrevention();
	}
#endif
}


#if defined(Q_OS_WIN)

QString ExamModeFeaturePlugin::pacFilePath()
{
	return QStringLiteral("C:/ProgramData/Veyon/exammode.pac");
}



QString ExamModeFeaturePlugin::siteFilterStateFile()
{
	return QStringLiteral("C:/ProgramData/Veyon/exammode-site-state.json");
}



/** Écrit un PAC : liste blanche (allow) ou liste noire (block), robuste au DoH. */
QString ExamModeFeaturePlugin::legacySiteFilterMarkerFile()
{
	return QStringLiteral("C:/ProgramData/Veyon/exam-sitefilter.on");
}



QString ExamModeFeaturePlugin::legacyLaunchPreventionStateFile()
{
	return QStringLiteral("C:/ProgramData/Veyon/exammode-blocked-apps.txt");
}



bool ExamModeFeaturePlugin::writePacFile()
{
	const auto pac = ExamModeProfile::buildPac( m_urlRules, m_defaultUrlAction );

	QDir().mkpath( QFileInfo( pacFilePath() ).absolutePath() );
	QSaveFile file( pacFilePath() );
	if( file.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		file.write( pac ) != pac.size() || file.commit() == false )
	{
		vWarning() << "ExamMode: impossible d'écrire le fichier PAC" << pacFilePath();
		return false;
	}
	return true;
}



bool ExamModeFeaturePlugin::regSet( const QString& key, const QString& name, const QString& type, const QString& data )
{
	if( ExamModeWindowsNative::setRegistryValue( key, name, type, data ) == false )
	{
		vWarning() << "ExamMode: échec RegSetValueEx" << key << name;
		return false;
	}
	return true;
}



bool ExamModeFeaturePlugin::regDelete( const QString& key, const QString& name )
{
	if( ExamModeWindowsNative::deleteRegistryValue( key, name ) == false )
	{
		vWarning() << "ExamMode: échec RegDeleteValue" << key << name;
		return false;
	}
	return true;
}



QJsonObject ExamModeFeaturePlugin::regRead( const QString& key, const QString& name )
{
	return ExamModeWindowsNative::readRegistryValue( key, name );
}



bool ExamModeFeaturePlugin::regRestore( const QString& key, const QString& name, const QJsonObject& previous,
										   const QJsonObject& expected )
{
	if( expected.isEmpty() == false )
	{
		const auto current = regRead( key, name );
		if( current[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << "ExamMode: lecture registre impossible, restauration différée" << key << name;
			return false;
		}
		const auto currentType = current[QStringLiteral("type")].toString();
		const auto expectedType = expected[QStringLiteral("type")].toString();
		const auto currentData = current[QStringLiteral("data")].toString();
		const auto expectedData = expected[QStringLiteral("data")].toString();
		const bool dataMatches = expectedType.compare( QStringLiteral("REG_DWORD"), Qt::CaseInsensitive ) == 0 ?
			currentData.toULongLong( nullptr, 0 ) == expectedData.toULongLong( nullptr, 0 ) :
			currentData == expectedData;
		if( current[QStringLiteral("exists")].toBool() != expected[QStringLiteral("exists")].toBool() ||
			currentType.compare( expectedType, Qt::CaseInsensitive ) != 0 || dataMatches == false )
		{
			vWarning() << "ExamMode: politique modifiée par un tiers, restauration ignorée" << key << name;
			return true;
		}
	}
	if( previous[QStringLiteral("exists")].toBool() )
	{
		return ExamModeWindowsNative::restoreRegistryValue( key, name, previous );
	}
	return previous[QStringLiteral("ok")].toBool() && regDelete( key, name );
}



/**
 * Filtrage des sites sous Windows : PAC (data: pour Chrome/Edge, file:// pour
 * Firefox) via politiques HKLM + désactivation DoH (sinon le navigateur pourrait
 * résoudre hors du contrôle). Couvre liste noire ET blanche.
 */
bool ExamModeFeaturePlugin::applyWindowsSiteFiltering( const QStringList& sites, const QString& mode )
{
	removeWindowsSiteFiltering();		// restaure d'abord l'état précédent (PAC + firewall)
	if( QFile::exists( siteFilterStateFile() ) )
	{
		vWarning() << "ExamMode: restauration précédente incomplète; nouveau filtrage refusé";
		return false;
	}

	// Backend firewall (opt-in) : allow-list egress au niveau du pare-feu Windows,
	// robuste à l'IP directe / DoH / résolveur alternatif / VPN — parité avec le
	// backend nftables Linux. Posé AVANT le court-circuit PAC pour couvrir aussi les
	// profils purement CIDR (sans règle URL). Fail-closed : en cas d'échec, aucune
	// règle partielle et le poste reste ouvert (bruyamment journalisé).
	if( m_networkBackend == ExamModeProfile::NetworkBackend::Firewall &&
		applyWindowsFirewall() == false )
	{
		vWarning() << "ExamMode: backend firewall Windows non appliqué (poste NON isolé)";
		return false;
	}

	if( m_urlRules.isEmpty() && m_defaultUrlAction == ExamModeProfile::RuleAction::Allow )
	{
		return true;		// pas de filtrage de domaines PAC (le firewall éventuel est déjà posé)
	}
	const auto pac = ExamModeProfile::buildPac( m_urlRules, m_defaultUrlAction );
	const auto dataUrl = QStringLiteral("data:application/x-ns-proxy-autoconfig;base64,")
		+ QString::fromLatin1( pac.toBase64() );
	const auto fileUrl = QStringLiteral("file:///") + pacFilePath();

	const QList<QPair<QString, QString>> policyValues = {
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"), QStringLiteral("ProxyMode") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"), QStringLiteral("ProxyPacUrl") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"), QStringLiteral("DnsOverHttpsMode") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"), QStringLiteral("ProxyMode") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"), QStringLiteral("ProxyPacUrl") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"), QStringLiteral("DnsOverHttpsMode") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("Mode") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("AutoConfigURL") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("Locked") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\DNSOverHTTPS"), QStringLiteral("Enabled") },
	};

	QJsonArray entries;
	for( const auto& policy : policyValues )
	{
		QString type = QStringLiteral("REG_SZ");
		QString data;
		if( policy.second == QStringLiteral("ProxyMode") ) { data = QStringLiteral("pac_script"); }
		else if( policy.second == QStringLiteral("ProxyPacUrl") ) { data = dataUrl; }
		else if( policy.second == QStringLiteral("DnsOverHttpsMode") ) { data = QStringLiteral("off"); }
		else if( policy.second == QStringLiteral("Mode") ) { data = QStringLiteral("autoConfig"); }
		else if( policy.second == QStringLiteral("AutoConfigURL") ) { data = fileUrl; }
		else if( policy.second == QStringLiteral("Locked") ) { type = QStringLiteral("REG_DWORD"); data = QStringLiteral("1"); }
		else if( policy.second == QStringLiteral("Enabled") ) { type = QStringLiteral("REG_DWORD"); data = QStringLiteral("0"); }
		const auto previous = regRead( policy.first, policy.second );
		if( previous[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << "ExamMode: sauvegarde de politique navigateur impossible" << policy.first << policy.second;
			return false;
		}
		entries.append( QJsonObject{
			{ QStringLiteral("key"), policy.first }, { QStringLiteral("name"), policy.second },
			{ QStringLiteral("previous"), previous },
			{ QStringLiteral("expected"), QJsonObject{
				{ QStringLiteral("exists"), true }, { QStringLiteral("type"), type },
				{ QStringLiteral("data"), data },
			} },
		} );
	}

	QByteArray previousPac;
	QFile oldPac( pacFilePath() );
	const bool previousPacExists = oldPac.open( QIODevice::ReadOnly );
	if( previousPacExists )
	{
		previousPac = oldPac.readAll();
	}
	const QJsonObject state{
		{ QStringLiteral("version"), 2 }, { QStringLiteral("entries"), entries },
		{ QStringLiteral("pacExisted"), previousPacExists },
		{ QStringLiteral("previousPac"), QString::fromLatin1( previousPac.toBase64() ) },
	};
	if( writeJsonFile( siteFilterStateFile(), state ) == false )
	{
		vWarning() << "ExamMode: impossible de sauvegarder les politiques navigateur";
		return false;
	}
	if( writePacFile() == false )
	{
		removeWindowsSiteFiltering();
		return false;
	}

	const QStringList chromiumKeys = {
		QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"),
		QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"),
	};
	bool success = true;
	for( const auto& key : chromiumKeys )
	{
		success = regSet( key, QStringLiteral("ProxyMode"), QStringLiteral("REG_SZ"), QStringLiteral("pac_script") ) && success;
		success = regSet( key, QStringLiteral("ProxyPacUrl"), QStringLiteral("REG_SZ"), dataUrl ) && success;
		success = regSet( key, QStringLiteral("DnsOverHttpsMode"), QStringLiteral("REG_SZ"), QStringLiteral("off") ) && success;
	}

	// Firefox : proxy autoConfig + DoH off (via ADMX → registre)
	success = regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
		QStringLiteral("Mode"), QStringLiteral("REG_SZ"), QStringLiteral("autoConfig") ) && success;
	success = regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
		QStringLiteral("AutoConfigURL"), QStringLiteral("REG_SZ"), fileUrl ) && success;
	success = regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
		QStringLiteral("Locked"), QStringLiteral("REG_DWORD"), QStringLiteral("1") ) && success;
	success = regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\DNSOverHTTPS"),
		QStringLiteral("Enabled"), QStringLiteral("REG_DWORD"), QStringLiteral("0") ) && success;

	if( success == false )
	{
		removeWindowsSiteFiltering();
	}
	return success;
}



void ExamModeFeaturePlugin::removeWindowsSiteFiltering()
{
	// Retire d'abord l'allow-list egress (rétablit la connectivité) ; idempotent,
	// sans effet si le backend firewall n'a jamais été posé.
	removeWindowsFirewall();

	if( QFile::exists( siteFilterStateFile() ) == false )
	{
		return;
	}
	const auto state = readJsonFile( siteFilterStateFile() );
	const auto entries = state[QStringLiteral("entries")].toArray();
	if( entries.isEmpty() )
	{
		vWarning() << "ExamMode: sauvegarde des politiques illisible; restauration différée";
		return;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		success = regRestore( entry[QStringLiteral("key")].toString(), entry[QStringLiteral("name")].toString(),
			entry[QStringLiteral("previous")].toObject(), entry[QStringLiteral("expected")].toObject() ) && success;
	}

	if( state[QStringLiteral("pacExisted")].toBool() )
	{
		QSaveFile file( pacFilePath() );
		const auto contents = QByteArray::fromBase64( state[QStringLiteral("previousPac")].toString().toLatin1() );
		success = file.open( QIODevice::WriteOnly ) && file.write( contents ) == contents.size() && file.commit() && success;
	}
	else
	{
		success = ( QFile::exists( pacFilePath() ) == false || QFile::remove( pacFilePath() ) ) && success;
	}

	if( success )
	{
		QFile::remove( siteFilterStateFile() );
	}
	else
	{
		vWarning() << "ExamMode: restauration des politiques navigateur incomplète";
	}
}



void ExamModeFeaturePlugin::cleanupLegacyWindowsState()
{
	if( QFile::exists( legacySiteFilterMarkerFile() ) )
	{
		vWarning() << "ExamMode: migration de l'ancien état de filtrage non transactionnel";
		const QStringList chromiumKeys = {
			QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"),
			QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"),
		};
		for( const auto& key : chromiumKeys )
		{
			regDelete( key, QStringLiteral("ProxyMode") );
			regDelete( key, QStringLiteral("ProxyPacUrl") );
			regDelete( key, QStringLiteral("DnsOverHttpsMode") );
		}
		regDelete( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("Mode") );
		regDelete( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("AutoConfigURL") );
		regDelete( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"), QStringLiteral("Locked") );
		regDelete( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\DNSOverHTTPS"), QStringLiteral("Enabled") );
		QFile::remove( QStringLiteral("C:/ProgramData/Veyon/exam.pac") );
		QFile::remove( legacySiteFilterMarkerFile() );
	}

	QFile legacyState( legacyLaunchPreventionStateFile() );
	if( legacyState.open( QIODevice::ReadOnly | QIODevice::Text ) )
	{
		const auto images = QString::fromUtf8( legacyState.readAll() ).split( QLatin1Char('\n'), Qt::SkipEmptyParts );
		for( const auto& image : images )
		{
			const auto key = QStringLiteral(
				"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%1" ).arg( image );
			const auto debugger = regRead( key, QStringLiteral("Debugger") );
			if( debugger[QStringLiteral("data")].toString().compare(
				QStringLiteral("C:\\Windows\\System32\\systray.exe"), Qt::CaseInsensitive ) == 0 )
			{
				regDelete( key, QStringLiteral("Debugger") );
			}
		}
		legacyState.close();
		QFile::remove( legacyLaunchPreventionStateFile() );
	}
}



QString ExamModeFeaturePlugin::windowsFirewallStateFile()
{
	return QStringLiteral("C:/ProgramData/Veyon/exammode-firewall-state.json");
}



bool ExamModeFeaturePlugin::applyWindowsFirewall()
{
	// Modifier la politique globale par un utilitaire en ligne de commande n'est
	// ni transactionnel ni restaurable exactement. Jusqu'à l'intégration d'un
	// fournisseur WFP à sous-couche dynamique, annoncer cette capacité serait une
	// faille de sécurité. Le profil est donc refusé explicitement, sans mutation.
	vWarning() << "ExamMode: firewall Windows refusé — fournisseur WFP transactionnel indisponible";
	return false;
}



/** Retire les règles egress et restaure la politique par défaut. Idempotent. */
void ExamModeFeaturePlugin::removeWindowsFirewall()
{
	// applyWindowsFirewall refuse toujours d'appliquer un pare-feu (pas de WFP
	// transactionnel) : il n'y a donc rien à retirer ici. On se contente de signaler
	// un marqueur d'état hérité (ancien backend netsh) qui exige une intervention
	// manuelle. NB : la restriction d'ACL du fichier PAC, si elle est voulue, doit
	// se faire à l'ÉCRITURE du PAC, pas ici.
	if( QFile::exists( windowsFirewallStateFile() ) )
	{
		vCritical() << "ExamMode: état firewall hérité détecté; restauration manuelle requise"
			<< windowsFirewallStateFile();
	}
}

#endif
