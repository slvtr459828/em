//! Hand-written declarations of include/em.h (the subset the crate uses).

use libc::{c_int, c_void, size_t, sockaddr, sockaddr_storage, socklen_t, ssize_t};
use std::mem::{align_of, offset_of, size_of};

/// Mirror of `struct em_scope`. Opaque to Rust: only the C runtime reads or writes the fields,
/// and the value must never move after `em_scope_open`. Layout is checked against the C struct
/// by `layout.c` (compiled in build.rs) and by the const asserts below.
#[repr(C)]
pub struct EmScope {
    parent: *mut EmScope,
    deadline: i64,
    cancelled: c_int,
    first_err: c_int,
    children: *mut c_void,
    nchildren: size_t,
    closer: *mut c_void,
    owner: *mut c_void,
}

const _: () = {
    assert!(size_of::<EmScope>() == 56 && align_of::<EmScope>() == 8);
    assert!(offset_of!(EmScope, deadline) == 8 && offset_of!(EmScope, cancelled) == 16);
    assert!(offset_of!(EmScope, first_err) == 20 && offset_of!(EmScope, children) == 24);
    assert!(offset_of!(EmScope, nchildren) == 32 && offset_of!(EmScope, closer) == 40);
    assert!(offset_of!(EmScope, owner) == 48);
};

impl EmScope {
    pub const fn zeroed() -> Self {
        EmScope {
            parent: std::ptr::null_mut(),
            deadline: 0,
            cancelled: 0,
            first_err: 0,
            children: std::ptr::null_mut(),
            nchildren: 0,
            closer: std::ptr::null_mut(),
            owner: std::ptr::null_mut(),
        }
    }
}

pub type FiberFn = extern "C" fn(*mut c_void) -> c_int;

extern "C" {
    pub fn em_run(nworkers: c_int, main_fn: FiberFn, arg: *mut c_void) -> c_int;
    pub fn em_backend() -> *const libc::c_char;
    pub fn em_worker_id() -> c_int;
    pub fn em_worker_count() -> c_int;
    pub fn em_cancel_all();

    pub fn em_spawn(s: *mut EmScope, f: FiberFn, arg: *mut c_void) -> c_int;
    pub fn em_yield();
    pub fn em_self() -> *mut c_void;

    pub fn em_scope_open(s: *mut EmScope, deadline_ns: i64);
    pub fn em_scope_cancel(s: *mut EmScope);
    pub fn em_scope_close(s: *mut EmScope) -> c_int;

    pub fn em_now() -> i64;
    pub fn em_sleep_until(deadline_ns: i64) -> c_int;

    pub fn em_tcp_listen(a: *const sockaddr, n: socklen_t, backlog: c_int) -> c_int;
    pub fn em_accept(lfd: c_int) -> c_int;
    pub fn em_connect(a: *const sockaddr, n: socklen_t) -> c_int;
    pub fn em_recv(fd: c_int, buf: *mut c_void, len: size_t) -> ssize_t;
    pub fn em_read(fd: c_int, buf: *mut c_void, len: size_t) -> ssize_t;
    pub fn em_send(fd: c_int, buf: *const c_void, len: size_t) -> ssize_t;
    pub fn em_send_all(fd: c_int, buf: *const c_void, len: size_t) -> c_int;
    pub fn em_shutdown(fd: c_int, how: c_int) -> c_int;
    pub fn em_udp_bind(a: *const sockaddr, n: socklen_t) -> c_int;
    pub fn em_recvfrom(fd: c_int, buf: *mut c_void, len: size_t, src: *mut sockaddr, srclen: *mut socklen_t) -> ssize_t;
    pub fn em_sendto(fd: c_int, buf: *const c_void, len: size_t, dst: *const sockaddr, n: socklen_t) -> ssize_t;
    pub fn em_close(fd: c_int) -> c_int;

    pub fn em_offload(f: extern "C" fn(*mut c_void) -> isize, arg: *mut c_void) -> isize;
    pub fn em_resolve(host: *const libc::c_char, port: u16, family: c_int, out: *mut sockaddr_storage, max: c_int) -> c_int;
}
