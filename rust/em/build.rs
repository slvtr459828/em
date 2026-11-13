// Compiles the C runtime (src/*.c + the context-switch asm for the target) into the crate.
// The C sources stay where they are; nothing is copied or vendored.
use std::{env, path::PathBuf};

fn main() {
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let src = root.join("src");
    let asm = match env::var("CARGO_CFG_TARGET_ARCH").unwrap().as_str() {
        "x86_64" => "ctx_x86_64.S",
        "aarch64" => "ctx_aarch64.S",
        a => panic!("em supports x86_64 and aarch64, not {a}"),
    };
    let mut b = cc::Build::new();
    for f in ["fiber.c", "scope.c", "io.c", "io_uring.c", "io_epoll.c", "timer.c", "pool.c", asm] {
        b.file(src.join(f));
    }
    // layout.c is a compile-time check only (_Static_assert); it emits no code.
    b.file("layout.c")
        .include(root.join("include"))
        .std("gnu11")
        .opt_level(2) // the C side's asserts and fast paths are meant for -O2 even in debug test builds
        .flag("-Wall")
        .flag("-Wextra")
        .compile("em_c");
    println!("cargo:rustc-link-lib=uring");
    println!("cargo:rerun-if-changed=layout.c");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());
}
