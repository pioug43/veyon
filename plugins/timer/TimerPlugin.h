/*
 * TimerPlugin.h - declaration of TimerPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Minuteur de séance : affiche un compte à rebours sur les postes (« fin de
 * l'exercice dans 10 min ») et, à l'expiration, peut verrouiller les écrans ou
 * simplement laisser le décompte à zéro.
 *
 * Le verrouillage à l'expiration réutilise la fonctionnalité ScreenLock plutôt
 * que d'en réimplémenter une : le maître l'enclenche lui-même à l'échéance, ce
 * qui laisse un seul propriétaire du verrouillage.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QPointer>

#include "FeatureProviderInterface.h"

class QTimer;
class TimerStudentWidget;

class TimerPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.Timer")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		StartTimer,
		StopTimer,
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		Seconds,		// int : durée du compte à rebours
		Label,			// QString : intitulé affiché au-dessus du décompte
		Fullscreen,		// bool : plein écran plutôt qu'un bandeau discret
		ExpiryAction,	// int : ce que le maître déclenche à l'échéance
	};
	Q_ENUM(Argument)

	/** Ce que le maître déclenche à l'expiration. */
	enum class ExpiryAction
	{
		None,
		Lock,
	};
	Q_ENUM(ExpiryAction)

	explicit TimerPlugin( QObject* parent = nullptr );
	~TimerPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("bd6f2c81-40a7-4e95-b13c-72e0d5a86f39") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("Timer");
	}

	QString description() const override
	{
		return tr( "Display a countdown on all computers" );
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

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;

	bool isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const override;

private:
	static constexpr int MinimumSeconds = 10;
	static constexpr int MaximumSeconds = 6 * 60 * 60;
	static constexpr int MaximumLabelLength = 120;

	void triggerExpiryAction();

	const Feature m_timerFeature;
	const FeatureList m_features;

	// côté maître : échéance en cours, pour déclencher l'action à l'expiration
	QTimer* m_expiryTimer{nullptr};
	ExpiryAction m_expiryAction{ExpiryAction::None};
	ComputerControlInterfaceList m_expiryTargets{};

	// côté poste
	bool m_timerActiveOnServer{false};
};
