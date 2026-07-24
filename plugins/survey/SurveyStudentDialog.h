/*
 * SurveyStudentDialog.h - dialogue de réponse affiché dans la session utilisateur
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QPointer>
#include <QRadioButton>
#include <QTextEdit>

#include "FeatureMessage.h"

class VeyonWorkerInterface;

namespace Ui { class SurveyStudentDialog; }

class SurveyStudentDialog : public QDialog
{
	Q_OBJECT
public:
	explicit SurveyStudentDialog( const FeatureMessage& message, VeyonWorkerInterface* worker,
								  QWidget* parent = nullptr );
	~SurveyStudentDialog() override;

	static void showDialog( const FeatureMessage& message, VeyonWorkerInterface* worker );
	static void closeDialog();

private Q_SLOTS:
	void onSubmitClicked();

private:
	Ui::SurveyStudentDialog* ui;
	const FeatureMessage m_message;
	VeyonWorkerInterface* m_worker;

	static QPointer<SurveyStudentDialog> s_instance;

	int m_questionType;
	QList<QRadioButton*> m_radioButtons;
	QList<QCheckBox*> m_checkBoxes;
	QLineEdit* m_shortTextEdit{nullptr};
	QTextEdit* m_longTextEdit{nullptr};
};
