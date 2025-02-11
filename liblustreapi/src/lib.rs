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

    #[test]
    fn log() {
        unsafe {
            llapi_msg_set_level(llapi_message_level_LLAPI_MSG_INFO.try_into().unwrap());
            let msg = CString::new("test").unwrap();
            llapi_error(llapi_message_level_LLAPI_MSG_INFO, -22, msg.as_ptr())
        }
    }
}
