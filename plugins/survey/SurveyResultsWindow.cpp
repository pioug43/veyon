/*
 * SurveyResultsWindow.cpp - suivi des réponses d'un sondage côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QSaveFile>
#include <QTableWidgetItem>

#include "SurveyPlugin.h"
#include "SurveyResultsWindow.h"

#include "ui_SurveyResultsWindow.h"

namespace
{

enum ResultColumn
{
	ComputerColumn,
	StatusColumn,
	TimeColumn,
	AnswerColumn,
};

}


SurveyResultsWindow::SurveyResultsWindow( SurveyPlugin* plugin, const QString& question, int questionType,
										  const QStringList& options,
										  const ComputerControlInterfaceList& computerControlInterfaces,
										  QWidget* parent ) :
	QWidget( parent, Qt::Window ),
	ui( new Ui::SurveyResultsWindow ),
	m_plugin( plugin ),
	m_question( question ),
	m_questionType( questionType ),
	m_options( options )
{
	ui->setupUi( this );
	setAttribute( Qt::WA_DeleteOnClose );

	ui->questionLabel->setText( QStringLiteral("<b>%1</b>").arg( question.toHtmlEscaped() ) );

	for( const auto& controlInterface : computerControlInterfaces )
	{
		m_resultIndexes.insert( controlInterface.data(), m_results.count() );
		m_results.append( Result{ controlInterface, {}, {}, false } );
	}

	// L'agrégat n'a de sens que pour les questions à choix : pour les réponses
	// libres, seul le tableau détaillé est pertinent.
	const bool hasSummary = m_questionType == int(SurveyPlugin::QuestionType::SingleChoice) ||
							m_questionType == int(SurveyPlugin::QuestionType::MultipleChoice) ||
							m_questionType == int(SurveyPlugin::QuestionType::TrueFalse);
	ui->summaryGroupBox->setVisible( hasSummary );

	connect( ui->exportButton, &QPushButton::clicked, this, &SurveyResultsWindow::exportCsv );
	connect( ui->closeButton, &QPushButton::clicked, this, &QWidget::close );

	connect( m_plugin, &SurveyPlugin::surveyAnswerReceived,
			 this, &SurveyResultsWindow::appendAnswer );

	rebuildTable();
	rebuildSummary();
}



SurveyResultsWindow::~SurveyResultsWindow()
{
	delete ui;
}



void SurveyResultsWindow::appendAnswer( ComputerControlInterface::Pointer computerControlInterface,
										const QString& answer, const QString& answeredAt )
{
	const auto index = m_resultIndexes.value( computerControlInterface.data(), -1 );
	if( index < 0 )
	{
		return;		// réponse d'un poste qui n'était pas ciblé par ce sondage
	}

	auto& result = m_results[index];
	result.answer = answer;
	result.answeredAt = answeredAt;
	result.answered = true;

	rebuildTable();
	rebuildSummary();
}



void SurveyResultsWindow::rebuildTable()
{
	ui->resultTable->setUpdatesEnabled( false );
	ui->resultTable->setRowCount( m_results.count() );

	int answeredCount = 0;

	for( int row = 0; row < m_results.count(); ++row )
	{
		const auto& result = m_results.at( row );
		if( result.answered )
		{
			++answeredCount;
		}

		auto localTime = result.answeredAt;
		if( localTime.isEmpty() == false )
		{
			// les réponses sont horodatées en UTC : les afficher en heure locale
			const auto timestamp = QDateTime::fromString( result.answeredAt, Qt::ISODate );
			if( timestamp.isValid() )
			{
				localTime = timestamp.toLocalTime().toString( QStringLiteral("HH:mm:ss") );
			}
		}

		ui->resultTable->setItem( row, ComputerColumn,
			new QTableWidgetItem( displayName( result.target.data() ) ) );
		ui->resultTable->setItem( row, StatusColumn,
			new QTableWidgetItem( result.answered ? tr( "Answered" ) : tr( "Waiting" ) ) );
		ui->resultTable->setItem( row, TimeColumn, new QTableWidgetItem( localTime ) );
		ui->resultTable->setItem( row, AnswerColumn, new QTableWidgetItem( result.answer ) );
	}

	ui->resultTable->resizeColumnsToContents();
	ui->resultTable->setUpdatesEnabled( true );

	ui->progressLabel->setText( tr( "%1 of %2 computers answered" )
									.arg( answeredCount ).arg( m_results.count() ) );
}



void SurveyResultsWindow::rebuildSummary()
{
	if( ui->summaryGroupBox->isVisible() == false )
	{
		return;
	}

	auto options = m_options;
	if( m_questionType == int(SurveyPlugin::QuestionType::TrueFalse) )
	{
		// le dialogue élève propose ces deux réponses pour une question Vrai/Faux.
		// Affectation explicite : avec Qt 5.15, « = { a, b } » sur un QStringList
		// est une surcharge ambiguë.
		options = QStringList{ tr( "True" ), tr( "False" ) };
	}

	QList<int> counts;
	counts.reserve( options.count() );
	int totalAnswers = 0;

	for( const auto& option : std::as_const(options) )
	{
		int count = 0;
		for( const auto& result : std::as_const(m_results) )
		{
			if( result.answered == false )
			{
				continue;
			}
			// une réponse à choix multiple contient plusieurs libellés séparés par
			// des virgules : on cherche donc le libellé parmi les éléments
			const auto answers = result.answer.split( QLatin1Char(','), Qt::SkipEmptyParts );
			for( const auto& answer : answers )
			{
				if( answer.trimmed().compare( option, Qt::CaseInsensitive ) == 0 )
				{
					++count;
					break;
				}
			}
		}
		counts.append( count );
		totalAnswers += count;
	}

	ui->summaryTable->setUpdatesEnabled( false );
	ui->summaryTable->setRowCount( options.count() );

	for( int row = 0; row < options.count(); ++row )
	{
		const auto count = counts.at( row );
		const auto share = totalAnswers > 0 ?
			QStringLiteral("%1 %").arg( 100.0 * count / totalAnswers, 0, 'f', 1 ) :
			QStringLiteral("-");

		ui->summaryTable->setItem( row, 0, new QTableWidgetItem( options.at( row ) ) );
		ui->summaryTable->setItem( row, 1, new QTableWidgetItem( QString::number( count ) ) );
		ui->summaryTable->setItem( row, 2, new QTableWidgetItem( share ) );
	}

	ui->summaryTable->resizeColumnsToContents();
	ui->summaryTable->setUpdatesEnabled( true );
}



void SurveyResultsWindow::exportCsv()
{
	const auto fileName = QFileDialog::getSaveFileName( this, tr( "Export survey results" ),
														QDir::homePath(), tr( "CSV files (*.csv)" ) );
	if( fileName.isEmpty() )
	{
		return;
	}

	QString content;
	content += QStringLiteral("%1;%2;%3;%4\n")
				   .arg( csvField( tr( "Computer" ) ), csvField( tr( "Status" ) ),
						 csvField( tr( "Time" ) ), csvField( tr( "Answer" ) ) );

	for( const auto& result : std::as_const(m_results) )
	{
		content += QStringLiteral("%1;%2;%3;%4\n")
					   .arg( csvField( displayName( result.target.data() ) ),
							 csvField( result.answered ? tr( "Answered" ) : tr( "Waiting" ) ),
							 csvField( result.answeredAt ),
							 csvField( result.answer ) );
	}

	QSaveFile file( fileName );
	const auto data = content.toUtf8();
	if( file.open( QIODevice::WriteOnly | QIODevice::Text ) == false ||
		file.write( data ) != data.size() || file.commit() == false )
	{
		QMessageBox::warning( this, tr( "Export survey results" ),
							  tr( "Could not write file %1." ).arg( fileName ) );
	}
}



QString SurveyResultsWindow::displayName( const ComputerControlInterface* computerControlInterface )
{
	if( computerControlInterface == nullptr )
	{
		return {};
	}

	auto name = computerControlInterface->computerName();

	auto user = computerControlInterface->userFullName();
	if( user.isEmpty() )
	{
		user = computerControlInterface->userLoginName();
	}

	if( user.isEmpty() == false )
	{
		name += QStringLiteral(" (%1)").arg( user );
	}

	return name;
}



/**
 * Échappement CSV : guillemets doublés, champ toujours encadré.
 *
 * Une cellule commençant par « = », « + », « - » ou « @ » est interprétée
 * comme une formule par les tableurs : elle est préfixée d'une apostrophe. Les
 * réponses viennent des élèves — sans cela, un élève dicterait une formule
 * exécutée à l'ouverture du fichier par l'enseignant.
 */
QString SurveyResultsWindow::csvField( const QString& value )
{
	auto escaped = value;
	escaped.replace( QLatin1Char('"'), QStringLiteral("\"\"") );

	static const QString FormulaStarters = QStringLiteral("=+-@\t\r");
	if( escaped.isEmpty() == false && FormulaStarters.contains( escaped.at( 0 ) ) )
	{
		escaped.prepend( QLatin1Char('\'') );
	}

	return QStringLiteral("\"%1\"").arg( escaped );
}
