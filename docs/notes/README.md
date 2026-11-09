# Session notes

One file per session or per wall: `YYYY-MM-DD-topic.md`. The point is that work can be picked up
at any time without re-guessing what was already tried.

Minimum contents:

```markdown
# <short title: the symptom or the goal>

## Symptom
What was seen (log, address, screenshot) and how to reproduce it.

## Hypotheses
Each hypothesis, how it was tested, and the result (including the ones ruled out).

## Root cause
What was actually wrong, with evidence.

## Fix
Commits (this repo and/or the ps3recomp fork) and why it's done that way.

## Verification
How it was confirmed, and what hasn't been tested yet.
```

Don't put lifted code or game memory dumps in notes. Function addresses, register values and
short log excerpts are fine.
