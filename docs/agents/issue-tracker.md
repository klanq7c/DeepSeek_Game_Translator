# Issue Tracker: Local Markdown

Issues and PRDs for this repository live as Markdown files in `.scratch/`.

## Conventions

- One feature per directory: `.scratch/<feature-slug>/`
- The PRD is `.scratch/<feature-slug>/PRD.md`.
- Implementation issues are `.scratch/<feature-slug>/issues/<NN>-<slug>.md`,
  numbered from `01`.
- Triage state is recorded as a `Status:` line near the top of each issue file.
- Comments and conversation history are appended under a `## Comments` heading.

When a skill publishes to the issue tracker, it creates a file under the
corresponding `.scratch/<feature-slug>/` directory. When a skill fetches a
ticket, it reads the path or issue number supplied by the user.
