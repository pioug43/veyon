/*
 * QuizStudentDialog.h - dialogue de questionnaire dans la session de l'élève
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Une question à la fois, avec compte à rebours global s'il y a une limite de
 * temps. Chaque réponse est envoyée dès qu'on passe à la question suivante :
 * si le poste est coupé en cours de route, ce qui a déjà été répondu est
 * conservé côté enseignant.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QJsonArray>
#include <QPointer>
#include <QWidget>

#include "Feature.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTimer;
class QVBoxLayout;
class VeyonWorkerInterface;

class QuizStudentDialog : public QWidget
{
	Q_OBJECT
public:
	static void open( Feature::Uid featureUid, VeyonWorkerInterface* worker, const QString& quizId,
					  const QJsonArray& questions, int durationSeconds );
	static void shutdown();

private:
	QuizStudentDialog( Feature::Uid featureUid, VeyonWorkerInterface* worker, const QString& quizId,
					   const QJsonArray& questions, int durationSeconds );
	~QuizStudentDialog() override = default;

	void showQuestion();
	void submitCurrentAnswer();
	void advance();
	void finish();
	void tick();
	void clearAnswerWidgets();

	static QPointer<QuizStudentDialog> s_instance;

	const Feature::Uid m_featureUid;
	VeyonWorkerInterface* m_worker;
	const QString m_quizId;
	const QJsonArray m_questions;

	QLabel* m_progress{nullptr};
	QLabel* m_countdown{nullptr};
	QLabel* m_questionText{nullptr};
	QVBoxLayout* m_answerLayout{nullptr};
	QPushButton* m_nextButton{nullptr};

	QList<QRadioButton *> m_radioButtons;
	QList<QCheckBox *> m_checkBoxes;
	QLineEdit* m_textAnswer{nullptr};

	int m_currentIndex{0};
	int m_remainingSeconds{0};
	QTimer* m_ticker{nullptr};
	bool m_finished{false};
};
