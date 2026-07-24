/*
 * ChatStudentWindow.cpp - fenêtre de chat affichée dans la session utilisateur
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QScrollBar>

#include "ChatStudentWindow.h"
#include "ui_ChatStudentWindow.h"
#include "ChatPlugin.h"
#include "VeyonWorkerInterface.h"

QPointer<ChatStudentWindow> ChatStudentWindow::s_instance = nullptr;


ChatStudentWindow::ChatStudentWindow( Feature::Uid featureUid, VeyonWorkerInterface* worker,
									  bool canSendMessages, bool staysOnTop ) :
	QWidget( nullptr, staysOnTop ? ( Qt::Window | Qt::WindowStaysOnTopHint )
								 : Qt::WindowFlags( Qt::Window ) ),
	ui( new Ui::ChatStudentWindow ),
	m_featureUid( featureUid ),
	m_worker( worker )
{
	ui->setupUi( this );

	if( canSendMessages )
	{
		connect( ui->sendButton, &QPushButton::clicked, this, &ChatStudentWindow::sendMessage );
		connect( ui->messageEdit, &QLineEdit::returnPressed, this, &ChatStudentWindow::sendMessage );
	}
	else
	{
		ui->messageEdit->hide();
		ui->sendButton->hide();
	}
}



ChatStudentWindow::~ChatStudentWindow()
{
	delete ui;
}



void ChatStudentWindow::open( Feature::Uid featureUid, VeyonWorkerInterface* worker,
							  bool canSendMessages, bool staysOnTop )
{
	if( s_instance.isNull() )
	{
		s_instance = new ChatStudentWindow( featureUid, worker, canSendMessages, staysOnTop );
	}

	s_instance->popup();
}



void ChatStudentWindow::appendTeacherMessage( const QString& text )
{
	if( s_instance.isNull() )
	{
		return;
	}

	s_instance->appendMessage( tr( "Teacher" ), text, false );
	s_instance->popup();
}



void ChatStudentWindow::shutdown()
{
	if( s_instance )
	{
		s_instance->m_terminating = true;
		s_instance->close();
		s_instance->deleteLater();
		s_instance = nullptr;
	}
}



void ChatStudentWindow::closeEvent( QCloseEvent* event )
{
	// tant que le chat est actif, fermer ne fait que masquer la fenêtre :
	// elle reste disponible en arrière-plan et réapparaît au prochain message
	if( m_terminating == false )
	{
		hide();
		event->ignore();
		return;
	}

	event->accept();
}



void ChatStudentWindow::sendMessage()
{
	const auto text = ui->messageEdit->text().trimmed().left( ChatPlugin::MaximumMessageLength );
	if( text.isEmpty() )
	{
		return;
	}

	FeatureMessage reply( m_featureUid, ChatPlugin::FeatureCommand::MessageToMaster );
	reply.addArgument( ChatPlugin::Argument::Text, text );
	reply.addArgument( ChatPlugin::Argument::Timestamp,
					   QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );

	if( m_worker )
	{
		m_worker->sendFeatureMessageReply( reply );
	}

	appendMessage( tr( "Me" ), text, true );
	ui->messageEdit->clear();
}



void ChatStudentWindow::appendMessage( const QString& sender, const QString& text, bool own )
{
	const auto color = own ? QStringLiteral("#2e7d32") : QStringLiteral("#1d6fa5");

	ui->chatView->append( QStringLiteral("<p style=\"margin:2px 0\">"
										 "<span style=\"color:gray\">[%1]</span> "
										 "<b style=\"color:%2\">%3:</b> %4</p>")
							  .arg( QDateTime::currentDateTime().toString( QStringLiteral("hh:mm") ),
									color,
									sender.toHtmlEscaped(),
									QString( text ).toHtmlEscaped()
										.replace( QLatin1Char('\n'), QStringLiteral("<br>") ) ) );

	ui->chatView->verticalScrollBar()->setValue( ui->chatView->verticalScrollBar()->maximum() );
}



void ChatStudentWindow::popup()
{
	if( isMinimized() )
	{
		showNormal();
	}
	else
	{
		show();
	}

	raise();
	activateWindow();
	QApplication::alert( this );
}
