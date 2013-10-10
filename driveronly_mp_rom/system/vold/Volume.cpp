/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <linux/kdev_t.h>
#include <linux/fs.h>

#include <cutils/properties.h>

#include <diskconfig/diskconfig.h>

#include <private/android_filesystem_config.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/fs.h>

#include "Volume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "Fat.h"
#include "Process.h"
#include "cryptfs.h"

extern "C" void dos_partition_dec(void const *pp, struct dos_partition *d);
extern "C" void dos_partition_enc(void *pp, struct dos_partition *d);


/*
 * Secure directory - stuff that only root can see
 */
const char *Volume::SECDIR            = "/mnt/secure";

/*
 * Secure staging directory - where media is mounted for preparation
 */
const char *Volume::SEC_STGDIR        = "/mnt/secure/staging";

/*
 * Path to the directory on the media which contains publicly accessable
 * asec imagefiles. This path will be obscured before the mount is
 * exposed to non priviledged users.
 */
const char *Volume::SEC_STG_SECIMGDIR = "/mnt/secure/staging/.android_secure";

/*
 * Path to external storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_EXT   = "/mnt/secure/asec";

/*
 * Path to internal storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_INT   = "/data/app-asec";
/*
 * Path to where secure containers are mounted
 */
const char *Volume::ASECDIR           = "/mnt/asec";

/*
 * Path to where OBBs are mounted
 */
const char *Volume::LOOPDIR           = "/mnt/obb";

static const char *stateToStr(int state) {
    if (state == Volume::State_Init)
        return "Initializing";
    else if (state == Volume::State_NoMedia)
        return "No-Media";
    else if (state == Volume::State_Idle)
        return "Idle-Unmounted";
    else if (state == Volume::State_Pending)
        return "Pending";
    else if (state == Volume::State_Mounted)
        return "Mounted";
    else if (state == Volume::State_Unmounting)
        return "Unmounting";
    else if (state == Volume::State_Checking)
        return "Checking";
    else if (state == Volume::State_Formatting)
        return "Formatting";
    else if (state == Volume::State_Shared)
        return "Shared-Unmounted";
    else if (state == Volume::State_SharedMnt)
        return "Shared-Mounted";
    else
        return "Unknown-Error";
}

Volume::Volume(VolumeManager *vm, const char *label, const char *mount_point) {
    mVm = vm;
    mDebug = false;
    mLabel = strdup(label);
#ifdef MTK_2SDCARD_SWAP
    strcpy(mFstabMntPath, mount_point);
    mMountpoint = mFstabMntPath;
#else
    mMountpoint = strdup(mount_point);
#endif   
    mState = mPreState = Volume::State_Init;
    mCurrentlyMountedKdev = -1;
    mPartIdx = -1;
    mRetryMount = false;
    mIsEmmcStorage = false;
}

Volume::~Volume() {
    free(mLabel);
#ifdef MTK_2SDCARD_SWAP	
	mMountpoint = NULL ;
#else
    free(mMountpoint);
#endif
}

void Volume::protectFromAutorunStupidity() {
    char filename[255];

    snprintf(filename, sizeof(filename), "%s/autorun.inf", SEC_STGDIR);
    if (!access(filename, F_OK)) {
        SLOGW("Volume contains an autorun.inf! - removing");
        /*
         * Ensure the filename is all lower-case so
         * the process killer can find the inode.
         * Probably being paranoid here but meh.
         */
        rename(filename, filename);
        Process::killProcessesWithOpenFiles(filename, 2);
        if (unlink(filename)) {
            SLOGE("Failed to remove %s (%s)", filename, strerror(errno));
        }
    }
}

void Volume::setDebug(bool enable) {
    mDebug = enable;
}

dev_t Volume::getDiskDevice() {
    return MKDEV(0, 0);
};

dev_t Volume::getShareDevice() {
    return getDiskDevice();
}

void Volume::handleVolumeShared() {
}

void Volume::handleVolumeUnshared() {
}

int Volume::handleBlockEvent(NetlinkEvent *evt) {
    errno = ENOSYS;
    return -1;
}

void Volume::setState(int state) {
   setState(state, false);
}

void Volume::setState(int state, bool bValue) {
    char msg[255];
    int oldState = mState;

    if (oldState == state) {
        SLOGW("Duplicate state (%d)\n", state);
        return;
    }

    if ((oldState == Volume::State_Pending) && (state != Volume::State_Idle)) {
        mRetryMount = false;
    }

    mPreState = oldState;
    mState = state;

    SLOGD("Volume %s state changing %d (%s) -> %d (%s), bValue(%d)", mLabel,
         oldState, stateToStr(oldState), mState, stateToStr(mState), bValue);
    snprintf(msg, sizeof(msg),
             "Volume %s %s state changed from %d (%s) to %d (%s) %d", getLabel(),
             getMountpoint(), oldState, stateToStr(oldState), mState,
             stateToStr(mState), bValue);   


    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeStateChange,
                                         msg, false);
}

int Volume::createDeviceNode(const char *path, int major, int minor) {
    mode_t mode = 0660 | S_IFBLK;
    dev_t dev = (major << 8) | minor;
    if (mknod(path, mode, dev) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

int Volume::formatVol() {

    dev_t deviceNodes[MAX_SUP_PART];

    if (getState() == Volume::State_NoMedia) {
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        errno = EBUSY;
        return -1;
    }

    SLOGI("mDiskNumParts = %d\n", getDeviceNumParts());
    bool formatEntireDevice = ((mPartIdx == -1) && (0 != getDeviceNumParts()));
    char devicePath[255];
    dev_t diskNode = getDiskDevice();
    //dev_t partNode = MKDEV(MAJOR(diskNode), (formatEntireDevice ? 1 : mPartIdx));
    getDeviceNodes((dev_t *) &deviceNodes, MAX_SUP_PART);
    dev_t partNode  = deviceNodes[0];

    setState(Volume::State_Formatting);

    int ret = -1;
    // Only initialize the MBR if we are formatting the entire device
    if (formatEntireDevice) {
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(diskNode), MINOR(diskNode));
	SLOGI("Call initializeMbr().\n");
        if (initializeMbr(devicePath)) {
            SLOGE("Failed to initialize MBR (%s)", strerror(errno));
            goto err;
        }
	SLOGI("Exit initializeMbr().\n");
    }

    sprintf(devicePath, "/dev/block/vold/%d:%d",
            MAJOR(partNode), MINOR(partNode));

    if (mDebug) {
        SLOGI("Formatting volume %s (%s)", getLabel(), devicePath);
    }

    if (Fat::format(devicePath, 0)) {
        SLOGE("Failed to format (%s)", strerror(errno));
        goto err;
    }

    ret = 0;

err:
    setState(Volume::State_Idle);
    return ret;
}

bool Volume::isMountpointMounted(const char *path) {
    char device[256];
    char mount_path[256];
    char rest[256];
    FILE *fp;
    char line[1024];

    if (!(fp = fopen("/proc/mounts", "r"))) {
        SLOGE("Error opening /proc/mounts (%s)", strerror(errno));
        return false;
    }

    while(fgets(line, sizeof(line), fp)) {
        line[strlen(line)-1] = '\0';
        sscanf(line, "%255s %255s %255s\n", device, mount_path, rest);
        if (!strcmp(mount_path, path)) {
            fclose(fp);
            return true;
        }

    }

    fclose(fp);
    return false;
}

int Volume::mountVol() {
    dev_t deviceNodes[MAX_SUP_PART];
    int n, i, curState, rc = 0;
    char errmsg[255];
    const char* externalStorage = getenv("EXTERNAL_STORAGE");
    bool primaryStorage = externalStorage && !strcmp(getMountpoint(), externalStorage);
    char decrypt_state[PROPERTY_VALUE_MAX];
    char crypto_state[PROPERTY_VALUE_MAX];
    char encrypt_progress[PROPERTY_VALUE_MAX];
    int flags;

    if (mPreState != Volume::State_Shared && mVm->isSomeVolumeShared()) {
        SLOGI("Some volume is State_Shared, force to share current volume, %s \n", getLabel());
        return mVm->shareVolume(getLabel(), "ums");
    }

    property_get("vold.decrypt", decrypt_state, "");
    property_get("vold.encrypt_progress", encrypt_progress, "");

    /* Don't try to mount the volumes if we have not yet entered the disk password
     * or are in the process of encrypting.
     */
     #ifdef VENDOR_EDIT
     //Jiangtao.Guo@Prd.SysSrv.InputManager, 2013/01/25, Add for 
         bool isExternal = false;
		SLOGD("mountVol:  getMountpoint()= %s\n", getMountpoint());
		if (access(getMountpoint(), F_OK))
        {
            SLOGE("Mountpoint doesn't exist, crearte one");
            mkdir(getMountpoint(), 0777);
        }
         #ifdef OPPO_EMMC_DYNAMICMOUNT_SUPPORT
			SLOGD("access = %d\n", access("/sys/block/mmcblk1", F_OK));
            SLOGD("primaryStorage = %d\n",primaryStorage);
             if(access("/sys/block/mmcblk1", F_OK) == 0 && primaryStorage)
             {
                isExternal = true;
             }
         #else
            isExternal = true;
         #endif
     #endif /* VENDOR_EDIT */
    if ((getState() == Volume::State_NoMedia) ||
        ((!strcmp(decrypt_state, "1") || encrypt_progress[0]) && primaryStorage)) {
        snprintf(errmsg, sizeof(errmsg),
                 "Volume %s %s mount failed - no media",
                 getLabel(), getMountpoint());
        mVm->getBroadcaster()->sendBroadcast(
                                         ResponseCode::VolumeMountFailedNoMedia,
                                         errmsg, false);
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        if (getState() == Volume::State_Pending) {
            mRetryMount = true;
        }
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        return 0;
    }

    SLOGW("mountvol %d", IsEmmcStorage());
    #ifdef MTK_SHARED_SDCARD
	  if (IsEmmcStorage()) {
          usleep(1000*150);
		  setState(Volume::State_Mounted);
		  return 0;
	  }    
    #endif

    n = getDeviceNodes((dev_t *) &deviceNodes, MAX_SUP_PART);
    if (!n) {
        SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
        return -1;
    }
    SLOGD("Found %d device nodes", n);

    #ifndef MTK_EMULATOR_SUPPORT
    if (!IsEmmcStorage() && !strncmp(getLabel(), "sdcard", 6)) {
       SLOGD("Reinit SD card");
	   if (mVm->reinitExternalSD()){
		   SLOGE("Fail: reinitExternalSD()");
		   /* Card inserted but fail to reinit, there is something wrong with this card */
		   errno = EIO;
		   return -1;
       }
    }
    #endif

    /* If we're running encrypted, and the volume is marked as encryptable and nonremovable,
     * and vold is asking to mount the primaryStorage device, then we need to decrypt
     * that partition, and update the volume object to point to it's new decrypted
     * block device
     */
    property_get("ro.crypto.state", crypto_state, "");
    flags = getFlags();
    if (primaryStorage &&
        ((flags & (VOL_NONREMOVABLE | VOL_ENCRYPTABLE))==(VOL_NONREMOVABLE | VOL_ENCRYPTABLE)) &&
        !strcmp(crypto_state, "encrypted") && !isDecrypted()) {
       char new_sys_path[MAXPATHLEN];
       char nodepath[256];
       int new_major, new_minor;

       if (n != 1) {
           /* We only expect one device node returned when mounting encryptable volumes */
           SLOGE("Too many device nodes returned when mounting %d\n", getMountpoint());
           return -1;
       }

       if (cryptfs_setup_volume(getLabel(), MAJOR(deviceNodes[0]), MINOR(deviceNodes[0]),
                                new_sys_path, sizeof(new_sys_path),
                                &new_major, &new_minor)) {
           SLOGE("Cannot setup encryption mapping for %d\n", getMountpoint());
           return -1;
       }
       /* We now have the new sysfs path for the decrypted block device, and the
        * majore and minor numbers for it.  So, create the device, update the
        * path to the new sysfs path, and continue.
        */
        snprintf(nodepath,
                 sizeof(nodepath), "/dev/block/vold/%d:%d",
                 new_major, new_minor);
        if (createDeviceNode(nodepath, new_major, new_minor)) {
            SLOGE("Error making device node '%s' (%s)", nodepath,
                                                       strerror(errno));
        }

        // Todo: Either create sys filename from nodepath, or pass in bogus path so
        //       vold ignores state changes on this internal device.
        updateDeviceInfo(nodepath, new_major, new_minor);

        /* Get the device nodes again, because they just changed */
        n = getDeviceNodes((dev_t *) &deviceNodes, MAX_SUP_PART);
        if (!n) {
            SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
            return -1;
        }
    }

    for (i = 0; i < n; i++) {
        char devicePath[255];

        sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(deviceNodes[i]),
                MINOR(deviceNodes[i]));

        if (deviceNodes[i] == (dev_t)(-1)) {
            SLOGE("Partition '%s' is invalid dev_t. Skip mounting!", devicePath);
            continue;
        }

        SLOGI("%s being considered for volume %s\n", devicePath, getLabel());

        errno = 0;
        if ((getState() == Volume::State_NoMedia) ) {
            SLOGI("NoMedia! skip mounting the storage. Update errno to ENODEV");
            errno = ENODEV;
            return -1;
        } 
        setState(Volume::State_Checking);

        /*
         * If FS check failed, we should move on to next partition
         * instead of returning an error
         */

__CHECK_FAT_AGAIN:
        if (Fat::check(devicePath)) {
#if 0
            if (errno == ENODATA) {
                SLOGW("%s does not contain a FAT filesystem\n", devicePath);
                continue;
            }
            errno = EIO;
            /* Badness - abort the mount */
            SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
            setState(Volume::State_Idle);
            return -1;
#else

#ifdef MTK_EMMC_SUPPORT
            if ( mVm->isFirstBoot() && IsEmmcStorage()) {
                SLOGI("** This is first boot and internal sd is not formatted. Try to format it. (%s)\n", devicePath);
			    if (Fat::format(devicePath, 0)) {
				  SLOGE("Failed to format (%s)", strerror(errno));				  
			    }
                else {
                    SLOGI("Format successfully. (%s)\n", devicePath);
					property_set("persist.first_boot", "0");
                    goto __CHECK_FAT_AGAIN;
                }
            }
#endif

            SLOGW("%s failed FS checks, move on to next partition", devicePath);
            continue;
#endif                                        
        }

#ifdef MTK_EMMC_SUPPORT
        else {
             if ( mVm->isFirstBoot() && IsEmmcStorage()) {
                 property_set("persist.first_boot", "0");           
             }
        }  
#endif     

        /*
         * Mount the device on our internal staging mountpoint so we can
         * muck with it before exposing it to non priviledged users.
         */
        errno = 0;
        int gid;

        if (primaryStorage) {
            // Special case the primary SD card.
            // For this we grant write access to the SDCARD_RW group.
            gid = AID_SDCARD_RW;
        } else {
            // For secondary external storage we keep things locked up.

            /* Note: Change the google default setting from AID_MEDIA_RW to AID_SDCARD_RW. */
            // gid = AID_MEDIA_RW;
            gid = AID_SDCARD_RW;
        }
        
#ifdef MTK_EMMC_DISCARD     
        if (Fat::doMount(devicePath, "/mnt/secure/staging", false, false, false,
                AID_SYSTEM, gid, 0702, true, IsEmmcStorage())) {
            SLOGE("%s failed to mount via VFAT (%s)\n", devicePath, strerror(errno));
            #ifdef VENDOR_EDIT
            //LinJie.Xu@Prd.SysSrv.USB, 2013/04/02, Add for if we umount failed last time may cause mount failed.
            //we just try umount it first
            SLOGE("%s failed to mount via VFAT (%d)\n", devicePath, errno);
             if(errno == 16)
             {
                if(doUnmount("/mnt/secure/staging", true))
                {
                    SLOGE("unmount /mnt/secure/staging failed\n");
                     continue;
                }
                else
                {
                    if (Fat::doMount(devicePath, "/mnt/secure/staging", false, false, false,
                             AID_SYSTEM, gid, 0702, true, IsEmmcStorage()))
                             {
                                SLOGE("%s failed to mount via VFAT twice(%s)\n", devicePath, strerror(errno));
                                 continue;
                             }
                }
             }
            #endif /* VENDOR_EDIT */
 
            
           
        }
#else //MTK_EMMC_DISCARD   
        if (Fat::doMount(devicePath, "/mnt/secure/staging", false, false, false,
                AID_SYSTEM, gid, 0702, true)) {
            SLOGE("%s failed to mount via VFAT (%s)\n", devicePath, strerror(errno));
                       #ifdef VENDOR_EDIT
            //LinJie.Xu@Prd.SysSrv.USB, 2013/04/02, Add for  if we umount failed last time may cause mount failed.
            //we just try umount it first
            SLOGE("%s failed to mount via VFAT (%d)\n", devicePath, errno);
             if(errno == 16)
             {
                if(doUnmount("/mnt/secure/staging", true))
                {
                    SLOGE("unmount /mnt/secure/staging failed\n");
                     continue;
                }
                else
                {
                   if (Fat::doMount(devicePath, "/mnt/secure/staging", false, false, false,
                                    AID_SYSTEM, gid, 0702, true))
                             {
                                SLOGE("%s failed to mount via VFAT twice(%s)\n", devicePath, strerror(errno));
                                 continue;
                             }
                }
             }
            #endif /* VENDOR_EDIT */
        }
#endif //MTK_EMMC_DISCARD           

        SLOGI("Device %s, target %s mounted @ /mnt/secure/staging", devicePath, getMountpoint());
        SLOGI("PrimaryStory = %d, externalStorage = %s", primaryStorage, externalStorage);

        protectFromAutorunStupidity();

        // only create android_secure on primary storage
    #ifndef VENDOR_EDIT
    //Jiangtao.Guo@Prd.SysSrv.InputManager, 2013/01/25, Modify for 
    /*
          if (primaryStorage && createBindMounts()) {
                SLOGE("Failed to create bindmounts (%s)", strerror(errno));
                umount("/mnt/secure/staging");
                setState(Volume::State_Idle);
                return -1;
              }
    */
    #else /* VENDOR_EDIT */
             property_set("persist.oppo.Nospace", "0"); 
            if(isExternal && createBindMounts()) {
            
                if(errno == 28) // No space left on device
                {
                    property_set("persist.oppo.Nospace", "1");  
                }
               else
               {
                    SLOGE("Failed to create bindmounts (%s)", strerror(errno));
                    umount("/mnt/secure/staging");
                    setState(Volume::State_Idle);
                    return -1;
               }
            }
    #endif /* VENDOR_EDIT */
    
     

        /*
         * Now that the bindmount trickery is done, atomically move the
         * whole subtree to expose it to non priviledged users.
         */
        if (doMoveMount("/mnt/secure/staging", getMountpoint(), false)) {
            SLOGE("Failed to move mount (%s)", strerror(errno));
            umount("/mnt/secure/staging");
            setState(Volume::State_Idle);
            return -1;
        }
        int fd;
        if ((fd = open(devicePath, O_RDWR)) < 0) {
             SLOGE("Cannot open device '%s' (errno=%d)", devicePath, errno);				  
        }
        else {
           setState(Volume::State_Mounted, Fat::isFat32(fd));
           close(fd);
        }
        mCurrentlyMountedKdev = deviceNodes[i];
        return 0;
    }

    SLOGE("Volume %s found no suitable devices for mounting :(\n", getLabel());

    curState = getState();
    if (curState == Volume::State_NoMedia) {
        SLOGI("Mount fail caused by NoMedia! Update errno to ENODEV");
        errno = ENODEV;
    } 

    if ((curState != Volume::State_NoMedia) && 
	(curState != Volume::State_Mounted)) {
         setState(Volume::State_Idle);
    }

    if(curState == Volume::State_Mounted) {
        return 0;
    }
    return -1;
}

int Volume::createBindMounts() {
    unsigned long flags;

    /*
     * Rename old /android_secure -> /.android_secure
     */
    if (!access("/mnt/secure/staging/android_secure", R_OK | X_OK) &&
         access(SEC_STG_SECIMGDIR, R_OK | X_OK)) {
        if (rename("/mnt/secure/staging/android_secure", SEC_STG_SECIMGDIR)) {
            SLOGE("Failed to rename legacy asec dir (%s)", strerror(errno));
        }
    }

    /*
     * Ensure that /android_secure exists and is a directory
     */
    if (access(SEC_STG_SECIMGDIR, R_OK | X_OK)) {
        if (errno == ENOENT) {
            if (mkdir(SEC_STG_SECIMGDIR, 0777)) {
                SLOGE("Failed to create %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
                return -1;
            }
        } else {
            SLOGE("Failed to access %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
            return -1;
        }
    } else {
        struct stat sbuf;

        if (stat(SEC_STG_SECIMGDIR, &sbuf)) {
            SLOGE("Failed to stat %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
            return -1;
        }
        if (!S_ISDIR(sbuf.st_mode)) {
            SLOGE("%s is not a directory", SEC_STG_SECIMGDIR);
            errno = ENOTDIR;
            return -1;
        }
    }

    /*
     * Bind mount /mnt/secure/staging/android_secure -> /mnt/secure/asec so we'll
     * have a root only accessable mountpoint for it.
     */
    if (mount(SEC_STG_SECIMGDIR, SEC_ASECDIR_EXT, "", MS_BIND, NULL)) {
        SLOGE("Failed to bind mount points %s -> %s (%s)",
                SEC_STG_SECIMGDIR, SEC_ASECDIR_EXT, strerror(errno));
        return -1;
    }

    /*
     * Mount a read-only, zero-sized tmpfs  on <mountpoint>/android_secure to
     * obscure the underlying directory from everybody - sneaky eh? ;)
     */
    if (mount("tmpfs", SEC_STG_SECIMGDIR, "tmpfs", MS_RDONLY, "size=0,mode=000,uid=0,gid=0")) {
        SLOGE("Failed to obscure %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
        umount("/mnt/asec_secure");
        return -1;
    }
        return 0;
}

int Volume::doMoveMount(const char *src, const char *dst, bool force) {
    unsigned int flags = MS_MOVE;
    int retries = 5;

    while(retries--) {
        if (!mount(src, dst, "", flags, NULL)) {
            if (mDebug) {
                SLOGD("Moved mount %s -> %s sucessfully", src, dst);
            }
            return 0;
        } else if (errno != EBUSY) {
            SLOGE("Failed to move mount %s -> %s (%s)", src, dst, strerror(errno));
            return -1;
        }
        int action = 0;

        if (force) {
            if (retries == 1) {
                action = 2; // SIGKILL
            } else if (retries == 2) {
                action = 1; // SIGHUP
            }
        }
        SLOGW("Failed to move %s -> %s (%s, retries %d, action %d)",
                src, dst, strerror(errno), retries, action);
        Process::killProcessesWithOpenFiles(src, action);
        usleep(1000*250);
    }

    errno = EBUSY;
    SLOGE("Giving up on move %s -> %s (%s)", src, dst, strerror(errno));
	Process::FindProcessesWithOpenFiles(src);
    return -1;
}

int Volume::doUnmount(const char *path, bool force) {
    int retries = 3;

    bool isHotPlug = mVm->getHotPlug();
    if (isHotPlug == true) {
        retries = 5;        
    }

    SLOGD("doUnmount: %s retries = %d, isHotPlug=%d", path, retries, isHotPlug);

    if (mDebug) {
        SLOGD("Unmounting {%s}, force = %d", path, force);
    }

    while (retries--) {
        if (!umount(path) || errno == EINVAL || errno == ENOENT) {
            SLOGI("%s sucessfully unmounted", path);
            return 0;
        }

        int action = 0;

        if (force) {
            if (retries == 1) {
                action = 2; // SIGKILL
            } else if (retries == 2) {
                action = 1; // SIGHUP
            }
        }

        SLOGW("Failed to unmount %s (%s, retries %d, action %d)",
                path, strerror(errno), retries, action);

        Process::killProcessesWithOpenFiles(path, action);
        if (retries > 0) 
          usleep(1000*1000);
        
        if(isHotPlug && (retries == 1))
            usleep(1000*1000);

    }
    errno = EBUSY;
    SLOGE("Giving up on unmount %s (%s)", path, strerror(errno));
	Process::FindProcessesWithOpenFiles(path);
    return -1;
}

int Volume::unmountVol(bool force, bool revert) {
    int i, rc;
    const char* externalStorage = getenv("EXTERNAL_STORAGE");
    bool primaryStorage = externalStorage && !strcmp(getMountpoint(), externalStorage);


      #ifdef VENDOR_EDIT
     //Jiangtao.Guo@Prd.SysSrv.InputManager, 2013/01/25, Add for 
    bool is_multi_user_supported = getenv("EMULATED_STORAGE_TARGET")!=NULL;
        
		bool isExternal = false;
		SLOGD("mountVol:  getMountpoint()= %s\n", getMountpoint());
		if (access(getMountpoint(), F_OK))
        {
            SLOGE("Mountpoint doesn't exist, crearte one");
            mkdir(getMountpoint(), 0777);
        }
         #ifdef OPPO_EMMC_DYNAMICMOUNT_SUPPORT
			SLOGD("access = %d\n", access("/sys/block/mmcblk1", F_OK));
            SLOGD("primaryStorage = %d\n",primaryStorage);
             if(access("/sys/block/mmcblk1", F_OK) == 0 && primaryStorage)
             {
                isExternal = true;
             }
         #else
            isExternal = true;
         #endif
     #endif /* VENDOR_EDIT */
    if (getState() != Volume::State_Mounted) {
        SLOGE("Volume %s unmount request when not mounted", getLabel());
        errno = EINVAL;
        return UNMOUNT_NOT_MOUNTED_ERR;
    }

    #ifdef MTK_SHARED_SDCARD
    if (IsEmmcStorage()) {
        setState(Volume::State_Idle);
        return 0;
    }    
    #endif

    setState(Volume::State_Unmounting);
    usleep(1000 * 100); // Give the framework some time to react


    /*
     * First move the mountpoint back to our internal staging point
     * so nobody else can muck with it while we work.
     */
    const char* handleMntPoint;
    if(is_multi_user_supported) {
        handleMntPoint = getMountpoint();      
    }
    else {
        if (doMoveMount(getMountpoint(), SEC_STGDIR, force)) {
            SLOGE("Failed to move mount %s => %s (%s)", getMountpoint(), SEC_STGDIR, strerror(errno));
            setState(Volume::State_Mounted);
            return -1;
        }
        protectFromAutorunStupidity();
        handleMntPoint = SEC_STGDIR;
    }

#ifndef VENDOR_EDIT
//LinJie.Xu@Prd.SysSrv.USB, 2013/03/04, Modify for 
/*
   if (primaryStorage){
*/
#else /* VENDOR_EDIT */
     if (isExternal){
#endif /* VENDOR_EDIT */
    
   

        /*
         * Unmount the tmpfs which was obscuring the asec image directory
         * from non root users
         */
        char secure_dir[PATH_MAX];
        snprintf(secure_dir, PATH_MAX, "%s/.android_secure", handleMntPoint); 
        if (doUnmount(secure_dir, force)) {
            SLOGE("Failed to unmount tmpfs on %s (%s)", secure_dir, strerror(errno));
            goto fail_republish;
        }

        /*
         * Remove the bindmount we were using to keep a reference to
         * the previously obscured directory.
         */

        if (doUnmount(Volume::SEC_ASECDIR_EXT, force)) {
            SLOGE("Failed to remove bindmount on %s (%s)", SEC_ASECDIR_EXT, strerror(errno));
            goto fail_remount_tmpfs;
        }
    }

    /*
     * Finally, unmount the actual block device from the staging dir
     */
    if (doUnmount(handleMntPoint, force)) {
        SLOGE("Failed to unmount %s (%s)", handleMntPoint, strerror(errno));
    #ifndef VENDOR_EDIT
    //LinJie.Xu@Prd.SysSrv.USB, 2013/04/02, Modify for if we unmount failed,we just unmount when next mount
    /*
         goto fail_recreate_bindmount;
    */
    #else /* VENDOR_EDIT */
          goto fail_do_move_mount;
    #endif /* VENDOR_EDIT */
        

    }


    SLOGI("%s unmounted sucessfully", handleMntPoint);

    /* If this is an encrypted volume, and we've been asked to undo
     * the crypto mapping, then revert the dm-crypt mapping, and revert
     * the device info to the original values.
     */
    if (revert && isDecrypted()) {
        cryptfs_revert_volume(getLabel());
        revertDeviceInfo();
        SLOGI("Encrypted volume %s reverted successfully", getMountpoint());
    }

    setState(Volume::State_Idle);
    mCurrentlyMountedKdev = -1;
    return 0;

    /*
     * Failure handling - try to restore everything back the way it was
     */
fail_recreate_bindmount:
    if (mount(SEC_STG_SECIMGDIR, SEC_ASECDIR_EXT, "", MS_BIND, NULL)) {
        SLOGE("Failed to restore bindmount after failure! - Storage will appear offline!");
        goto out_nomedia;
    }
fail_remount_tmpfs:
    if (mount("tmpfs", SEC_STG_SECIMGDIR, "tmpfs", MS_RDONLY, "size=0,mode=0,uid=0,gid=0")) {
        SLOGE("Failed to restore tmpfs after failure! - Storage will appear offline!");
        goto out_nomedia;
    }
fail_republish:
    if (doMoveMount(SEC_STGDIR, getMountpoint() , force) ){
        SLOGE("Failed to republish mount after failure! - Storage will appear offline!");
        goto out_nomedia;
    }

    setState(Volume::State_Mounted);
    return -1;

out_nomedia:
    setState(Volume::State_NoMedia);
    return -1;
#ifdef VENDOR_EDIT
//LinJie.Xu@Prd.SysSrv.USB, 2013/04/02, Add for 
fail_do_move_mount:
    setState(Volume::State_Idle);
    return -1;
#endif /* VENDOR_EDIT */

}
int Volume::initializeMbr(const char *deviceNode) {
    struct disk_info dinfo;

    SLOGI("Enter Volume::initializeMbr()\n");	

    memset(&dinfo, 0, sizeof(dinfo));

    if (!(dinfo.part_lst = (struct part_info *) malloc(MAX_NUM_PARTS * sizeof(struct part_info)))) {
        SLOGE("Failed to malloc prt_lst");
        return -1;
    }

    memset(dinfo.part_lst, 0, MAX_NUM_PARTS * sizeof(struct part_info));
    dinfo.device = strdup(deviceNode);
    dinfo.scheme = PART_SCHEME_MBR;
    dinfo.sect_size = 512;
    dinfo.skip_lba = 2048;
    dinfo.num_lba = 0;
    dinfo.num_parts = 1;

    struct part_info *pinfo = &dinfo.part_lst[0];

    pinfo->name = strdup("android_sdcard");
    pinfo->flags |= PART_ACTIVE_FLAG;
    pinfo->type = PC_PART_TYPE_FAT32;
    pinfo->len_kb = -1;
    SLOGI("Volume::initializeMbr() -- calls apply_disk_config()\n");
    int rc = apply_disk_config(&dinfo, 0);
    SLOGI("Volume::initializeMbr() -- exit apply_disk_config()\n");
    if (rc) {
        SLOGE("Failed to apply disk configuration (%d)", rc);
        goto out;
    }

 out:
    SLOGI("Exit Volume::initializeMbr() -- free name\n");
    free(pinfo->name);
    SLOGI("Exit Volume::initializeMbr() -- free dinfo.device\n");
    free(dinfo.device);
    SLOGI("Exit Volume::initializeMbr() -- free dinfo.part_lst\n");
    free(dinfo.part_lst);

    return rc;
}


char *Volume::replace(const char *src, char *token, char *target) {
    static char buffer[1024];
    char *pch;
    if( !(pch = strstr(src, token)))
    {
        strcpy(buffer, src);
        return buffer;
    }
    strncpy( buffer, src, pch-src);
    buffer[pch-src]=0;
    sprintf( buffer+(pch-src), "%s%s", target, pch+strlen(token));
    return buffer;

}
