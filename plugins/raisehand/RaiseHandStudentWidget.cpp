/*
 * RaiseHandStudentWidget.cpp - bouton flottant « demander de l'aide »
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>

#include "FeatureMessage.h"
#include "RaiseHandPlugin.h"
#include "RaiseHandStudentWidget.h"
#include "VeyonWorkerInterface.h"

QPointer<RaiseHandStudentWidget> RaiseHandStudentWidget::s_instance = nullptr;


RaiseHandStudentWidget::RaiseHandStudentWidget( Feature::Uid featureUid, VeyonWorkerInterface* worker ) :
	QWidget( nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint ),
	m_featureUid( featureUid ),
	m_worker( worker )
{
	setAttribute( Qt::WA_ShowWithoutActivating );		// ne pas voler le focus de l'élève
	setWindowTitle( tr( "Ask for help" ) );

	auto* const layout = new QHBoxLayout( this );
	layout->setContentsMargins( 4, 4, 4, 4 );

	m_button = new QPushButton( this );
	m_button->setCursor( Qt::PointingHandCursor );
	layout->addWidget( m_button );

	connect( m_button, &QPushButton::clicked, this, &RaiseHandStudentWidget::toggleHand );

	updateButton();
	moveToDefaultPosition();
}



void RaiseHandStudentWidget::open( Feature::Uid featureUid, VeyonWorkerInterface* worker )
{
	if( s_instance.isNull() )
	{
		s_instance = new RaiseHandStudentWidget( featureUid, worker );
	}
	else
	{
		// Réactivation du mode : le maître repart d'une file vide, donc une
		// demande encore affichée ici serait invisible pour lui. Réarmer le
		// bouton évite à l'élève de devoir cliquer deux fois pour être vu.
		s_instance->m_raised = false;
		s_instance->updateButton();
	}

	s_instance->show();
	s_instance->raise();
}



void RaiseHandStudentWidget::clearRequest()
{
	if( s_instance.isNull() == false )
	{
		s_instance->m_raised = false;
		s_instance->updateButton();
	}
}



void RaiseHandStudentWidget::shutdown()
{
	if( s_instance )
	{
		s_instance->close();
		s_instance->deleteLater();
		s_instance = nullptr;
	}
}



/** Place le bouton en bas à droite de l'écran principal, hors de la zone de travail. */
void RaiseHandStudentWidget::moveToDefaultPosition()
{
	const auto* const screen = QGuiApplication::primaryScreen();
	if( screen == nullptr )
	{
		return;
	}

	const auto available = screen->availableGeometry();
	adjustSize();

	move( available.right() - width() - 24, available.bottom() - height() - 24 );
}



void RaiseHandStudentWidget::updateButton()
{
	if( m_raised )
	{
		m_button->setText( tr( "✋ Help requested — click to cancel" ) );
		m_button->setToolTip( tr( "Your teacher has been notified. Click to withdraw your request." ) );
	}
	else
	{
		m_button->setText( tr( "✋ Ask for help" ) );
		m_button->setToolTip( tr( "Let your teacher know that you need help." ) );
	}

	adjustSize();
}



void RaiseHandStudentWidget::toggleHand()
{
	m_raised = ! m_raised;
	updateButton();

	FeatureMessage reply{ m_featureUid, m_raised ? RaiseHandPlugin::FeatureCommand::HandRaised
												 : RaiseHandPlugin::FeatureCommand::HandLowered };
	reply.addArgument( RaiseHandPlugin::Argument::Timestamp,
					   QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );

	m_worker->sendFeatureMessageReply( reply );
}



void RaiseHandStudentWidget::mousePressEvent( QMouseEvent* event )
{
	m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();

	QWidget::mousePressEvent( event );
}



void RaiseHandStudentWidget::mouseMoveEvent( QMouseEvent* event )
{
	// déplacement à la souris : l'élève peut écarter le bouton de son travail
	if( event->buttons().testFlag( Qt::LeftButton ) )
	{
		move( event->globalPosition().toPoint() - m_dragOffset );
	}

	QWidget::mouseMoveEvent( event );
}
