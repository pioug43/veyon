# Plugin Quiz — questionnaire noté

L'exercice noté en plusieurs questions, là où le plugin `survey` couvre la
question unique posée à la volée. L'élève répond dans un dialogue séquentiel,
une question à la fois, avec compte à rebours si une durée est fixée.

Fonctionnalité `Quiz` — UUID `a2f5b681-3c70-4e94-8d25-16b0e7c3f958`.

## Le corrigé ne descend jamais sur le poste

Le plugin ne transporte qu'énoncés et propositions. La correction est faite par
l'appelant, à partir des réponses brutes. Un corrigé présent sur la machine de
l'élève serait lisible par qui sait regarder.

Cette garantie n'est pas un filtrage mais une **reconstruction** : chaque
question reçue est réécrite à partir d'une liste blanche de champs
(`id`, `type`, `text`, `options`). Un champ inattendu — `correct`, `solution`,
`points`… — ne peut donc pas passer, même par inadvertance de l'appelant.

## Pilotage par la Web API

```http
PUT /api/v1/feature/a2f5b681-3c70-4e94-8d25-16b0e7c3f958
{
  "active": true,
  "arguments": {
    "quizId": "42",
    "durationSeconds": 600,
    "questions": "[{\"id\":\"1\",\"type\":\"single\",\"text\":\"Capitale de la France ?\",\"options\":[\"Paris\",\"Lyon\"]}]"
  }
}
```

`questions` est un tableau JSON **passé en chaîne** — la forme naturelle pour
transporter une structure imbriquée dans un argument de fonctionnalité.

| Champ | Valeurs |
|---|---|
| `type` | `single` (un choix), `multiple` (plusieurs), `text` (réponse libre) |
| `options` | au moins deux propositions, sauf pour `text` |
| `durationSeconds` | `0` = pas de limite ; borné à 6 h |

Une question sans énoncé, ou à choix avec moins de deux propositions, est
écartée. Si aucune question n'est exploitable, l'envoi est refusé.

## Relève des réponses

```http
GET /api/v1/feature/a2f5b681-3c70-4e94-8d25-16b0e7c3f958
→ {
    "active": true,
    "status": {
      "quizId": "42",
      "answers": {
        "1": { "answer": "Paris", "answeredAt": "2026-07-25T09:12:44Z" }
      },
      "finished": false
    }
  }
```

Chaque réponse remonte **dès le passage à la question suivante**, pas à la fin :
un poste coupé en cours d'épreuve ne fait pas perdre ce qui a déjà été répondu.
À l'expiration du temps, la réponse en cours est envoyée avant la clôture.

Pour une question à choix multiple, `answer` contient les libellés retenus
séparés par des virgules. La correspondance avec le corrigé est à la charge de
l'appelant, qui décide de la tolérance (casse, espaces, ordre).

## Fermeture

```http
PUT /api/v1/feature/a2f5b681-3c70-4e94-8d25-16b0e7c3f958   { "active": false }
```
