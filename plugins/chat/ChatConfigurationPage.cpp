/*
 * ChatConfigurationPage.cpp - page de configuration du plugin Chat
 * (Veyon Configurator)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "ChatConfiguration.h"
#include "ChatConfigurationPage.h"
#include "Configuration/UiMapping.h"

#include "ui_ChatConfigurationPage.h"


ChatConfigurationPage::ChatConfigurationPage( ChatConfiguration& configuration, QWidget* parent ) :
	ConfigurationPage( parent ),
	ui( new Ui::ChatConfigurationPage ),
	m_configuration( configuration )
{
	ui->setupUi( this );
}



ChatConfigurationPage::~ChatConfigurationPage()
{
	delete ui;
}



void ChatConfigurationPage::resetWidgets()
{
	FOREACH_CHAT_CONFIG_PROPERTY(INIT_WIDGET_FROM_PROPERTY);
}



void ChatConfigurationPage::connectWidgetsToProperties()
{
	FOREACH_CHAT_CONFIG_PROPERTY(CONNECT_WIDGET_TO_PROPERTY)
}



void ChatConfigurationPage::applyConfiguration()
{
}
