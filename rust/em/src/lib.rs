//! Rust binding of the em C runtime: stackful fibers, one worker thread per core, io_uring (or
//! epoll) underneath, and code that looks blocking.
//!
//! ```no_run
//! use std::io::{Read, Write};
//!
//! fn main() -> std::io::Result<()> {
//!     em::run(0, || {                                   // runs on EVERY worker, like em_run
//!         let l = em::TcpListener::bind("0.0.0.0:8080".parse().unwrap())?;
//!         em::scope(|s| loop {
//!             let mut c = l.accept()?;
//!             s.spawn(move || {
//!                 let mut buf = [0u8; 4096];
//!                 loop {
//!                     let n = c.read(&mut buf)?;        // std::io::Read: the fiber parks, the worker runs others
//!                     if n == 0 { return Ok(()); }
//!                     c.write_all(&buf[..n])?;
//!                 }
//!             })?;
//!         })
//!     })
//! }
//! ```
//!
//! # Model
//!
//! * [`run`] starts one worker per core and runs the same closure as the first fiber of each
//!   (`Fn + Sync`). Fibers never move between workers, so sockets are `!Send + !Sync` and
//!   spawned closures need no `Send`.
//! * Every fiber belongs to a [`scope`], like `std::thread::scope`: it may borrow from the
//!   caller and is always awaited. The first child to fail (error or panic) cancels its
//!   siblings; blocked calls then return `ECANCELED`, or [`ErrorKind::TimedOut`] when a
//!   [`scope_timeout`] deadline passed. Cancellation never surfaces as `EINTR`, so `read_exact`
//!   and `write_all` do not retry it.
//! * `scope` returns the first error as the original [`io::Error`]. A panic is kept and resumed
//!   once the scope is closed. A panic in the scope body still cancels and awaits every child
//!   before unwinding, otherwise they would point into a dead stack.
//! * Calling em outside a fiber (after `run` returned, from another thread) returns `EPERM`;
//!   dropping a socket there just closes the fd.
//!
//! # Rules that the types cannot enforce
//!
//! * **Blocking std calls stall the whole worker** (`std::fs`, `std::net`, `thread::sleep`,
//!   `Mutex` held across a wait, DNS): em does not hook syscalls (D6). Use [`sleep`] and the
//!   sockets of this crate.
//! * **`thread_local!` is shared by all fibers of a worker**: it is per thread, not per fiber.
//! * **A fiber stack is 256 KiB and fixed** (D7). Deep recursion or big stack arrays end in
//!   `SIGSEGV` at the guard page, not in a Rust panic. Put big buffers in the heap.
//! * Each `spawn` allocates a `Box` for the closure.
//!
//! The backend is chosen once per [`run`] by the `EM_BACKEND` environment variable
//! (`auto`, `uring`, `epoll`); see [`backend`].
//!
//! Sockets are `!Send`:
//!
//! ```compile_fail,E0277
//! fn f(s: em::TcpStream) {
//!     std::thread::spawn(move || drop(s));
//! }
//! ```

use std::any::Any;
use std::cell::{RefCell, UnsafeCell};
use std::io;
use std::marker::PhantomData;
use std::panic::{catch_unwind, resume_unwind, AssertUnwindSafe};
use std::sync::Mutex;
use std::time::Duration;

use libc::{c_int, c_void};

mod ffi;
mod net;

pub use net::{read_fd, TcpListener, TcpStream, UdpSocket};

type Payload = Box<dyn Any + Send + 'static>;

/// `-errno` from the C API as an `io::Error` (ETIMEDOUT becomes `ErrorKind::TimedOut`).
fn errno(r: c_int) -> io::Error {
    io::Error::from_raw_os_error(-r)
}

fn cvt(r: isize) -> io::Result<usize> {
    if r < 0 { Err(errno(r as c_int)) } else { Ok(r as usize) }
}

/// A nonzero C result for a failed fiber (tells C to cancel its siblings); the `io::Error` itself
/// is kept on the Rust side.
fn fail_code(e: &io::Error) -> c_int {
    -e.raw_os_error().filter(|&c| c > 0).unwrap_or(libc::EIO)
}

fn deadline_after(d: Duration) -> i64 {
    let ns = i64::try_from(d.as_nanos()).unwrap_or(i64::MAX);
    unsafe { ffi::em_now() }.saturating_add(ns)
}

// ---- run -----------------------------------------------------------------------------------

struct Root<'a, F> {
    f: &'a F,
    err: Mutex<Option<io::Error>>,
    panic: Mutex<Option<Payload>>,
}

extern "C" fn root_main<F: Fn() -> io::Result<()> + Sync>(arg: *mut c_void) -> c_int {
    // SAFETY: arg is the `Root` that `run` keeps alive until em_run returns.
    let root = unsafe { &*(arg as *const Root<'_, F>) };
    match catch_unwind(AssertUnwindSafe(|| (root.f)())) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            let c = fail_code(&e);
            root.err.lock().unwrap().get_or_insert(e);
            c
        }
        Err(p) => {
            root.panic.lock().unwrap().get_or_insert(p);
            -libc::EIO
        }
    }
}

/// Runs `f` as the first fiber of every worker (`n` workers, 0 = one per allowed CPU); the
/// calling thread is worker 0. Returns when all of them are done.
///
/// A worker's error cancels the others; the first error to be recorded is returned as the
/// original `io::Error`. If a fiber panicked, the payload is resumed here after the runtime
/// has shut down. Returns `EBUSY` when called from inside a worker, and the kernel's error
/// when the backend cannot start (e.g. `EPERM` when io_uring is blocked and
/// `EM_BACKEND=uring`). A deadlocked worker reports `EDEADLK`.
pub fn run<F>(n: usize, f: F) -> io::Result<()>
where
    F: Fn() -> io::Result<()> + Sync,
{
    let n = c_int::try_from(n).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
    let root = Root { f: &f, err: Mutex::new(None), panic: Mutex::new(None) };
    // SAFETY: `root` outlives em_run; root_main only shares it by reference and Root is Sync.
    let r = unsafe { ffi::em_run(n, root_main::<F>, &root as *const _ as *mut c_void) };
    if let Some(p) = root.panic.into_inner().unwrap() {
        resume_unwind(p);
    }
    if r == 0 {
        return Ok(());
    }
    match root.err.into_inner().unwrap() {
        // a deadlock is reported by the runtime even though the fibers unwound with errors
        Some(e) if r != -libc::EDEADLK => Err(e),
        _ => Err(errno(r)),
    }
}

// ---- scope ---------------------------------------------------------------------------------

#[derive(Default)]
struct State {
    err: RefCell<Option<io::Error>>,
    panic: RefCell<Option<Payload>>,
}

impl State {
    fn fail(&self, e: io::Error) {
        self.err.borrow_mut().get_or_insert(e);
    }
    fn panicked(&self, p: Payload) {
        self.panic.borrow_mut().get_or_insert(p);
    }
}

/// A set of fibers that are awaited when the enclosing [`scope`] ends. See [`Scope::spawn`].
pub struct Scope<'scope, 'env: 'scope> {
    // Lives in the frame of scope_at and is never moved after em_scope_open; only `&Scope` is
    // handed out. UnsafeCell: the C runtime mutates it through the pointer.
    raw: UnsafeCell<ffi::EmScope>,
    state: State,
    scope: PhantomData<&'scope mut &'scope ()>,
    env: PhantomData<&'env mut &'env ()>,
}

struct Task<F> {
    f: F,
    state: *const State,
}

extern "C" fn child_main<F: FnOnce() -> io::Result<()>>(arg: *mut c_void) -> c_int {
    // SAFETY: arg came from Box::into_raw in `spawn`; the scope (and its State) is alive
    // because scope_at waits for every child before returning.
    let Task { f, state } = *unsafe { Box::from_raw(arg as *mut Task<F>) };
    let state = unsafe { &*state };
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            let c = fail_code(&e);
            state.fail(e);
            c
        }
        Err(p) => {
            state.panicked(p);
            -libc::EIO
        }
    }
}

impl<'scope, 'env> Scope<'scope, 'env> {
    /// Starts a fiber on the current worker. `f` may borrow anything that outlives the scope
    /// and does not need `Send`: the fiber never leaves this worker. An `Err` or a panic from
    /// `f` cancels the whole scope; [`scope`] then returns that error (or resumes the panic).
    /// `Err` here means the fiber could not be created (`ENOMEM`: stack mapping limit).
    pub fn spawn<F>(&'scope self, f: F) -> io::Result<()>
    where
        F: FnOnce() -> io::Result<()> + 'scope,
    {
        let task = Box::into_raw(Box::new(Task { f, state: &self.state }));
        // SAFETY: self.raw is an open scope that is not moved; ownership of `task` passes to
        // child_main, which always runs once em_spawn succeeds.
        let r = unsafe { ffi::em_spawn(self.raw.get(), child_main::<F>, task as *mut c_void) };
        if r < 0 {
            drop(unsafe { Box::from_raw(task) });
            return Err(errno(r));
        }
        Ok(())
    }
}

fn scope_at<'env, F, T>(deadline: i64, f: F) -> io::Result<T>
where
    F: for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> io::Result<T>,
{
    // SAFETY: em_self only reads the worker's current fiber.
    if unsafe { ffi::em_self() }.is_null() {
        return Err(io::Error::from_raw_os_error(libc::EPERM));
    }
    let sc = Scope { raw: UnsafeCell::new(ffi::EmScope::zeroed()), state: State::default(), scope: PhantomData, env: PhantomData };
    // SAFETY: from here `sc` is only borrowed, so the scope never moves.
    unsafe { ffi::em_scope_open(sc.raw.get(), deadline) };
    // Whatever the body does, every child must be awaited before this frame can unwind.
    let out = match catch_unwind(AssertUnwindSafe(|| f(&sc))) {
        Ok(Ok(t)) => Some(t),
        Ok(Err(e)) => {
            sc.state.fail(e);
            None
        }
        Err(p) => {
            sc.state.panicked(p);
            None
        }
    };
    if out.is_none() {
        unsafe { ffi::em_scope_cancel(sc.raw.get()) };
    }
    let code = unsafe { ffi::em_scope_close(sc.raw.get()) };
    if let Some(p) = sc.state.panic.take() {
        resume_unwind(p);
    }
    if let Some(e) = sc.state.err.take() {
        return Err(e);
    }
    match out {
        Some(t) if code == 0 => Ok(t),
        // cancelled from outside (parent scope, cancel_all) or timed out, with no error of its own
        _ => Err(errno(if code == 0 { -libc::ECANCELED } else { code })),
    }
}

/// Runs `f`, then waits for every fiber spawned on the [`Scope`] it receives. Must be called
/// from a fiber (inside [`run`]), else `EPERM`.
///
/// Returns the first error of the body or of a child (the original `io::Error`); a panic is
/// resumed here after all children ended.
pub fn scope<'env, F, T>(f: F) -> io::Result<T>
where
    F: for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> io::Result<T>,
{
    scope_at(0, f)
}

/// Like [`scope`] with a deadline `dur` from now (never later than the enclosing scope's).
/// Blocked calls inside it return [`ErrorKind::TimedOut`].
///
/// [`ErrorKind::TimedOut`]: std::io::ErrorKind::TimedOut
pub fn scope_timeout<'env, F, T>(dur: Duration, f: F) -> io::Result<T>
where
    F: for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> io::Result<T>,
{
    scope_at(deadline_after(dur), f)
}

// ---- small calls ---------------------------------------------------------------------------

/// Parks the fiber for `dur`. `Err(ECANCELED)` / `Err(TimedOut)` if the scope was cancelled or
/// its deadline came first; `EPERM` outside a fiber.
pub fn sleep(dur: Duration) -> io::Result<()> {
    let r = unsafe { ffi::em_sleep_until(deadline_after(dur)) };
    if r < 0 { Err(errno(r)) } else { Ok(()) }
}

/// Lets the other fibers of this worker run. No-op outside a fiber.
pub fn yield_now() {
    unsafe { ffi::em_yield() }
}

/// Cancels the root scope of every worker (graceful shutdown). No-op outside [`run`].
pub fn cancel_all() {
    unsafe { ffi::em_cancel_all() }
}

/// Index of the current worker. Meaningful inside [`run`].
pub fn worker_id() -> usize {
    unsafe { ffi::em_worker_id() as usize }
}

/// Number of workers of the current run (0 outside [`run`]).
pub fn worker_count() -> usize {
    unsafe { ffi::em_worker_count() as usize }
}

/// `"uring"` or `"epoll"`: the backend of the current run. Meaningful inside [`run`].
pub fn backend() -> &'static str {
    // SAFETY: em_backend returns one of two string literals.
    unsafe { std::ffi::CStr::from_ptr(ffi::em_backend()) }.to_str().unwrap_or("?")
}
