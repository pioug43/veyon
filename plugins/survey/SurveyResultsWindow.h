/*
 * SurveyResultsWindow.h - suivi des réponses d'un sondage côté enseignant
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Tableau des postes interrogés (réponse, horodatage) rafraîchi en direct via
 * le signal SurveyPlugin::surveyAnswerReceived, agrégat en pourcentages pour
 * les questions à choix, et export CSV.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QHash>
#include <QStringList>
#include <QWidget>

#include "ComputerControlInterface.h"

class SurveyPlugin;

namespace Ui { class SurveyResultsWindow; }

class SurveyResultsWindow : public QWidget
{
	Q_OBJECT
public:
	SurveyResultsWindow( SurveyPlugin* plugin, const QString& question, int questionType,
						 const QStringList& options,
						 const ComputerControlInterfaceList& computerControlInterfaces,
						 QWidget* parent = nullptr );
	~SurveyResultsWindow() override;

private Q_SLOTS:
	void appendAnswer( ComputerControlInterface::Pointer computerControlInterface,
					   const QString& answer, const QString& answeredAt );

private:
	struct Result
	{
		ComputerControlInterface::Pointer target;
		QString answer;
		QString answeredAt;
		bool answered{false};
	};

	void rebuildTable();
	void rebuildSummary();
	void exportCsv();

	static QString displayName( const ComputerControlInterface* computerControlInterface );
	static QString csvField( const QString& value );

	Ui::SurveyResultsWindow* ui;
	SurveyPlugin* m_plugin;

	const QString m_question;
	const int m_questionType;
	const QStringList m_options;

	QList<Result> m_results;
	QHash<const ComputerControlInterface*, int> m_resultIndexes;
};
