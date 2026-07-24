/*
 * ChatPlugin.cpp - implementation of ChatPlugin class
 *
 * Voir ChatPlugin.h pour la description générale du fonctionnement.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QMessageBox>

#include "ChatPlugin.h"
#include "ChatConfigurationPage.h"
#include "ChatMasterWindow.h"
#include "ChatStudentWindow.h"
#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"


ChatPlugin::ChatPlugin( QObject* parent ) :
	QObject( parent ),
	m_configuration( &VeyonCore::config() ),
	m_chatFeature( QStringLiteral("Chat"),
				   Feature::Flag::Mode | Feature::Flag::AllComponents,
				   Feature::Uid( "2a9651b3-a87c-4901-9218-806ed972e32f" ),
				   Feature::Uid(),
				   tr( "Chat" ), tr( "Stop chat" ),
				   tr( "Open a chat with the users of the selected computers. "
					   "The chat window stays open in the background so users "
					   "can reply at any time." ),
				   QStringLiteral(":/chat/chat.png") ),
	m_features( { m_chatFeature } )
{
}



bool ChatPlugin::controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
								 const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( hasFeature( featureUid ) == false )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();

	if( operation == Operation::Stop )
	{
		sendFeatureMessage( FeatureMessage{featureUid, FeatureCommand::StopChat}, targetInterfaces );
		return true;
	}

	if( operation != Operation::Start )
	{
		return false;
	}

	if( m_configuration.chatEnabled() == false )
	{
		vWarning() << "chat feature is disabled in the configuration";
		return false;
	}

	// point d'entrée générique (Web API/portail) : Start avec un argument
	// "text" ouvre le chat sur les postes et affiche directement le message
	FeatureMessage message{ featureUid, FeatureCommand::StartChat };

	const auto timestamp = QDateTime::currentDateTimeUtc();
	const auto text = arguments.value( argToString(Argument::Text) ).toString()
						  .trimmed().left( MaximumMessageLength );
	if( text.isEmpty() == false )
	{
		message.addArgument( Argument::Text, text );
		message.addArgument( Argument::Timestamp, timestamp.toString( Qt::ISODate ) );

		for( const auto& controlInterface : std::as_const(targetInterfaces) )
		{
			recordMessage( controlInterface.data(), true, text, timestamp );
		}
	}

	sendFeatureMessage( message, targetInterfaces );

	return true;
}



bool ChatPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
							   const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( feature.uid() != m_chatFeature.uid() )
	{
		return false;
	}

	if( m_configuration.chatEnabled() == false )
	{
		QMessageBox::information( master.mainWindow(), tr( "Chat" ),
								  tr( "The chat feature is disabled in the Veyon configuration." ) );
		return true;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();

	if( targetInterfaces.isEmpty() )
	{
		return true;
	}

	sendFeatureMessage( FeatureMessage{feature.uid(), FeatureCommand::StartChat}, targetInterfaces );

	if( m_masterWindow.isNull() )
	{
		m_masterWindow = new ChatMasterWindow( this, master.mainWindow() );
	}

	m_masterWindow->addTargets( targetInterfaces );
	m_masterWindow->show();
	m_masterWindow->raise();
	m_masterWindow->activateWindow();

	return true;
}



bool ChatPlugin::stopFeature( VeyonMasterInterface& master, const Feature& feature,
							  const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(master)

	if( feature.uid() != m_chatFeature.uid() )
	{
		return false;
	}

	controlFeature( feature.uid(), Operation::Stop, {}, computerControlInterfaces );

	if( m_masterWindow )
	{
		m_masterWindow->removeTargets( computerControlInterfaces );
	}

	return true;
}



bool ChatPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
									   const FeatureMessage& message )
{
	if( message.featureUid() != m_chatFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::MessageToMaster )
	{
		return false;
	}

	const auto text = message.argument( Argument::Text ).toString()
						  .trimmed().left( MaximumMessageLength );
	if( text.isEmpty() )
	{
		return true;
	}

	const auto timestamp = QDateTime::currentDateTimeUtc();

	recordMessage( computerControlInterface.data(), false, text, timestamp );

	Q_EMIT chatMessageReceived( computerControlInterface, text, timestamp );

	return true;
}



bool ChatPlugin::handleFeatureMessage( VeyonServerInterface& server,
									   const MessageContext& messageContext,
									   const FeatureMessage& message )
{
	if( message.featureUid() != m_chatFeature.uid() )
	{
		return false;
	}

	if( m_configuration.chatEnabled() == false )
	{
		return true;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartChat:
	case FeatureCommand::MessageToClient:
		// mémoriser le maître à l'origine du chat : chaque message entrant
		// rafraîchit le contexte, ce qui suit les reconnexions du maître
		m_masterContext = messageContext;
		m_chatActiveOnServer = true;
		break;

	case FeatureCommand::StopChat:
		m_chatActiveOnServer = false;
		break;

	default:
		return false;
	}

	// la fenêtre de chat est une interface utilisateur : elle doit s'afficher
	// dans la session de l'utilisateur, pas via le worker SYSTEM (session 0)
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool ChatPlugin::handleFeatureMessageFromWorker( VeyonServerInterface& server,
												 const FeatureMessage& message )
{
	if( message.featureUid() != m_chatFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::MessageToMaster )
	{
		return false;
	}

	if( m_configuration.chatEnabled() == false ||
		m_configuration.chatStudentsCanSendMessages() == false )
	{
		return true;
	}

	return server.sendFeatureMessageReply( m_masterContext, message );
}



bool ChatPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	if( message.featureUid() != m_chatFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartChat:
	case FeatureCommand::MessageToClient:
		ChatStudentWindow::open( m_chatFeature.uid(), &worker,
								 m_configuration.chatStudentsCanSendMessages(),
								 m_configuration.chatStudentWindowStaysOnTop() );

		{
			const auto text = message.argument( Argument::Text ).toString()
								  .trimmed().left( MaximumMessageLength );
			if( text.isEmpty() == false )
			{
				ChatStudentWindow::appendTeacherMessage( text );
			}
		}
		return true;

	case FeatureCommand::StopChat:
		ChatStudentWindow::shutdown();
		return true;

	default:
		break;
	}

	return false;
}



bool ChatPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	return featureUid == m_chatFeature.uid() && m_chatActiveOnServer;
}



QVariantMap ChatPlugin::featureStatus( Feature::Uid featureUid,
									   ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_chatFeature.uid() )
	{
		return {};
	}

	// le pool VDI (location Veyon) permet au VDI manager de regrouper les
	// conversations par pool côté Web API
	QMutexLocker locker( &m_historyMutex );
	return {
		{ QStringLiteral("pool"), computerControlInterface->computer().location() },
		{ QStringLiteral("messages"), m_history.value( computerControlInterface.data() ) },
	};
}



ConfigurationPage* ChatPlugin::createConfigurationPage()
{
	return new ChatConfigurationPage( m_configuration );
}



void ChatPlugin::sendMessageToStudents( const QString& text,
										const ComputerControlInterfaceList& computerControlInterfaces )
{
	const auto trimmedText = text.trimmed().left( MaximumMessageLength );
	if( trimmedText.isEmpty() )
	{
		return;
	}

	const auto timestamp = QDateTime::currentDateTimeUtc();

	FeatureMessage message{ m_chatFeature.uid(), FeatureCommand::MessageToClient };
	message.addArgument( Argument::Text, trimmedText );
	message.addArgument( Argument::Timestamp, timestamp.toString( Qt::ISODate ) );

	sendFeatureMessage( message, computerControlInterfaces );

	for( const auto& controlInterface : computerControlInterfaces )
	{
		recordMessage( controlInterface.data(), true, trimmedText, timestamp );
	}
}



void ChatPlugin::recordMessage( const ComputerControlInterface* computerControlInterface,
								bool fromTeacher, const QString& text, const QDateTime& timestamp )
{
	QMutexLocker locker( &m_historyMutex );

	auto& messages = m_history[computerControlInterface];
	messages.append( QVariantMap{
		{ QStringLiteral("from"), fromTeacher ? QStringLiteral("teacher") : QStringLiteral("student") },
		{ QStringLiteral("text"), text },
		{ QStringLiteral("timestamp"), timestamp.toString( Qt::ISODate ) },
	} );

	while( messages.count() > MaximumStoredMessages )
	{
		messages.removeFirst();
	}
}


IMPLEMENT_CONFIG_PROXY(ChatConfiguration)
