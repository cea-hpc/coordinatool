// SPDX-License-Identifier: LGPL-3.0-or-later

// From the bindgen tutorial (unlicense)
// https://rust-lang.github.io/rust-bindgen/tutorial-0.html

use std::env;
use std::path::PathBuf;

fn main() {
    // -llustreapi
    println!("cargo:rustc-link-lib=lustreapi");

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // hsm_copy debug trait doesn't work (depends on another struct that
        // cannot have debug), skip it.
        .no_debug("hsm_copy")
        // el9 ships rust 1.79 not supported by default, explicitly opt for support
        .rust_target(bindgen::RustTarget::stable(79, 0).map_err(|_| ()).unwrap())
        // The input header we would like to generate
        // bindings for.
        .header("src/wrapper.h")
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
