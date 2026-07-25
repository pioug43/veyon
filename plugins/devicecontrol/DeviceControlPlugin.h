/*
 * DeviceControlPlugin.h - declaration of DeviceControlPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Contrôles matériels d'un poste, activables indépendamment pendant un cours :
 * couper le son, bloquer le stockage USB, empêcher l'impression, désactiver la
 * webcam. Chacun est un mode séparé — l'enseignant coupe le son sans pour
 * autant bloquer les clés USB.
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

#include "FeatureProviderInterface.h"

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
	static bool isEndpointComponent();
	static QString printServiceName();

	FeatureMessage statusMessage( Device device ) const;

	const Feature m_muteAudioFeature;
	const Feature m_blockUsbFeature;
	const Feature m_blockPrintingFeature;
	const Feature m_blockWebcamFeature;
	const FeatureList m_features;

	// côté poste : contrôles actuellement posés
	QSet<int> m_activeDevices;
	QHash<int, QString> m_deviceStatus;

	// côté maître : dernier état connu par poste
	mutable QMutex m_remoteStatusMutex;
	QHash<const ComputerControlInterface*, QVariantMap> m_remoteStatuses;
	QSet<const ComputerControlInterface*> m_trackedRemoteInterfaces;
};
