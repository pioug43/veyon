/*
 * CourseRestrictionsConfiguration.h - configuration values for CourseRestrictions
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Les profils de restrictions sont stockés sous forme de tableau JSON, sur le
 * même principe que les applications/sites prédéfinis du plugin
 * desktopservices : chaque profil configuré apparaît comme une sous-fonction
 * du bouton « Restrictions de cours » dans la console.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "VeyonConfiguration.h"
#include "Configuration/Proxy.h"

#define FOREACH_COURSE_RESTRICTIONS_CONFIG_PROPERTY(OP) \
	OP(CourseRestrictionsConfiguration, m_configuration, bool, courseRestrictionsEnabled, setCourseRestrictionsEnabled, "Enabled", "CourseRestrictions", true, Configuration::Property::Flag::Standard)	\
	OP(CourseRestrictionsConfiguration, m_configuration, QJsonArray, courseRestrictionsProfiles, setCourseRestrictionsProfiles, "Profiles", "CourseRestrictions", QJsonArray(), Configuration::Property::Flag::Standard)	\

// clazy:excludeall=missing-qobject-macro

DECLARE_CONFIG_PROXY(CourseRestrictionsConfiguration, FOREACH_COURSE_RESTRICTIONS_CONFIG_PROPERTY)
