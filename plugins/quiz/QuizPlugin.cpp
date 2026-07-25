/*
 * QuizPlugin.cpp - implementation of QuizPlugin class
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Voir QuizPlugin.h pour la description générale, notamment le fait que les
 * bonnes réponses ne quittent jamais l'appelant.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QUuid>

#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "QuizPlugin.h"
#include "QuizQuestions.h"
#include "QuizStudentDialog.h"
#include "VeyonServerInterface.h"



QuizPlugin::QuizPlugin( QObject* parent ) :
	QObject( parent ),
	m_quizFeature( QStringLiteral("Quiz"),
				   Feature::Flag::Mode | Feature::Flag::AllComponents,
				   Feature::Uid( "a2f5b681-3c70-4e94-8d25-16b0e7c3f958" ),
				   Feature::Uid(),
				   tr( "Quiz" ), tr( "Stop quiz" ),
				   tr( "Send a series of questions to the users of the selected computers "
					   "and collect their answers as they go." ),
				   QStringLiteral(":/core/document-edit.png") ),
	m_features( { m_quizFeature } )
{
}



bool QuizPlugin::controlFeature( Feature::Uid featureUid, Operation operation,
								 const QVariantMap& arguments,
								 const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( featureUid != m_quizFeature.uid() )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();

	if( operation == Operation::Stop )
	{
		sendFeatureMessage( FeatureMessage{ featureUid, FeatureCommand::StopQuiz }, targetInterfaces );
		return true;
	}

	if( operation != Operation::Start )
	{
		return false;
	}

	// Les questions arrivent en JSON : c'est la forme naturelle pour un appelant
	// HTTP, et elle évite d'inventer un encodage maison pour une structure
	// imbriquée.
	const auto rawQuestions = arguments.value( argToString( Argument::Questions ) ).toString()
								  .left( MaximumQuestionsLength );
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson( rawQuestions.toUtf8(), &error );
	if( error.error != QJsonParseError::NoError || document.isArray() == false )
	{
		vWarning() << "Quiz: questions illisibles" << error.errorString();
		return false;
	}

	const auto questions = QuizQuestions::sanitize( document.array(), MaximumQuestions );
	if( questions.isEmpty() )
	{
		vWarning() << "Quiz: aucune question exploitable; envoi refusé";
		return false;
	}

	auto quizId = arguments.value( argToString( Argument::QuizId ) ).toString().trimmed().left( 64 );
	if( quizId.isEmpty() )
	{
		quizId = QUuid::createUuid().toString( QUuid::WithoutBraces );
	}

	const auto duration = qBound( 0, arguments.value( argToString( Argument::DurationSeconds ) ).toInt(),
								  MaximumDurationSeconds );

	// un nouveau questionnaire remet à zéro les résultats des postes visés
	{
		QMutexLocker locker( &m_resultsMutex );
		for( const auto& controlInterface : std::as_const(targetInterfaces) )
		{
			auto* const rawInterface = controlInterface.data();
			// Sans ce suivi, un poste qui ne répond jamais (éteint, changement
			// de salle) laisserait une entrée orpheline : l'adresse réutilisée
			// par une autre interface rendrait la copie du poste précédent.
			trackInterface( rawInterface );
			m_results.insert( rawInterface, QVariantMap{
				{ QStringLiteral("quizId"), quizId },
				{ QStringLiteral("answers"), QVariantMap{} },
				{ QStringLiteral("finished"), false },
			} );
		}
	}

	FeatureMessage message{ featureUid, FeatureCommand::StartQuiz };
	message.addArgument( Argument::QuizId, quizId )
		.addArgument( Argument::Questions,
					  QString::fromUtf8( QJsonDocument( questions ).toJson( QJsonDocument::Compact ) ) )
		.addArgument( Argument::DurationSeconds, duration );

	sendFeatureMessage( message, targetInterfaces );

	return true;
}



bool QuizPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
									   const FeatureMessage& message )
{
	if( message.featureUid() != m_quizFeature.uid() )
	{
		return false;
	}

	const auto command = message.command<FeatureCommand>();
	if( command != FeatureCommand::SubmitAnswer && command != FeatureCommand::QuizFinished )
	{
		return false;
	}

	QMutexLocker locker( &m_resultsMutex );

	auto* const rawInterface = computerControlInterface.data();
	trackInterface( rawInterface );

	auto result = m_results.value( rawInterface );
	result.insert( QStringLiteral("quizId"), message.argument( Argument::QuizId ) );

	if( command == FeatureCommand::SubmitAnswer )
	{
		auto answers = result.value( QStringLiteral("answers") ).toMap();
		answers.insert( message.argument( Argument::QuestionId ).toString(), QVariantMap{
			{ QStringLiteral("answer"),
			  message.argument( Argument::Answer ).toString().left( MaximumAnswerLength ) },
			{ QStringLiteral("answeredAt"), message.argument( Argument::AnsweredAt ) },
		} );
		result.insert( QStringLiteral("answers"), answers );
	}
	else
	{
		result.insert( QStringLiteral("finished"), true );
		result.insert( QStringLiteral("finishedAt"), message.argument( Argument::AnsweredAt ) );
	}

	m_results.insert( rawInterface, result );

	return true;
}



/** Purge l'entrée d'un poste à sa destruction. À appeler sous m_resultsMutex. */
void QuizPlugin::trackInterface( const ComputerControlInterface* rawInterface )
{
	if( m_trackedRemoteInterfaces.contains( rawInterface ) )
	{
		return;
	}

	m_trackedRemoteInterfaces.insert( rawInterface );

	connect( rawInterface, &QObject::destroyed, this, [this, rawInterface]() {
		QMutexLocker resultsLocker( &m_resultsMutex );
		m_results.remove( rawInterface );
		m_trackedRemoteInterfaces.remove( rawInterface );
	} );
}



bool QuizPlugin::handleFeatureMessage( VeyonServerInterface& server,
									   const MessageContext& messageContext,
									   const FeatureMessage& message )
{
	if( message.featureUid() != m_quizFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartQuiz:
		// mémoriser le maître pour lui relayer les réponses ; chaque message
		// entrant rafraîchit le contexte, ce qui suit ses reconnexions
		m_masterContext = messageContext;
		m_quizActiveOnServer = true;
		break;

	case FeatureCommand::StopQuiz:
		m_quizActiveOnServer = false;
		break;

	default:
		return false;
	}

	// le questionnaire est une interface utilisateur : il doit s'afficher dans
	// la session de l'utilisateur, pas via le worker SYSTEM (session 0)
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool QuizPlugin::handleFeatureMessageFromWorker( VeyonServerInterface& server,
												 const FeatureMessage& message )
{
	if( message.featureUid() != m_quizFeature.uid() )
	{
		return false;
	}

	const auto command = message.command<FeatureCommand>();
	if( command != FeatureCommand::SubmitAnswer && command != FeatureCommand::QuizFinished )
	{
		return false;
	}

	if( command == FeatureCommand::QuizFinished )
	{
		m_quizActiveOnServer = false;
	}

	return server.sendFeatureMessageReply( m_masterContext, message );
}



bool QuizPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	if( message.featureUid() != m_quizFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartQuiz:
	{
		const auto document = QJsonDocument::fromJson(
			message.argument( Argument::Questions ).toString().toUtf8() );

		// Le nettoyage est refait ici, à l'arrivée : le garantir seulement à
		// l'émission supposerait que tout maître authentifié est bien
		// intentionné. Rien de superflu à ce que la machine de l'élève ne
		// conserve, même en mémoire, que ce qu'elle doit afficher.
		QuizStudentDialog::open( m_quizFeature.uid(), &worker,
								 message.argument( Argument::QuizId ).toString(),
								 QuizQuestions::sanitize( document.array(), MaximumQuestions ),
								 message.argument( Argument::DurationSeconds ).toInt() );
		return true;
	}

	case FeatureCommand::StopQuiz:
		QuizStudentDialog::shutdown();
		return true;

	default:
		break;
	}

	return false;
}



bool QuizPlugin::isFeatureActive( VeyonServerInterface& server, Feature::Uid featureUid ) const
{
	Q_UNUSED(server)

	return featureUid == m_quizFeature.uid() && m_quizActiveOnServer;
}



QVariantMap QuizPlugin::featureStatus( Feature::Uid featureUid,
									   ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_quizFeature.uid() )
	{
		return {};
	}

	QMutexLocker locker( &m_resultsMutex );

	return m_results.value( computerControlInterface.data(), QVariantMap{
		{ QStringLiteral("answers"), QVariantMap{} },
		{ QStringLiteral("finished"), false },
	} );
}
