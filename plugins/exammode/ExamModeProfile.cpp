/*
 * ExamModeProfile.cpp - validation helpers for exam mode profiles
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#include <QCryptographicHash>
#include <QHash>
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
	for( const auto& original : domains )
	{
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


QByteArray ExamModeProfile::buildPac( const QStringList& normalizedDomains, SiteMode mode )
{
	QJsonArray domains;
	for( const auto& domain : normalizedDomains )
	{
		domains.append( domain );
	}
	const auto json = QString::fromUtf8( QJsonDocument( domains ).toJson( QJsonDocument::Compact ) );
	const auto listedResult = mode == SiteMode::Allow ? QStringLiteral("DIRECT") :
												 QStringLiteral("PROXY 127.0.0.1:1");
	const auto otherResult = mode == SiteMode::Allow ? QStringLiteral("PROXY 127.0.0.1:1") :
											 QStringLiteral("DIRECT");

	return QStringLiteral(
		"function FindProxyForURL(url, host) {\n"
		"  host = ('' + host).toLowerCase();\n"
		"  if (host === 'localhost' || host === '127.0.0.1' || host === '::1') return 'DIRECT';\n"
		"  var list = %1;\n"
		"  for (var i = 0; i < list.length; ++i) {\n"
		"    var d = list[i];\n"
		"    if (host === d || dnsDomainIs(host, '.' + d)) return '%2';\n"
		"  }\n"
		"  return '%3';\n"
		"}\n" ).arg( json, listedResult, otherResult ).toUtf8();
}


ExamModeProfile::ProcessPolicy ExamModeProfile::resolveProcessRules( const QVariantList& rules,
														 const QString& platform, QStringList* rejected )
{
	QHash<QString, QString> terminate;
	QHash<QString, QString> prevent;
	QSet<QString> allowed;
	const auto normalizedPlatform = platform.trimmed().toLower();

	for( const auto& value : rules )
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
	for( const auto& value : rules )
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
										RuleAction defaultUrlAction, int leaseSeconds )
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
	const QJsonObject canonical{
		{ QStringLiteral("profileId"), profileId }, { QStringLiteral("revision"), revision },
		{ QStringLiteral("leaseSeconds"), leaseSeconds },
		{ QStringLiteral("terminateApplications"), terminate }, { QStringLiteral("preventLaunchApplications"), prevent },
		{ QStringLiteral("urlRules"), urls },
		{ QStringLiteral("defaultUrlAction"), defaultUrlAction == RuleAction::Allow ? QStringLiteral("allow") : QStringLiteral("block") },
	};
	return QString::fromLatin1( QCryptographicHash::hash(
		QJsonDocument( canonical ).toJson( QJsonDocument::Compact ), QCryptographicHash::Sha256 ).toHex() );
}
