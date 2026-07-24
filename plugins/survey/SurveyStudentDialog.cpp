/*
 * SurveyStudentDialog.cpp - dialogue de réponse affiché dans la session utilisateur
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QMessageBox>

#include "SurveyStudentDialog.h"
#include "ui_SurveyStudentDialog.h"
#include "SurveyPlugin.h"
#include "VeyonWorkerInterface.h"

QPointer<SurveyStudentDialog> SurveyStudentDialog::s_instance = nullptr;


SurveyStudentDialog::SurveyStudentDialog( const FeatureMessage& message, VeyonWorkerInterface* worker,
										  QWidget* parent ) :
	QDialog( parent, Qt::WindowStaysOnTopHint ),
	ui( new Ui::SurveyStudentDialog ),
	m_message( message ),
	m_worker( worker )
{
	ui->setupUi( this );
	setAttribute( Qt::WA_DeleteOnClose );

	using QuestionType = SurveyPlugin::QuestionType;
	using Argument = SurveyPlugin::Argument;

	m_questionType = message.argument( Argument::QuestionType ).toInt();
	const auto question = message.argument( Argument::Question ).toString();
	const auto options = message.argument( Argument::Options ).toStringList();

	ui->questionLabel->setText( question );

	const auto type = QuestionType( m_questionType );

	if( type == QuestionType::SingleChoice )
	{
		for( const auto& option : options )
		{
			auto* radioButton = new QRadioButton( option, ui->scrollAreaWidgetContents );
			ui->contentLayout->addWidget( radioButton );
			m_radioButtons.append( radioButton );
		}
	}
	else if( type == QuestionType::MultipleChoice )
	{
		for( const auto& option : options )
		{
			auto* checkBox = new QCheckBox( option, ui->scrollAreaWidgetContents );
			ui->contentLayout->addWidget( checkBox );
			m_checkBoxes.append( checkBox );
		}
	}
	else if( type == QuestionType::ShortText )
	{
		m_shortTextEdit = new QLineEdit( ui->scrollAreaWidgetContents );
		ui->contentLayout->addWidget( m_shortTextEdit );
	}
	else if( type == QuestionType::LongText )
	{
		m_longTextEdit = new QTextEdit( ui->scrollAreaWidgetContents );
		ui->contentLayout->addWidget( m_longTextEdit );
	}
	else if( type == QuestionType::TrueFalse )
	{
		auto* trueButton = new QRadioButton( tr("True"), ui->scrollAreaWidgetContents );
		auto* falseButton = new QRadioButton( tr("False"), ui->scrollAreaWidgetContents );
		ui->contentLayout->addWidget( trueButton );
		ui->contentLayout->addWidget( falseButton );
		m_radioButtons.append( trueButton );
		m_radioButtons.append( falseButton );
	}

	ui->contentLayout->addStretch();

	connect( ui->submitButton, &QPushButton::clicked, this, &SurveyStudentDialog::onSubmitClicked );
}



SurveyStudentDialog::~SurveyStudentDialog()
{
	delete ui;
}



void SurveyStudentDialog::showDialog( const FeatureMessage& message, VeyonWorkerInterface* worker )
{
	closeDialog();

	s_instance = new SurveyStudentDialog( message, worker );
	s_instance->show();
	s_instance->raise();
	s_instance->activateWindow();
}



void SurveyStudentDialog::closeDialog()
{
	if( s_instance )
	{
		s_instance->close();
		s_instance = nullptr;
	}
}



void SurveyStudentDialog::onSubmitClicked()
{
	using QuestionType = SurveyPlugin::QuestionType;
	using Argument = SurveyPlugin::Argument;

	QString answer;
	const auto type = QuestionType( m_questionType );

	if( type == QuestionType::SingleChoice || type == QuestionType::TrueFalse )
	{
		for( auto* radioButton : std::as_const(m_radioButtons) )
		{
			if( radioButton->isChecked() )
			{
				answer = radioButton->text();
				break;
			}
		}
	}
	else if( type == QuestionType::MultipleChoice )
	{
		QStringList selected;
		for( auto* checkBox : std::as_const(m_checkBoxes) )
		{
			if( checkBox->isChecked() )
			{
				selected.append( checkBox->text() );
			}
		}
		answer = selected.join( QStringLiteral(", ") );
	}
	else if( type == QuestionType::ShortText && m_shortTextEdit )
	{
		answer = m_shortTextEdit->text().trimmed();
	}
	else if( type == QuestionType::LongText && m_longTextEdit )
	{
		answer = m_longTextEdit->toPlainText().trimmed();
	}

	if( answer.isEmpty() )
	{
		QMessageBox::warning( this, tr("Survey"), tr("Please select or enter an answer first.") );
		return;
	}

	FeatureMessage reply( m_message.featureUid(), SurveyPlugin::FeatureCommand::SubmitAnswer );
	reply.addArgument( Argument::Answer, answer.left( 4000 ) );
	reply.addArgument( Argument::SurveyId, m_message.argument( Argument::SurveyId ) );

	if( m_worker )
	{
		m_worker->sendFeatureMessageReply( reply );
	}

	QMessageBox::information( this, tr("Survey"), tr("Your answer has been submitted.") );
	close();
}
