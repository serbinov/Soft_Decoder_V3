---
description: Run a whole-firmware code review with the review-lead orchestrator
agent: review-lead
subtask: true
---
Run a full read-only code review of the firmware in `$ARGUMENTS` (default:
`firmware/`). Dispatch the domain reviewers (review-dcc, review-motor,
review-web, review-storage, review-media, review-rtos), verify all CRITICAL and
HIGH findings against the source, deduplicate, and return one prioritized
report. Do not modify any files.
