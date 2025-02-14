// SPDX-License-Identifier: LGPL-3.0-or-later

// XXX only needed for older rust (128 bit FFI)
#![allow(improper_ctypes)]
// non-standard names generated
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
// it's ok not to use everything...
#![allow(dead_code)]
// also clippy!
#![allow(clippy::all)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod hsm;
pub use hsm::*;

mod root;
pub use root::*;

#[cfg(test)]
mod tests {
    use crate::bindings::*;
    use std::ffi::CString;

    // until that finds a better place...
    #[test]
    fn log() {
        unsafe {
            llapi_msg_set_level(llapi_message_level_LLAPI_MSG_INFO.try_into().unwrap());
            let msg = CString::new("test").unwrap();
            llapi_error(llapi_message_level_LLAPI_MSG_INFO, -22, msg.as_ptr())
        }
    }

    /// Since rust does not have a way to skip test, we'll skip tests that require
    /// env vars by making the tests succeed.
    /// This test leaves an actionable trace for user to notice that without having all tests
    /// fail in a noisy way
    #[test]
    fn check_env() {
        assert!(std::env::var("LUSTRE_MNTPATH").is_ok());
        assert!(std::env::var("LUSTRE_FSNAME").is_ok());
    }
}
