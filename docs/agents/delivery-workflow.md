# Delivery workflow

## Branches and pull requests

- Default to one short-lived branch and PR per implementation ticket, for example
  `issue/8-diagnostic-retention`, based on the current `main`.
- When several tickets must land together, use one integration branch and PR for
  the spec. Identify every included ticket and assess each one's acceptance
  criteria separately.
- Keep `main` as the reviewed, passing integration point. Land implementation
  through PRs unless the user explicitly requests a direct commit.

## Specs and tickets

- Keep the parent GitHub issue as the spec's acceptance contract. Child tickets
  link to the parent and declare their blockers using the tracker conventions.
- Work unblocked tickets first. Keep each ticket self-contained enough to
  implement and verify in a fresh session.
- Reconcile changes to agreed scope or behavior with the spec and ticket before
  claiming completion. If work combines several planned stages, record that
  explicitly rather than describing each stage as independently delivered.
- Use actual newlines in issue bodies. Follow the tracker's UTF-8 body-file
  convention for multiline text.

## Verification and closure

- Compare the final diff with every acceptance criterion and run the relevant
  checks against a current build. Commit references and passing tests alone do
  not establish complete spec coverage.
- Review both spec compliance and repository coding standards before landing.
- Use `Closes #N` in a PR only for tickets whose criteria are fully satisfied;
  use `Refs #N` for partial work. Record remaining gaps in the ticket.
- Before manually closing an issue, post the implementing commit or PR links,
  acceptance evidence, and verification results, following the tracker workflow.
- Close the parent spec only when all required child work and the spec's final
  acceptance criteria are complete.

## Shared context

Keep agent guidance, specs, glossary, and ADRs in version control so future
branches and sessions inherit the same decisions. Treat local scratch files as
working material; GitHub issues remain authoritative for spec and ticket status.
