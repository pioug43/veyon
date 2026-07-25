/*
 * RaiseHandPlugin.cpp - implementation of RaiseHandPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir RaiseHandPlugin.h pour la description générale.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>

#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "RaiseHandMasterWindow.h"
#include "RaiseHandPlugin.h"
#include "RaiseHandStudentWidget.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"


RaiseHandPlugin::RaiseHandPlugin( QObject* parent ) :
	QObject( parent ),
	m_raiseHandFeature( QStringLiteral("RaiseHand"),
						Feature::Flag::Mode | Feature::Flag::AllComponents,
						Feature::Uid( "0a5c9e37-4b62-4f18-9d3a-8e7c1b204f56" ),
						Feature::Uid(),
						tr( "Raise hand" ), tr( "Stop raise hand" ),
						tr( "Show a small button on all computers so that users can let you "
							"know they need help. Requests are listed in the order they arrive." ),
						QStringLiteral(":/core/help-about.png") ),
	// Fonctionnalité méta sans action propre : le poste la déclare active tant
	// qu'une demande est en attente, ce qui suffit à faire apparaître une icône
	// sur sa vignette (la console dessine une icône par fonctionnalité active).
	// Volontairement SANS parent : une fonctionnalité fille portant le drapeau
	// Master deviendrait une entrée du menu déroulant du bouton (cf.
	// VeyonMaster::subFeatures) — c'est aussi ce que fait le plugin demo pour
	// ses fonctionnalités méta.
	m_handRaisedFeature( QStringLiteral("HandRaised"),
						 Feature::Flag::Meta | Feature::Flag::AllComponents,
						 Feature::Uid( "b83f1d95-6e07-42ca-8b41-59d0a7c3e218" ),
						 Feature::Uid(),
						 tr( "Help requested" ), {},
						 tr( "A user of this computer is waiting for help." ),
						 QStringLiteral(":/core/help-about.png") ),
	m_features( { m_raiseHandFeature, m_handRaisedFeature } )
{
}



bool RaiseHandPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
									  const QVariantMap& arguments,
									  const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(arguments)

	if( featureUid != m_raiseHandFeature.uid() )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();		// pas de bouton sur le poste enseignant

	switch( operation )
	{
	case Operation::Start:
		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StartRaiseHand }, targetInterfaces );
		return true;

	case Operation::Stop:
		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StopRaiseHand }, targetInterfaces );
		return true;

	default:
		break;
	}

	return false;
}



bool RaiseHandPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
									const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( feature.uid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	controlFeature( m_raiseHandFeature.uid(), Operation::Start, {}, computerControlInterfaces );

	if( m_masterWindow.isNull() )
	{
		m_masterWindow = new RaiseHandMasterWindow( this, master.mainWindow() );
	}

	m_masterWindow->show();
	m_masterWindow->raise();

	return true;
}



bool RaiseHandPlugin::stopFeature( VeyonMasterInterface& master, const Feature& feature,
								   const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(master)

	if( feature.uid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	// la file se ferme avec le mode : les demandes en attente n'ont plus d'objet
	if( m_masterWindow )
	{
		m_masterWindow->close();
	}

	return controlFeature( m_raiseHandFeature.uid(), Operation::Stop, {}, computerControlInterfaces );
}



void RaiseHandPlugin::clearRequest( const ComputerControlInterface::Pointer& computerControlInterface )
{
	if( computerControlInterface.isNull() )
	{
		return;
	}

	sendFeatureMessage( FeatureMessage{ m_raiseHandFeature.uid(), FeatureCommand::ClearHand },
						{ computerControlInterface } );
}



bool RaiseHandPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
											const FeatureMessage& message )
{
	if( message.featureUid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::HandRaised:
	{
		auto timestamp = QDateTime::fromString(
			message.argument( Argument::Timestamp ).toString(), Qt::ISODate );
		if( timestamp.isValid() == false )
		{
			timestamp = QDateTime::currentDateTimeUtc();
		}

		Q_EMIT handRaised( computerControlInterface, timestamp );
		return true;
	}

	case FeatureCommand::HandLowered:
		Q_EMIT handLowered( computerControlInterface );
		return true;

	default:
		break;
	}

	return false;
}



bool RaiseHandPlugin::handleFeatureMessage( VeyonServerInterface& server,
											const MessageContext& messageContext,
											const FeatureMessage& message )
{
	if( message.featureUid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartRaiseHand:
		// mémoriser le maître pour lui relayer les demandes ; chaque message
		// entrant rafraîchit le contexte, ce qui suit ses reconnexions
		m_masterContext = messageContext;
		m_modeActive = true;
		m_handRaised = false;
		break;

	case FeatureCommand::StopRaiseHand:
		m_modeActive = false;
		m_handRaised = false;
		break;

	case FeatureCommand::ClearHand:
		m_masterContext = messageContext;
		m_handRaised = false;
		break;

	default:
		return false;
	}

	// le bouton est une interface utilisateur : il doit s'afficher dans la
	// session de l'utilisateur, pas via le worker SYSTEM (session 0)
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool RaiseHandPlugin::handleFeatureMessageFromWorker( VeyonServerInterface& server,
													  const FeatureMessage& message )
{
	if( message.featureUid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::HandRaised:
		m_handRaised = true;
		break;

	case FeatureCommand::HandLowered:
		m_handRaised = false;
		break;

	default:
		return false;
	}

	return server.sendFeatureMessageReply( m_masterContext, message );
}



bool RaiseHandPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	if( message.featureUid() != m_raiseHandFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartRaiseHand:
		RaiseHandStudentWidget::open( m_raiseHandFeature.uid(), &worker );
		return true;

	case FeatureCommand::ClearHand:
		RaiseHandStudentWidget::clearRequest();
		return true;

	case FeatureCommand::StopRaiseHand:
		RaiseHandStudentWidget::shutdown();
		return true;

	default:
		break;
	}

	return false;
}



bool RaiseHandPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	if( featureUid == m_raiseHandFeature.uid() )
	{
		return m_modeActive;
	}

	// C'est cette fonctionnalité méta qui fait apparaître l'icône sur la
	// vignette du poste, et uniquement pendant l'attente.
	if( featureUid == m_handRaisedFeature.uid() )
	{
		return m_handRaised;
	}

	return false;
}



QVariantMap RaiseHandPlugin::featureStatus( Feature::Uid featureUid,
											ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_raiseHandFeature.uid() )
	{
		return {};
	}

	// Côté maître, l'état par poste se lit dans les fonctionnalités actives
	// remontées par le poste : HandRaised y figure tant que la demande dure.
	const auto activeFeatures = computerControlInterface.isNull()
		? FeatureUidList{}
		: computerControlInterface->activeFeatures();

	return {
		{ QStringLiteral("handRaised"), activeFeatures.contains( m_handRaisedFeature.uid() ) },
	};
}
