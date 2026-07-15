/*
 * ExamModeProfile.cpp - validation helpers for exam mode profiles
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include "ExamModeProfile.h"

namespace
{

QStringList sortedValues( const QSet<QString>& values )
{
	QStringList result;
	for( const auto& value : values )
	{
		result.append( value );
	}
	result.sort( Qt::CaseInsensitive );
	return result;
}

}


QString ExamModeProfile::siteModeName( SiteMode mode )
{
	return mode == SiteMode::Allow ? QStringLiteral("allow") : QStringLiteral("block");
}


ExamModeProfile::SiteMode ExamModeProfile::siteModeFromString( const QString& value, bool* valid )
{
	const auto normalized = value.trimmed().toLower();
	const bool isValid = normalized.isEmpty() || normalized == QStringLiteral("block") ||
						 normalized == QStringLiteral("allow");
	if( valid )
	{
		*valid = isValid;
	}
	return normalized == QStringLiteral("allow") ? SiteMode::Allow : SiteMode::Block;
}


ExamModeProfile::NetworkBackend ExamModeProfile::networkBackendFromString( const QString& value, bool* valid )
{
	const auto normalized = value.trimmed().toLower();
	const bool isValid = normalized.isEmpty() || normalized == QStringLiteral("hosts") ||
						 normalized == QStringLiteral("firewall");
	if( valid )
	{
		*valid = isValid;
	}
	return normalized == QStringLiteral("firewall") ? NetworkBackend::Firewall : NetworkBackend::Hosts;
}


QStringList ExamModeProfile::strictModeApplications( const QString& platform )
{
	const auto p = platform.trimmed().toLower();
	if( p == QStringLiteral("windows") )
	{
		return {
			// shells / interpréteurs
			QStringLiteral("cmd.exe"), QStringLiteral("powershell.exe"), QStringLiteral("powershell_ise.exe"),
			QStringLiteral("pwsh.exe"), QStringLiteral("wscript.exe"), QStringLiteral("cscript.exe"),
			QStringLiteral("mshta.exe"), QStringLiteral("python.exe"), QStringLiteral("pythonw.exe"),
			QStringLiteral("node.exe"), QStringLiteral("perl.exe"), QStringLiteral("ruby.exe"),
			QStringLiteral("wsl.exe"), QStringLiteral("wslhost.exe"), QStringLiteral("bash.exe"),
			// exécution / contournement de politiques
			QStringLiteral("rundll32.exe"), QStringLiteral("regsvr32.exe"), QStringLiteral("installutil.exe"),
			QStringLiteral("msbuild.exe"), QStringLiteral("conhost.exe"), QStringLiteral("openconsole.exe"),
			QStringLiteral("wt.exe"),
			// arrêt d'ExamMode / édition de l'état
			QStringLiteral("taskmgr.exe"), QStringLiteral("procexp.exe"), QStringLiteral("procexp64.exe"),
			QStringLiteral("regedit.exe"), QStringLiteral("reg.exe"), QStringLiteral("sc.exe"),
			QStringLiteral("mmc.exe"), QStringLiteral("services.exe"), QStringLiteral("taskkill.exe"),
		};
	}
	if( p == QStringLiteral("macos") )
	{
		return {
			QStringLiteral("Terminal"), QStringLiteral("iTerm2"), QStringLiteral("iTerm"),
			QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("sh"), QStringLiteral("fish"),
			QStringLiteral("python3"), QStringLiteral("python"), QStringLiteral("node"),
			QStringLiteral("perl"), QStringLiteral("ruby"), QStringLiteral("osascript"),
			QStringLiteral("Activity Monitor"),
		};
	}
	// linux (défaut). NB : « sh » et « dash » sont volontairement exclus — trop
	// de scripts système/session les utilisent ; les tuer romprait la session.
	// On bloque les shells interactifs et les interpréteurs.
	return {
		// shells interactifs
		QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish"),
		// interpréteurs
		QStringLiteral("python3"), QStringLiteral("python"), QStringLiteral("node"), QStringLiteral("perl"),
		QStringLiteral("ruby"), QStringLiteral("lua"), QStringLiteral("php"),
		// émulateurs de terminal
		QStringLiteral("xterm"), QStringLiteral("gnome-terminal"), QStringLiteral("gnome-terminal-"),
		QStringLiteral("konsole"), QStringLiteral("xfce4-terminal"), QStringLiteral("x-terminal-emul"),
		QStringLiteral("terminator"), QStringLiteral("tilix"), QStringLiteral("kitty"), QStringLiteral("alacritty"),
		// surveillance / arrêt de processus
		QStringLiteral("gnome-system-mon"), QStringLiteral("ksysguard"), QStringLiteral("xkill"),
	};
}


QStringList ExamModeProfile::normalizeNetworks( const QStringList& networks, QStringList* rejected )
{
	// dédup + ordre déterministe via QMap (clé = forme canonique).
	QMap<QString, QString> normalized;
	int seen = 0;
	for( const auto& original : networks )
	{
		if( ++seen > MaxNetworks )
		{
			if( rejected ) { rejected->append( QStringLiteral("<network cap exceeded>") ); }
			break;
		}
		auto candidate = original.trimmed();
		candidate.remove( QLatin1Char('\r') );
		candidate.remove( QLatin1Char('\n') );
		if( candidate.isEmpty() )
		{
			continue;
		}
		// une IP nue est acceptée comme /32 (IPv4) ou /128 (IPv6).
		if( candidate.contains( QLatin1Char('/') ) == false )
		{
			const QHostAddress bare( candidate );
			if( bare.isNull() == false )
			{
				candidate += bare.protocol() == QAbstractSocket::IPv6Protocol ?
					QStringLiteral("/128") : QStringLiteral("/32");
			}
		}
		const auto subnet = QHostAddress::parseSubnet( candidate );
		if( subnet.first.isNull() || subnet.second < 0 )
		{
			if( rejected ) { rejected->append( original ); }
			continue;
		}
		// forme canonique : adistanceréseau/préfixe (évite les doublons 10.0.0.5/24 vs 10.0.0.0/24).
		const auto canonical = QStringLiteral("%1/%2").arg( subnet.first.toString() ).arg( subnet.second );
		normalized.insert( canonical, canonical );
	}
	return normalized.values();
}


QByteArray ExamModeProfile::buildNftablesRuleset( const QStringList& allowedNetworks,
												  const QStringList& dnsServers,
												  const QStringList& supervisionNetworks )
{
	// Politique egress « drop » : seule sort la loopback, les connexions déjà
	// établies, le DNS vers les résolveurs autorisés et les réseaux autorisés.
	// Bloque donc IP directe hors liste, DoH vers résolveur arbitraire, VPN vers
	// endpoint arbitraire, DNS alternatif — contournements impossibles au niveau
	// navigateur. Table dédiée pour un flush sûr et idempotent.
	QStringList allowLines;
	const auto emitCidr = [&allowLines]( const QString& cidr ) {
		const auto parts = cidr.split( QLatin1Char('/') );
		if( parts.size() != 2 ) { return; }
		const QHostAddress addr( parts.first() );
		const auto family = addr.protocol() == QAbstractSocket::IPv6Protocol ?
			QStringLiteral("ip6 daddr") : QStringLiteral("ip daddr");
		allowLines.append( QStringLiteral("    %1 %2 accept").arg( family, cidr ) );
	};
	for( const auto& cidr : ExamModeProfile::normalizeNetworks( allowedNetworks ) ) { emitCidr( cidr ); }
	for( const auto& cidr : ExamModeProfile::normalizeNetworks( supervisionNetworks ) ) { emitCidr( cidr ); }

	QStringList dnsLines;
	for( const auto& server : ExamModeProfile::normalizeNetworks( dnsServers ) )
	{
		const auto parts = server.split( QLatin1Char('/') );
		if( parts.size() != 2 ) { continue; }
		const QHostAddress addr( parts.first() );
		const auto family = addr.protocol() == QAbstractSocket::IPv6Protocol ?
			QStringLiteral("ip6 daddr") : QStringLiteral("ip daddr");
		dnsLines.append( QStringLiteral("    %1 %2 udp dport 53 accept").arg( family, parts.first() ) );
		dnsLines.append( QStringLiteral("    %1 %2 tcp dport 53 accept").arg( family, parts.first() ) );
	}

	return QStringLiteral(
		"#!/usr/sbin/nft -f\n"
		"table inet veyon_exammode\n"
		"delete table inet veyon_exammode\n"
		"table inet veyon_exammode {\n"
		"  chain output {\n"
		"    type filter hook output priority 0; policy drop;\n"
		"    oif \"lo\" accept\n"
		"    ct state established,related accept\n"
		"%1\n"
		"%2\n"
		"  }\n"
		"}\n" )
		.arg( dnsLines.join( QLatin1Char('\n') ), allowLines.join( QLatin1Char('\n') ) )
		.toUtf8();
}


QStringList ExamModeProfile::normalizeApplications( const QStringList& applications )
{
	// déduplication insensible à la casse et ordre déterministe (clés QMap
	// triées) : indispensable à la stabilité du digest de profil.
	QMap<QString, QString> normalized;
	for( auto application : applications )
	{
		application = application.trimmed();
		application.remove( QLatin1Char('\r') );
		application.remove( QLatin1Char('\n') );
		if( application.isEmpty() == false )
		{
			normalized.insert( application.toLower(), application );
		}
	}
	return normalized.values();
}


QStringList ExamModeProfile::normalizeDomains( const QStringList& domains, QStringList* rejected )
{
	QSet<QString> normalized;
	int seen = 0;
	for( const auto& original : domains )
	{
		if( ++seen > MaxDomains )
		{
			if( rejected ) { rejected->append( QStringLiteral("<domain cap exceeded>") ); }
			break;
		}
		auto candidate = original.trimmed().toLower();
		while( candidate.startsWith( QStringLiteral("*.") ) || candidate.startsWith( QLatin1Char('.') ) )
		{
			candidate.remove( 0, candidate.startsWith( QStringLiteral("*.") ) ? 2 : 1 );
		}

		const bool hasScheme = candidate.contains( QStringLiteral( "://" ) );
		const QUrl url( hasScheme ? candidate : QStringLiteral("https://") + candidate );
		const auto path = url.path();
		auto host = url.host().toLower();
		if( host.endsWith( QLatin1Char('.') ) )
		{
			host.chop( 1 );
		}

		const auto ace = QString::fromLatin1( QUrl::toAce( host ) );
		const bool valid = url.isValid() && ace.isEmpty() == false &&
			url.userInfo().isEmpty() && url.port( -1 ) == -1 &&
			( path.isEmpty() || path == QStringLiteral("/") ) &&
			url.hasQuery() == false && url.hasFragment() == false &&
			ace != QStringLiteral("localhost") && ace.contains( QLatin1Char(' ') ) == false;

		if( valid )
		{
			normalized.insert( ace );
		}
		else if( rejected )
		{
			rejected->append( original );
		}
	}
	return sortedValues( normalized );
}


ExamModeProfile::ProcessPolicy ExamModeProfile::resolveProcessRules( const QVariantList& rules,
														 const QString& platform, QStringList* rejected )
{
	QHash<QString, QString> terminate;
	QHash<QString, QString> prevent;
	QSet<QString> allowed;
	const auto normalizedPlatform = platform.trimmed().toLower();

	auto capped = rules;
	if( capped.size() > MaxProcessRules )
	{
		if( rejected )
		{
			rejected->append( QStringLiteral("<%1 process rule(s) over cap>").arg( capped.size() - MaxProcessRules ) );
		}
		capped = capped.mid( 0, MaxProcessRules );
	}

	for( const auto& value : capped )
	{
		const auto rule = value.toMap();
		if( rule.isEmpty() || rule.value( QStringLiteral("active"), true ).toBool() == false )
		{
			continue;
		}
		const auto rulePlatform = rule.value( QStringLiteral("os"), QStringLiteral("all") ).toString().trimmed().toLower();
		if( rulePlatform != QStringLiteral("all") && rulePlatform != normalizedPlatform )
		{
			continue;
		}
		auto executable = rule.value( QStringLiteral("executable") ).toString().trimmed();
		executable.remove( QLatin1Char('\r') );
		executable.remove( QLatin1Char('\n') );
		const auto action = rule.value( QStringLiteral("action"), QStringLiteral("block") ).toString().trimmed().toLower();
		if( executable.isEmpty() || ( action != QStringLiteral("block") && action != QStringLiteral("allow") ) )
		{
			if( rejected ) { rejected->append( executable.isEmpty() ? QStringLiteral("<missing executable>") : executable ); }
			continue;
		}

		const auto key = executable.toLower();
		if( action == QStringLiteral("allow") )
		{
			allowed.insert( key );
			terminate.remove( key );
			prevent.remove( key );
			continue;
		}
		if( allowed.contains( key ) )
		{
			continue;
		}
		const bool strongKill = rule.value( QStringLiteral("strongKill"), true ).toBool();
		if( rule.value( QStringLiteral("terminateRunning"), strongKill ).toBool() )
		{
			terminate.insert( key, executable );
		}
		if( rule.value( QStringLiteral("preventLaunch"), strongKill ).toBool() )
		{
			prevent.insert( key, executable );
		}
	}

	ProcessPolicy policy;
	policy.terminateApplications = normalizeApplications( terminate.values() );
	policy.preventLaunchApplications = normalizeApplications( prevent.values() );
	return policy;
}


QList<ExamModeProfile::UrlRule> ExamModeProfile::normalizeUrlRules( const QVariantList& rules, QStringList* rejected )
{
	QList<UrlRule> normalized;
	auto capped = rules;
	if( capped.size() > MaxUrlRules )
	{
		if( rejected )
		{
			rejected->append( QStringLiteral("<%1 URL rule(s) over cap>").arg( capped.size() - MaxUrlRules ) );
		}
		capped = capped.mid( 0, MaxUrlRules );
	}
	for( const auto& value : capped )
	{
		const auto rule = value.toMap();
		if( rule.isEmpty() || rule.value( QStringLiteral("active"), true ).toBool() == false )
		{
			continue;
		}
		const auto expression = rule.value( QStringLiteral("expression") ).toString().trimmed();
		const auto actionName = rule.value( QStringLiteral("action"), QStringLiteral("block") ).toString().trimmed().toLower();
		const bool isRegex = rule.value( QStringLiteral("regex"), false ).toBool();
		const bool validRegex = isRegex == false || QRegularExpression( expression ).isValid();
		const bool valid = expression.isEmpty() == false && expression.size() <= 2048 &&
			expression.contains( QLatin1Char('\r') ) == false && expression.contains( QLatin1Char('\n') ) == false &&
			validRegex && ( actionName == QStringLiteral("allow") || actionName == QStringLiteral("block") );
		if( valid == false )
		{
			if( rejected ) { rejected->append( expression.isEmpty() ? QStringLiteral("<missing expression>") : expression ); }
			continue;
		}
		normalized.append( UrlRule{
			actionName == QStringLiteral("allow") ? RuleAction::Allow : RuleAction::Block,
			expression,
			isRegex,
		} );
	}
	return normalized;
}


QByteArray ExamModeProfile::buildPac( const QList<UrlRule>& rules, RuleAction defaultAction )
{
	QJsonArray serializedRules;
	for( const auto& rule : rules )
	{
		serializedRules.append( QJsonObject{
			{ QStringLiteral("action"), rule.action == RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") },
			{ QStringLiteral("expression"), rule.expression },
			{ QStringLiteral("regex"), rule.regularExpression },
		} );
	}
	const auto json = QString::fromUtf8( QJsonDocument( serializedRules ).toJson( QJsonDocument::Compact ) );
	const auto defaultResult = defaultAction == RuleAction::Allow ? QStringLiteral("DIRECT") :
		QStringLiteral("PROXY 127.0.0.1:1");

	return QStringLiteral(
		"function FindProxyForURL(url, host) {\n"
		"  host = ('' + host).toLowerCase();\n"
		"  if (host === 'localhost' || host === '127.0.0.1' || host === '::1') return 'DIRECT';\n"
		"  var rules = %1;\n"
		"  for (var i = 0; i < rules.length; ++i) {\n"
		"    var r = rules[i], match = false;\n"
		"    try {\n"
		"      match = r.regex ? (new RegExp(r.expression)).test(url) :\n"
		"        (shExpMatch(url, r.expression) || shExpMatch(host, r.expression) ||\n"
		"         host === r.expression || dnsDomainIs(host, '.' + r.expression));\n"
		"    } catch (e) { match = false; }\n"
		"    if (match) return r.action === 'allow' ? 'DIRECT' : 'PROXY 127.0.0.1:1';\n"
		"  }\n"
		"  return '%2';\n"
		"}\n" ).arg( json, defaultResult ).toUtf8();
}


QString ExamModeProfile::profileDigest( const QString& profileId, qint64 revision,
										const ProcessPolicy& processPolicy, const QList<UrlRule>& urlRules,
										RuleAction defaultUrlAction, int leaseSeconds,
										const NetworkPolicy& networkPolicy )
{
	QJsonArray terminate;
	for( const auto& application : processPolicy.terminateApplications ) { terminate.append( application ); }
	QJsonArray prevent;
	for( const auto& application : processPolicy.preventLaunchApplications ) { prevent.append( application ); }
	QJsonArray urls;
	for( const auto& rule : urlRules )
	{
		urls.append( QJsonObject{
			{ QStringLiteral("action"), rule.action == RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") },
			{ QStringLiteral("expression"), rule.expression }, { QStringLiteral("regex"), rule.regularExpression },
		} );
	}
	const auto toArray = []( const QStringList& values ) {
		QJsonArray array;
		for( const auto& value : values ) { array.append( value ); }
		return array;
	};
	const QJsonObject canonical{
		{ QStringLiteral("profileId"), profileId }, { QStringLiteral("revision"), revision },
		{ QStringLiteral("leaseSeconds"), leaseSeconds },
		{ QStringLiteral("terminateApplications"), terminate }, { QStringLiteral("preventLaunchApplications"), prevent },
		{ QStringLiteral("urlRules"), urls },
		{ QStringLiteral("defaultUrlAction"), defaultUrlAction == RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") },
		// La politique réseau fait partie de l'identité du profil : sans elle, une
		// modification de l'allow-list egress (backend firewall) serait indétectable
		// à révision constante et non couverte par le contrôle d'empreinte.
		{ QStringLiteral("networkBackend"),
			networkPolicy.backend == NetworkBackend::Firewall ? QStringLiteral("firewall") : QStringLiteral("hosts") },
		{ QStringLiteral("allowedNetworks"), toArray( networkPolicy.allowedNetworks ) },
		{ QStringLiteral("dnsServers"), toArray( networkPolicy.dnsServers ) },
		{ QStringLiteral("supervisionNetworks"), toArray( networkPolicy.supervisionNetworks ) },
	};
	return QString::fromLatin1( QCryptographicHash::hash(
		QJsonDocument( canonical ).toJson( QJsonDocument::Compact ), QCryptographicHash::Sha256 ).toHex() );
}
