/*
 * SurveyMasterDialog.cpp - composition d'un sondage côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QMessageBox>
#include <QPushButton>

#include "SurveyMasterDialog.h"
#include "SurveyPlugin.h"

#include "ui_SurveyMasterDialog.h"


SurveyMasterDialog::SurveyMasterDialog( QWidget* parent ) :
	QDialog( parent ),
	ui( new Ui::SurveyMasterDialog )
{
	ui->setupUi( this );

	connect( ui->questionType, QOverload<int>::of(&QComboBox::currentIndexChanged),
			 this, [this]() { updateOptionsState(); } );

	// « Vrai/Faux » fournit ses propres réponses côté élève ; les questions
	// libres n'en ont pas : la liste ne concerne que les questions à choix
	updateOptionsState();
}



SurveyMasterDialog::~SurveyMasterDialog()
{
	delete ui;
}



void SurveyMasterDialog::updateOptionsState()
{
	const auto type = questionType();
	const bool needsOptions = type == int(SurveyPlugin::QuestionType::SingleChoice) ||
							  type == int(SurveyPlugin::QuestionType::MultipleChoice);

	ui->optionsLabel->setEnabled( needsOptions );
	ui->optionsEdit->setEnabled( needsOptions );
}



QString SurveyMasterDialog::question() const
{
	return ui->questionEdit->toPlainText().trimmed();
}



int SurveyMasterDialog::questionType() const
{
	// l'ordre des entrées de la liste déroulante suit SurveyPlugin::QuestionType,
	// qui commence à 1
	return ui->questionType->currentIndex() + 1;
}



QStringList SurveyMasterDialog::options() const
{
	QStringList options;

	const auto lines = ui->optionsEdit->toPlainText().split( QLatin1Char('\n'), Qt::SkipEmptyParts );
	for( const auto& line : lines )
	{
		const auto option = line.trimmed();
		if( option.isEmpty() == false && options.contains( option ) == false )
		{
			options.append( option );
		}
	}

	return options;
}



QVariantMap SurveyMasterDialog::surveyArguments() const
{
	return {
		{ QStringLiteral("question"), question() },
		{ QStringLiteral("questionType"), questionType() },
		{ QStringLiteral("options"), options() },
	};
}



void SurveyMasterDialog::accept()
{
	// mêmes règles que SurveyPlugin::controlFeature(), signalées ici pendant la
	// saisie plutôt que par un refus silencieux au moment de l'envoi
	if( question().isEmpty() )
	{
		QMessageBox::information( this, tr( "Survey" ), tr( "Please enter a question." ) );
		return;
	}

	const auto type = questionType();
	if( ( type == int(SurveyPlugin::QuestionType::SingleChoice) ||
		  type == int(SurveyPlugin::QuestionType::MultipleChoice) ) &&
		options().count() < 2 )
	{
		QMessageBox::information( this, tr( "Survey" ),
								  tr( "Please enter at least two proposed answers, one per line." ) );
		return;
	}

	QDialog::accept();
}
