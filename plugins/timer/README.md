# Plugin Timer — minuteur de séance

Affiche un compte à rebours sur les postes (« fin de l'exercice dans 10 min »),
en bandeau discret en haut de l'écran ou en plein écran.

Fonctionnalité `Timer` — UUID `5e0a83c6-1d7f-4b92-8a45-c9f306e21b74`.

## Pilotage par la Web API

```http
PUT /api/v1/feature/5e0a83c6-1d7f-4b92-8a45-c9f306e21b74
{
  "active": true,
  "arguments": {
    "seconds": 900,
    "label": "Fin de l'exercice",
    "fullscreen": false
  }
}
```

`seconds` est borné côté poste entre 10 s et 6 h. `label` est facultatif
(120 caractères au plus). Pour retirer le décompte : `{"active": false}`.

## Action à l'expiration

Depuis la console, l'enseignant peut demander le **verrouillage des écrans** à
l'échéance. C'est le maître qui déclenche alors `ScreenLock` : le minuteur
n'implémente pas un second verrouillage, afin qu'il n'y ait qu'un seul
propriétaire du verrouillage — et donc une seule façon de le lever.

Par la Web API, l'action d'expiration n'est pas transmise : l'appelant
enchaîne lui-même le verrouillage s'il le souhaite, ce qui lui laisse le choix
de l'action (verrouiller, collecter les fichiers, envoyer un message…).

## Comportement à zéro

Le décompte **reste affiché** à `00:00` et passe en rouge sous une minute. Le
faire disparaître laisserait l'élève sans indication que le temps est écoulé.
