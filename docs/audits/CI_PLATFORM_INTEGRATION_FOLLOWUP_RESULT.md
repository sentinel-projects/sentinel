# Integration CI platform follow-up

Date: 2026-10-30

## Observations

Integrated main `7cc13551b77c801283a9dca4c8ab6a04f3a632da` passed Rust IPC and CodeQL workflows. Linux, macOS, Windows and Fedora workflows failed. These four platforms also failed on baseline main `87d762c4a053e7211a9f019b6591fa78edeb8631`.

Current runs: Linux 41040802599, macOS 41040802717, Windows 41040802660, Fedora 41040802644. Baseline runs: Linux 40959792321, macOS 40959792440, Windows 40959792292, Fedora 40959792376. Runs are available at `https://github.com/sentinel-projects/sentinel/actions/runs/<run-id>`.

Windows failed-case comparison found 49 unique cases in the current run and 50 in baseline, with no newly failing case. Existing failures include process/network sandbox availability, ACL and symlink fixtures, path/newline assumptions, fonts, resource thresholds and real-HTTP fixtures. They remain unresolved; security enforcement and tests were not disabled.

## Scoped corrections

- Query the actual QML `Accessible.expanded` property instead of assuming availability from Qt version. Official Qt 6.11.3 Qt Quick attached-accessibility header lacks expanded/expandable properties; local Qt 6.12 exposes them. Supported expanded/collapsed state assertions remain enforced; keyboard behavior remains required on both.
- Wait for background-job completion with a bounded state assertion instead of a fixed 50 ms sleep.
- Install D-Bus for the isolated macOS portal fixture.
- Trust only the exact checked-out workspace in the two Fedora build containers, resolving Git ownership rejection without wildcard trust.

## Validation and limits

Local Debug build of the two changed test targets passed. Portal shortcuts, background-job persistence and desktop accessibility passed (3/3). Accessibility output confirmed expanded-state capability was available, so both state assertions executed. These tests require host service/socket access; the restricted execution environment could not bind the isolated D-Bus socket. They passed with host access.

Workflow actionlint and diff whitespace checks passed. Earlier integration ran all 180 test targets; 176 initially passed and all four failures subsequently passed after scoped corrections, with three additional UI regression targets passing.

This follow-up does not claim that remote platform CI is green. New remote runs must validate the environment corrections. Windows baseline failures require separate platform work. No live provider request, credential access, daemon restart or production-profile change was performed.
