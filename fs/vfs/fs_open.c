/****************************************************************************
 * fs/vfs/fs_open.c
 *
 *   Copyright (C) 2007-2009, 2011-2012, 2016-2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <assert.h>
#ifdef CONFIG_FILE_MODE
#include <stdarg.h>
#endif

#include <nuttx/cancelpt.h>
#include <nuttx/fs/fs.h>

#include "inode/inode.h"
#include "driver/driver.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: inode_checkflags
 *
 * Description:
 *   Check if the access described by 'oflags' is supported on 'inode'
 *
 ****************************************************************************/

int inode_checkflags(FAR struct inode *inode, int oflags)
{
  if (((oflags & O_RDOK) != 0 && !inode->u.i_ops->read) ||
      ((oflags & O_WROK) != 0 && !inode->u.i_ops->write))
    {
      return -EACCES;
    }
  else
    {
      return OK;
    }
}

/****************************************************************************
 * Name: open
 *
 * Description:
 *   Standard 'open' interface
 *
 ****************************************************************************/

int open(const char *path, int oflags, ...)
{
  struct inode_search_s desc;
  FAR struct file *filep;
  FAR struct inode *inode;
#if defined(CONFIG_FILE_MODE) || !defined(CONFIG_DISABLE_MOUNTPOINT)
  mode_t mode = 0666;
#endif
  int ret;
  int fd;

#ifdef CONFIG_FILE_MODE
#  ifdef CONFIG_CPP_HAVE_WARNING
#    warning "File creation not implemented"
#  endif

  /* open() is a cancellation point */

  (void)enter_cancellation_point();

  /* If the file is opened for creation, then get the mode bits */

  if ((oflags & (O_WRONLY | O_CREAT)) != 0)
    {
      va_list ap;
      va_start(ap, oflags);
      mode = va_arg(ap, mode_t);
      va_end(ap);
    }
#endif

  /* Get an inode for this file */

  SETUP_SEARCH(&desc, path, false);

  ret = inode_find(&desc);
  if (ret < 0)
    {
      /* "O_CREAT is not set and the named file does not exist.  Or, a
       * directory component in pathname does not exist or is a dangling
       * symbolic link."
       */

      ret = -ret;
      goto errout_with_search;
    }

  /* Get the search results */

  inode = desc.node;
  DEBUGASSERT(inode != NULL);

#if !defined(CONFIG_DISABLE_PSEUDOFS_OPERATIONS) && \
    !defined(CONFIG_DISABLE_MOUNTPOINT)
   /* If the inode is block driver, then we may return a character driver
    * proxy for the block driver.  block_proxy() will instantiate a BCH
    * character driver wrapper around the block driver, open(), then
    * unlink() the character driver.  On success, block_proxy() will
    * return the file descriptor of the opened character driver.
    *
    * NOTE: This will recurse to open the character driver proxy.
    */

   if (INODE_IS_BLOCK(inode))
     {
       /* Release the inode reference */

       inode_release(inode);

       /* Get the file descriptor of the opened character driver proxy */

       fd = block_proxy(path, oflags);
       if (fd < 0)
         {
           ret = fd;
           goto errout_with_search;
         }

       /* Return the file descriptor */

       RELEASE_SEARCH(&desc);
       leave_cancellation_point();
       return fd;
     }
   else
#endif

  /* Verify that the inode is either a "normal" character driver or a
   * mountpoint.  We specifically "special" inodes (semaphores, message
   * queues, shared memory).
   */

#ifndef CONFIG_DISABLE_MOUNTPOINT
  if ((!INODE_IS_DRIVER(inode) && !INODE_IS_MOUNTPT(inode)) || !inode->u.i_ops)
#else
  if (!INODE_IS_DRIVER(inode) || !inode->u.i_ops)
#endif
    {
      ret = ENXIO;
      goto errout_with_inode;
    }

  /* Make sure that the inode supports the requested access */

  ret = inode_checkflags(inode, oflags);
  if (ret < 0)
    {
      ret = -ret;
      goto errout_with_inode;
    }

  /* Associate the inode with a file structure */

  fd = files_allocate(inode, oflags, 0, 0);
  if (fd < 0)
    {
      ret = EMFILE;
      goto errout_with_inode;
    }

  /* Get the file structure corresponding to the file descriptor. */

  filep = fs_getfilep(fd);
  if (!filep)
    {
      /* The errno value has already been set */

      goto errout;
    }

  /* Perform the driver open operation.  NOTE that the open method may be
   * called many times.  The driver/mountpoint logic should handled this
   * because it may also be closed that many times.
   */

  ret = OK;
  if (inode->u.i_ops->open)
    {
#ifndef CONFIG_DISABLE_MOUNTPOINT
      if (INODE_IS_MOUNTPT(inode))
        {
          ret = inode->u.i_mops->open(filep, desc.relpath, oflags, mode);
        }
      else
#endif
        {
          ret = inode->u.i_ops->open(filep);
        }
    }

  if (ret < 0)
    {
      ret = -ret;
      goto errout_with_fd;
    }

#ifdef CONFIG_PSEUDOTERM_SUSV1
  /* If the return value from the open method is > 0, then it may actually
   * be an encoded file descriptor.  This kind of logic is currently only
   * needed for /dev/ptmx:  When dev ptmx is opened, it does not return a
   * file descriptor associated with the /dev/ptmx inode, but rather with
   * the inode of master device created by the /dev/ptmx open method.
   *
   * The encoding supports (a) returning file descriptor 0 (which really
   * should not happen), and (b) avoiding confusion if some other open
   * method returns a positive, non-zero value which is not a file
   * descriptor.
   */

  if (OPEN_ISFD(ret))
    {
      /* Release file descriptor and inode that we allocated.  We don't
       * need those.
       */

      files_release(fd);
      inode_release(inode);

      /* Instead, decode and return the descriptor associated with the
       * master side device.
       */

      fd = (int)OPEN_GETFD(ret);
      DEBUGASSERT((unsigned)fd < (CONFIG_NFILE_DESCRIPTORS + CONFIG_NSOCKET_DESCRIPTORS));
    }
#endif

  RELEASE_SEARCH(&desc);
  leave_cancellation_point();
  return fd;

errout_with_fd:
  files_release(fd);

errout_with_inode:
  inode_release(inode);

errout_with_search:
  RELEASE_SEARCH(&desc);
  set_errno(ret);

errout:
  leave_cancellation_point();
  return ERROR;
}
