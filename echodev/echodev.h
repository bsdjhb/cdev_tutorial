/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __ECHODEV_H__
#define	__ECHODEV_H__

#include <sys/ioccom.h>

#define	ECHODEV_GBUFSIZE	_IOR('E', 100, size_t)	/* get buffer size */
#define	ECHODEV_SBUFSIZE	_IOW('E', 101, size_t)	/* set buffer size */
#define	ECHODEV_CLEAR		_IO('E', 102)		/* clear buffer */

#endif /* !__ECHODEV_H__ */
