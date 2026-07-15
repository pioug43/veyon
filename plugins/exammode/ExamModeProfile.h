/*
 * ExamModeProfile.h - validation helpers for exam mode profiles
 *
 * Copyright (c) 2026 Pierrick Belledent
 */

#pragma once

#include <QList>
#include <QStringList>
#include <QVariantList>

namespace ExamModeProfile
{

enum class SiteMode
{
	Block,
	Allow,
};

enum class RuleAction
{
	Block,
	Allow,
};

struct ProcessPolicy
{
	QStringList terminateApplications;
	QStringList preventLaunchApplications;
};

struct UrlRule
{
	RuleAction action{RuleAction::Block};
	QString expression;
	bool regularExpression{false};
};

QString siteModeName( SiteMode mode );
SiteMode siteModeFromString( const QString& value, bool* valid = nullptr );
QStringList normalizeApplications( const QStringList& applications );
QStringList normalizeDomains( const QStringList& domains, QStringList* rejected = nullptr );
ProcessPolicy resolveProcessRules( const QVariantList& rules, const QString& platform,
								  QStringList* rejected = nullptr );
QList<UrlRule> normalizeUrlRules( const QVariantList& rules, QStringList* rejected = nullptr );
QByteArray buildPac( const QStringList& normalizedDomains, SiteMode mode );
QByteArray buildPac( const QList<UrlRule>& rules, RuleAction defaultAction );
QString profileDigest( const QString& profileId, qint64 revision, const ProcessPolicy& processPolicy,
					   const QList<UrlRule>& urlRules, RuleAction defaultUrlAction, int leaseSeconds );

}
