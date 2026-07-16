/*
 * ExamModeFeaturePlugin.h - declaration of ExamModeFeaturePlugin class
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

#pragma once

#include <QJsonObject>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>

#include "ExamModeProfile.h"
#include "ExamModeSession.h"
#include "FeatureProviderInterface.h"

class QTimer;
class ExamModeLinuxExecGuard;

/**
 * Mode examen (Portail VDI) : quand la supervision est activée sur un poste VDI,
 * ce plugin applique le PROFIL EXAMEN — restriction des logiciels (fin des
 * processus interdits) et des sites web (fichier hosts) — côté service Veyon
 * (qui tourne en SYSTEM/root et peut donc appliquer ces restrictions).
 *
 * Le VERROUILLAGE à la sortie de la VM n'est PAS géré ici : il réutilise la
 * fonction native ScreenLock de Veyon, déclenchée par le portail (Web API)
 * lorsqu'il détecte la sortie via les événements Horizon / l'inactivité.
 */
class ExamModeFeaturePlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.ExamMode")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	// Arguments transmis avec la feature (Q_ENUM : sérialisés en camelCase par
	// argToString → clés JSON « blockedApps », « sites », « sitesMode »).
	enum class Argument
	{
		BlockedApps,	// QStringList : noms d'exécutables interdits
		Sites,			// QStringList : domaines
		SitesMode,		// QString : "block" | "allow"
		LeaseSeconds,	// int : durée du bail avant levée automatique des restrictions
		BlockedAppsWindows,
		BlockedAppsLinux,
		BlockedAppsMacos,
		ProcessRules,
		UrlRules,
		UrlDefaultAction,
		ProfileId,
		ProfileRevision,
		ProfileDigest,
		Strict,				// bool : ajoute shells/interpréteurs/outils système au blocage
		NetworkBackend,		// QString : "hosts" (défaut) | "firewall" (Linux, nftables)
		AllowedNetworks,	// QStringList : CIDR autorisés en sortie (backend firewall)
		DnsServers,			// QStringList : résolveurs DNS autorisés (backend firewall)
		SupervisionNetworks,// QStringList : CIDR portail/master toujours autorisés
		SessionId,			// QString : identifiant opaque de session d'examen
		Sequence,			// quint64 : compteur strictement croissant (anti-rejeu)
		IssuedAt,			// qint64 : epoch millisecondes
		ExpiresAt,			// qint64 : epoch millisecondes
		HighSecurity,		// bool : signature et capacités fortes obligatoires
		RequiredCapabilities,// QStringList : contrôles obligatoires déclarés par le portail
		ExternalCapabilities,// QVariantMap : contrôles VDI externes attestés par le portail
		SigningKeyId,		// QString : clé publique Veyon autorisée
		ProfileSignature,	// QByteArray/base64 : RSA-SHA512 sur la charge canonique
		Status,
		ErrorCode,
		ErrorMessage,
		EffectiveDigest,
		Capabilities,
		BackendResults,
		Timestamp,
	};
	Q_ENUM(Argument)

	explicit ExamModeFeaturePlugin( QObject* parent = nullptr );
	~ExamModeFeaturePlugin() override;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("b7e3f1a2-9c44-4d8e-a1b2-3c4d5e6f7a80") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 3 );
	}

	QString name() const override
	{
		return QStringLiteral("ExamMode");
	}

	QString description() const override
	{
		return tr( "Enforce an exam profile (blocked applications and websites) on a computer" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Veyon Community");
	}

	QString copyright() const override
	{
		return QStringLiteral("Pierrick Belledent");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;
	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;
	void sendAsyncFeatureMessages( VeyonServerInterface& server,
							   const MessageContext& messageContext ) override;
	bool isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const override;
	QVariantMap featureStatus( Feature::Uid featureUid,
						   ComputerControlInterface::Pointer computerControlInterface ) const override;

private:
	enum class FeatureCommand
	{
		StartExam,
		StopExam,
		ExamStatus,
	};

	bool startEnforcement( const ExamModeProfile::ProcessPolicy& processPolicy, const QStringList& sites,
						   const QList<ExamModeProfile::UrlRule>& urlRules, const QString& sitesMode,
						   const QString& defaultUrlAction, int leaseSeconds, const QString& profileId,
						   qint64 profileRevision, const QString& expectedDigest,
						   const ExamModeProfile::NetworkPolicy& networkPolicy,
						   const ExamModeSession::Envelope& envelope );
	void stopEnforcement();
	bool stopEnforcement( const ExamModeSession::Envelope& envelope );
	void enforceTick();				// termine périodiquement les processus interdits
	void auditRunningBlockedProcesses() const;	// journalise les processus interdits trouvés actifs
	void killApplication( const QString& executable ) const;
	// Filtrage des sites. Windows : PAC + politiques proxy navigateur + DoH off
	// (liste noire OU blanche, robuste au contournement DoH). Linux : fichier
	// hosts (liste noire seulement ; « allow » averti).
	bool applySiteFiltering( const QStringList& sites, const QString& mode );
	void removeSiteFiltering();
	bool applyHostsBlocking( const QStringList& sites );
	void revertHostsBlocking();
	bool removeHostsSection();		// retire notre section du fichier hosts (sans garde)
	void flushDnsCache() const;		// purge le cache DNS résolveur pour un effet immédiat
	static QString hostsFilePath();
#if defined(Q_OS_LINUX)
	// Backend réseau nftables (egress allow-list, fail-closed) — expérimental,
	// activé uniquement quand networkBackend=firewall. Armé d'un dead-man
	// systemd pour éviter de verrouiller la VM si le processus meurt.
	bool applyLinuxFirewall();
	void removeLinuxFirewall();
	bool armFirewallDeadMan( int seconds ) const;
	void disarmFirewallDeadMan() const;
	static QString firewallStateFile();
	static QString firewallRulesetFile();
#endif
#if defined(Q_OS_WIN)
	bool applyWindowsSiteFiltering( const QStringList& sites, const QString& mode );
	void removeWindowsSiteFiltering();
	bool writePacFile();
	void cleanupLegacyWindowsState();
	// Le backend firewall Windows est refusé tant qu'une implémentation WFP avec
	// transaction dynamique et restauration exacte n'est pas disponible.
	bool applyWindowsFirewall();
	void removeWindowsFirewall();
	static QString windowsFirewallStateFile();
	static QString pacFilePath();
	static QString siteFilterStateFile();
	static QString legacySiteFilterMarkerFile();
	static QString legacyLaunchPreventionStateFile();
	static bool regSet( const QString& key, const QString& name, const QString& type, const QString& data );
	static bool regDelete( const QString& key, const QString& name );
	static QJsonObject regRead( const QString& key, const QString& name );
	static bool regRestore( const QString& key, const QString& name, const QJsonObject& previous,
						const QJsonObject& expected = {} );
#endif

	// Empêchement du LANCEMENT des logiciels interdits (Windows : Image File
	// Execution Options). Le kill périodique reste en complément (instances déjà
	// ouvertes). Sous Linux : sans effet (on s'appuie sur le kill).
	bool applyLaunchPrevention( const QStringList& apps );
	void removeLaunchPrevention();
	void cleanupStaleLaunchPrevention();		// au démarrage : retire un blocage résiduel (crash)
	static QString windowsImageName( const QString& executable );
	static QString launchPreventionStateFile();
	static bool isEndpointComponent();
	ExamModeSession::Envelope envelopeFromMessage( const FeatureMessage& message ) const;
	bool verifyEnvelopeSignature( const ExamModeSession::Envelope& envelope,
							  const QString& operation, const QString& digest ) const;
	QVariantMap localCapabilities( const ExamModeProfile::NetworkPolicy& networkPolicy ) const;
	bool requiredCapabilitiesAvailable( const ExamModeSession::Envelope& envelope,
									const QVariantMap& capabilities, QString* missing ) const;
	void setStatus( const QString& status, const QString& errorCode = {}, const QString& errorMessage = {},
					const QVariantMap& backendResults = {} );
	FeatureMessage statusMessage() const;
	bool verifyEnforcement();
	static QString replayStateFile();
	void loadReplayState();
	void persistReplayState() const;
	static QString activeStateFile();
	void persistActiveState() const;
	void recoverActiveState();

	const Feature m_examModeFeature;
	const FeatureList m_features;

	QTimer* m_timer{nullptr};
	QTimer* m_watchdog{nullptr};	// lève tout si le portail cesse de ré-appliquer (fail-safe)
	QTimer* m_driftTimer{nullptr};
	QTimer* m_fullscreenTimer{nullptr};	// contrôle plein écran VDI rapide (2 s)
	ExamModeLinuxExecGuard* m_execGuard{nullptr};	// prévention de lancement Linux (fanotify)
	bool m_active{false};
	QStringList m_blockedApps{};
	QStringList m_launchPreventedApps{};
	QStringList m_sites{};
	QList<ExamModeProfile::UrlRule> m_urlRules{};
	QString m_sitesMode{QStringLiteral("block")};
	ExamModeProfile::RuleAction m_defaultUrlAction{ExamModeProfile::RuleAction::Allow};
	bool m_hasStructuredUrlRules{false};
	ExamModeProfile::NetworkBackend m_networkBackend{ExamModeProfile::NetworkBackend::Hosts};
	QStringList m_allowedNetworks{};		// CIDR autorisés en sortie (backend firewall)
	QStringList m_dnsServers{};				// résolveurs DNS autorisés (backend firewall)
	QStringList m_supervisionNetworks{};	// CIDR du portail/master à toujours laisser passer
	QString m_profileId{QStringLiteral("legacy")};
	qint64 m_profileRevision{0};
	QString m_profileDigest{};
	int m_leaseSeconds{300};
	QString m_sessionId{};
	quint64 m_lastSequence{0};
	qint64 m_sessionExpiresAtMs{0};
	bool m_highSecurity{false};
	QString m_status{QStringLiteral("IDLE")};
	QString m_errorCode{};
	QString m_errorMessage{};
	QVariantMap m_capabilities{};
	QVariantMap m_backendResults{};
	qint64 m_statusTimestampMs{0};
	// Fin (epoch ms) du délai de grâce accordé à l'étudiant pour passer le client
	// VDI en plein écran à l'activation. 0 = pas de grâce en cours.
	qint64 m_fullscreenGraceUntilMs{0};
	QString m_statusSessionId{};
	quint64 m_statusSequence{0};
	// Version monotone du statut : n'émettre le rapport ExamStatus (par client) que
	// lorsqu'il a réellement changé. Démarre à 1 pour qu'un client fraîchement
	// connecté (propriété absente = 0) reçoive le statut courant exactement une fois.
	quint64 m_statusVersion{1};
	mutable QMutex m_remoteStatusMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_remoteStatuses;
	QSet<const ComputerControlInterface*> m_trackedRemoteInterfaces;
	bool m_hostsModified{false};
	bool m_siteFilteringActive{false};
	QString m_hostsSignature{};		// évite de réappliquer le filtrage réseau si la politique est inchangée
	QString m_appsSignature{};		// évite de réappliquer IFEO si la liste est inchangée

	// délimiteurs de notre section dans le fichier hosts (retrait propre au stop)
	static constexpr auto HostsMarkerBegin = "# >>> Veyon ExamMode >>>";
	static constexpr auto HostsMarkerEnd = "# <<< Veyon ExamMode <<<";

	// Délai laissé à l'étudiant pour se mettre en plein écran à l'activation.
	static constexpr int FullscreenGraceSeconds = 60;
};
