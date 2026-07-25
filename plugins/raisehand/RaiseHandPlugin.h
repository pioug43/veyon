/*
 * RaiseHandPlugin.h - declaration of RaiseHandPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * « Lever la main » : pendant que l'enseignant active ce mode, chaque poste
 * affiche un petit bouton flottant permettant à l'élève de signaler qu'il a
 * besoin d'aide. L'enseignant voit la file des demandes, dans l'ordre
 * d'arrivée, et les traite une à une.
 *
 * Deux fonctionnalités sont déclarées :
 *  - RaiseHand : le MODE (bouton de la console). Actif sur un poste tant que
 *    le bouton flottant y est affiché.
 *  - HandRaised : fonctionnalité META, déclarée active par le poste
 *    uniquement pendant qu'une demande est en attente. Elle n'a pas d'action
 *    propre : elle sert à faire apparaître une icône sur la vignette du poste
 *    concerné, la console dessinant déjà une icône par fonctionnalité active.
 *
 * La chaîne de relais maître → serveur → worker (session utilisateur) puis
 * worker → serveur → maître reprend le schéma des plugins chat et survey.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QPointer>

#include "FeatureProviderInterface.h"
#include "MessageContext.h"

class RaiseHandMasterWindow;

class RaiseHandPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.RaiseHand")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		StartRaiseHand,		// maître → poste : afficher le bouton flottant
		StopRaiseHand,		// maître → poste : retirer le bouton flottant
		HandRaised,			// poste → maître : l'élève demande de l'aide
		HandLowered,		// poste → maître : l'élève retire sa demande
		ClearHand,			// maître → poste : demande traitée, bouton réarmé
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		Timestamp,			// QString ISO-8601 (UTC)
	};
	Q_ENUM(Argument)

	explicit RaiseHandPlugin( QObject* parent = nullptr );
	~RaiseHandPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("48d2c6f1-7b09-4e35-9a8c-2f5b1d047e63") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("RaiseHand");
	}

	QString description() const override
	{
		return tr( "Let users ask for help during a lesson" );
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

	bool startFeature( VeyonMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool stopFeature( VeyonMasterInterface& master, const Feature& feature,
					  const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessageFromWorker( VeyonServerInterface& server,
										 const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;

	bool isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const override;

	QVariantMap featureStatus( Feature::Uid featureUid,
							   ComputerControlInterface::Pointer computerControlInterface ) const override;

	/** Marque la demande d'un poste comme traitée (réarme son bouton). */
	void clearRequest( const ComputerControlInterface::Pointer& computerControlInterface );

Q_SIGNALS:
	void handRaised( ComputerControlInterface::Pointer computerControlInterface,
					 const QDateTime& timestamp );
	void handLowered( ComputerControlInterface::Pointer computerControlInterface );

private:
	const Feature m_raiseHandFeature;
	const Feature m_handRaisedFeature;
	const FeatureList m_features;

	// côté maître : file des demandes en attente
	QPointer<RaiseHandMasterWindow> m_masterWindow;

	// côté serveur (poste) : maître à l'origine du mode, pour lui relayer les
	// demandes saisies dans la session utilisateur (cf. chat, survey)
	MessageContext m_masterContext{};
	bool m_modeActive{false};
	bool m_handRaised{false};
};
