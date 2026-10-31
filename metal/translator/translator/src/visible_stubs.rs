//! Metal visible-function stubs found by name, and the record of what one link did with them.
//!
//! A kernel that calls a `[[visible]]` function it does not define calls a stub,
//! `@<symbol>.MTL_VISIBLE_FN_REF`, declared `section "air.externally_defined"`. AIR normally
//! lists every stub in `!air.visible_function_references`, and `linked_functions` resolves the
//! stubs it finds there. A stub the list does not name (an AIR whose list is missing or written in
//! a form the metadata parser does not read) used to survive linking untouched, and the emitter
//! then refused the whole kernel ("unsupported Metal visible function reference") although the
//! pipeline supplied the function: MPS's `AMDConvolution::ndArrayConvolution2DAMD_NHWC` on the
//! bare-metal Mac, 2026-10-31. Finding the stubs by their suffix as well closes that gap.
//!
//! The report is the per-thread record of one link (`reset`, `note_*`, `take`): the plugin reads it
//! through `nvmtl_link_report` after `nvmtl_link_visible` / `nvmtl_link_runtime` and logs it, so a
//! card or Mac report names every visible function that was linked, devirtualised or left out.

use std::cell::RefCell;
use std::collections::BTreeSet;

pub(crate) const VISIBLE_STUB_SUFFIX: &str = ".MTL_VISIBLE_FN_REF";

/// One stub global as it is spelled in the module, and the visible function it stands for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct SuffixStub {
    pub(crate) global: String,
    pub(crate) symbol: String,
}

fn plain(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'.' | b'$' | b'-')
}

fn decode(encoded: &str) -> Option<String> {
    let bytes = encoded.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'\\' {
            let hex = std::str::from_utf8(bytes.get(i + 1..i + 3)?).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

/// Every global `@...MTL_VISIBLE_FN_REF...` the module spells, quoted or not, first use first.
/// Text that is not a global (a metadata string such as `!"f.MTL_VISIBLE_FN_REF"`) is not a stub.
pub(crate) fn suffix_stubs(module: &str) -> Vec<SuffixStub> {
    let bytes = module.as_bytes();
    let mut seen = BTreeSet::<String>::new();
    let mut stubs = Vec::new();
    for (at, _) in module.match_indices(VISIBLE_STUB_SUFFIX) {
        let after = at + VISIBLE_STUB_SUFFIX.len();
        let mut start = at;
        while start > 0 && plain(bytes[start - 1]) {
            start -= 1;
        }
        let found = if start > 0 && bytes[start - 1] == b'@' && start < at {
            let mut end = after;
            while end < bytes.len() && plain(bytes[end]) {
                end += 1;
            }
            Some((module[start - 1..end].to_string(), module[start..at].to_string()))
        } else {
            let line_start = module[..at].rfind('\n').map_or(0, |n| n + 1);
            module[line_start..at]
                .rfind('"')
                .map(|q| line_start + q)
                .filter(|&q| q > 0 && bytes[q - 1] == b'@')
                .and_then(|q| {
                    let close = after + module[after..].find(|c: char| c == '"' || c == '\n')?;
                    if bytes[close] != b'"' || q + 1 >= at {
                        return None;
                    }
                    let symbol = decode(&module[q + 1..at])?;
                    Some((module[q - 1..=close].to_string(), symbol))
                })
        };
        let Some((global, symbol)) = found else {
            continue;
        };
        if !symbol.is_empty() && seen.insert(global.clone()) {
            stubs.push(SuffixStub { global, symbol });
        }
    }
    stubs
}

/// What one link did with the visible functions it met.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LinkReport {
    /// Stubs replaced by a direct call to a linked function.
    pub linked: BTreeSet<String>,
    /// Stubs no linked function supplies: renamed `.MTL_UNRESOLVED_VISIBLE_FN`, refused if called.
    pub unresolved: BTreeSet<String>,
    /// Stubs found by their suffix only, not in `!air.visible_function_references`.
    pub unlisted: BTreeSet<String>,
    /// Table slots read with a constant index, linked as a direct call.
    pub table_direct: BTreeSet<String>,
    /// Functions behind a table read with a run-time index, reached through a switch dispatcher.
    pub table_dispatched: BTreeSet<String>,
}

thread_local! {
    static REPORT: RefCell<LinkReport> = RefCell::new(LinkReport::default());
}

pub fn reset() {
    REPORT.with(|r| *r.borrow_mut() = LinkReport::default());
}

pub fn take() -> LinkReport {
    REPORT.with(|r| std::mem::take(&mut *r.borrow_mut()))
}

fn note(f: impl FnOnce(&mut LinkReport)) {
    REPORT.with(|r| f(&mut *r.borrow_mut()));
}

pub(crate) fn note_linked(symbol: &str) {
    note(|r| {
        r.linked.insert(symbol.to_string());
    });
}

pub(crate) fn note_unresolved(symbol: &str) {
    note(|r| {
        r.unresolved.insert(symbol.to_string());
    });
}

pub(crate) fn note_unlisted(symbol: &str) {
    note(|r| {
        r.unlisted.insert(symbol.to_string());
    });
}

pub(crate) fn note_table_direct(symbol: &str) {
    note(|r| {
        r.table_direct.insert(symbol.to_string());
    });
}

pub(crate) fn note_table_dispatched(symbol: &str) {
    note(|r| {
        r.table_dispatched.insert(symbol.to_string());
    });
}

const LISTED_NAMES: usize = 16;

fn names(set: &BTreeSet<String>) -> String {
    let mut text = set
        .iter()
        .take(LISTED_NAMES)
        .map(String::as_str)
        .collect::<Vec<_>>()
        .join(", ");
    if set.len() > LISTED_NAMES {
        text.push_str(&format!(", +{} more", set.len() - LISTED_NAMES));
    }
    text
}

impl LinkReport {
    /// One line for the plugin's log, e.g.
    /// `linked 2 [a, b]; unresolved 1 [c]; table const-slot 0 []; table dispatch 0 []; found without !air.visible_function_references 0 []`.
    pub fn summary(&self) -> String {
        format!(
            "linked {} [{}]; unresolved {} [{}]; table const-slot {} [{}]; table dispatch {} [{}]; \
             found without !air.visible_function_references {} [{}]",
            self.linked.len(),
            names(&self.linked),
            self.unresolved.len(),
            names(&self.unresolved),
            self.table_direct.len(),
            names(&self.table_direct),
            self.table_dispatched.len(),
            names(&self.table_dispatched),
            self.unlisted.len(),
            names(&self.unlisted),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_plain_quoted_and_uniqued_stubs_but_not_metadata_strings() {
        let module = r#"define void @main() {
  call void @a.MTL_VISIBLE_FN_REF()
  call float @"AMDConvolution::act.MTL_VISIBLE_FN_REF"(float 1.0)
  call void @b.MTL_VISIBLE_FN_REF.1()
  call void @a.MTL_VISIBLE_FN_REF()
  ret void
}
!0 = !{!"c.MTL_VISIBLE_FN_REF"}
"#;
        let stubs = suffix_stubs(module);
        assert_eq!(
            stubs,
            vec![
                SuffixStub {
                    global: "@a.MTL_VISIBLE_FN_REF".into(),
                    symbol: "a".into()
                },
                SuffixStub {
                    global: "@\"AMDConvolution::act.MTL_VISIBLE_FN_REF\"".into(),
                    symbol: "AMDConvolution::act".into()
                },
                SuffixStub {
                    global: "@b.MTL_VISIBLE_FN_REF.1".into(),
                    symbol: "b".into()
                },
            ]
        );
    }

    #[test]
    fn decodes_escapes_in_quoted_stub_names() {
        let stubs = suffix_stubs("call void @\"x\\22y.MTL_VISIBLE_FN_REF\"()\n");
        assert_eq!(stubs.len(), 1);
        assert_eq!(stubs[0].symbol, "x\"y");
    }

    #[test]
    fn the_report_names_and_counts_each_decision() {
        reset();
        note_linked("b");
        note_linked("a");
        note_unresolved("c");
        note_unlisted("a");
        let summary = take().summary();
        assert!(summary.starts_with("linked 2 [a, b]; unresolved 1 [c];"), "{summary}");
        assert!(summary.ends_with("references 1 [a]"), "{summary}");
        assert_eq!(take(), LinkReport::default());
    }
}
