// SPDX-License-Identifier: LGPL-3.0-or-later

use anyhow::{anyhow, Context, Result};
use std::ffi::{CStr, CString};

use crate::bindings::*;
use crate::search_fsname;

pub struct HsmCopytool {
    ct_priv: *mut hsm_copytool_private,
    archives: Vec<u32>,
    fsname: String,
}

pub enum HsmCopytoolAction {
    None = hsm_copytool_action_HSMA_NONE as isize,
    Archive = hsm_copytool_action_HSMA_ARCHIVE as isize,
    Restore = hsm_copytool_action_HSMA_RESTORE as isize,
    Remove = hsm_copytool_action_HSMA_REMOVE as isize,
    Cancel = hsm_copytool_action_HSMA_CANCEL as isize,
}

impl TryFrom<u32> for HsmCopytoolAction {
    type Error = anyhow::Error;

    fn try_from(v: u32) -> Result<Self, Self::Error> {
        match v {
            x if x == HsmCopytoolAction::Archive as u32 => Ok(HsmCopytoolAction::Archive),
            x if x == HsmCopytoolAction::Restore as u32 => Ok(HsmCopytoolAction::Restore),
            x if x == HsmCopytoolAction::Remove as u32 => Ok(HsmCopytoolAction::Remove),
            x if x == HsmCopytoolAction::Cancel as u32 => Ok(HsmCopytoolAction::Cancel),
            _ => Err(anyhow!("Invalid action type {}", v)),
        }
    }
}

pub struct OwnedHsmActionItem {
    pub action: HsmCopytoolAction,
    pub fid: lu_fid,
    pub dfid: lu_fid,
    pub extent: hsm_extent,
    pub cookie: u64,
    pub gid: u64,
    pub data: Vec<u8>,
    pub flags: u64,
    pub archive_id: u32,
}

impl HsmCopytool {
    /// Register new copytool
    /// - mntpath points to the filesystem
    ///
    pub fn new(mntpath: &str, mut archives: Vec<u32>) -> Result<Self> {
        let fsname = search_fsname(mntpath)?;

        let mut ct_priv = std::ptr::null::<hsm_copytool_private> as *mut hsm_copytool_private;
        let c_mntpath = CString::new(mntpath)?;
        /* normalize the array in presence of wildcard:
         * make it contain only 0 to allow for sanity check in recv */
        if archives.contains(&0) {
            archives.clear()
        }
        if archives.is_empty() {
            archives.push(0)
        }
        let res = unsafe {
            llapi_hsm_copytool_register(
                &mut ct_priv,
                c_mntpath.as_ptr(),
                archives.len() as i32,
                archives.as_mut_ptr() as *mut i32,
                0,
            )
        };
        if res < 0 {
            Err(std::io::Error::from_raw_os_error(-res))?;
        }
        Ok(HsmCopytool {
            ct_priv,
            fsname,
            archives,
        })
    }

    pub fn recv(&mut self) -> Result<Vec<OwnedHsmActionItem>> {
        let mut hal_ptr = std::ptr::null::<hsm_action_list> as *mut hsm_action_list;
        let mut msgsize = 0;
        let res = unsafe { llapi_hsm_copytool_recv(self.ct_priv, &mut hal_ptr, &mut msgsize) };
        if res < 0 {
            Err(std::io::Error::from_raw_os_error(-res))?
        }
        // SAFETY: allocated by llapi_hsm_copytool_recv
        // we do not need to cleanup when function returns, next recv() or
        // unregister() will free this buffer.
        let hal = unsafe { hal_ptr.as_ref().unwrap() };
        if hal.hal_version != HAL_VERSION {
            // intermediate variable is required to prevent unaligned access
            // from reference defined in format macro
            let v = hal.hal_version;
            return Err(anyhow!("Unexpected hal version {}", v));
        }
        let archive_id = hal.hal_archive_id;
        if self.archives[0] != 0 && !self.archives.contains(&archive_id) {
            return Err(anyhow!("Unsolicited hal archive_id {}", archive_id));
        }
        // SAFETY: allocated by recv(), nul terminated
        let fsname_c = unsafe { CStr::from_ptr(hal.hal_fsname.as_ptr()) };
        let fsname = fsname_c.to_str().context("Invalid utf8 in fsname")?;
        if fsname != &self.fsname {
            return Err(anyhow!(
                "Action list for wrong fs? received {} but had registered {}",
                fsname,
                self.fsname
            ));
        }

        let mut count = hal.hal_count.try_into()?;
        let mut msgsize: usize = msgsize.try_into()?;
        let mut actions = Vec::<OwnedHsmActionItem>::with_capacity(count);

        // hai_first is static inline, which bindgen does not allow using easily
        // (perhaps https://github.com/rust-lang/rust-bindgen/discussions/2405 ?)
        // for now just open-code it: we need to add fsname length (including nul byte!)
        // and pad to 8...
        let offset = fsname_c.to_bytes_with_nul().len();
        let offset = offset + 7 & !7; // align to next multiple of 8...
        let offset = offset + std::mem::size_of::<hsm_action_list>(); // add hal size
        if msgsize < offset {
            Err(anyhow!("Hsm action list too short, had {}", msgsize))?;
        }
        msgsize -= offset;
        // SAFETY: just checked size
        let mut hai_ptr = unsafe { hal_ptr.byte_add(offset) as *const hsm_action_item };
        while count > 0 {
            if msgsize < std::mem::size_of::<hsm_action_item>() {
                Err(anyhow!(
                    "Hsm action list too short on {}th item ({} left), msgsize {}, need at least {}",
                    actions.len(),
                    count,
                    msgsize,
                    std::mem::size_of::<hsm_action_item>()
                ))?;
            }
            // SAFETY: we just checked it fits.
            let hai = unsafe { &*hai_ptr };
            let hai_len: usize = hai.hai_len.try_into()?;
            let offset: usize = hai_len + 7 & !7;
            if hai_len < std::mem::size_of::<hsm_action_item>() {
                Err(anyhow!(
                    "Hsm action list {}th item had hai_len {}, shorter than hai ({}) ?",
                    actions.len(),
                    hai_len,
                    std::mem::size_of::<hsm_action_item>()
                ))?;
            }
            if msgsize < offset {
                Err(anyhow!(
                    "Hsm action list too short on {}th item ({} left), msgsize {} / offset {}",
                    actions.len(),
                    count,
                    msgsize,
                    offset
                ))?;
            }

            // SAFETY: just checked lengths
            let data: &[u8] = unsafe {
                &*(hai
                    .hai_data
                    .as_slice(hai_len - std::mem::size_of::<hsm_action_item>())
                    as *const _ as *const [u8])
            };
            // XXX make data into a String? (fallback to lossy + warning?)

            actions.push(OwnedHsmActionItem {
                action: hai.hai_action.try_into()?,
                archive_id,
                cookie: hai.hai_cookie,
                fid: hai.hai_fid,
                dfid: hai.hai_dfid,
                data: data.to_owned(),
                extent: hai.hai_extent,
                flags: hal.hal_flags,
                gid: hai.hai_gid,
            });

            // onto next...
            count -= 1;
            msgsize -= offset;
            // SAFETY: just checked it fits
            hai_ptr = unsafe { hai_ptr.byte_add(offset) };
        }

        Ok(actions)
    }

    pub fn get_fd(&mut self) -> Result<i32> {
        let res = unsafe { llapi_hsm_copytool_get_fd(self.ct_priv) };
        if res < 0 {
            Err(std::io::Error::from_raw_os_error(-res))?
        }
        Ok(res)
    }
}

impl Drop for HsmCopytool {
    fn drop(&mut self) {
        unsafe {
            llapi_hsm_copytool_unregister(&mut self.ct_priv);
        }
    }
}
