/*
 * DeviceControlPlugin.cpp - implementation of DeviceControlPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir DeviceControlPlugin.h pour la description générale.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>

#include "ComputerControlInterface.h"
#include "DeviceControlPlugin.h"
#include "PlatformPluginInterface.h"
#include "PlatformServiceFunctions.h"
#include "RegistryPolicyGuard.h"
#include "VeyonServerInterface.h"

#include "DeviceControlWindowsAudio.h"

#if !defined(Q_OS_WIN)
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#endif

namespace
{

/** Fichier d'état du mode examen : sa présence non expirée signifie qu'un
 *  examen est en cours et que le blocage USB de ce plugin doit s'effacer. */
QString examModeActiveStateFile()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("C:/ProgramData/Veyon/exammode-active-state.json");
#else
	return QStringLiteral("/var/lib/veyon/exammode-active-state.json");
#endif
}


#if !defined(Q_OS_WIN)

QString stateDirectory()
{
	return QStringLiteral("/var/lib/veyon");
}


/** Règle udev refusant le stockage USB, posée puis retirée par ce plugin. */
QString usbRuleFile()
{
	return QStringLiteral("/etc/udev/rules.d/99-veyon-devicecontrol-usb.rules");
}


/** Modes d'accès d'origine des périphériques vidéo, pour les restaurer. */
QString webcamStateFile()
{
	return stateDirectory() + QStringLiteral("/devicecontrol-webcam-state.json");
}

#endif

}


DeviceControlPlugin::DeviceControlPlugin( QObject* parent ) :
	QObject( parent ),
	m_muteAudioFeature( QStringLiteral("MuteAudio"),
						Feature::Flag::Mode | Feature::Flag::AllComponents,
						Feature::Uid( "e1b7d340-92af-4c65-8d13-6a0f5b2c7e94" ),
						Feature::Uid(),
						tr( "Mute audio" ), tr( "Restore audio" ),
						tr( "Mute the sound output of all computers." ),
						QStringLiteral(":/core/media-playback-stop.png") ),
	m_blockUsbFeature( QStringLiteral("BlockUsbStorage"),
					   Feature::Flag::Mode | Feature::Flag::AllComponents,
					   Feature::Uid( "4f8a1c07-63be-4d92-a5f1-8c204e7b93d6" ),
					   Feature::Uid(),
					   tr( "Block USB storage" ), tr( "Allow USB storage" ),
					   tr( "Prevent the use of USB flash drives and other removable storage." ),
					   QStringLiteral(":/core/edit-delete.png") ),
	m_blockPrintingFeature( QStringLiteral("BlockPrinting"),
							Feature::Flag::Mode | Feature::Flag::AllComponents,
							Feature::Uid( "9d05e2b8-71c4-4a3f-b628-0e5f1a94c37b" ),
							Feature::Uid(),
							tr( "Block printing" ), tr( "Allow printing" ),
							tr( "Stop the printing service so that nothing can be printed." ),
							QStringLiteral(":/core/edit-delete.png") ),
	m_blockWebcamFeature( QStringLiteral("BlockWebcam"),
						  Feature::Flag::Mode | Feature::Flag::AllComponents,
						  Feature::Uid( "2a63f9d1-08e5-471c-9b40-d7c3e85216af" ),
						  Feature::Uid(),
						  tr( "Block webcams" ), tr( "Allow webcams" ),
						  tr( "Deny access to the cameras of all computers." ),
						  QStringLiteral(":/core/edit-delete.png") ),
	m_features( { m_muteAudioFeature, m_blockUsbFeature, m_blockPrintingFeature, m_blockWebcamFeature } )
{
	if( isEndpointComponent() )
	{
		// Un arrêt brutal a pu laisser un blocage posé : le poste ne doit pas
		// redémarrer muet, sans clé USB ni imprimante.
		cleanupResidualState();
	}
}



DeviceControlPlugin::~DeviceControlPlugin()
{
	if( isEndpointComponent() )
	{
		releaseAll();
	}
}



bool DeviceControlPlugin::isEndpointComponent()
{
	return VeyonCore::component() == VeyonCore::Component::Service ||
		   VeyonCore::component() == VeyonCore::Component::Server;
}



QString DeviceControlPlugin::deviceName( Device device )
{
	switch( device )
	{
	case Device::Audio: return QStringLiteral("audio");
	case Device::UsbStorage: return QStringLiteral("usb");
	case Device::Printing: return QStringLiteral("printing");
	case Device::Webcam: return QStringLiteral("webcam");
	}

	return {};
}



DeviceControlPlugin::Device DeviceControlPlugin::deviceOfFeature( Feature::Uid featureUid, bool* found ) const
{
	*found = true;

	if( featureUid == m_muteAudioFeature.uid() ) { return Device::Audio; }
	if( featureUid == m_blockUsbFeature.uid() ) { return Device::UsbStorage; }
	if( featureUid == m_blockPrintingFeature.uid() ) { return Device::Printing; }
	if( featureUid == m_blockWebcamFeature.uid() ) { return Device::Webcam; }

	*found = false;

	return Device::Audio;
}



QString DeviceControlPlugin::printServiceName()
{
#if defined(Q_OS_WIN)
	return QStringLiteral("Spooler");
#else
	return QStringLiteral("cups");
#endif
}



bool DeviceControlPlugin::examModeActive()
{
	QFile file( examModeActiveStateFile() );
	if( file.open( QIODevice::ReadOnly ) == false )
	{
		return false;
	}

	const auto document = QJsonDocument::fromJson( file.readAll() );
	if( document.isObject() == false )
	{
		return false;
	}

	return document.object().value( QStringLiteral("expiresAtMs") ).toVariant().toLongLong() >
		   QDateTime::currentMSecsSinceEpoch();
}



bool DeviceControlPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
										  const QVariantMap& arguments,
										  const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(arguments)

	bool found = false;
	const auto device = deviceOfFeature( featureUid, &found );
	if( found == false )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();		// ne jamais se couper le son à soi-même

	if( operation != Operation::Start && operation != Operation::Stop )
	{
		return false;
	}

	FeatureMessage message{ featureUid, operation == Operation::Start ? FeatureCommand::Apply
																	  : FeatureCommand::Release };
	message.addArgument( Argument::Device, deviceName( device ) );

	sendFeatureMessage( message, targetInterfaces );

	return true;
}



bool DeviceControlPlugin::handleFeatureMessage( VeyonServerInterface& server,
												const MessageContext& messageContext,
												const FeatureMessage& message )
{
	bool found = false;
	const auto device = deviceOfFeature( message.featureUid(), &found );
	if( found == false )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::Apply:
	{
		QString errorCode;
		if( applyDevice( device, &errorCode ) )
		{
			m_activeDevices.insert( int(device) );
			m_deviceStatus.insert( int(device), QStringLiteral("APPLIED") );
		}
		else
		{
			m_deviceStatus.insert( int(device), errorCode.isEmpty()
				? QStringLiteral("REJECTED") : errorCode );
		}
		break;
	}

	case FeatureCommand::Release:
		releaseDevice( device );
		m_activeDevices.remove( int(device) );
		m_deviceStatus.insert( int(device), QStringLiteral("IDLE") );
		break;

	default:
		return false;
	}

	server.sendFeatureMessageReply( messageContext, statusMessage( device ) );

	return true;
}



bool DeviceControlPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	Q_UNUSED(worker)
	Q_UNUSED(message)

	// aucune interface utilisateur : les contrôles sont silencieux
	return false;
}



bool DeviceControlPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
												const FeatureMessage& message )
{
	bool found = false;
	deviceOfFeature( message.featureUid(), &found );
	if( found == false || message.command<FeatureCommand>() != FeatureCommand::DeviceStatus )
	{
		return false;
	}

	const auto device = message.argument( Argument::Device ).toString();

	QMutexLocker locker( &m_remoteStatusMutex );

	auto* const rawInterface = computerControlInterface.data();
	if( m_trackedRemoteInterfaces.contains( rawInterface ) == false )
	{
		m_trackedRemoteInterfaces.insert( rawInterface );
		connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
			QMutexLocker statusLocker( &m_remoteStatusMutex );
			m_remoteStatuses.remove( rawInterface );
			m_trackedRemoteInterfaces.remove( rawInterface );
		} );
	}

	// un état par périphérique et par poste
	auto status = m_remoteStatuses.value( rawInterface );
	status.insert( device, message.argument( Argument::Status ) );
	status.insert( QStringLiteral("timestamp"), message.argument( Argument::Timestamp ) );
	m_remoteStatuses.insert( rawInterface, status );

	return true;
}



bool DeviceControlPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	bool found = false;
	const auto device = deviceOfFeature( featureUid, &found );

	return found && m_activeDevices.contains( int(device) );
}



QVariantMap DeviceControlPlugin::featureStatus( Feature::Uid featureUid,
												ComputerControlInterface::Pointer computerControlInterface ) const
{
	bool found = false;
	deviceOfFeature( featureUid, &found );
	if( found == false )
	{
		return {};
	}

	QMutexLocker locker( &m_remoteStatusMutex );

	return m_remoteStatuses.value( computerControlInterface.data() );
}



FeatureMessage DeviceControlPlugin::statusMessage( Device device ) const
{
	auto uid = m_muteAudioFeature.uid();
	switch( device )
	{
	case Device::Audio: uid = m_muteAudioFeature.uid(); break;
	case Device::UsbStorage: uid = m_blockUsbFeature.uid(); break;
	case Device::Printing: uid = m_blockPrintingFeature.uid(); break;
	case Device::Webcam: uid = m_blockWebcamFeature.uid(); break;
	}

	FeatureMessage message{ uid, FeatureCommand::DeviceStatus };
	message.addArgument( Argument::Device, deviceName( device ) )
		.addArgument( Argument::Status, m_deviceStatus.value( int(device), QStringLiteral("IDLE") ) )
		.addArgument( Argument::Timestamp, QDateTime::currentMSecsSinceEpoch() );

	return FeatureMessage{ message };
}



bool DeviceControlPlugin::applyDevice( Device device, QString* errorCode )
{
	if( isEndpointComponent() == false )
	{
		return false;
	}

	switch( device )
	{
	case Device::Audio:
		if( applyAudioMute( true ) == false )
		{
			*errorCode = QStringLiteral("UNSUPPORTED");
			return false;
		}
		return true;

	case Device::UsbStorage:
		return applyUsbBlocking( true, errorCode );

	case Device::Printing:
		if( applyPrintingBlocking( true ) == false )
		{
			*errorCode = QStringLiteral("REJECTED");
			return false;
		}
		return true;

	case Device::Webcam:
		if( applyWebcamBlocking( true ) == false )
		{
			*errorCode = QStringLiteral("UNSUPPORTED");
			return false;
		}
		return true;
	}

	return false;
}



void DeviceControlPlugin::releaseDevice( Device device )
{
	if( isEndpointComponent() == false )
	{
		return;
	}

	QString errorCode;

	switch( device )
	{
	case Device::Audio: applyAudioMute( false ); break;
	case Device::UsbStorage: applyUsbBlocking( false, &errorCode ); break;
	case Device::Printing: applyPrintingBlocking( false ); break;
	case Device::Webcam: applyWebcamBlocking( false ); break;
	}
}



void DeviceControlPlugin::releaseAll()
{
	const auto devices = m_activeDevices;
	for( const auto device : devices )
	{
		releaseDevice( static_cast<Device>( device ) );
	}

	m_activeDevices.clear();
}



/** Au démarrage du poste : lève tout blocage résiduel laissé par un arrêt brutal. */
void DeviceControlPlugin::cleanupResidualState()
{
	QString errorCode;

	applyUsbBlocking( false, &errorCode );
	applyWebcamBlocking( false );

	// L'impression n'a pas d'état persistant : un service arrêté est de toute
	// façon redémarré par le système au prochain amorçage selon son mode de
	// démarrage. Le son non plus : la coupure ne survit pas à la session.
}



/**
 * Coupe ou rétablit la sortie audio par défaut. Windows : API CoreAudio, dans
 * la session interactive où tourne le serveur. Linux : PulseAudio/PipeWire par
 * pactl, exécuté dans la session utilisateur.
 */
bool DeviceControlPlugin::applyAudioMute( bool muted ) const
{
#if defined(Q_OS_WIN)
	return DeviceControlWindowsAudio::setDefaultOutputMuted( muted );
#else
	return QProcess::startDetached( QStringLiteral("pactl"),
		{ QStringLiteral("set-sink-mute"), QStringLiteral("@DEFAULT_SINK@"),
		  muted ? QStringLiteral("1") : QStringLiteral("0") } );
#endif
}



/**
 * Windows : politiques USBSTOR + RemovableStorageDevices, transactionnelles.
 * Linux : règle udev refusant le pilote usb-storage, puis rechargement.
 */
bool DeviceControlPlugin::applyUsbBlocking( bool blocked, QString* errorCode )
{
	// Le mode examen pose les mêmes politiques : le laisser seul maître.
	if( blocked && examModeActive() )
	{
		vWarning() << "DeviceControl: mode examen actif; blocage USB refusé";
		if( errorCode != nullptr )
		{
			*errorCode = QStringLiteral("EXAM_MODE_ACTIVE");
		}
		return false;
	}

#if defined(Q_OS_WIN)
	RegistryPolicyGuard guard{ QStringLiteral("devicecontrol-usb"), QStringLiteral("DeviceControl") };

	if( blocked == false )
	{
		guard.remove();
		return true;
	}

	return guard.apply( {
		{ QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services\\USBSTOR"),
			QStringLiteral("Start"), QStringLiteral("REG_DWORD"), QStringLiteral("4") },
		{ QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\RemovableStorageDevices"),
			QStringLiteral("Deny_All"), QStringLiteral("REG_DWORD"), QStringLiteral("1") },
	} );
#else
	if( blocked == false )
	{
		if( QFile::exists( usbRuleFile() ) == false )
		{
			return true;
		}
		const bool removed = QFile::remove( usbRuleFile() );
		QProcess::startDetached( QStringLiteral("udevadm"), { QStringLiteral("control"),
															  QStringLiteral("--reload-rules") } );
		return removed;
	}

	QDir().mkpath( QFileInfo( usbRuleFile() ).absolutePath() );

	QSaveFile file( usbRuleFile() );
	const auto rule = QByteArrayLiteral(
		"# Posée par Veyon (plugin DeviceControl) - retirée à la levée du blocage\n"
		"ACTION==\"add\", SUBSYSTEMS==\"usb\", SUBSYSTEM==\"block\", ENV{ID_USB_DRIVER}==\"usb-storage\", "
		"ATTR{authorized}=\"0\"\n" );
	if( file.open( QIODevice::WriteOnly ) == false ||
		file.write( rule ) != rule.size() || file.commit() == false )
	{
		vWarning() << "DeviceControl: impossible d'écrire la règle udev" << usbRuleFile();
		if( errorCode != nullptr )
		{
			*errorCode = QStringLiteral("REJECTED");
		}
		return false;
	}

	QProcess::startDetached( QStringLiteral("udevadm"), { QStringLiteral("control"),
														  QStringLiteral("--reload-rules") } );

	// Les volumes déjà montés ne sont pas démontés : la règle ne vaut que pour
	// les périphériques branchés ensuite.
	vInfo() << "DeviceControl: stockage USB bloqué pour les prochains branchements";

	return true;
#endif
}



/** Arrête ou redémarre le service d'impression du poste. */
bool DeviceControlPlugin::applyPrintingBlocking( bool blocked )
{
	auto& serviceFunctions = VeyonCore::platform().serviceFunctions();
	const auto service = printServiceName();

	if( serviceFunctions.isRegistered( service ) == false )
	{
		// pas de service d'impression : rien à bloquer, ce n'est pas un échec
		vInfo() << "DeviceControl: aucun service d'impression" << service << "sur ce poste";
		return true;
	}

	return blocked ? serviceFunctions.stop( service ) : serviceFunctions.start( service );
}



/**
 * Windows : politique de confidentialité refusant la caméra (ConsentStore).
 * Linux : retrait des droits d'accès sur les périphériques vidéo, les modes
 * d'origine étant mémorisés pour être rétablis.
 */
bool DeviceControlPlugin::applyWebcamBlocking( bool blocked )
{
#if defined(Q_OS_WIN)
	RegistryPolicyGuard guard{ QStringLiteral("devicecontrol-webcam"), QStringLiteral("DeviceControl") };

	if( blocked == false )
	{
		guard.remove();
		return true;
	}

	return guard.apply( {
		{ QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam"),
			QStringLiteral("Value"), QStringLiteral("REG_SZ"), QStringLiteral("Deny") },
	} );
#else
	const QDir devDirectory( QStringLiteral("/dev") );
	const auto videoDevices = devDirectory.entryList( { QStringLiteral("video*") },
													  QDir::System | QDir::Files );

	if( blocked == false )
	{
		QFile stateFile( webcamStateFile() );
		if( stateFile.open( QIODevice::ReadOnly ) == false )
		{
			return true;		// aucun blocage posé
		}

		const auto document = QJsonDocument::fromJson( stateFile.readAll() );
		stateFile.close();

		const auto modes = document.isObject() ? document.object() : QJsonObject{};
		for( auto it = modes.begin(); it != modes.end(); ++it )
		{
			QFile::setPermissions( it.key(),
				static_cast<QFileDevice::Permissions>( it.value().toInt() ) );
		}

		QFile::remove( webcamStateFile() );

		return true;
	}

	if( videoDevices.isEmpty() )
	{
		vInfo() << "DeviceControl: aucun périphérique vidéo sur ce poste";
		return true;
	}

	QJsonObject modes;
	for( const auto& device : videoDevices )
	{
		const auto path = devDirectory.filePath( device );
		modes.insert( path, static_cast<int>( QFile::permissions( path ) ) );
	}

	QDir().mkpath( stateDirectory() );
	QSaveFile stateFile( webcamStateFile() );
	const auto contents = QJsonDocument( modes ).toJson( QJsonDocument::Compact );
	if( stateFile.open( QIODevice::WriteOnly ) == false ||
		stateFile.write( contents ) != contents.size() || stateFile.commit() == false )
	{
		vWarning() << "DeviceControl: impossible de sauvegarder les droits des périphériques vidéo";
		return false;
	}

	bool success = true;
	for( const auto& device : videoDevices )
	{
		success = QFile::setPermissions( devDirectory.filePath( device ), {} ) && success;
	}

	return success;
#endif
}
