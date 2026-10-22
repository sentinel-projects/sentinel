# Folder review: resources, tests, third_party and tools

Date: 2026-10-22. Application candidate
`2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`, branch
`fix/26-release-manifests`. Personal-Brain CLI unavailable.

Maintainer authorized folder discovery and direct issue recording. Compared
findings with the current issue title/body inventory before posting. No product
fix, asset replacement, global installation, release approval, commit or push.
Previous untracked audit reports preserved.

## Findings and tracking

| Issue | Scope | Finding / classification |
| --- | --- | --- |
| [#87](https://github.com/sentinel-projects/sentinel/issues/87) | tools/terminal | Custom Cargo --root receives no adjacent mascot assets; reproduced helper bug |
| [#88](https://github.com/sentinel-projects/sentinel/issues/88) | tools/packaging | Source archive publication replaces a concurrently created output; fault/scheduling reproduction |
| [#89](https://github.com/sentinel-projects/sentinel/issues/89) | third_party | Four incomplete original-notice evidence records; existing documented B04 gate newly given a dedicated issue |
| [#90](https://github.com/sentinel-projects/sentinel/issues/90) | resources | Three embedded WAVs lack closing author/acquisition/redistribution evidence; existing B08 gate newly given a dedicated issue |

[#68 comment](https://github.com/sentinel-projects/sentinel/issues/68#issuecomment-6336566378)
adds the missing helper failure-case coverage. It explicitly preserves deliberate
safe-refusal tests: removing an Unsupported/Blocked assertion does not establish
an unimplemented product capability.

#89/#90 are not claims of newly discovered vulnerabilities or legal violations.
The known evidence gaps were already disclosed by THIRD_PARTY_NOTICES and the
SEC-P1 register. Integrity, format and version checks do not supply missing owner
records or constitute distribution clearance. No upstream legal interpretation
was newly adjudicated and no replacement notices were invented.

## Discovery coverage

- `resources`: canonical branding manifest/checker/report, runtime icon/font/sound
  inventory, font notice guidance, Lucide provenance, selected runtime resource
  consumers and CMake sound aliases; XML/image/WAV parsing and manifest hashes.
- `tests`: CMake target registration and selected isolation helpers; verification,
  performance and packaging Python suites; artifact/outcome parsing and relevant
  test gaps for the newly demonstrated tools bugs. This is not a line-by-line
  review or full execution of every C++/QML test.
- `third_party`: all provenance records and their referenced local license files,
  current Cargo lock version pairs, completeness flags, corresponding notice and
  distribution-register disclosures.
- `tools`: terminal install/stage/evaluation helpers; source archive and install
  ownership checks; branding/privacy/compile-database checks; IPC generation from
  earlier passes; verification collectors/schema/provenance paths; benchmark
  process ownership and selected sampler/IPC paths; i18n audit and live coding
  helper source. The live coding helper was not run against a model.

## Reproductions

### Custom installation root

Invoked the real install-dev.sh with temporary CARGO_HOME and --root CUSTOM.
Both cargo and sentinel were harmless PATH stubs. Cargo received the explicit
root, script exited 0, CUSTOM/share/sentinel/core-warden was absent, and the
asset symlink appeared under CARGO_HOME/share/sentinel/core-warden instead.
No real Cargo installation or personal shell/preference directory was changed.

### Source archive publication race

Invoked the actual archive() function with mocked Git metadata and a small
synthetic Git stdout stream. A scheduling/fault hook created an independent
KEEP-CONCURRENT-OUTPUT file immediately before the final Path.replace call.
The replacing rename overwrote it. This establishes the publication race, not
archive-format correctness or a real simultaneous Git change. The paired spec's
write_bytes path also lacks exclusive publication by source inspection; that
second collision was not separately executed.

Scratch probes and body files live under
`/private/tmp/sentinel-resource-tools-review`. They are local execution aids,
not permanent published artifacts or new repository test cases.

## Executed checks

- `tools/branding/check.py`: passed; 69 branding files agree with the frozen
  geometry/inventory/hash contract. Its source scan covered 5,441 files at run
  time; that number is scanner input, not a count of manually reviewed files.
- `tools/analysis/check_personal_paths.py`: 0 flagged files. This is a focused
  home-path scan, not complete privacy/secret certification.
- Resource parser checks: 50 PNG, 91 SVG, 3 WAV, one ICNS and one ICO input parsed
  successfully. These are structural/decode checks, not visual, audible/native
  playback, accessibility or every icon subrepresentation acceptance.
- All six third_party provenance records match the local supplement SHA-256
  and a name/version in cli/Cargo.lock. Four records still explicitly say
  complete_license_text=false. Upstream files were not fetched anew.
- Lucide original-notice supplement checksum matches its provenance record;
  local icon acquisition/modification provenance remains separately unverified.
- Seven scoped existing CTest targets passed: resource benchmark runner,
  verification Desktop/daemon, registry, Desktop UI collector, E2E collector,
  verification inventory and branding sources. Total scoped CTest time about
  3.5 seconds; these are mainly helper/fixture checks, not the full product suite.
- Current verification registry validates with zero errors and explicitly
  reports release_certified=false.
- `git diff --check`: passed.

## Limits and remaining depth

No full C++ build/CTest, live provider, microphone/audio playback, native terminal
rendering/install, real source-archive generation, legal clearance, artifact
publication or rights-record creation ran. Hash/version consistency cannot close
B04/B08 or the broader existing resource/toolchain provenance gates.

Remaining depth includes sanitizer/native test cells, exhaustive fixture isolation,
collector log-memory/deadline stress, physical terminal signal/cleanup behavior,
source/binary/asset compatibility of staged packages, complete license payload
review and full visual/audio resource acceptance. Current scoped passes do not
establish that these folders contain no additional defects.
