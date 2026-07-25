/*
 * DeviceControlPlugin.h - declaration of DeviceControlPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Contrôles matériels d'un poste, activables indépendamment pendant un cours :
 * couper le son, bloquer le stockage USB, empêcher l'impression, désactiver la
 * webcam. Chacun est indépendant : couper le son ne bloque pas les clés USB.
 *
 * Ce sont volontairement des ACTIONS et non des « modes » : la console
 * n'autorise qu'un mode actif à la fois et arrête tous les autres avant d'en
 * démarrer un — bloquer l'USB aurait alors rétabli le son, et au passage levé
 * le verrouillage d'écran ou les restrictions de cours. La levée passe par la
 * fonctionnalité « Rétablir » ou par la Web API.
 *
 * Le mode examen dispose de son propre blocage USB, à bail et signé. Il reste
 * PRIORITAIRE : tant qu'un examen est actif, le blocage USB de ce plugin est
 * refusé, faute de quoi les deux écriraient dans les mêmes valeurs de registre
 * et se restaureraient de travers.
 *
 * Toute mutation système est transactionnelle : l'état antérieur est persisté
 * avant modification et restauré au démarrage du service si un arrêt brutal a
 * laissé un blocage en place.
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
#include <QThreadPool>

#include "FeatureProviderInterface.h"

class QTimer;

class DeviceControlPlugin : public QObject, FeatureProviderInterface, PluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.DeviceControl")
	Q_INTERFACES(PluginInterface FeatureProviderInterface)
public:
	enum class FeatureCommand
	{
		Apply,
		Release,
		DeviceStatus,
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		Device,			// QString : audio | usb | printing | webcam
		Status,			// QString : APPLIED | REJECTED | UNSUPPORTED | IDLE
		ErrorCode,
		Timestamp,		// qint64 : epoch ms
	};
	Q_ENUM(Argument)

	explicit DeviceControlPlugin( QObject* parent = nullptr );
	~DeviceControlPlugin() override;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("7c4e9a10-5f83-4b26-9d17-3ea6c0b85d42") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("DeviceControl");
	}

	QString description() const override
	{
		return tr( "Mute audio and block USB storage, printing or webcams" );
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

	/**
	 * Neutralisé volontairement : la console appelle stopFeature() sur TOUTES
	 * les fonctionnalités dès qu'un mode démarre. Sans cela, verrouiller les
	 * écrans rétablirait le son et débloquerait les clés USB. La levée se fait
	 * par la fonctionnalité « Rétablir » ou par la Web API.
	 */
	bool stopFeature( VeyonMasterInterface& master, const Feature& feature,
					  const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool handleFeatureMessage( VeyonServerInterface& server,
							   const MessageContext& messageContext,
							   const FeatureMessage& message ) override;

	bool handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message ) override;

	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;

	bool isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const override;

	QVariantMap featureStatus( Feature::Uid featureUid,
							   ComputerControlInterface::Pointer computerControlInterface ) const override;

private:
	/** Les quatre contrôles, désignés par une chaîne stable dans le protocole. */
	enum class Device
	{
		Audio,
		UsbStorage,
		Printing,
		Webcam,
	};

	static QString deviceName( Device device );
	Feature::Uid featureOfDevice( Device device ) const;
	Device deviceOfFeature( Feature::Uid featureUid, bool* found ) const;

	bool applyDevice( Device device, QString* errorCode );
	void releaseDevice( Device device );
	void releaseAll();
	void cleanupResidualState();

	bool applyAudioMute( bool muted ) const;
	bool applyUsbBlocking( bool blocked, QString* errorCode );
	bool applyPrintingBlocking( bool blocked );
	bool applyWebcamBlocking( bool blocked );

	static bool examModeActive();
	void checkExamModeArbitration();
	static bool isEndpointComponent();
	static QString printServiceName();

	FeatureMessage statusMessage( Device device ) const;

	const Feature m_muteAudioFeature;
	const Feature m_blockUsbFeature;
	const Feature m_blockPrintingFeature;
	const Feature m_blockWebcamFeature;
	const Feature m_releaseAllFeature;
	const FeatureList m_features;

	// état du service d'impression avant blocage : un service déjà arrêté ne
	// doit pas être démarré par la levée
	bool m_printServiceWasRunning{false};

	// contrôle périodique de la priorité du mode examen sur le blocage USB
	static constexpr int ArbitrationIntervalMs = 5000;
	QTimer* m_arbitrationTimer{nullptr};

	// Pool POSSÉDÉ, et non le pool global, pour les transitions de service :
	// son destructeur attend la fin des tâches en cours, et il s'exécute avant
	// que le cœur Veyon ne soit démonté. Avec le pool global, une transition
	// lancée depuis notre destructeur pouvait s'exécuter après la disparition
	// de VeyonCore::instance() et faire planter le service à l'arrêt.
	// Un seul fil : les transitions d'un même service doivent être sérialisées.
	QThreadPool m_servicePool;

	// côté poste : contrôles actuellement posés
	QSet<int> m_activeDevices;
	QHash<int, QString> m_deviceStatus;

	// côté maître : dernier état connu par poste
	mutable QMutex m_remoteStatusMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_remoteStatuses;
	QSet<const ComputerControlInterface*> m_trackedRemoteInterfaces;
};
