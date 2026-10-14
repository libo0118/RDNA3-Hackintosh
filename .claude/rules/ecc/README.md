# ECC rules

`common/` and `cpp/` are copied unchanged from ECC 2.2.3
(https://github.com/affaan-m/ECC, commit b92f181, `rules/`; MIT, see
LICENSE). The ECC plugin itself (`ecc@ecc`: skills, agents, commands,
hooks) is enabled for this repository in `.claude/settings.json`; Claude
Code plugins cannot ship rules, so these packs live here.

Where they disagree with the repository's `CLAUDE.md`, `CLAUDE.md` wins
(its "ECC" section lists the parts that do not apply to a kernel driver).
To update: copy the two directories again from a newer ECC checkout.
