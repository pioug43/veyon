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

enum class NetworkBackend
{
	Hosts,		// Linux/macOS : /etc/hosts (liste noire seulement)
	Firewall,	// Linux : nftables egress allow-list par CIDR (fail-closed)
};

// Bornes anti-abus (DoS / profils dégénérés) sur le nombre d'entrées acceptées.
constexpr int MaxProcessRules = 512;
constexpr int MaxUrlRules = 512;
constexpr int MaxDomains = 4096;
constexpr int MaxNetworks = 512;

QString siteModeName( SiteMode mode );
SiteMode siteModeFromString( const QString& value, bool* valid = nullptr );
NetworkBackend networkBackendFromString( const QString& value, bool* valid = nullptr );
QStringList normalizeApplications( const QStringList& applications );
QStringList normalizeDomains( const QStringList& domains, QStringList* rejected = nullptr );
// Interpréteurs / shells / outils système ajoutés au blocage quand le profil
// active le mode « strict » (opt-in). Défense en profondeur contre l'exécution
// de code arbitraire et l'arrêt d'ExamMode. Dépend de la plateforme.
QStringList strictModeApplications( const QString& platform );
// Valide et normalise une liste de CIDR IPv4/IPv6 (dédup, ordre déterministe).
QStringList normalizeNetworks( const QStringList& networks, QStringList* rejected = nullptr );
ProcessPolicy resolveProcessRules( const QVariantList& rules, const QString& platform,
								  QStringList* rejected = nullptr );
QList<UrlRule> normalizeUrlRules( const QVariantList& rules, QStringList* rejected = nullptr );
QByteArray buildPac( const QStringList& normalizedDomains, SiteMode mode );
QByteArray buildPac( const QList<UrlRule>& rules, RuleAction defaultAction );
// Génère un ruleset nftables egress à politique « drop » : ne laisse sortir que
// la loopback, les connexions établies, le DNS vers les résolveurs autorisés et
// les réseaux explicitement autorisés (CIDR). Utilisé par le backend firewall.
QByteArray buildNftablesRuleset( const QStringList& allowedNetworks, const QStringList& dnsServers,
								 const QStringList& supervisionNetworks );
QString profileDigest( const QString& profileId, qint64 revision, const ProcessPolicy& processPolicy,
					   const QList<UrlRule>& urlRules, RuleAction defaultUrlAction, int leaseSeconds );

}
