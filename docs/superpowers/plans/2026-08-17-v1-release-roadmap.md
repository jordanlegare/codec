# V1 Release Roadmap README Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the approved 26-week, gate-driven path from v0.1.1 to a release-ready v1.0.0 to the normative README.

**Architecture:** Keep the existing phase descriptions intact and add one schedule section between the delivery phases and stop/redesign gates. The section maps dated releases to their exit gates, defines the required one-or-two-update weekly cadence, and makes quality gates—not calendar pressure—the promotion authority.

**Tech Stack:** GitHub-flavored Markdown, POSIX shell validation, Git.

## Global Constraints

- Schedule window: August 17, 2026 through February 12, 2027, inclusive.
- Publish one Friday status update every week.
- Publish an additional Tuesday engineering update when a build, benchmark, or milestone candidate lands.
- A failed exit gate moves the milestone; it never weakens an acceptance, safety, integrity, or identity requirement.
- v1.0.0 requires every applicable README acceptance criterion and no open critical defect.

---

### Task 1: Add and verify the release schedule

**Files:**
- Modify: `README.md`
- Create: `docs/superpowers/plans/2026-08-17-v1-release-roadmap.md`

**Interfaces:**
- Consumes: Existing `Delivery plan`, `Acceptance criteria`, and `Stop/redesign gates` sections.
- Produces: A normative `26-week path to v1.0.0` README section with milestone dates, promotion gates, update cadence, reporting schema, and final release conditions.

- [ ] **Step 1: Insert the approved roadmap**

Add the milestone table immediately after Phase 7. Use the approved dates: August 28, October 2, October 30, November 27, December 18, January 8, January 15, January 29, February 2, February 5, and February 12.

- [ ] **Step 2: Add the weekly update contract**

Require a Friday status update, allow a Tuesday engineering update when material evidence lands, and require completed work, validation evidence, performance movement, risks, next gate, schedule confidence, and immutable artifact references.

- [ ] **Step 3: Add milestone-promotion rules**

State that release dates are targets, promotion depends on exit evidence, failed gates rebaseline the schedule openly, and optional Phase 7 transport is not a v1.0.0 blocker.

- [ ] **Step 4: Validate the document**

Run:

```bash
git diff --check
rg -n "26-week path|v0\.2\.0|v0\.9\.0|v1\.0\.0|Friday status|Tuesday engineering|failed gate" README.md
for value in 2026-08-28 2026-10-02 2026-10-30 2026-11-27 2026-12-18 2027-01-08 2027-01-15 2027-01-29 2027-02-05 2027-02-12; do test "$(date -d "$value" +%A)" = Friday; done
```

Expected: no whitespace errors; every required roadmap phrase is present; every Friday milestone date validates as Friday.

- [ ] **Step 5: Review the diff and commit**

Run:

```bash
git diff -- README.md docs/superpowers/plans/2026-08-17-v1-release-roadmap.md
git add README.md docs/superpowers/plans/2026-08-17-v1-release-roadmap.md
git commit -m "docs: schedule the path to CODEC 1.0"
```

Expected: one focused documentation commit containing only the approved roadmap and its execution plan.
