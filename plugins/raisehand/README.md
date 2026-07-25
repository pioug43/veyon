# Plugin RaiseHand — demandes d'aide

Pendant que l'enseignant active ce mode, chaque poste affiche un petit bouton
flottant par lequel l'élève signale qu'il a besoin d'aide.

## Deux fonctionnalités

| Nom | UUID | Rôle |
|---|---|---|
| `RaiseHand` | `0a5c9e37-4b62-4f18-9d3a-8e7c1b204f56` | le mode : bouton affiché ou non |
| `HandRaised` | `b83f1d95-6e07-42ca-8b41-59d0a7c3e218` | fonctionnalité **méta**, déclarée active par le poste tant qu'une demande est en attente |

`HandRaised` n'a pas d'action propre. Elle existe pour que la console fasse
apparaître une icône sur la vignette du poste concerné : Veyon dessine déjà une
icône par fonctionnalité active, il n'y avait donc rien à ajouter au cœur de la
console. Elle est volontairement **sans parent** — une fonctionnalité fille
portant le drapeau `Master` deviendrait une entrée parasite du menu déroulant.

## Pilotage par la Web API

```http
PUT /api/v1/feature/0a5c9e37-4b62-4f18-9d3a-8e7c1b204f56   { "active": true }
```

Le bouton apparaît sur les postes. Pour le retirer : `{"active": false}`.

État par poste :

```http
GET /api/v1/feature/0a5c9e37-4b62-4f18-9d3a-8e7c1b204f56
→ { "active": true, "status": { "handRaised": true } }
```

`handRaised` est vrai tant que la demande n'a pas été traitée. Depuis la
console, « Traiter » réarme le bouton de l'élève ; l'élève peut aussi retirer sa
demande lui-même.

## Limites

- Le poste de l'enseignant n'affiche jamais le bouton (`removeLocalHostInterfaces`).
- Le bouton vit dans la session utilisateur : un poste sans session ouverte ne
  l'affiche pas.
