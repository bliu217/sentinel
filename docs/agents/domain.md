# Domain docs

## Before exploring

Read `GLOSSARY.md` at the repo root and relevant decisions in `docs/adr/`.

If these files do not exist, proceed silently. Domain modeling creates
them lazily when terminology or decisions are resolved.

## Layout

This repository uses a single-context layout:

- `GLOSSARY.md`: shared domain vocabulary.
- `docs/adr/`: architecture decision records.

## Vocabulary

Use glossary terms in issue titles, proposals, hypotheses, and tests.
If a needed concept is missing, reconsider the term or note the gap
for domain modeling.

## Decision conflicts

Explicitly flag proposals that contradict an existing ADR, identifying
the decision and explaining why it should be reconsidered.
