/*
 * QuizQuestions.h - normalisation des questions envoyées aux postes
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * Isolé du plugin pour être testable : c'est ici que se joue la garantie
 * « le corrigé ne descend jamais sur le poste », et une garantie de sécurité
 * qu'aucun test ne couvre n'en est pas une.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QJsonArray>

namespace QuizQuestions
{

/**
 * Reconstruit le tableau de questions à partir d'une liste blanche de champs
 * (id, type, text, options). Ce n'est délibérément PAS un filtrage : rien de
 * ce qui n'est pas explicitement recopié ne peut atteindre le poste, y compris
 * un champ de corrigé ajouté plus tard par un appelant distrait.
 *
 * Écarte les entrées inexploitables : non-objet, énoncé vide, question à choix
 * comptant moins de deux propositions. Normalise un type inconnu en « single »
 * et attribue un identifiant de position à une question qui n'en a pas.
 */
QJsonArray sanitize( const QJsonArray& questions, int maximumQuestions );

}
