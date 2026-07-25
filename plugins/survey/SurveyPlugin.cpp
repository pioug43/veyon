/*
 * SurveyPlugin.cpp - implementation of SurveyPlugin class
 *
 * Voir SurveyPlugin.h pour l'origine (PR upstream veyon/veyon#1151) et la
 * liste des corrections apportées par rapport à la version d'origine.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QMessageBox>
#include <QUuid>

#include "SurveyPlugin.h"
#include "SurveyMasterDialog.h"
#include "SurveyResultsWindow.h"
#include "SurveyStudentDialog.h"
#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "VeyonMasterInterface.h"
#include "VeyonServerInterface.h"


SurveyPlugin::SurveyPlugin( QObject* parent ) :
	QObject( parent ),
	m_surveyFeature( QStringLiteral("Survey"),
					 Feature::Flag::Mode | Feature::Flag::AllComponents,
					 Feature::Uid( "5d7f2a1b-3c4d-5e6f-aaaa-111122223333" ),
					 Feature::Uid(),
					 tr( "Survey" ), tr( "Stop survey" ),
					 tr( "Send a question to the users of the selected computers "
						 "and collect their answers." ),
					 QStringLiteral(":/core/document-edit.png") ),
	m_features( { m_surveyFeature } )
{
}



bool SurveyPlugin::controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
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
		sendFeatureMessage( FeatureMessage{featureUid, FeatureCommand::StopSurvey}, targetInterfaces );
		return true;
	}

	if( operation != Operation::Start )
	{
		return false;
	}

	const auto question = arguments.value( QStringLiteral("question") ).toString()
							  .trimmed().left( MaximumQuestionLength );
	if( question.isEmpty() )
	{
		vWarning() << "refusing to start survey without a question";
		return false;
	}

	auto questionType = arguments.value( QStringLiteral("questionType"),
										 int(QuestionType::SingleChoice) ).toInt();
	if( questionType < int(QuestionType::SingleChoice) || questionType > int(QuestionType::TrueFalse) )
	{
		questionType = int(QuestionType::SingleChoice);
	}

	QStringList options;
	const auto rawOptions = arguments.value( QStringLiteral("options") ).toStringList();
	for( const auto& option : rawOptions )
	{
		const auto trimmed = option.trimmed().left( MaximumOptionLength );
		if( trimmed.isEmpty() == false && options.contains( trimmed ) == false )
		{
			options.append( trimmed );
		}
		if( options.count() >= MaximumOptionCount )
		{
			break;
		}
	}

	if( ( questionType == int(QuestionType::SingleChoice) ||
		  questionType == int(QuestionType::MultipleChoice) ) &&
		options.count() < 2 )
	{
		vWarning() << "refusing to start choice survey with less than two options";
		return false;
	}

	auto surveyId = arguments.value( QStringLiteral("surveyId") ).toString().trimmed().left( 64 );
	if( surveyId.isEmpty() )
	{
		surveyId = QUuid::createUuid().toString( QUuid::WithoutBraces );
	}

	// un nouveau sondage invalide les réponses mémorisées des postes ciblés
	{
		QMutexLocker locker( &m_answersMutex );
		for( const auto& controlInterface : std::as_const(targetInterfaces) )
		{
			m_answers.remove( controlInterface.data() );
		}
	}

	FeatureMessage message{ featureUid, FeatureCommand::StartSurvey };
	message.addArgument( Argument::Question, question );
	message.addArgument( Argument::QuestionType, questionType );
	message.addArgument( Argument::Options, options );
	message.addArgument( Argument::SurveyId, surveyId );

	sendFeatureMessage( message, targetInterfaces );

	return true;
}



bool SurveyPlugin::startFeature( VeyonMasterInterface& master, const Feature& feature,
								 const ComputerControlInterfaceList& computerControlInterfaces )
{
	if( feature.uid() != m_surveyFeature.uid() )
	{
		return false;
	}

	auto targetInterfaces = computerControlInterfaces;
	targetInterfaces.removeLocalHostInterfaces();

	if( targetInterfaces.isEmpty() )
	{
		QMessageBox::information( master.mainWindow(), tr( "Survey" ),
								  tr( "Please select at least one computer." ) );
		return true;
	}

	SurveyMasterDialog dialog( master.mainWindow() );
	if( dialog.exec() != QDialog::Accepted )
	{
		return true;
	}

	if( controlFeature( m_surveyFeature.uid(), Operation::Start,
						dialog.surveyArguments(), targetInterfaces ) == false )
	{
		return true;
	}

	// une fenêtre de suivi par sondage : la précédente n'a plus d'objet
	delete m_resultsWindow;

	m_resultsWindow = new SurveyResultsWindow( this, dialog.question(), dialog.questionType(),
											   dialog.options(), targetInterfaces,
											   master.mainWindow() );
	m_resultsWindow->show();

	return true;
}



bool SurveyPlugin::stopFeature( VeyonMasterInterface& master, const Feature& feature,
								const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(master)

	if( feature.uid() != m_surveyFeature.uid() )
	{
		return false;
	}

	// la fenêtre de suivi reste ouverte : l'enseignant peut encore consulter et
	// exporter les réponses après avoir refermé le sondage sur les postes
	return controlFeature( m_surveyFeature.uid(), Operation::Stop, {}, computerControlInterfaces );
}



bool SurveyPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
										 const FeatureMessage& message )
{
	if( message.featureUid() != m_surveyFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::SubmitAnswer )
	{
		return false;
	}

	const QVariantMap answer{
		{ QStringLiteral("answered"), true },
		{ QStringLiteral("answer"), message.argument( Argument::Answer ).toString().left( MaximumAnswerLength ) },
		{ QStringLiteral("surveyId"), message.argument( Argument::SurveyId ).toString().left( 64 ) },
		{ QStringLiteral("answeredAt"), QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) },
	};

	{
		QMutexLocker locker( &m_answersMutex );
		m_answers.insert( computerControlInterface.data(), answer );
	}

	Q_EMIT surveyAnswerReceived( computerControlInterface,
								 answer.value( QStringLiteral("answer") ).toString(),
								 answer.value( QStringLiteral("answeredAt") ).toString() );

	return true;
}



bool SurveyPlugin::handleFeatureMessage( VeyonServerInterface& server,
										 const MessageContext& messageContext,
										 const FeatureMessage& message )
{
	if( message.featureUid() != m_surveyFeature.uid() )
	{
		return false;
	}

	// mémoriser le maître à l'origine du sondage pour pouvoir lui relayer
	// la réponse saisie dans la session utilisateur (cf. filetransfer)
	if( message.command<FeatureCommand>() == FeatureCommand::StartSurvey )
	{
		m_masterContext = messageContext;
	}

	// le dialogue est une interface utilisateur : il doit s'afficher dans la
	// session de l'utilisateur, pas via le worker SYSTEM (session 0)
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );

	return true;
}



bool SurveyPlugin::handleFeatureMessageFromWorker( VeyonServerInterface& server,
												   const FeatureMessage& message )
{
	if( message.featureUid() != m_surveyFeature.uid() ||
		message.command<FeatureCommand>() != FeatureCommand::SubmitAnswer )
	{
		return false;
	}

	return server.sendFeatureMessageReply( m_masterContext, message );
}



bool SurveyPlugin::handleFeatureMessage( VeyonWorkerInterface& worker, const FeatureMessage& message )
{
	if( message.featureUid() != m_surveyFeature.uid() )
	{
		return false;
	}

	switch( message.command<FeatureCommand>() )
	{
	case FeatureCommand::StartSurvey:
		SurveyStudentDialog::showDialog( message, &worker );
		return true;

	case FeatureCommand::StopSurvey:
		SurveyStudentDialog::closeDialog();
		return true;

	default:
		break;
	}

	return false;
}



QVariantMap SurveyPlugin::featureStatus( Feature::Uid featureUid,
										 ComputerControlInterface::Pointer computerControlInterface ) const
{
	if( featureUid != m_surveyFeature.uid() )
	{
		return {};
	}

	QMutexLocker locker( &m_answersMutex );
	return m_answers.value( computerControlInterface.data(), QVariantMap{
		{ QStringLiteral("answered"), false },
	} );
}
