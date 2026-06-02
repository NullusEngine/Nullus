# Contributing to Nullus Engine

Thank you for your interest in Nullus Engine. This project is source-available,
but it is not open source under an OSI-approved license. Please read the
licensing and contribution terms before opening an issue or pull request.

## License and CLA

By submitting a pull request, patch, issue comment, suggestion, design, asset,
or other contribution to Nullus Engine, you agree that:

- your contribution is provided under the Contributor License Grant in
  [LICENSE](./LICENSE);
- your contribution is also subject to the Contributor License Agreement in
  [CLA.md](./CLA.md);
- you have the right to submit the contribution;
- your contribution may be used, modified, redistributed, sublicensed, and
  relicensed by the Nullus Engine author as part of Nullus Engine or related
  projects.

Do not submit contributions if you cannot accept these terms.

## What to Contribute

Good contributions include:

- focused bug fixes;
- small editor or runtime improvements;
- documentation fixes;
- platform or build fixes;
- tests that cover existing behavior;
- well-scoped rendering, resource, reflection, or scene-system improvements.

Please avoid large rewrites or architecture changes without opening an issue
first. Nullus Engine is still evolving, and broad changes need discussion before
review.

## Pull Request Guidelines

Before opening a pull request:

- keep the change focused on one topic;
- follow the existing code style and naming patterns;
- include tests when the change affects behavior;
- update documentation when user-facing behavior changes;
- do not hand-edit generated files under `Runtime/*/Gen/`;
- do not add third-party code, assets, binaries, or SDK files unless their
  license is clear and compatible with this repository;
- do not include secrets, private keys, credentials, or private project data.

The project owner may edit, squash, reject, or close contributions at their
discretion.

## Generated Code

Nullus uses MetaParser to generate reflection code. Generated files are build
artifacts and should not be manually edited. Change the source declarations or
the generator instead.

## Third-Party Materials

If your contribution includes or depends on third-party material, clearly
identify it in the pull request and include its license. Contributions with
unclear third-party licensing may be rejected.

## Reporting Security Issues

Please do not disclose security-sensitive issues publicly before the project
owner has had a chance to review them. If no private security channel is listed
for the repository, open a minimal issue asking for a private contact method
without including exploit details.
