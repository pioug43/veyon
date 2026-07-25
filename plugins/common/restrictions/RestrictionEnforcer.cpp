/*
 * RestrictionEnforcer.cpp - implementation of RestrictionEnforcer class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir RestrictionEnforcer.h pour la description générale. Les backends sont
 * repris de la logique éprouvée du plugin exammode (transactions registre,
 * section délimitée du fichier hosts, PAC + politiques navigateur) en les
 * paramétrant par un scope.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
#include <QSet>

#include "ExamModeWindowsNative.h"
#include "RestrictionEnforcer.h"
#include "VeyonCore.h"

namespace
{

QString stateDirectory()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon");
#else
	return QStringLiteral("/var/lib/veyon");
#endif
}


#if defined(Q_OS_WIN)

// Les états transactionnels sur disque ne concernent que les backends Windows
// (IFEO, politiques navigateur) : sous Linux/macOS, la section délimitée du
// fichier hosts est à elle seule son propre état, restaurable sans sauvegarde.

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
	if( file.commit() == false )
	{
		return false;
	}
	// L'état de restauration ne doit être lisible/modifiable que par SYSTEM et les
	// administrateurs : il décrit précisément ce qu'il faut remettre pour lever
	// les restrictions.
	return ExamModeWindowsNative::restrictFileToAdministratorsAndSystem( path );
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


/**
 * Restaure une valeur de registre à son état antérieur. Si « expected » est
 * fourni et que la valeur courante en diffère, un tiers a modifié la politique
 * depuis notre écriture : on n'écrase pas son réglage.
 */
bool restoreRegistryValue( const QString& key, const QString& name, const QJsonObject& previous,
						   const QJsonObject& expected )
{
	if( expected.isEmpty() == false )
	{
		const auto current = ExamModeWindowsNative::readRegistryValue( key, name );
		if( current[QStringLiteral("ok")].toBool() == false )
		{
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
			// politique modifiée par un tiers : on la laisse telle quelle
			return true;
		}
	}
	if( previous[QStringLiteral("exists")].toBool() )
	{
		return ExamModeWindowsNative::restoreRegistryValue( key, name, previous );
	}
	return previous[QStringLiteral("ok")].toBool() &&
		   ExamModeWindowsNative::deleteRegistryValue( key, name );
}

#endif

}



RestrictionEnforcer::RestrictionEnforcer( const Scope& scope ) :
	m_scope( scope )
{
}



RestrictionEnforcer::~RestrictionEnforcer()
{
	if( m_active )
	{
		remove();
	}
}



QString RestrictionEnforcer::stateFile( const QString& suffix ) const
{
	return QStringLiteral("%1/%2-%3").arg( stateDirectory(), m_scope.id, suffix );
}



QString RestrictionEnforcer::hostsMarkerBegin() const
{
	return QStringLiteral("# >>> Veyon %1 >>>").arg( m_scope.logPrefix );
}



QString RestrictionEnforcer::hostsMarkerEnd() const
{
	return QStringLiteral("# <<< Veyon %1 <<<").arg( m_scope.logPrefix );
}



QString RestrictionEnforcer::hostsFilePath()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/Windows/System32/drivers/etc/hosts");
#else
	return QStringLiteral("/etc/hosts");
#endif
}



QString RestrictionEnforcer::windowsImageName( const QString& executable )
{
	auto image = executable.trimmed().section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
	static const QRegularExpression InvalidImageCharacters( QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])") );
	if( image.isEmpty() || image == QStringLiteral(".") || image == QStringLiteral("..") ||
		InvalidImageCharacters.match( image ).hasMatch() )
	{
		return {};
	}
	if( image.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) == false )
	{
		image += QStringLiteral(".exe");
	}
	return image;
}



bool RestrictionEnforcer::apply( const Policy& policy )
{
	m_terminateApplications = policy.terminateApplications;
	m_backendResults.clear();

	// La terminaison périodique n'a pas d'état système à poser : elle est
	// toujours disponible dès lors que le composant est privilégié.
	m_backendResults.insert( QStringLiteral("process"),
		policy.terminateApplications.isEmpty() ? QStringLiteral("IDLE") : QStringLiteral("APPLIED") );

	bool success = true;

	if( applyLaunchPrevention( policy.preventLaunchApplications ) == false )
	{
		success = false;
	}

	if( applySiteFiltering( policy ) == false )
	{
		success = false;
	}

	m_active = true;

	terminateBlockedProcesses();

	return success;
}



void RestrictionEnforcer::remove()
{
	removeSiteFiltering();
	removeLaunchPrevention();

	m_terminateApplications.clear();
	m_backendResults.clear();
	m_active = false;
}



void RestrictionEnforcer::cleanupResidualState()
{
	if( removeHostsSection() )
	{
		vInfo() << m_scope.logPrefix << ": section hosts résiduelle retirée";
		flushDnsCache();
	}

	if( QFile::exists( stateFile( QStringLiteral("launch-state.json") ) ) )
	{
		vInfo() << m_scope.logPrefix << ": prévention de lancement résiduelle retirée";
		removeLaunchPrevention();
	}

#if defined(Q_OS_WIN)
	if( QFile::exists( stateFile( QStringLiteral("site-state.json") ) ) )
	{
		vInfo() << m_scope.logPrefix << ": filtrage de sites résiduel retiré";
		removeWindowsSiteFiltering();
	}
#endif
}



void RestrictionEnforcer::terminateBlockedProcesses() const
{
	if( m_terminateApplications.isEmpty() )
	{
		return;
	}

#if defined(Q_OS_WIN)
	QStringList images;
	for( const auto& application : m_terminateApplications )
	{
		const auto image = windowsImageName( application );
		if( image.isEmpty() == false )
		{
			images.append( image );
		}
	}
	const auto result = ExamModeWindowsNative::terminateProcesses( images );
	if( result.failed.isEmpty() == false )
	{
		vWarning() << m_scope.logPrefix << ": échec de terminaison" << result.failed;
	}
#else
	for( const auto& application : m_terminateApplications )
	{
		// Nom d'exécutable exact (-x) : ne jamais cibler la ligne de commande (-f),
		// qui tuerait un processus dont un argument contient ce mot.
		auto process = application.trimmed().section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
		if( process.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) )
		{
			process.chop( 4 );
		}
		if( process.isEmpty() == false )
		{
			QProcess::startDetached( QStringLiteral("pkill"),
				{ QStringLiteral("-x"), QStringLiteral("--"), process } );
		}
	}
#endif
}



/**
 * Empêche le LANCEMENT des exécutables interdits. Windows : IFEO
 * (HKLM\…\Image File Execution Options\<exe>\Debugger = systray.exe, no-op
 * silencieux). La liste appliquée est persistée pour permettre la restauration
 * après un arrêt brutal. Linux/macOS : non supporté par ce moteur (la
 * prévention noyau fanotify reste propre au mode examen) — on s'appuie sur la
 * terminaison périodique et on le signale honnêtement dans l'état.
 */
bool RestrictionEnforcer::applyLaunchPrevention( const QStringList& applications )
{
#if defined(Q_OS_WIN)
	removeLaunchPrevention();		// restaure d'abord un éventuel état antérieur
	if( QFile::exists( stateFile( QStringLiteral("launch-state.json") ) ) )
	{
		vWarning() << m_scope.logPrefix << ": restauration IFEO précédente incomplète; blocage refusé";
		m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("FAILED") );
		return false;
	}
	if( applications.isEmpty() )
	{
		m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("IDLE") );
		return true;
	}

	QJsonArray entries;
	for( const auto& application : applications )
	{
		const auto image = windowsImageName( application );
		if( image.isEmpty() )
		{
			continue;
		}
		const auto key = QStringLiteral(
			"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%1" ).arg( image );
		const auto previous = ExamModeWindowsNative::readRegistryValue( key, QStringLiteral("Debugger") );
		if( previous[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << m_scope.logPrefix << ": sauvegarde IFEO impossible; transaction refusée" << key;
			m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("FAILED") );
			return false;
		}
		entries.append( QJsonObject{
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
		m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("IDLE") );
		return true;
	}

	if( writeJsonFile( stateFile( QStringLiteral("launch-state.json") ), QJsonObject{
		{ QStringLiteral("version"), 1 }, { QStringLiteral("entries"), entries } } ) == false )
	{
		vWarning() << m_scope.logPrefix << ": impossible de sauvegarder l'état IFEO; blocage annulé";
		m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("FAILED") );
		return false;
	}

	bool success = true;
	for( const auto& value : std::as_const(entries) )
	{
		const auto entry = value.toObject();
		success = ExamModeWindowsNative::setRegistryValue( entry[QStringLiteral("key")].toString(),
			QStringLiteral("Debugger"), QStringLiteral("REG_SZ"),
			QStringLiteral("C:\\Windows\\System32\\systray.exe") ) && success;
	}
	if( success == false )
	{
		vWarning() << m_scope.logPrefix << ": écriture IFEO partielle; restauration";
		removeLaunchPrevention();
	}
	m_backendResults.insert( QStringLiteral("launchPrevention"),
		success ? QStringLiteral("APPLIED") : QStringLiteral("FAILED") );
	return success;
#else
	if( applications.isEmpty() )
	{
		m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("IDLE") );
		return true;
	}
	// Honnêteté du rapport : la prévention de lancement n'est pas disponible ;
	// seule la terminaison périodique s'applique. Ce n'est pas un échec bloquant
	// pour un mode « cours », mais l'appelant doit le savoir.
	vInfo() << m_scope.logPrefix << ": prévention de lancement non supportée sur cette plateforme;"
			<< "repli sur la terminaison périodique";
	m_backendResults.insert( QStringLiteral("launchPrevention"), QStringLiteral("UNSUPPORTED") );
	return true;
#endif
}



void RestrictionEnforcer::removeLaunchPrevention()
{
#if defined(Q_OS_WIN)
	const auto path = stateFile( QStringLiteral("launch-state.json") );
	if( QFile::exists( path ) == false )
	{
		return;
	}
	const auto entries = readJsonFile( path ).value( QStringLiteral("entries") ).toArray();
	if( entries.isEmpty() )
	{
		vWarning() << m_scope.logPrefix << ": état IFEO illisible; fichier conservé pour diagnostic";
		return;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		success = restoreRegistryValue( entry[QStringLiteral("key")].toString(), QStringLiteral("Debugger"),
			entry[QStringLiteral("previous")].toObject(), entry[QStringLiteral("expected")].toObject() ) && success;
	}
	if( success )
	{
		QFile::remove( path );
	}
	else
	{
		vWarning() << m_scope.logPrefix << ": restauration IFEO incomplète; nouvelle tentative au prochain démarrage";
	}
#endif
}



bool RestrictionEnforcer::applySiteFiltering( const Policy& policy )
{
#if defined(Q_OS_WIN)
	return applyWindowsSiteFiltering( policy );
#else
	// Le fichier hosts ne sait faire qu'une liste NOIRE de domaines exacts. Toute
	// sémantique de liste blanche ou d'expression régulière est refusée plutôt que
	// silencieusement ignorée (un blocage cru appliqué serait pire qu'un refus).
	for( const auto& rule : policy.urlRules )
	{
		if( rule.regularExpression )
		{
			vWarning() << m_scope.logPrefix << ": les règles URL par expression régulière exigent le backend PAC"
					   << "(Windows); filtrage web refusé sur cette plateforme";
			removeHostsSection();
			flushDnsCache();
			m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("UNSUPPORTED") );
			return false;
		}
	}
	if( policy.defaultUrlAction == RestrictionRules::RuleAction::Block )
	{
		vWarning() << m_scope.logPrefix << ": l'action par défaut « block » (liste blanche) exige le backend PAC"
				   << "(Windows); filtrage web refusé sur cette plateforme";
		removeHostsSection();
		flushDnsCache();
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("UNSUPPORTED") );
		return false;
	}
	return applyHostsBlocking( policy.blockedDomains );
#endif
}



void RestrictionEnforcer::removeSiteFiltering()
{
#if defined(Q_OS_WIN)
	removeWindowsSiteFiltering();
#else
	if( removeHostsSection() )
	{
		flushDnsCache();
	}
#endif
}



bool RestrictionEnforcer::applyHostsBlocking( const QStringList& domains )
{
	removeHostsSection();		// repart d'un fichier propre (idempotent)

	if( domains.isEmpty() )
	{
		flushDnsCache();
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("IDLE") );
		return true;
	}

	// Limite intrinsèque du fichier hosts : pas de jokers. Seuls le domaine et son
	// sous-domaine « www » sont couverts ; les autres sous-domaines et l'accès par
	// IP directe restent joignables. Suffisant pour un usage « confort de cours »,
	// pas pour un examen (utiliser exammode et son backend nftables).
	vInfo() << m_scope.logPrefix << ": backend hosts — domaine et www.<domaine> uniquement;"
			<< "les autres sous-domaines et l'accès par IP restent joignables";

	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadOnly | QIODevice::Text ) == false )
	{
		vWarning() << m_scope.logPrefix << ": impossible d'ouvrir" << hostsFilePath();
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
		return false;
	}

	QByteArray content = file.readAll();
	const auto permissions = file.permissions();
	file.close();

	if( content.isEmpty() == false && content.endsWith( '\n' ) == false )
	{
		content.append( '\n' );
	}

	content.append( '\n' ).append( hostsMarkerBegin().toUtf8() ).append( '\n' );
	for( const auto& domain : domains )
	{
		const auto line = QStringLiteral("127.0.0.1\t%1\n127.0.0.1\twww.%1\n::1\t%1\n::1\twww.%1\n").arg( domain );
		content.append( line.toUtf8() );
	}
	content.append( hostsMarkerEnd().toUtf8() ).append( '\n' );

	QSaveFile output( hostsFilePath() );
	if( output.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		output.setPermissions( permissions ) == false ||
		output.write( content ) != content.size() || output.commit() == false )
	{
		vWarning() << m_scope.logPrefix << ": échec de l'écriture atomique de" << hostsFilePath();
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
		return false;
	}

	flushDnsCache();
	m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("APPLIED") );
	return true;
}



/** Retire la section délimitée de ce scope. Sûr même si elle est absente. */
bool RestrictionEnforcer::removeHostsSection()
{
	QFile file( hostsFilePath() );
	if( file.open( QIODevice::ReadOnly | QIODevice::Text ) == false )
	{
		return false;
	}

	const auto raw = QString::fromUtf8( file.readAll() );
	const auto permissions = file.permissions();
	file.close();

	const auto markerBegin = hostsMarkerBegin();
	if( raw.contains( markerBegin ) == false )
	{
		return false;		// rien à faire
	}

	const auto markerEnd = hostsMarkerEnd();
	const auto lines = raw.split( QLatin1Char('\n') );
	QString rebuilt;
	bool inSection = false;
	for( const auto& line : lines )
	{
		if( line.startsWith( markerBegin ) )
		{
			inSection = true;
			continue;
		}
		if( line.startsWith( markerEnd ) )
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

	QSaveFile output( hostsFilePath() );
	const auto content = rebuilt.toUtf8();
	if( output.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		output.setPermissions( permissions ) == false ||
		output.write( content ) != content.size() || output.commit() == false )
	{
		vWarning() << m_scope.logPrefix << ": impossible de nettoyer" << hostsFilePath();
		return false;
	}
	return true;
}



void RestrictionEnforcer::flushDnsCache() const
{
#if defined(Q_OS_WIN)
	QProcess::startDetached( QStringLiteral("ipconfig"), { QStringLiteral("/flushdns") } );
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
	QProcess::startDetached( QStringLiteral("dscacheutil"), { QStringLiteral("-flushcache") } );
	QProcess::startDetached( QStringLiteral("killall"), { QStringLiteral("-HUP"), QStringLiteral("mDNSResponder") } );
#else
	QProcess::startDetached( QStringLiteral("resolvectl"), { QStringLiteral("flush-caches") } );
#endif
}


#if defined(Q_OS_WIN)

QString RestrictionEnforcer::pacFilePath() const
{
	return stateFile( QStringLiteral("proxy.pac") );
}



bool RestrictionEnforcer::writePacFile( const Policy& policy ) const
{
	const auto pac = RestrictionRules::buildPac( policy.urlRules, policy.defaultUrlAction );

	QDir().mkpath( QFileInfo( pacFilePath() ).absolutePath() );
	QSaveFile file( pacFilePath() );
	if( file.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		file.write( pac ) != pac.size() || file.commit() == false )
	{
		vWarning() << m_scope.logPrefix << ": impossible d'écrire le fichier PAC" << pacFilePath();
		return false;
	}
	return true;
}



/**
 * Filtrage web sous Windows : PAC (data: pour Chrome/Edge, file:// pour Firefox)
 * via les politiques HKLM + désactivation de DNS-over-HTTPS (sinon le navigateur
 * résoudrait hors de notre contrôle). Couvre liste noire ET liste blanche.
 * Transactionnel : l'état antérieur de chaque valeur est persisté avant écriture.
 */
bool RestrictionEnforcer::applyWindowsSiteFiltering( const Policy& policy )
{
	removeWindowsSiteFiltering();		// restaure d'abord l'état antérieur
	if( QFile::exists( stateFile( QStringLiteral("site-state.json") ) ) )
	{
		vWarning() << m_scope.logPrefix << ": restauration précédente incomplète; filtrage refusé";
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
		return false;
	}

	if( policy.urlRules.isEmpty() && policy.defaultUrlAction == RestrictionRules::RuleAction::Allow )
	{
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("IDLE") );
		return true;
	}

	const auto pac = RestrictionRules::buildPac( policy.urlRules, policy.defaultUrlAction );
	// Type explicite obligatoire : avec QT_USE_QSTRINGBUILDER, « auto » déduirait
	// un QStringBuilder qui ne conserve que des RÉFÉRENCES vers les temporaires
	// concaténés — celles-ci pendouilleraient dès la fin de l'instruction.
	const QString dataUrl = QStringLiteral("data:application/x-ns-proxy-autoconfig;base64,")
		+ QString::fromLatin1( pac.toBase64() );
	const QString fileUrl = QStringLiteral("file:///") + pacFilePath();

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
	for( const auto& policyValue : policyValues )
	{
		QString type = QStringLiteral("REG_SZ");
		QString data;
		if( policyValue.second == QStringLiteral("ProxyMode") ) { data = QStringLiteral("pac_script"); }
		else if( policyValue.second == QStringLiteral("ProxyPacUrl") ) { data = dataUrl; }
		else if( policyValue.second == QStringLiteral("DnsOverHttpsMode") ) { data = QStringLiteral("off"); }
		else if( policyValue.second == QStringLiteral("Mode") ) { data = QStringLiteral("autoConfig"); }
		else if( policyValue.second == QStringLiteral("AutoConfigURL") ) { data = fileUrl; }
		else if( policyValue.second == QStringLiteral("Locked") ) { type = QStringLiteral("REG_DWORD"); data = QStringLiteral("1"); }
		else if( policyValue.second == QStringLiteral("Enabled") ) { type = QStringLiteral("REG_DWORD"); data = QStringLiteral("0"); }

		const auto previous = ExamModeWindowsNative::readRegistryValue( policyValue.first, policyValue.second );
		if( previous[QStringLiteral("ok")].toBool() == false )
		{
			vWarning() << m_scope.logPrefix << ": sauvegarde de politique navigateur impossible"
					   << policyValue.first << policyValue.second;
			m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
			return false;
		}
		entries.append( QJsonObject{
			{ QStringLiteral("key"), policyValue.first }, { QStringLiteral("name"), policyValue.second },
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
		oldPac.close();
	}

	if( writeJsonFile( stateFile( QStringLiteral("site-state.json") ), QJsonObject{
		{ QStringLiteral("version"), 1 }, { QStringLiteral("entries"), entries },
		{ QStringLiteral("pacExisted"), previousPacExists },
		{ QStringLiteral("previousPac"), QString::fromLatin1( previousPac.toBase64() ) },
	} ) == false )
	{
		vWarning() << m_scope.logPrefix << ": impossible de sauvegarder les politiques navigateur";
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
		return false;
	}

	if( writePacFile( policy ) == false )
	{
		removeWindowsSiteFiltering();
		m_backendResults.insert( QStringLiteral("sites"), QStringLiteral("FAILED") );
		return false;
	}

	bool success = true;
	for( const auto& value : std::as_const(entries) )
	{
		const auto entry = value.toObject();
		const auto expected = entry[QStringLiteral("expected")].toObject();
		success = ExamModeWindowsNative::setRegistryValue( entry[QStringLiteral("key")].toString(),
			entry[QStringLiteral("name")].toString(), expected[QStringLiteral("type")].toString(),
			expected[QStringLiteral("data")].toString() ) && success;
	}

	if( success == false )
	{
		vWarning() << m_scope.logPrefix << ": écriture des politiques navigateur partielle; restauration";
		removeWindowsSiteFiltering();
	}

	m_backendResults.insert( QStringLiteral("sites"),
		success ? QStringLiteral("APPLIED") : QStringLiteral("FAILED") );
	return success;
}



void RestrictionEnforcer::removeWindowsSiteFiltering()
{
	const auto path = stateFile( QStringLiteral("site-state.json") );
	if( QFile::exists( path ) == false )
	{
		return;
	}

	const auto state = readJsonFile( path );
	const auto entries = state.value( QStringLiteral("entries") ).toArray();
	if( entries.isEmpty() )
	{
		vWarning() << m_scope.logPrefix << ": sauvegarde des politiques illisible; restauration différée";
		return;
	}

	bool success = true;
	for( const auto& value : entries )
	{
		const auto entry = value.toObject();
		success = restoreRegistryValue( entry[QStringLiteral("key")].toString(),
			entry[QStringLiteral("name")].toString(), entry[QStringLiteral("previous")].toObject(),
			entry[QStringLiteral("expected")].toObject() ) && success;
	}

	if( state.value( QStringLiteral("pacExisted") ).toBool() )
	{
		QSaveFile file( pacFilePath() );
		const auto contents = QByteArray::fromBase64(
			state.value( QStringLiteral("previousPac") ).toString().toLatin1() );
		success = file.open( QIODevice::WriteOnly ) && file.write( contents ) == contents.size() &&
				  file.commit() && success;
	}
	else
	{
		success = ( QFile::exists( pacFilePath() ) == false || QFile::remove( pacFilePath() ) ) && success;
	}

	if( success )
	{
		QFile::remove( path );
	}
	else
	{
		vWarning() << m_scope.logPrefix << ": restauration des politiques navigateur incomplète";
	}
}

#endif
