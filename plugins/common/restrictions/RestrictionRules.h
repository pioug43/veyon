/*
 * RestrictionRules.h - shared rule model for restriction backends
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Le modèle de règles (applications, domaines, règles URL ordonnées, génération
 * du PAC) est né dans le plugin exammode ; il est désormais partagé avec le
 * plugin courserestrictions. Les fichiers ExamModeProfile.{h,cpp} ont été
 * déplacés ici SANS modification de code afin de garantir l'absence de
 * régression sur le mode examen (brique critique en production).
 *
 * TODO (une fois la chaîne de build validée) : renommer le namespace
 * ExamModeProfile en RestrictionRules et supprimer cet alias.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include "ExamModeProfile.h"

// Nom neutre à utiliser dans tout code non spécifique au mode examen.
namespace RestrictionRules = ExamModeProfile;
