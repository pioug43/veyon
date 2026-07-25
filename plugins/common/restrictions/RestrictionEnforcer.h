/*
 * RestrictionEnforcer.h - reusable application/website restriction backends
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Moteur d'application des restrictions logicielles et web, extrait de la
 * logique éprouvée du plugin exammode et paramétré par un « scope » : chaque
 * consommateur (exammode, courserestrictions) possède ses propres marqueurs de
 * section hosts et ses propres fichiers d'état, afin qu'un consommateur ne
 * puisse jamais nettoyer l'état d'un autre.
 *
 * Backends :
 *  - terminaison de processus : Windows (Toolhelp, sans shell) / Linux (pkill -x)
 *  - prévention de lancement  : Windows (IFEO, transactionnel). Linux : non
 *    supporté ici — la prévention noyau (fanotify) reste propre à exammode ;
 *    le mode cours se contente de la terminaison périodique.
 *  - filtrage web             : Windows (PAC + politiques navigateur + DoH off,
 *    transactionnel) / Linux, macOS (section délimitée du fichier hosts)
 *
 * Toutes les mutations sont transactionnelles : l'état antérieur est persisté
 * avant modification, ce qui permet de restaurer le poste même après un crash
 * (cf. cleanupResidualState(), à appeler au démarrage du composant).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include "RestrictionRules.h"

class RestrictionEnforcer
{
public:
	/** Identité du consommateur : préfixe de journalisation et espace de noms
	 *  des états sur disque. Deux scopes distincts n'interfèrent jamais. */
	struct Scope
	{
		QString id;			// identifiant technique, ex. "courserestrictions"
		QString logPrefix;	// préfixe des messages de journal, ex. "CourseRestrictions"
	};

	/** Politique à appliquer. Les listes sont supposées déjà normalisées par
	 *  RestrictionRules (normalizeApplications / normalizeDomains / …). */
	struct Policy
	{
		QStringList terminateApplications;
		QStringList preventLaunchApplications;
		QStringList blockedDomains;		// backend hosts (Linux/macOS)
		QList<RestrictionRules::UrlRule> urlRules;		// backend PAC (Windows)
		RestrictionRules::RuleAction defaultUrlAction{RestrictionRules::RuleAction::Allow};
	};

	explicit RestrictionEnforcer( const Scope& scope );
	~RestrictionEnforcer();

	RestrictionEnforcer( const RestrictionEnforcer& ) = delete;
	RestrictionEnforcer& operator=( const RestrictionEnforcer& ) = delete;

	/**
	 * Applique la politique. Retourne true si TOUS les backends demandés ont
	 * abouti. En cas d'échec partiel, les backends appliqués restent en place et
	 * backendResults() détaille le résultat par backend (le poste n'est jamais
	 * laissé dans un état non restaurable).
	 */
	bool apply( const Policy& policy );

	/** Lève toutes les restrictions posées par ce scope. Idempotent. */
	void remove();

	/**
	 * À appeler au démarrage du composant : retire un état résiduel laissé par
	 * un arrêt brutal (section hosts, clés IFEO, politiques navigateur).
	 */
	void cleanupResidualState();

	/** Passe périodique : termine les processus interdits encore actifs. */
	void terminateBlockedProcesses() const;

	bool isActive() const { return m_active; }

	/** Détail par backend ("process", "launchPrevention", "sites") pour le
	 *  rapport d'état remonté à l'appelant. */
	QVariantMap backendResults() const { return m_backendResults; }

	/** Nom d'image Windows normalisé ("firefox" → "firefox.exe"), vide si invalide. */
	static QString windowsImageName( const QString& executable );

private:
	bool applyLaunchPrevention( const QStringList& applications );
	void removeLaunchPrevention();

	bool applySiteFiltering( const Policy& policy );
	void removeSiteFiltering();

	bool applyHostsBlocking( const QStringList& domains );
	bool removeHostsSection();
	void flushDnsCache() const;

	QString stateFile( const QString& suffix ) const;
	QString hostsMarkerBegin() const;
	QString hostsMarkerEnd() const;

#if defined(Q_OS_WIN)
	bool applyWindowsSiteFiltering( const Policy& policy );
	void removeWindowsSiteFiltering();
	bool writePacFile( const Policy& policy ) const;
	QString pacFilePath() const;
#endif

	static QString hostsFilePath();

	const Scope m_scope;

	bool m_active{false};
	QStringList m_terminateApplications{};
	QVariantMap m_backendResults{};
};
