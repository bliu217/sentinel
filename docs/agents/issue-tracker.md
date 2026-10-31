# Issue tracker: GitHub

Issues and specs live in GitHub Issues in `bliu217/sentinel`.
Use the `gh` CLI from this repository.

## Conventions

- Before creating the first issue in a session, verify the target with
  `gh repo view --json nameWithOwner`.
- Create: `gh issue create --title "..." --body-file <path>`.
  Use a UTF-8 file for multiline bodies.
- Read: `gh issue view <number> --json number,title,body,labels,comments`.
- List: `gh issue list --state open`, with appropriate label filters.
- Comment: `gh issue comment <number> --body-file <path>`.
- Apply or remove labels: `gh issue edit <number> --add-label "..."`
  or `--remove-label "..."`.
- Close: post an explanation first, then run `gh issue close <number>`.
- Use native GitHub sub-issues and issue dependencies when supported.
  Otherwise, put `Part of #<parent>` and `Blocked by: #<number>`
  at the top of the child issue body.
- An issue is unblocked when every blocker is closed.

File issues only in the user's intended repository. If `gh` resolves
 a different target, confirm the target before creating issues.

## Pull requests as a triage surface

**PRs as a request surface: no.**

## Skill operations

When a skill says "publish to the issue tracker", create a GitHub issue.
When it says "fetch the relevant ticket", read the GitHub issue.

## Wayfinding operations

- The map is an issue labelled `wayfinder:map`.
- Child tickets are sub-issues of the map. If sub-issues are unavailable,
  use a task list in the map and `Part of #<map>` in each child.
- Child labels are `wayfinder:<type>`, where type is research,
  prototype, grilling, or task.
- Choose the first open child in map order with no open blockers
  and no assignee.
- Claim it with `gh issue edit <number> --add-assignee @me`.
- Resolve it by commenting with the answer, closing the issue, and
  appending a summary and link to the map's Decisions-so-far.
