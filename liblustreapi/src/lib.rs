// SPDX-License-Identifier: LGPL-3.0-or-later

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
use bindings::*;

pub struct LlapiCopytool {
    ct_priv: *mut hsm_copytool_private,
}

impl LlapiCopytool {
    pub fn new(mntpath: &str, archives: &mut [i32]) -> Result<Self, std::io::Error> {
        let mut ct_priv = std::ptr::null::<hsm_copytool_private> as *mut hsm_copytool_private;
        let res = unsafe {
            llapi_hsm_copytool_register(
                &mut ct_priv,
                std::ffi::CString::new(mntpath).unwrap().as_ptr(),
                archives.len().try_into().unwrap(),
                archives.as_mut_ptr(),
                0,
            )
        };
        if res < 0 {
            return Err(std::io::Error::from_raw_os_error(-res));
        }
        Ok(LlapiCopytool { ct_priv })
    }
}

impl Drop for LlapiCopytool {
    fn drop(&mut self) {
        unsafe {
            llapi_hsm_copytool_unregister(&mut self.ct_priv);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn log() {
        unsafe {
            llapi_msg_set_level(llapi_message_level_LLAPI_MSG_INFO.try_into().unwrap());
            llapi_error(
                llapi_message_level_LLAPI_MSG_INFO,
                -22,
                std::ffi::CString::new("test").unwrap().as_ptr(),
            )
        }
    }
}
