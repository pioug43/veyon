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
#include <QProcess>
#include <QTimer>

#include "ExamModeFeaturePlugin.h"
#include "FeatureMessage.h"
#include "VeyonCore.h"
#include "VeyonServerInterface.h"

// intervalle de rappel de l'application des restrictions (fin des processus interdits)
static constexpr int EnforceIntervalMs = 4000;
// fail-safe : le portail ré-applique le profil chaque minute ; sans re-push
// pendant ce délai, on lève automatiquement toutes les restrictions.
static constexpr int WatchdogMs = 5 * 60 * 1000;


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
	if( VeyonCore::component() == VeyonCore::Component::Service )
	{
		removeHostsSection();
		cleanupStaleLaunchPrevention();
#if defined(Q_OS_WIN)
		// filtrage sites résiduel (politiques proxy/DoH) laissé par un crash
		if( QFile::exists( siteFilterMarkerFile() ) )
		{
			vInfo() << "ExamMode: nettoyage d'un filtrage de sites résiduel";
			removeWindowsSiteFiltering();
		}
#endif
	}
}



ExamModeFeaturePlugin::~ExamModeFeaturePlugin()
{
	// filet de sécurité : ne jamais laisser de restriction derrière soi
	removeSiteFiltering();
	removeLaunchPrevention();
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
	const bool wasActive = m_active;
	m_blockedApps = blockedApps;
	m_sites = sites;
	m_sitesMode = sitesMode.isEmpty() ? QStringLiteral("block") : sitesMode;
	m_active = true;

	// Le portail ré-applique le profil chaque minute (couverture des postes
	// connectés après coup) : on ne réécrit le fichier hosts QUE si la liste des
	// sites ou le mode a changé, pour éviter une réécriture/flush DNS inutile.
	const auto signature = hostsSignature( m_sites, m_sitesMode );
	if( signature != m_hostsSignature )
	{
		vInfo() << "ExamMode: enforcement -" << m_blockedApps.size() << "app(s),"
				<< m_sites.size() << "site(s), mode" << m_sitesMode;
		applySiteFiltering( m_sites, m_sitesMode );
		m_hostsSignature = signature;
	}
	else if( wasActive == false )
	{
		vInfo() << "ExamMode: enforcement (re)started -" << m_blockedApps.size() << "app(s)";
	}

	// Empêche le lancement des logiciels interdits (IFEO Windows) si la liste change.
	const auto appsSig = m_blockedApps.join( QLatin1Char('\n') );
	if( appsSig != m_appsSignature )
	{
		applyLaunchPrevention( m_blockedApps );
		m_appsSignature = appsSig;
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
	m_watchdog->start( WatchdogMs );

	enforceTick();		// passe immédiate sur les processus interdits
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



QString ExamModeFeaturePlugin::hostsSignature( const QStringList& sites, const QString& mode )
{
	return mode + QLatin1Char('\n') + sites.join( QLatin1Char('\n') );
}



/**
 * Applique le filtrage des sites. Windows : PAC (liste noire OU blanche) + DoH
 * désactivé, robuste au contournement. Linux : fichier hosts (liste noire).
 */
void ExamModeFeaturePlugin::applySiteFiltering( const QStringList& sites, const QString& mode )
{
#if defined(Q_OS_WIN)
	applyWindowsSiteFiltering( sites, mode );
#else
	Q_UNUSED(mode)
	applyHostsBlocking( sites );
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



void ExamModeFeaturePlugin::applyHostsBlocking( const QStringList& sites )
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
		return;
	}

	removeHostsSection();		// repart d'un fichier propre (idempotent)

	// Aucun site à bloquer : on s'est déjà assuré qu'il ne reste pas de section.
	const auto cleaned = [&sites] {
		QStringList out;
		for( const auto& s : sites )
		{
			const auto h = s.trimmed();
			if( h.isEmpty() == false )
			{
				out.append( h );
			}
		}
		return out;
	}();
	if( cleaned.isEmpty() )
	{
		flushDnsCache();
		return;
	}

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
	for( const auto& host : cleaned )
	{
		// Redirige le domaine ET son sous-domaine www vers l'adresse de rebouclage.
		const auto line = QStringLiteral("127.0.0.1\t%1\n127.0.0.1\twww.%1\n::1\t%1\n::1\twww.%1\n").arg( host );
		content.append( line.toUtf8() );
	}
	content.append( HostsMarkerEnd ).append( '\n' );

	file.resize( 0 );
	file.write( content );
	file.close();
	m_hostsModified = true;
	flushDnsCache();		// effet immédiat malgré les caches DNS/navigateur
}



void ExamModeFeaturePlugin::revertHostsBlocking()
{
	if( m_hostsModified == false )
	{
		return;
	}
	removeHostsSection();
	flushDnsCache();
	m_hostsModified = false;
}



/** Retire notre section délimitée du fichier hosts. Sûr même sans section présente. */
void ExamModeFeaturePlugin::removeHostsSection()
{
	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadWrite | QIODevice::Text ) == false )
	{
		return;
	}

	const auto raw = QString::fromUtf8( file.readAll() );
	if( raw.contains( QLatin1String(HostsMarkerBegin) ) == false )
	{
		file.close();
		return;		// rien à faire
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

	file.resize( 0 );
	file.write( rebuilt.toUtf8() );
	file.close();
}



/** Purge le cache DNS pour que le blocage prenne effet sans attendre l'expiration. */
void ExamModeFeaturePlugin::flushDnsCache() const
{
#if defined(Q_OS_WIN)
	QProcess::startDetached( QStringLiteral("ipconfig"), { QStringLiteral("/flushdns") } );
#else
	// systemd-resolved (la plupart des VDI Linux récents) ; échec silencieux sinon.
	QProcess::startDetached( QStringLiteral("resolvectl"), { QStringLiteral("flush-caches") } );
#endif
}



/** Nom d'image Windows (basename + suffixe .exe) pour une clé IFEO. */
QString ExamModeFeaturePlugin::windowsImageName( const QString& executable )
{
	auto image = executable.trimmed().section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
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
	return QStringLiteral("C:/ProgramData/Veyon/exammode-blocked-apps.txt");
#else
	return QStringLiteral("/var/lib/veyon/exammode-blocked-apps.txt");
#endif
}



/**
 * Empêche le LANCEMENT des exécutables interdits. Sous Windows via IFEO :
 * HKLM\...\Image File Execution Options\<exe> valeur Debugger = systray.exe
 * (no-op silencieux) → l'exe ne démarre plus. La liste appliquée est persistée
 * pour pouvoir la nettoyer même après un crash (cf. cleanupStaleLaunchPrevention).
 * Sous Linux : sans effet (le kill périodique fait le travail).
 */
void ExamModeFeaturePlugin::applyLaunchPrevention( const QStringList& apps )
{
#if defined(Q_OS_WIN)
	removeLaunchPrevention();		// repart d'un état propre

	QStringList applied;
	for( const auto& app : apps )
	{
		const auto image = windowsImageName( app );
		if( image.isEmpty() )
		{
			continue;
		}
		const auto key = QStringLiteral(
			"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%1" ).arg( image );
		QProcess reg;
		reg.start( QStringLiteral("reg"), { QStringLiteral("add"), key,
			QStringLiteral("/v"), QStringLiteral("Debugger"),
			QStringLiteral("/t"), QStringLiteral("REG_SZ"),
			QStringLiteral("/d"), QStringLiteral("C:\\Windows\\System32\\systray.exe"),
			QStringLiteral("/f") } );
		reg.waitForFinished( 5000 );
		applied.append( image );
	}
	m_preventedApps = applied;

	// persiste la liste pour un nettoyage fiable même après un crash du service
	QDir().mkpath( QFileInfo( launchPreventionStateFile() ).absolutePath() );
	QFile state( launchPreventionStateFile() );
	if( state.open( QIODevice::WriteOnly | QIODevice::Text ) )
	{
		state.write( applied.join( QLatin1Char('\n') ).toUtf8() );
		state.close();
	}
#else
	Q_UNUSED(apps)
#endif
}



/** Lève le blocage de lancement (retire les clés IFEO) et supprime le fichier d'état. */
void ExamModeFeaturePlugin::removeLaunchPrevention()
{
#if defined(Q_OS_WIN)
	for( const auto& image : m_preventedApps )
	{
		const auto key = QStringLiteral(
			"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%1" ).arg( image );
		QProcess reg;
		reg.start( QStringLiteral("reg"), { QStringLiteral("delete"), key,
			QStringLiteral("/v"), QStringLiteral("Debugger"), QStringLiteral("/f") } );
		reg.waitForFinished( 5000 );
	}
	m_preventedApps.clear();
	QFile::remove( launchPreventionStateFile() );
#endif
}



/** Au démarrage du service : retire un blocage de lancement laissé par un crash. */
void ExamModeFeaturePlugin::cleanupStaleLaunchPrevention()
{
#if defined(Q_OS_WIN)
	QFile state( launchPreventionStateFile() );
	if( state.open( QIODevice::ReadOnly | QIODevice::Text ) == false )
	{
		return;
	}
	m_preventedApps = QString::fromUtf8( state.readAll() )
		.split( QLatin1Char('\n'), Qt::SkipEmptyParts );
	state.close();
	if( m_preventedApps.isEmpty() == false )
	{
		vInfo() << "ExamMode: nettoyage d'un blocage de lancement résiduel -" << m_preventedApps.size() << "app(s)";
		removeLaunchPrevention();
	}
	else
	{
		QFile::remove( launchPreventionStateFile() );
	}
#endif
}


#if defined(Q_OS_WIN)

QString ExamModeFeaturePlugin::pacFilePath()
{
	return QStringLiteral("C:/ProgramData/Veyon/exam.pac");
}



QString ExamModeFeaturePlugin::siteFilterMarkerFile()
{
	return QStringLiteral("C:/ProgramData/Veyon/exam-sitefilter.on");
}



/** Écrit un PAC : liste blanche (allow) ou liste noire (block), robuste au DoH. */
void ExamModeFeaturePlugin::writePacFile( const QStringList& sites, const QString& mode )
{
	QStringList items;
	for( const auto& s : sites )
	{
		const auto h = s.trimmed().toLower();
		if( h.isEmpty() == false )
		{
			items.append( QStringLiteral("\"%1\"").arg( QString(h).replace( QLatin1Char('"'), QString() ) ) );
		}
	}
	const bool allow = ( mode == QStringLiteral("allow") );
	// site listé -> "PROXY 127.0.0.1:1" (mort = bloqué) ou "DIRECT" selon le mode
	const auto listedResult = allow ? QStringLiteral("DIRECT") : QStringLiteral("PROXY 127.0.0.1:1");
	const auto otherResult  = allow ? QStringLiteral("PROXY 127.0.0.1:1") : QStringLiteral("DIRECT");

	const auto pac = QStringLiteral(
		"function FindProxyForURL(url, host) {\n"
		"  host = ('' + host).toLowerCase();\n"
		"  if (host === 'localhost' || host === '127.0.0.1' || isPlainHostName(host)) return 'DIRECT';\n"
		"  var list = [%1];\n"
		"  var inList = false;\n"
		"  for (var i = 0; i < list.length; i++) {\n"
		"    var d = list[i];\n"
		"    if (host === d || host === 'www.' + d || dnsDomainIs(host, '.' + d)) { inList = true; break; }\n"
		"  }\n"
		"  return inList ? '%2' : '%3';\n"
		"}\n" ).arg( items.join( QStringLiteral(", ") ), listedResult, otherResult );

	QDir().mkpath( QFileInfo( pacFilePath() ).absolutePath() );
	QFile f( pacFilePath() );
	if( f.open( QIODevice::WriteOnly | QIODevice::Text ) )
	{
		f.write( pac.toUtf8() );
		f.close();
	}
}



/** reg add helper (échec silencieux). */
void ExamModeFeaturePlugin::regSet( const QString& key, const QString& name, const QString& type, const QString& data )
{
	QProcess p;
	p.start( QStringLiteral("reg"), { QStringLiteral("add"), key, QStringLiteral("/v"), name,
		QStringLiteral("/t"), type, QStringLiteral("/d"), data, QStringLiteral("/f") } );
	p.waitForFinished( 5000 );
}



void ExamModeFeaturePlugin::regDelete( const QString& key, const QString& name )
{
	QProcess p;
	p.start( QStringLiteral("reg"), { QStringLiteral("delete"), key, QStringLiteral("/v"), name, QStringLiteral("/f") } );
	p.waitForFinished( 5000 );
}



/**
 * Filtrage des sites sous Windows : PAC (data: pour Chrome/Edge, file:// pour
 * Firefox) via politiques HKLM + désactivation DoH (sinon le navigateur pourrait
 * résoudre hors du contrôle). Couvre liste noire ET blanche.
 */
void ExamModeFeaturePlugin::applyWindowsSiteFiltering( const QStringList& sites, const QString& mode )
{
	removeWindowsSiteFiltering();		// état propre

	if( sites.isEmpty() && mode != QStringLiteral("allow") )
	{
		return;		// liste noire vide = rien à filtrer
	}

	writePacFile( sites, mode );

	// PAC en data: URL (Chrome/Edge restreignent file://) + fichier pour Firefox.
	QFile f( pacFilePath() );
	QByteArray pac;
	if( f.open( QIODevice::ReadOnly ) ) { pac = f.readAll(); f.close(); }
	const auto dataUrl = QStringLiteral("data:application/x-ns-proxy-autoconfig;base64,")
		+ QString::fromLatin1( pac.toBase64() );
	const auto fileUrl = QStringLiteral("file:///") + pacFilePath();

	const QStringList chromiumKeys = {
		QStringLiteral("HKLM\\SOFTWARE\\Policies\\Google\\Chrome"),
		QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge"),
	};
	for( const auto& key : chromiumKeys )
	{
		regSet( key, QStringLiteral("ProxyMode"), QStringLiteral("REG_SZ"), QStringLiteral("pac_script") );
		regSet( key, QStringLiteral("ProxyPacUrl"), QStringLiteral("REG_SZ"), dataUrl );
		regSet( key, QStringLiteral("DnsOverHttpsMode"), QStringLiteral("REG_SZ"), QStringLiteral("off") );
	}

	// Firefox : proxy autoConfig + DoH off (via ADMX → registre)
	regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
			QStringLiteral("Mode"), QStringLiteral("REG_SZ"), QStringLiteral("autoConfig") );
	regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
			QStringLiteral("AutoConfigURL"), QStringLiteral("REG_SZ"), fileUrl );
	regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\Proxy"),
			QStringLiteral("Locked"), QStringLiteral("REG_DWORD"), QStringLiteral("1") );
	regSet( QStringLiteral("HKLM\\SOFTWARE\\Policies\\Mozilla\\Firefox\\DNSOverHTTPS"),
			QStringLiteral("Enabled"), QStringLiteral("REG_DWORD"), QStringLiteral("0") );

	// marqueur pour nettoyage anti-crash au démarrage
	QFile marker( siteFilterMarkerFile() );
	if( marker.open( QIODevice::WriteOnly ) ) { marker.write( "1" ); marker.close(); }
}



void ExamModeFeaturePlugin::removeWindowsSiteFiltering()
{
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

	QFile::remove( pacFilePath() );
	QFile::remove( siteFilterMarkerFile() );
}

#endif
