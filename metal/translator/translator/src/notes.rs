//! Notes a translation leaves for its host (DRIVER-PLAN N72): decisions that
//! change the output without failing it, such as a demoted pointer phi.
//! The CLI only had stderr; inside a Metal app stderr reaches no report, so
//! each note is also kept on the translating thread until the host takes it
//! (the wrapper's `nvmtl_translate_notes`, the plugin's log).

use std::cell::RefCell;

/// Notes beyond this many per thread are counted, not kept (a host that
/// never takes them must not grow without bound).
const MAX_KEPT: usize = 64;

thread_local! {
    static NOTES: RefCell<(Vec<String>, usize)> = const { RefCell::new((Vec::new(), 0)) };
    static SEEN: RefCell<std::collections::HashSet<String>> = RefCell::new(Default::default());
}

/// Records a note: on stderr (the CLI) and for the host.
pub fn push(note: impl Into<String>) {
    let note = note.into();
    eprintln!("{note}");
    NOTES.with(|notes| {
        let mut notes = notes.borrow_mut();
        if notes.0.len() < MAX_KEPT {
            notes.0.push(note);
        } else {
            notes.1 += 1;
        }
    });
}

/// Records a note the first time `key` is seen since the host last took
/// the notes (one translation: the primary emitter and the
/// raw-construction retry meet the same decision).
pub fn push_once(key: &str, note: impl Into<String>) {
    let first = SEEN.with(|seen| seen.borrow_mut().insert(key.to_string()));
    if first {
        push(note);
    }
}

/// Takes this thread's notes, oldest first; a last line counts the dropped.
pub fn take() -> Vec<String> {
    SEEN.with(|seen| seen.borrow_mut().clear());
    NOTES.with(|notes| {
        let (mut kept, dropped) = std::mem::take(&mut *notes.borrow_mut());
        if dropped > 0 {
            kept.push(format!(
                "metal2vulkan: {dropped} more notes dropped (over {MAX_KEPT})"
            ));
        }
        kept
    })
}
