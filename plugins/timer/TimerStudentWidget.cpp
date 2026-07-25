/*
 * TimerStudentWidget.cpp - compte à rebours affiché dans la session de l'élève
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include "TimerStudentWidget.h"

QPointer<TimerStudentWidget> TimerStudentWidget::s_instance = nullptr;

namespace
{

// en dessous de ce reste, le décompte passe en rouge
constexpr int WarningThresholdSeconds = 60;

}


TimerStudentWidget::TimerStudentWidget( int seconds, const QString& label, bool fullscreen ) :
	QWidget( nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ),
	m_remainingSeconds( seconds ),
	m_fullscreen( fullscreen )
{
	setAttribute( Qt::WA_ShowWithoutActivating );		// informer sans interrompre
	setWindowTitle( tr( "Remaining time" ) );

	auto* const layout = new QVBoxLayout( this );
	layout->setContentsMargins( m_fullscreen ? 48 : 12, m_fullscreen ? 48 : 6,
								m_fullscreen ? 48 : 12, m_fullscreen ? 48 : 6 );

	m_label = new QLabel( label, this );
	m_label->setAlignment( Qt::AlignCenter );
	m_label->setVisible( label.isEmpty() == false );
	layout->addWidget( m_label );

	m_countdown = new QLabel( this );
	m_countdown->setAlignment( Qt::AlignCenter );
	layout->addWidget( m_countdown );

	auto countdownFont = m_countdown->font();
	countdownFont.setPointSize( m_fullscreen ? 96 : 20 );
	countdownFont.setBold( true );
	m_countdown->setFont( countdownFont );

	if( m_fullscreen )
	{
		auto labelFont = m_label->font();
		labelFont.setPointSize( 28 );
		m_label->setFont( labelFont );
	}

	setAutoFillBackground( true );
	setStyleSheet( QStringLiteral("QWidget { background-color: #202020; } QLabel { color: #f0f0f0; }") );

	m_ticker = new QTimer( this );
	connect( m_ticker, &QTimer::timeout, this, &TimerStudentWidget::tick );
	m_ticker->start( 1000 );

	updateDisplay();

	if( m_fullscreen )
	{
		showFullScreen();
	}
	else
	{
		placeBanner();
	}
}



void TimerStudentWidget::open( int seconds, const QString& label, bool fullscreen )
{
	// un nouveau minuteur remplace le précédent : deux décomptes concurrents
	// n'auraient aucun sens à l'écran
	shutdown();

	s_instance = new TimerStudentWidget( seconds, label, fullscreen );
	s_instance->show();
	s_instance->raise();
}



void TimerStudentWidget::shutdown()
{
	if( s_instance )
	{
		s_instance->close();
		s_instance->deleteLater();
		s_instance = nullptr;
	}
}



/** Bandeau centré en haut de l'écran principal, sous la barre de titre. */
void TimerStudentWidget::placeBanner()
{
	const auto* const screen = QGuiApplication::primaryScreen();
	if( screen == nullptr )
	{
		return;
	}

	const auto available = screen->availableGeometry();
	adjustSize();

	move( available.center().x() - width() / 2, available.top() + 12 );
}



void TimerStudentWidget::tick()
{
	if( m_remainingSeconds > 0 )
	{
		--m_remainingSeconds;
	}

	updateDisplay();

	// À zéro, le décompte reste affiché : c'est le maître qui décide de la
	// suite (verrouillage ou rien). Le faire disparaître laisserait l'élève
	// sans indication que le temps est écoulé.
	if( m_remainingSeconds <= 0 )
	{
		m_ticker->stop();
	}
}



void TimerStudentWidget::updateDisplay()
{
	const auto hours = m_remainingSeconds / 3600;
	const auto minutes = ( m_remainingSeconds % 3600 ) / 60;
	const auto seconds = m_remainingSeconds % 60;

	m_countdown->setText( hours > 0
		? QStringLiteral("%1:%2:%3").arg( hours ).arg( minutes, 2, 10, QLatin1Char('0') )
			.arg( seconds, 2, 10, QLatin1Char('0') )
		: QStringLiteral("%1:%2").arg( minutes, 2, 10, QLatin1Char('0') )
			.arg( seconds, 2, 10, QLatin1Char('0') ) );

	m_countdown->setStyleSheet( m_remainingSeconds <= WarningThresholdSeconds
		? QStringLiteral("color: #ff6b6b;")
		: QStringLiteral("color: #f0f0f0;") );

	if( m_fullscreen == false )
	{
		placeBanner();		// la largeur change avec le texte
	}
}
