/*
 * CourseRestrictionsConfigurationPage.cpp - page de configuration du plugin
 * CourseRestrictions (Veyon Configurator)
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <QJsonArray>
#include <QTableWidgetItem>

#include "Configuration/UiMapping.h"
#include "CourseRestrictionsConfiguration.h"
#include "CourseRestrictionsConfigurationPage.h"
#include "CourseRestrictionsProfile.h"

#include "ui_CourseRestrictionsConfigurationPage.h"

namespace
{

enum Column
{
	NameColumn,
	ApplicationsColumn,
	WebsitesColumn,
	AllowListColumn,
};

}


CourseRestrictionsConfigurationPage::CourseRestrictionsConfigurationPage(
		CourseRestrictionsConfiguration& configuration, QWidget* parent ) :
	ConfigurationPage( parent ),
	ui( new Ui::CourseRestrictionsConfigurationPage ),
	m_configuration( configuration )
{
	ui->setupUi( this );

	connect( ui->addProfile, &QPushButton::clicked,
			 this, &CourseRestrictionsConfigurationPage::addProfile );
	connect( ui->removeProfile, &QPushButton::clicked,
			 this, &CourseRestrictionsConfigurationPage::removeProfile );
	connect( ui->profileTable, &QTableWidget::itemChanged, this, [this]() {
		if( m_loadingProfiles == false )
		{
			saveProfiles();
		}
	} );
}



CourseRestrictionsConfigurationPage::~CourseRestrictionsConfigurationPage()
{
	delete ui;
}



void CourseRestrictionsConfigurationPage::resetWidgets()
{
	FOREACH_COURSE_RESTRICTIONS_CONFIG_PROPERTY(INIT_WIDGET_FROM_PROPERTY);

	loadProfiles();
}



void CourseRestrictionsConfigurationPage::connectWidgetsToProperties()
{
	FOREACH_COURSE_RESTRICTIONS_CONFIG_PROPERTY(CONNECT_WIDGET_TO_PROPERTY)
}



void CourseRestrictionsConfigurationPage::applyConfiguration()
{
}



void CourseRestrictionsConfigurationPage::loadProfiles()
{
	m_loadingProfiles = true;

	ui->profileTable->setRowCount( 0 );

	int row = 0;
	const auto profiles = m_configuration.courseRestrictionsProfiles();
	for( const auto& value : profiles )
	{
		const CourseRestrictionsProfile profile{ value.toObject() };

		auto* const nameItem = new QTableWidgetItem( profile.name() );
		nameItem->setData( Qt::UserRole, profile.uid() );

		auto* const allowListItem = new QTableWidgetItem;
		allowListItem->setFlags( ( allowListItem->flags() | Qt::ItemIsUserCheckable ) & ~Qt::ItemIsEditable );
		allowListItem->setCheckState( profile.websiteMode() == QStringLiteral("allow") ?
										  Qt::Checked : Qt::Unchecked );
		allowListItem->setToolTip( tr( "When checked, only the listed websites remain reachable "
									   "(requires the PAC backend, i.e. Windows)." ) );

		ui->profileTable->setRowCount( row+1 );
		ui->profileTable->setItem( row, NameColumn, nameItem );
		ui->profileTable->setItem( row, ApplicationsColumn,
			new QTableWidgetItem( CourseRestrictionsProfile::joinList( profile.blockedApplications() ) ) );
		ui->profileTable->setItem( row, WebsitesColumn,
			new QTableWidgetItem( CourseRestrictionsProfile::joinList( profile.websites() ) ) );
		ui->profileTable->setItem( row, AllowListColumn, allowListItem );

		++row;
	}

	m_loadingProfiles = false;
}



void CourseRestrictionsConfigurationPage::saveProfiles()
{
	QJsonArray profiles;

	for( int row = 0; row < ui->profileTable->rowCount(); ++row )
	{
		const auto* const nameItem = ui->profileTable->item( row, NameColumn );
		const auto* const applicationsItem = ui->profileTable->item( row, ApplicationsColumn );
		const auto* const websitesItem = ui->profileTable->item( row, WebsitesColumn );
		const auto* const allowListItem = ui->profileTable->item( row, AllowListColumn );

		if( nameItem == nullptr || nameItem->text().trimmed().isEmpty() )
		{
			continue;		// une ligne sans nom n'est pas un profil exploitable
		}

		const auto profile = CourseRestrictionsProfile::fromValues(
			nameItem->text(),
			applicationsItem ? applicationsItem->text() : QString{},
			websitesItem ? websitesItem->text() : QString{},
			allowListItem && allowListItem->checkState() == Qt::Checked ?
				QStringLiteral("allow") : QStringLiteral("block"),
			nameItem->data( Qt::UserRole ).toUuid() );

		profiles.append( profile.toJson() );
	}

	m_configuration.setCourseRestrictionsProfiles( profiles );
}



void CourseRestrictionsConfigurationPage::addProfile()
{
	auto profiles = m_configuration.courseRestrictionsProfiles();

	profiles.append( CourseRestrictionsProfile::fromValues( tr( "New profile" ), {}, {},
														   QStringLiteral("block") ).toJson() );

	m_configuration.setCourseRestrictionsProfiles( profiles );

	loadProfiles();

	ui->profileTable->setCurrentCell( ui->profileTable->rowCount()-1, NameColumn );
}



void CourseRestrictionsConfigurationPage::removeProfile()
{
	const auto row = ui->profileTable->currentRow();
	if( row < 0 )
	{
		return;
	}

	ui->profileTable->removeRow( row );

	saveProfiles();
}
