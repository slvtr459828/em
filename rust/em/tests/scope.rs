mod common;

use std::cell::{Cell, RefCell};
use std::io::{self, Read};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::SeqCst};
use std::time::Duration;

const ECANCELED: i32 = 125;

struct Guard<'a>(&'a AtomicBool);
impl Drop for Guard<'_> {
    fn drop(&mut self) {
        self.0.store(true, SeqCst);
    }
}

/// 2. Children borrow locals of the caller.
// conformance: same semantics as C test t3 (many fibers)
#[test]
fn children_borrow_locals() {
    em::run(1, || {
        let out = RefCell::new(Vec::new()); // a local of the fiber: !Sync, no Send needed
        em::scope(|s| {
            for i in 0..50 {
                let out = &out;
                s.spawn(move || {
                    em::yield_now();
                    out.borrow_mut().push(i);
                    Ok(())
                })?;
            }
            Ok(())
        })?;
        let mut v = out.into_inner();
        v.sort();
        assert_eq!(v, (0..50).collect::<Vec<_>>());
        Ok(())
    })
    .unwrap();
}

/// 3. scope_timeout around a read on an idle connection; Drop ran.
// conformance: same semantics as C test t8 (deadline)
#[test]
fn timeout_read() {
    let addr = common::local_addr();
    let dropped = AtomicBool::new(false);
    em::run(1, || {
        let l = em::TcpListener::bind(addr)?;
        let r = em::scope(|s| {
            s.spawn(|| {
                let _c = em::TcpStream::connect(addr)?;
                em::sleep(Duration::from_secs(30)) // idle peer; cancelled when the scope ends
            })?;
            let mut c = l.accept()?;
            let e = em::scope_timeout(Duration::from_millis(10), |_| {
                let _g = Guard(&dropped);
                c.read(&mut [0u8; 8])
            })
            .unwrap_err();
            assert_eq!(e.kind(), io::ErrorKind::TimedOut);
            assert!(dropped.load(SeqCst), "Drop of the guard did not run");
            em::cancel_all();
            Ok(())
        });
        // cancel_all ends the root scope with ECANCELED; anything else is a failure
        assert_eq!(r.unwrap_err().raw_os_error(), Some(ECANCELED));
        Ok(())
    })
    .unwrap_err();
}

/// 4. A fails with io::Error::other("x"); B blocked in read is cancelled; scope returns "x".
// conformance: same semantics as C test t10, t11 (fail-fast, nested)
#[test]
fn error_propagates_and_cancels() {
    let addr = common::local_addr();
    let b_saw = AtomicI32::new(0);
    em::run(1, || {
        let l = em::TcpListener::bind(addr)?;
        let r = em::scope(|s| {
            s.spawn(|| {
                let mut c = l.accept()?;
                let r = c.read(&mut [0u8; 8]);
                b_saw.store(r.as_ref().err().and_then(|e| e.raw_os_error()).unwrap_or(-1), SeqCst);
                r.map(drop)
            })?;
            s.spawn(|| {
                let _c = em::TcpStream::connect(addr)?;
                em::sleep(Duration::from_secs(30))
            })?;
            s.spawn(|| {
                em::sleep(Duration::from_millis(20))?;
                Err(io::Error::other("x"))
            })?;
            Ok(())
        });
        let e = r.unwrap_err();
        assert_eq!(e.kind(), io::ErrorKind::Other);
        assert_eq!(e.to_string(), "x");
        Ok(())
    })
    .unwrap();
    assert_eq!(b_saw.load(SeqCst), ECANCELED);
}

/// A panic in the scope body still waits for the children before unwinding.
// conformance: same semantics as C test t15 (unwind awaits children)
#[test]
fn body_panic_awaits_children() {
    em::run(1, || {
        let finished = Cell::new(false);
        let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _ = em::scope(|s| {
                s.spawn(|| {
                    let r = em::sleep(Duration::from_secs(30));
                    finished.set(true); // runs only if the child is awaited
                    r
                })?;
                panic!("body");
                #[allow(unreachable_code)]
                Ok(())
            });
        }));
        assert_eq!(*r.unwrap_err().downcast::<&str>().unwrap(), "body");
        assert!(finished.get(), "scope returned before its child ended");
        Ok(())
    })
    .unwrap();
}

/// 6. Outside a fiber: EPERM; nested run: EBUSY; a socket kept past run is harmless.
// conformance: same semantics as C test t30 (outside a fiber)
#[test]
fn outside_fiber() {
    const EPERM: i32 = 1;
    const EBUSY: i32 = 16;
    assert_eq!(em::sleep(Duration::from_millis(1)).unwrap_err().raw_os_error(), Some(EPERM));
    assert_eq!(em::scope(|_| Ok(())).unwrap_err().raw_os_error(), Some(EPERM));
    em::yield_now();

    thread_local!(static KEPT: RefCell<Option<em::TcpListener>> = const { RefCell::new(None) });
    let addr = common::local_addr();
    em::run(1, || {
        let l = em::TcpListener::bind(addr)?;
        KEPT.with(|k| *k.borrow_mut() = Some(l));
        assert_eq!(em::run(1, || Ok(())).unwrap_err().raw_os_error(), Some(EBUSY));
        Ok(())
    })
    .unwrap();
    KEPT.with(|k| {
        let l = k.borrow_mut().take().unwrap();
        assert_eq!(l.accept().err().unwrap().raw_os_error(), Some(EPERM));
        drop(l); // plain close()
    });
}

/// run reports the first Err as the original io::Error.
// conformance: same semantics as C test t17, t27 (run result)
#[test]
fn run_returns_error() {
    let e = em::run(1, || Err(io::Error::other("boom"))).unwrap_err();
    assert_eq!(e.to_string(), "boom");
}
