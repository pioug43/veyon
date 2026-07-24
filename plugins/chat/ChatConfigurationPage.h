/*
 * ChatConfigurationPage.h - page de configuration du plugin Chat
 * (Veyon Configurator)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "ConfigurationPage.h"

namespace Ui { class ChatConfigurationPage; }

class ChatConfiguration;

class ChatConfigurationPage : public ConfigurationPage
{
	Q_OBJECT
public:
	explicit ChatConfigurationPage( ChatConfiguration& configuration, QWidget* parent = nullptr );
	~ChatConfigurationPage() override;

	void resetWidgets() override;
	void connectWidgetsToProperties() override;
	void applyConfiguration() override;

private:
	Ui::ChatConfigurationPage* ui;

	ChatConfiguration& m_configuration;
};
