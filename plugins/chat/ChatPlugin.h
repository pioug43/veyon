/*
 * ChatPlugin.h - declaration of ChatPlugin class
 *
 * Équivalent libre de l'addon commercial « Chat » (https://veyon.io/fr/addons/) :
 * messagerie bidirectionnelle enseignant ⇄ élèves. La chaîne de relais
 * maître → serveur → worker (session utilisateur) puis worker → serveur →
 * maître reprend le schéma du plugin survey. L'activation et les options
 * sont paramétrables dans Veyon Configurator (ChatConfigurationPage).
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

#include "ChatConfiguration.h"
#include "ConfigurationPagePluginInterface.h"
#include "FeatureProviderInterface.h"
#include "MessageContext.h"

class ChatMasterWindow;

class ChatPlugin : public QObject, FeatureProviderInterface, PluginInterface, ConfigurationPagePluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.Chat")
	Q_INTERFACES(PluginInterface
					 FeatureProviderInterface
						 ConfigurationPagePluginInterface)
public:
	enum class FeatureCommand
	{
		StartChat,
		StopChat,
		MessageToClient,
		MessageToMaster
	};
	Q_ENUM(FeatureCommand)

	enum class Argument
	{
		Text,
		Timestamp
	};
	Q_ENUM(Argument)

	explicit ChatPlugin( QObject* parent = nullptr );
	~ChatPlugin() override = default;

	Plugin::Uid uid() const override
	{
		return Plugin::Uid{ QStringLiteral("bb6b4a37-3d37-4748-9c70-70213f9e86df") };
	}

	QVersionNumber version() const override
	{
		return QVersionNumber( 1, 0 );
	}

	QString name() const override
	{
		return QStringLiteral("Chat");
	}

	QString description() const override
	{
		return tr( "Chat with the users of the selected computers" );
	}

	QString vendor() const override
	{
		return QStringLiteral("Veyon Community / Portail VDI INSA Lyon");
	}

	QString copyright() const override
	{
		return QStringLiteral("Portail VDI INSA Lyon");
	}

	const FeatureList& featureList() const override
	{
		return m_features;
	}

	const Feature& chatFeature() const
	{
		return m_chatFeature;
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

	ConfigurationPage* createConfigurationPage() override;

	// appelé par ChatMasterWindow pour envoyer un message aux postes ciblés
	void sendMessageToStudents( const QString& text, const ComputerControlInterfaceList& computerControlInterfaces );

	static constexpr int MaximumMessageLength = 4000;

Q_SIGNALS:
	// message d'un élève reçu côté maître (relayé à ChatMasterWindow)
	void chatMessageReceived( ComputerControlInterface::Pointer computerControlInterface,
							  const QString& text, const QDateTime& timestamp );

private:
	void recordMessage( const ComputerControlInterface* computerControlInterface,
						bool fromTeacher, const QString& text, const QDateTime& timestamp );

	static constexpr int MaximumStoredMessages = 500;

	ChatConfiguration m_configuration;

	const Feature m_chatFeature;
	const FeatureList m_features;

	// côté maître : fenêtre de conversation de l'enseignant
	QPointer<ChatMasterWindow> m_masterWindow;

	// côté serveur (poste élève) : contexte du dernier maître ayant ouvert le
	// chat, nécessaire pour relayer les messages saisis dans la session
	// utilisateur vers ce maître (cf. survey)
	MessageContext m_masterContext{};
	bool m_chatActiveOnServer{false};

	// côté maître : historique par poste, exposé au portail via featureStatus()
	mutable QMutex m_historyMutex;
	QHash<const ComputerControlInterface*, QVariantList> m_history;
};
