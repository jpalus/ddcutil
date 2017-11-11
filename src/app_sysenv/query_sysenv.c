/* query_sysenv.c
 *
 * <copyright>
 * Copyright (C) 2014-2017 Sanford Rockowitz <rockowitz@minsoft.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * </endcopyright>
 */

#include <config.h>

#define _GNU_SOURCE 1       // for function group_member

/** \cond */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-2.0/glib.h>         // glib-2.0/ to avoid bogus eclipse error
#include <grp.h>
#include <limits.h>
// #include <libosinfo-1.0/osinfo/osinfo.h>
// #include <libudev.h>        // not yet used
#include <linux/hiddev.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef USE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#endif

#include "util/data_structures.h"
#include "util/device_id_util.h"
#include "util/edid.h"
#include "util/file_util.h"
#include "util/report_util.h"
#include "util/string_util.h"
#include "util/subprocess_util.h"
#include "util/sysfs_util.h"
#ifdef PROBE_USING_SYSTEMD
#include "util/systemd_util.h"
#endif
#ifdef USE_X11
#include "util/x11_util.h"
#endif
#include "util/udev_i2c_util.h"
#include "util/udev_usb_util.h"
#include "util/udev_util.h"
/** \endcond */

#include "base/build_info.h"
#include "base/core.h"
#include "base/ddc_errno.h"
#include "base/linux_errno.h"

#include "i2c/i2c_bus_core.h"

#include "ddc/ddc_packet_io.h"

#include "adl/adl_shim.h"

#include "query_sysenv_base.h"
#include "query_sysenv_dmidecode.h"
#include "query_sysenv_drm.h"
#include "query_sysenv_i2c.h"
#include "query_sysenv_logs.h"
#include "query_sysenv_procfs.h"
#include "query_sysenv_sysfs.h"
#include "query_sysenv_xref.h"

#include "query_sysenv.h"


/** Perform redundant checks as cross-verification */
bool redundant_i2c_device_identification_checks = true;


//
// Get list of /dev/i2c devices
//
// There are too many ways of doing this throughout the code.
// Consolidate them here.  (IN PROGRESS)
//

Byte_Value_Array get_i2c_devices_by_existence_test() {
   Byte_Value_Array bva = bva_create();
   for (int busno=0; busno < I2C_BUS_MAX; busno++) {
      if (i2c_device_exists(busno)) {
         // if (!is_ignorable_i2c_device(busno))
         bva_append(bva, busno);
      }
   }
   return bva;
}


Byte_Value_Array get_i2c_devices_by_ls() {
   Byte_Value_Array bva = bva_create();

   int ival;

   // returns array of I2C bus numbers in string form, sorted in numeric order
   GPtrArray * busnums = execute_shell_cmd_collect("ls /dev/i2c* | cut -c 10- | sort -n");

   if (!busnums) {
      rpt_vstring(1, "No I2C buses found");
      goto bye;
   }
   if (busnums->len > 0) {
      bool isint = str_to_int(g_ptr_array_index(busnums,0), &ival);
      if (!isint) {
         rpt_vstring(1, "Apparently no I2C buses");
         goto bye;
      }
   }
   for (int ndx = 0; ndx < busnums->len; ndx++) {
      char * sval = g_ptr_array_index(busnums, ndx);
      bool isint = str_to_int(sval, &ival);
      if (!isint) {
         rpt_vstring(1, "Parsing error.  Invalid I2C bus number: %s", sval);
      }
      else {
         bva_append(bva, ival);
         // is_smbus_device_using_sysfs(ival);
      }
   }
bye:
   if (busnums)
      g_ptr_array_free(busnums, true);

   return bva;
}


/** Consolidated function to identify I2C devices.
 *
 *  \return #ByteValueArray of bus numbers for detected I2C devices
 */
// TODO: simplify, no longer need to test with multiple methods
Byte_Value_Array identify_i2c_devices() {

   Byte_Value_Array i2c_device_numbers_result = NULL;   // result

   Byte_Value_Array bva1 = NULL;
   Byte_Value_Array bva2 = NULL;
   Byte_Value_Array bva3 = NULL;
   Byte_Value_Array bva4 = NULL;

   bva1 = get_i2c_devices_by_existence_test();
   if (redundant_i2c_device_identification_checks) {
      bva2 = get_i2c_devices_by_ls();
      bva3 = get_i2c_device_numbers_using_udev(/* include_smbus= */ true);
      bva4 = get_i2c_device_numbers_using_udev_w_sysattr_name_filter(NULL);

      assert(bva_sorted_eq(bva1,bva2));
      assert(bva_sorted_eq(bva1,bva3));
      assert(bva_sorted_eq(bva1,bva4));
   }

   i2c_device_numbers_result = bva1;
   if (redundant_i2c_device_identification_checks) {
      bva_free(bva2);
      bva_free(bva3);
      bva_free(bva4);
   }
   // DBGMSG("Identified %d I2C devices", bva_length(bva1));
   return i2c_device_numbers_result;
}




/** Compile time and runtime checks of endianness.
 *
 *  \param depth logical indentation depth
 */
static void report_endian(int depth) {
   int d1 = depth+1;
   rpt_title("Byte order checks:", depth);

   bool is_bigendian = (*(uint16_t *)"\0\xff" < 0x100);
   rpt_vstring(d1, "Is big endian (local test):       %s", bool_repr(is_bigendian));

   rpt_vstring(d1, "WORDS_BIGENDIAN macro (autoconf): "
#ifdef WORDS_BIGENDIAN
         "defined"
#else
         "not defined"
#endif
         );
   rpt_vstring(d1, "__BYTE_ORDER__ macro (gcc):       "
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
         "__ORDER_LITTLE_ENDIAN__"
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
         "__ORDER_BIG_ENDIAN__"
#elif __BYTE_ORDER__ == __ORDER_PDP_ENDIAN__
         "__ORDER_PDP_ENDIAN__"
#else
         "unexpected value"
#endif
         );

#ifdef REDUNDANT
   __u32 i = 1;
   bool is_bigendian2 =  ( (*(char*)&i) == 0 );
   rpt_vstring(d1, "Is big endian (runtime test): %s", bool_repr(is_bigendian2));
#endif
}


//
// Higher level functions
//

/** Reports basic system information
 *
 * \param  accum  pointer to struct in which information is returned
 */
static void query_base_env(Env_Accumulator * accum) {
   rpt_vstring(0, "ddcutil version: %s", BUILD_VERSION);
   rpt_nl();

   sysenv_rpt_file_first_line("/proc/version", NULL, 0);

   char * expected_architectures[] = {"x86_64", "i386", "i686", "armv7l", NULL};
   char * architecture   = execute_shell_cmd_one_line_result("arch");      // alt: use uname -m
   char * distributor_id = execute_shell_cmd_one_line_result("lsb_release -s -i");  // e.g. Ubuntu, Raspbian
   char * release        = execute_shell_cmd_one_line_result("lsb_release -s -r");
   rpt_nl();
   rpt_vstring(0, "Architecture:     %s", architecture);
   rpt_vstring(0, "Distributor id:   %s", distributor_id);
   rpt_vstring(0, "Release:          %s", release);

   if ( ntsa_find(expected_architectures, architecture) >= 0) {
      rpt_vstring(0, "Found a known architecture");
   }
   else {
      rpt_vstring(0, "Unexpected architecture %s.  Please report.", architecture);
   }

   accum->architecture   = architecture;
   accum->distributor_id = distributor_id;
   accum->is_raspbian    = streq(accum->distributor_id, "Raspbian");
   accum->is_arm         = str_starts_with(accum->architecture, "arm");
   free(release);

#ifdef REDUNDANT
   rpt_nl();
   rpt_vstring(0,"/etc/os-release...");
   bool ok = execute_shell_cmd_rpt("grep PRETTY_NAME /etc/os-release", 1 /* depth */);
   if (!ok)
      rpt_vstring(1,"Unable to read PRETTY_NAME from /etc/os-release");
#endif

   rpt_nl();
   sysenv_rpt_file_first_line("/proc/cmdline", NULL, 0);

   if (get_output_level() >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Processor information as reported by lscpu:");
        bool ok = execute_shell_cmd_rpt("lscpu", 1);
        if (!ok) {   // lscpu should always be there, but just in case:
           rpt_vstring(1, "Command lscpu not found");
           rpt_nl();
           rpt_title("Processor information from /proc/cpuinfo:", 0);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep vendor_id | uniq", 1);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"cpu family\" | uniq", 1);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"model[[:space:]][[:space:]]\" | uniq",  1);   //  "model"
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"model name\" | uniq",  1);   // "model name"
       }

       rpt_nl();
        if (accum->is_arm) {
           rpt_vstring(0, "Skipping dmidecode checks on architecture %s.", accum->architecture);
        }
        else {
           query_dmidecode();
        }

      rpt_nl();
      report_endian(0);
   }

}



/** Checks on the existence and accessibility of /dev/i2c devices.
 *
 *  \param accum   accumulates environment information
 *
 * Checks that user has RW access to all /dev/i2c devices.
 * Checks if group i2c exists and whether the current user is a member.
 * Checks for references to i2c in /etc/udev/makedev.d
 *
 * If the only driver in driver_list is fglrx, the tests are
 * skipped (or if verbose output, purely informational).
 *
 * TODO: ignore i2c smbus devices
 *
 *  \remark
 *  assumes drivers already detected, i.e. **accum->driver_list** already set
 *
 *  \remark
 *  Sets:
 *    accum->group_i2c_exists
 *    accum->cur_user_in_group_i2c
 *    accum->cur_user_any_devi2c_rw
 *    accum->cur_user_all_devi2c_rw
 */
static void check_i2c_devices(Env_Accumulator * accum) {
   bool debug = false;
   DBGMSF(debug, "Starting");

   accum->dev_i2c_devices_required = true;
   accum->group_i2c_exists = false;
   accum->cur_user_in_group_i2c = false;
   accum->cur_user_any_devi2c_rw = false;
   accum->cur_user_all_devi2c_rw = true;  // i.e. none fail the test
   accum->any_dev_i2c_has_group_i2c = false;
   accum->all_dev_i2c_has_group_i2c = true;
   accum->any_dev_i2c_is_group_rw = false;
   accum->all_dev_i2c_is_group_rw = true;

   Driver_Name_Node * driver_list = accum->driver_list;
   // int rc;
   // char username[32+1];       // per man useradd, max username length is 32
   char *uname = NULL;
   // bool have_i2c_devices = false;

   rpt_vstring(0,"Checking /dev/i2c-* devices...");
   DDCA_Output_Level output_level = get_output_level();

   bool just_fglrx = only_fglrx(driver_list);
   if (just_fglrx){
      accum->dev_i2c_devices_required = false;
      rpt_nl();
      rpt_vstring(0,"Apparently using only the AMD proprietary driver fglrx.");
      rpt_vstring(0,"Devices /dev/i2c-* are not required.");
      // TODO: delay leaving to properl set other variables
      if (output_level < DDCA_OL_VERBOSE)
         return;
      rpt_vstring(0, "/dev/i2c device detail is purely informational.");
   }

   rpt_nl();
   rpt_multiline(0,
          "Unless the system is using the AMD proprietary driver fglrx, devices /dev/i2c-*",
          "must exist and the logged on user must have read/write permission for those",
          "devices (or at least those devices associated with monitors).",
          "Typically, this access is enabled by:",
          "  - setting the group for /dev/i2c-* to i2c",
          "  - setting group RW permissions for /dev/i2c-*",
          "  - making the current user a member of group i2c",
          "Alternatively, this could be enabled by just giving everyone RW permission",
          "The following tests probe for these conditions.",
          NULL
         );

   rpt_nl();
   rpt_vstring(0,"Checking for /dev/i2c-* devices...");
   execute_shell_cmd_rpt("ls -l /dev/i2c-*", 1);

#ifdef OLD
   rc = getlogin_r(username, sizeof(username));
   printf("(%s) getlogin_r() returned %d, strlen(username)=%zd\n", __func__,
          rc, strlen(username));
   if (rc == 0)
      printf("(%s) username = |%s|\n", __func__, username);
   // printf("\nLogged on user:  %s\n", username);
   printf("(%s) getlogin() returned |%s|\n", __func__, getlogin());
   char * cmd = "echo $LOGNAME";
   printf("(%s) executing command: %s\n", __func__, cmd);
   bool ok = execute_shell_cmd_rpt(cmd, 0);
   printf("(%s) execute_shell_cmd() returned %s\n", __func__, bool_repr(ok));

#endif
   uid_t uid = getuid();
   // uid_t euid = geteuid();
   // printf("(%s) uid=%u, euid=%u\n", __func__, uid, euid);
   // gets logged on user name, user id, group id
   struct passwd *  pwd = getpwuid(uid);
   rpt_nl();
   rpt_vstring(0,"Current user: %s (%u)\n", pwd->pw_name, uid);
   uname = strdup(pwd->pw_name);

   bool all_i2c_rw = false;
   int busct = i2c_device_count();   // simple count, no side effects, consider replacing with local code
   if (busct == 0 && !just_fglrx) {
      rpt_vstring(0,"WARNING: No /dev/i2c-* devices found");
   }
   else {
      all_i2c_rw = true;
      int  busno;
      char fnbuf[20];

      for (busno=0; busno < 32; busno++) {
         if (i2c_device_exists(busno)) {
            snprintf(fnbuf, sizeof(fnbuf), "/dev/i2c-%d", busno);
            int rc;
            int errsv;
            DBGMSF(debug, "Calling access() for %s", fnbuf);
            rc = access(fnbuf, R_OK|W_OK);
            if (rc < 0) {
               errsv = errno;
               rpt_vstring(0,"Device %s is not readable and writable.  Error = %s",
                      fnbuf, linux_errno_desc(errsv) );
               all_i2c_rw = false;
               accum->cur_user_all_devi2c_rw = false;
            }
            else
               accum->cur_user_any_devi2c_rw = true;

            struct stat fs;
            rc = stat(fnbuf, &fs);
            if (rc < 0) {
               errsv = errno;
               rpt_vstring(0,"Error getting group information for device %s.  Error = %s",
                      fnbuf, linux_errno_desc(errsv) );
            }
            else {
               bool cur_file_grp_rw =  ( fs.st_mode & S_IRGRP ) && (fs.st_mode & S_IWGRP)  ;
               struct group*  grp;
               errno = 0;
               grp = getgrgid(fs.st_gid);
               if (!grp) {
                  errsv = errno;
                  rpt_vstring(0,"Error getting group information for group %d.  Error = %s",
                         fs.st_gid, linux_errno_desc(errsv) );
               }
               else {
                  char * gr_name = grp->gr_name;
                  if (accum->dev_i2c_common_group_name) {
                     if (!streq(accum->dev_i2c_common_group_name, gr_name))
                        accum->dev_i2c_common_group_name = "MIXED";
                  }
                  else accum->dev_i2c_common_group_name = strdup(gr_name);
                  if (streq(gr_name, "i2c"))
                     accum->any_dev_i2c_has_group_i2c = true;
                  else
                     accum->all_dev_i2c_has_group_i2c = false;

                  DBGMSF(debug, "file=%s, st_gid=%d, gr_name=%s, cur_file_grp_rw=%s",
                        fnbuf, fs.st_gid, gr_name, bool_repr(cur_file_grp_rw));

                  if (fs.st_gid != 0) {    // root group is special case
                     if (cur_file_grp_rw)
                        accum->any_dev_i2c_is_group_rw = true;
                     else
                        accum->all_dev_i2c_is_group_rw = false;
                  }
               }
            }
         }
      }

      if (!all_i2c_rw) {
         rpt_vstring(
               0,
               "WARNING: Current user (%s) does not have RW access to all /dev/i2c-* devices.",
               uname);
      }
      else
         rpt_vstring(0,"Current user (%s) has RW access to all /dev/i2c-* devices.",
               // username);
               uname);
   }

   if (!all_i2c_rw || output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Checking for group i2c...");
      // replaced by C code
      // execute_shell_cmd("grep i2c /etc/group", 1);

      bool group_i2c_exists = false;   // avoid special value in gid_i2c
      // gid_t gid_i2c;
      struct group * pgi2c = getgrnam("i2c");
      if (pgi2c) {
         rpt_vstring(0,"   Group i2c exists");
         accum->group_i2c_exists = true;
         group_i2c_exists = true;
         // gid_i2c = pgi2c->gr_gid;
         // DBGMSG("getgrnam returned gid=%d for group i2c", gid_i2c);
         // DBGMSG("getgrnam() reports members for group i2c: %s", *pgi2c->gr_mem);
         int ndx=0;
         char * curname;
         bool found_curuser = false;
         while ( (curname = pgi2c->gr_mem[ndx]) ) {
            rtrim_in_place(curname);
            // DBGMSG("member_names[%d] = |%s|", ndx, curname);
            if (streq(curname, uname /* username */)) {
               found_curuser = true;
            }
            ndx++;
         }
         if (found_curuser) {
            rpt_vstring(1,"Current user %s is a member of group i2c", uname  /* username */);
            accum->cur_user_in_group_i2c = true;
         }
         else {
            rpt_vstring(1, "WARNING: Current user %s is NOT a member of group i2c", uname /*username*/);
            rpt_vstring(2, "Suggestion:  Add current user to group i2c.");
            rpt_vstring(2, "Use command: sudo usermod -G i2c -a <username>");
         }
      }
      if (!group_i2c_exists) {
         rpt_vstring(0,"   Group i2c does not exist");
      }
      free(uname);
   #ifdef BAD
      // getgroups, getgrouplist returning nonsense
      else {
         uid_t uid = geteuid();
         gid_t gid = getegid();
         struct passwd * pw = getpwuid(uid);
         printf("Effective uid %d: %s\n", uid, pw->pw_name);
         char * uname = strdup(pw->pw_name);
         struct group * pguser = getgrgid(gid);
         printf("Effective gid %d: %s\n", gid, pguser->gr_name);
         if (group_member(gid_i2c)) {
            printf("User %s (%d) is a member of group i2c (%d)\n", uname, uid, gid_i2c);
         }
         else {
            printf("WARNING: User %s (%d) is a not member of group i2c (%d)\n", uname, uid, gid_i2c);
         }

         size_t supp_group_ct = getgroups(0,NULL);
         gid_t * glist = calloc(supp_group_ct, sizeof(gid_t));
         int rc = getgroups(supp_group_ct, glist);
         int errsv = errno;
         DBGMSF(debug, "getgroups() returned %d", rc);
         if (rc < 0) {
            DBGMSF(debug, "getgroups() returned %d", rc);

         }
         else {
            DBGMSG("Found %d supplementary group ids", rc);
            int ndx;
            for (ndx=0; ndx<rc; ndx++) {
               DBGMSG("Supplementary group id: %d", *glist+ndx);
            }

         }

         int supp_group_ct2 = 100;
         glist = calloc(supp_group_ct2, sizeof(gid_t));
         DBGMSG("Calling getgrouplist for user %s", uname);
         rc = getgrouplist(uname, gid, glist, &supp_group_ct2);
         errsv = errno;
         DBGMSG("getgrouplist returned %d, supp_group_ct=%d", rc, supp_group_ct2);
         if (rc < 0) {
            DBGMSF(debug, "getgrouplist() returned %d", rc);
         }
         else {
            DBGMSG("getgrouplist found %d supplementary group ids", rc);
            int ndx;
            for (ndx=0; ndx<rc; ndx++) {
               DBGMSG("Supplementary group id: %d", *glist+ndx);
            }
         }
      }
   #endif

      rpt_nl();
      rpt_vstring(0,"Looking for udev nodes files that reference i2c:");
      execute_shell_cmd_rpt("grep -H i2c /etc/udev/makedev.d/*", 1);
      rpt_nl();
      rpt_vstring(0,"Looking for udev rules files that reference i2c:");
      execute_shell_cmd_rpt("grep -H i2c "
                        "/lib/udev/rules.d/*rules "
                        "/run/udev/rules.d/*rules "
                        "/etc/udev/rules.d/*rules", 1 );
   }
   DBGMSF(debug, "Done");
}


/* Checks if a module is built in to the kernel.
 *
 * Arguments:
 *   module_name    simple module name, as it appears in the file system, e.g. i2c-dev
 *
 * Returns:         true/false
 */
static bool is_module_builtin(char * module_name) {
   bool debug = false;
   bool result = false;

   struct utsname utsbuf;
   int rc = uname(&utsbuf);
   assert(rc == 0);
   // DBGMSG("uname() returned release: %s", &utsbuf.release);

   // works, but simpler to use uname() that doesn't require free(osrelease)
   // char * osrelease = file_get_first_line("/proc/sys/kernel/osrelease", true /* verbose */);
   // assert(streq(utsbuf.release, osrelease));

   char modules_builtin_fn[100];
   snprintf(modules_builtin_fn, 100, "/lib/modules/%s/modules.builtin", utsbuf.release);
   // free(osrelease);


   // TODO: replace shell command with API read and scan of file,
   //       can use code from query_sysenv_logs.c

   char cmdbuf[200];

   snprintf(cmdbuf, 200, "grep -H %s.ko %s", module_name, modules_builtin_fn);
   // DBGMSG("cmdbuf = |%s|", cmdbuf);

   GPtrArray * response = execute_shell_cmd_collect(cmdbuf);
   // internal rc =  0 if found, 256 if not found
   // returns 0 lines if not found
   // NULL response if command error

   // DBGMSG("execute_shell_cmd_collect() returned %d lines", response->len);
   // for (int ndx = 0; ndx < response->len; ndx++) {
   //    puts(g_ptr_array_index(response, ndx));
   // }

   result = (response && response->len > 0);
   g_ptr_array_free(response, true);

   DBGMSF(debug, "module_name = %s, returning %s", module_name, bool_repr(result));
   return result;
}



/* Checks if module i2c_dev is required and if so whether it is loaded.
 * Reports the result.
 *
 * \param  accum  collects environment information
 *
 * \remark
 * Sets #accum->module_i2c_dev_needed
 *      #accum->module_i2c_dev_loaded
 *
 */
static void check_i2c_dev_module(Env_Accumulator * accum) {
   rpt_vstring(0,"Checking for module i2c_dev...");
   struct driver_name_node * video_driver_list = accum->driver_list;  // for transition

   DDCA_Output_Level output_level = get_output_level();

   accum->module_i2c_dev_needed = true;
   accum->module_i2c_dev_loaded = false;

   bool module_required = !only_nvidia_or_fglrx(video_driver_list);
   if (!module_required) {
      rpt_vstring(0,"Using only proprietary nvidia or fglrx driver. Module i2c_dev not required.");
      // if (output_level < DDCA_OL_VERBOSE)
      accum->module_i2c_dev_needed = false;
      return;
      // rpt_vstring(0,"Remaining i2c_dev detail is purely informational.");
   }

   bool is_builtin = is_module_builtin("i2c-dev");
   rpt_vstring(0,"   Module %s is %sbuilt into kernel", "i2c_dev", (is_builtin) ? "" : "NOT ");
   if (is_builtin) {
      accum->module_i2c_dev_loaded = true;
      return;
      // if (output_level < DDCA_OL_VERBOSE)
      //    return;
      // if (module_required)  // no need for duplicate message
      //    rpt_vstring(0,"Remaining i2c_dev detail is purely informational.");
   }

   bool is_loaded = is_module_loaded_using_sysfs("i2c_dev");
   accum->module_i2c_dev_loaded = is_loaded;
      // DBGMSF(debug, "is_loaded=%d", is_loaded);
   if (!is_builtin)
      rpt_vstring(1,"Module %-16s is %sloaded", "i2c_dev", (is_loaded) ? "" : "NOT ");

   if (bva_length(accum->dev_i2c_device_numbers) == 0 && !is_builtin && !is_loaded && module_required) {
      rpt_nl();
      if (!only_nvidia_or_fglrx(video_driver_list)) {
         rpt_vstring(0, "No /dev/i2c devices found, but module i2c_dev is not loaded.");
         rpt_vstring(0, "Suggestion:");
         rpt_vstring(1, "Manually load module i2c-dev using the command \"modprobe i2c-dev\"");
         rpt_vstring(1,  "If this solves the problem, put an entry in directory /etc/modules-load.c");
         rpt_vstring(1, "that will cause i2c-dev to be loaded.  Type \"man modules-load.d\" for details");
         rpt_nl();
      }
   }
   if ( (!is_loaded && !is_builtin) || output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Check that kernel module i2c_dev is being loaded by examining files where this would be specified...");
      execute_shell_cmd_rpt("grep -H i2c[-_]dev "
                        "/etc/modules "
                        "/etc/modules-load.d/*conf "
                        "/run/modules-load.d/*conf "
                        "/usr/lib/modules-load.d/*conf "
                        , 1);
      rpt_nl();
      rpt_vstring(0,"Check for any references to i2c_dev in /etc/modprobe.d ...");
      execute_shell_cmd_rpt("grep -H i2c[-_]dev "
                        "/etc/modprobe.d/*conf "
                        "/run/modprobe.d/*conf "
                        , 1);
   }
}


#ifdef OLD

/* Checks for installed packages i2c-tools and libi2c-dev
 */
static void query_packages() {
   rpt_multiline(0,
         "ddcutil requiries package i2c-tools.  Use both dpkg and rpm to look for it.",
          "While we're at it, check for package libi2c-dev which is used for building",
          "ddcutil.",
          NULL
         );

   bool ok;
   // n. apt show produces warning msg that format of output may change.
   // better to use dpkg
   rpt_nl();
   rpt_vstring(0,"Using dpkg to look for package i2c-tools...");
   ok = execute_shell_cmd_rpt("dpkg --status i2c-tools", 1);
   if (!ok)
      rpt_vstring(0,"dpkg command not found");
   else {
      execute_shell_cmd_rpt("dpkg --listfiles i2c-tools", 1);
   }

   rpt_nl();
   rpt_vstring(0,"Using dpkg to look for package libi2c-dev...");
   ok = execute_shell_cmd_rpt("dpkg --status libi2c-dev", 1);
   if (!ok)
      rpt_vstring(0,"dpkg command not found");
   else {
      execute_shell_cmd_rpt("dpkg --listfiles libi2c-dev", 1);
   }

   rpt_nl();
   rpt_vstring(0,"Using rpm to look for package i2c-tools...");
   ok = execute_shell_cmd_rpt("rpm -q -l --scripts i2c-tools", 1);
   if (!ok)
      rpt_vstring(0,"rpm command not found");
}
#endif


/* Performs checks specific to the nvidia and fglrx proprietary video drivers.
 *
 * Arguments:
 *    driver list    list of loaded drivers
 *
 * Returns:          nothing
 */
static void driver_specific_tests(struct driver_name_node * driver_list) {
   rpt_vstring(0,"Performing driver specific checks...");
   bool found_driver_specific_checks = false;

   if (found_driver(driver_list, "nvidia")) {
      found_driver_specific_checks = true;
      rpt_nl();
      rpt_vstring(0,"Checking for special settings for proprietary Nvidia driver ");
      rpt_vstring(0,"(Needed for some newer Nvidia cards).");
      execute_shell_cmd_rpt("grep -iH i2c /etc/X11/xorg.conf /etc/X11/xorg.conf.d/*", 1);
   }

   if (found_driver(driver_list, "fglrx")) {
      found_driver_specific_checks = true;
      rpt_nl();
      rpt_vstring(0,"Performing ADL specific checks...");
#ifdef HAVE_ADL
     if (!adlshim_is_available()) {
        set_output_level(DDCA_OL_VERBOSE);  // force error msg that names missing dll
        bool ok = adlshim_initialize();
        if (!ok)
           printf("WARNING: Using AMD proprietary video driver fglrx but unable to load ADL library\n");
     }
#else
     rpt_vstring(0,"WARNING: Using AMD proprietary video driver fglrx but ddcutil built without ADL support");
#endif
   }

   if (!found_driver_specific_checks)
      rpt_vstring(0,"No driver specific checks apply.");
}


#ifdef USE_X11
//
// Using X11 API
//

/* Reports EDIDs known to X11
 *
 * Arguments:    none
 *
 * Returns:      nothing
 */
void query_x11() {
   GPtrArray* edid_recs = get_x11_edids();
   rpt_nl();
   rpt_vstring(0,"EDIDs reported by X11 for connected xrandr outputs:");
   // DBGMSG("Got %d X11_Edid_Recs\n", edid_recs->len);

   for (int ndx=0; ndx < edid_recs->len; ndx++) {
      X11_Edid_Rec * prec = g_ptr_array_index(edid_recs, ndx);
      // printf(" Output name: %s -> %p\n", prec->output_name, prec->edid);
      // hex_dump(prec->edid, 128);
      rpt_vstring(1, "xrandr output: %s", prec->output_name);
      rpt_vstring(2, "Raw EDID:");
      rpt_hex_dump(prec->edidbytes, 128, 2);
      Parsed_Edid * parsed_edid = create_parsed_edid(prec->edidbytes);
      if (parsed_edid) {
         report_parsed_edid_base(
               parsed_edid,
               true,   // verbose
               false,  // show_hex
               2);     // depth
         free_parsed_edid(parsed_edid);
      }
      else {
         rpt_vstring(2, "Unable to parse EDID");
         // printf(" Unparsable EDID for output name: %s -> %p\n", prec->output_name, prec->edidbytes);
         // hex_dump(prec->edidbytes, 128);
      }
      rpt_nl();

      Device_Id_Xref * xref = device_xref_get(prec->edidbytes);
      xref->xrandr_name = strdup(prec->output_name);
   }
   free_x11_edids(edid_recs);

   // Display * x11_disp = open_default_x11_display();
   // GPtrArray *  outputs = get_x11_connected_outputs(x11_disp);
   // close_x11_display(x11_disp);
}
#endif


//
// i2cdetect
//

/** Examines /dev/i2c devices using command i2cdetect, if it exists.
 *
 *  \param  i2c_device_numbers  I2C bus numbers to check
 *
 */
static void query_using_i2cdetect(Byte_Value_Array i2c_device_numbers) {
   assert(i2c_device_numbers);

   int d0 = 0;
   int d1 = 1;

   rpt_vstring(d0,"Examining I2C buses using i2cdetect... ");

   if (bva_length(i2c_device_numbers) == 0) {
      rpt_vstring(d1, "No I2C buses found");
   }
   else {
      for (int ndx=0; ndx< bva_length(i2c_device_numbers); ndx++) {
         int busno = bva_get(i2c_device_numbers, ndx);
         if (is_ignorable_i2c_device(busno)) {
            // calling i2cdetect for an SMBUs device fills dmesg with error messages
            rpt_nl();
            rpt_vstring(d1, "Device /dev/i2c-%d is a SMBus or other ignorable device.  Skipping i2cdetect.", busno);
         }
         else {
            char cmd[80];
            snprintf(cmd, 80, "i2cdetect -y %d", busno);
            rpt_nl();
            rpt_vstring(d1,"Probing bus /dev/i2c-%d using command \"%s\"", busno, cmd);
            int rc = execute_shell_cmd_rpt(cmd, 2 /* depth */);
            // DBGMSG("execute_shell_cmd(\"%s\") returned %d", cmd, rc);
            if (rc != 1) {
               rpt_vstring(d1,"i2cdetect command unavailable");
               break;
            }
         }
      }
   }
}


/** Queries UDEV for devices in subsystem "i2c-dev".
 *  Also looks for devices with name attribute "DPMST"
 */
static void probe_i2c_devices_using_udev() {
   char * subsys_name = "i2c-dev";
   rpt_vstring(0,"Probing I2C devices using udev, susbsystem %s...", subsys_name);
   // probe_udev_subsystem() is in udev_util.c, which is only linked in if USE_USB

   // Detailed scan of I2C device information
   probe_udev_subsystem(subsys_name, /*show_usb_parent=*/ false, 1);
   rpt_nl();

   GPtrArray * summaries = get_i2c_devices_using_udev();
   report_i2c_udev_device_summaries(summaries, "Summary of udev I2C devices",1);
   for (int ndx = 0; ndx < summaries->len; ndx++) {
      Udev_Device_Summary * summary = g_ptr_array_index(summaries, ndx);
      assert( memcmp(summary->marker, UDEV_DEVICE_SUMMARY_MARKER, 4) == 0);
      int busno = udev_i2c_device_summary_busno(summary);
      Device_Id_Xref * xref = device_xref_find_by_busno(busno);
      if (xref) {
         xref->udev_name = strdup(summary->sysattr_name);
         xref->udev_syspath = strdup(summary->devpath);
      }
      else {
         // DBGMSG("Device_Id_Xref not found for busno %d", busno);
      }
   }
   free_udev_device_summaries(summaries);   // ok if summaries == NULL

   rpt_nl();
   char * nameattr = "DPMST";
   rpt_vstring(0,"Looking for udev devices with name attribute %s...", nameattr);
   summaries = find_devices_by_sysattr_name(nameattr);
   report_i2c_udev_device_summaries(summaries, "Summary of udev DPMST devices...",1);
   free_udev_device_summaries(summaries);   // ok if summaries == NULL
}


/** Analyze collected environment information, Make suggestions.
 *
 * \param accum  accumulated environment information
 * \param depth  logical indentation depth
 */
void final_analysis(Env_Accumulator * accum, int depth) {
   int d1 = depth + 1;
   int d2 = depth + 2;
   int d3 = depth + 3;
   int suggestion_ct = 0;

   bool odd_groups = accum->dev_i2c_common_group_name &&
                    !streq(accum->dev_i2c_common_group_name, "root") &&
                    !streq(accum->dev_i2c_common_group_name, "i2c");

   rpt_vstring(depth, "Configuration suggestions:");

   if (odd_groups)
      rpt_multiline(d1, "",
                        "/dev/i2c devices have non-standard or varying group names.",
                        "Suggestions are incomplete.",
                        NULL);

   // TODO: Also compare dev_i2c_devices vs sys_bus_i2c_devices ?
   if (bva_length(accum->dev_i2c_device_numbers) == 0 &&
       accum->module_i2c_dev_needed &&
       !accum->module_i2c_dev_loaded)
   {
      rpt_nl();
      rpt_vstring(d1, "Suggestion %d:", ++suggestion_ct);
      rpt_vstring(d2, "No /dev/i2c devices found, and module i2c_dev is not loaded.");
      rpt_vstring(d2, "Manually load module i2c-dev using the command \"modprobe i2c-dev\"");
      rpt_vstring(d2,  "If this solves the problem, put an entry in directory /etc/modules-load.c");
      rpt_vstring(d2, "that will cause i2c-dev to be loaded.  Type \"man modules-load.d\" for details");
   }

   if (!accum->cur_user_all_devi2c_rw) {
      // TODO: case of cur_user_all_dev_i2c_rw == false but
      //               cur_user_any_dev_i2c_rw == true

      if (!accum->group_i2c_exists) {
         rpt_nl();
         rpt_vstring(d1, "Suggestion %d:", ++suggestion_ct);
         rpt_vstring(d2, "Create group i2c, assign all /dev/i2c devices to group i2c,");
         rpt_vstring(d2, "and add the current user to group i2c");
         rpt_vstring(d2, "To create group i2c, use command:");
         rpt_vstring(d3, "sudo groupadd --system i2c");
      }
      if (accum->group_i2c_exists && !accum->all_dev_i2c_has_group_i2c) {
         // TODO handle odd case of all_dev_i2c_has_group_i2c == false but
         //                         any_dev_i2c_has_group_i2c == true
         rpt_nl();
         rpt_vstring(d1, "Suggestion %d:", ++suggestion_ct);
         rpt_vstring(d2, "Assign /dev/i2c devices to group i2c by adding rule to /etc/udev/rules.d");
      }

      if (!accum->cur_user_in_group_i2c) {
         rpt_nl();
         rpt_vstring(d1, "Suggestion %d:", ++suggestion_ct);
         rpt_vstring(d2, "Current user is not a member of group i2c");
         rpt_vstring(d2, "Execute command:");
         rpt_vstring(d3, "sudo usermod -G i2c -a <username>");
      }
   }

   if (suggestion_ct == 0)
      rpt_vstring(d1, "None");
}

//
// Mainline
//

/* Master function to query the system environment
 *
 * Arguments:    none
 *
 * Returns:      nothing
 */
void query_sysenv() {
   device_xref_init();

   Env_Accumulator * accumulator = env_accumulator_new();

   rpt_nl();
   rpt_vstring(0,"*** Basic System Information ***");
   rpt_nl();
   query_base_env(accumulator);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 1: Identify video card and driver ***");
   rpt_nl();
   query_card_and_driver_using_sysfs(accumulator);


   rpt_nl();
   rpt_vstring(0,"*** Primary Check 2: Check that /dev/i2c-* exist and writable ***");
   rpt_nl();
   Byte_Value_Array i2c_device_numbers = identify_i2c_devices();
   accumulator->dev_i2c_device_numbers = i2c_device_numbers;
   assert(i2c_device_numbers);
   rpt_vstring(0, "Identified %d I2C devices", bva_length(accumulator->dev_i2c_device_numbers));
   rpt_nl();
   check_i2c_devices(accumulator);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 3: Check that module i2c_dev is loaded ***");
   rpt_nl();
   check_i2c_dev_module(accumulator);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 4: Driver specific checks ***");
   rpt_nl();
   driver_specific_tests(accumulator->driver_list);

   // TODO: move to end of function
   // Free the driver list created by query_card_and_driver_using_sysfs()
   // free_driver_name_list(accumulator->driver_list);
   // driver_list = NULL;

#ifdef OLD
   rpt_nl();
   rpt_vstring(0,"*** Primary Check 5: Installed packages ***");
   rpt_nl();
   query_packages();
   rpt_nl();
#endif

   rpt_nl();
   rpt_vstring(0,"*** Additional probes ***");
   // printf("Gathering card and driver information...\n");
   rpt_nl();
   query_proc_modules_for_video();
   if (!accumulator->is_arm) {
      // rpt_nl();
      // query_card_and_driver_using_lspci();
      //rpt_nl();
      //query_card_and_driver_using_lspci_alt();
   }
   rpt_nl();
   query_loaded_modules_using_sysfs();
   rpt_nl();
   query_sys_bus_i2c(accumulator);

   DDCA_Output_Level output_level = get_output_level();
   if (output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      query_proc_driver_nvidia();

      rpt_nl();
      query_i2c_buses();

      rpt_nl();
      rpt_vstring(0,"xrandr connection report:");
      execute_shell_cmd_rpt("xrandr|grep connected", 1 /* depth */);
      rpt_nl();

      rpt_vstring(0,"Checking for possibly conflicting programs...");
      execute_shell_cmd_rpt("ps aux | grep ddccontrol | grep -v grep", 1);
      rpt_nl();

      query_using_i2cdetect(accumulator->dev_i2c_device_numbers);

      raw_scan_i2c_devices(accumulator);

#ifdef USE_X11
      query_x11();
#endif

      // probe_udev_subsystem() is in udev_util.c, which is only linked in if USE_USB
      probe_i2c_devices_using_udev();

      // temp
      // get_i2c_smbus_devices_using_udev();

      probe_config_files(accumulator);
      probe_logs(accumulator);

#ifdef USE_LIBDRM
      probe_using_libdrm();
#else
      rpt_vstring(0, "Not built with libdrm support.  Skipping DRM related checks");
#endif

      query_drm_using_sysfs();

      device_xref_report(0);
   }

   rpt_nl();
   env_accumulator_report(accumulator, 0);

   rpt_nl();
   final_analysis(accumulator, 0);

   env_accumulator_free(accumulator);     // make Coverity happy
}
