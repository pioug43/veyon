/*
 * CourseRestrictionsPlugin.h - declaration of CourseRestrictionsPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * « Restrictions de cours » : blocage d'applications et de sites web à la
 * volée pendant un cours.
 *
 * Différences avec le plugin exammode, qui partage les mêmes backends
 * (bibliothèque veyon-restrictions) :
 *  - pas de profil signé, pas de bail, pas de dead-man, pas de nftables : c'est
 *    un mode « confort de cours », levé par un simple Stop ;
 *  - pilotable depuis la console (profils prédéfinis dans la configuration)
 *    ET par la Web API générique ;
 *  - le mode examen est PRIORITAIRE : tant qu'un examen est actif sur le poste,
 *    les restrictions de cours sont refusées puis retirées, afin que les deux
 *    plugins ne se disputent jamais les mêmes clés de registre / le même
 *    fichier hosts.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>

#include "ConfigurationPagePluginInterface.h"
#include "CourseRestrictionsConfiguration.h"
#include "CourseRestrictionsProfile.h"
#include "FeatureProviderInterface.h"
#include "RestrictionEnforcer.h"

class QTimer;

class CourseRestrictionsPlugin : public QObject, FeatureProviderInterface, PluginInterface,
								 ConfigurationPagePluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.CourseRestrictions")
	Q_INTERFACES(PluginInterface
					 FeatureProviderInterface
						 ConfigurationPagePluginInterface)
public:
	// Arguments de la fonctionnalité (Q_ENUM : sérialisés en camelCase par
	// argToString → clés JSON « profileName », « blockedApplications »…).
	enum class Argument
	{
		ProfileId,				// QString : identifiant du profil appliqué
		ProfileName,			// QString : libellé affiché/journalisé
		BlockedApplications,	// QStringList : exécutables interdits (forme simple)
		Websites,				// QStringList : domaines (forme simple)
		WebsiteMode,			// QString : "block" (liste noire) | "allow" (liste blanche)
		ProcessRules,			// QVariantList : règles typées par OS (forme structurée)
		UrlRules,				// QVariantList : règles URL ordonnées (forme structurée)
		UrlDefaultAction,		// QString : "allow" (défaut) | "block"
		Status,					// QString : APPLIED | DEGRADED | REJECTED | IDLE
		ErrorCode,
		ErrorMessage,
		BackendResults,			// QVariantMap : résultat par backend
		AppliedAt,				// QString ISO-8601
		Timestamp,				// qint64 : epoch ms
	};
	Q_ENUM(Argument)

	explicit CourseRestrictionsPlugin( QObject* parent = nullptr );
	~CourseRestrictionsPlugin() override;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("6d1f7b48-25c0-4f3a-9b1e-0c7a5d2e8f41") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("CourseRestrictions");
	}

	QString description() const override
	{
		return tr( "Block applications and websites during a lesson" );
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

	/**
	 * Les postes ne connaissent que la fonctionnalité parente : c'est elle qu'ils
	 * déclarent active. Sans ce mappage, Veyon considérerait qu'un poste dont le
	 * « mode désigné » est un profil n'a jamais appliqué ce mode et le lui
	 * ré-enverrait à chaque changement d'état de connexion.
	 */
	Feature::Uid metaFeature( Feature::Uid featureUid ) const override;

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						 const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool startFeature( VeyonMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool stopFeature( VeyonMasterInterface& master, const Feature& feature,
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

	ConfigurationPage* createConfigurationPage() override;

private:
	enum class FeatureCommand
	{
		StartRestrictions,
		StopRestrictions,
		RestrictionsStatus,
	};

	// intervalle de la passe de terminaison des processus interdits. Identique à
	// celui du mode examen : suffisamment court pour que la fenêtre d'usage d'une
	// application interdite reste très brève.
	static constexpr int EnforceIntervalMs = 1500;
	// intervalle de contrôle de la priorité du mode examen (cf. yieldToExamMode)
	static constexpr int ArbitrationIntervalMs = 5000;

	static constexpr int MaximumProfileNameLength = 120;

	void updateFeatures();
	QList<CourseRestrictionsProfile> configuredProfiles() const;
	static QVariantMap profileArguments( const CourseRestrictionsProfile& profile );

	bool startEnforcement( const FeatureMessage& message );
	void stopEnforcement();
	void enforceTick();
	/** Le mode examen est prioritaire : s'il est actif, on lève nos restrictions. */
	bool examModeActive() const;
	void checkExamModeArbitration();

	void setStatus( const QString& status, const QString& errorCode = {}, const QString& errorMessage = {} );
	FeatureMessage statusMessage() const;
	static bool isEndpointComponent();

	CourseRestrictionsConfiguration m_configuration;

	const Feature m_courseRestrictionsFeature;
	FeatureList m_profileFeatures{};
	FeatureList m_features{};

	RestrictionEnforcer m_enforcer;

	QTimer* m_enforceTimer{nullptr};
	QTimer* m_arbitrationTimer{nullptr};

	bool m_active{false};
	QString m_profileId{};
	QString m_profileName{};
	QString m_status{QStringLiteral("IDLE")};
	QString m_errorCode{};
	QString m_errorMessage{};
	QString m_appliedAt{};
	QVariantMap m_backendResults{};
	// Version monotone du statut : n'émettre le rapport que lorsqu'il a
	// réellement changé pour un client donné. Émettre à chaque trame injecterait
	// un message dans le flux RFB et le désynchroniserait (cf. exammode).
	quint64 m_statusVersion{1};

	// côté maître : dernier statut connu par poste, exposé via featureStatus()
	mutable QMutex m_remoteStatusMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_remoteStatuses;
	QSet<const ComputerControlInterface*> m_trackedRemoteInterfaces;
};
