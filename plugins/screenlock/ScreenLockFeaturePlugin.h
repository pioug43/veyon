/*
 * ScreenLockFeaturePlugin.h - declaration of ScreenLockFeaturePlugin class
 *
 * Copyright (c) 2017-2026 Tobias Junghans <tobydox@veyon.io>
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

#include "FeatureProviderInterface.h"

class LockWidget;

class ScreenLockFeaturePlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.ScreenLock")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	explicit ScreenLockFeaturePlugin( QObject* parent = nullptr );
	~ScreenLockFeaturePlugin() override;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("2ad98ccb-e9a5-43ef-8c4c-876ac5efbcb1") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 2 );
	}

	QString name() const override
	{
		return QStringLiteral("ScreenLock");
	}

	QString description() const override
	{
		return tr( "Lock screen and input devices of a computer" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Veyon Community");
	}

	QString copyright() const override
	{
		return QStringLiteral("Tobias Junghans");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	/**
	 * Les postes ne connaissent que la fonctionnalité ScreenLock : les deux
	 * entrées de menu ne vivent que côté maître. Sans ce mappage, un poste dont
	 * le « mode désigné » est une de ces entrées serait considéré comme n'ayant
	 * jamais appliqué le mode, et se verrait re-verrouillé à chaque changement
	 * d'état de connexion.
	 */
	Feature::Uid metaFeature( Feature::Uid featureUid ) const override;

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool startFeature( VeyonMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;

private:
	enum class FeatureCommand
	{
		StartLock,
		StopLock
	};

	enum class Argument
	{
		CustomMessage
	};

	static constexpr int MaximumMessageLength = 500;

	const Feature m_screenLockFeature;
	// Deux entrées de menu côté maître : verrouillage simple et verrouillage
	// accompagné d'un message affiché à la place de l'image de verrouillage.
	// La seconde n'a de sens que sur le maître (elle ouvre une saisie), d'où
	// des sous-fonctions Master plutôt qu'un argument caché.
	const Feature m_screenLockDirectFeature;
	const Feature m_screenLockWithMessageFeature;
	const Feature m_lockInputDevicesFeature;
	const FeatureList m_features;

	LockWidget* m_lockWidget;

};
