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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include "ExamModeFeaturePlugin.h"
#include "ExamModeProfile.h"
#include "FeatureMessage.h"
#include "VeyonCore.h"
#include "VeyonServerInterface.h"

// intervalle de rappel de l'application des restrictions (fin des processus interdits)
static constexpr int EnforceIntervalMs = 4000;
// fail-safe : le portail ré-applique le profil chaque minute ; sans re-push
// pendant ce délai, on lève automatiquement toutes les restrictions.
static constexpr int DefaultLeaseSeconds = 5 * 60;
static constexpr int MinimumLeaseSeconds = 60;
static constexpr int MaximumLeaseSeconds = 60 * 60;

namespace
{

#if defined(Q_OS_WIN)
bool writeJsonFile( const QString& path, const QJsonObject& object )
{
	QDir().mkpath( QFileInfo( path ).absolutePath() );
	QSaveFile file( path );
	if( file.open( QIODevice::WriteOnly ) == false )
	{
		return false;
	}
	if( file.write( QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) < 0 )
	{
		file.cancelWriting();
		return false;
	}
	return file.commit();
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
#endif

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

		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StartExam }
							.addArgument( Argument::BlockedApps, apps )
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
							.addArgument( Argument::ProfileDigest, profileDigest ),
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

	// handleFeatureMessage(server) s'exécute dans le composant Server du poste,
	// suffisamment privilégié pour agir sur le système (ScreenLock y désactive
	// déjà les périphériques d'entrée) : on y applique donc hosts/registre/kill.
	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartExam:
	{
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
		QStringList rejectedProcesses;
		const auto processPolicy = ExamModeProfile::resolveProcessRules( processRules, platform, &rejectedProcesses );
		QStringList rejectedUrls;
		const auto urlRules = ExamModeProfile::normalizeUrlRules(
			message.argument( Argument::UrlRules ).toList(), &rejectedUrls );
		if( rejectedProcesses.isEmpty() == false )
		{
			vWarning() << "ExamMode:" << rejectedProcesses.size() << "regle(s) processus invalide(s) ignoree(s)";
		}
		if( rejectedUrls.isEmpty() == false )
		{
			vWarning() << "ExamMode:" << rejectedUrls.size() << "regle(s) URL invalide(s) ignoree(s)";
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
				message.argument( Argument::ProfileDigest ).toString() ) == false )
		{
			vWarning() << "ExamMode: profil StartExam refusé — restrictions inchangées";
		}
		return true;
	}

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



bool ExamModeFeaturePlugin::startEnforcement( const ExamModeProfile::ProcessPolicy& processPolicy,
											  const QStringList& sites, const QList<ExamModeProfile::UrlRule>& urlRules,
											  const QString& sitesMode, const QString& defaultUrlAction, int leaseSeconds,
											  const QString& profileId, qint64 profileRevision, const QString& expectedDigest )
{
	const bool wasActive = m_active;
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
		normalizedProcessPolicy, effectiveUrlRules, effectiveDefaultAction, normalizedLeaseSeconds );
	if( validMode == false )
	{
		vWarning() << "ExamMode: mode de filtrage invalide" << sitesMode << "— utilisation de block";
	}
	if( rejectedSites.isEmpty() == false )
	{
		vWarning() << "ExamMode:" << rejectedSites.size() << "site(s) invalide(s) ignoré(s)" << rejectedSites;
	}
	if( normalizedProfileId.size() > 128 || normalizedProfileId.contains( QLatin1Char('\r') ) ||
		normalizedProfileId.contains( QLatin1Char('\n') ) || profileRevision < 0 )
	{
		vWarning() << "ExamMode: identite ou revision de profil invalide";
		return false;
	}
	if( normalizedProfileId == m_profileId && profileRevision < m_profileRevision )
	{
		vWarning() << "ExamMode: revision de profil obsolete refusee" << profileRevision << "<" << m_profileRevision;
		return false;
	}
	const bool managedRevision = normalizedProfileId != QStringLiteral("legacy") ||
		profileRevision > 0 || expectedDigest.trimmed().isEmpty() == false;
	if( managedRevision && normalizedProfileId == m_profileId && profileRevision == m_profileRevision &&
		m_profileDigest.isEmpty() == false && digest != m_profileDigest )
	{
		vWarning() << "ExamMode: contenu different pour une revision deja appliquee";
		return false;
	}
	if( expectedDigest.trimmed().isEmpty() == false &&
		digest.compare( expectedDigest.trimmed(), Qt::CaseInsensitive ) != 0 )
	{
		vWarning() << "ExamMode: empreinte du profil incorrecte; profil refuse";
		return false;
	}
	m_blockedApps = normalizedProcessPolicy.terminateApplications;
	m_launchPreventedApps = normalizedProcessPolicy.preventLaunchApplications;
	m_sites = normalizedSites;
	m_urlRules = effectiveUrlRules;
	m_sitesMode = normalizedSitesMode;
	m_defaultUrlAction = effectiveDefaultAction;
	m_hasStructuredUrlRules = urlRules.isEmpty() == false;
	m_leaseSeconds = normalizedLeaseSeconds;
	m_profileId = normalizedProfileId;
	m_profileRevision = profileRevision;
	m_profileDigest = digest;
	m_active = true;
	vInfo() << "ExamMode: profil" << m_profileId << "revision" << m_profileRevision
		<< "empreinte" << m_profileDigest.left( 12 );

	// Le portail ré-applique le profil chaque minute (couverture des postes
	// connectés après coup) : on ne réapplique le filtrage réseau QUE si la
	// politique réseau a changé (règles URL — qui incluent les sites hérités —,
	// action par défaut, mode), pour éviter réécriture hosts/flush DNS et
	// aller-retour registre inutiles quand seules les applis changent.
	QStringList signatureParts{ m_sitesMode,
		m_defaultUrlAction == ExamModeProfile::RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") };
	for( const auto& rule : m_urlRules )
	{
		signatureParts.append( QStringLiteral("%1|%2|%3").arg(
			rule.action == ExamModeProfile::RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block"),
			rule.regularExpression ? QStringLiteral("regex") : QStringLiteral("glob"),
			rule.expression ) );
	}
	const auto signature = signatureParts.join( QLatin1Char('\n') );
	if( signature != m_hostsSignature )
	{
		vInfo() << "ExamMode: enforcement -" << m_blockedApps.size() << "app(s),"
				<< m_sites.size() << "site(s), mode" << m_sitesMode;
		m_siteFilteringActive = applySiteFiltering( m_sites, m_sitesMode );
		m_hostsSignature = m_siteFilteringActive ? signature : QString{};
		if( m_siteFilteringActive == false && ( m_sites.isEmpty() == false || m_sitesMode == QStringLiteral("allow") ) )
		{
			vWarning() << "ExamMode: filtrage réseau non appliqué; nouvelle tentative au prochain renouvellement";
		}
	}
	else if( wasActive == false )
	{
		vInfo() << "ExamMode: enforcement (re)started -" << m_blockedApps.size() << "app(s)";
	}

	// Empêche le lancement des logiciels interdits (IFEO Windows) si la liste change.
	const auto appsSig = m_blockedApps.join( QLatin1Char('\n') ) + QStringLiteral("\n--prevent--\n") +
		m_launchPreventedApps.join( QLatin1Char('\n') );
	if( appsSig != m_appsSignature )
	{
		if( applyLaunchPrevention( m_launchPreventedApps ) == false )
		{
			vWarning() << "ExamMode: le blocage au lancement n'a pas pu être appliqué complètement";
			m_appsSignature.clear();
		}
		else
		{
			m_appsSignature = appsSig;
		}
	}

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
			stopEnforcement();
		} );
	}
	m_watchdog->start( m_leaseSeconds * 1000 );

	enforceTick();		// passe immédiate sur les processus interdits
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
	removeLaunchPrevention();
	removeSiteFiltering();
	m_blockedApps.clear();
	m_launchPreventedApps.clear();
	m_sites.clear();
	m_urlRules.clear();
	m_hasStructuredUrlRules = false;
	m_defaultUrlAction = ExamModeProfile::RuleAction::Allow;
	m_siteFilteringActive = false;
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
	// /F force, /IM par nom d'image ; les profils Omnissa peuvent contenir un chemin complet.
	const auto image = windowsImageName( name );
	if( image.isEmpty() )
	{
		vWarning() << "ExamMode: nom d'application Windows invalide ignoré" << name;
		return;
	}
	QProcess::startDetached( QStringLiteral("taskkill"), { QStringLiteral("/F"), QStringLiteral("/IM"), image } );
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
	if( m_hasStructuredUrlRules )
	{
		vWarning() << "ExamMode: les regles URL ordonnees/regex necessitent le backend PAC Windows; profil refuse";
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
		entries.append( QJsonObject{
			{ QStringLiteral("image"), image },
			{ QStringLiteral("key"), key },
			{ QStringLiteral("previous"), regRead( key, QStringLiteral("Debugger") ) },
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
#else
	Q_UNUSED(apps)
	return true;
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
	QProcess p;
	p.start( QStringLiteral("reg"), { QStringLiteral("add"), key, QStringLiteral("/v"), name,
		QStringLiteral("/t"), type, QStringLiteral("/d"), data, QStringLiteral("/f") } );
	if( p.waitForStarted( 2000 ) == false || p.waitForFinished( 5000 ) == false ||
		p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0 )
	{
		vWarning() << "ExamMode: échec reg add" << key << name << p.readAllStandardError();
		return false;
	}
	return true;
}



bool ExamModeFeaturePlugin::regDelete( const QString& key, const QString& name )
{
	if( regRead( key, name )[QStringLiteral("exists")].toBool() == false )
	{
		return true;
	}
	QProcess p;
	p.start( QStringLiteral("reg"), { QStringLiteral("delete"), key, QStringLiteral("/v"), name, QStringLiteral("/f") } );
	if( p.waitForStarted( 2000 ) == false || p.waitForFinished( 5000 ) == false ||
		p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0 )
	{
		vWarning() << "ExamMode: échec reg delete" << key << name << p.readAllStandardError();
		return false;
	}
	return true;
}



QJsonObject ExamModeFeaturePlugin::regRead( const QString& key, const QString& name )
{
	QProcess process;
	process.start( QStringLiteral("reg"), { QStringLiteral("query"), key, QStringLiteral("/v"), name } );
	if( process.waitForStarted( 2000 ) == false || process.waitForFinished( 5000 ) == false ||
		process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 )
	{
		return QJsonObject{ { QStringLiteral("exists"), false } };
	}

	const auto expression = QRegularExpression(
		QStringLiteral("^\\s*%1\\s+(REG_[A-Z0-9_]+)\\s+(.*)$").arg( QRegularExpression::escape( name ) ),
		QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption );
	const auto match = expression.match( QString::fromLocal8Bit( process.readAllStandardOutput() ) );
	if( match.hasMatch() == false )
	{
		return QJsonObject{ { QStringLiteral("exists"), false } };
	}
	return QJsonObject{
		{ QStringLiteral("exists"), true },
		{ QStringLiteral("type"), match.captured( 1 ) },
		{ QStringLiteral("data"), match.captured( 2 ).trimmed() },
	};
}



bool ExamModeFeaturePlugin::regRestore( const QString& key, const QString& name, const QJsonObject& previous,
										   const QJsonObject& expected )
{
	if( expected.isEmpty() == false )
	{
		const auto current = regRead( key, name );
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
		return regSet( key, name, previous[QStringLiteral("type")].toString(),
			previous[QStringLiteral("data")].toString() );
	}
	return regDelete( key, name );
}



/**
 * Filtrage des sites sous Windows : PAC (data: pour Chrome/Edge, file:// pour
 * Firefox) via politiques HKLM + désactivation DoH (sinon le navigateur pourrait
 * résoudre hors du contrôle). Couvre liste noire ET blanche.
 */
bool ExamModeFeaturePlugin::applyWindowsSiteFiltering( const QStringList& sites, const QString& mode )
{
	removeWindowsSiteFiltering();		// restaure d'abord l'état précédent
	if( QFile::exists( siteFilterStateFile() ) )
	{
		vWarning() << "ExamMode: restauration précédente incomplète; nouveau filtrage refusé";
		return false;
	}

	if( m_urlRules.isEmpty() && m_defaultUrlAction == ExamModeProfile::RuleAction::Allow )
	{
		return true;		// liste noire vide = rien à filtrer
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
		entries.append( QJsonObject{
			{ QStringLiteral("key"), policy.first }, { QStringLiteral("name"), policy.second },
			{ QStringLiteral("previous"), regRead( policy.first, policy.second ) },
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

#endif
