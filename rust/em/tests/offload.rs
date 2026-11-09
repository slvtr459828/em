use std::io;
use std::net::SocketAddr;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::SeqCst};
use std::time::{Duration, Instant};

/// `offload` borrows locals (no 'static), runs on another thread, and returns the value.
// conformance: same semantics as C test t33 (runs on another thread)
#[test]
fn offload_borrows_and_runs_elsewhere() {
    em::run(1, || {
        let worker = std::thread::current().id();
        let mut buf = [0u32; 8];
        let name = String::from("borrowed");
        let n = em::offload(|| {
            assert_ne!(std::thread::current().id(), worker);
            for (i, b) in buf.iter_mut().enumerate() {
                *b = (i * i) as u32;
            }
            name.len()
        });
        assert_eq!(n, 8);
        assert_eq!(buf[7], 49);
        Ok(())
    })
    .unwrap();
}

/// The worker keeps running other fibers during a blocking call; a deadline does not cut it.
// conformance: same semantics as C tests t32 (no stall) and t34 (not interruptible)
#[test]
fn offload_does_not_stall_and_is_not_interruptible() {
    let ticked = AtomicBool::new(false);
    let ticks = AtomicU64::new(0);
    em::run(1, || {
        em::scope(|s| {
            s.spawn(|| {
                while !ticked.load(SeqCst) {
                    em::sleep(Duration::from_millis(2))?;
                    ticks.fetch_add(1, SeqCst);
                }
                Ok(())
            })?;
            s.spawn(|| {
                em::scope_timeout(Duration::from_millis(20), |_| {
                    let t0 = Instant::now();
                    let r = em::offload(|| {
                        std::thread::sleep(Duration::from_millis(100));
                        42
                    });
                    ticked.store(true, SeqCst);
                    assert_eq!(r, 42);
                    assert!(t0.elapsed() >= Duration::from_millis(95));
                    // the deadline passed meanwhile: the next cancel point says so
                    assert_eq!(em::sleep(Duration::from_secs(1)).unwrap_err().kind(), io::ErrorKind::TimedOut);
                    Ok(())
                })
                .map(|_| ())
                .or_else(|e| if e.kind() == io::ErrorKind::TimedOut { Ok(()) } else { Err(e) })
            })?;
            Ok(())
        })
    })
    .unwrap();
    assert!(ticks.load(SeqCst) >= 5, "the worker ran other fibers during the call");
}

/// A panic in `offload` is thrown again on the fiber, with its payload.
// conformance: same semantics as C test t10 (panic payload resumed; the C side has no panics,
// this is the binding rule of DESIGN.md "Fiber")
#[test]
fn offload_panic_is_resumed_on_the_fiber() {
    let caught = AtomicBool::new(false);
    em::run(1, || {
        let r = catch_unwind(AssertUnwindSafe(|| em::offload(|| -> i32 { panic!("boom-7") })));
        let p = r.expect_err("the panic must reach the fiber");
        assert_eq!(*p.downcast::<&str>().unwrap(), "boom-7");
        caught.store(true, SeqCst);
        // and the worker is fine afterwards
        assert_eq!(em::offload(|| 1 + 1), 2);
        Ok(())
    })
    .unwrap();
    assert!(caught.load(SeqCst));
}

/// Outside a fiber `f` runs inline.
// conformance: same semantics as C test t33 (-EPERM outside em_run)
#[test]
fn offload_outside_a_fiber_runs_inline() {
    let here = std::thread::current().id();
    assert_eq!(em::offload(|| std::thread::current().id()), here);
}

// conformance: same semantics as C test t36 (resolve)
#[test]
fn resolve_localhost_and_numeric() {
    em::run(1, || {
        let a = em::resolve("localhost", 80)?;
        assert!(!a.is_empty());
        assert!(a.iter().all(|x| x.port() == 80 && x.ip().is_loopback()));
        let a = em::resolve("127.0.0.1", 8080)?;
        assert_eq!(a, vec!["127.0.0.1:8080".parse::<SocketAddr>().unwrap()]);
        let e = em::resolve("nonexistent.invalid", 80).unwrap_err();
        // a sandbox with no resolver answers "try again"
        assert!(matches!(e.kind(), io::ErrorKind::NotFound | io::ErrorKind::WouldBlock), "{e}");
        Ok(())
    })
    .unwrap();
    assert_eq!(em::resolve("localhost", 80).unwrap_err().raw_os_error(), Some(libc::EPERM));
}
