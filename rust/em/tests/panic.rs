use std::sync::atomic::{AtomicI32, Ordering::SeqCst};
use std::io;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::time::Duration;

// 5. A child panics: siblings are cancelled, run resumes the same payload. Also run under
// RUST_BACKTRACE=1 (make test-rust): the unwinder must not crash walking a fiber stack.
// conformance: same semantics as C test t10 (fail-fast)
#[test]
fn child_panic_cancels_siblings_and_is_resumed() {
    let sibling = AtomicI32::new(0);
    let r = catch_unwind(AssertUnwindSafe(|| {
        em::run(1, || {
            em::scope(|s| {
                s.spawn(|| {
                    let r = em::sleep(Duration::from_secs(30));
                    sibling.store(r.as_ref().err().and_then(|e| e.raw_os_error()).unwrap_or(-1), SeqCst);
                    r
                })?;
                s.spawn(|| {
                    em::sleep(Duration::from_millis(10))?;
                    std::panic::panic_any(String::from("payload-42"));
                })?;
                Ok::<(), io::Error>(())
            })
        })
    }));
    let p = r.expect_err("run must resume the panic");
    assert_eq!(*p.downcast::<String>().unwrap(), "payload-42");
    assert_eq!(sibling.load(SeqCst), 125); // ECANCELED
}

/// A panic in the root closure of a worker.
// conformance: same semantics as C test t18 (cross-worker fail-fast)
#[test]
fn root_panic_is_resumed() {
    let r = catch_unwind(|| em::run(1, || -> io::Result<()> { panic!("root") }));
    assert_eq!(*r.unwrap_err().downcast::<&str>().unwrap(), "root");
}
