/*
 * CourseRestrictionsConfigurationPage.h - page de configuration du plugin
 * CourseRestrictions (Veyon Configurator)
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "ConfigurationPage.h"

namespace Ui { class CourseRestrictionsConfigurationPage; }

class CourseRestrictionsConfiguration;

class CourseRestrictionsConfigurationPage : public ConfigurationPage
{
	Q_OBJECT
public:
	explicit CourseRestrictionsConfigurationPage( CourseRestrictionsConfiguration& configuration,
												  QWidget* parent = nullptr );
	~CourseRestrictionsConfigurationPage() override;

	void resetWidgets() override;
	void connectWidgetsToProperties() override;
	void applyConfiguration() override;

private:
	void loadProfiles();
	void saveProfiles();
	void addProfile();
	void removeProfile();

	Ui::CourseRestrictionsConfigurationPage* ui;

	CourseRestrictionsConfiguration& m_configuration;

	// évite de réécrire la configuration pendant le remplissage du tableau
	bool m_loadingProfiles{false};
};
