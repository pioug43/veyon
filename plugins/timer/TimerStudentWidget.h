/*
 * TimerStudentWidget.h - compte à rebours affiché dans la session de l'élève
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Deux présentations : un bandeau discret en haut de l'écran, ou un affichage
 * plein écran quand le temps restant doit être impossible à manquer. Dans les
 * deux cas la fenêtre reste au premier plan sans voler le focus : elle informe,
 * elle n'interrompt pas.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QTimer;

class TimerStudentWidget : public QWidget
{
	Q_OBJECT
public:
	static void open( int seconds, const QString& label, bool fullscreen );
	static void shutdown();

private:
	TimerStudentWidget( int seconds, const QString& label, bool fullscreen );
	~TimerStudentWidget() override = default;

	void tick();
	void updateDisplay();
	void placeBanner();

	static QPointer<TimerStudentWidget> s_instance;

	QLabel* m_label{nullptr};
	QLabel* m_countdown{nullptr};
	QTimer* m_ticker{nullptr};

	int m_remainingSeconds;
	int m_lastBannerWidth{-1};
	const bool m_fullscreen;
};
