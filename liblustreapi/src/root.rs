// SPDX-License-Identifier: LGPL-3.0-or-later

use anyhow::{Context, Result};
use std::ffi::CString;

use crate::bindings::*;

pub fn search_fsname(mntpath: &str) -> Result<String> {
    // fsname is guaranteed to be at most LUSTRE_MAXFSNAME + 1 long...
    let mut fsname = vec![0; (LUSTRE_MAXFSNAME + 1) as usize];
    let mntpath = CString::new(mntpath)?;

    let res = unsafe {
        llapi_search_fsname(
            mntpath.as_ptr(),
            fsname.as_mut_ptr() as *mut std::os::raw::c_char,
        )
    };
    if res < 0 {
        Err(std::io::Error::from_raw_os_error(-res))?;
    }

    // conversion back to String, uff...
    // https://users.rust-lang.org/t/allocating-a-c-string-in-rust-and-passing-to-c/50902
    let len = fsname
        .iter()
        .position(|&c| c == 0)
        .expect("buffer overflow!");
    fsname.truncate(len);
    Ok(String::from_utf8(fsname).context("Invalid UTF-8 in fsname")?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn search() {
        // XXX this test expects a fs to be mounted with fixed name in fixed location,
        // use env vars?
        let name = search_fsname("/mnt/lustre0").unwrap();
        assert_eq!(&name, "testfs0");
    }
}
