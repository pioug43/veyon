/*
 * ChatMasterWindow.cpp - fenêtre de conversation côté enseignant
 *
 * Arbre des postes à gauche, regroupés par pool VDI (location Veyon) avec
 * « All computers » en racine ; conversation à droite. Les messages élèves
 * arrivent via le signal ChatPlugin::chatMessageReceived ; l'envoi passe
 * par ChatPlugin::sendMessageToStudents.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QApplication>
#include <QCloseEvent>
#include <QScrollBar>
#include <QTreeWidgetItem>

#include "ChatMasterWindow.h"
#include "ui_ChatMasterWindow.h"
#include "ChatPlugin.h"

namespace {
constexpr auto PoolNameRole = Qt::UserRole;
constexpr auto TargetPointerRole = Qt::UserRole + 1;
}


ChatMasterWindow::ChatMasterWindow( ChatPlugin* plugin, QWidget* parent ) :
	QWidget( parent, Qt::Window ),
	ui( new Ui::ChatMasterWindow ),
	m_plugin( plugin )
{
	ui->setupUi( this );
	setAttribute( Qt::WA_DeleteOnClose );

	m_allComputersItem = new QTreeWidgetItem( ui->computerTree, { tr( "All computers" ) } );
	ui->computerTree->setCurrentItem( m_allComputersItem );

	connect( ui->sendButton, &QPushButton::clicked, this, &ChatMasterWindow::sendMessage );
	connect( ui->messageEdit, &QLineEdit::returnPressed, this, &ChatMasterWindow::sendMessage );

	connect( ui->computerTree, &QTreeWidget::currentItemChanged, this, [this]( QTreeWidgetItem* current ) {
		if( current )
		{
			auto font = current->font( 0 );
			font.setBold( false );
			current->setFont( 0, font );
		}
		rebuildView();
	} );

	connect( m_plugin, &ChatPlugin::chatMessageReceived,
			 this, &ChatMasterWindow::appendStudentMessage );
}



ChatMasterWindow::~ChatMasterWindow()
{
	delete ui;
}



void ChatMasterWindow::addTargets( const ComputerControlInterfaceList& computerControlInterfaces )
{
	for( const auto& controlInterface : computerControlInterfaces )
	{
		if( m_targets.contains( controlInterface ) )
		{
			continue;
		}

		m_targets.append( controlInterface );

		auto* item = new QTreeWidgetItem( poolItem( poolOf( controlInterface ) ),
										  { displayName( controlInterface.data() ) } );
		item->setData( 0, TargetPointerRole,
					   QVariant::fromValue( reinterpret_cast<quintptr>( controlInterface.data() ) ) );
		m_computerItems.insert( controlInterface.data(), item );
	}

	ui->computerTree->expandAll();
}



void ChatMasterWindow::removeTargets( const ComputerControlInterfaceList& computerControlInterfaces )
{
	for( const auto& controlInterface : computerControlInterfaces )
	{
		const auto index = m_targets.indexOf( controlInterface );
		if( index < 0 )
		{
			continue;
		}

		m_targets.removeAt( index );

		if( auto* item = m_computerItems.take( controlInterface.data() ) )
		{
			auto* pool = item->parent();
			delete item;
			if( pool && pool->childCount() == 0 )
			{
				delete pool;
			}
		}
	}

	if( m_targets.isEmpty() )
	{
		close();
	}
}



void ChatMasterWindow::closeEvent( QCloseEvent* event )
{
	// fermer la fenêtre met fin au chat sur les postes encore ciblés
	if( m_targets.isEmpty() == false )
	{
		m_plugin->controlFeature( m_plugin->chatFeature().uid(),
								  FeatureProviderInterface::Operation::Stop,
								  {}, m_targets );
		m_targets.clear();
	}

	event->accept();
}



void ChatMasterWindow::appendStudentMessage( ComputerControlInterface::Pointer computerControlInterface,
											 const QString& text, const QDateTime& timestamp )
{
	addTargets( { computerControlInterface } );

	m_log.append( Entry{ computerControlInterface, {}, false,
						 displayName( computerControlInterface.data() ), text, timestamp } );

	if( entryVisible( m_log.constLast(), currentSelection() ) )
	{
		rebuildView();
	}
	else
	{
		markUnread( m_computerItems.value( computerControlInterface.data() ) );
	}

	QApplication::alert( this );
}



void ChatMasterWindow::sendMessage()
{
	const auto text = ui->messageEdit->text().trimmed().left( ChatPlugin::MaximumMessageLength );
	if( text.isEmpty() || m_targets.isEmpty() )
	{
		return;
	}

	const auto selection = currentSelection();

	ComputerControlInterfaceList recipients;
	switch( selection.kind )
	{
	case SelectionKind::All:
		recipients = m_targets;
		break;
	case SelectionKind::Pool:
		for( const auto& controlInterface : std::as_const(m_targets) )
		{
			if( poolOf( controlInterface ) == selection.pool )
			{
				recipients.append( controlInterface );
			}
		}
		break;
	case SelectionKind::Computer:
		recipients.append( selection.target );
		break;
	}

	if( recipients.isEmpty() )
	{
		return;
	}

	m_plugin->sendMessageToStudents( text, recipients );

	m_log.append( Entry{ selection.target,
						 selection.kind == SelectionKind::Pool ? selection.pool : QString{},
						 true, tr( "Me" ), text, QDateTime::currentDateTimeUtc() } );

	ui->messageEdit->clear();
	rebuildView();
}



void ChatMasterWindow::rebuildView()
{
	const auto selection = currentSelection();

	QString html;

	for( const auto& entry : std::as_const(m_log) )
	{
		if( entryVisible( entry, selection ) == false )
		{
			continue;
		}

		auto sender = entry.sender;
		// hors de la vue du destinataire exact, préciser la portée des
		// messages envoyés à un pool ou à un poste précis
		if( entry.fromTeacher && selection.kind != SelectionKind::Computer )
		{
			if( entry.pool.isEmpty() == false )
			{
				sender = tr( "Me → pool %1" ).arg( entry.pool );
			}
			else if( entry.target.isNull() == false )
			{
				sender = tr( "Me → %1" ).arg( displayName( entry.target.data() ) );
			}
		}

		const auto color = entry.fromTeacher ? QStringLiteral("#1d6fa5") : QStringLiteral("#2e7d32");

		html += QStringLiteral("<p style=\"margin:2px 0\">"
							   "<span style=\"color:gray\">[%1]</span> "
							   "<b style=\"color:%2\">%3:</b> %4</p>")
					.arg( entry.timestamp.toLocalTime().toString( QStringLiteral("hh:mm") ),
						  color,
						  sender.toHtmlEscaped(),
						  entry.text.toHtmlEscaped().replace( QLatin1Char('\n'), QStringLiteral("<br>") ) );
	}

	ui->chatView->setHtml( html );
	ui->chatView->verticalScrollBar()->setValue( ui->chatView->verticalScrollBar()->maximum() );
}



ChatMasterWindow::Selection ChatMasterWindow::currentSelection() const
{
	Selection selection;

	auto* item = ui->computerTree->currentItem();
	if( item == nullptr || item == m_allComputersItem )
	{
		return selection;
	}

	if( item->parent() == nullptr )
	{
		selection.kind = SelectionKind::Pool;
		selection.pool = item->data( 0, PoolNameRole ).toString();
		return selection;
	}

	const auto pointer = reinterpret_cast<const ComputerControlInterface*>(
		item->data( 0, TargetPointerRole ).value<quintptr>() );

	for( const auto& controlInterface : m_targets )
	{
		if( controlInterface.data() == pointer )
		{
			selection.kind = SelectionKind::Computer;
			selection.target = controlInterface;
			break;
		}
	}

	return selection;
}



bool ChatMasterWindow::entryVisible( const Entry& entry, const Selection& selection ) const
{
	switch( selection.kind )
	{
	case SelectionKind::All:
		return true;

	case SelectionKind::Pool:
		if( entry.fromTeacher )
		{
			return ( entry.target.isNull() && entry.pool.isEmpty() ) ||    // diffusion générale
				   entry.pool == selection.pool ||                         // diffusion à ce pool
				   ( entry.target.isNull() == false &&
					 poolOf( entry.target ) == selection.pool );           // message à un poste du pool
		}
		return entry.target.isNull() == false && poolOf( entry.target ) == selection.pool;

	case SelectionKind::Computer:
		if( entry.fromTeacher )
		{
			return ( entry.target.isNull() && entry.pool.isEmpty() ) ||
				   ( entry.pool.isEmpty() == false && entry.pool == poolOf( selection.target ) ) ||
				   entry.target == selection.target;
		}
		return entry.target == selection.target;
	}

	return false;
}



void ChatMasterWindow::markUnread( QTreeWidgetItem* item )
{
	while( item )
	{
		auto font = item->font( 0 );
		font.setBold( true );
		item->setFont( 0, font );
		item = item->parent();
	}
}



QTreeWidgetItem* ChatMasterWindow::poolItem( const QString& pool )
{
	for( int i = 0; i < ui->computerTree->topLevelItemCount(); ++i )
	{
		auto* item = ui->computerTree->topLevelItem( i );
		if( item != m_allComputersItem && item->data( 0, PoolNameRole ).toString() == pool )
		{
			return item;
		}
	}

	auto* item = new QTreeWidgetItem( ui->computerTree,
									  { pool.isEmpty() ? tr( "Without pool" ) : pool } );
	item->setData( 0, PoolNameRole, pool );

	return item;
}



QString ChatMasterWindow::poolOf( const ComputerControlInterface::Pointer& computerControlInterface )
{
	return computerControlInterface.isNull() ? QString{}
											 : computerControlInterface->computer().location();
}



QString ChatMasterWindow::displayName( const ComputerControlInterface* computerControlInterface )
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
