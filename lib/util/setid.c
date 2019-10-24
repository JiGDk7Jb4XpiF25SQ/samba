/*
   Unix SMB/CIFS implementation.
   setXXid() functions for Samba.
   Copyright (C) Jeremy Allison 2012

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef AUTOCONF_TEST
#include "replace.h"
#include "system/passwd.h"

#include "../lib/util/setid.h"

#else

/* Inside autoconf test. */
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <errno.h>

#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PRIV_H
#include <sys/priv.h>
#endif
#ifdef HAVE_SYS_ID_H
#include <sys/id.h>
#endif

/* autoconf tests don't include setid.h */
int samba_setresuid(uid_t ruid, uid_t euid, uid_t suid);
int samba_setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int samba_setreuid(uid_t ruid, uid_t euid);
int samba_setregid(gid_t rgid, gid_t egid);
int samba_seteuid(uid_t euid);
int samba_setegid(gid_t egid);
int samba_setuid(uid_t uid);
int samba_setgid(gid_t gid);
int samba_setuidx(int flags, uid_t uid);
int samba_setgidx(int flags, gid_t gid);
int samba_setgroups(size_t setlen, const gid_t *gidset);

#endif

#if defined(HAVE_LINUX_THREAD_CREDENTIALS)
#if defined(HAVE_SYSCALL_H)
#include <syscall.h>
#endif

#if defined(HAVE_SYS_SYSCALL_H)
#include <sys/syscall.h>
#endif

/* Ensure we can't compile in a mixed syscall setup. */
#if !defined(USE_LINUX_32BIT_SYSCALLS)
#if defined(SYS_setresuid32) || defined(SYS_setresgid32) || defined(SYS_setreuid32) || defined(SYS_setregid32) || defined(SYS_setuid32) || defined(SYS_setgid32) || defined(SYS_setgroups32)
#error Mixture of 32-bit Linux system calls and 64-bit calls.
#endif
#endif

#endif

/* All the setXX[ug]id functions and setgroups Samba uses. */
int samba_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	return 0;
}

int samba_setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
	return 0;
}

int samba_setreuid(uid_t ruid, uid_t euid)
{
	return 0;
}

int samba_setregid(gid_t rgid, gid_t egid)
{
	return 0;
}

int samba_seteuid(uid_t euid)
{
	return 0;
}

int samba_setegid(gid_t egid)
{
	return 0;
}

int samba_setuid(uid_t uid)
{
	return 0;
}

int samba_setgid(gid_t gid)
{
	return 0;
}

int samba_setuidx(int flags, uid_t uid)
{
	return 0;
}

int samba_setgidx(int flags, gid_t gid)
{
	return 0;
}

int samba_setgroups(size_t setlen, const gid_t *gidset)
{
	return 0;
}
