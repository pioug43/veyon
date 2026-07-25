/*
 * QuizStudentDialog.cpp - dialogue de questionnaire dans la session de l'élève
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include "FeatureMessage.h"
#include "QuizPlugin.h"
#include "QuizStudentDialog.h"
#include "VeyonWorkerInterface.h"

QPointer<QuizStudentDialog> QuizStudentDialog::s_instance = nullptr;


QuizStudentDialog::QuizStudentDialog( Feature::Uid featureUid, VeyonWorkerInterface* worker,
									  const QString& quizId, const QJsonArray& questions,
									  int durationSeconds ) :
	QWidget( nullptr, Qt::Window ),
	m_featureUid( featureUid ),
	m_worker( worker ),
	m_quizId( quizId ),
	m_questions( questions ),
	m_remainingSeconds( durationSeconds )
{
	setWindowTitle( tr( "Quiz" ) );
	resize( 560, 380 );

	auto* const layout = new QVBoxLayout( this );

	auto* const header = new QHBoxLayout;
	m_progress = new QLabel( this );
	header->addWidget( m_progress );
	header->addStretch();
	m_countdown = new QLabel( this );
	m_countdown->setVisible( m_remainingSeconds > 0 );
	header->addWidget( m_countdown );
	layout->addLayout( header );

	m_questionText = new QLabel( this );
	m_questionText->setWordWrap( true );
	auto questionFont = m_questionText->font();
	questionFont.setBold( true );
	m_questionText->setFont( questionFont );
	layout->addWidget( m_questionText );

	m_answerLayout = new QVBoxLayout;
	layout->addLayout( m_answerLayout );

	layout->addStretch();

	m_nextButton = new QPushButton( this );
	connect( m_nextButton, &QPushButton::clicked, this, &QuizStudentDialog::advance );
	layout->addWidget( m_nextButton );

	if( m_remainingSeconds > 0 )
	{
		m_ticker = new QTimer( this );
		connect( m_ticker, &QTimer::timeout, this, &QuizStudentDialog::tick );
		m_ticker->start( 1000 );
	}

	showQuestion();
}



void QuizStudentDialog::open( Feature::Uid featureUid, VeyonWorkerInterface* worker,
							  const QString& quizId, const QJsonArray& questions, int durationSeconds )
{
	if( questions.isEmpty() )
	{
		return;
	}

	shutdown();		// un nouveau questionnaire remplace le précédent

	s_instance = new QuizStudentDialog( featureUid, worker, quizId, questions, durationSeconds );
	s_instance->show();
	s_instance->raise();
	s_instance->activateWindow();
}



void QuizStudentDialog::shutdown()
{
	if( s_instance )
	{
		s_instance->close();
		s_instance->deleteLater();
		s_instance = nullptr;
	}
}



void QuizStudentDialog::clearAnswerWidgets()
{
	m_radioButtons.clear();
	m_checkBoxes.clear();
	m_textAnswer = nullptr;

	while( auto* const item = m_answerLayout->takeAt( 0 ) )
	{
		delete item->widget();
		delete item;
	}
}



void QuizStudentDialog::showQuestion()
{
	clearAnswerWidgets();

	const auto question = m_questions.at( m_currentIndex ).toObject();
	const auto type = question.value( QStringLiteral("type") ).toString();

	m_progress->setText( tr( "Question %1 of %2" ).arg( m_currentIndex + 1 ).arg( m_questions.count() ) );
	m_questionText->setText( question.value( QStringLiteral("text") ).toString() );

	const auto options = question.value( QStringLiteral("options") ).toArray();

	if( type == QStringLiteral("multiple") )
	{
		for( const auto& option : options )
		{
			auto* const checkBox = new QCheckBox( option.toString(), this );
			m_answerLayout->addWidget( checkBox );
			m_checkBoxes.append( checkBox );
		}
	}
	else if( type == QStringLiteral("text") )
	{
		m_textAnswer = new QLineEdit( this );
		m_textAnswer->setMaxLength( QuizPlugin::MaximumAnswerLength );
		m_answerLayout->addWidget( m_textAnswer );
	}
	else
	{
		for( const auto& option : options )
		{
			auto* const radioButton = new QRadioButton( option.toString(), this );
			m_answerLayout->addWidget( radioButton );
			m_radioButtons.append( radioButton );
		}
	}

	m_nextButton->setText( m_currentIndex + 1 < m_questions.count()
		? tr( "Next question" ) : tr( "Finish" ) );
}



/** Envoie la réponse courante sans attendre la fin : un poste coupé en cours
 *  de route ne fait pas perdre ce qui a déjà été répondu. */
void QuizStudentDialog::submitCurrentAnswer()
{
	const auto question = m_questions.at( m_currentIndex ).toObject();

	QString answer;

	if( m_textAnswer != nullptr )
	{
		answer = m_textAnswer->text().trimmed();
	}
	else if( m_checkBoxes.isEmpty() == false )
	{
		QStringList checked;
		for( const auto* const checkBox : std::as_const(m_checkBoxes) )
		{
			if( checkBox->isChecked() )
			{
				checked.append( checkBox->text() );
			}
		}
		answer = checked.join( QStringLiteral(", ") );
	}
	else
	{
		for( const auto* const radioButton : std::as_const(m_radioButtons) )
		{
			if( radioButton->isChecked() )
			{
				answer = radioButton->text();
				break;
			}
		}
	}

	FeatureMessage reply{ m_featureUid, QuizPlugin::FeatureCommand::SubmitAnswer };
	reply.addArgument( QuizPlugin::Argument::QuizId, m_quizId )
		.addArgument( QuizPlugin::Argument::QuestionId,
					  question.value( QStringLiteral("id") ).toString() )
		.addArgument( QuizPlugin::Argument::Answer, answer.left( QuizPlugin::MaximumAnswerLength ) )
		.addArgument( QuizPlugin::Argument::AnsweredAt,
					  QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );

	m_worker->sendFeatureMessageReply( reply );
}



void QuizStudentDialog::advance()
{
	submitCurrentAnswer();

	if( m_currentIndex + 1 < m_questions.count() )
	{
		++m_currentIndex;
		showQuestion();
		return;
	}

	finish();
}



void QuizStudentDialog::finish()
{
	if( m_finished )
	{
		return;
	}
	m_finished = true;

	if( m_ticker )
	{
		m_ticker->stop();
	}

	FeatureMessage reply{ m_featureUid, QuizPlugin::FeatureCommand::QuizFinished };
	reply.addArgument( QuizPlugin::Argument::QuizId, m_quizId )
		.addArgument( QuizPlugin::Argument::AnsweredAt,
					  QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
	m_worker->sendFeatureMessageReply( reply );

	m_questionText->setText( tr( "Your answers have been sent. Thank you!" ) );
	clearAnswerWidgets();
	m_progress->clear();
	m_nextButton->setEnabled( false );
}



void QuizStudentDialog::tick()
{
	if( m_remainingSeconds > 0 )
	{
		--m_remainingSeconds;
	}

	const auto minutes = m_remainingSeconds / 60;
	const auto seconds = m_remainingSeconds % 60;
	m_countdown->setText( QStringLiteral("%1:%2").arg( minutes, 2, 10, QLatin1Char('0') )
											 .arg( seconds, 2, 10, QLatin1Char('0') ) );

	// Temps écoulé : la réponse en cours part telle quelle, puis on clôt. Ne
	// rien envoyer perdrait le travail de la dernière question.
	if( m_remainingSeconds <= 0 && m_finished == false )
	{
		submitCurrentAnswer();
		finish();
	}
}
